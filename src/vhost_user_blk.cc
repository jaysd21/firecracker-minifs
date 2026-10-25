#include "remote_disk/vhost_user_blk.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace remote_disk {
namespace {

/* ---- vhost-user wire constants ---- */
enum Req : uint32_t {
    GET_FEATURES = 1, SET_FEATURES = 2, SET_OWNER = 3, RESET_OWNER = 4,
    SET_MEM_TABLE = 5, SET_LOG_BASE = 6, SET_LOG_FD = 7, SET_VRING_NUM = 8,
    SET_VRING_ADDR = 9, SET_VRING_BASE = 10, GET_VRING_BASE = 11,
    SET_VRING_KICK = 12, SET_VRING_CALL = 13, SET_VRING_ERR = 14,
    GET_PROTOCOL_FEATURES = 15, SET_PROTOCOL_FEATURES = 16,
    GET_QUEUE_NUM = 17, SET_VRING_ENABLE = 18, GET_CONFIG = 24,
    SET_CONFIG = 25, GET_MAX_MEM_SLOTS = 36,
};
constexpr uint32_t kVersion = 1, kReplyFlag = 1u << 2, kNeedReply = 1u << 3;
constexpr uint64_t kProtoReplyAck = 1ull << 3, kProtoConfig = 1ull << 9;
constexpr uint64_t kProtoMq = 1ull << 0;

/* ---- virtio ---- */
constexpr uint64_t kFBlkRo = 1ull << 5, kFBlkSize = 1ull << 6,
                   kFIndirect = 1ull << 28, kFVersion1 = 1ull << 32,
                   kFProtocol = 1ull << 30;
constexpr uint16_t kDescNext = 1, kDescWrite = 2, kDescIndirect = 4;
constexpr uint32_t kBlkIn = 0, kBlkOut = 1, kBlkFlush = 4, kBlkGetId = 8;
constexpr uint8_t kStatusOk = 0, kStatusIoErr = 1, kStatusUnsupp = 2;
constexpr size_t kConfigSize = 60;
constexpr size_t kMaxRegions = 8;

struct Header { uint32_t request, flags, size; };
struct VringDesc { uint64_t addr; uint32_t len; uint16_t flags, next; };

struct Region {
    uint64_t gpa = 0, size = 0, user = 0;
    uint8_t* base = nullptr; /* host mapping of gpa */
    void* map = nullptr;
    size_t map_len = 0;
};

struct Segment { uint8_t* ptr; uint32_t len; };

} // namespace

struct VhostUserBlk::Impl {
    struct Queue {
        uint32_t num = 0;
        VringDesc* desc = nullptr;
        volatile uint16_t* avail = nullptr; /* flags, idx, ring[] */
        uint8_t* used = nullptr;
        uint16_t last_avail = 0;
        bool addr_set = false, enabled = false;
        int kick = -1, call = -1;
        bool stalled = false;
    };
    struct Pending {
        uint16_t head;
        uint32_t data_len;
        std::vector<Segment> segs;
        uint8_t* status;
        std::vector<uint8_t> bounce;
    };

    RemoteDisk& disk;
    std::string path;
    int listen_fd = -1, conn = -1, epfd = -1;
    uint64_t features = 0, proto_features = 0;
    bool owner = false;
    Region regions[kMaxRegions];
    size_t nregions = 0;
    Queue q;
    size_t inflight = 0;
    bool leaked = false;

    explicit Impl(RemoteDisk& d, std::string p) : disk(d), path(std::move(p)) {}

    /* --- memory translation --- */
    uint8_t* gpa_to_host(uint64_t gpa, uint64_t len) const
    {
        for (size_t i = 0; i < nregions; ++i) {
            const Region& r = regions[i];
            if (gpa >= r.gpa && gpa - r.gpa < r.size && len <= r.size - (gpa - r.gpa))
                return r.base + (gpa - r.gpa);
        }
        return nullptr;
    }
    uint8_t* user_to_host(uint64_t user, uint64_t len) const
    {
        for (size_t i = 0; i < nregions; ++i) {
            const Region& r = regions[i];
            if (user >= r.user && user - r.user < r.size && len <= r.size - (user - r.user))
                return r.base + (user - r.user);
        }
        return nullptr;
    }
    void unmap_regions()
    {
        for (size_t i = 0; i < nregions; ++i)
            if (regions[i].map != nullptr) munmap(regions[i].map, regions[i].map_len);
        nregions = 0;
        for (auto& r : regions) r = Region{};
    }

