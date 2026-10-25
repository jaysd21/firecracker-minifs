#include "remote_disk/disk_mapping.h"

#include <gtest/gtest.h>
#include <limits>

using namespace remote_disk;

namespace {
DiskMapping make(uint64_t size = 4096, uint64_t count = 4, uint64_t start = 100,
                 uint64_t disk_bytes = 0)
{
    DiskLayout l;
    l.object_size = size; l.object_count = count; l.object_id_start = start;
    l.disk_bytes = disk_bytes;
    DiskMapping m;
    std::string err;
    EXPECT_TRUE(DiskMapping::create(l, m, &err)) << err;
    return m;
}
} // namespace

TEST(DiskMapping, Capacity)
{
    EXPECT_EQ(make().capacity(), 4u * 4096);
    EXPECT_EQ(make(4096, 4, 1, 3 * 4096 + 512).capacity(), 3u * 4096 + 512);
}

TEST(DiskMapping, ReadAtStart)
{
    std::vector<Extent> e;
    ASSERT_EQ(make().map(0, 512, e), MapStatus::kOk);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].object_id_lo, 100u);
    EXPECT_EQ(e[0].object_offset, 0u);
    EXPECT_EQ(e[0].length, 512u);
}

TEST(DiskMapping, ReadMiddleOfObject)
{
    std::vector<Extent> e;
    ASSERT_EQ(make().map(4096 + 1024, 1024, e), MapStatus::kOk);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].object_id_lo, 101u);
    EXPECT_EQ(e[0].object_offset, 1024u);
}

TEST(DiskMapping, ReadEndingAtObjectBoundary)
{
    std::vector<Extent> e;
    ASSERT_EQ(make().map(4096 - 512, 512, e), MapStatus::kOk);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].object_id_lo, 100u);
    EXPECT_EQ(e[0].object_offset + e[0].length, 4096u);
}

TEST(DiskMapping, ReadSpanningTwoObjects)
{
    std::vector<Extent> e;
    ASSERT_EQ(make().map(4096 - 512, 1024, e), MapStatus::kOk);
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].object_id_lo, 100u);
    EXPECT_EQ(e[0].object_offset, 4096u - 512);
    EXPECT_EQ(e[0].length, 512u);
    EXPECT_EQ(e[0].buffer_offset, 0u);
    EXPECT_EQ(e[1].object_id_lo, 101u);
    EXPECT_EQ(e[1].object_offset, 0u);
    EXPECT_EQ(e[1].length, 512u);
    EXPECT_EQ(e[1].buffer_offset, 512u);
}

TEST(DiskMapping, ReadSpanningManyObjects)
{
    std::vector<Extent> e;
    ASSERT_EQ(make().map(512, 4096 * 3, e), MapStatus::kOk);
    ASSERT_EQ(e.size(), 4u);
    uint64_t total = 0;
    for (size_t i = 0; i < e.size(); ++i) {
        EXPECT_EQ(e[i].object_id_lo, 100u + i);
        EXPECT_EQ(e[i].buffer_offset, total);
        total += e[i].length;
    }
    EXPECT_EQ(total, 4096u * 3);
    EXPECT_EQ(e[1].length, 4096u);
    EXPECT_EQ(e[1].object_offset, 0u);
}

TEST(DiskMapping, FinalValidByte)
{
    std::vector<Extent> e;
    ASSERT_EQ(make().map(4 * 4096 - 1, 1, e), MapStatus::kOk);
    EXPECT_EQ(e[0].object_id_lo, 103u);
    EXPECT_EQ(e[0].object_offset, 4095u);
    EXPECT_EQ(make().map(4 * 4096, 1, e), MapStatus::kOutOfRange);
}

TEST(DiskMapping, WholeDisk)
{
    std::vector<Extent> e;
    ASSERT_EQ(make().map(0, 4 * 4096, e), MapStatus::kOk);
    EXPECT_EQ(e.size(), 4u);
}

TEST(DiskMapping, BeyondCapacity)
{
    std::vector<Extent> e;
    auto m = make();
    EXPECT_EQ(m.map(4 * 4096 - 512, 1024, e), MapStatus::kOutOfRange);
    EXPECT_EQ(m.map(4 * 4096 + 1, 1, e), MapStatus::kOutOfRange);
    EXPECT_TRUE(e.empty());
}

TEST(DiskMapping, CapacityShorterThanObjects)
{
    std::vector<Extent> e;
    auto m = make(4096, 4, 1, 4096 + 512);
    EXPECT_EQ(m.map(4096, 512, e), MapStatus::kOk);
    EXPECT_EQ(m.map(4096 + 512, 512, e), MapStatus::kOutOfRange);
}

TEST(DiskMapping, ZeroLength)
{
    std::vector<Extent> e;
    EXPECT_EQ(make().map(0, 0, e), MapStatus::kZeroLength);
}

TEST(DiskMapping, ArithmeticOverflow)
{
    std::vector<Extent> e;
    auto m = make();
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(m.map(max, 1, e), MapStatus::kOutOfRange);
    EXPECT_EQ(m.map(1, max, e), MapStatus::kOutOfRange);
    EXPECT_EQ(m.map(max - 511, 1024, e), MapStatus::kOutOfRange);
    EXPECT_EQ(m.map(4096, max - 4095, e), MapStatus::kOutOfRange);
    EXPECT_EQ(m.map(max, max, e), MapStatus::kOutOfRange);
}

TEST(DiskMapping, RejectsBadLayouts)
{
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    DiskMapping m;
    std::string err;
    DiskLayout l;
    l.object_count = 1;
    l.object_size = 0;                EXPECT_FALSE(DiskMapping::create(l, m, &err));
    l.object_size = 1000;             EXPECT_FALSE(DiskMapping::create(l, m, &err));
    l.object_size = 4096; l.object_count = 0; EXPECT_FALSE(DiskMapping::create(l, m, &err));
    l.object_count = max;             EXPECT_FALSE(DiskMapping::create(l, m, &err)); /* size*count */
    l.object_size = 1ull << 33; l.object_count = 1; EXPECT_FALSE(DiskMapping::create(l, m, &err));
    l.object_size = 4096; l.object_count = 4; l.object_id_start = max - 2;
    EXPECT_FALSE(DiskMapping::create(l, m, &err)); /* id range wraps */
    l.object_id_start = max - 3;      EXPECT_TRUE(DiskMapping::create(l, m, &err));
    l.disk_bytes = 5 * 4096;          EXPECT_FALSE(DiskMapping::create(l, m, &err));
    l.disk_bytes = 100;               EXPECT_FALSE(DiskMapping::create(l, m, &err));
}

TEST(DiskMapping, ObjectIdHighWordCarried)
{
    DiskLayout l;
    l.object_id_hi = 0xabcdef; l.object_size = 512; l.object_count = 2;
    DiskMapping m;
    ASSERT_TRUE(DiskMapping::create(l, m, nullptr));
    std::vector<Extent> e;
    ASSERT_EQ(m.map(0, 1024, e), MapStatus::kOk);
    EXPECT_EQ(e[0].object_id_hi, 0xabcdefu);
    EXPECT_EQ(e[1].object_id_hi, 0xabcdefu);
}
