#include "payload/view/PayloadOwner.hpp"

#include "payload/backing/HeapBacking.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/InputSpan.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/view/View.hpp"

#include <fastdb_payload.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace fastdb::payload::view {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

Error allocation_error() {
    return Error::from_details(
        FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
        "Portable payload owner allocation failed",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"allocation_failed"}}}));
}

Error backing_error(std::uint32_t code, const char* reason) {
    return Error::from_details(
        code, JsonPointer{}.append("backing"),
        "Portable payload owner backing failed",
        JsonValue::object({JsonValue::Member{"reason", JsonValue{reason}}}));
}

class InvalidatorWaitRegistration final {
public:
    explicit InvalidatorWaitRegistration(
        AccessBarrierState& barrier) noexcept
        : barrier_(&barrier) {
        ++barrier_->waiting_invalidators;
        barrier_->drained.notify_all();
    }

    InvalidatorWaitRegistration(const InvalidatorWaitRegistration&) = delete;
    InvalidatorWaitRegistration& operator=(
        const InvalidatorWaitRegistration&) = delete;

    ~InvalidatorWaitRegistration() {
        --barrier_->waiting_invalidators;
        barrier_->drained.notify_all();
    }

private:
    AccessBarrierState* barrier_;
};

Result<backing::CommittedBacking> copy_to_heap(
    const std::uint8_t* bytes,
    std::uint64_t byte_count) {
    backing::BackingReservation reservation(backing::heap_callbacks());
    const std::uint32_t reserve_status =
        reservation.reserve(UINT32_C(1), byte_count, UINT32_C(1));
    const std::uint32_t reserve_classification =
        backing::classify_callback_status(
            backing::CallbackOperation::reserve_direct, reserve_status);
    if (reserve_classification != UINT32_C(0)) {
        return Result<backing::CommittedBacking>::failure(
            reserve_classification == FDB_PAYLOAD_E_ALLOCATION_FAILED
                ? allocation_error()
                : backing_error(FDB_PAYLOAD_E_BACKING_CONTRACT,
                                "heap_reserve_failed"));
    }
    if (reservation.writable_data() == nullptr ||
        reservation.capacity() < byte_count) {
        static_cast<void>(reservation.rollback());
        return Result<backing::CommittedBacking>::failure(
            backing_error(FDB_PAYLOAD_E_BACKING_CONTRACT,
                          "invalid_heap_reservation"));
    }
    if (byte_count != UINT64_C(0)) {
        std::memcpy(reservation.writable_data(), bytes,
                    static_cast<std::size_t>(byte_count));
    }
    const std::uint32_t commit_status = reservation.commit(byte_count);
    const std::uint32_t commit_classification =
        backing::classify_callback_status(
            backing::CallbackOperation::commit, commit_status);
    if (commit_classification != UINT32_C(0)) {
        static_cast<void>(reservation.rollback());
        return Result<backing::CommittedBacking>::failure(
            commit_classification == FDB_PAYLOAD_E_ALLOCATION_FAILED
                ? allocation_error()
                : backing_error(commit_classification,
                                "heap_commit_failed"));
    }
    return Result<backing::CommittedBacking>::success(
        reservation.take_committed());
}

}  // namespace

Result<PayloadOwner> PayloadOwner::publish(
    backing::CommittedBacking backing,
    spec::CompiledSpec spec,
    PayloadIndex index,
    std::optional<build::ExecutionReport> report) try {
    return Result<PayloadOwner>::success(PayloadOwner{
        std::make_shared<PayloadOwnerState>(
            std::move(backing), std::move(spec), std::move(index),
            std::move(report))});
} catch (const std::bad_alloc&) {
    return Result<PayloadOwner>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<PayloadOwner>::failure(allocation_error());
}

Result<PayloadOwner> PayloadOwner::open_copy(
    spec::CompiledSpec spec,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    OpenOptions options) try {
    if (byte_count > options.max_total_bytes || bytes == nullptr ||
        !layout::input_span_is_addressable(byte_count)) {
        auto rejected = open_payload(spec, bytes, byte_count, options);
        if (!rejected.has_value()) {
            return Result<PayloadOwner>::failure(
                std::move(rejected).error());
        }
        return Result<PayloadOwner>::failure(
            backing_error(FDB_PAYLOAD_E_INTERNAL,
                          "copy_preflight_invariant"));
    }
    auto copied = copy_to_heap(bytes, byte_count);
    if (!copied.has_value()) {
        return Result<PayloadOwner>::failure(std::move(copied).error());
    }
    backing::CommittedBacking owned = std::move(copied).value();
    auto opened = open_payload(spec, owned.readable_data(),
                               owned.readable_size(), options);
    if (!opened.has_value()) {
        return Result<PayloadOwner>::failure(std::move(opened).error());
    }
    return publish(std::move(owned), std::move(spec),
                   std::move(opened).value(), std::nullopt);
} catch (const std::bad_alloc&) {
    return Result<PayloadOwner>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<PayloadOwner>::failure(allocation_error());
}

