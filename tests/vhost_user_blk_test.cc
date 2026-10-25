/* Drives VhostUserBlk with a fake frontend that speaks the vhost-user protocol
 * the way Firecracker does (memfd-backed guest memory, one split virtqueue). */
#include "mock_storage.h"
#include "remote_disk/vhost_user_blk.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace remote_disk;
using remote_disk::testing::MockStorage;

namespace {

constexpr uint64_t kObj = 8192, kCount = 4;
constexpr uint32_t kQ = 64;
constexpr uint64_t kGuestBase = 0x100000;       /* guest-physical */
constexpr uint64_t kUserBase = 0x7f0000000000;  /* "Firecracker" virtual address */
constexpr size_t kMemSize = 1 << 20;
constexpr uint64_t kDesc = 0, kAvail = 16 * kQ, kUsed = 4096, kHdr = 8192, kData = 16384;

uint8_t pattern(uint64_t o) { return static_cast<uint8_t>(o * 37 + (o >> 9)); }

struct Frontend {
    int sock = -1;
    uint8_t* mem = nullptr;
    int memfd = -1, call = -1, kick = -1;
    uint16_t avail_idx = 0;

    bool connect_to(const std::string& path)
    {
        for (int i = 0; i < 200; ++i) {
            sock = socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un a{}; a.sun_family = AF_UNIX;
            std::strcpy(a.sun_path, path.c_str());
            if (connect(sock, (sockaddr*)&a, sizeof(a)) == 0) return true;
            close(sock);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
    void send(uint32_t req, const void* p, uint32_t n, std::vector<int> fds = {}, bool need_reply = false)
    {
        struct { uint32_t r, f, s; } h{req, 1u | (need_reply ? 8u : 0u), n};
        uint8_t buf[512]; std::memcpy(buf, &h, 12); if (n) std::memcpy(buf + 12, p, n);
        iovec iov{buf, 12u + n};
        msghdr m{}; m.msg_iov = &iov; m.msg_iovlen = 1;
        alignas(8) char ctrl[CMSG_SPACE(sizeof(int) * 8)] = {};
        if (!fds.empty()) {
            m.msg_control = ctrl; m.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
            cmsghdr* c = CMSG_FIRSTHDR(&m);
            c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
            std::memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
        }
        ASSERT_EQ(sendmsg(sock, &m, 0), (ssize_t)(12 + n));
    }
    std::vector<uint8_t> reply()
    {
        uint32_t h[3];
        EXPECT_EQ(recv(sock, h, 12, MSG_WAITALL), 12);
        std::vector<uint8_t> out(h[2]);
        if (h[2]) { EXPECT_EQ(recv(sock, out.data(), h[2], MSG_WAITALL), (ssize_t)h[2]); }
        return out;
    }
    uint64_t reply_u64() { auto r = reply(); uint64_t v = 0; if (r.size() >= 8) std::memcpy(&v, r.data(), 8); return v; }

    void setup(uint64_t expect_sectors)
    {
        send(3, nullptr, 0); /* SET_OWNER */
        send(1, nullptr, 0);
        uint64_t feats = reply_u64();
        EXPECT_TRUE(feats & (1ull << 32));  /* VERSION_1 */
        EXPECT_TRUE(feats & (1ull << 30));  /* PROTOCOL_FEATURES */
        EXPECT_TRUE(feats & (1ull << 5));   /* RO */
        send(15, nullptr, 0);
        uint64_t proto = reply_u64();
        EXPECT_TRUE(proto & (1ull << 9));   /* CONFIG */
        EXPECT_TRUE(proto & (1ull << 3));   /* REPLY_ACK */
        send(16, &proto, 8, {}, true); EXPECT_EQ(reply_u64(), 0u);
        uint64_t acked = (1ull << 32) | (1ull << 30) | (1ull << 5);
        send(2, &acked, 8, {}, true); EXPECT_EQ(reply_u64(), 0u);

        /* GET_CONFIG as Firecracker does: offset 0, size 60, flags 1 */
        uint8_t req[12 + 60] = {};
        uint32_t cfg[3] = {0, 60, 1}; std::memcpy(req, cfg, 12);
        send(24, req, sizeof(req));
        auto cr = reply();
        ASSERT_EQ(cr.size(), 72u);
        uint64_t cap; std::memcpy(&cap, cr.data() + 12, 8);
        EXPECT_EQ(cap, expect_sectors);

        memfd = memfd_create("guest", 0);
        ASSERT_EQ(ftruncate(memfd, kMemSize), 0);
        mem = (uint8_t*)mmap(nullptr, kMemSize, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        ASSERT_NE(mem, MAP_FAILED);
        uint8_t tbl[8 + 32] = {};
        uint32_t n = 1; std::memcpy(tbl, &n, 4);
        uint64_t reg[4] = {kGuestBase, kMemSize, kUserBase, 0};
        std::memcpy(tbl + 8, reg, 32);
        send(5, tbl, sizeof(tbl), {memfd}, true); EXPECT_EQ(reply_u64(), 0u);

        uint32_t st[2] = {0, kQ};
        send(8, st, 8, {}, true); EXPECT_EQ(reply_u64(), 0u);
        uint64_t addr[6] = {0, kUserBase + kDesc, kUserBase + kUsed, kUserBase + kAvail, 0, 0};
        uint32_t a0[2] = {0, 0}; std::memcpy(addr, a0, 8);
        send(9, addr, 48, {}, true); EXPECT_EQ(reply_u64(), 0u);
        uint32_t base[2] = {0, 0};
        send(10, base, 8, {}, true); EXPECT_EQ(reply_u64(), 0u);
        call = eventfd(0, EFD_NONBLOCK); kick = eventfd(0, EFD_NONBLOCK);
        uint64_t idx0 = 0;
        send(13, &idx0, 8, {call}, true); EXPECT_EQ(reply_u64(), 0u);
        send(12, &idx0, 8, {kick}, true); EXPECT_EQ(reply_u64(), 0u);
        uint32_t en[2] = {0, 1};
        send(18, en, 8, {}, true); EXPECT_EQ(reply_u64(), 0u);
    }

    template <class T> void put(uint64_t off, T v) { std::memcpy(mem + off, &v, sizeof(T)); }
    template <class T> T get(uint64_t off) { T v; std::memcpy(&v, mem + off, sizeof(T)); return v; }

    /* Submits a request: header, data descriptor(s), status. Descriptor slot i
     * of the slot-th request uses desc ids [slot*4, slot*4+3]. */
    void submit(int slot, uint32_t type, uint64_t sector, uint32_t data_len, uint64_t data_off,
                bool split = false)
    {
        const uint64_t hdr = kHdr + slot * 64, status = kHdr + slot * 64 + 32;
        put<uint32_t>(hdr, type); put<uint32_t>(hdr + 4, 0); put<uint64_t>(hdr + 8, sector);
        put<uint8_t>(status, 0xAA);
        auto desc = [&](int i, uint64_t gpa, uint32_t len, uint16_t flags, uint16_t next) {
            uint64_t d = kDesc + (slot * 4 + i) * 16;
            put<uint64_t>(d, kGuestBase + gpa); put<uint32_t>(d + 8, len);
            put<uint16_t>(d + 12, flags); put<uint16_t>(d + 14, next);
        };
        const int b = slot * 4;
        if (data_len == 0) {
            desc(0, hdr, 16, 1, b + 1);
            desc(1, status, 1, 2, 0);
        } else if (!split) {
            desc(0, hdr, 16, 1, b + 1);
            desc(1, data_off, data_len, (type == 0 ? 2 : 0) | 1, b + 2);
            desc(2, status, 1, 2, 0);
        } else {
            desc(0, hdr, 16, 1, b + 1);
            desc(1, data_off, data_len / 2, 2 | 1, b + 2);
            desc(2, data_off + 2 * data_len, data_len - data_len / 2, 2 | 1, b + 3);
            desc(3, status, 1, 2, 0);
        }
        put<uint16_t>(kAvail + 4 + 2 * (avail_idx % kQ), b);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        put<uint16_t>(kAvail + 2, ++avail_idx);
        uint64_t one = 1; ASSERT_EQ(write(kick, &one, 8), 8);
    }

    bool wait_used(uint16_t want, int ms = 5000)
    {
        for (int i = 0; i < ms; ++i) {
            if (get<uint16_t>(kUsed + 2) == want) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    uint8_t status(int slot) { return get<uint8_t>(kHdr + slot * 64 + 32); }
};

struct Rig {
    MockStorage storage;
    DiskMapping mapping;
    std::unique_ptr<RemoteDisk> disk;
    std::unique_ptr<VhostUserBlk> server;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::string path;

    Rig()
    {
        path = "/tmp/claude-1000/vub_test_" + std::to_string(getpid()) + ".sock";
        DiskLayout l; l.object_size = kObj; l.object_count = kCount; l.object_id_start = 1;
        EXPECT_TRUE(DiskMapping::create(l, mapping, nullptr));
        for (uint64_t i = 0; i < kCount; ++i) {
            auto& o = storage.objects[1 + i]; o.resize(kObj);
            for (uint64_t b = 0; b < kObj; ++b) o[b] = pattern(i * kObj + b);
        }
        disk = std::make_unique<RemoteDisk>(storage, mapping, 8, 4, 8);
        server = std::make_unique<VhostUserBlk>(*disk, path);
        std::string err;
        EXPECT_TRUE(server->listen(&err)) << err;
        thread = std::thread([this] { server->run(stop); });
    }
    ~Rig() { stop = true; thread.join(); }
};

bool data_matches(const uint8_t* p, uint64_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) if (p[i] != pattern(off + i)) return false;
    return true;
}

} // namespace

TEST(VhostUserBlk, ConfigAndSingleRead)
{
    Rig rig; Frontend fe;
    ASSERT_TRUE(fe.connect_to(rig.path));
    fe.setup(kObj * kCount / 512);
    fe.submit(0, 0, 3, 1024, kData);
    ASSERT_TRUE(fe.wait_used(1));
    EXPECT_EQ(fe.status(0), 0);
    EXPECT_TRUE(data_matches(fe.mem + kData, 3 * 512, 1024));
    EXPECT_EQ(fe.get<uint32_t>(kUsed + 4 + 4), 1024u + 1); /* used len */
}

TEST(VhostUserBlk, ReadSpanningObjectsAndSplitDescriptors)
{
    Rig rig; Frontend fe;
    ASSERT_TRUE(fe.connect_to(rig.path));
    fe.setup(kObj * kCount / 512);
    fe.submit(0, 0, (kObj - 1024) / 512, 2048, kData);            /* crosses object 0/1 */
    fe.submit(1, 0, 1, 1024, 65536, /*split=*/true);              /* two data descriptors */
    ASSERT_TRUE(fe.wait_used(2));
    EXPECT_EQ(fe.status(0), 0);
    EXPECT_TRUE(data_matches(fe.mem + kData, kObj - 1024, 2048));
    EXPECT_EQ(fe.status(1), 0);
    EXPECT_TRUE(data_matches(fe.mem + 65536, 512, 512));
    EXPECT_TRUE(data_matches(fe.mem + 65536 + 2 * 1024, 512 + 512, 512));
}

TEST(VhostUserBlk, ErrorsAreReportedAsIoErr)
{
    Rig rig; Frontend fe;
    ASSERT_TRUE(fe.connect_to(rig.path));
    fe.setup(kObj * kCount / 512);
    fe.submit(0, 0, kObj * kCount / 512, 512, kData);   /* beyond capacity */
    ASSERT_TRUE(fe.wait_used(1));
    EXPECT_EQ(fe.status(0), 1);
    fe.submit(1, 1, 0, 512, kData);                     /* write: read-only device */
    ASSERT_TRUE(fe.wait_used(2));
    EXPECT_EQ(fe.status(1), 1);
    fe.submit(2, 4, 0, 0, 0);                           /* flush: unsupported */
    ASSERT_TRUE(fe.wait_used(3));
    EXPECT_EQ(fe.status(2), 2);
    fe.submit(3, 0, ~0ull / 256, 512, kData);           /* sector*512 overflow */
    ASSERT_TRUE(fe.wait_used(4));
    EXPECT_EQ(fe.status(3), 1);
}

TEST(VhostUserBlk, StorageFailureAndBackpressure)
{
    Rig rig; Frontend fe;
    ASSERT_TRUE(fe.connect_to(rig.path));
    fe.setup(kObj * kCount / 512);
    /* Mock errors are only touched from the server thread after this point, so
     * set them before submitting. */
    rig.storage.fail_object[2] = IoStatus::kIoError;
    fe.submit(0, 0, kObj / 512, 512, kData);
    ASSERT_TRUE(fe.wait_used(1));
    EXPECT_EQ(fe.status(0), 1);

    /* More requests than RemoteDisk admits (8): the rest must wait, not drop. */
    int n = 12;
    for (int i = 0; i < n; ++i) fe.submit(i + 1, 0, i, 512, kData + 4096 * (i + 1));
    ASSERT_TRUE(fe.wait_used(1 + n));
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(fe.status(i + 1), 0) << i;
        EXPECT_TRUE(data_matches(fe.mem + kData + 4096 * (i + 1), i * 512, 512)) << i;
    }
}

TEST(VhostUserBlk, ReconnectAfterFrontendDisconnect)
{
    Rig rig;
    {
        Frontend fe;
        ASSERT_TRUE(fe.connect_to(rig.path));
        fe.setup(kObj * kCount / 512);
        fe.submit(0, 0, 0, 512, kData);
        ASSERT_TRUE(fe.wait_used(1));
        close(fe.sock);
    }
    Frontend again;
    ASSERT_TRUE(again.connect_to(rig.path));
    again.setup(kObj * kCount / 512);
    again.submit(0, 0, 2, 512, kData);
    ASSERT_TRUE(again.wait_used(1));
    EXPECT_EQ(again.status(0), 0);
    EXPECT_TRUE(data_matches(again.mem + kData, 1024, 512));
}
