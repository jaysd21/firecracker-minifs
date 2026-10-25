#include "remote_disk/disk_mapping.h"

#include <algorithm>
#include <limits>

namespace remote_disk {

bool DiskMapping::create(const DiskLayout& layout, DiskMapping& out,
                         std::string* error)
{
    auto fail = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (layout.object_size == 0 ||
        layout.object_size > std::numeric_limits<uint32_t>::max())
        return fail("object size must be in 1..4GiB-1");
    if (layout.object_size % kSectorSize != 0)
        return fail("object size must be a multiple of 512");
    if (layout.object_count == 0) return fail("object count must be > 0");

    uint64_t total = 0;
    if (__builtin_mul_overflow(layout.object_size, layout.object_count, &total))
        return fail("object size * count overflows 64 bits");
    if (layout.object_id_start >
        std::numeric_limits<uint64_t>::max() - (layout.object_count - 1))
        return fail("object id range overflows 64 bits");

    uint64_t capacity = total;
    if (layout.disk_bytes != 0) {
        if (layout.disk_bytes > total)
            return fail("disk bytes exceeds object_size * object_count");
        if (layout.disk_bytes % kSectorSize != 0)
            return fail("disk bytes must be a multiple of 512");
        capacity = layout.disk_bytes;
    }
    out.layout_ = layout;
    out.capacity_ = capacity;
    return true;
}

MapStatus DiskMapping::map(uint64_t offset, uint64_t length,
                           std::vector<Extent>& extents) const
{
    extents.clear();
    if (length == 0) return MapStatus::kZeroLength;
    if (offset > capacity_ || length > capacity_ - offset)
        return MapStatus::kOutOfRange;

    const uint64_t size = layout_.object_size;
    uint64_t done = 0;
    while (done < length) {
        const uint64_t position = offset + done; /* < capacity_, no overflow */
        const uint64_t index = position / size;
        const uint64_t within = position % size;
        const uint64_t piece = std::min(length - done, size - within);
        extents.push_back(Extent{layout_.object_id_hi,
                                 layout_.object_id_start + index, within,
                                 static_cast<uint32_t>(piece), done});
        done += piece;
    }
    return MapStatus::kOk;
}

} // namespace remote_disk