Result<PayloadOwner> PayloadOwner::open_external(
    spec::CompiledSpec spec,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    backing::RetainedBacking retained,
    OpenOptions options) try {
    if (bytes != retained.readable_data() ||
        byte_count != retained.readable_size()) {
        return Result<PayloadOwner>::failure(
            backing_error(FDB_PAYLOAD_E_INVALID_ARGUMENT,
                          "retained_span_mismatch"));
    }
    auto opened = open_payload(spec, bytes, byte_count, options);
    if (!opened.has_value()) {
        return Result<PayloadOwner>::failure(std::move(opened).error());
    }
    return publish(std::move(retained).take_committed(), std::move(spec),
                   std::move(opened).value(), std::nullopt);
} catch (const std::bad_alloc&) {
    return Result<PayloadOwner>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<PayloadOwner>::failure(allocation_error());
}

Result<Access> PayloadOwner::acquire() const try {
    std::uint64_t generation = UINT64_C(0);
    auto pin = AccessPin::acquire_current(state_->barrier, generation);
    if (!pin.has_value()) {
        return Result<Access>::failure(std::move(pin).error());
    }
    if (!state_->backing.has_value()) {
        return Result<Access>::failure(backing_error(
            FDB_PAYLOAD_E_INTERNAL, "missing_committed_backing"));
    }
    auto access = std::make_unique<Access::State>();
    access->owner = state_;
    access->pin.emplace(std::move(pin).value());
    access->kind = AccessKind::payload;
    access->bytes = state_->backing->readable_data();
    access->byte_count = state_->backing->readable_size();
    return Result<Access>::success(Access{std::move(access)});
} catch (const std::bad_alloc&) {
    return Result<Access>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<Access>::failure(allocation_error());
}

Result<View> PayloadOwner::entry_view(std::uint32_t entry_index) const try {
    std::uint64_t generation = UINT64_C(0);
    auto pin = AccessPin::acquire_current(state_->barrier, generation);
    if (!pin.has_value()) {
        return Result<View>::failure(std::move(pin).error());
    }
    if (!state_->backing.has_value()) {
        return Result<View>::failure(backing_error(
            FDB_PAYLOAD_E_INTERNAL, "missing_committed_backing"));
    }
    auto sequence = state_->index.entry_sequence(
        state_->backing->readable_data(), state_->backing->readable_size(),
        entry_index);
    if (!sequence.has_value()) {
        return Result<View>::failure(std::move(sequence).error());
    }
    auto view_state = std::make_shared<View::State>();
    view_state->is_backed = true;
    view_state->is_sequence = true;
    view_state->owner = state_;
    view_state->generation = generation;
    view_state->sequence = sequence.value();
    view_state->diagnostic_path =
        JsonPointer{}.append("entries").append(entry_index);
    return Result<View>::success(View{std::move(view_state)});
} catch (const std::bad_alloc&) {
    return Result<View>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<View>::failure(allocation_error());
}

Result<void> PayloadOwner::require_spec_sha256(
    const std::array<std::uint8_t, 32>& expected) const {
    return spec::require_spec_sha256(
        state_->spec.digest(), expected,
        JsonPointer{}.append("payload").append("spec_sha256"));
}

Result<void> PayloadOwner::invalidate() const {
    AccessBarrierState& barrier = state_->barrier;
    std::optional<backing::CommittedBacking> released;
    std::unique_lock<std::mutex> lock(barrier.mutex);
    if (barrier.invalidating) {
        if (barrier.waiting_invalidators ==
            std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::failure(backing_error(
                FDB_PAYLOAD_E_INTERNAL,
                "waiting_invalidator_overflow"));
        }
        InvalidatorWaitRegistration registration{barrier};
        barrier.drained.wait(lock, [&barrier] {
            return !barrier.invalidating;
        });
    }
    if (barrier.invalidated) {
        return Result<void>::success();
    }
    barrier.invalidating = true;
    barrier.drained.notify_all();
    barrier.drained.wait(lock, [&barrier] {
        return barrier.active_accesses == UINT64_C(0);
    });
    if (state_->backing.has_value()) {
        released.emplace(std::move(*state_->backing));
        state_->backing.reset();
    }
    if (barrier.generation != std::numeric_limits<std::uint64_t>::max()) {
        ++barrier.generation;
    }
    barrier.invalidated = true;

    lock.unlock();
    released.reset();
    lock.lock();

    barrier.invalidating = false;
    barrier.drained.notify_all();
    return Result<void>::success();
}

}  // namespace fastdb::payload::view
