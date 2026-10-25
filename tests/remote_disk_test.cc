#include "mock_storage.h"
#include "remote_disk/remote_disk.h"

#include <gtest/gtest.h>
#include <memory>

using namespace remote_disk;
using remote_disk::testing::MockStorage;

namespace {

constexpr uint64_t kObj = 4096;
constexpr uint64_t kCount = 4;

uint8_t pattern(uint64_t disk_offset) { return static_cast<uint8_t>((disk_offset * 131) ^ (disk_offset >> 7)); }

struct Fixture {
    MockStorage storage;
    DiskMapping mapping;
    std::unique_ptr<RemoteDisk> disk;

    explicit Fixture(uint32_t max_requests = 8, uint32_t max_chunks = 4, uint32_t retries = 8)
    {
        DiskLayout l;
        l.object_size = kObj; l.object_count = kCount; l.object_id_start = 10;
        EXPECT_TRUE(DiskMapping::create(l, mapping, nullptr));
        for (uint64_t i = 0; i < kCount; ++i) {
            auto& o = storage.objects[10 + i];
            o.resize(kObj);
            for (uint64_t b = 0; b < kObj; ++b) o[b] = pattern(i * kObj + b);
        }
        disk = std::make_unique<RemoteDisk>(storage, mapping, max_requests, max_chunks, retries);
    }

    /* Reads and pumps to completion. Returns the status the callback saw. */
    IoStatus read(uint64_t off, uint64_t len, std::vector<uint8_t>& buf, int* calls = nullptr)
    {
        buf.assign(len, 0xEE);
        int count = 0;
        IoStatus got = IoStatus::kIoError;
        EXPECT_EQ(disk->read(off, len, buf.data(), [&](IoStatus s) { got = s; ++count; }),
                  SubmitResult::kAccepted);
        for (int i = 0; i < 1000 && disk->pump() > 0; ++i) {}
        if (calls) *calls = count; else EXPECT_EQ(count, 1);
        return got;
    }
    static bool matches(const std::vector<uint8_t>& buf, uint64_t off)
    {
        for (size_t i = 0; i < buf.size(); ++i)
            if (buf[i] != pattern(off + i)) return false;
        return true;
    }
};

} // namespace

TEST(RemoteDisk, ReadAtStart)
{
    Fixture f; std::vector<uint8_t> b;
    EXPECT_EQ(f.read(0, 512, b), IoStatus::kOk);
    EXPECT_TRUE(Fixture::matches(b, 0));
}

TEST(RemoteDisk, ReadMiddleOfObject)
{
    Fixture f; std::vector<uint8_t> b;
    EXPECT_EQ(f.read(kObj + 1024, 2048, b), IoStatus::kOk);
    EXPECT_TRUE(Fixture::matches(b, kObj + 1024));
    ASSERT_EQ(f.storage.log.size(), 1u);
    EXPECT_EQ(f.storage.log[0].object_id_lo, 11u);
    EXPECT_EQ(f.storage.log[0].offset, 1024u);
}

TEST(RemoteDisk, ReadEndingAtBoundary)
{
    Fixture f; std::vector<uint8_t> b;
    EXPECT_EQ(f.read(kObj - 512, 512, b), IoStatus::kOk);
    EXPECT_TRUE(Fixture::matches(b, kObj - 512));
    EXPECT_EQ(f.storage.submits, 1u);
}

TEST(RemoteDisk, ReadSpanningTwoObjects)
{
    Fixture f; std::vector<uint8_t> b;
    EXPECT_EQ(f.read(kObj - 512, 1024, b), IoStatus::kOk);
    EXPECT_TRUE(Fixture::matches(b, kObj - 512));
    EXPECT_EQ(f.storage.submits, 2u);
}

TEST(RemoteDisk, ReadSpanningManyObjectsOutOfOrder)
{
    Fixture f(8, 8);
    f.storage.reverse_order = true;
    std::vector<uint8_t> b;
    EXPECT_EQ(f.read(512, kObj * 3, b), IoStatus::kOk);
    EXPECT_TRUE(Fixture::matches(b, 512));
    EXPECT_EQ(f.storage.submits, 4u);
}

TEST(RemoteDisk, FinalValidByte)
{
    Fixture f; std::vector<uint8_t> b;
    EXPECT_EQ(f.read(kCount * kObj - 1, 1, b), IoStatus::kOk);
    EXPECT_EQ(b[0], pattern(kCount * kObj - 1));
}

