#pragma once
#include "remote_disk/remote_disk.h"

#include <atomic>
#include <string>

namespace remote_disk {

/* Minimal vhost-user block backend (single queue, read-only) serving one
 * frontend (Firecracker) at a time over a Unix socket. Runs entirely on the
 * calling thread: socket I/O, virtqueue processing and RemoteDisk pumping. */
class VhostUserBlk {
public:
    VhostUserBlk(RemoteDisk& disk, std::string socket_path);
    ~VhostUserBlk();

    /* Binds and listens. */
    bool listen(std::string* error);

    /* Event loop; returns when stop_flag becomes true. After a frontend
     * disconnects it drains in-flight I/O and waits for the next frontend. */
    void run(const std::atomic<bool>& stop_flag);

    /* True if shutdown left transport-owned buffers behind (see RemoteDisk). */
    bool leaked_buffers() const { return leaked_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool leaked_ = false;
};

} // namespace remote_disk
