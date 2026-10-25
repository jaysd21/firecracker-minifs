#pragma once
#include <cstddef>
#include <cstdint>

namespace remote_disk {

enum class IoStatus : int {
    kOk = 0,
    kNotFound,
    kQueueFull, /* transient: try again later */
    kInvalid,
    kIoError,
    kTimeout,
    kDisconnected,
    kShortRead,
    kShutdown,
    kInvalidRange,
};

const char* to_string(IoStatus status);
inline bool is_transient(IoStatus status) { return status == IoStatus::kQueueFull; }

struct GetRequest {
    uint64_t user_data;
    uint64_t object_id_hi;
    uint64_t object_id_lo;
    uint64_t offset;
    uint32_t length;
    void* dst; /* must stay valid until the completion is returned */
};

struct Completion {
    uint64_t user_data;
    IoStatus status;
    uint64_t bytes;
};

/* Asynchronous, read-only object access. All methods must be called from one
 * owner thread (the DDL ring is single-producer/single-consumer). */
class StorageClient {
public:
    virtual ~StorageClient() = default;
    virtual bool ready() = 0;
    /* kOk: accepted, exactly one Completion will follow. kQueueFull: nothing
     * was queued, retry after poll(). Any other status: rejected, no
     * Completion will follow. */
    virtual IoStatus submit_get(const GetRequest& request) = 0;
    /* Drives the transport and returns up to max completions. */
    virtual size_t poll(Completion* out, size_t max) = 0;
    virtual void stop() = 0;
};

} // namespace remote_disk
