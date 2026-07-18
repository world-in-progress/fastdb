#include "payload/view/AccessBarrier.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"

#include <fastdb_payload.h>

#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace fastdb::payload::view {
namespace {

error::Error barrier_error(std::uint32_t code, const char* reason) {
    return error::Error::from_details(
        code, json::JsonPointer{}.append("view"),
        "Portable payload access barrier rejected the operation",
        json::JsonValue::object({json::JsonValue::Member{
            "reason", json::JsonValue{reason}}}));
}

}  // namespace

error::Result<AccessPin> AccessPin::acquire_impl(
    AccessBarrierState& barrier,
    std::uint64_t captured_generation,
    bool capture_current,
    std::uint64_t* current_generation) {
    std::lock_guard<std::mutex> lock(barrier.mutex);
    if (barrier.invalidating || barrier.invalidated) {
        return error::Result<AccessPin>::failure(
            barrier_error(FDB_PAYLOAD_E_VIEW_INVALIDATED,
                          "view_invalidated"));
    }
    if (!capture_current && captured_generation != barrier.generation) {
        return error::Result<AccessPin>::failure(
            barrier_error(FDB_PAYLOAD_E_STALE_GENERATION,
                          "stale_generation"));
    }
    if (barrier.active_accesses ==
        std::numeric_limits<std::uint64_t>::max()) {
        return error::Result<AccessPin>::failure(
            barrier_error(FDB_PAYLOAD_E_INTERNAL,
                          "active_access_overflow"));
    }
    ++barrier.active_accesses;
    if (current_generation != nullptr) {
        *current_generation = barrier.generation;
    }
    return error::Result<AccessPin>::success(AccessPin{barrier});
}

error::Result<AccessPin> AccessPin::acquire(
    AccessBarrierState& barrier,
    std::uint64_t captured_generation) {
    return acquire_impl(barrier, captured_generation, false, nullptr);
}

error::Result<AccessPin> AccessPin::acquire_current(
    AccessBarrierState& barrier,
    std::uint64_t& captured_generation) {
    return acquire_impl(barrier, UINT64_C(0), true, &captured_generation);
}

AccessPin::AccessPin(AccessPin&& other) noexcept
    : barrier_(std::exchange(other.barrier_, nullptr)) {}

AccessPin& AccessPin::operator=(AccessPin&& other) noexcept {
    if (this != &other) {
        release();
        barrier_ = std::exchange(other.barrier_, nullptr);
    }
    return *this;
}

AccessPin::~AccessPin() { release(); }

void AccessPin::release() noexcept {
    AccessBarrierState* const barrier =
        std::exchange(barrier_, nullptr);
    if (barrier == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(barrier->mutex);
    if (barrier->active_accesses == UINT64_C(0)) {
        return;
    }
    --barrier->active_accesses;
    if (barrier->active_accesses == UINT64_C(0)) {
        barrier->drained.notify_all();
    }
}

}  // namespace fastdb::payload::view
