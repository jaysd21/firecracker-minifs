/* Runs RemoteDisk + MiniFsAdapter against a live MiniFS DDL3 target.
 * Driven by scripts/integration_test.sh, which populates the image first.
 *   MINIFS_SERVER, MINIFS_PORT, MINIFS_TENANT, MINIFS_OBJECT_HI,
 *   MINIFS_OBJECT_START, MINIFS_OBJECT_SIZE, MINIFS_IMAGE */
#include "remote_disk/minifs_adapter.h"
#include "remote_disk/remote_disk.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

using namespace remote_disk;

namespace {

std::string env(const char* name, const char* fallback = "")
{
    const char* v = std::getenv(name);
    return v ? v : fallback;
}
uint64_t env_u64(const char* name, uint64_t fallback)
{
    const char* v = std::getenv(name);
    return v ? std::strtoull(v, nullptr, 0) : fallback;
}

class Live : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (env("MINIFS_SERVER").empty()) GTEST_SKIP() << "MINIFS_SERVER not set";
        std::ifstream in(env("MINIFS_IMAGE"), std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in) << "MINIFS_IMAGE unreadable";
        image.resize(static_cast<size_t>(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(image.data()), image.size());

        config.server = env("MINIFS_SERVER");
        config.port = env_u64("MINIFS_PORT", 58000);
        config.tenant = env_u64("MINIFS_TENANT", 1);
        object_size = env_u64("MINIFS_OBJECT_SIZE", 1u << 20);
        config.max_io_bytes = object_size;
        config.max_inflight = 8;
        config.timeout_seconds = 15;
        layout.object_id_hi = env_u64("MINIFS_OBJECT_HI", 0xdd1c000000000000ull);
        layout.object_id_start = env_u64("MINIFS_OBJECT_START", 1);
        layout.object_size = object_size;
        populated = (image.size() + object_size - 1) / object_size;
        layout.object_count = populated + 1; /* last object intentionally missing */
        layout.disk_bytes = 0;
        std::string err;
        ASSERT_TRUE(DiskMapping::create(layout, mapping, &err)) << err;
        client = MiniFsAdapter::connect(config, &err);
        ASSERT_NE(client, nullptr) << err;
        disk = std::make_unique<RemoteDisk>(*client, mapping, 32, 8, 64);
    }
    void TearDown() override
    {
        if (disk) { EXPECT_TRUE(disk->shutdown(5000)); }
        if (client) client->stop();
    }

    IoStatus read(uint64_t off, uint64_t len, std::vector<uint8_t>& out)
    {
        out.assign(len, 0xEE);
        bool done = false;
        IoStatus st = IoStatus::kIoError;
        auto r = disk->read(off, len, out.data(), [&](IoStatus s) { st = s; done = true; });
        if (r != SubmitResult::kAccepted) return IoStatus::kInvalidRange;
        while (!done) disk->pump();
        return st;
    }

    MiniFsConfig config;
    DiskLayout layout;
    DiskMapping mapping;
    std::vector<uint8_t> image;
    uint64_t object_size = 0, populated = 0;
    std::unique_ptr<MiniFsAdapter> client;
    std::unique_ptr<RemoteDisk> disk;
};

} // namespace

TEST_F(Live, ReadsMatchImage)
{
    std::vector<uint8_t> out;
    const uint64_t cases[][2] = {
        {0, 512}, {512, 4096}, {object_size - 512, 1024},      /* spans 0/1 */
        {object_size, object_size},                            /* whole object */
        {object_size / 2, object_size * 2},                    /* spans 3 objects */
        {image.size() - 512, 512},                             /* tail of image */
    };
    for (auto& c : cases) {
        if (c[0] + c[1] > image.size()) continue;
        ASSERT_EQ(read(c[0], c[1], out), IoStatus::kOk) << c[0] << "+" << c[1];
        EXPECT_EQ(0, std::memcmp(out.data(), image.data() + c[0], c[1])) << c[0] << "+" << c[1];
    }
}

TEST_F(Live, RandomConcurrentReads)
{
    std::mt19937_64 rng(1234);
    struct Job { uint64_t off, len; std::vector<uint8_t> buf; int done = 0; IoStatus st = IoStatus::kOk; };
    std::vector<Job> jobs(200);
    size_t issued = 0, finished = 0;
    while (finished < jobs.size()) {
        while (issued < jobs.size()) {
            Job& j = jobs[issued];
            j.len = (1 + rng() % 256) * 512;
            j.off = (rng() % ((image.size() - j.len) / 512)) * 512;
            j.buf.assign(j.len, 0);
            const size_t idx = issued;
            auto r = disk->read(j.off, j.len, j.buf.data(), [&jobs, &finished, idx](IoStatus s) {
                jobs[idx].st = s; ++jobs[idx].done; ++finished; });
            if (r == SubmitResult::kBusy) break;
            ASSERT_EQ(r, SubmitResult::kAccepted);
            ++issued;
        }
        disk->pump();
    }
    for (auto& j : jobs) {
        ASSERT_EQ(j.done, 1);
        ASSERT_EQ(j.st, IoStatus::kOk);
        ASSERT_EQ(0, std::memcmp(j.buf.data(), image.data() + j.off, j.len));
    }
}

TEST_F(Live, MissingObjectFails)
{
    std::vector<uint8_t> out;
    const IoStatus st = read(populated * object_size, 512, out);
    std::printf("missing-object GET status: %s\n", to_string(st));
    EXPECT_NE(st, IoStatus::kOk);
}

TEST_F(Live, PartialFailureAcrossPresentAndMissingObject)
{
    std::vector<uint8_t> out;
    EXPECT_NE(read(populated * object_size - 512, 1024, out), IoStatus::kOk);
    EXPECT_EQ(disk->inflight_requests(), 0u);
    /* Disk still usable afterwards. */
    ASSERT_EQ(read(0, 512, out), IoStatus::kOk);
    EXPECT_EQ(0, std::memcmp(out.data(), image.data(), 512));
}

TEST_F(Live, OutOfRangeRejectedLocally)
{
    std::vector<uint8_t> out(512);
    EXPECT_EQ(disk->read(mapping.capacity(), 512, out.data(), [](IoStatus) {}),
              SubmitResult::kInvalidRange);
}