    void close_fd(int& fd)
    {
        if (fd >= 0) { if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr); close(fd); }
        fd = -1;
    }

    /* --- socket helpers --- */
    bool recv_msg(Header& h, uint8_t* payload, size_t cap, int* fds, size_t& nfds)
    {
        nfds = 0;
        alignas(8) char ctrl[CMSG_SPACE(sizeof(int) * kMaxRegions)];
        iovec iov{&h, sizeof(h)};
        msghdr msg{};
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = ctrl; msg.msg_controllen = sizeof(ctrl);
        ssize_t n;
        do { n = recvmsg(conn, &msg, MSG_CMSG_CLOEXEC); } while (n < 0 && errno == EINTR);
        if (n != static_cast<ssize_t>(sizeof(h))) return false;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < count; ++i) {
                    int fd;
                    std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                    if (nfds < kMaxRegions) fds[nfds++] = fd; else close(fd);
                }
            }
        }
        if (h.size > cap) return false;
        size_t got = 0;
        while (got < h.size) {
            ssize_t r = read(conn, payload + got, h.size - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }
    bool send_reply(const Header& req, const void* payload, uint32_t size)
    {
        Header h{req.request, kVersion | kReplyFlag, size};
        uint8_t buf[sizeof(Header) + 256];
        std::memcpy(buf, &h, sizeof(h));
        if (size) std::memcpy(buf + sizeof(h), payload, size);
        size_t sent = 0;
        while (sent < sizeof(h) + size) {
            ssize_t r = send(conn, buf + sent, sizeof(h) + size - sent, MSG_NOSIGNAL);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return false;
            sent += static_cast<size_t>(r);
        }
        return true;
    }
    bool ack(const Header& req, uint64_t value)
    {
        if ((proto_features & kProtoReplyAck) && (req.flags & kNeedReply))
            return send_reply(req, &value, sizeof(value));
        return true;
    }

    /* --- virtqueue --- */
    uint16_t avail_idx() const { return q.avail[1]; }

    void push_used(uint16_t head, uint32_t len)
    {
        uint16_t idx;
        std::memcpy(&idx, q.used + 2, 2);
        uint8_t* elem = q.used + 4 + 8 * (idx % q.num);
        const uint32_t id = head;
        std::memcpy(elem, &id, 4);
        std::memcpy(elem + 4, &len, 4);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        ++idx;
        std::memcpy(q.used + 2, &idx, 2);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!(q.avail[0] & 1) && q.call >= 0) {
            const uint64_t one = 1;
            ssize_t w = write(q.call, &one, sizeof(one));
            (void)w;
        }
    }

    void finish(std::unique_ptr<Pending> p, IoStatus status)
    {
        uint32_t written = 1;
        if (status == IoStatus::kOk) {
            size_t off = 0;
            for (const Segment& s : p->segs) {
                std::memcpy(s.ptr, p->bounce.data() + off, s.len);
                off += s.len;
            }
            written += p->data_len;
            *p->status = kStatusOk;
        } else {
            std::fprintf(stderr, "vhost-user-blk: read failed: %s\n", to_string(status));
            *p->status = kStatusIoErr;
        }
        --inflight;
        push_used(p->head, written);
    }

    void complete_now(uint16_t head, uint8_t* status, uint8_t value, uint32_t len = 1)
    {
        if (status) *status = value;
        push_used(head, len);
    }

    /* Walks one descriptor chain. Returns false on a malformed chain. */
    bool parse_chain(uint16_t head, Pending& p, uint32_t& type, uint64_t& sector)
    {
        std::vector<Segment> all; /* every descriptor, in order */
        std::vector<bool> writable;
        auto walk = [&](VringDesc* table, uint32_t count, uint16_t first) -> bool {
            uint16_t i = first;
            for (uint32_t steps = 0; steps <= count; ++steps) {
                if (i >= count) return false;
                const VringDesc d = table[i];
                if (d.flags & kDescIndirect) return false; /* nested */
                uint8_t* host = gpa_to_host(d.addr, d.len);
                if (host == nullptr) return false;
                all.push_back({host, d.len});
                writable.push_back(d.flags & kDescWrite);
                if (!(d.flags & kDescNext)) return true;
                i = d.next;
            }
            return false; /* loop */
        };
        if (head >= q.num) return false;
        const VringDesc first = q.desc[head];
        if (first.flags & kDescIndirect) {
            if (first.len % sizeof(VringDesc) != 0 || first.len == 0) return false;
            auto* table = reinterpret_cast<VringDesc*>(gpa_to_host(first.addr, first.len));
            if (table == nullptr || !walk(table, first.len / sizeof(VringDesc), 0)) return false;
        } else if (!walk(q.desc, q.num, head)) {
            return false;
        }
        if (all.size() < 2) return false;
        if (all[0].len < 16 || writable[0]) return false;
        uint32_t hdr[4];
        std::memcpy(hdr, all[0].ptr, 16);
        type = hdr[0];
        std::memcpy(&sector, all[0].ptr + 8, 8);
        if (all.back().len < 1 || !writable.back()) return false;
        p.status = all.back().ptr + all.back().len - 1; /* last byte */
        p.data_len = 0;
        for (size_t i = 1; i + 1 < all.size(); ++i) {
            if (type == kBlkIn && !writable[i]) return false;
            if (type == kBlkIn) {
                if (p.data_len > UINT32_MAX - all[i].len) return false;
                p.data_len += all[i].len;
                p.segs.push_back(all[i]);
            }
        }
        /* The status byte may share a descriptor with data: trim it. */
        if (type == kBlkIn && all.back().len > 1) {
            p.data_len += all.back().len - 1;
            p.segs.push_back({all.back().ptr, all.back().len - 1});
        }
        if (type == kBlkGetId && all.size() >= 3) p.segs.push_back(all[1]);
        return true;
    }

    void process_queue()
    {
        q.stalled = false;
        if (!q.addr_set || !q.enabled) return;
        while (q.last_avail != avail_idx()) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            const uint16_t head = q.avail[2 + (q.last_avail % q.num)];
            auto p = std::make_unique<Pending>();
            p->head = head;
            uint32_t type = 0;
            uint64_t sector = 0;
            if (!parse_chain(head, *p, type, sector)) {
                std::fprintf(stderr, "vhost-user-blk: malformed descriptor chain %u\n", head);
                ++q.last_avail;
                push_used(head, 0);
                continue;
            }
            if (type == kBlkIn) {
                if (p->data_len == 0 || p->data_len % kSectorSize != 0 ||
                    sector > UINT64_MAX / kSectorSize) {
                    ++q.last_avail;
                    complete_now(head, p->status, kStatusIoErr);
                    continue;
                }
                p->bounce.resize(p->data_len);
                uint8_t* const status_ptr = p->status;
                const uint32_t data_len = p->data_len;
                uint8_t* const bounce_ptr = p->bounce.data();
                auto holder = std::make_shared<std::unique_ptr<Pending>>(std::move(p));
                const SubmitResult r = disk.read(
                    sector * kSectorSize, data_len, bounce_ptr,
                    [this, holder](IoStatus s) { finish(std::move(*holder), s); });
                if (r == SubmitResult::kBusy) { q.stalled = true; return; }
                ++q.last_avail;
                if (r == SubmitResult::kAccepted) { ++inflight; continue; }
                complete_now(head, status_ptr, kStatusIoErr);
            } else if (type == kBlkGetId) {
                if (!p->segs.empty()) {
                    std::memset(p->segs[0].ptr, 0, p->segs[0].len);
                    std::memcpy(p->segs[0].ptr, "nebula-minifs", std::min<uint32_t>(13, p->segs[0].len));
                }
                ++q.last_avail;
                complete_now(head, p->status, kStatusOk, 1 + (p->segs.empty() ? 0 : p->segs[0].len));
            } else {
                ++q.last_avail; /* OUT / FLUSH / others: read-only device */
                complete_now(head, p->status, type == kBlkOut ? kStatusIoErr : kStatusUnsupp);
            }
        }
    }

    /* --- config space --- */
    void fill_config(uint8_t* cfg)
    {
        std::memset(cfg, 0, kConfigSize);
        const uint64_t sectors = disk.capacity() / kSectorSize;
        const uint32_t blk = kSectorSize;
        std::memcpy(cfg, &sectors, 8);
        std::memcpy(cfg + 20, &blk, 4);
    }

    /* --- message handling --- */
    void stop_queue()
    {
        q.enabled = false;
        close_fd(q.kick);
        close_fd(q.call);
    }

    void drain()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (inflight > 0 && std::chrono::steady_clock::now() < deadline) disk.pump();
        if (inflight > 0) leaked = true; /* bounce buffers stay referenced */
    }

    void disconnect()
    {
        stop_queue();
        drain();
        if (!leaked) unmap_regions();
        q = Queue{};
        close_fd(conn);
        owner = false; features = proto_features = 0;
    }

    bool handle()
    {
        Header h;
        uint8_t payload[512];
        int fds[kMaxRegions];
        size_t nfds = 0;
        if (!recv_msg(h, payload, sizeof(payload), fds, nfds)) {
            for (size_t i = 0; i < nfds; ++i) close(fds[i]);
            return false;
        }
        auto close_all = [&] { for (size_t i = 0; i < nfds; ++i) close(fds[i]); nfds = 0; };
        bool ok = true;
        switch (h.request) {
        case GET_FEATURES: {
            uint64_t f = kFVersion1 | kFProtocol | kFBlkRo | kFBlkSize | kFIndirect;
            ok = send_reply(h, &f, 8);
            break;
        }
        case SET_FEATURES:
            std::memcpy(&features, payload, 8);
            ok = ack(h, 0);
            break;
        case GET_PROTOCOL_FEATURES: {
            uint64_t f = kProtoReplyAck | kProtoConfig;
            ok = send_reply(h, &f, 8);
            break;
        }
        case SET_PROTOCOL_FEATURES:
            std::memcpy(&proto_features, payload, 8);
            ok = ack(h, 0);
            break;
        case GET_QUEUE_NUM: {
            uint64_t n = 1;
            ok = send_reply(h, &n, 8);
            break;
        }
        case GET_MAX_MEM_SLOTS: {
            uint64_t n = kMaxRegions;
            ok = send_reply(h, &n, 8);
            break;
        }
        case SET_OWNER: owner = true; ok = ack(h, 0); break;
        case RESET_OWNER: ok = ack(h, 0); break;
        case SET_MEM_TABLE: {
            uint32_t n;
            std::memcpy(&n, payload, 4);
            bool good = n <= kMaxRegions && n == nfds && h.size >= 8 + 32 * n;
            if (good) {
                stop_queue_keep();
                unmap_regions();
                for (uint32_t i = 0; i < n && good; ++i) {
                    uint64_t f[4];
                    std::memcpy(f, payload + 8 + 32 * i, 32);
                    Region& r = regions[i];
                    r.gpa = f[0]; r.size = f[1]; r.user = f[2];
                    r.map_len = f[1] + f[3];
                    r.map = mmap(nullptr, r.map_len, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fds[i], 0);
                    if (r.map == MAP_FAILED) { r.map = nullptr; good = false; break; }
                    r.base = static_cast<uint8_t*>(r.map) + f[3];
                    nregions = i + 1;
                }
            }
            close_all();
            if (!good) { std::fprintf(stderr, "vhost-user-blk: bad SET_MEM_TABLE\n"); unmap_regions(); ok = false; }
            else ok = ack(h, 0);
            break;
        }
        case SET_VRING_NUM: {
            uint32_t s[2]; std::memcpy(s, payload, 8);
            q.num = s[1];
            ok = ack(h, (s[0] == 0 && s[1] > 0 && s[1] <= 32768) ? 0 : 1);
            break;
        }
        case SET_VRING_ADDR: {
            uint64_t a[6]; std::memcpy(a, payload, 48);
            const uint64_t n = q.num;
            q.desc = reinterpret_cast<VringDesc*>(user_to_host(a[1], 16 * n));
            q.used = user_to_host(a[2], 4 + 8 * n);
            q.avail = reinterpret_cast<volatile uint16_t*>(user_to_host(a[3], 4 + 2 * n));
            q.addr_set = q.desc && q.used && q.avail;
            ok = ack(h, q.addr_set ? 0 : 1);
            break;
        }
        case SET_VRING_BASE: {
            uint32_t s[2]; std::memcpy(s, payload, 8);
            q.last_avail = static_cast<uint16_t>(s[1]);
            ok = ack(h, 0);
            break;
        }
        case GET_VRING_BASE: {
            uint32_t s[2] = {0, q.last_avail};
            stop_queue();
            ok = send_reply(h, s, 8);
            break;
        }
        case SET_VRING_KICK:
        case SET_VRING_CALL: {
            uint64_t v; std::memcpy(&v, payload, 8);
            int& slot = h.request == SET_VRING_KICK ? q.kick : q.call;
            close_fd(slot);
            if (!(v & 0x100) && nfds == 1) {
                slot = fds[0];
                nfds = 0;
                if (h.request == SET_VRING_KICK) {
                    int fl = fcntl(slot, F_GETFL);
                    fcntl(slot, F_SETFL, fl | O_NONBLOCK);
                    epoll_event ev{EPOLLIN, {.fd = slot}};
                    epoll_ctl(epfd, EPOLL_CTL_ADD, slot, &ev);
                }
            }
            close_all();
            ok = ack(h, 0);
            if (ok && h.request == SET_VRING_KICK) process_queue();
            break;
        }
        case SET_VRING_ERR: close_all(); ok = ack(h, 0); break;
        case SET_VRING_ENABLE: {
            uint32_t s[2]; std::memcpy(s, payload, 8);
            q.enabled = s[1] != 0;
            ok = ack(h, 0);
            if (ok) process_queue();
            break;
        }
        case GET_CONFIG: {
            uint32_t off, size, flags;
            std::memcpy(&off, payload, 4); std::memcpy(&size, payload + 4, 4);
            std::memcpy(&flags, payload + 8, 4);
            if (size > kConfigSize || off > kConfigSize || off + size > kConfigSize) {
                ok = false;
                break;
            }
            uint8_t cfg[kConfigSize];
            fill_config(cfg);
            uint8_t reply[12 + kConfigSize];
            std::memcpy(reply, payload, 12);
            std::memcpy(reply + 12, cfg + off, size);
            ok = send_reply(h, reply, 12 + size);
            break;
        }
        case SET_CONFIG: ok = ack(h, 1); break; /* read-only: reject */
        default:
            std::fprintf(stderr, "vhost-user-blk: unsupported request %u\n", h.request);
            close_all();
            ok = (h.flags & kNeedReply) ? ack(h, 1) : true;
            break;
        }
        close_all();
        return ok;
    }

    /* A new memory table invalidates ring pointers; keep fds but force re-SET. */
    void stop_queue_keep() { drain(); q.addr_set = false; }
};

