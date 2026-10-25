#pragma once
#include "remote_disk/disk_config.h"
#include "remote_disk/disk_mapping.h"
#include "remote_disk/storage_client.h"

#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <unordered_map>

namespace remote_disk {

enum class SubmitResult { kAccepted, kBusy, kInvalidRange, kShutdown };

/* Read-only virtual disk over a StorageClient. Single-threaded: every method
 * and every callback runs on the thread that owns the client. */
class RemoteDisk {
public:
    using Callback = std::function<void(IoStatus)>;

    RemoteDisk(StorageClient& client, const DiskMapping& mapping,
               uint32_t max_requests, uint32_t max_inflight_chunks,
               uint32_t max_retries);
    ~RemoteDisk();

    uint64_t capacity() const { return mapping_.capacity(); }

    /* Reads [offset, offset+length) into dst. kAccepted: callback fires
     * exactly once, never from inside this call, and dst must stay valid until
     * then. Any other result: no callback. */
    SubmitResult read(uint64_t offset, uint64_t length, void* dst, Callback cb);

    /* Submits queued chunks, reaps completions, fires callbacks. Returns the
     * number of requests still in flight. */
    size_t pump();

    struct Stats {
        uint64_t requests = 0, failed_requests = 0, chunks = 0, bytes = 0, retries = 0;
    };
    const Stats& stats() const { return stats_; }

    size_t inflight_requests() const { return requests_.size(); }

    /* Stops accepting reads and fails queued chunks with kShutdown, then
     * pumps for up to timeout_ms waiting for submitted chunks. Returns false
     * if some chunks never completed; their buffers are still referenced by
     * the transport and the caller must not free them. */
    bool shutdown(uint32_t timeout_ms);

private:
    struct Request {
        uint8_t* dst = nullptr;
        std::vector<Extent> extents;
        std::deque<uint32_t> todo;
        std::vector<uint32_t> attempts;
        uint32_t outstanding = 0;
        bool queued = false;
        IoStatus error = IoStatus::kOk;
        Callback cb;
    };
    struct Chunk {
        uint64_t request_id;
        uint32_t extent;
    };

    void submit_queued();
    void reap();
    void requeue(uint64_t request_id, Request& request);
    void finish_if_idle(uint64_t request_id, Request& request);
    void settle();
    void fail_request(Request& request, IoStatus status);

    StorageClient& client_;
    DiskMapping mapping_;
    const uint32_t max_requests_;
    const uint32_t max_inflight_chunks_;
    const uint32_t max_retries_;
    bool shutting_down_ = false;
    uint64_t next_request_id_ = 1;
    uint64_t next_chunk_id_ = 1;
    std::unordered_map<uint64_t, std::unique_ptr<Request>> requests_;
    std::deque<uint64_t> queue_; /* request ids that still have todo extents */
    std::unordered_map<uint64_t, Chunk> chunks_;
    std::vector<uint64_t> to_settle_;
    bool settling_ = false;
    Stats stats_;
};

} // namespace remote_disk
