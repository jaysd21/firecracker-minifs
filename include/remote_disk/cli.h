#pragma once
#include "remote_disk/disk_config.h"

#include <cerrno>
#include <cstdlib>
#include <string>

namespace remote_disk {

inline bool parse_u64(const char* text, uint64_t& value)
{
    if (text == nullptr || *text == '\0' || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') return false;
    value = parsed;
    return true;
}

/* Parses the options shared by the backend and the populate tool. Returns 1 if
 * consumed (advancing index), 0 if not recognised, -1 on a bad value. */
inline int parse_common_option(const std::string& opt, int& index, int argc,
                               char** argv, DiskConfig& c)
{
    auto next = [&]() -> const char* { return index + 1 < argc ? argv[++index] : nullptr; };
    uint64_t v = 0;
    if (opt == "--server") { const char* t = next(); if (!t) return -1; c.minifs.server = t; }
    else if (opt == "--port") { if (!parse_u64(next(), v) || v == 0 || v > 65535) return -1; c.minifs.port = v; }
    else if (opt == "--tenant") { if (!parse_u64(next(), v) || v > 65535) return -1; c.minifs.tenant = v; }
    else if (opt == "--object-hi") { if (!parse_u64(next(), c.layout.object_id_hi)) return -1; }
    else if (opt == "--object-start") { if (!parse_u64(next(), c.layout.object_id_start)) return -1; }
    else if (opt == "--object-size") { if (!parse_u64(next(), c.layout.object_size)) return -1; }
    else if (opt == "--objects") { if (!parse_u64(next(), c.layout.object_count)) return -1; }
    else if (opt == "--disk-bytes") { if (!parse_u64(next(), c.layout.disk_bytes)) return -1; }
    else if (opt == "--ddl-cpu") { if (!parse_u64(next(), v) || v > 65535) return -1; c.minifs.ddl_cpu = v; }
    else if (opt == "--numa") { if (!parse_u64(next(), v) || v > 32767) return -1; c.minifs.numa_node = v; }
    else if (opt == "--timeout-seconds") { if (!parse_u64(next(), v) || v == 0 || v > 86400) return -1; c.minifs.timeout_seconds = v; }
    else if (opt == "--max-inflight") { if (!parse_u64(next(), v) || v == 0 || v > 1024) return -1; c.max_inflight_chunks = v; c.minifs.max_inflight = v; }
    else return 0;
    return 1;
}

} // namespace remote_disk