VhostUserBlk::VhostUserBlk(RemoteDisk& disk, std::string socket_path)
    : impl_(std::make_unique<Impl>(disk, std::move(socket_path)))
{
}

VhostUserBlk::~VhostUserBlk()
{
    if (impl_->conn >= 0) impl_->disconnect();
    if (impl_->listen_fd >= 0) { close(impl_->listen_fd); unlink(impl_->path.c_str()); }
    if (impl_->epfd >= 0) close(impl_->epfd);
}

bool VhostUserBlk::listen(std::string* error)
{
    Impl& m = *impl_;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m.path.size() >= sizeof(addr.sun_path)) {
        if (error) *error = "socket path too long";
        return false;
    }
    std::strcpy(addr.sun_path, m.path.c_str());
    unlink(m.path.c_str());
    m.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m.listen_fd < 0 ||
        bind(m.listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(m.listen_fd, 1) != 0) {
        if (error) *error = std::string("cannot listen on socket: ") + std::strerror(errno);
        return false;
    }
    chmod(m.path.c_str(), 0600);
    m.epfd = epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{EPOLLIN, {.fd = m.listen_fd}};
    epoll_ctl(m.epfd, EPOLL_CTL_ADD, m.listen_fd, &ev);
    return true;
}

void VhostUserBlk::run(const std::atomic<bool>& stop_flag)
{
    Impl& m = *impl_;
    while (!stop_flag.load()) {
        const size_t busy = m.disk.pump();
        epoll_event events[8];
        const int n = epoll_wait(m.epfd, events, 8, busy > 0 ? 0 : 100);
        if (busy > 0 && n == 0) sched_yield();
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == m.listen_fd) {
                int c = accept4(m.listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (c < 0) continue;
                if (m.conn >= 0) { close(c); continue; } /* one frontend at a time */
                m.conn = c;
                epoll_event ev{EPOLLIN | EPOLLRDHUP, {.fd = c}};
                epoll_ctl(m.epfd, EPOLL_CTL_ADD, c, &ev);
                std::fprintf(stderr, "vhost-user-blk: frontend connected\n");
            } else if (fd == m.conn) {
                if (!m.handle()) {
                    std::fprintf(stderr, "vhost-user-blk: frontend disconnected\n");
                    m.disconnect();
                    if (m.leaked) { leaked_ = true; return; }
                }
            } else if (fd == m.q.kick) {
                uint64_t v;
                ssize_t r = read(fd, &v, sizeof(v));
                (void)r;
                m.process_queue();
            }
        }
        if (m.q.stalled) m.process_queue(); /* retry after completions */
    }
    if (m.conn >= 0) m.disconnect();
    leaked_ = m.leaked;
}

} // namespace remote_disk
