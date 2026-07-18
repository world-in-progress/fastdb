#pragma once

#include <cstdint>

namespace fastdb::payload::build {

struct ExecutionReport final {
    std::uint32_t mode;
    std::uint32_t fallback_reason;
    std::uint64_t requested_bytes;
    std::uint64_t used_bytes;
    std::uint64_t staging_bytes;
    std::uint64_t region_count;
    std::uint64_t backing_capacity;
};

}  // namespace fastdb::payload::build
