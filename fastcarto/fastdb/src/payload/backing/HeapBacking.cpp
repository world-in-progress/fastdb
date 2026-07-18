#include "payload/backing/HeapBacking.hpp"

#include <fastdb_payload.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace fastdb::payload::backing {
namespace {

constexpr std::uint32_t success_status = UINT32_C(0);

struct HeapReservation final {
    HeapReservation(std::size_t storage_size,
                    std::uint64_t minimum_capacity,
                    std::uint32_t alignment)
        : storage(storage_size),
          capacity(minimum_capacity) {
        const std::uintptr_t begin =
            reinterpret_cast<std::uintptr_t>(storage.data());
        const std::uintptr_t mask =
            static_cast<std::uintptr_t>(alignment) - std::uintptr_t{1};
        const std::uintptr_t aligned =
            (begin + mask) & ~mask;
        writable = reinterpret_cast<std::uint8_t*>(aligned);
    }

    std::vector<std::uint8_t> storage;
    std::uint8_t* writable{nullptr};
    std::uint64_t capacity;
    std::atomic<std::uint64_t> references{UINT64_C(1)};
};

bool checked_storage_size(std::uint64_t minimum_capacity,
                          std::uint32_t alignment,
                          std::size_t& out_size) noexcept {
    if (alignment == UINT32_C(0) ||
        (alignment & (alignment - UINT32_C(1))) != UINT32_C(0)) {
        return false;
    }
    const std::uint64_t padding =
        static_cast<std::uint64_t>(alignment) - UINT64_C(1);
    if (minimum_capacity >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()) - padding) {
        return false;
    }
    const std::size_t requested = static_cast<std::size_t>(
        minimum_capacity + padding);
    if (requested > std::vector<std::uint8_t>{}.max_size()) {
        return false;
    }
    out_size = requested;
    return true;
}

std::uint32_t reserve(void*,
                      std::uint32_t,
                      std::uint64_t minimum_capacity,
                      std::uint32_t alignment,
                      void** out_owner_token,
                      std::uint8_t** out_writable_data,
                      std::uint64_t* out_capacity) noexcept {
    try {
        std::size_t storage_size = 0U;
        if (!checked_storage_size(minimum_capacity, alignment,
                                  storage_size)) {
            return FDB_PAYLOAD_E_ALLOCATION_FAILED;
        }
        auto* reservation = new HeapReservation(
            storage_size, minimum_capacity, alignment);
        *out_owner_token = reservation;
        *out_writable_data = reservation->writable;
        *out_capacity = reservation->capacity;
        return success_status;
    } catch (const std::bad_alloc&) {
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    } catch (const std::length_error&) {
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }
}

std::uint32_t write(void*,
                    void* owner_token,
                    std::uint64_t offset,
                    const std::uint8_t* source,
                    std::uint64_t source_size) noexcept {
    auto* reservation = static_cast<HeapReservation*>(owner_token);
    if ((source == nullptr && source_size != UINT64_C(0)) ||
        offset > reservation->capacity ||
        source_size > reservation->capacity - offset) {
        return FDB_PAYLOAD_E_BACKING_CONTRACT;
    }
    if (source_size != UINT64_C(0)) {
        std::memcpy(reservation->writable + offset, source,
                    static_cast<std::size_t>(source_size));
    }
    return success_status;
}

std::uint32_t commit(void*,
                     void* owner_token,
                     std::uint64_t used_size,
                     const std::uint8_t** out_readable_data,
                     std::uint64_t* out_readable_size) noexcept {
    auto* reservation = static_cast<HeapReservation*>(owner_token);
    if (used_size > reservation->capacity) {
        return FDB_PAYLOAD_E_COMMIT_FAILED;
    }
    *out_readable_data = reservation->writable;
    *out_readable_size = used_size;
    return success_status;
}

std::uint32_t rollback(void*, void* owner_token) noexcept {
    delete static_cast<HeapReservation*>(owner_token);
    return success_status;
}

std::uint32_t retain(void*, void* owner_token) noexcept {
    static_cast<HeapReservation*>(owner_token)
        ->references.fetch_add(UINT64_C(1), std::memory_order_relaxed);
    return success_status;
}

void release(void*, void* owner_token) noexcept {
    auto* reservation = static_cast<HeapReservation*>(owner_token);
    if (reservation->references.fetch_sub(UINT64_C(1),
                                          std::memory_order_acq_rel) ==
        UINT64_C(1)) {
        delete reservation;
    }
}

const Callbacks callbacks{
    nullptr, &reserve, &write, &commit, &rollback, &retain, &release};

}  // namespace

const Callbacks& heap_callbacks() noexcept {
    return callbacks;
}

}  // namespace fastdb::payload::backing
