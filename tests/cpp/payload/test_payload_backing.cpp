#include "TestSupport.hpp"

#include "payload/build/PayloadBuilder.hpp"
#include "payload/spec/CompiledSpec.hpp"

#if __has_include("payload/backing/Backing.hpp")
#include "payload/backing/Backing.hpp"
#include "payload/backing/HeapBacking.hpp"
#define FASTDB_TASK6_HAS_BACKING_CALLBACKS 1
#else
#define FASTDB_TASK6_HAS_BACKING_CALLBACKS 0
#endif

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocation_failure {

std::atomic<std::int64_t> fail_after{INT64_C(-1)};
std::atomic<std::size_t> current_live_bytes{0U};

struct AllocationHeader final {
    void* raw;
    std::size_t requested;
};

void* allocate(std::size_t size,
               std::size_t alignment = alignof(std::max_align_t)) {
    const std::int64_t remaining =
        fail_after.load(std::memory_order_relaxed);
    if (remaining >= INT64_C(0) &&
        fail_after.fetch_sub(INT64_C(1), std::memory_order_relaxed) ==
            INT64_C(0)) {
        throw std::bad_alloc();
    }
    alignment = std::max(alignment, alignof(AllocationHeader));
    const std::size_t payload_size = size == 0U ? 1U : size;
    if (payload_size >
        std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) -
            (alignment - 1U)) {
        throw std::bad_alloc();
    }
    void* const raw = std::malloc(payload_size + sizeof(AllocationHeader) +
                                  alignment - 1U);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    const std::uintptr_t begin =
        reinterpret_cast<std::uintptr_t>(raw) + sizeof(AllocationHeader);
    const std::uintptr_t aligned =
        (begin + alignment - 1U) & ~(alignment - 1U);
    auto* const header =
        reinterpret_cast<AllocationHeader*>(aligned) - 1;
    header->raw = raw;
    header->requested = size;
    current_live_bytes.fetch_add(size, std::memory_order_relaxed);
    return reinterpret_cast<void*>(aligned);
}

void deallocate(void* value) noexcept {
    if (value == nullptr) {
        return;
    }
    auto* const header =
        reinterpret_cast<AllocationHeader*>(value) - 1;
    current_live_bytes.fetch_sub(header->requested,
                                 std::memory_order_relaxed);
    std::free(header->raw);
}

}  // namespace allocation_failure

void* operator new(std::size_t size) {
    return allocation_failure::allocate(size);
}
void* operator new[](std::size_t size) {
    return allocation_failure::allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(
        size, static_cast<std::size_t>(alignment));
}
void operator delete(void* value) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::size_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::size_t,
                     std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::size_t,
                       std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}

#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
namespace fastdb::payload::view {

struct PayloadOwnerTestAccess final {
    static const backing::CommittedBacking& backing(
        const PayloadOwner& owner) noexcept {
        return owner.state_->backing;
    }
};

}  // namespace fastdb::payload::view
#endif

namespace fastdb::payload::backing {
struct Callbacks;
}

namespace {

using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::build::BuildPlan;
using fastdb::payload::error::Result;
using fastdb::payload::spec::CompiledSpec;

#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
const fastdb::payload::backing::CommittedBacking& owner_backing(
    const fastdb::payload::view::PayloadOwner& owner) noexcept {
    return fastdb::payload::view::PayloadOwnerTestAccess::backing(owner);
}

const std::uint8_t* owner_data(
    const fastdb::payload::view::PayloadOwner& owner) noexcept {
    return owner_backing(owner).readable_data();
}

std::uint64_t owner_size(
    const fastdb::payload::view::PayloadOwner& owner) noexcept {
    return owner_backing(owner).readable_size();
}

std::uint64_t owner_capacity(
    const fastdb::payload::view::PayloadOwner& owner) noexcept {
    return owner_backing(owner).capacity();
}

const fastdb::payload::build::ExecutionReport& owner_report(
    const fastdb::payload::view::PayloadOwner& owner) {
    return owner.execution_report().value();
}
#endif

constexpr std::uint32_t allow_staging = UINT32_C(1);
constexpr std::uint32_t require_direct = UINT32_C(2);
constexpr std::uint32_t direct_mode = UINT32_C(1);
constexpr std::uint32_t staged_mode = UINT32_C(2);
constexpr std::uint32_t success_status = UINT32_C(0);
constexpr std::uint32_t unknown_status = UINT32_C(0x6a17f00d);
constexpr std::uint32_t unspecified_error = UINT32_MAX;

constexpr std::array<std::uint32_t, 4> deterministic_unknown_statuses{{
    UINT32_C(0x0badc0de),
    UINT32_C(0x13579bdf),
    UINT32_C(0x6a17f00d),
    UINT32_C(0xfdb0ffff),
}};

enum class CallbackKind : std::uint8_t {
    reserve,
    write,
    commit,
    rollback,
    retain,
    release,
};

enum class BackingShape : std::uint8_t {
    core_heap,
    stable_span,
    range_write_only,
    decline_direct_then_staged,
    short_capacity,
    misaligned_span,
    no_write_path,
    null_commit_span,
    short_commit_span,
    oversized_commit_span,
    relocated_commit_span,
};

enum class MatrixCategory : std::uint8_t {
    plan,
    success,
    policy,
    contract,
    cleanup,
    allocation,
    reentry,
    documentation,
};

struct CallbackReceipt final {
    CallbackKind callback;
    std::uint32_t mode;
    std::uint64_t minimum_capacity;
    std::uint32_t alignment;
    void* token;
    std::uint64_t offset;
    std::uint64_t source_size;
    std::uint64_t used_size;
    void* owner_output;
    std::uint8_t* writable_output;
    std::uint64_t capacity_output;
    const std::uint8_t* readable_output;
    std::uint64_t readable_size_output;
    std::uint32_t status;
};

struct FailureInjection final {
    CallbackKind callback;
    std::uint64_t invocation;
    std::uint32_t status;
    bool dirty_outputs;
};

struct FakeCallbacks final {
    void* context;
    std::uint32_t (*reserve)(void*, std::uint32_t, std::uint64_t,
                             std::uint32_t, void**, std::uint8_t**,
                             std::uint64_t*);
    std::uint32_t (*write)(void*, void*, std::uint64_t,
                           const std::uint8_t*, std::uint64_t);
    std::uint32_t (*commit)(void*, void*, std::uint64_t,
                            const std::uint8_t**, std::uint64_t*);
    std::uint32_t (*rollback)(void*, void*);
    std::uint32_t (*retain)(void*, void*);
    void (*release)(void*, void*);
};

class FakeBacking final {
public:
    explicit FakeBacking(BackingShape shape,
                         std::uint64_t capacity = UINT64_C(128) * 1024U)
        : shape_(shape),
          storage_(static_cast<std::size_t>(capacity), 0U),
          relocated_storage_(static_cast<std::size_t>(capacity), 0U) {}

    FakeCallbacks callbacks(bool with_write = true) noexcept {
        return FakeCallbacks{this,
                             &reserve_thunk,
                             with_write ? &write_thunk : nullptr,
                             &commit_thunk,
                             &rollback_thunk,
                             &retain_thunk,
                             &release_thunk};
    }

#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
    fastdb::payload::backing::Callbacks production_callbacks(
        bool with_write = true) noexcept {
        return fastdb::payload::backing::Callbacks{
            this,
            &reserve_thunk,
            with_write ? &write_thunk : nullptr,
            &commit_thunk,
            &rollback_thunk,
            &retain_thunk,
            &release_thunk};
    }
#endif

    void inject(FailureInjection injection) {
        injections_.push_back(injection);
    }

    void use_null_token(bool value = true) noexcept {
        null_token_ = value;
    }

    void reserve_receipts(std::size_t count) {
        receipts_.reserve(count);
    }

#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
    void enable_nested_execution(
        const fastdb::payload::build::BuildPlan& plan,
        FakeBacking& backing) noexcept {
        nested_plan_ = &plan;
        nested_backing_ = &backing;
    }

    bool nested_succeeded() const noexcept { return nested_succeeded_; }
#endif

    const std::vector<CallbackReceipt>& receipts() const noexcept {
        return receipts_;
    }

    bool storage_equals(const std::uint8_t* data,
                        std::uint64_t size) const noexcept {
        return size == storage_.size() &&
               (size == UINT64_C(0) ||
                std::memcmp(storage_.data(), data,
                            static_cast<std::size_t>(size)) == 0);
    }

private:
    static constexpr std::size_t callback_index(CallbackKind callback) {
        return static_cast<std::size_t>(callback);
    }

    std::uint32_t status_for(CallbackKind callback, bool& dirty_outputs) {
        const std::size_t index = callback_index(callback);
        const std::uint64_t invocation = ++invocations_[index];
        for (const FailureInjection& injection : injections_) {
            if (injection.callback == callback &&
                injection.invocation == invocation) {
                dirty_outputs = injection.dirty_outputs;
                return injection.status;
            }
        }
        dirty_outputs = false;
        return success_status;
    }

    void* token() noexcept {
        return null_token_ ? nullptr : static_cast<void*>(&token_storage_);
    }

    std::uint8_t* writable_base() noexcept {
        if (shape_ == BackingShape::range_write_only ||
            shape_ == BackingShape::no_write_path) {
            return nullptr;
        }
        std::uint8_t* base = storage_.data();
        if (shape_ == BackingShape::misaligned_span && !storage_.empty()) {
            return base + 1;
        }
        return base;
    }

    std::uint64_t advertised_capacity() const noexcept {
        if (shape_ == BackingShape::short_capacity) {
            return storage_.empty()
                       ? UINT64_C(0)
                       : static_cast<std::uint64_t>(storage_.size() - 1U);
        }
        return static_cast<std::uint64_t>(storage_.size());
    }

