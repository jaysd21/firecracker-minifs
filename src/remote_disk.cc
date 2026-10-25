#include "remote_disk/remote_disk.h"

#include <chrono>

namespace remote_disk {

const char* to_string(IoStatus status)
{
    switch (status) {
    case IoStatus::kOk: return "ok";
    case IoStatus::kNotFound: return "object not found";
    case IoStatus::kQueueFull: return "queue full";
    case IoStatus::kInvalid: return "invalid request";
    case IoStatus::kIoError: return "i/o error";
    case IoStatus::kTimeout: return "timeout";
    case IoStatus::kDisconnected: return "disconnected";
    case IoStatus::kShortRead: return "short read";
    case IoStatus::kShutdown: return "shutdown";
    case IoStatus::kInvalidRange: return "out of range";
    }
    return "unknown";
}

RemoteDisk::RemoteDisk(StorageClient& client, const DiskMapping& mapping,
                       uint32_t max_requests, uint32_t max_inflight_chunks,
                       uint32_t max_retries)
    : client_(client), mapping_(mapping), max_requests_(max_requests),
      max_inflight_chunks_(max_inflight_chunks), max_retries_(max_retries)
{
}

RemoteDisk::~RemoteDisk()
{
    if (!requests_.empty()) shutdown(1000);
}

SubmitResult RemoteDisk::read(uint64_t offset, uint64_t length, void* dst,
                              Callback cb)
{
    if (shutting_down_) return SubmitResult::kShutdown;
    auto request = std::make_unique<Request>();
    if (dst == nullptr || !cb ||
        mapping_.map(offset, length, request->extents) != MapStatus::kOk)
        return SubmitResult::kInvalidRange;
    if (requests_.size() >= max_requests_) return SubmitResult::kBusy;

    request->dst = static_cast<uint8_t*>(dst);
    request->cb = std::move(cb);
    request->attempts.assign(request->extents.size(), 0);
    for (uint32_t i = 0; i < request->extents.size(); ++i)
        request->todo.push_back(i);
    request->queued = true;
    ++stats_.requests;
    const uint64_t id = next_request_id_++;
    queue_.push_back(id);
    requests_.emplace(id, std::move(request));
    return SubmitResult::kAccepted;
}

void RemoteDisk::fail_request(Request& request, IoStatus status)
{
    if (request.error == IoStatus::kOk) request.error = status;
    request.todo.clear();
}

void RemoteDisk::requeue(uint64_t request_id, Request& request)
{
    if (request.queued) return;
    request.queued = true;
    queue_.push_front(request_id);
}

void RemoteDisk::finish_if_idle(uint64_t request_id, Request& request)
{
    if (request.outstanding == 0 && request.todo.empty())
        to_settle_.push_back(request_id);
}

void RemoteDisk::submit_queued()
{
    while (!queue_.empty() && chunks_.size() < max_inflight_chunks_) {
        const uint64_t id = queue_.front();
        auto found = requests_.find(id);
        if (found == requests_.end()) { queue_.pop_front(); continue; }
        Request& request = *found->second;
        if (request.todo.empty()) {
            request.queued = false;
            queue_.pop_front();
            finish_if_idle(id, request);
            continue;
        }
        const uint32_t index = request.todo.front();
        const Extent& extent = request.extents[index];
        const uint64_t chunk_id = next_chunk_id_;
        const IoStatus status = client_.submit_get(GetRequest{
            chunk_id, extent.object_id_hi, extent.object_id_lo,
            extent.object_offset, extent.length,
            request.dst + extent.buffer_offset});
        if (status == IoStatus::kOk) {
            ++next_chunk_id_;
            chunks_[chunk_id] = Chunk{id, index};
            ++request.outstanding;
            request.todo.pop_front();
        } else if (status == IoStatus::kQueueFull) {
            break;
        } else {
            fail_request(request, status);
        }
    }
}

void RemoteDisk::reap()
{
    Completion batch[32];
    for (;;) {
        const size_t count = client_.poll(batch, 32);
        for (size_t i = 0; i < count; ++i) {
            const Completion& done = batch[i];
            auto chunk = chunks_.find(done.user_data);
            if (chunk == chunks_.end()) continue;
            const Chunk info = chunk->second;
            chunks_.erase(chunk);
            auto found = requests_.find(info.request_id);
            if (found == requests_.end()) continue; /* abandoned at shutdown */
            Request& request = *found->second;
            --request.outstanding;
            if (done.status == IoStatus::kOk) { ++stats_.chunks; stats_.bytes += done.bytes; }

            IoStatus status = done.status;
            if (status == IoStatus::kOk &&
                done.bytes != request.extents[info.extent].length)
                status = IoStatus::kShortRead;
            if (status != IoStatus::kOk) {
                if (is_transient(status) && !shutting_down_ &&
                    request.error == IoStatus::kOk &&
                    request.attempts[info.extent]++ < max_retries_) {
                    ++stats_.retries;
                    request.todo.push_front(info.extent);
                    requeue(info.request_id, request);
                    continue;
                }
                fail_request(request, status);
            }
            finish_if_idle(info.request_id, request);
        }
        if (count < 32) break;
    }
}

void RemoteDisk::settle()
{
    if (settling_) return;
    settling_ = true;
    /* Callbacks may call read(), which never touches to_settle_. */
    std::vector<std::pair<Callback, IoStatus>> done;
    for (const uint64_t id : to_settle_) {
        auto found = requests_.find(id);
        if (found == requests_.end()) continue;
        Request& request = *found->second;
        if (request.outstanding != 0 || !request.todo.empty()) continue;
        if (request.error != IoStatus::kOk) ++stats_.failed_requests;
        done.emplace_back(std::move(request.cb), request.error);
        requests_.erase(found);
    }
    to_settle_.clear();
    for (auto& entry : done) entry.first(entry.second);
    settling_ = false;
}

size_t RemoteDisk::pump()
{
    reap();
    submit_queued();
    settle();
    return requests_.size();
}

bool RemoteDisk::shutdown(uint32_t timeout_ms)
{
    shutting_down_ = true;
    for (auto& entry : requests_) {
        fail_request(*entry.second, IoStatus::kShutdown);
        finish_if_idle(entry.first, *entry.second);
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (!requests_.empty() && std::chrono::steady_clock::now() < deadline)
        pump();
    if (requests_.empty()) return true;

    /* Transport never returned some chunks: complete the guests' requests
     * anyway (exactly once) but report that buffers are still referenced. */
    for (auto& entry : requests_) {
        entry.second->outstanding = 0;
        to_settle_.push_back(entry.first);
    }
    settle();
    return false;
}

} // namespace remote_disk