TEST(RemoteDisk, RejectsOutOfRangeAndOverflow)
{
    Fixture f; std::vector<uint8_t> b(16);
    auto cb = [](IoStatus) { FAIL() << "callback must not fire for rejected reads"; };
    const uint64_t max = ~0ull;
    EXPECT_EQ(f.disk->read(kCount * kObj, 1, b.data(), cb), SubmitResult::kInvalidRange);
    EXPECT_EQ(f.disk->read(kCount * kObj - 1, 2, b.data(), cb), SubmitResult::kInvalidRange);
    EXPECT_EQ(f.disk->read(max, 1, b.data(), cb), SubmitResult::kInvalidRange);
    EXPECT_EQ(f.disk->read(2, max, b.data(), cb), SubmitResult::kInvalidRange);
    EXPECT_EQ(f.disk->read(0, 0, b.data(), cb), SubmitResult::kInvalidRange);
    EXPECT_EQ(f.disk->read(0, 16, nullptr, cb), SubmitResult::kInvalidRange);
    EXPECT_EQ(f.storage.submits, 0u);
    EXPECT_EQ(f.disk->inflight_requests(), 0u);
}

TEST(RemoteDisk, MissingObjectPropagates)
{
    Fixture f; std::vector<uint8_t> b;
    f.storage.objects.erase(12);
    EXPECT_EQ(f.read(2 * kObj, 512, b), IoStatus::kNotFound);
}

TEST(RemoteDisk, RemoteFailurePropagates)
{
    Fixture f; std::vector<uint8_t> b;
    f.storage.fail_object[10] = IoStatus::kIoError;
    EXPECT_EQ(f.read(0, 512, b), IoStatus::kIoError);
    f.storage.fail_object[10] = IoStatus::kTimeout;
    EXPECT_EQ(f.read(0, 512, b), IoStatus::kTimeout);
    f.storage.fail_object[10] = IoStatus::kDisconnected;
    EXPECT_EQ(f.read(0, 512, b), IoStatus::kDisconnected);
}

TEST(RemoteDisk, PartialFailureInMultiObjectRead)
{
    Fixture f(8, 8); std::vector<uint8_t> b;
    f.storage.fail_object[11] = IoStatus::kIoError; /* middle of three */
    int calls = 0;
    EXPECT_EQ(f.read(0, kObj * 3, b, &calls), IoStatus::kIoError);
    EXPECT_EQ(calls, 1); /* completed once, after every chunk settled */
    EXPECT_EQ(f.disk->inflight_requests(), 0u);
    EXPECT_EQ(f.storage.pending(), 0u);
}

TEST(RemoteDisk, FailureStopsSubmittingRemainingChunks)
{
    Fixture f(8, /*max_chunks=*/1); std::vector<uint8_t> b;
    f.storage.fail_object[10] = IoStatus::kIoError;
    EXPECT_EQ(f.read(0, kObj * 4, b), IoStatus::kIoError);
    EXPECT_EQ(f.storage.submits, 1u); /* chunks 2..4 never sent */
}

TEST(RemoteDisk, ShortReadIsAnError)
{
    Fixture f; std::vector<uint8_t> b;
    f.storage.short_reads = true;
    EXPECT_EQ(f.read(0, 512, b), IoStatus::kShortRead);
}

TEST(RemoteDisk, QueueBackpressureThenProgress)
{
    Fixture f(8, 8);
    f.storage.queue_limit = 1; /* storage accepts one GET at a time */
    std::vector<uint8_t> b;
    EXPECT_EQ(f.read(0, kObj * 4, b), IoStatus::kOk);
    EXPECT_TRUE(Fixture::matches(b, 0));
    EXPECT_EQ(f.storage.submits, 4u);
}

TEST(RemoteDisk, TransientQueueFullIsRetried)
{
    Fixture f; std::vector<uint8_t> b;
    f.storage.transient_failures = 3;
    EXPECT_EQ(f.read(0, 512, b), IoStatus::kOk);
    EXPECT_TRUE(Fixture::matches(b, 0));
    EXPECT_EQ(f.storage.submits, 4u);
}

TEST(RemoteDisk, TransientRetriesAreBounded)
{
    Fixture f(8, 4, /*retries=*/2); std::vector<uint8_t> b;
    f.storage.transient_failures = 100;
    EXPECT_EQ(f.read(0, 512, b), IoStatus::kQueueFull);
    EXPECT_EQ(f.storage.submits, 3u);
}

TEST(RemoteDisk, RequestAdmissionIsBounded)
{
    Fixture f(/*max_requests=*/2);
    f.storage.hold = true;
    std::vector<uint8_t> a(512), b(512), c(512);
    int done = 0;
    auto cb = [&](IoStatus s) { EXPECT_EQ(s, IoStatus::kOk); ++done; };
    EXPECT_EQ(f.disk->read(0, 512, a.data(), cb), SubmitResult::kAccepted);
    EXPECT_EQ(f.disk->read(512, 512, b.data(), cb), SubmitResult::kAccepted);
    EXPECT_EQ(f.disk->read(1024, 512, c.data(), cb), SubmitResult::kBusy);
    f.storage.hold = false;
    while (f.disk->pump() > 0) {}
    EXPECT_EQ(done, 2);
    EXPECT_EQ(f.disk->read(1024, 512, c.data(), cb), SubmitResult::kAccepted);
    while (f.disk->pump() > 0) {}
    EXPECT_EQ(done, 3);
}

