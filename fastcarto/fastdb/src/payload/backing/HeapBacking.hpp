#pragma once

#include "payload/backing/Backing.hpp"

#include <cstdint>

namespace fastdb::payload::backing {

const Callbacks& heap_callbacks() noexcept;

#if defined(FASTDB_PAYLOAD_BUILD_TESTING)
struct HeapReserveObservation final {
    std::uint64_t direct_reserves;
    std::uint64_t staged_reserves;
};

void reset_heap_reserve_observation() noexcept;
HeapReserveObservation heap_reserve_observation() noexcept;
#endif

}  // namespace fastdb::payload::backing
