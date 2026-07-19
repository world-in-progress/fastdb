#pragma once

#include <fastdb_payload.h>

#include "payload/error/Error.hpp"
#include "payload/error/Result.hpp"
#include "payload/build/BuildPlan.hpp"
#include "payload/build/PayloadBuilder.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace fastdb::payload::abi::detail {

struct ByteView final {
    const std::uint8_t* data{nullptr};
    std::uint64_t size{UINT64_C(0)};
};

using ErrorDestroy = void (*)(fdb_payload_v1_error_t*) noexcept;

template <typename Handle>
void retain_reference(Handle* handle) noexcept {
    if (handle == nullptr) {
        return;
    }
    constexpr std::uint64_t immortal =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t current =
        handle->references.load(std::memory_order_relaxed);
    while (current != immortal && current != UINT64_C(0)) {
        if (handle->references.compare_exchange_weak(
                current, current + UINT64_C(1), std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

template <typename Handle, typename Destroy>
void release_reference(Handle* handle, Destroy&& destroy) noexcept {
    if (handle == nullptr) {
        return;
    }
    constexpr std::uint64_t immortal =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t current =
        handle->references.load(std::memory_order_acquire);
    while (current != immortal && current != UINT64_C(0)) {
        if (handle->references.compare_exchange_weak(
                current, current - UINT64_C(1), std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (current == UINT64_C(1)) {
                std::forward<Destroy>(destroy)(handle);
            }
            return;
        }
    }
}

}  // namespace fastdb::payload::abi::detail

struct fdb_payload_v1_spec final {
    explicit fdb_payload_v1_spec(fastdb::payload::spec::CompiledSpec value)
        : compiled(std::move(value)) {}

    std::atomic<std::uint64_t> references{UINT64_C(1)};
    const fastdb::payload::spec::CompiledSpec compiled;
};

struct fdb_payload_v1_blob final {
    explicit fdb_payload_v1_blob(std::vector<std::uint8_t> value)
        : bytes(std::move(value)) {}

    std::atomic<std::uint64_t> references{UINT64_C(1)};
    const std::vector<std::uint8_t> bytes;
};

struct fdb_payload_v1_builder final {
    explicit fdb_payload_v1_builder(
        std::unique_ptr<fastdb::payload::build::PayloadBuilder> initial_value)
        noexcept
        : value(std::move(initial_value)) {}

    std::unique_ptr<fastdb::payload::build::PayloadBuilder> value;
};

struct fdb_payload_v1_plan final {
    explicit fdb_payload_v1_plan(
        fastdb::payload::build::BuildPlan initial_value) noexcept
        : value(std::move(initial_value)) {}

    std::atomic<std::uint64_t> references{UINT64_C(1)};
    const fastdb::payload::build::BuildPlan value;
};

struct fdb_payload_v1_payload final {
    explicit fdb_payload_v1_payload(
        fastdb::payload::view::PayloadOwner initial_value) noexcept
        : value(std::move(initial_value)) {}

    std::atomic<std::uint64_t> references{UINT64_C(1)};
    const fastdb::payload::view::PayloadOwner value;
};

struct fdb_payload_v1_error {
    fdb_payload_v1_error(
        std::uint64_t initial_references,
        std::uint32_t initial_code,
        fastdb::payload::abi::detail::ByteView initial_symbol,
        fastdb::payload::abi::detail::ByteView initial_path,
        fastdb::payload::abi::detail::ByteView initial_message,
        fastdb::payload::abi::detail::ByteView initial_details_json,
        fastdb::payload::abi::detail::ErrorDestroy initial_destroy) noexcept
        : references(initial_references),
          code(initial_code),
          symbol(initial_symbol),
          path(initial_path),
          message(initial_message),
          details_json(initial_details_json),
          destroy(initial_destroy) {}

    std::atomic<std::uint64_t> references;
    std::uint32_t code;
    fastdb::payload::abi::detail::ByteView symbol;
    fastdb::payload::abi::detail::ByteView path;
    fastdb::payload::abi::detail::ByteView message;
    fastdb::payload::abi::detail::ByteView details_json;
    fastdb::payload::abi::detail::ErrorDestroy destroy;
};

namespace fastdb::payload::abi {

fdb_payload_v1_blob_t* make_blob(std::vector<std::uint8_t> bytes);
fdb_payload_v1_error_t* make_error(error::Error error) noexcept;
fdb_payload_v1_error_t* allocation_error() noexcept;

fdb_payload_v1_status_t publish_error(
    const error::Error& error,
    fdb_payload_v1_error_t** out_error) noexcept;
fdb_payload_v1_status_t publish_internal_error(
    const char* message,
    fdb_payload_v1_error_t** out_error) noexcept;

template <typename Operation>
fdb_payload_v1_status_t guard_status(
    fdb_payload_v1_error_t** out_error,
    Operation&& operation) noexcept {
    if (out_error == nullptr) {
        return FDB_PAYLOAD_E_INVALID_ARGUMENT;
    }
    *out_error = nullptr;
    try {
        error::Result<void> result =
            std::forward<Operation>(operation)();
        if (result.has_value()) {
            return UINT32_C(0);
        }
        return publish_error(result.error(), out_error);
    } catch (const std::bad_alloc&) {
        *out_error = allocation_error();
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    } catch (const error::Error& known) {
        return publish_error(known, out_error);
    } catch (const std::exception&) {
        return publish_internal_error("Unhandled internal exception",
                                      out_error);
    } catch (...) {
        return publish_internal_error("Unhandled unknown exception",
                                      out_error);
    }
}

}  // namespace fastdb::payload::abi