    std::uint32_t reserve(std::uint32_t mode,
                          std::uint64_t minimum_capacity,
                          std::uint32_t alignment,
                          void** out_owner,
                          std::uint8_t** out_writable,
                          std::uint64_t* out_capacity) {
#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
        if (nested_plan_ != nullptr && !nested_started_) {
            nested_started_ = true;
            auto nested_callbacks =
                nested_backing_->production_callbacks(false);
            auto nested = nested_plan_->execute(require_direct,
                                                &nested_callbacks);
            nested_succeeded_ = nested.has_value();
        }
#endif
        bool dirty_outputs = false;
        std::uint32_t status = status_for(CallbackKind::reserve, dirty_outputs);
        if (status == success_status &&
            shape_ == BackingShape::decline_direct_then_staged &&
            mode == direct_mode) {
            status = FDB_PAYLOAD_E_DIRECT_UNAVAILABLE;
        }
        if (status != success_status && dirty_outputs) {
            *out_owner = reinterpret_cast<void*>(UINTPTR_MAX);
            *out_writable = reinterpret_cast<std::uint8_t*>(UINTPTR_MAX);
            *out_capacity = UINT64_MAX;
        } else if (status == success_status) {
            *out_owner = token();
            *out_writable = writable_base();
            *out_capacity = advertised_capacity();
        }
        receipts_.push_back(CallbackReceipt{
            CallbackKind::reserve,
            mode,
            minimum_capacity,
            alignment,
            nullptr,
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            *out_owner,
            *out_writable,
            *out_capacity,
            nullptr,
            UINT64_C(0),
            status,
        });
        return status;
    }

    std::uint32_t write(void* owner,
                        std::uint64_t offset,
                        const std::uint8_t* source,
                        std::uint64_t source_size) {
        bool dirty_outputs = false;
        const std::uint32_t status =
            status_for(CallbackKind::write, dirty_outputs);
        if (status == success_status && source_size != UINT64_C(0) &&
            source != nullptr && offset <= storage_.size() &&
            source_size <= storage_.size() - offset) {
            std::memcpy(storage_.data() + static_cast<std::size_t>(offset),
                        source, static_cast<std::size_t>(source_size));
        }
        receipts_.push_back(CallbackReceipt{
            CallbackKind::write,
            UINT32_C(0),
            UINT64_C(0),
            UINT32_C(0),
            owner,
            offset,
            source_size,
            UINT64_C(0),
            nullptr,
            nullptr,
            UINT64_C(0),
            nullptr,
            UINT64_C(0),
            status,
        });
        return status;
    }

    std::uint32_t commit(void* owner,
                         std::uint64_t used_size,
                         const std::uint8_t** out_readable,
                         std::uint64_t* out_readable_size) {
        bool dirty_outputs = false;
        const std::uint32_t status =
            status_for(CallbackKind::commit, dirty_outputs);
        if (status != success_status && dirty_outputs) {
            *out_readable = reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX);
            *out_readable_size = UINT64_MAX;
        } else if (status == success_status) {
            *out_readable = storage_.data();
            *out_readable_size = used_size;
            if (shape_ == BackingShape::null_commit_span) {
                *out_readable = nullptr;
            } else if (shape_ == BackingShape::short_commit_span &&
                       used_size != UINT64_C(0)) {
                *out_readable_size = used_size - UINT64_C(1);
            } else if (shape_ == BackingShape::oversized_commit_span) {
                *out_readable_size = used_size + UINT64_C(1);
            } else if (shape_ == BackingShape::relocated_commit_span) {
                relocated_storage_ = storage_;
                *out_readable = relocated_storage_.data();
            }
        }
        receipts_.push_back(CallbackReceipt{
            CallbackKind::commit,
            UINT32_C(0),
            UINT64_C(0),
            UINT32_C(0),
            owner,
            UINT64_C(0),
            UINT64_C(0),
            used_size,
            nullptr,
            nullptr,
            UINT64_C(0),
            *out_readable,
            *out_readable_size,
            status,
        });
        return status;
    }

    std::uint32_t rollback(void* owner) {
        bool dirty_outputs = false;
        const std::uint32_t status =
            status_for(CallbackKind::rollback, dirty_outputs);
        receipts_.push_back(CallbackReceipt{
            CallbackKind::rollback, UINT32_C(0), UINT64_C(0), UINT32_C(0),
            owner, UINT64_C(0), UINT64_C(0), UINT64_C(0), nullptr, nullptr,
            UINT64_C(0), nullptr, UINT64_C(0), status});
        return status;
    }

    std::uint32_t retain(void* owner) {
        bool dirty_outputs = false;
        const std::uint32_t status =
            status_for(CallbackKind::retain, dirty_outputs);
        receipts_.push_back(CallbackReceipt{
            CallbackKind::retain, UINT32_C(0), UINT64_C(0), UINT32_C(0),
            owner, UINT64_C(0), UINT64_C(0), UINT64_C(0), nullptr, nullptr,
            UINT64_C(0), nullptr, UINT64_C(0), status});
        return status;
    }

    void release(void* owner) {
        bool ignored = false;
        static_cast<void>(status_for(CallbackKind::release, ignored));
        receipts_.push_back(CallbackReceipt{
            CallbackKind::release, UINT32_C(0), UINT64_C(0), UINT32_C(0),
            owner, UINT64_C(0), UINT64_C(0), UINT64_C(0), nullptr, nullptr,
            UINT64_C(0), nullptr, UINT64_C(0), success_status});
    }

    static std::uint32_t reserve_thunk(void* context,
                                       std::uint32_t mode,
                                       std::uint64_t minimum_capacity,
                                       std::uint32_t alignment,
                                       void** out_owner,
                                       std::uint8_t** out_writable,
                                       std::uint64_t* out_capacity) {
        return static_cast<FakeBacking*>(context)->reserve(
            mode, minimum_capacity, alignment, out_owner, out_writable,
            out_capacity);
    }

    static std::uint32_t write_thunk(void* context,
                                     void* owner,
                                     std::uint64_t offset,
                                     const std::uint8_t* source,
                                     std::uint64_t source_size) {
        return static_cast<FakeBacking*>(context)->write(
            owner, offset, source, source_size);
    }

    static std::uint32_t commit_thunk(void* context,
                                      void* owner,
                                      std::uint64_t used_size,
                                      const std::uint8_t** out_readable,
                                      std::uint64_t* out_readable_size) {
        return static_cast<FakeBacking*>(context)->commit(
            owner, used_size, out_readable, out_readable_size);
    }

    static std::uint32_t rollback_thunk(void* context, void* owner) {
        return static_cast<FakeBacking*>(context)->rollback(owner);
    }

    static std::uint32_t retain_thunk(void* context, void* owner) {
        return static_cast<FakeBacking*>(context)->retain(owner);
    }

    static void release_thunk(void* context, void* owner) {
        static_cast<FakeBacking*>(context)->release(owner);
    }

    BackingShape shape_;
    std::vector<std::uint8_t> storage_;
    std::vector<std::uint8_t> relocated_storage_;
    std::vector<FailureInjection> injections_;
    std::vector<CallbackReceipt> receipts_;
    std::array<std::uint64_t, 6> invocations_{};
    std::uint8_t token_storage_{UINT8_C(0)};
    bool null_token_{false};
#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
    const fastdb::payload::build::BuildPlan* nested_plan_{nullptr};
    FakeBacking* nested_backing_{nullptr};
    bool nested_started_{false};
    bool nested_succeeded_{false};
#endif
};

struct StateTransitionCase final {
    std::string_view event;
    std::string_view prior_state;
    std::string_view next_state;
    std::uint32_t rollback_count;
    std::uint32_t release_count;
};

constexpr std::array<StateTransitionCase, 11> state_machine{{
    {"construction", "none", "empty", UINT32_C(0), UINT32_C(0)},
    {"reserve failure", "empty", "empty", UINT32_C(0), UINT32_C(0)},
    {"reserve success", "empty", "reserved", UINT32_C(0), UINT32_C(0)},
    {"write success", "reserved", "reserved", UINT32_C(0), UINT32_C(0)},
    {"write/pre-commit failure", "reserved", "rolled_back", UINT32_C(1),
     UINT32_C(0)},
    {"commit failure", "reserved", "rolled_back", UINT32_C(1), UINT32_C(0)},
    {"rollback return", "reserved", "rolled_back", UINT32_C(1), UINT32_C(0)},
    {"commit success", "reserved", "committed", UINT32_C(0), UINT32_C(0)},
    {"post-commit invalid committed-span validation -> BACKING_CONTRACT",
     "committed", "dead", UINT32_C(0), UINT32_C(1)},
    {"PayloadOwner move", "committed", "transferred", UINT32_C(0),
     UINT32_C(0)},
    {"PayloadOwner destruction", "committed", "dead", UINT32_C(0),
     UINT32_C(1)},
}};

struct FrozenMatrixCase final {
    std::uint32_t requirement;
    MatrixCategory category;
    std::string_view name;
    std::uint32_t policy;
    BackingShape backing;
    CallbackKind failure_callback;
    std::uint64_t failure_invocation;
    std::uint32_t callback_status;
    std::uint32_t expected_error;
    std::uint32_t expected_rollbacks;
    std::uint32_t expected_releases;
};

