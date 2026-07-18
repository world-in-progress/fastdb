#pragma once

#include "payload/error/Result.hpp"

#include <cstdint>
#include <utility>

namespace fastdb::payload::backing {

using ReserveCallback = std::uint32_t (*)(
    void* context,
    std::uint32_t reserve_mode,
    std::uint64_t minimum_capacity,
    std::uint32_t alignment,
    void** out_owner_token,
    std::uint8_t** out_writable_data,
    std::uint64_t* out_capacity);
using WriteCallback = std::uint32_t (*)(void* context,
                                        void* owner_token,
                                        std::uint64_t offset,
                                        const std::uint8_t* source,
                                        std::uint64_t source_size);
using CommitCallback = std::uint32_t (*)(
    void* context,
    void* owner_token,
    std::uint64_t used_size,
    const std::uint8_t** out_readable_data,
    std::uint64_t* out_readable_size);
using RollbackCallback = std::uint32_t (*)(void* context,
                                           void* owner_token);
using RetainCallback = std::uint32_t (*)(void* context, void* owner_token);
using ReleaseCallback = void (*)(void* context, void* owner_token);

struct Callbacks final {
    void* context;
    ReserveCallback reserve;
    WriteCallback write;
    CommitCallback commit;
    RollbackCallback rollback;
    RetainCallback retain;
    ReleaseCallback release;
};

enum class CallbackOperation : std::uint8_t {
    reserve_direct,
    reserve_staged,
    write,
    commit,
    rollback,
    retain,
};

std::uint32_t classify_callback_status(CallbackOperation operation,
                                       std::uint32_t status) noexcept;

enum class ReservationState : std::uint8_t {
    empty,
    reserved,
    committed,
    rolled_back,
};

class CommittedBacking final {
public:
    CommittedBacking(Callbacks callbacks,
                     void* owner_token,
                     const std::uint8_t* readable_data,
                     std::uint64_t readable_size,
                     std::uint64_t capacity) noexcept;
    CommittedBacking(const CommittedBacking&) = delete;
    CommittedBacking& operator=(const CommittedBacking&) = delete;
    CommittedBacking(CommittedBacking&& other) noexcept;
    CommittedBacking& operator=(CommittedBacking&& other) noexcept;
    ~CommittedBacking();

    const std::uint8_t* readable_data() const noexcept {
        return readable_data_;
    }
    std::uint64_t readable_size() const noexcept { return readable_size_; }
    std::uint64_t capacity() const noexcept { return capacity_; }

private:
    void release() noexcept;

    Callbacks callbacks_{};
    void* owner_token_{nullptr};
    const std::uint8_t* readable_data_{nullptr};
    std::uint64_t readable_size_{UINT64_C(0)};
    std::uint64_t capacity_{UINT64_C(0)};
    bool active_{false};
};

class RetainedBacking final {
public:
    static error::Result<RetainedBacking> acquire(
        Callbacks callbacks,
        void* owner_token,
        const std::uint8_t* readable_data,
        std::uint64_t readable_size);

    RetainedBacking(const RetainedBacking&) = delete;
    RetainedBacking& operator=(const RetainedBacking&) = delete;
    RetainedBacking(RetainedBacking&&) noexcept = default;
    RetainedBacking& operator=(RetainedBacking&&) noexcept = default;
    ~RetainedBacking() = default;

    const std::uint8_t* readable_data() const noexcept {
        return backing_.readable_data();
    }
    std::uint64_t readable_size() const noexcept {
        return backing_.readable_size();
    }

    CommittedBacking take_committed() && noexcept {
        return std::move(backing_);
    }

private:
    explicit RetainedBacking(CommittedBacking backing) noexcept
        : backing_(std::move(backing)) {}

    CommittedBacking backing_;
};

class BackingReservation final {
public:
    explicit BackingReservation(Callbacks callbacks) noexcept
        : callbacks_(callbacks) {}
    BackingReservation(const BackingReservation&) = delete;
    BackingReservation& operator=(const BackingReservation&) = delete;
    BackingReservation(BackingReservation&&) = delete;
    BackingReservation& operator=(BackingReservation&&) = delete;
    ~BackingReservation();

    std::uint32_t reserve(std::uint32_t mode,
                          std::uint64_t minimum_capacity,
                          std::uint32_t alignment);
    std::uint32_t write(std::uint64_t offset,
                        const std::uint8_t* source,
                        std::uint64_t source_size) const;
    std::uint32_t commit(std::uint64_t used_size);
    std::uint32_t rollback();
    CommittedBacking take_committed() noexcept;

    ReservationState state() const noexcept { return state_; }
    const Callbacks& callbacks() const noexcept { return callbacks_; }
    void* owner_token() const noexcept { return owner_token_; }
    std::uint8_t* writable_data() const noexcept { return writable_data_; }
    std::uint64_t capacity() const noexcept { return capacity_; }

private:
    Callbacks callbacks_;
    ReservationState state_{ReservationState::empty};
    void* owner_token_{nullptr};
    std::uint8_t* writable_data_{nullptr};
    std::uint64_t capacity_{UINT64_C(0)};
    const std::uint8_t* readable_data_{nullptr};
    std::uint64_t readable_size_{UINT64_C(0)};
};

}  // namespace fastdb::payload::backing
