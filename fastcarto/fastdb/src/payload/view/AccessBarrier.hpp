#pragma once

#include "payload/error/Result.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace fastdb::payload::view {

struct AccessBarrierState final {
    std::mutex mutex;
    std::condition_variable drained;
    std::uint64_t generation{UINT64_C(1)};
    std::uint64_t active_accesses{UINT64_C(0)};
    std::uint64_t waiting_invalidators{UINT64_C(0)};
    bool invalidating{false};
    bool invalidated{false};
};

class AccessPin final {
public:
    static error::Result<AccessPin> acquire(
        AccessBarrierState& barrier,
        std::uint64_t captured_generation);
    static error::Result<AccessPin> acquire_current(
        AccessBarrierState& barrier,
        std::uint64_t& captured_generation);

    AccessPin(const AccessPin&) = delete;
    AccessPin& operator=(const AccessPin&) = delete;
    AccessPin(AccessPin&& other) noexcept;
    AccessPin& operator=(AccessPin&& other) noexcept;
    ~AccessPin();

private:
    static error::Result<AccessPin> acquire_impl(
        AccessBarrierState& barrier,
        std::uint64_t captured_generation,
        bool capture_current,
        std::uint64_t* current_generation);

    explicit AccessPin(AccessBarrierState& barrier) noexcept
        : barrier_(&barrier) {}

    void release() noexcept;

    AccessBarrierState* barrier_{nullptr};
};

}  // namespace fastdb::payload::view
