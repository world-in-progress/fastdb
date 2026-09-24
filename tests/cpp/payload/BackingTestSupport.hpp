#pragma once

#include "payload/backing/Backing.hpp"
#include "payload/build/BuildPlan.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#define FASTDB_TASK6_HAS_BACKING_CALLBACKS 1

namespace fastdb::payload::view {

struct PayloadOwnerTestAccess final {
    static const backing::CommittedBacking& backing(
        const PayloadOwner& owner) noexcept {
        if (!owner.state_->backing.has_value()) {
            std::abort();
        }
        return *owner.state_->backing;
    }
};

}  // namespace fastdb::payload::view

namespace fastdb::payload::test {

inline const backing::CommittedBacking& owner_backing(
    const view::PayloadOwner& owner) noexcept {
    return view::PayloadOwnerTestAccess::backing(owner);
}

inline const std::uint8_t* owner_data(
    const view::PayloadOwner& owner) noexcept {
    return owner_backing(owner).readable_data();
}

inline std::uint64_t owner_size(
    const view::PayloadOwner& owner) noexcept {
    return owner_backing(owner).readable_size();
}

inline std::uint64_t owner_capacity(
    const view::PayloadOwner& owner) noexcept {
    return owner_backing(owner).capacity();
}

inline const build::ExecutionReport& owner_report(
    const view::PayloadOwner& owner) {
    return owner.execution_report().value();
}

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
    corrupt_committed_image,
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
    explicit FakeBacking(
        BackingShape shape,
        std::uint64_t capacity = UINT64_C(128) * UINT64_C(1024))
        : shape_(shape),
          storage_(static_cast<std::size_t>(capacity), UINT8_C(0)),
          relocated_storage_(static_cast<std::size_t>(capacity),
                             UINT8_C(0)) {}

    FakeCallbacks callbacks(bool with_write = true) noexcept {
        return FakeCallbacks{this,
                             &reserve_thunk,
                             with_write ? &write_thunk : nullptr,
                             &commit_thunk,
                             &rollback_thunk,
                             &retain_thunk,
                             &release_thunk};
    }

    backing::Callbacks production_callbacks(
        bool with_write = true) noexcept {
        return backing::Callbacks{this,
                                  &reserve_thunk,
                                  with_write ? &write_thunk : nullptr,
                                  &commit_thunk,
                                  &rollback_thunk,
                                  &retain_thunk,
                                  &release_thunk};
    }

    void inject(FailureInjection injection) {
        injections_.push_back(injection);
    }

    void use_null_token(bool value = true) noexcept {
        null_token_ = value;
    }

    void reserve_receipts(std::size_t count) {
        receipts_.reserve(count);
    }

    void enable_nested_execution(const build::BuildPlan& plan,
                                 FakeBacking& backing) noexcept {
        nested_plan_ = &plan;
        nested_backing_ = &backing;
    }

    bool nested_succeeded() const noexcept { return nested_succeeded_; }

    const std::vector<CallbackReceipt>& receipts() const noexcept {
        return receipts_;
    }

    const std::vector<std::uint8_t>& storage() const noexcept {
        return storage_;
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

    std::uint32_t status_for(CallbackKind callback,
                             bool& dirty_outputs) {
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
        if (nested_plan_ != nullptr && !nested_started_) {
            nested_started_ = true;
            auto nested_callbacks =
                nested_backing_->production_callbacks(false);
            auto nested = nested_plan_->execute(require_direct,
                                                &nested_callbacks);
            nested_succeeded_ = nested.has_value();
        }
        bool dirty_outputs = false;
        std::uint32_t status =
            status_for(CallbackKind::reserve, dirty_outputs);
        if (status == success_status &&
            shape_ == BackingShape::decline_direct_then_staged &&
            mode == direct_mode) {
            status = FDB_PAYLOAD_E_DIRECT_UNAVAILABLE;
        }
        if (status != success_status && dirty_outputs) {
            *out_owner = reinterpret_cast<void*>(UINTPTR_MAX);
            *out_writable =
                reinterpret_cast<std::uint8_t*>(UINTPTR_MAX);
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
            *out_readable =
                reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX);
            *out_readable_size = UINT64_MAX;
        } else if (status == success_status) {
            *out_readable = storage_.data();
            *out_readable_size = used_size;
            if (shape_ == BackingShape::corrupt_committed_image &&
                used_size != UINT64_C(0)) {
                storage_[0] ^= UINT8_C(0xff);
            }
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
            CallbackKind::rollback, UINT32_C(0), UINT64_C(0),
            UINT32_C(0), owner, UINT64_C(0), UINT64_C(0), UINT64_C(0),
            nullptr, nullptr, UINT64_C(0), nullptr, UINT64_C(0), status});
        return status;
    }

    std::uint32_t retain(void* owner) {
        bool dirty_outputs = false;
        const std::uint32_t status =
            status_for(CallbackKind::retain, dirty_outputs);
        receipts_.push_back(CallbackReceipt{
            CallbackKind::retain, UINT32_C(0), UINT64_C(0),
            UINT32_C(0), owner, UINT64_C(0), UINT64_C(0), UINT64_C(0),
            nullptr, nullptr, UINT64_C(0), nullptr, UINT64_C(0), status});
        return status;
    }

    void release(void* owner) {
        bool ignored = false;
        static_cast<void>(status_for(CallbackKind::release, ignored));
        receipts_.push_back(CallbackReceipt{
            CallbackKind::release, UINT32_C(0), UINT64_C(0),
            UINT32_C(0), owner, UINT64_C(0), UINT64_C(0), UINT64_C(0),
            nullptr, nullptr, UINT64_C(0), nullptr, UINT64_C(0),
            success_status});
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
    const build::BuildPlan* nested_plan_{nullptr};
    FakeBacking* nested_backing_{nullptr};
    bool nested_started_{false};
    bool nested_succeeded_{false};
};

inline std::uint64_t callback_count(const FakeBacking& backing,
                                    CallbackKind callback) {
    return static_cast<std::uint64_t>(std::count_if(
        backing.receipts().begin(), backing.receipts().end(),
        [callback](const CallbackReceipt& receipt) {
            return receipt.callback == callback;
        }));
}

}  // namespace fastdb::payload::test