constexpr std::array<FrozenMatrixCase, 37> frozen_matrix{{
    {UINT32_C(1), MatrixCategory::plan, "exact plan facts and source mutation isolation",
     allow_staging, BackingShape::core_heap, CallbackKind::reserve, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(2), MatrixCategory::success, "repeat execution is byte/report stable",
     allow_staging, BackingShape::core_heap, CallbackKind::reserve, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(2)},
    {UINT32_C(3), MatrixCategory::success, "Core heap direct",
     allow_staging, BackingShape::core_heap, CallbackKind::reserve, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(3), MatrixCategory::success, "stable span direct",
     allow_staging, BackingShape::stable_span, CallbackKind::reserve, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(3), MatrixCategory::success, "range write direct",
     allow_staging, BackingShape::range_write_only, CallbackKind::reserve,
     UINT64_C(0), success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(3), MatrixCategory::success, "declined direct staged byte parity",
     allow_staging, BackingShape::decline_direct_then_staged,
     CallbackKind::reserve, UINT64_C(0), success_status, success_status,
     UINT32_C(0), UINT32_C(1)},
    {UINT32_C(3), MatrixCategory::success, "relocated readable base accepted",
     require_direct, BackingShape::relocated_commit_span,
     CallbackKind::commit, UINT64_C(0), success_status, success_status,
     UINT32_C(0), UINT32_C(1)},
    {UINT32_C(4), MatrixCategory::contract, "range writes cover >64KiB pool once",
     require_direct, BackingShape::range_write_only, CallbackKind::write,
     UINT64_C(0), success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(5), MatrixCategory::success, "null owner token through commit/release",
     require_direct, BackingShape::stable_span, CallbackKind::reserve, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(6), MatrixCategory::success, "32-thread distinct-context execution",
     require_direct, BackingShape::stable_span, CallbackKind::reserve, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(32)},
    {UINT32_C(7), MatrixCategory::reentry, "distinct-context nested execution",
     require_direct, BackingShape::stable_span, CallbackKind::reserve, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(2)},
    {UINT32_C(8), MatrixCategory::policy, "invalid policy fails before reserve",
     UINT32_C(0), BackingShape::stable_span, CallbackKind::reserve, UINT64_C(0),
     success_status, unspecified_error, UINT32_C(0), UINT32_C(0)},
    {UINT32_C(9), MatrixCategory::policy, "declined direct is exact direct unavailable",
     require_direct, BackingShape::decline_direct_then_staged,
     CallbackKind::reserve, UINT64_C(1), FDB_PAYLOAD_E_DIRECT_UNAVAILABLE,
     FDB_PAYLOAD_E_DIRECT_UNAVAILABLE, UINT32_C(0), UINT32_C(0)},
    {UINT32_C(10), MatrixCategory::contract, "short capacity rolls back",
     require_direct, BackingShape::short_capacity, CallbackKind::reserve,
     UINT64_C(0), success_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(1),
     UINT32_C(0)},
    {UINT32_C(10), MatrixCategory::contract, "misaligned span rolls back",
     require_direct, BackingShape::misaligned_span, CallbackKind::reserve,
     UINT64_C(0), success_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(1),
     UINT32_C(0)},
    {UINT32_C(10), MatrixCategory::contract, "no writable span or write rolls back",
     require_direct, BackingShape::no_write_path, CallbackKind::reserve,
     UINT64_C(0), success_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(1),
     UINT32_C(0)},
    {UINT32_C(11), MatrixCategory::contract, "write receipts forbid partial coverage",
     require_direct, BackingShape::range_write_only, CallbackKind::write,
     UINT64_C(0), success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(11), MatrixCategory::contract, "write receipts forbid overlap/descent",
     require_direct, BackingShape::range_write_only, CallbackKind::write,
     UINT64_C(0), success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(12), MatrixCategory::contract, "null commit span releases",
     require_direct, BackingShape::null_commit_span, CallbackKind::commit,
     UINT64_C(0), success_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(0),
     UINT32_C(1)},
    {UINT32_C(12), MatrixCategory::contract, "short commit span releases",
     require_direct, BackingShape::short_commit_span, CallbackKind::commit,
     UINT64_C(0), success_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(0),
     UINT32_C(1)},
    {UINT32_C(12), MatrixCategory::contract, "oversized commit span releases",
     require_direct, BackingShape::oversized_commit_span, CallbackKind::commit,
     UINT64_C(0), success_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(0),
     UINT32_C(1)},
    {UINT32_C(13), MatrixCategory::contract, "unknown reserve status is contract error",
     require_direct, BackingShape::stable_span, CallbackKind::reserve, UINT64_C(1),
     unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(0), UINT32_C(0)},
    {UINT32_C(13), MatrixCategory::contract, "unknown write status is contract error",
     require_direct, BackingShape::range_write_only, CallbackKind::write,
     UINT64_C(1), unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(1),
     UINT32_C(0)},
    {UINT32_C(13), MatrixCategory::contract, "unknown commit status is contract error",
     require_direct, BackingShape::stable_span, CallbackKind::commit, UINT64_C(1),
     unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(1), UINT32_C(0)},
    {UINT32_C(13), MatrixCategory::contract, "unknown rollback status preserves original",
     require_direct, BackingShape::range_write_only, CallbackKind::rollback,
     UINT64_C(1), unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(1),
     UINT32_C(0)},
    {UINT32_C(13), MatrixCategory::contract, "staged direct-unavailable is contract error",
     allow_staging, BackingShape::decline_direct_then_staged,
     CallbackKind::reserve, UINT64_C(2), FDB_PAYLOAD_E_DIRECT_UNAVAILABLE,
     FDB_PAYLOAD_E_BACKING_CONTRACT, UINT32_C(0), UINT32_C(0)},
    {UINT32_C(13), MatrixCategory::contract, "unknown retain status is contract error",
     require_direct, BackingShape::stable_span, CallbackKind::retain,
     UINT64_C(1), unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT,
     UINT32_C(0), UINT32_C(0)},
    {UINT32_C(14), MatrixCategory::cleanup, "reserve failure ignores dirty outputs",
     require_direct, BackingShape::stable_span, CallbackKind::reserve, UINT64_C(1),
     FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED,
     UINT32_C(0), UINT32_C(0)},
    {UINT32_C(15), MatrixCategory::cleanup, "first write failure rolls back once",
     require_direct, BackingShape::range_write_only, CallbackKind::write,
     UINT64_C(1), FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ALLOCATION_FAILED, UINT32_C(1), UINT32_C(0)},
    {UINT32_C(15), MatrixCategory::cleanup, "middle write failure rolls back once",
     require_direct, BackingShape::range_write_only, CallbackKind::write,
     UINT64_C(2), FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ALLOCATION_FAILED, UINT32_C(1), UINT32_C(0)},
    {UINT32_C(15), MatrixCategory::cleanup, "final write failure rolls back once",
     require_direct, BackingShape::range_write_only, CallbackKind::write,
     UINT64_MAX, FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ALLOCATION_FAILED, UINT32_C(1), UINT32_C(0)},
    {UINT32_C(16), MatrixCategory::cleanup, "commit failure ignores dirty outputs",
     require_direct, BackingShape::stable_span, CallbackKind::commit, UINT64_C(1),
     FDB_PAYLOAD_E_COMMIT_FAILED, FDB_PAYLOAD_E_COMMIT_FAILED, UINT32_C(1),
     UINT32_C(0)},
    {UINT32_C(17), MatrixCategory::cleanup, "rollback allocation failure preserves original",
     require_direct, BackingShape::range_write_only, CallbackKind::rollback,
     UINT64_C(1), FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ROLLBACK_FAILED, UINT32_C(1), UINT32_C(0)},
    {UINT32_C(17), MatrixCategory::cleanup, "rollback failed preserves original",
     require_direct, BackingShape::range_write_only, CallbackKind::rollback,
     UINT64_C(1), FDB_PAYLOAD_E_ROLLBACK_FAILED, FDB_PAYLOAD_E_ROLLBACK_FAILED,
     UINT32_C(1), UINT32_C(0)},
    {UINT32_C(18), MatrixCategory::contract,
     "post-commit invalid committed span returns BACKING_CONTRACT",
     require_direct, BackingShape::short_commit_span, CallbackKind::commit,
     UINT64_C(0), success_status, FDB_PAYLOAD_E_BACKING_CONTRACT,
     UINT32_C(0), UINT32_C(1)},
    {UINT32_C(19), MatrixCategory::cleanup, "move chain releases once without retain",
     require_direct, BackingShape::stable_span, CallbackKind::retain, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(1)},
    {UINT32_C(21), MatrixCategory::documentation, "same-token reentry limit is documented",
     require_direct, BackingShape::stable_span, CallbackKind::reserve, UINT64_C(0),
     success_status, success_status, UINT32_C(0), UINT32_C(0)},
}};

struct AllocationMatrixCase final {
    std::string_view name;
    std::string_view phase;
    std::uint32_t expected_rollbacks;
    std::uint32_t expected_releases;
};

constexpr std::array<AllocationMatrixCase, 3> allocation_matrix{{
    {"plan creation before reserve", "plan", UINT32_C(0), UINT32_C(0)},
    {"direct encode after reserve", "direct", UINT32_C(1), UINT32_C(0)},
    {"staged heap encode/copy", "staging", UINT32_C(0), UINT32_C(0)},
}};

struct StatusMatrixCase final {
    CallbackKind callback;
    std::uint32_t status;
    std::uint32_t expected_error;
};

constexpr std::array<StatusMatrixCase, 18> status_matrix{{
    {CallbackKind::reserve, success_status, success_status},
    {CallbackKind::reserve, FDB_PAYLOAD_E_DIRECT_UNAVAILABLE,
     FDB_PAYLOAD_E_DIRECT_UNAVAILABLE},
    {CallbackKind::reserve, FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ALLOCATION_FAILED},
    {CallbackKind::reserve, unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT},
    {CallbackKind::write, success_status, success_status},
    {CallbackKind::write, FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ALLOCATION_FAILED},
    {CallbackKind::write, unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT},
    {CallbackKind::commit, success_status, success_status},
    {CallbackKind::commit, FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ALLOCATION_FAILED},
    {CallbackKind::commit, FDB_PAYLOAD_E_COMMIT_FAILED,
     FDB_PAYLOAD_E_COMMIT_FAILED},
    {CallbackKind::commit, unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT},
    {CallbackKind::rollback, success_status, success_status},
    {CallbackKind::rollback, FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ROLLBACK_FAILED},
    {CallbackKind::rollback, FDB_PAYLOAD_E_ROLLBACK_FAILED,
     FDB_PAYLOAD_E_ROLLBACK_FAILED},
    {CallbackKind::rollback, unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT},
    {CallbackKind::retain, success_status, success_status},
    {CallbackKind::retain, FDB_PAYLOAD_E_ALLOCATION_FAILED,
     FDB_PAYLOAD_E_ALLOCATION_FAILED},
    {CallbackKind::retain, unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT},
}};

