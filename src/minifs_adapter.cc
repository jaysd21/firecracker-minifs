#include "remote_disk/minifs_adapter.h"

#include "contracts/ddl_minifs_protocol.h"
#include "ddl/client_target/core_client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace remote_disk {

using ddl::runtime::CoreClient;
using ddl::runtime::CoreServiceConfig;
using ddl::runtime::CoreStatus;
using ddl::runtime::DdlCqe;
using ddl::runtime::DdlSqe;
using nebula::objstore::DdlMiniFsReadRequest;
using nebula::objstore::kDdlMiniFsGetOpcode;

namespace {

IoStatus map_status(int32_t status)
{
    switch (static_cast<CoreStatus>(status)) {
    case CoreStatus::kOk: return IoStatus::kOk;
    case CoreStatus::kNotFound: return IoStatus::kNotFound;
    case CoreStatus::kNoCredits:
    case CoreStatus::kQueueFull: return IoStatus::kQueueFull;
    case CoreStatus::kInvalid: return IoStatus::kInvalid;
    case CoreStatus::kTimeout: return IoStatus::kTimeout;
    case CoreStatus::kDisconnected: return IoStatus::kDisconnected;
    case CoreStatus::kNoMemory:
    case CoreStatus::kIoError:
    case CoreStatus::kUnauthorized: break;
    }
    return IoStatus::kIoError;
}

CoreServiceConfig make_config(const MiniFsConfig& c)
{
    CoreServiceConfig config;
    config.core_id = 0;
    config.cpu_id = c.ddl_cpu;
    config.numa_node = c.numa_node;
    config.peer_count = 1;
    std::strncpy(config.peers[0].address, c.server.c_str(),
                 sizeof(config.peers[0].address) - 1);
    config.peers[0].port = c.port;
    config.peers[0].connection_id = 1;
    config.slab_bytes = std::max<uint32_t>(4096, c.max_io_bytes);
    config.primary_slabs = 16;
    config.standby_slabs = 8;
    config.medium_ring_bytes = 8u * 1024u * 1024u;
    config.medium_ring_lanes = 1;
    config.application_buffer_bytes = config.slab_bytes;
    config.application_buffer_count = 1;
    config.prefault = true;
    config.small_credits = 64;
    config.runtime.policy.small_max_bytes = 8192;
    config.runtime.policy.medium_max_bytes = 256u * 1024u;
    config.runtime.dispatch_budget = 64;
    config.runtime.transport_budget = 64;
    config.runtime.submit_budget = 64;
    config.runtime.expiry_scan_slice = 64;
    config.runtime.request_timeout_ns =
        static_cast<uint64_t>(c.timeout_seconds) * 1000000000ull;
    config.runtime.mm_max_inflight = 8;
    for (uint32_t klass = 0; klass < ddl::runtime::kCoreQosClasses; ++klass) {
        config.qos[klass].weight = 1;
        config.qos[klass].max_inflight = std::max<uint32_t>(1, c.max_inflight);
    }
    return config;
}

} // namespace

/* Each in-flight GET owns a slot: the 16-byte read payload must outlive the
 * submit call, and the caller's user_data is restored from here. */
struct MiniFsAdapter::Impl {
    struct Slot {
        DdlMiniFsReadRequest read{};
        uint64_t user_data = 0;
        bool busy = false;
    };

    MiniFsConfig config;
    CoreClient client;
    bool started = false;
    bool unsent = false; /* SQEs queued but not yet pushed to the core */
    std::vector<Slot> slots;
    std::vector<uint32_t> free_slots;

    void flush()
    {
        if (unsent) { client.submit(); unsent = false; }
    }
};

MiniFsAdapter::MiniFsAdapter() : impl_(std::make_unique<Impl>()) {}

MiniFsAdapter::~MiniFsAdapter() { stop(); }

std::unique_ptr<MiniFsAdapter> MiniFsAdapter::connect(const MiniFsConfig& config,
                                                      std::string* error)
{
    auto fail = [&](const char* message) -> std::unique_ptr<MiniFsAdapter> {
        if (error != nullptr) *error = message;
        return nullptr;
    };
    if (config.server.empty() || config.server.size() >= sizeof(CoreServiceConfig{}.peers[0].address))
        return fail("invalid MiniFS server address");

    std::unique_ptr<MiniFsAdapter> adapter(new MiniFsAdapter());
    Impl& impl = *adapter->impl_;
    impl.config = config;
    const uint32_t depth = std::max<uint32_t>(1, config.max_inflight);
    impl.slots.resize(depth);
    for (uint32_t i = 0; i < depth; ++i) impl.free_slots.push_back(depth - 1 - i);

    if (!impl.client.start(make_config(config)))
        return fail("DDL client startup failed");
    impl.started = true;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config.timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        impl.client.poll();
        if (impl.client.ready()) return adapter;
        std::this_thread::yield();
    }
    return fail("timed out connecting to MiniFS target");
}

bool MiniFsAdapter::ready() { return impl_->started && impl_->client.ready(); }

IoStatus MiniFsAdapter::submit_get(const GetRequest& request)
{
    Impl& impl = *impl_;
    if (!impl.started) return IoStatus::kDisconnected;
    if (request.length == 0 || request.length > impl.config.max_io_bytes ||
        request.dst == nullptr)
        return IoStatus::kInvalid;
    if (impl.free_slots.empty()) return IoStatus::kQueueFull;
    DdlSqe* sqe = impl.client.get_sqe();
    if (sqe == nullptr) return IoStatus::kQueueFull;

    const uint32_t index = impl.free_slots.back();
    impl.free_slots.pop_back();
    Impl::Slot& slot = impl.slots[index];
    slot.read = DdlMiniFsReadRequest{request.offset, request.length, 0};
    slot.user_data = request.user_data;
    slot.busy = true;

    sqe->user_data = index;
    sqe->object_id_hi = request.object_id_hi;
    sqe->object_id_lo = request.object_id_lo;
    sqe->tenant = impl.config.tenant;
    sqe->opcode = kDdlMiniFsGetOpcode;
    sqe->buffer = &slot.read;
    sqe->length = sizeof(slot.read);
    sqe->output = request.dst;
    sqe->output_capacity = request.length;
    sqe->flags = ddl::runtime::kDdlSqeRead;
    impl.unsent = true;
    impl.flush();
    return IoStatus::kOk;
}

size_t MiniFsAdapter::poll(Completion* out, size_t max)
{
    Impl& impl = *impl_;
    if (!impl.started) return 0;
    impl.client.poll();
    impl.flush();
    size_t count = 0;
    DdlCqe cqe{};
    while (count < max && impl.client.poll_completion(cqe)) {
        Impl::Slot& slot = impl.slots[cqe.user_data];
        out[count++] = Completion{slot.user_data, map_status(cqe.status),
                                  cqe.result_bytes};
        slot.busy = false;
        impl.free_slots.push_back(static_cast<uint32_t>(cqe.user_data));
    }
    return count;
}

void MiniFsAdapter::stop()
{
    if (impl_ && impl_->started) {
        impl_->client.stop();
        impl_->started = false;
    }
}

} // namespace remote_disk
