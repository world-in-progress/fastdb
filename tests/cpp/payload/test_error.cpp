#include "TestSupport.hpp"

#include <fastdb_payload.h>

#include "payload/abi/Handles.hpp"
#include "payload/error/Error.hpp"
#include "payload/error/Result.hpp"
#include "payload/json/Jcs.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

std::atomic<bool> fail_allocations{false};

void* test_allocate(std::size_t size) {
    if (fail_allocations.load(std::memory_order_relaxed)) {
        throw std::bad_alloc{};
    }
    void* const allocation = std::malloc(size == 0U ? 1U : size);
    if (allocation == nullptr) {
        throw std::bad_alloc{};
    }
    return allocation;
}

}  // namespace

void* operator new(std::size_t size) { return test_allocate(size); }

void* operator new[](std::size_t size) { return test_allocate(size); }

void operator delete(void* allocation) noexcept { std::free(allocation); }

void operator delete[](void* allocation) noexcept { std::free(allocation); }

void operator delete(void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

static_assert(sizeof(fdb_payload_v1_compile_options_t) ==
                  FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE,
              "compile options ABI size");
static_assert(sizeof(fdb_payload_v1_capabilities_t) ==
                  FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE,
              "capabilities ABI size");
static_assert(std::is_same_v<fdb_payload_v1_status_t, std::uint32_t>);
static_assert(std::is_same_v<fdb_payload_v1_profile_t, std::uint32_t>);

namespace {

using fastdb::payload::abi::guard_status;
using fastdb::payload::abi::make_blob;
using fastdb::payload::abi::make_error;
using fastdb::payload::error::Error;
using fastdb::payload::error::Result;
using fastdb::payload::json::JcsFailure;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::json::JsonPointerBuilder;
using fastdb::payload::json::JsonValue;

struct ErrorCase final {
    std::uint32_t code;
    std::string_view symbol;
};

std::atomic<std::uint32_t> saturation_destroy_calls{UINT32_C(0)};

void record_saturation_destroy(fdb_payload_v1_error_t*) noexcept {
    saturation_destroy_calls.fetch_add(UINT32_C(1),
                                       std::memory_order_relaxed);
}

constexpr ErrorCase error_cases[] = {
    {FDB_PAYLOAD_E_INVALID_JSON, "INVALID_JSON"},
    {FDB_PAYLOAD_E_DUPLICATE_KEY, "DUPLICATE_KEY"},
    {FDB_PAYLOAD_E_UNKNOWN_FIELD, "UNKNOWN_FIELD"},
    {FDB_PAYLOAD_E_UNSUPPORTED_SCHEMA, "UNSUPPORTED_SCHEMA"},
    {FDB_PAYLOAD_E_INVALID_TYPE, "INVALID_TYPE"},
    {FDB_PAYLOAD_E_DUPLICATE_ID, "DUPLICATE_ID"},
    {FDB_PAYLOAD_E_UNRESOLVED_COMPONENT, "UNRESOLVED_COMPONENT"},
    {FDB_PAYLOAD_E_PROFILE_VIOLATION, "PROFILE_VIOLATION"},
    {FDB_PAYLOAD_E_INVALID_NUMBER, "INVALID_NUMBER"},
    {FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "SPEC_RESOURCE_LIMIT"},
    {FDB_PAYLOAD_E_MISSING_ENTRY, "MISSING_ENTRY"},
    {FDB_PAYLOAD_E_MISSING_FIELD, "MISSING_FIELD"},
    {FDB_PAYLOAD_E_UNEXPECTED_NULL, "UNEXPECTED_NULL"},
    {FDB_PAYLOAD_E_TYPE_MISMATCH, "TYPE_MISMATCH"},
    {FDB_PAYLOAD_E_OUT_OF_RANGE, "OUT_OF_RANGE"},
    {FDB_PAYLOAD_E_BUILDER_STATE, "BUILDER_STATE"},
    {FDB_PAYLOAD_E_DIRECT_UNAVAILABLE, "DIRECT_UNAVAILABLE"},
    {FDB_PAYLOAD_E_PLAN_STATE, "PLAN_STATE"},
    {FDB_PAYLOAD_E_INVALID_MAGIC, "INVALID_MAGIC"},
    {FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION,
     "UNSUPPORTED_BINARY_VERSION"},
    {FDB_PAYLOAD_E_LENGTH_OVERFLOW, "LENGTH_OVERFLOW"},
    {FDB_PAYLOAD_E_OUT_OF_BOUNDS, "OUT_OF_BOUNDS"},
    {FDB_PAYLOAD_E_MISALIGNED, "MISALIGNED"},
    {FDB_PAYLOAD_E_DIGEST_MISMATCH, "DIGEST_MISMATCH"},
    {FDB_PAYLOAD_E_INVALID_REFERENCE, "INVALID_REFERENCE"},
    {FDB_PAYLOAD_E_RESOURCE_LIMIT, "RESOURCE_LIMIT"},
    {FDB_PAYLOAD_E_VIEW_INVALIDATED, "VIEW_INVALIDATED"},
    {FDB_PAYLOAD_E_STALE_GENERATION, "STALE_GENERATION"},
    {FDB_PAYLOAD_E_READ_ONLY, "READ_ONLY"},
    {FDB_PAYLOAD_E_BACKING_CONTRACT, "BACKING_CONTRACT"},
    {FDB_PAYLOAD_E_ALLOCATION_FAILED, "ALLOCATION_FAILED"},
    {FDB_PAYLOAD_E_COMMIT_FAILED, "COMMIT_FAILED"},
    {FDB_PAYLOAD_E_ROLLBACK_FAILED, "ROLLBACK_FAILED"},
    {FDB_PAYLOAD_E_UNSUPPORTED_TARGET, "UNSUPPORTED_TARGET"},
    {FDB_PAYLOAD_E_INVALID_ARTIFACT_PATH, "INVALID_ARTIFACT_PATH"},
    {FDB_PAYLOAD_E_GENERATOR_FAILED, "GENERATOR_FAILED"},
    {FDB_PAYLOAD_E_INVALID_ARGUMENT, "INVALID_ARGUMENT"},
    {FDB_PAYLOAD_E_UNSUPPORTED_ABI, "UNSUPPORTED_ABI"},
    {FDB_PAYLOAD_E_NOT_FOUND, "NOT_FOUND"},
    {FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE, "INDEX_OUT_OF_RANGE"},
    {FDB_PAYLOAD_E_INTERNAL, "INTERNAL"},
};

std::string_view bytes_view(const std::uint8_t* data, std::uint64_t size) {
    if (data == nullptr) {
        return {};
    }
    return {reinterpret_cast<const char*>(data),
            static_cast<std::size_t>(size)};
}

using ErrorBytesGetter = void (*)(const fdb_payload_v1_error_t*,
                                  const std::uint8_t**,
                                  std::uint64_t*);

std::string_view error_bytes(const fdb_payload_v1_error_t* error,
                             ErrorBytesGetter getter) {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    getter(error, &data, &size);
    return bytes_view(data, size);
}

Error sample_error(std::uint32_t code) {
    return Error::from_details(code, JsonPointer{}, "diagnostic message",
                               JsonValue::object({}));
}

int test_every_stable_error_symbol() {
    for (const ErrorCase& expected : error_cases) {
        Error value = sample_error(expected.code);
        require(value.code() == expected.code);
        require(value.symbol() == expected.symbol);

        fdb_payload_v1_error_t* handle = make_error(std::move(value));
        require(handle != nullptr);
        require(fdb_payload_v1_error_code(handle) == expected.code);
        require(error_bytes(handle, fdb_payload_v1_error_symbol) ==
                expected.symbol);
        fdb_payload_v1_error_release(handle);
    }
    return EXIT_SUCCESS;
}

int test_owned_error_fields_are_exact_and_canonical() {
    JsonPointer path = JsonPointer{}
                           .append("entries")
                           .append(UINT64_C(0))
                           .append("id/value~");
    JsonValue details = JsonValue::object({
        JsonValue::Member{"z", JsonValue{2.0}},
        JsonValue::Member{"a", JsonValue{"first"}},
    });
    Error value = Error::from_details(FDB_PAYLOAD_E_INVALID_TYPE,
                                      std::move(path),
                                      "entry identifier is invalid",
                                      std::move(details));

    require(value.code() == FDB_PAYLOAD_E_INVALID_TYPE);
    require(value.symbol() == "INVALID_TYPE");
    require(value.path() == "/entries/0/id~1value~0");
    require(value.message() == "entry identifier is invalid");
    require(value.details_json() == "{\"a\":\"first\",\"z\":2}");
    require(std::string_view{value.what()} == value.message());

    fdb_payload_v1_error_t* handle = make_error(std::move(value));
    require(handle != nullptr);
    require(error_bytes(handle, fdb_payload_v1_error_symbol) ==
            "INVALID_TYPE");
    require(error_bytes(handle, fdb_payload_v1_error_path) ==
            "/entries/0/id~1value~0");
    require(error_bytes(handle, fdb_payload_v1_error_message) ==
            "entry identifier is invalid");
    require(error_bytes(handle, fdb_payload_v1_error_details_json) ==
            "{\"a\":\"first\",\"z\":2}");
    fdb_payload_v1_error_release(handle);
    return EXIT_SUCCESS;
}

int test_mutable_json_pointer_escapes_and_backtracks_exactly() {
    JsonPointerBuilder path;
    const JsonPointerBuilder::Mark root = path.mark();
    path.append("entries");
    path.append(UINT64_C(12));
    const JsonPointerBuilder::Mark entry = path.mark();
    path.append("id/value~");
    require(path.snapshot().value() == "/entries/12/id~1value~0");

    path.rewind(entry);
    path.append("next");
    require(path.snapshot().value() == "/entries/12/next");

    path.rewind(root);
    require(path.snapshot().value().empty());
    return EXIT_SUCCESS;
}

int test_invalid_message_fails_closed_to_valid_internal_error() {
    Error value = Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE, JsonPointer{}.append("message"),
        std::string("\xED\xA0\x80", 3),
        JsonValue::object({JsonValue::Member{"ignored", JsonValue{true}}}));

    require(value.code() == FDB_PAYLOAD_E_INTERNAL);
    require(value.symbol() == "INTERNAL");
    require(value.path().empty());
    require(value.message() == "Core error diagnostic validation failed");
    require(value.details_json() == "{}");

    const auto serialized_message =
        fastdb::payload::json::jcs_serialize(
            JsonValue{std::string(value.message())});
    require(std::holds_alternative<std::string>(serialized_message));

    fdb_payload_v1_error_t* handle = make_error(std::move(value));
    require(handle != nullptr);
    require(fdb_payload_v1_error_code(handle) == FDB_PAYLOAD_E_INTERNAL);
    require(error_bytes(handle, fdb_payload_v1_error_symbol) == "INTERNAL");
    require(error_bytes(handle, fdb_payload_v1_error_path).empty());
    require(error_bytes(handle, fdb_payload_v1_error_message) ==
            "Core error diagnostic validation failed");
    require(error_bytes(handle, fdb_payload_v1_error_details_json) == "{}");
    fdb_payload_v1_error_release(handle);
    return EXIT_SUCCESS;
}

int test_jcs_failures_have_stable_codes_and_bad_details_fail_closed() {
    constexpr struct {
        JcsFailure failure;
        std::uint32_t code;
    } mappings[] = {
        {JcsFailure::non_finite_number, FDB_PAYLOAD_E_INVALID_NUMBER},
        {JcsFailure::invalid_utf8, FDB_PAYLOAD_E_INVALID_JSON},
        {JcsFailure::duplicate_member, FDB_PAYLOAD_E_DUPLICATE_KEY},
    };

    for (const auto& mapping : mappings) {
        Error value = Error::from_jcs_failure(
            mapping.failure, JsonPointer{}, "JCS failure",
            JsonValue::object({JsonValue::Member{"kind", JsonValue{"jcs"}}}));
        require(value.code() == mapping.code);
        require(value.details_json() == "{\"kind\":\"jcs\"}");
    }

    const std::vector<JsonValue> invalid_details = {
        JsonValue{std::numeric_limits<double>::infinity()},
        JsonValue{std::string("\xED\xA0\x80", 3)},
        JsonValue::object({
            JsonValue::Member{"x", JsonValue{1.0}},
            JsonValue::Member{"x", JsonValue{2.0}},
        }),
    };
    for (const JsonValue& details : invalid_details) {
        Error value = Error::from_details(FDB_PAYLOAD_E_INVALID_TYPE,
                                          JsonPointer{}, "bad details",
                                          details);
        require(value.code() == FDB_PAYLOAD_E_INTERNAL);
        require(value.symbol() == "INTERNAL");
        require(value.path().empty());
        require(value.details_json() == "{}");
    }
    return EXIT_SUCCESS;
}

int test_result_contains_exactly_one_branch() {
    Result<std::string> success = Result<std::string>::success("value");
    require(success.has_value());
    require(success.value() == "value");

    Result<std::string> failure =
        Result<std::string>::failure(sample_error(FDB_PAYLOAD_E_NOT_FOUND));
    require(!failure.has_value());
    require(failure.error().code() == FDB_PAYLOAD_E_NOT_FOUND);

    Result<void> void_success = Result<void>::success();
    require(void_success.has_value());

    Result<void> void_failure =
        Result<void>::failure(sample_error(FDB_PAYLOAD_E_INVALID_ARGUMENT));
    require(!void_failure.has_value());
    require(void_failure.error().code() == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    return EXIT_SUCCESS;
}

int test_blob_lifetime_is_atomic_and_null_safe() {
    fdb_payload_v1_blob_t* blob =
        make_blob({UINT8_C(0x00), UINT8_C(0x7f), UINT8_C(0x80),
                   UINT8_C(0xff)});
    require(blob != nullptr);
    require(fdb_payload_v1_blob_size(blob) == UINT64_C(4));
    const std::uint8_t* const original = fdb_payload_v1_blob_data(blob);
    require(original != nullptr);
    require(original[0] == UINT8_C(0x00));
    require(original[3] == UINT8_C(0xff));

    std::atomic<bool> intact{true};
    std::vector<std::thread> workers;
    for (std::uint32_t worker = UINT32_C(0); worker < UINT32_C(8); ++worker) {
        workers.emplace_back([blob, original, &intact]() {
            for (std::uint32_t iteration = UINT32_C(0);
                 iteration < UINT32_C(20000); ++iteration) {
                fdb_payload_v1_blob_retain(blob);
                if (fdb_payload_v1_blob_size(blob) != UINT64_C(4) ||
                    fdb_payload_v1_blob_data(blob) != original ||
                    original[2] != UINT8_C(0x80)) {
                    intact.store(false, std::memory_order_relaxed);
                }
                fdb_payload_v1_blob_release(blob);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(intact.load(std::memory_order_relaxed));
    fdb_payload_v1_blob_release(blob);

    fdb_payload_v1_blob_t* empty = make_blob({});
    require(empty != nullptr);
    require(fdb_payload_v1_blob_data(empty) == nullptr);
    require(fdb_payload_v1_blob_size(empty) == UINT64_C(0));
    fdb_payload_v1_blob_release(empty);

    require(fdb_payload_v1_blob_data(nullptr) == nullptr);
    require(fdb_payload_v1_blob_size(nullptr) == UINT64_C(0));
    fdb_payload_v1_blob_retain(nullptr);
    fdb_payload_v1_blob_release(nullptr);
    return EXIT_SUCCESS;
}

int test_error_lifetime_is_atomic_and_null_safe() {
    fdb_payload_v1_error_t* error =
        make_error(sample_error(FDB_PAYLOAD_E_PROFILE_VIOLATION));
    require(error != nullptr);

    std::atomic<bool> intact{true};
    std::vector<std::thread> workers;
    for (std::uint32_t worker = UINT32_C(0); worker < UINT32_C(8); ++worker) {
        workers.emplace_back([error, &intact]() {
            for (std::uint32_t iteration = UINT32_C(0);
                 iteration < UINT32_C(20000); ++iteration) {
                fdb_payload_v1_error_retain(error);
                if (fdb_payload_v1_error_code(error) !=
                        FDB_PAYLOAD_E_PROFILE_VIOLATION ||
                    error_bytes(error, fdb_payload_v1_error_symbol) !=
                        "PROFILE_VIOLATION") {
                    intact.store(false, std::memory_order_relaxed);
                }
                fdb_payload_v1_error_release(error);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(intact.load(std::memory_order_relaxed));
    fdb_payload_v1_error_release(error);

    const std::uint8_t* data = reinterpret_cast<const std::uint8_t*>(1);
    std::uint64_t size = UINT64_C(99);
    fdb_payload_v1_error_symbol(nullptr, &data, &size);
    require(data == nullptr);
    require(size == UINT64_C(0));
    fdb_payload_v1_error_path(nullptr, &data, &size);
    require(data == nullptr);
    require(size == UINT64_C(0));
    fdb_payload_v1_error_message(nullptr, &data, &size);
    require(data == nullptr);
    require(size == UINT64_C(0));
    fdb_payload_v1_error_details_json(nullptr, &data, &size);
    require(data == nullptr);
    require(size == UINT64_C(0));
    fdb_payload_v1_error_symbol(nullptr, nullptr, nullptr);
    require(fdb_payload_v1_error_code(nullptr) == UINT32_C(0));
    fdb_payload_v1_error_retain(nullptr);
    fdb_payload_v1_error_release(nullptr);
    return EXIT_SUCCESS;
}

int test_max_minus_one_retain_saturates_and_release_is_a_noop() {
    constexpr std::uint64_t immortal =
        std::numeric_limits<std::uint64_t>::max();
    saturation_destroy_calls.store(UINT32_C(0), std::memory_order_relaxed);
    fdb_payload_v1_error_t probe{
        immortal - UINT64_C(1), FDB_PAYLOAD_E_INTERNAL, {}, {}, {}, {},
        record_saturation_destroy,
    };

    fdb_payload_v1_error_retain(&probe);
    require(probe.references.load(std::memory_order_relaxed) == immortal);

    fdb_payload_v1_error_release(&probe);
    require(probe.references.load(std::memory_order_relaxed) == immortal);
    require(saturation_destroy_calls.load(std::memory_order_relaxed) ==
            UINT32_C(0));

    fdb_payload_v1_error_retain(&probe);
    require(probe.references.load(std::memory_order_relaxed) == immortal);
    fdb_payload_v1_error_release(&probe);
    require(probe.references.load(std::memory_order_relaxed) == immortal);
    require(saturation_destroy_calls.load(std::memory_order_relaxed) ==
            UINT32_C(0));
    return EXIT_SUCCESS;
}

int test_exception_barrier_maps_all_categories() {
    fdb_payload_v1_error_t* error = nullptr;
    fdb_payload_v1_status_t status = guard_status(
        &error, []() -> Result<void> { return Result<void>::success(); });
    require(status == UINT32_C(0));
    require(error == nullptr);

    status = guard_status(&error, []() -> Result<void> {
        return Result<void>::failure(
            sample_error(FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE));
    });
    require(status == FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    require(error != nullptr);
    require(fdb_payload_v1_error_code(error) == status);
    fdb_payload_v1_error_release(error);

    status = guard_status(&error, []() -> Result<void> {
        throw sample_error(FDB_PAYLOAD_E_DUPLICATE_ID);
    });
    require(status == FDB_PAYLOAD_E_DUPLICATE_ID);
    require(error != nullptr);
    require(fdb_payload_v1_error_code(error) == status);
    fdb_payload_v1_error_release(error);

    status = guard_status(&error, []() -> Result<void> {
        throw std::runtime_error("unexpected runtime failure");
    });
    require(status == FDB_PAYLOAD_E_INTERNAL);
    require(error != nullptr);
    require(fdb_payload_v1_error_code(error) == status);
    fdb_payload_v1_error_release(error);

    status = guard_status(&error, []() -> Result<void> { throw 7; });
    require(status == FDB_PAYLOAD_E_INTERNAL);
    require(error != nullptr);
    require(fdb_payload_v1_error_code(error) == status);
    fdb_payload_v1_error_release(error);

    fail_allocations.store(true, std::memory_order_relaxed);
    status = guard_status(&error, []() -> Result<void> {
        void* const allocation = ::operator new(sizeof(std::uint8_t));
        ::operator delete(allocation);
        return Result<void>::success();
    });
    const std::uint32_t emergency_code =
        fdb_payload_v1_error_code(error);
    const std::string_view emergency_symbol =
        error_bytes(error, fdb_payload_v1_error_symbol);
    const std::string_view emergency_path =
        error_bytes(error, fdb_payload_v1_error_path);
    const std::string_view emergency_message =
        error_bytes(error, fdb_payload_v1_error_message);
    const std::string_view emergency_details =
        error_bytes(error, fdb_payload_v1_error_details_json);
    fdb_payload_v1_error_retain(error);
    fdb_payload_v1_error_release(error);
    fail_allocations.store(false, std::memory_order_relaxed);

    require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(error != nullptr);
    require(emergency_code == status);
    require(emergency_symbol == "ALLOCATION_FAILED");
    require(emergency_path.empty());
    require(!emergency_message.empty());
    require(emergency_details == "{}");

    for (std::uint32_t iteration = UINT32_C(0);
         iteration < UINT32_C(10000); ++iteration) {
        fdb_payload_v1_error_release(error);
    }
    require(fdb_payload_v1_error_code(error) ==
            FDB_PAYLOAD_E_ALLOCATION_FAILED);
    fdb_payload_v1_error_release(error);

    status = guard_status(nullptr, []() -> Result<void> {
        return Result<void>::success();
    });
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    require(test_every_stable_error_symbol() == EXIT_SUCCESS);
    require(test_max_minus_one_retain_saturates_and_release_is_a_noop() ==
            EXIT_SUCCESS);
    require(test_owned_error_fields_are_exact_and_canonical() == EXIT_SUCCESS);
    require(test_mutable_json_pointer_escapes_and_backtracks_exactly() ==
            EXIT_SUCCESS);
    require(test_invalid_message_fails_closed_to_valid_internal_error() ==
            EXIT_SUCCESS);
    require(test_jcs_failures_have_stable_codes_and_bad_details_fail_closed() ==
            EXIT_SUCCESS);
    require(test_result_contains_exactly_one_branch() == EXIT_SUCCESS);
    require(test_blob_lifetime_is_atomic_and_null_safe() == EXIT_SUCCESS);
    require(test_error_lifetime_is_atomic_and_null_safe() == EXIT_SUCCESS);
    require(test_exception_barrier_maps_all_categories() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