int verify_fixture_and_matrix_skeleton() {
    require(state_machine.size() == 11U);
    require(status_matrix.size() == 18U);
    require(allocation_matrix.size() == 3U);
    require(allocation_matrix[0].phase == "plan");
    require(allocation_matrix[0].expected_rollbacks == UINT32_C(0));
    require(allocation_matrix[0].expected_releases == UINT32_C(0));
    require(allocation_matrix[1].phase == "direct");
    require(allocation_matrix[1].expected_rollbacks == UINT32_C(1));
    require(allocation_matrix[1].expected_releases == UINT32_C(0));
    require(allocation_matrix[2].phase == "staging");
    require(allocation_matrix[2].expected_rollbacks == UINT32_C(0));
    require(allocation_matrix[2].expected_releases == UINT32_C(0));
    for (const std::uint32_t status : deterministic_unknown_statuses) {
        require(status != success_status);
        require(status != FDB_PAYLOAD_E_DIRECT_UNAVAILABLE);
        require(status != FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(status != FDB_PAYLOAD_E_COMMIT_FAILED);
        require(status != FDB_PAYLOAD_E_ROLLBACK_FAILED);
    }

    std::array<bool, 22> covered{};
    for (const FrozenMatrixCase& item : frozen_matrix) {
        require(!item.name.empty());
        require(item.requirement >= UINT32_C(1));
        require(item.requirement <= UINT32_C(21));
        covered[item.requirement] = true;
    }
    covered[UINT32_C(20)] = allocation_matrix.size() == 3U;
    for (std::uint32_t requirement = UINT32_C(1);
         requirement <= UINT32_C(21); ++requirement) {
        require(covered[requirement]);
    }

    FakeBacking backing(BackingShape::range_write_only);
    const FakeCallbacks callbacks = backing.callbacks();
    require(callbacks.context == &backing);
    require(callbacks.reserve != nullptr);
    require(callbacks.write != nullptr);
    require(callbacks.commit != nullptr);
    require(callbacks.rollback != nullptr);
    require(callbacks.retain != nullptr);
    require(callbacks.release != nullptr);

    FakeBacking no_write(BackingShape::no_write_path);
    require(no_write.callbacks(false).write == nullptr);
    no_write.use_null_token();
    no_write.inject(FailureInjection{CallbackKind::reserve, UINT64_C(1),
                                     FDB_PAYLOAD_E_ALLOCATION_FAILED, true});
    require(no_write.receipts().empty());
    return EXIT_SUCCESS;
}

#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
fastdb::payload::backing::CallbackOperation production_operation(
    CallbackKind callback) {
    using Operation = fastdb::payload::backing::CallbackOperation;
    switch (callback) {
        case CallbackKind::reserve:
            return Operation::reserve_direct;
        case CallbackKind::write:
            return Operation::write;
        case CallbackKind::commit:
            return Operation::commit;
        case CallbackKind::rollback:
            return Operation::rollback;
        case CallbackKind::retain:
            return Operation::retain;
        case CallbackKind::release:
            break;
    }
    std::abort();
}

int test_callback_status_classification() {
    using fastdb::payload::backing::CallbackOperation;
    using fastdb::payload::backing::classify_callback_status;
    for (const StatusMatrixCase& item : status_matrix) {
        require(classify_callback_status(production_operation(item.callback),
                                         item.status) ==
                item.expected_error);
    }
    require(classify_callback_status(CallbackOperation::reserve_direct,
                                     FDB_PAYLOAD_E_DIRECT_UNAVAILABLE) ==
            FDB_PAYLOAD_E_DIRECT_UNAVAILABLE);
    require(classify_callback_status(CallbackOperation::reserve_staged,
                                     success_status) == success_status);
    require(classify_callback_status(CallbackOperation::reserve_staged,
                                     FDB_PAYLOAD_E_ALLOCATION_FAILED) ==
            FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(classify_callback_status(CallbackOperation::reserve_staged,
                                     FDB_PAYLOAD_E_DIRECT_UNAVAILABLE) ==
            FDB_PAYLOAD_E_BACKING_CONTRACT);
    for (const CallbackKind operation : {
             CallbackKind::reserve, CallbackKind::write,
             CallbackKind::commit, CallbackKind::rollback,
             CallbackKind::retain}) {
        for (const std::uint32_t status :
             deterministic_unknown_statuses) {
            require(classify_callback_status(
                        production_operation(operation), status) ==
                    FDB_PAYLOAD_E_BACKING_CONTRACT);
        }
    }
    for (const std::uint32_t status : deterministic_unknown_statuses) {
        require(classify_callback_status(CallbackOperation::reserve_staged,
                                         status) ==
                FDB_PAYLOAD_E_BACKING_CONTRACT);
    }
    return EXIT_SUCCESS;
}

struct RetainProbe final {
    std::uint32_t status{success_status};
    std::uint32_t retains{UINT32_C(0)};
    std::uint32_t releases{UINT32_C(0)};

    static std::uint32_t retain(void* context, void*) {
        auto& self = *static_cast<RetainProbe*>(context);
        ++self.retains;
        return self.status;
    }

    static void release(void* context, void*) {
        ++static_cast<RetainProbe*>(context)->releases;
    }

    fastdb::payload::backing::Callbacks callbacks() {
        return fastdb::payload::backing::Callbacks{
            this, nullptr, nullptr, nullptr, nullptr, &retain, &release};
    }
};

int test_retained_backing_acquisition() {
    using fastdb::payload::backing::RetainedBacking;
    static_assert(!std::is_copy_constructible_v<RetainedBacking>);
    static_assert(std::is_move_constructible_v<RetainedBacking>);

    std::array<std::uint8_t, 3> bytes{{UINT8_C(1), UINT8_C(2), UINT8_C(3)}};
    for (const bool omit_retain : {false, true}) {
        RetainProbe missing;
        auto callbacks = missing.callbacks();
        if (omit_retain) {
            callbacks.retain = nullptr;
        } else {
            callbacks.release = nullptr;
        }
        auto rejected = RetainedBacking::acquire(
            callbacks, nullptr, bytes.data(), bytes.size());
        require(!rejected.has_value());
        require(rejected.error().code() == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(rejected.error().path() == "/backing");
        require(rejected.error().details_json() ==
                "{\"reason\":\"missing_retain_or_release\"}");
        require(missing.retains == UINT32_C(0));
        require(missing.releases == UINT32_C(0));
    }

    RetainProbe success;
    {
        auto retained = RetainedBacking::acquire(
            success.callbacks(), nullptr, bytes.data(), bytes.size());
        require(retained.has_value());
        require(success.retains == UINT32_C(1));
        require(retained.value().readable_data() == bytes.data());
        require(retained.value().readable_size() == bytes.size());
        RetainedBacking moved = std::move(retained).value();
        require(moved.readable_data() == bytes.data());
    }
    require(success.releases == UINT32_C(1));

    for (const std::uint32_t status :
         {FDB_PAYLOAD_E_ALLOCATION_FAILED, unknown_status}) {
        RetainProbe failed;
        failed.status = status;
        auto rejected = RetainedBacking::acquire(
            failed.callbacks(), nullptr, bytes.data(), bytes.size());
        require(!rejected.has_value());
        require(rejected.error().code() ==
                (status == FDB_PAYLOAD_E_ALLOCATION_FAILED
                     ? FDB_PAYLOAD_E_ALLOCATION_FAILED
                     : FDB_PAYLOAD_E_BACKING_CONTRACT));
        require(failed.retains == UINT32_C(1));
        require(failed.releases == UINT32_C(0));
        if (status == unknown_status) {
            require(rejected.error().details_json() ==
                    "{\"callback\":\"retain\",\"callback_status\":1779953677}");
        }
    }
    return EXIT_SUCCESS;
}

int test_heap_container_limit_failure_is_structured() {
    const auto& callbacks = fastdb::payload::backing::heap_callbacks();
    const std::uint64_t request =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::ptrdiff_t>::max()) +
        UINT64_C(1);
    const std::size_t live_before =
        allocation_failure::current_live_bytes.load(
            std::memory_order_relaxed);
    for (std::uint32_t attempt = UINT32_C(0); attempt < UINT32_C(2);
         ++attempt) {
        void* owner_token = nullptr;
        std::uint8_t* writable_data = nullptr;
        std::uint64_t capacity = UINT64_C(0);
        const std::uint32_t status = callbacks.reserve(
            callbacks.context, direct_mode, request, UINT32_C(8),
            &owner_token, &writable_data, &capacity);
        require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(owner_token == nullptr);
        require(writable_data == nullptr);
        require(capacity == UINT64_C(0));
        require(allocation_failure::current_live_bytes.load(
                    std::memory_order_relaxed) == live_before);
    }
    return EXIT_SUCCESS;
}
#endif

template <typename Builder, typename = void>
struct HasFreezePlan : std::false_type {};

template <typename Builder>
struct HasFreezePlan<
    Builder,
    std::void_t<decltype(std::declval<Builder&>().freeze_plan())>>
    : std::true_type {};

template <typename Plan, typename = void>
struct HasExecute : std::false_type {};

template <typename Plan>
struct HasExecute<
    Plan,
    std::void_t<decltype(std::declval<const Plan&>().execute(
        allow_staging,
        static_cast<const fastdb::payload::backing::Callbacks*>(nullptr)))>>
    : std::true_type {};

template <typename Plan>
int verify_wished_for_direct_execution(const Plan& plan) {
    if constexpr (!HasExecute<Plan>::value) {
        std::cerr
            << __FILE__ << ':' << __LINE__
            << ": requirement failed: BuildPlan::execute(): "
               "missing Task 6 capability: direct plan execution does not "
               "exist\n";
        return EXIT_FAILURE;
    } else {
        auto heap = plan.execute(allow_staging, nullptr);
        require(heap.has_value());
        require(owner_data(heap.value()) != nullptr);
        require(owner_size(heap.value()) == plan.info().total_bytes);
        require(owner_capacity(heap.value()) >= plan.info().total_bytes);
        const auto& heap_report = owner_report(heap.value());
        require(heap_report.mode == direct_mode);
        require(heap_report.fallback_reason == UINT32_C(0));
        require(heap_report.requested_bytes == plan.info().total_bytes);
        require(heap_report.used_bytes == plan.info().total_bytes);
        require(heap_report.staging_bytes == UINT64_C(0));
        require(heap_report.region_count == plan.info().region_count);
        require(heap_report.backing_capacity ==
                owner_capacity(heap.value()));

#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
        FakeBacking stable(BackingShape::stable_span,
                           plan.info().total_bytes);
        auto callbacks = stable.production_callbacks(false);
        auto external = plan.execute(require_direct, &callbacks);
        require(external.has_value());
        require(owner_size(external.value()) ==
                owner_size(heap.value()));
        require(std::equal(
            owner_data(heap.value()),
            owner_data(heap.value()) + owner_size(heap.value()),
            owner_data(external.value())));
        const auto& external_report = owner_report(external.value());
        require(external_report.mode == direct_mode);
        require(external_report.fallback_reason == UINT32_C(0));
        require(external_report.requested_bytes == plan.info().total_bytes);
        require(external_report.used_bytes == plan.info().total_bytes);
        require(external_report.staging_bytes == UINT64_C(0));
        require(external_report.region_count == plan.info().region_count);
        require(external_report.backing_capacity == plan.info().total_bytes);
        return EXIT_SUCCESS;
#else
        std::cerr
            << __FILE__ << ':' << __LINE__
            << ": requirement failed: payload/backing/Backing.hpp: "
               "missing Task 6 stable-span callback capability\n";
        return EXIT_FAILURE;
#endif
    }
}

template <typename Builder>
int verify_wished_for_freeze_plan(Builder& builder) {
    if constexpr (!HasFreezePlan<Builder>::value) {
        std::cerr
            << __FILE__ << ':' << __LINE__
            << ": requirement failed: PayloadBuilder::freeze_plan(): "
               "missing Task 6 capability: PayloadBuilder::freeze_plan() "
               "does not exist\n";
        return EXIT_FAILURE;
    } else {
        auto planned = builder.freeze_plan();
        require(planned.has_value());
        const auto& info = planned.value().info();
        require(info.total_bytes == UINT64_C(128));
        require(info.region_count == UINT64_C(0));
        require(info.logical_value_count == UINT64_C(0));
        require(info.list_element_count == UINT64_C(0));
        require(info.text_bytes == UINT64_C(0));
        require(info.opaque_bytes == UINT64_C(0));
        require(info.validation_work == UINT64_C(1));
        require(info.max_alignment == UINT32_C(1));
        require(info.direct_build_status == UINT32_C(1));
        return verify_wished_for_direct_execution(planned.value());
    }
}

int test_missing_task6_capability() {
    constexpr std::string_view empty_record =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]})";
    auto compiled = CompiledSpec::compile(empty_record);
    require(compiled.has_value());
    auto created = PayloadBuilder::create(std::move(compiled).value());
    require(created.has_value());
    return verify_wished_for_freeze_plan(created.value());
}