TEST(RemoteDisk, CallbackNeverRunsInsideRead)
{
    Fixture f; std::vector<uint8_t> b(512);
    bool fired = false;
    ASSERT_EQ(f.disk->read(0, 512, b.data(), [&](IoStatus) { fired = true; }), SubmitResult::kAccepted);
    EXPECT_FALSE(fired);
    while (f.disk->pump() > 0) {}
    EXPECT_TRUE(fired);
}

TEST(RemoteDisk, BufferStaysReferencedUntilCompletion)
{
    Fixture f; std::vector<uint8_t> b(1024, 0xEE);
    f.storage.hold = true;
    bool fired = false;
    ASSERT_EQ(f.disk->read(0, 1024, b.data(), [&](IoStatus) { fired = true; }), SubmitResult::kAccepted);
    f.disk->pump();
    f.disk->pump();
    EXPECT_FALSE(fired);
    EXPECT_EQ(b[0], 0xEE); /* nothing written, nothing completed yet */
    EXPECT_EQ(f.storage.log.back().dst, b.data());
    f.storage.hold = false;
    while (f.disk->pump() > 0) {}
    EXPECT_TRUE(fired);
    EXPECT_TRUE(Fixture::matches(b, 0));
}

TEST(RemoteDisk, ManyOutstandingReads)
{
    Fixture f(64, 8);
    f.storage.reverse_order = true;
    constexpr int N = 40;
    std::vector<std::vector<uint8_t>> bufs(N, std::vector<uint8_t>(1024));
    std::vector<int> calls(N, 0);
    for (int i = 0; i < N; ++i) {
        const uint64_t off = (i * 1536ull) % (kCount * kObj - 1024) / 512 * 512;
        ASSERT_EQ(f.disk->read(off, 1024, bufs[i].data(), [&, i](IoStatus s) {
            EXPECT_EQ(s, IoStatus::kOk); ++calls[i]; }), SubmitResult::kAccepted);
    }
    while (f.disk->pump() > 0) {}
    for (int i = 0; i < N; ++i) {
        const uint64_t off = (i * 1536ull) % (kCount * kObj - 1024) / 512 * 512;
        EXPECT_EQ(calls[i], 1);
        EXPECT_TRUE(Fixture::matches(bufs[i], off)) << i;
    }
}

TEST(RemoteDisk, ShutdownFailsQueuedAndRejectsNew)
{
    Fixture f(8, 1);
    f.storage.hold = true;
    std::vector<uint8_t> a(kObj * 3), c(512);
    std::vector<IoStatus> st;
    ASSERT_EQ(f.disk->read(0, kObj * 3, a.data(), [&](IoStatus s) { st.push_back(s); }), SubmitResult::kAccepted);
    f.disk->pump(); /* one chunk in flight, two queued */
    f.storage.hold = false;
    EXPECT_TRUE(f.disk->shutdown(1000));
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0], IoStatus::kShutdown);
    EXPECT_EQ(f.disk->read(0, 512, c.data(), [](IoStatus) {}), SubmitResult::kShutdown);
}

TEST(RemoteDisk, ShutdownTimeoutStillCompletesExactlyOnce)
{
    Fixture f;
    f.storage.hold = true; /* transport never answers */
    std::vector<uint8_t> a(512);
    int calls = 0;
    ASSERT_EQ(f.disk->read(0, 512, a.data(), [&](IoStatus s) { EXPECT_EQ(s, IoStatus::kShutdown); ++calls; }),
              SubmitResult::kAccepted);
    f.disk->pump();
    EXPECT_FALSE(f.disk->shutdown(20)); /* reports buffers still referenced */
    EXPECT_EQ(calls, 1);
    f.storage.hold = false;
    f.disk->pump(); /* late completion is ignored, no second callback */
    EXPECT_EQ(calls, 1);
}

TEST(RemoteDisk, ReentrantReadFromCallback)
{
    Fixture f; std::vector<uint8_t> a(512), b(512);
    bool second = false;
    ASSERT_EQ(f.disk->read(0, 512, a.data(), [&](IoStatus) {
        EXPECT_EQ(f.disk->read(512, 512, b.data(), [&](IoStatus) { second = true; }), SubmitResult::kAccepted);
    }), SubmitResult::kAccepted);
    while (f.disk->pump() > 0) {}
    EXPECT_TRUE(second);
}

TEST(RemoteDisk, ReadOnlyByConstruction)
{
    /* StorageClient exposes no write path; the mock never sees anything but GETs. */
    Fixture f; std::vector<uint8_t> b;
    EXPECT_EQ(f.read(0, 4096, b), IoStatus::kOk);
    auto before = f.storage.objects;
    EXPECT_EQ(f.read(512, 2048, b), IoStatus::kOk);
    EXPECT_EQ(before, f.storage.objects);
}
