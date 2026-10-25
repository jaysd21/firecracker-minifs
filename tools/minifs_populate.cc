/* minifs-populate: uploads a raw disk image into consecutive MiniFS objects.
 * This is a provisioning tool, separate from the read-only backend. The last
 * object is zero-padded to --object-size. */
#include "remote_disk/cli.h"
#include "remote_disk/disk_mapping.h"

#include "contracts/ddl_minifs_protocol.h"
#include "ddl/client_target/core_client.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

using namespace remote_disk;
using ddl::runtime::CoreClient;
using ddl::runtime::CoreServiceConfig;
using ddl::runtime::CoreStatus;
using ddl::runtime::DdlCqe;

int main(int argc, char** argv)
{
    DiskConfig c;
    std::string image;
    bool verify = true;
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        if (opt == "--image" && i + 1 < argc) { image = argv[++i]; continue; }
        if (opt == "--no-verify") { verify = false; continue; }
        const int r = parse_common_option(opt, i, argc, argv, c);
        if (r <= 0) {
            std::fprintf(stderr, "usage: %s --image FILE --server IP [--port N --tenant N "
                "--object-hi N --object-start N --object-size N --ddl-cpu N --numa N] [--no-verify]\n"
                "  --objects is derived from the image size\n", argv[0]);
            return 2;
        }
    }
    std::ifstream in(image, std::ios::binary | std::ios::ate);
    if (!in || c.minifs.server.empty()) { std::fprintf(stderr, "need --image and --server\n"); return 2; }
    const uint64_t bytes = static_cast<uint64_t>(in.tellg());
    in.seekg(0);
    c.layout.object_count = (bytes + c.layout.object_size - 1) / c.layout.object_size;
    DiskMapping mapping;
    std::string error;
    if (bytes == 0 || !DiskMapping::create(c.layout, mapping, &error)) {
        std::fprintf(stderr, "invalid layout: %s\n", error.c_str());
        return 2;
    }

    const uint32_t osz = static_cast<uint32_t>(c.layout.object_size);
    CoreServiceConfig cfg;
    cfg.core_id = 0; cfg.cpu_id = c.minifs.ddl_cpu; cfg.numa_node = c.minifs.numa_node;
    cfg.peer_count = 1;
    std::strncpy(cfg.peers[0].address, c.minifs.server.c_str(), sizeof(cfg.peers[0].address) - 1);
    cfg.peers[0].port = c.minifs.port; cfg.peers[0].connection_id = 1;
    cfg.slab_bytes = std::max<uint32_t>(4096, osz);
    cfg.primary_slabs = 16; cfg.standby_slabs = 8;
    cfg.medium_ring_bytes = 8u << 20; cfg.medium_ring_lanes = 1;
    cfg.application_buffer_bytes = cfg.slab_bytes; cfg.application_buffer_count = 1;
    cfg.prefault = true; cfg.small_credits = 64;
    cfg.runtime.policy.small_max_bytes = 8192;
    cfg.runtime.policy.medium_max_bytes = 256u << 10;
    cfg.runtime.dispatch_budget = cfg.runtime.transport_budget = cfg.runtime.submit_budget = 64;
    cfg.runtime.expiry_scan_slice = 64;
    cfg.runtime.request_timeout_ns = uint64_t(c.minifs.timeout_seconds) * 1000000000ull;
    cfg.runtime.mm_max_inflight = 8;
    for (uint32_t k = 0; k < ddl::runtime::kCoreQosClasses; ++k) { cfg.qos[k].weight = 1; cfg.qos[k].max_inflight = 16; }

    CoreClient client;
    if (!client.start(cfg)) { std::fprintf(stderr, "client start failed\n"); return 1; }
    auto deadline = [&] { return std::chrono::steady_clock::now() + std::chrono::seconds(c.minifs.timeout_seconds); };
    auto dl = deadline();
    while (!client.ready() && std::chrono::steady_clock::now() < dl) { client.poll(); std::this_thread::yield(); }
    if (!client.ready()) { std::fprintf(stderr, "connect timed out\n"); client.stop(); return 1; }

    auto run = [&](uint32_t opcode, uint64_t lo, void* in_buf, uint64_t in_len, void* out, uint64_t out_cap,
                   uint64_t expect) -> bool {
        nebula::objstore::DdlMiniFsReadRequest read{0, osz, 0};
        ddl::runtime::DdlSqe* sqe = client.get_sqe();
        if (!sqe) return false;
        sqe->user_data = lo; sqe->object_id_hi = c.layout.object_id_hi; sqe->object_id_lo = lo;
        sqe->tenant = c.minifs.tenant; sqe->opcode = opcode;
        if (out) { sqe->buffer = &read; sqe->length = sizeof(read); sqe->output = out;
                   sqe->output_capacity = out_cap; sqe->flags = ddl::runtime::kDdlSqeRead; }
        else { sqe->buffer = in_buf; sqe->length = in_len; }
        if (client.submit() != 1) return false;
        DdlCqe cqe{};
        auto end = deadline();
        while (std::chrono::steady_clock::now() < end) {
            client.poll();
            if (client.poll_completion(cqe))
                return cqe.status == int32_t(CoreStatus::kOk) && cqe.result_bytes == expect;
            std::this_thread::yield();
        }
        return false;
    };

    std::vector<uint8_t> buf(osz), back(osz);
    for (uint64_t i = 0; i < c.layout.object_count; ++i) {
        std::fill(buf.begin(), buf.end(), 0);
        in.read(reinterpret_cast<char*>(buf.data()), osz);
        const uint64_t lo = c.layout.object_id_start + i;
        if (!run(nebula::objstore::kDdlMiniFsPutOpcode, lo, buf.data(), osz, nullptr, 0, osz)) {
            std::fprintf(stderr, "PUT failed for object %llu\n", (unsigned long long)lo);
            client.stop(); return 1;
        }
        if (verify) {
            std::fill(back.begin(), back.end(), 0);
            if (!run(nebula::objstore::kDdlMiniFsGetOpcode, lo, nullptr, 0, back.data(), osz, osz) || back != buf) {
                std::fprintf(stderr, "verify failed for object %llu\n", (unsigned long long)lo);
                client.stop(); return 1;
            }
        }
    }
    client.stop();
    std::printf("[POPULATE_COMPLETE] image_bytes=%llu objects=%llu object_size=%u object_start=%llu\n",
                (unsigned long long)bytes, (unsigned long long)c.layout.object_count, osz,
                (unsigned long long)c.layout.object_id_start);
    return 0;
}