#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
int require_complete_range_writes(const FakeBacking& backing,
                                  std::uint64_t total_bytes) {
    std::uint64_t expected_offset = UINT64_C(0);
    std::uint64_t write_count = UINT64_C(0);
    for (const CallbackReceipt& receipt : backing.receipts()) {
        if (receipt.callback != CallbackKind::write) {
            continue;
        }
        require(receipt.offset == expected_offset);
        require(receipt.source_size <= UINT64_C(65536));
        require(receipt.source_size <= total_bytes - expected_offset);
        expected_offset += receipt.source_size;
        ++write_count;
    }
    require(write_count >= UINT64_C(2));
    require(expected_offset == total_bytes);
    return EXIT_SUCCESS;
}

bool reports_equal(
    const fastdb::payload::build::ExecutionReport& left,
    const fastdb::payload::build::ExecutionReport& right) noexcept;

int test_range_staged_and_policy() {
    constexpr std::string_view source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"text","cardinality":"one","type":{"kind":"str"}},{"id":"wide","cardinality":"one","type":{"kind":"wstr"}},{"id":"blob","cardinality":"one","type":{"kind":"bytes"}},{"id":"items","cardinality":"one","type":{"kind":"list","items":{"kind":"u8"}}}],"components":[]})";
    auto compiled = CompiledSpec::compile(source);
    require(compiled.has_value());
    auto created = PayloadBuilder::create(std::move(compiled).value());
    require(created.has_value());
    std::vector<std::uint8_t> caller_bytes(70001U);
    for (std::size_t index = 0U; index < caller_bytes.size(); ++index) {
        caller_bytes[index] = static_cast<std::uint8_t>(index % 251U);
    }
    std::string caller_text{"alpha"};
    std::array<std::uint16_t, 3> caller_wide{{
        UINT16_C(0x0042), UINT16_C(0xd83d), UINT16_C(0xde00)}};
    require(created.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(created.value().push_str(caller_text).has_value());
    require(created.value().begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(created.value()
                .push_wstr(caller_wide.data(), caller_wide.size())
                .has_value());
    require(created.value().begin_entry(UINT32_C(2), UINT64_C(1)).has_value());
    require(created.value()
                .push_bytes(caller_bytes.data(), caller_bytes.size())
                .has_value());
    require(created.value().begin_entry(UINT32_C(3), UINT64_C(1)).has_value());
    require(created.value().begin_list(UINT64_C(3)).has_value());
    require(created.value().push_u8(UINT8_C(7)).has_value());
    require(created.value().push_u8(UINT8_C(8)).has_value());
    require(created.value().push_u8(UINT8_C(9)).has_value());
    auto planned = created.value().freeze_plan();
    require(planned.has_value());
    require(planned.value().info().logical_value_count == UINT64_C(11));
    require(planned.value().info().list_element_count == UINT64_C(3));
    require(planned.value().info().text_bytes == UINT64_C(11));
    require(planned.value().info().opaque_bytes == UINT64_C(70001));
    require(planned.value().info().max_alignment == UINT32_C(8));

    auto before_mutation = planned.value().execute(allow_staging, nullptr);
    require(before_mutation.has_value());
    std::vector<std::uint8_t> frozen_bytes(
        owner_data(before_mutation.value()),
        owner_data(before_mutation.value()) +
            static_cast<std::size_t>(
                owner_size(before_mutation.value())));
    const auto frozen_report =
        owner_report(before_mutation.value());

    std::fill(caller_text.begin(), caller_text.end(), '\0');
    std::fill(caller_wide.begin(), caller_wide.end(), UINT16_C(0));
    std::fill(caller_bytes.begin(), caller_bytes.end(), UINT8_C(0));
    auto heap = planned.value().execute(allow_staging, nullptr);
    require(heap.has_value());
    require(owner_size(heap.value()) == frozen_bytes.size());
    require(std::equal(frozen_bytes.begin(), frozen_bytes.end(),
                       owner_data(heap.value())));
    require(reports_equal(frozen_report,
                          owner_report(heap.value())));

    FakeBacking range_only(BackingShape::range_write_only,
                           planned.value().info().total_bytes);
    auto range_callbacks = range_only.production_callbacks(true);
    auto range =
        planned.value().execute(require_direct, &range_callbacks);
    require(range.has_value(),
            range.has_value() ? std::string_view{}
                              : range.error().details_json());
    require(owner_report(range.value()).mode == direct_mode);
    require(owner_report(range.value()).fallback_reason == UINT32_C(0));
    require(owner_report(range.value()).staging_bytes == UINT64_C(0));
    require(std::equal(owner_data(heap.value()),
                       owner_data(heap.value()) +
                           owner_size(heap.value()),
                       owner_data(range.value())));
    require(require_complete_range_writes(
                range_only, planned.value().info().total_bytes) ==
            EXIT_SUCCESS);

    FakeBacking staged(BackingShape::range_write_only,
                       planned.value().info().total_bytes);
    staged.inject(FailureInjection{CallbackKind::reserve, UINT64_C(1),
                                   FDB_PAYLOAD_E_DIRECT_UNAVAILABLE, false});
    auto staged_callbacks = staged.production_callbacks(true);
    auto staged_result =
        planned.value().execute(allow_staging, &staged_callbacks);
    require(staged_result.has_value());
    require(owner_report(staged_result.value()).mode == staged_mode);
    require(owner_report(staged_result.value()).fallback_reason ==
            UINT32_C(2));
    require(owner_report(staged_result.value()).staging_bytes ==
            planned.value().info().total_bytes);
    require(std::equal(owner_data(heap.value()),
                       owner_data(heap.value()) +
                           owner_size(heap.value()),
                       owner_data(staged_result.value())));
    require(require_complete_range_writes(
                staged, planned.value().info().total_bytes) == EXIT_SUCCESS);

    FakeBacking declined(BackingShape::decline_direct_then_staged,
                         planned.value().info().total_bytes);
    auto declined_callbacks = declined.production_callbacks(false);
    auto unavailable =
        planned.value().execute(require_direct, &declined_callbacks);
    require(!unavailable.has_value());
    require(unavailable.error().code() == FDB_PAYLOAD_E_DIRECT_UNAVAILABLE);
    require(unavailable.error().path() == "/backing");
    require(unavailable.error().details_json() ==
            "{\"reason\":\"backing_declined_direct\"}");
    require(declined.receipts().size() == 1U);
    require(declined.receipts()[0].mode == direct_mode);

    FakeBacking invalid_policy(BackingShape::stable_span,
                               planned.value().info().total_bytes);
    auto invalid_callbacks = invalid_policy.production_callbacks(false);
    auto invalid = planned.value().execute(UINT32_C(0), &invalid_callbacks);
    require(!invalid.has_value());
    require(invalid_policy.receipts().empty());
    return EXIT_SUCCESS;
}

std::uint64_t callback_count(const FakeBacking& backing,
                             CallbackKind callback) {
    return static_cast<std::uint64_t>(std::count_if(
        backing.receipts().begin(), backing.receipts().end(),
        [callback](const CallbackReceipt& receipt) {
            return receipt.callback == callback;
        }));
}

Result<PayloadBuilder> make_bytes_builder(std::uint64_t byte_count) {
    constexpr std::string_view source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"blob","cardinality":"one","type":{"kind":"bytes"}}],"components":[]})";
    auto compiled = CompiledSpec::compile(source);
    if (!compiled.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(compiled).error());
    }
    auto created = PayloadBuilder::create(std::move(compiled).value());
    if (!created.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(created).error());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byte_count),
                                    UINT8_C(0x5a));
    auto begun =
        created.value().begin_entry(UINT32_C(0), UINT64_C(1));
    if (!begun.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(begun).error());
    }
    auto pushed =
        created.value().push_bytes(bytes.data(), byte_count);
    if (!pushed.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(pushed).error());
    }
    return Result<PayloadBuilder>::success(std::move(created).value());
}

