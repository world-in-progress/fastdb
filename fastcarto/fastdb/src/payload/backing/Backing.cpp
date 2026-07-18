#include "payload/backing/Backing.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"

#include <fastdb_payload.h>

#include <utility>

namespace fastdb::payload::backing {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

Error missing_retain_or_release_error() {
    return Error::from_details(
        FDB_PAYLOAD_E_BACKING_CONTRACT, JsonPointer{}.append("backing"),
        "Portable payload retained backing callbacks are incomplete",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"missing_retain_or_release"}}}));
}

Error retain_callback_error(std::uint32_t code, std::uint32_t status) {
    return Error::from_details(
        code, JsonPointer{}.append("backing"),
        "Portable payload backing retain failed",
        JsonValue::object({
            JsonValue::Member{"callback", JsonValue{"retain"}},
            JsonValue::Member{"callback_status",
                              JsonValue{static_cast<double>(status)}},
        }));
}

}  // namespace

std::uint32_t classify_callback_status(CallbackOperation operation,
                                       std::uint32_t status) noexcept {
    if (status == UINT32_C(0)) {
        return UINT32_C(0);
    }
    if (status == FDB_PAYLOAD_E_ALLOCATION_FAILED) {
        return operation == CallbackOperation::rollback
                   ? FDB_PAYLOAD_E_ROLLBACK_FAILED
                   : FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }
    if (operation == CallbackOperation::reserve_direct &&
        status == FDB_PAYLOAD_E_DIRECT_UNAVAILABLE) {
        return FDB_PAYLOAD_E_DIRECT_UNAVAILABLE;
    }
    if (operation == CallbackOperation::commit &&
        status == FDB_PAYLOAD_E_COMMIT_FAILED) {
        return FDB_PAYLOAD_E_COMMIT_FAILED;
    }
    if (operation == CallbackOperation::rollback &&
        status == FDB_PAYLOAD_E_ROLLBACK_FAILED) {
        return FDB_PAYLOAD_E_ROLLBACK_FAILED;
    }
    return FDB_PAYLOAD_E_BACKING_CONTRACT;
}

CommittedBacking::CommittedBacking(Callbacks callbacks,
                                   void* owner_token,
                                   const std::uint8_t* readable_data,
                                   std::uint64_t readable_size,
                                   std::uint64_t capacity) noexcept
    : callbacks_(callbacks),
      owner_token_(owner_token),
      readable_data_(readable_data),
      readable_size_(readable_size),
      capacity_(capacity),
      active_(true) {}

CommittedBacking::CommittedBacking(CommittedBacking&& other) noexcept
    : callbacks_(other.callbacks_),
      owner_token_(other.owner_token_),
      readable_data_(other.readable_data_),
      readable_size_(other.readable_size_),
      capacity_(other.capacity_),
      active_(other.active_) {
    other.active_ = false;
}

CommittedBacking& CommittedBacking::operator=(
    CommittedBacking&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    callbacks_ = other.callbacks_;
    owner_token_ = other.owner_token_;
    readable_data_ = other.readable_data_;
    readable_size_ = other.readable_size_;
    capacity_ = other.capacity_;
    active_ = other.active_;
    other.active_ = false;
    return *this;
}

CommittedBacking::~CommittedBacking() {
    release();
}

void CommittedBacking::release() noexcept {
    if (!active_) {
        return;
    }
    active_ = false;
    callbacks_.release(callbacks_.context, owner_token_);
}

Result<RetainedBacking> RetainedBacking::acquire(
    Callbacks callbacks,
    void* owner_token,
    const std::uint8_t* readable_data,
    std::uint64_t readable_size) {
    if (callbacks.retain == nullptr || callbacks.release == nullptr) {
        return Result<RetainedBacking>::failure(
            missing_retain_or_release_error());
    }
    const std::uint32_t status =
        callbacks.retain(callbacks.context, owner_token);
    const std::uint32_t classified =
        classify_callback_status(CallbackOperation::retain, status);
    if (classified != UINT32_C(0)) {
        return Result<RetainedBacking>::failure(
            retain_callback_error(classified, status));
    }
    return Result<RetainedBacking>::success(RetainedBacking{
        CommittedBacking{callbacks, owner_token, readable_data, readable_size,
                         readable_size}});
}

BackingReservation::~BackingReservation() {
    if (state_ == ReservationState::reserved) {
        static_cast<void>(rollback());
    } else if (state_ == ReservationState::committed) {
        callbacks_.release(callbacks_.context, owner_token_);
        state_ = ReservationState::empty;
    }
}

std::uint32_t BackingReservation::reserve(
    std::uint32_t mode,
    std::uint64_t minimum_capacity,
    std::uint32_t alignment) {
    void* owner_token = nullptr;
    std::uint8_t* writable_data = nullptr;
    std::uint64_t capacity = UINT64_C(0);
    const std::uint32_t status = callbacks_.reserve(
        callbacks_.context, mode, minimum_capacity, alignment, &owner_token,
        &writable_data, &capacity);
    if (status != UINT32_C(0)) {
        return status;
    }
    owner_token_ = owner_token;
    writable_data_ = writable_data;
    capacity_ = capacity;
    state_ = ReservationState::reserved;
    return status;
}

std::uint32_t BackingReservation::write(
    std::uint64_t offset,
    const std::uint8_t* source,
    std::uint64_t source_size) const {
    return callbacks_.write(callbacks_.context, owner_token_, offset, source,
                            source_size);
}

std::uint32_t BackingReservation::commit(std::uint64_t used_size) {
    const std::uint8_t* readable_data = nullptr;
    std::uint64_t readable_size = UINT64_C(0);
    const std::uint32_t status = callbacks_.commit(
        callbacks_.context, owner_token_, used_size, &readable_data,
        &readable_size);
    if (status != UINT32_C(0)) {
        return status;
    }
    readable_data_ = readable_data;
    readable_size_ = readable_size;
    state_ = ReservationState::committed;
    return status;
}

std::uint32_t BackingReservation::rollback() {
    state_ = ReservationState::rolled_back;
    return callbacks_.rollback(callbacks_.context, owner_token_);
}

CommittedBacking BackingReservation::take_committed() noexcept {
    state_ = ReservationState::empty;
    return CommittedBacking(callbacks_, owner_token_, readable_data_,
                            readable_size_, capacity_);
}

}  // namespace fastdb::payload::backing
