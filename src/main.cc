/* remote-disk-backend: serves a MiniFS object range as a read-only vhost-user
 * block device for Firecracker. */
#include "remote_disk/cli.h"
#include "remote_disk/minifs_adapter.h"
#include "remote_disk/vhost_user_blk.h"

#include <atomic>
#include <csignal>
#include <cstdio>

#include <sched.h>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage(const char* program)
{
    std::fprintf(stderr,
        "usage: %s --server IP --objects N --socket PATH [options]\n"
        "  --socket PATH         vhost-user Unix socket Firecracker connects to\n"
        "  --server IP           MiniFS DDL3 target address\n"
        "  --port N              target port (default 58000)\n"
        "  --tenant N            tenant ID (default 1)\n"
        "  --object-hi N         high object-ID word (default 0xdd1c000000000000)\n"
        "  --object-start N      first low object-ID word (default 1)\n"
        "  --object-size N       bytes per object, multiple of 512 (default 1048576)\n"
        "  --objects N           number of objects\n"
        "  --disk-bytes N        exported size if smaller than objects*size\n"
        "  --ddl-cpu N           client owner CPU, pinned (default 0)\n"
        "  --numa N              client NUMA node (default 0)\n"
        "  --max-inflight N      concurrent GETs (default 8)\n"
        "  --timeout-seconds N   connect/request timeout (default 30)\n",
        program);
}

} // namespace

int main(int argc, char** argv)
{
    using namespace remote_disk;
    DiskConfig config;
    std::string socket_path;
    config.minifs.max_inflight = config.max_inflight_chunks;
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        if (opt == "--help") { usage(argv[0]); return 0; }
        if (opt == "--socket") {
            if (i + 1 >= argc) { usage(argv[0]); return 2; }
            socket_path = argv[++i];
            continue;
        }
        const int r = parse_common_option(opt, i, argc, argv, config);
        if (r <= 0) {
            std::fprintf(stderr, r < 0 ? "bad value for %s\n" : "unknown option %s\n", opt.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (config.minifs.server.empty() || socket_path.empty()) { usage(argv[0]); return 2; }
    config.minifs.max_io_bytes = static_cast<uint32_t>(
        std::min<uint64_t>(config.layout.object_size, UINT32_MAX));

    DiskMapping mapping;
    std::string error;
    if (!DiskMapping::create(config.layout, mapping, &error)) {
        std::fprintf(stderr, "invalid disk layout: %s\n", error.c_str());
        return 2;
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(config.minifs.ddl_cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        std::fprintf(stderr, "warning: cannot pin to CPU %u\n", config.minifs.ddl_cpu);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    auto client = MiniFsAdapter::connect(config.minifs, &error);
    if (!client) { std::fprintf(stderr, "error: %s\n", error.c_str()); return 1; }

    int rc = 0;
    {
        RemoteDisk disk(*client, mapping, config.max_requests,
                        config.max_inflight_chunks, config.max_retries);
        VhostUserBlk server(disk, socket_path);
        if (!server.listen(&error)) { std::fprintf(stderr, "error: %s\n", error.c_str()); return 1; }
        std::printf("[REMOTE_DISK_READY] socket=%s capacity=%llu objects=%llu object_size=%llu\n",
                    socket_path.c_str(), (unsigned long long)mapping.capacity(),
                    (unsigned long long)config.layout.object_count,
                    (unsigned long long)config.layout.object_size);
        std::fflush(stdout);
        server.run(g_stop);
        const bool drained = disk.shutdown(5000);
        const auto& st = disk.stats();
        std::printf("[REMOTE_DISK_STATS] requests=%llu failed=%llu chunk_gets=%llu bytes_read=%llu retries=%llu\n",
                    (unsigned long long)st.requests, (unsigned long long)st.failed_requests,
                    (unsigned long long)st.chunks, (unsigned long long)st.bytes,
                    (unsigned long long)st.retries);
        if (!drained || server.leaked_buffers()) {
            std::fprintf(stderr, "error: storage did not drain; leaking buffers and exiting\n");
            rc = 1;
            std::_Exit(rc); /* transport may still write into leaked buffers */
        }
    }
    client->stop();
    return rc;
}