Result<BuildPlan> make_bytes_plan(std::uint64_t byte_count) {
    auto builder = make_bytes_builder(byte_count);
    if (!builder.has_value()) {
        return Result<BuildPlan>::failure(std::move(builder).error());
    }
    return builder.value().freeze_plan();
}

int test_plan_allocation_retryability() {
    std::uint64_t failures = UINT64_C(0);
    bool reached_success = false;
    for (std::int64_t allocation = INT64_C(0);
         allocation < INT64_C(4096); ++allocation) {
        auto builder = make_bytes_builder(UINT64_C(70001));
        require(builder.has_value());
        const std::size_t live_before =
            allocation_failure::current_live_bytes.load(
                std::memory_order_relaxed);
        bool failed = false;
        {
            allocation_failure::fail_after.store(
                allocation, std::memory_order_relaxed);
            auto planned = builder.value().freeze_plan();
            allocation_failure::fail_after.store(
                INT64_C(-1), std::memory_order_relaxed);
            if (planned.has_value()) {
                auto executed =
                    planned.value().execute(require_direct, nullptr);
                require(executed.has_value());
                reached_success = true;
            } else {
                require(planned.error().code() ==
                        FDB_PAYLOAD_E_ALLOCATION_FAILED);
                failed = true;
            }
        }
        if (!failed) {
            break;
        }
        require(allocation_failure::current_live_bytes.load(
                    std::memory_order_relaxed) == live_before);
        ++failures;
        auto retried = builder.value().freeze_plan();
        require(retried.has_value(), retried.has_value()
                                         ? std::string_view{}
                                         : retried.error().details_json());
        auto executed = retried.value().execute(require_direct, nullptr);
        require(executed.has_value());
    }
    require(reached_success);
    require(failures > UINT64_C(0));
    return EXIT_SUCCESS;
}

bool reports_equal(
    const fastdb::payload::build::ExecutionReport& left,
    const fastdb::payload::build::ExecutionReport& right) noexcept {
    return left.mode == right.mode &&
           left.fallback_reason == right.fallback_reason &&
           left.requested_bytes == right.requested_bytes &&
           left.used_bytes == right.used_bytes &&
           left.staging_bytes == right.staging_bytes &&
           left.region_count == right.region_count &&
           left.backing_capacity == right.backing_capacity;
}

