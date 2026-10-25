#pragma once
#include "remote_disk/disk_config.h"

#include <string>
#include <vector>

namespace remote_disk {

/* One contiguous piece of a disk request that lies inside a single object. */
struct Extent {
    uint64_t object_id_hi;
    uint64_t object_id_lo;
    uint64_t object_offset;
    uint32_t length;
    uint64_t buffer_offset; /* offset into the caller's request buffer */
};

enum class MapStatus { kOk, kZeroLength, kOutOfRange };

constexpr uint64_t kSectorSize = 512;

class DiskMapping {
public:
    /* Validates the layout (alignment, overflow, id range). */
    static bool create(const DiskLayout& layout, DiskMapping& out,
                       std::string* error);

    uint64_t capacity() const { return capacity_; }
    uint64_t object_size() const { return layout_.object_size; }
    const DiskLayout& layout() const { return layout_; }

    /* Splits [offset, offset+length) into per-object extents. Never wraps. */
    MapStatus map(uint64_t offset, uint64_t length,
                  std::vector<Extent>& extents) const;

private:
    DiskLayout layout_{};
    uint64_t capacity_ = 0;
};

} // namespace remote_disk
