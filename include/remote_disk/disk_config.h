#pragma once
#include <cstdint>
#include <string>

namespace remote_disk {

/* How the virtual disk is laid out over MiniFS objects. Object (start + i)
 * holds disk bytes [i * object_size, (i + 1) * object_size). */
struct DiskLayout {
    uint64_t object_id_hi = 0xdd1c000000000000ull;
    uint64_t object_id_start = 1;
    uint64_t object_size = 1024ull * 1024ull;
    uint64_t object_count = 0;
    /* 0 = object_size * object_count. Otherwise the exported capacity, which
     * may end inside the last object. */
    uint64_t disk_bytes = 0;
};

/* Connection to one NebulaR MiniFS DDL3 target. */
struct MiniFsConfig {
    std::string server;
    uint16_t port = 58000;
    uint16_t tenant = 1;
    uint16_t ddl_cpu = 0;
    int16_t numa_node = 0;
    uint32_t timeout_seconds = 30;
    /* Largest single GET; must be >= layout.object_size and <= the server's
     * --max-object-mb. */
    uint32_t max_io_bytes = 1024u * 1024u;
    uint32_t max_inflight = 16;
};

struct DiskConfig {
    DiskLayout layout;
    MiniFsConfig minifs;
    uint32_t max_requests = 64;        /* guest requests admitted at once */
    uint32_t max_inflight_chunks = 8;  /* concurrent GETs to the server */
    uint32_t max_retries = 64;         /* transient (queue full) retries */
};

} // namespace remote_disk