int test_execution_allocation_sweeps() {
    auto planned = make_bytes_plan(UINT64_C(70001));
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;
    std::vector<std::uint8_t> expected;
    {
        auto heap = planned.value().execute(require_direct, nullptr);
        require(heap.has_value());
        expected.assign(owner_data(heap.value()),
                        owner_data(heap.value()) +
                            static_cast<std::size_t>(
                                owner_size(heap.value())));
    }

    std::uint64_t direct_failures = UINT64_C(0);
    bool direct_succeeded = false;
    for (std::int64_t allocation = INT64_C(0);
         allocation < INT64_C(4096); ++allocation) {
        const std::size_t live_before =
            allocation_failure::current_live_bytes.load(
                std::memory_order_relaxed);
        bool succeeded = false;
        {
            FakeBacking backing(BackingShape::stable_span, total);
            backing.reserve_receipts(8U);
            auto callbacks = backing.production_callbacks(false);
            allocation_failure::fail_after.store(
                allocation, std::memory_order_relaxed);
            auto result = planned.value().execute(require_direct, &callbacks);
            allocation_failure::fail_after.store(
                INT64_C(-1), std::memory_order_relaxed);
            if (result.has_value()) {
                require(owner_size(result.value()) == expected.size());
                require(std::equal(expected.begin(), expected.end(),
                                   owner_data(result.value())));
                succeeded = true;
            } else {
                require(result.error().code() ==
                        FDB_PAYLOAD_E_ALLOCATION_FAILED);
                require(callback_count(backing, CallbackKind::reserve) ==
                        UINT64_C(1));
                if (callback_count(backing, CallbackKind::commit) ==
                    UINT64_C(0)) {
                    require(callback_count(
                                backing, CallbackKind::rollback) ==
                            allocation_matrix[1].expected_rollbacks);
                    require(callback_count(
                                backing, CallbackKind::release) ==
                            allocation_matrix[1].expected_releases);
                } else {
                    require(callback_count(backing, CallbackKind::commit) ==
                            UINT64_C(1));
                    require(callback_count(
                                backing, CallbackKind::rollback) ==
                            UINT64_C(0));
                    require(callback_count(
                                backing, CallbackKind::release) ==
                            UINT64_C(1));
                }
                ++direct_failures;

                FakeBacking retry(BackingShape::stable_span, total);
                auto retry_callbacks = retry.production_callbacks(false);
                auto retried = planned.value().execute(
                    require_direct, &retry_callbacks);
                require(retried.has_value());
                require(std::equal(expected.begin(), expected.end(),
                                   owner_data(retried.value())));
            }
        }
        require(allocation_failure::current_live_bytes.load(
                    std::memory_order_relaxed) == live_before);
        if (succeeded) {
            direct_succeeded = true;
            break;
        }
    }
    require(direct_succeeded);
    require(direct_failures > UINT64_C(0));

    std::uint64_t staged_failures = UINT64_C(0);
    bool staged_succeeded = false;
    for (std::int64_t allocation = INT64_C(0);
         allocation < INT64_C(4096); ++allocation) {
        const std::size_t live_before =
            allocation_failure::current_live_bytes.load(
                std::memory_order_relaxed);
        bool succeeded = false;
        {
            FakeBacking backing(
                BackingShape::decline_direct_then_staged, total);
            backing.reserve_receipts(8U);
            auto callbacks = backing.production_callbacks(false);
            allocation_failure::fail_after.store(
                allocation, std::memory_order_relaxed);
            auto result = planned.value().execute(allow_staging, &callbacks);
            allocation_failure::fail_after.store(
                INT64_C(-1), std::memory_order_relaxed);
            if (result.has_value()) {
                require(owner_report(result.value()).mode ==
                        staged_mode);
                require(std::equal(expected.begin(), expected.end(),
                                   owner_data(result.value())));
                succeeded = true;
            } else {
                require(result.error().code() ==
                        FDB_PAYLOAD_E_ALLOCATION_FAILED);
                if (callback_count(backing, CallbackKind::commit) ==
                    UINT64_C(0)) {
                    const std::uint64_t reserve_count =
                        callback_count(backing, CallbackKind::reserve);
                    require(reserve_count == UINT64_C(1) ||
                            reserve_count == UINT64_C(2));
                    const std::uint64_t expected_rollbacks =
                        reserve_count == UINT64_C(1)
                            ? allocation_matrix[2].expected_rollbacks
                            : UINT64_C(1);
                    require(callback_count(backing,
                                           CallbackKind::rollback) ==
                            expected_rollbacks);
                    require(callback_count(
                                backing, CallbackKind::release) ==
                            allocation_matrix[2].expected_releases);
                } else {
                    require(callback_count(backing,
                                           CallbackKind::reserve) ==
                            UINT64_C(2));
                    require(callback_count(backing, CallbackKind::commit) ==
                            UINT64_C(1));
                    require(callback_count(
                                backing, CallbackKind::rollback) ==
                            UINT64_C(0));
                    require(callback_count(
                                backing, CallbackKind::release) ==
                            UINT64_C(1));
                }
                ++staged_failures;

                FakeBacking retry(
                    BackingShape::decline_direct_then_staged, total);
                auto retry_callbacks = retry.production_callbacks(false);
                auto retried = planned.value().execute(
                    allow_staging, &retry_callbacks);
                require(retried.has_value());
                require(owner_report(retried.value()).mode ==
                        staged_mode);
                require(std::equal(expected.begin(), expected.end(),
                                   owner_data(retried.value())));
            }
        }
        require(allocation_failure::current_live_bytes.load(
                    std::memory_order_relaxed) == live_before);
        if (succeeded) {
            staged_succeeded = true;
            break;
        }
    }
    require(staged_succeeded);
    require(staged_failures > UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_repeatable_concurrent_and_reentrant_execution() {
    auto planned = make_bytes_plan(UINT64_C(70001));
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;
    std::vector<std::uint8_t> expected;
    fastdb::payload::build::ExecutionReport expected_report{};
    {
        auto heap = planned.value().execute(require_direct, nullptr);
        require(heap.has_value());
        expected.assign(owner_data(heap.value()),
                        owner_data(heap.value()) +
                            static_cast<std::size_t>(
                                owner_size(heap.value())));
        expected_report = owner_report(heap.value());
    }

    constexpr std::size_t thread_count = 32U;
    std::array<std::unique_ptr<FakeBacking>, thread_count> backings{};
    std::array<bool, thread_count> passed{};
    std::array<fastdb::payload::build::ExecutionReport, thread_count>
        reports{};
    for (auto& backing : backings) {
        backing = std::make_unique<FakeBacking>(
            BackingShape::stable_span, total);
    }
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0U; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            auto callbacks = backings[index]->production_callbacks(false);
            auto result = planned.value().execute(require_direct, &callbacks);
            if (!result.has_value()) {
                return;
            }
            reports[index] = owner_report(result.value());
            passed[index] = owner_size(result.value()) == total &&
                            std::equal(expected.begin(), expected.end(),
                                       owner_data(result.value()));
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t index = 0U; index < thread_count; ++index) {
        require(passed[index]);
        require(reports_equal(reports[index], expected_report));
        require(callback_count(*backings[index], CallbackKind::retain) ==
                UINT64_C(0));
        require(callback_count(*backings[index], CallbackKind::rollback) ==
                UINT64_C(0));
        require(callback_count(*backings[index], CallbackKind::release) ==
                UINT64_C(1));
    }

    FakeBacking nested(BackingShape::stable_span, total);
    FakeBacking outer(BackingShape::stable_span, total);
    outer.enable_nested_execution(planned.value(), nested);
    auto outer_callbacks = outer.production_callbacks(false);
    {
        auto result = planned.value().execute(require_direct, &outer_callbacks);
        require(result.has_value());
        require(std::equal(expected.begin(), expected.end(),
                           owner_data(result.value())));
    }
    require(outer.nested_succeeded());
    require(nested.storage_equals(expected.data(), expected.size()));
    require(callback_count(nested, CallbackKind::release) == UINT64_C(1));
    require(callback_count(outer, CallbackKind::release) == UINT64_C(1));
    return EXIT_SUCCESS;
}

int test_state_status_and_cleanup() {
    auto planned = make_bytes_plan(UINT64_C(70001));
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;

    FakeBacking rollback_failed(BackingShape::range_write_only, total);
    rollback_failed.inject(FailureInjection{
        CallbackKind::write, UINT64_C(1),
        FDB_PAYLOAD_E_ALLOCATION_FAILED, false});
    rollback_failed.inject(FailureInjection{
        CallbackKind::rollback, UINT64_C(1),
        FDB_PAYLOAD_E_ROLLBACK_FAILED, false});
    auto rollback_failed_callbacks =
        rollback_failed.production_callbacks(true);
    auto rollback_result = planned.value().execute(
        require_direct, &rollback_failed_callbacks);
    require(!rollback_result.has_value());
    require(rollback_result.error().code() ==
            FDB_PAYLOAD_E_ROLLBACK_FAILED);
    require(rollback_result.error().details_json() ==
            "{\"original_code\":5002,\"original_symbol\":\"ALLOCATION_FAILED\",\"rollback_status\":5004}");
    require(callback_count(rollback_failed, CallbackKind::rollback) ==
            UINT64_C(1));
    require(callback_count(rollback_failed, CallbackKind::release) ==
            UINT64_C(0));

    FakeBacking rollback_unknown(BackingShape::range_write_only, total);
    rollback_unknown.inject(FailureInjection{
        CallbackKind::write, UINT64_C(1),
        FDB_PAYLOAD_E_ALLOCATION_FAILED, false});
    rollback_unknown.inject(FailureInjection{
        CallbackKind::rollback, UINT64_C(1), unknown_status, false});
    auto rollback_unknown_callbacks =
        rollback_unknown.production_callbacks(true);
    auto unknown_rollback_result = planned.value().execute(
        require_direct, &rollback_unknown_callbacks);
    require(!unknown_rollback_result.has_value());
    require(unknown_rollback_result.error().code() ==
            FDB_PAYLOAD_E_BACKING_CONTRACT);
    require(unknown_rollback_result.error().details_json() ==
            "{\"callback\":\"rollback\",\"callback_status\":1779953677,\"original_code\":5002,\"original_symbol\":\"ALLOCATION_FAILED\",\"rollback_status\":1779953677}");
    require(callback_count(rollback_unknown, CallbackKind::rollback) ==
            UINT64_C(1));
    require(callback_count(rollback_unknown, CallbackKind::release) ==
            UINT64_C(0));

    FakeBacking null_token(BackingShape::stable_span, total);
    null_token.use_null_token();
    auto null_callbacks = null_token.production_callbacks(false);
    {
        auto result =
            planned.value().execute(require_direct, &null_callbacks);
        require(result.has_value());
        require(callback_count(null_token, CallbackKind::release) ==
                UINT64_C(0));
        require(callback_count(null_token, CallbackKind::retain) ==
                UINT64_C(0));
    }
    require(callback_count(null_token, CallbackKind::rollback) ==
            UINT64_C(0));
    require(callback_count(null_token, CallbackKind::release) ==
            UINT64_C(1));
    const auto null_commit = std::find_if(
        null_token.receipts().begin(), null_token.receipts().end(),
        [](const CallbackReceipt& receipt) {
            return receipt.callback == CallbackKind::commit;
        });
    const auto null_release = std::find_if(
        null_token.receipts().begin(), null_token.receipts().end(),
        [](const CallbackReceipt& receipt) {
            return receipt.callback == CallbackKind::release;
        });
    require(null_commit != null_token.receipts().end());
    require(null_release != null_token.receipts().end());
    require(null_commit->token == nullptr);
    require(null_release->token == nullptr);

    FakeBacking dirty_reserve(BackingShape::stable_span, total);
    dirty_reserve.inject(FailureInjection{
        CallbackKind::reserve, UINT64_C(1),
        FDB_PAYLOAD_E_ALLOCATION_FAILED, true});
    auto dirty_reserve_callbacks =
        dirty_reserve.production_callbacks(false);
    auto reserve_result = planned.value().execute(
        require_direct, &dirty_reserve_callbacks);
    require(!reserve_result.has_value());
    require(reserve_result.error().code() ==
            FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(callback_count(dirty_reserve, CallbackKind::rollback) ==
            UINT64_C(0));
    require(callback_count(dirty_reserve, CallbackKind::release) ==
            UINT64_C(0));

    struct InvalidReservationCase final {
        BackingShape shape;
        bool with_write;
    };
    constexpr std::array<InvalidReservationCase, 3> invalid_reservations{{
        {BackingShape::short_capacity, false},
        {BackingShape::misaligned_span, false},
        {BackingShape::no_write_path, false},
    }};
    for (const InvalidReservationCase& item : invalid_reservations) {
        FakeBacking backing(item.shape, total);
        auto callbacks = backing.production_callbacks(item.with_write);
        auto result = planned.value().execute(require_direct, &callbacks);
        require(!result.has_value());
        require(result.error().code() == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(callback_count(backing, CallbackKind::rollback) ==
                UINT64_C(1));
        require(callback_count(backing, CallbackKind::release) ==
                UINT64_C(0));
    }

    FakeBacking write_baseline(BackingShape::range_write_only, total);
    auto write_baseline_callbacks =
        write_baseline.production_callbacks(true);
    {
        auto result = planned.value().execute(
            require_direct, &write_baseline_callbacks);
        require(result.has_value());
    }
    const std::uint64_t write_calls =
        callback_count(write_baseline, CallbackKind::write);
    require(write_calls >= UINT64_C(3));
    const std::array<std::uint64_t, 3> write_failures{{
        UINT64_C(1), (write_calls + UINT64_C(1)) / UINT64_C(2),
        write_calls}};
    for (const std::uint64_t failure_call : write_failures) {
        FakeBacking backing(BackingShape::range_write_only, total);
        backing.inject(FailureInjection{
            CallbackKind::write, failure_call,
            FDB_PAYLOAD_E_ALLOCATION_FAILED, false});
        auto callbacks = backing.production_callbacks(true);
        auto result = planned.value().execute(require_direct, &callbacks);
        require(!result.has_value());
        require(result.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(callback_count(backing, CallbackKind::rollback) ==
                UINT64_C(1));
        require(callback_count(backing, CallbackKind::release) ==
                UINT64_C(0));
    }

    struct CommitFailureCase final {
        std::uint32_t status;
        std::uint32_t expected_error;
    };
    constexpr std::array<CommitFailureCase, 3> commit_failures{{
        {FDB_PAYLOAD_E_ALLOCATION_FAILED,
         FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {FDB_PAYLOAD_E_COMMIT_FAILED, FDB_PAYLOAD_E_COMMIT_FAILED},
        {unknown_status, FDB_PAYLOAD_E_BACKING_CONTRACT},
    }};
    for (const CommitFailureCase& item : commit_failures) {
        FakeBacking backing(BackingShape::stable_span, total);
        backing.inject(FailureInjection{CallbackKind::commit, UINT64_C(1),
                                        item.status, true});
        auto callbacks = backing.production_callbacks(false);
        auto result = planned.value().execute(require_direct, &callbacks);
        require(!result.has_value());
        require(result.error().code() == item.expected_error);
        require(callback_count(backing, CallbackKind::rollback) ==
                UINT64_C(1));
        require(callback_count(backing, CallbackKind::release) ==
                UINT64_C(0));
    }

    constexpr std::array<BackingShape, 3> invalid_commits{{
        BackingShape::null_commit_span,
        BackingShape::short_commit_span,
        BackingShape::oversized_commit_span,
    }};
    for (const BackingShape shape : invalid_commits) {
        FakeBacking backing(shape, total);
        auto callbacks = backing.production_callbacks(false);
        auto result = planned.value().execute(require_direct, &callbacks);
        require(!result.has_value());
        require(result.error().code() == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(callback_count(backing, CallbackKind::rollback) ==
                UINT64_C(0));
        require(callback_count(backing, CallbackKind::release) ==
                UINT64_C(1));
    }

    FakeBacking relocated(BackingShape::relocated_commit_span, total);
    auto relocated_callbacks = relocated.production_callbacks(false);
    {
        auto result = planned.value().execute(
            require_direct, &relocated_callbacks);
        require(result.has_value());
    }
    require(callback_count(relocated, CallbackKind::rollback) == UINT64_C(0));
    require(callback_count(relocated, CallbackKind::release) == UINT64_C(1));

    FakeBacking staged_declined(BackingShape::range_write_only, total);
    staged_declined.inject(FailureInjection{
        CallbackKind::reserve, UINT64_C(1),
        FDB_PAYLOAD_E_DIRECT_UNAVAILABLE, false});
    staged_declined.inject(FailureInjection{
        CallbackKind::reserve, UINT64_C(2),
        FDB_PAYLOAD_E_DIRECT_UNAVAILABLE, false});
    auto staged_declined_callbacks =
        staged_declined.production_callbacks(true);
    auto staged_declined_result = planned.value().execute(
        allow_staging, &staged_declined_callbacks);
    require(!staged_declined_result.has_value());
    require(staged_declined_result.error().code() ==
            FDB_PAYLOAD_E_BACKING_CONTRACT);
    require(callback_count(staged_declined, CallbackKind::reserve) ==
            UINT64_C(2));
    require(callback_count(staged_declined, CallbackKind::rollback) ==
            UINT64_C(0));
    require(callback_count(staged_declined, CallbackKind::release) ==
            UINT64_C(0));

    FakeBacking moved(BackingShape::stable_span, total);
    auto moved_callbacks = moved.production_callbacks(false);
    {
        auto first = planned.value().execute(require_direct, &moved_callbacks);
        require(first.has_value());
        auto second = std::move(first).value();
        auto third = std::move(second);
        require(owner_size(third) == total);
    }
    require(callback_count(moved, CallbackKind::retain) == UINT64_C(0));
    require(callback_count(moved, CallbackKind::rollback) == UINT64_C(0));
    require(callback_count(moved, CallbackKind::release) == UINT64_C(1));
    return EXIT_SUCCESS;
}

int test_executed_unknown_statuses() {
    auto planned = make_bytes_plan(UINT64_C(70001));
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;

    FakeBacking rollback_allocation(BackingShape::range_write_only, total);
    rollback_allocation.inject(FailureInjection{
        CallbackKind::write, UINT64_C(1),
        FDB_PAYLOAD_E_ALLOCATION_FAILED, false});
    rollback_allocation.inject(FailureInjection{
        CallbackKind::rollback, UINT64_C(1),
        FDB_PAYLOAD_E_ALLOCATION_FAILED, false});
    auto rollback_allocation_callbacks =
        rollback_allocation.production_callbacks(true);
    auto rollback_allocation_result = planned.value().execute(
        require_direct, &rollback_allocation_callbacks);
    require(!rollback_allocation_result.has_value());
    require(rollback_allocation_result.error().code() ==
            FDB_PAYLOAD_E_ROLLBACK_FAILED);
    require(rollback_allocation_result.error().details_json() ==
            "{\"original_code\":5002,\"original_symbol\":\"ALLOCATION_FAILED\",\"rollback_status\":5002}");
    require(callback_count(rollback_allocation, CallbackKind::rollback) ==
            UINT64_C(1));
    require(callback_count(rollback_allocation, CallbackKind::release) ==
            UINT64_C(0));

    for (const std::uint32_t status : deterministic_unknown_statuses) {
        const std::string status_text = std::to_string(status);

        FakeBacking reserve_unknown(BackingShape::stable_span, total);
        reserve_unknown.inject(FailureInjection{
            CallbackKind::reserve, UINT64_C(1), status, true});
        auto reserve_callbacks =
            reserve_unknown.production_callbacks(false);
        auto reserve_result = planned.value().execute(
            require_direct, &reserve_callbacks);
        require(!reserve_result.has_value());
        require(reserve_result.error().code() ==
                FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(reserve_result.error().details_json() ==
                "{\"callback\":\"reserve\",\"callback_status\":" +
                    status_text + "}");
        require(callback_count(reserve_unknown, CallbackKind::rollback) ==
                UINT64_C(0));
        require(callback_count(reserve_unknown, CallbackKind::release) ==
                UINT64_C(0));

        FakeBacking write_unknown(BackingShape::range_write_only, total);
        write_unknown.inject(FailureInjection{
            CallbackKind::write, UINT64_C(1), status, false});
        auto write_callbacks = write_unknown.production_callbacks(true);
        auto write_result = planned.value().execute(
            require_direct, &write_callbacks);
        require(!write_result.has_value());
        require(write_result.error().code() ==
                FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(write_result.error().details_json() ==
                "{\"callback\":\"write\",\"callback_status\":" +
                    status_text + "}");
        require(callback_count(write_unknown, CallbackKind::rollback) ==
                UINT64_C(1));
        require(callback_count(write_unknown, CallbackKind::release) ==
                UINT64_C(0));

        FakeBacking commit_unknown(BackingShape::stable_span, total);
        commit_unknown.inject(FailureInjection{
            CallbackKind::commit, UINT64_C(1), status, true});
        auto commit_callbacks =
            commit_unknown.production_callbacks(false);
        auto commit_result = planned.value().execute(
            require_direct, &commit_callbacks);
        require(!commit_result.has_value());
        require(commit_result.error().code() ==
                FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(commit_result.error().details_json() ==
                "{\"callback\":\"commit\",\"callback_status\":" +
                    status_text + "}");
        require(callback_count(commit_unknown, CallbackKind::rollback) ==
                UINT64_C(1));
        require(callback_count(commit_unknown, CallbackKind::release) ==
                UINT64_C(0));

        FakeBacking rollback_unknown(BackingShape::range_write_only, total);
        rollback_unknown.inject(FailureInjection{
            CallbackKind::write, UINT64_C(1),
            FDB_PAYLOAD_E_ALLOCATION_FAILED, false});
        rollback_unknown.inject(FailureInjection{
            CallbackKind::rollback, UINT64_C(1), status, false});
        auto rollback_callbacks =
            rollback_unknown.production_callbacks(true);
        auto rollback_result = planned.value().execute(
            require_direct, &rollback_callbacks);
        require(!rollback_result.has_value());
        require(rollback_result.error().code() ==
                FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(rollback_result.error().details_json() ==
                "{\"callback\":\"rollback\",\"callback_status\":" +
                    status_text +
                    ",\"original_code\":5002,\"original_symbol\":\"ALLOCATION_FAILED\",\"rollback_status\":" +
                    status_text + "}");
        require(callback_count(rollback_unknown, CallbackKind::rollback) ==
                UINT64_C(1));
        require(callback_count(rollback_unknown, CallbackKind::release) ==
                UINT64_C(0));
    }
    return EXIT_SUCCESS;
}

int test_callback_requirements_and_prefix_copy() {
    auto planned = make_bytes_plan(UINT64_C(32));
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;

    constexpr std::array<CallbackKind, 4> required_callbacks{{
        CallbackKind::reserve,
        CallbackKind::commit,
        CallbackKind::rollback,
        CallbackKind::release,
    }};
    for (const CallbackKind missing : required_callbacks) {
        FakeBacking backing(BackingShape::stable_span, total);
        auto callbacks = backing.production_callbacks(false);
        switch (missing) {
            case CallbackKind::reserve:
                callbacks.reserve = nullptr;
                break;
            case CallbackKind::commit:
                callbacks.commit = nullptr;
                break;
            case CallbackKind::rollback:
                callbacks.rollback = nullptr;
                break;
            case CallbackKind::release:
                callbacks.release = nullptr;
                break;
            case CallbackKind::write:
            case CallbackKind::retain:
                std::abort();
        }
        auto result = planned.value().execute(require_direct, &callbacks);
        require(!result.has_value());
        require(result.error().code() == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(result.error().details_json() ==
                "{\"reason\":\"missing_required_callback\"}");
        require(backing.receipts().empty());
    }

    const std::uint64_t larger_capacity = total + UINT64_C(64);
    FakeBacking optional_retain(BackingShape::stable_span,
                                larger_capacity);
    auto callbacks = optional_retain.production_callbacks(false);
    callbacks.retain = nullptr;
    {
        auto result = planned.value().execute(require_direct, &callbacks);
        require(result.has_value());
        require(owner_capacity(result.value()) == larger_capacity);
        require(owner_report(result.value()).backing_capacity ==
                larger_capacity);
        const auto reserve_receipt = std::find_if(
            optional_retain.receipts().begin(),
            optional_retain.receipts().end(),
            [](const CallbackReceipt& receipt) {
                return receipt.callback == CallbackKind::reserve;
            });
        const auto commit_receipt = std::find_if(
            optional_retain.receipts().begin(),
            optional_retain.receipts().end(),
            [](const CallbackReceipt& receipt) {
                return receipt.callback == CallbackKind::commit;
            });
        require(reserve_receipt != optional_retain.receipts().end());
        require(reserve_receipt->mode == direct_mode);
        require(reserve_receipt->minimum_capacity == total);
        require(reserve_receipt->alignment ==
                planned.value().info().max_alignment);
        require(commit_receipt != optional_retain.receipts().end());
        require(commit_receipt->used_size == total);
        callbacks.context = nullptr;
        callbacks.release = nullptr;
    }
    require(callback_count(optional_retain, CallbackKind::retain) ==
            UINT64_C(0));
    require(callback_count(optional_retain, CallbackKind::rollback) ==
            UINT64_C(0));
    require(callback_count(optional_retain, CallbackKind::release) ==
            UINT64_C(1));
    return EXIT_SUCCESS;
}
#endif

}  // namespace

int main() {
    if (verify_fixture_and_matrix_skeleton() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_missing_task6_capability() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
#if FASTDB_TASK6_HAS_BACKING_CALLBACKS
    if (test_callback_status_classification() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_retained_backing_acquisition() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_heap_container_limit_failure_is_structured() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_range_staged_and_policy() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_state_status_and_cleanup() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_executed_unknown_statuses() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_callback_requirements_and_prefix_copy() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_plan_allocation_retryability() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_execution_allocation_sweeps() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_repeatable_concurrent_and_reentrant_execution();
#else
    return EXIT_FAILURE;
#endif
}
