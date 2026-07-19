#include <fastdb_payload.h>

#include "payload/abi/Handles.hpp"
#include "payload/backing/Backing.hpp"
#include "payload/build/PayloadBuilder.hpp"
#include "payload/error/Error.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/spec/SchemaRepository.hpp"
#include "payload/view/PayloadOwner.hpp"
#include "payload/view/View.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastdb::payload::abi {
namespace {

detail::ByteView byte_view(std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()),
            static_cast<std::uint64_t>(value.size())};
}

struct OwnedErrorHandle final : fdb_payload_v1_error_t {
    explicit OwnedErrorHandle(error::Error value);

    error::Error value;
};

void destroy_owned_error(fdb_payload_v1_error_t* handle) noexcept {
    delete static_cast<OwnedErrorHandle*>(handle);
}

OwnedErrorHandle::OwnedErrorHandle(error::Error initial_value)
    : fdb_payload_v1_error_t(UINT64_C(1), UINT32_C(0), {}, {}, {}, {},
                             destroy_owned_error),
      value(std::move(initial_value)) {
    code = value.code();
    symbol = byte_view(value.symbol());
    path = byte_view(value.path());
    message = byte_view(value.message());
    details_json = byte_view(value.details_json());
}

template <typename Result, typename Operation>
Result guard_value(Result fallback, Operation&& operation) noexcept {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
        return fallback;
    } catch (const error::Error&) {
        return fallback;
    } catch (const std::exception&) {
        return fallback;
    } catch (...) {
        return fallback;
    }
}

template <typename Operation>
void guard_void(Operation&& operation) noexcept {
    try {
        std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
    } catch (const error::Error&) {
    } catch (const std::exception&) {
    } catch (...) {
    }
}

void write_error_bytes(const fdb_payload_v1_error_t* error,
                       detail::ByteView fdb_payload_v1_error_t::*member,
                       const std::uint8_t** out_data,
                       std::uint64_t* out_size) noexcept {
    if (out_data != nullptr) {
        *out_data = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = UINT64_C(0);
    }
    if (error == nullptr) {
        return;
    }
    const detail::ByteView value = error->*member;
    if (out_data != nullptr) {
        *out_data = value.data;
    }
    if (out_size != nullptr) {
        *out_size = value.size;
    }
}

error::Error invalid_argument(std::string argument,
                              std::string reason) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_INVALID_ARGUMENT, json::JsonPointer{},
        "Invalid ABI argument",
        json::JsonValue::object({
            json::JsonValue::Member{"argument",
                                    json::JsonValue{std::move(argument)}},
            json::JsonValue::Member{"reason",
                                    json::JsonValue{std::move(reason)}},
        }));
}

error::Error unsupported_prefix(std::string argument,
                                std::uint32_t actual,
                                std::uint32_t minimum) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_UNSUPPORTED_ABI, json::JsonPointer{},
        "Unsupported ABI struct contract",
        json::JsonValue::object({
            json::JsonValue::Member{"argument",
                                    json::JsonValue{std::move(argument)}},
            json::JsonValue::Member{
                "minimum_struct_size",
                json::JsonValue{static_cast<double>(minimum)}},
            json::JsonValue::Member{"reason",
                                    json::JsonValue{"struct_size_too_small"}},
            json::JsonValue::Member{
                "struct_size", json::JsonValue{static_cast<double>(actual)}},
        }));
}

error::Error unsupported_field(std::string argument,
                               std::string field) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_UNSUPPORTED_ABI, json::JsonPointer{},
        "Unsupported ABI struct contract",
        json::JsonValue::object({
            json::JsonValue::Member{"argument",
                                    json::JsonValue{std::move(argument)}},
            json::JsonValue::Member{"field",
                                    json::JsonValue{std::move(field)}},
            json::JsonValue::Member{"reason",
                                    json::JsonValue{"nonzero_v1_field"}},
        }));
}

error::Error resource_limit(std::uint64_t actual, std::uint64_t limit) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, json::JsonPointer{},
        "JSON source exceeds configured limit",
        json::JsonValue::object({
            json::JsonValue::Member{"actual",
                                    json::JsonValue{std::to_string(actual)}},
            json::JsonValue::Member{"kind",
                                    json::JsonValue{"source_bytes"}},
            json::JsonValue::Member{"limit",
                                    json::JsonValue{std::to_string(limit)}},
        }));
}

error::Error missing_id(std::string_view id, std::string kind) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_NOT_FOUND, json::JsonPointer{},
        "Payload ID was not found",
        json::JsonValue::object({
            json::JsonValue::Member{"id", json::JsonValue{std::string(id)}},
            json::JsonValue::Member{"kind",
                                    json::JsonValue{std::move(kind)}},
            json::JsonValue::Member{"reason",
                                    json::JsonValue{"not_found"}},
        }));
}

error::Error index_out_of_range(std::string collection,
                                std::uint32_t index,
                                std::uint32_t count) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE, json::JsonPointer{},
        "Payload index is out of range",
        json::JsonValue::object({
            json::JsonValue::Member{"collection",
                                    json::JsonValue{std::move(collection)}},
            json::JsonValue::Member{"count",
                                    json::JsonValue{static_cast<double>(count)}},
            json::JsonValue::Member{"index",
                                    json::JsonValue{static_cast<double>(index)}},
            json::JsonValue::Member{"reason",
                                    json::JsonValue{"index_out_of_range"}},
        }));
}

error::Result<void> failure(error::Error value) {
    return error::Result<void>::failure(std::move(value));
}

fdb_payload_v1_compile_options_t default_options() noexcept {
    return {
        FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE,
        UINT32_C(0),
        UINT64_C(16777216),
        UINT64_C(1000000),
        UINT32_C(128),
        UINT32_C(65536),
        UINT32_C(65536),
        UINT32_C(65536),
        UINT64_C(1000000),
        {UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)},
    };
}

error::Result<spec::CompileLimits> compile_limits(
    const fdb_payload_v1_compile_options_t* supplied) {
    const fdb_payload_v1_compile_options_t defaults = default_options();
    if (supplied == nullptr) {
        return error::Result<spec::CompileLimits>::success(
            spec::CompileLimits{});
    }
    const std::uint32_t struct_size = supplied->struct_size;
    if (struct_size < FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE) {
        return error::Result<spec::CompileLimits>::failure(
            unsupported_prefix("options", struct_size,
                               FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE));
    }
    if (supplied->flags != UINT32_C(0)) {
        return error::Result<spec::CompileLimits>::failure(
            unsupported_field("options", "flags"));
    }
    for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4); ++index) {
        if (supplied->reserved[index] != UINT64_C(0)) {
            return error::Result<spec::CompileLimits>::failure(
                unsupported_field("options",
                                  "reserved[" + std::to_string(index) + "]"));
        }
    }

    spec::CompileLimits limits;
    limits.json.max_source_bytes =
        supplied->max_source_bytes == UINT64_C(0)
            ? defaults.max_source_bytes
            : supplied->max_source_bytes;
    limits.json.max_json_values =
        supplied->max_json_values == UINT64_C(0)
            ? defaults.max_json_values
            : supplied->max_json_values;
    limits.json.max_nesting_depth =
        supplied->max_nesting_depth == UINT32_C(0)
            ? defaults.max_nesting_depth
            : supplied->max_nesting_depth;
    limits.source.max_entries =
        supplied->max_entries == UINT32_C(0) ? defaults.max_entries
                                             : supplied->max_entries;
    limits.source.max_components =
        supplied->max_components == UINT32_C(0)
            ? defaults.max_components
            : supplied->max_components;
    limits.source.max_fields_per_component =
        supplied->max_fields_per_component == UINT32_C(0)
            ? defaults.max_fields_per_component
            : supplied->max_fields_per_component;
    limits.source.max_total_fields =
        supplied->max_total_fields == UINT64_C(0)
            ? defaults.max_total_fields
            : supplied->max_total_fields;
    return error::Result<spec::CompileLimits>::success(limits);
}

bool is_valid_id_byte(std::uint8_t value, bool first) noexcept {
    const bool uppercase = value >= static_cast<std::uint8_t>('A') &&
                           value <= static_cast<std::uint8_t>('Z');
    const bool lowercase = value >= static_cast<std::uint8_t>('a') &&
                           value <= static_cast<std::uint8_t>('z');
    const bool underscore = value == static_cast<std::uint8_t>('_');
    const bool digit = value >= static_cast<std::uint8_t>('0') &&
                       value <= static_cast<std::uint8_t>('9');
    return uppercase || lowercase || underscore || (!first && digit);
}

error::Result<std::string_view> validated_id(const std::uint8_t* id,
                                             std::uint64_t id_size,
                                             std::string argument) {
    if (id == nullptr && id_size != UINT64_C(0)) {
        return error::Result<std::string_view>::failure(
            invalid_argument(std::move(argument), "null_data"));
    }
    if (id_size == UINT64_C(0)) {
        return error::Result<std::string_view>::failure(
            invalid_argument(std::move(argument), "empty_id"));
    }
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (id_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            return error::Result<std::string_view>::failure(
                invalid_argument(std::move(argument),
                                 "native_size_overflow"));
        }
    }
    const std::size_t native_size = static_cast<std::size_t>(id_size);
    for (std::size_t index = 0; index < native_size; ++index) {
        if (!is_valid_id_byte(id[index], index == 0U)) {
            return error::Result<std::string_view>::failure(
                invalid_argument(std::move(argument), "invalid_id"));
        }
    }
    return error::Result<std::string_view>::success(std::string_view{
        reinterpret_cast<const char*>(id), native_size});
}

std::vector<std::uint8_t> copy_bytes(std::string_view value) {
    std::vector<std::uint8_t> result(value.size());
    if (!value.empty()) {
        std::memcpy(result.data(), value.data(), value.size());
    }
    return result;
}

error::Result<void> publish_blob(std::string_view value,
                                 fdb_payload_v1_blob_t** out_blob) {
    fdb_payload_v1_blob_t* const blob = make_blob(copy_bytes(value));
    *out_blob = blob;
    return error::Result<void>::success();
}

std::uint32_t public_profile(spec::Profile profile) noexcept {
    return profile == spec::Profile::record_v1
               ? FDB_PAYLOAD_PROFILE_RECORD_V1
               : FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1;
}

void clear_digest(std::uint8_t* digest) noexcept {
    if (digest != nullptr) {
        std::memset(digest, 0,
                    static_cast<std::size_t>(FDB_PAYLOAD_V1_SHA256_SIZE));
    }
}

void clear_capabilities_prefix(fdb_payload_v1_capabilities_t* value,
                               std::uint32_t declared_size) noexcept {
    if (value == nullptr || declared_size <= UINT32_C(4)) {
        return;
    }
    const std::uint32_t writable =
        std::min(declared_size, FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
    std::memset(reinterpret_cast<std::uint8_t*>(value) + sizeof(std::uint32_t),
                0, static_cast<std::size_t>(writable - UINT32_C(4)));
}

template <typename Value>
void clear_output_prefix(Value* value,
                         std::uint32_t declared_size,
                         std::uint32_t current_size) noexcept {
    if (value == nullptr || declared_size <= UINT32_C(4)) {
        return;
    }
    const std::uint32_t writable = std::min(declared_size, current_size);
    std::memset(reinterpret_cast<std::uint8_t*>(value) + sizeof(std::uint32_t),
                0, static_cast<std::size_t>(writable - UINT32_C(4)));
}

fdb_payload_v1_builder_options_t default_builder_options() noexcept {
    const build::BuilderLimits limits = build::default_builder_limits();
    return {
        FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE,
        UINT32_C(0),
        limits.max_value_nodes,
        limits.max_list_elements,
        limits.max_text_bytes,
        limits.max_opaque_bytes,
        limits.max_nesting_depth,
        limits.max_total_builder_bytes,
        {UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)},
    };
}

error::Result<build::BuilderLimits> builder_limits(
    const fdb_payload_v1_builder_options_t* supplied) {
    const fdb_payload_v1_builder_options_t defaults =
        default_builder_options();
    if (supplied == nullptr) {
        return error::Result<build::BuilderLimits>::success(
            build::default_builder_limits());
    }
    const std::uint32_t struct_size = supplied->struct_size;
    if (struct_size < FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE) {
        return error::Result<build::BuilderLimits>::failure(
            unsupported_prefix("options", struct_size,
                               FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE));
    }
    if (supplied->flags != UINT32_C(0)) {
        return error::Result<build::BuilderLimits>::failure(
            unsupported_field("options", "flags"));
    }
    for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4); ++index) {
        if (supplied->reserved[index] != UINT64_C(0)) {
            return error::Result<build::BuilderLimits>::failure(
                unsupported_field(
                    "options",
                    "reserved[" + std::to_string(index) + "]"));
        }
    }
    return error::Result<build::BuilderLimits>::success(
        build::BuilderLimits{
            supplied->max_value_nodes == UINT64_C(0)
                ? defaults.max_value_nodes
                : supplied->max_value_nodes,
            supplied->max_list_elements == UINT64_C(0)
                ? defaults.max_list_elements
                : supplied->max_list_elements,
            supplied->max_text_bytes == UINT64_C(0)
                ? defaults.max_text_bytes
                : supplied->max_text_bytes,
            supplied->max_opaque_bytes == UINT64_C(0)
                ? defaults.max_opaque_bytes
                : supplied->max_opaque_bytes,
            supplied->max_nesting_depth == UINT64_C(0)
                ? defaults.max_nesting_depth
                : supplied->max_nesting_depth,
            supplied->max_total_builder_bytes == UINT64_C(0)
                ? defaults.max_total_builder_bytes
                : supplied->max_total_builder_bytes,
        });
}

fdb_payload_v1_open_options_t default_public_open_options() noexcept {
    const view::OpenOptions options = view::default_open_options();
    return {
        FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE,
        options.validate_text_eager ? FDB_PAYLOAD_OPEN_VALIDATE_TEXT_EAGER
                                    : UINT32_C(0),
        options.max_total_bytes,
        options.max_regions,
        options.max_entries,
        options.max_components,
        options.max_nesting_depth,
        options.max_list_elements,
        options.max_graph_objects,
        options.max_string_bytes,
        options.max_validation_work,
        {UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)},
    };
}

error::Result<view::OpenOptions> open_options(
    const fdb_payload_v1_open_options_t* supplied) {
    const fdb_payload_v1_open_options_t defaults =
        default_public_open_options();
    if (supplied == nullptr) {
        return error::Result<view::OpenOptions>::success(
            view::default_open_options());
    }
    const std::uint32_t struct_size = supplied->struct_size;
    if (struct_size < FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE) {
        return error::Result<view::OpenOptions>::failure(
            unsupported_prefix("options", struct_size,
                               FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE));
    }
    if ((supplied->flags & ~FDB_PAYLOAD_OPEN_VALIDATE_TEXT_EAGER) !=
        UINT32_C(0)) {
        return error::Result<view::OpenOptions>::failure(
            unsupported_field("options", "flags"));
    }
    for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4); ++index) {
        if (supplied->reserved[index] != UINT64_C(0)) {
            return error::Result<view::OpenOptions>::failure(
                unsupported_field(
                    "options",
                    "reserved[" + std::to_string(index) + "]"));
        }
    }
    return error::Result<view::OpenOptions>::success(view::OpenOptions{
        (supplied->flags & FDB_PAYLOAD_OPEN_VALIDATE_TEXT_EAGER) !=
            UINT32_C(0),
        supplied->max_total_bytes == UINT64_C(0)
            ? defaults.max_total_bytes
            : supplied->max_total_bytes,
        supplied->max_regions == UINT64_C(0) ? defaults.max_regions
                                              : supplied->max_regions,
        supplied->max_entries == UINT64_C(0) ? defaults.max_entries
                                              : supplied->max_entries,
        supplied->max_components == UINT64_C(0)
            ? defaults.max_components
            : supplied->max_components,
        supplied->max_nesting_depth == UINT64_C(0)
            ? defaults.max_nesting_depth
            : supplied->max_nesting_depth,
        supplied->max_list_elements == UINT64_C(0)
            ? defaults.max_list_elements
            : supplied->max_list_elements,
        supplied->max_graph_objects == UINT64_C(0)
            ? defaults.max_graph_objects
            : supplied->max_graph_objects,
        supplied->max_string_bytes == UINT64_C(0)
            ? defaults.max_string_bytes
            : supplied->max_string_bytes,
        supplied->max_validation_work == UINT64_C(0)
            ? defaults.max_validation_work
            : supplied->max_validation_work,
    });
}

error::Result<build::FixedRun> fixed_run(
    const fdb_payload_v1_fixed_run_v1_t* supplied) {
    if (supplied == nullptr) {
        return error::Result<build::FixedRun>::failure(
            invalid_argument("run", "null_input"));
    }
    const std::uint32_t minimum =
        static_cast<std::uint32_t>(sizeof(fdb_payload_v1_fixed_run_v1_t));
    if (supplied->struct_size < minimum) {
        return error::Result<build::FixedRun>::failure(
            unsupported_prefix("run", supplied->struct_size, minimum));
    }
    if (supplied->flags != UINT32_C(0)) {
        return error::Result<build::FixedRun>::failure(
            unsupported_field("run", "flags"));
    }
    for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4); ++index) {
        if (supplied->reserved[index] != UINT64_C(0)) {
            return error::Result<build::FixedRun>::failure(
                unsupported_field(
                    "run", "reserved[" + std::to_string(index) + "]"));
        }
    }
    return error::Result<build::FixedRun>::success(build::FixedRun{
        static_cast<const std::uint8_t*>(supplied->data),
        supplied->data_byte_length,
        supplied->count,
        supplied->stride_bytes,
        supplied->validity,
        supplied->validity_byte_length,
        supplied->validity_bit_offset,
    });
}

constexpr std::uint32_t backing_prefix_size() noexcept {
    return static_cast<std::uint32_t>(
        offsetof(fdb_payload_v1_backing_v1_t, release) +
        sizeof(fdb_payload_v1_backing_release_fn));
}

error::Result<backing::Callbacks> backing_callbacks(
    const fdb_payload_v1_backing_v1_t* supplied,
    bool required) {
    if (supplied == nullptr) {
        if (!required) {
            return error::Result<backing::Callbacks>::success(
                backing::Callbacks{});
        }
        return error::Result<backing::Callbacks>::failure(
            invalid_argument("backing", "null_input"));
    }
    const std::uint32_t minimum = backing_prefix_size();
    if (supplied->struct_size < minimum) {
        return error::Result<backing::Callbacks>::failure(
            unsupported_prefix("backing", supplied->struct_size, minimum));
    }
    if (supplied->flags != UINT32_C(0)) {
        return error::Result<backing::Callbacks>::failure(
            unsupported_field("backing", "flags"));
    }
    for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4); ++index) {
        const auto covered_size = static_cast<std::uint32_t>(
            offsetof(fdb_payload_v1_backing_v1_t, reserved) +
            sizeof(supplied->reserved[0]) *
                static_cast<std::size_t>(index + UINT32_C(1)));
        if (supplied->struct_size >= covered_size) {
            if (supplied->reserved[index] != UINT64_C(0)) {
                return error::Result<backing::Callbacks>::failure(
                    unsupported_field(
                        "backing",
                        "reserved[" + std::to_string(index) + "]"));
            }
        }
    }
    return error::Result<backing::Callbacks>::success(backing::Callbacks{
        supplied->context,
        supplied->reserve,
        supplied->write,
        supplied->commit,
        supplied->rollback,
        supplied->retain,
        supplied->release,
    });
}

error::Result<void> require_builder(
    fdb_payload_v1_builder_t* builder) {
    if (builder == nullptr || builder->value == nullptr) {
        return failure(invalid_argument("builder", "null_handle"));
    }
    return error::Result<void>::success();
}

template <typename Operation>
error::Result<void> builder_call(fdb_payload_v1_builder_t* builder,
                                 Operation&& operation) {
    auto valid = require_builder(builder);
    if (!valid.has_value()) {
        return valid;
    }
    return std::forward<Operation>(operation)(*builder->value);
}

error::Result<std::string_view> borrowed_string(
    const std::uint8_t* data,
    std::uint64_t size,
    std::string argument) {
    if (data == nullptr && size != UINT64_C(0)) {
        return error::Result<std::string_view>::failure(
            invalid_argument(std::move(argument), "null_data"));
    }
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (size > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max())) {
            return error::Result<std::string_view>::failure(
                invalid_argument(std::move(argument),
                                 "native_size_overflow"));
        }
    }
    if (data == nullptr) {
        return error::Result<std::string_view>::success(std::string_view{});
    }
    return error::Result<std::string_view>::success(std::string_view{
        reinterpret_cast<const char*>(data), static_cast<std::size_t>(size)});
}

error::Error opened_payload_report_error() {
    return error::Error::from_details(
        FDB_PAYLOAD_E_PLAN_STATE, json::JsonPointer{},
        "Opened payload has no execution report",
        json::JsonValue::object({json::JsonValue::Member{
            "reason", json::JsonValue{
                          "opened_payload_has_no_execution_report"}}}));
}

}  // namespace

fdb_payload_v1_blob_t* make_blob(std::vector<std::uint8_t> bytes) {
    return new fdb_payload_v1_blob_t(std::move(bytes));
}

fdb_payload_v1_error_t* allocation_error() noexcept {
    static constexpr char symbol[] = "ALLOCATION_FAILED";
    static constexpr char path[] = "";
    static constexpr char message[] = "FastDB payload allocation failed";
    static constexpr char details[] = "{}";
    static fdb_payload_v1_error_t emergency{
        std::numeric_limits<std::uint64_t>::max(),
        FDB_PAYLOAD_E_ALLOCATION_FAILED,
        byte_view(symbol),
        byte_view(path),
        byte_view(message),
        byte_view(details),
        nullptr,
    };
    return &emergency;
}

fdb_payload_v1_error_t* make_error(error::Error value) noexcept {
    try {
        return new OwnedErrorHandle(std::move(value));
    } catch (const std::bad_alloc&) {
        return allocation_error();
    } catch (const error::Error&) {
        return allocation_error();
    } catch (const std::exception&) {
        return allocation_error();
    } catch (...) {
        return allocation_error();
    }
}

fdb_payload_v1_status_t publish_error(
    const error::Error& value,
    fdb_payload_v1_error_t** out_error) noexcept {
    if (out_error == nullptr) {
        return FDB_PAYLOAD_E_INVALID_ARGUMENT;
    }
    try {
        *out_error = make_error(value);
    } catch (const std::bad_alloc&) {
        *out_error = allocation_error();
    } catch (const error::Error&) {
        *out_error = allocation_error();
    } catch (const std::exception&) {
        *out_error = allocation_error();
    } catch (...) {
        *out_error = allocation_error();
    }
    return (*out_error)->code;
}

fdb_payload_v1_status_t publish_internal_error(
    const char* message,
    fdb_payload_v1_error_t** out_error) noexcept {
    if (out_error == nullptr) {
        return FDB_PAYLOAD_E_INVALID_ARGUMENT;
    }
    try {
        const error::Error internal = error::Error::from_details(
            FDB_PAYLOAD_E_INTERNAL, json::JsonPointer{}, message,
            json::JsonValue::object({}));
        return publish_error(internal, out_error);
    } catch (const std::bad_alloc&) {
        *out_error = allocation_error();
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    } catch (const error::Error& known) {
        return publish_error(known, out_error);
    } catch (const std::exception&) {
        *out_error = allocation_error();
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    } catch (...) {
        *out_error = allocation_error();
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }
}

}  // namespace fastdb::payload::abi

extern "C" uint32_t fdb_payload_v1_abi_version(void) {
    return fastdb::payload::abi::guard_value(
        UINT32_C(0), []() { return FDB_PAYLOAD_V1_ABI_VERSION; });
}

extern "C" void fdb_payload_v1_compile_options_init(
    fdb_payload_v1_compile_options_t* options) {
    fastdb::payload::abi::guard_void([options]() {
        if (options == nullptr) {
            return;
        }
        *options = fastdb::payload::abi::default_options();
    });
}

extern "C" void fdb_payload_v1_capabilities_init(
    fdb_payload_v1_capabilities_t* capabilities) {
    fastdb::payload::abi::guard_void([capabilities]() {
        if (capabilities == nullptr) {
            return;
        }
        const fdb_payload_v1_capabilities_t initialized{
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE,
            UINT32_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED,
            UINT32_C(0),
            {UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)},
        };
        *capabilities = initialized;
    });
}

extern "C" void fdb_payload_v1_builder_options_init(
    fdb_payload_v1_builder_options_t* options) {
    fastdb::payload::abi::guard_void([options]() {
        if (options != nullptr) {
            *options = fastdb::payload::abi::default_builder_options();
        }
    });
}

extern "C" void fdb_payload_v1_fixed_run_init(
    fdb_payload_v1_fixed_run_v1_t* run) {
    fastdb::payload::abi::guard_void([run]() {
        if (run == nullptr) {
            return;
        }
        run->struct_size =
            static_cast<std::uint32_t>(sizeof(fdb_payload_v1_fixed_run_v1_t));
        run->flags = UINT32_C(0);
        run->data = nullptr;
        run->data_byte_length = UINT64_C(0);
        run->count = UINT64_C(0);
        run->stride_bytes = UINT64_C(0);
        run->validity = nullptr;
        run->validity_byte_length = UINT64_C(0);
        run->validity_bit_offset = UINT64_C(0);
        for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4); ++index) {
            run->reserved[index] = UINT64_C(0);
        }
    });
}

extern "C" void fdb_payload_v1_open_options_init(
    fdb_payload_v1_open_options_t* options) {
    fastdb::payload::abi::guard_void([options]() {
        if (options != nullptr) {
            *options = fastdb::payload::abi::default_public_open_options();
        }
    });
}

extern "C" void fdb_payload_v1_plan_info_init(
    fdb_payload_v1_plan_info_t* info) {
    fastdb::payload::abi::guard_void([info]() {
        if (info == nullptr) {
            return;
        }
        const fdb_payload_v1_plan_info_t initialized{
            FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE,
            UINT32_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT32_C(0),
            FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED,
            {UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)},
        };
        *info = initialized;
    });
}

extern "C" void fdb_payload_v1_execution_report_init(
    fdb_payload_v1_execution_report_t* report) {
    fastdb::payload::abi::guard_void([report]() {
        if (report == nullptr) {
            return;
        }
        const fdb_payload_v1_execution_report_t initialized{
            FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE,
            UINT32_C(0),
            FDB_PAYLOAD_FALLBACK_NONE,
            UINT32_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            {UINT64_C(0), UINT64_C(0)},
        };
        *report = initialized;
    });
}

extern "C" void fdb_payload_v1_backing_init(
    fdb_payload_v1_backing_v1_t* backing) {
    fastdb::payload::abi::guard_void([backing]() {
        if (backing == nullptr) {
            return;
        }
        backing->struct_size =
            static_cast<std::uint32_t>(sizeof(fdb_payload_v1_backing_v1_t));
        backing->flags = UINT32_C(0);
        backing->context = nullptr;
        backing->reserve = nullptr;
        backing->write = nullptr;
        backing->commit = nullptr;
        backing->rollback = nullptr;
        backing->retain = nullptr;
        backing->release = nullptr;
        for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4); ++index) {
            backing->reserved[index] = UINT64_C(0);
        }
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_create(
    const fdb_payload_v1_spec_t* spec,
    const fdb_payload_v1_builder_options_t* options,
    fdb_payload_v1_builder_t** out_builder,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_builder != nullptr) {
            *out_builder = nullptr;
        }
        if (out_builder == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_builder", "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "spec", "null_handle"));
        }
        auto limits = fastdb::payload::abi::builder_limits(options);
        if (!limits.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(limits).error());
        }
        auto created = fastdb::payload::build::PayloadBuilder::create(
            spec->compiled, limits.value());
        if (!created.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(created).error());
        }
        auto value = std::make_unique<fastdb::payload::build::PayloadBuilder>(
            std::move(created).value());
        *out_builder = new fdb_payload_v1_builder_t(std::move(value));
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" void fdb_payload_v1_builder_release(
    fdb_payload_v1_builder_t* builder) {
    fastdb::payload::abi::guard_void([builder]() { delete builder; });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_entry_begin(
    fdb_payload_v1_builder_t* builder,
    uint32_t entry_index,
    uint64_t value_count,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& value) {
                return value.begin_entry(entry_index, value_count);
            });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_null(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [](auto& value) { return value.push_null(); });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_bool(
    fdb_payload_v1_builder_t* builder,
    uint8_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& target) { return target.push_bool(value); });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_u8(
    fdb_payload_v1_builder_t* builder,
    uint8_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& target) { return target.push_u8(value); });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_u16(
    fdb_payload_v1_builder_t* builder,
    uint16_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& target) { return target.push_u16(value); });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_u32(
    fdb_payload_v1_builder_t* builder,
    uint32_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& target) { return target.push_u32(value); });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_i32(
    fdb_payload_v1_builder_t* builder,
    int32_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& target) { return target.push_i32(value); });
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_builder_value_u8n_f64_bits(
    fdb_payload_v1_builder_t* builder,
    uint64_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder,
            [=](auto& target) { return target.push_u8n_bits(value); });
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_builder_value_u16n_f64_bits(
    fdb_payload_v1_builder_t* builder,
    uint64_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder,
            [=](auto& target) { return target.push_u16n_bits(value); });
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_builder_value_f32_bits(
    fdb_payload_v1_builder_t* builder,
    uint32_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder,
            [=](auto& target) { return target.push_f32_bits(value); });
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_builder_value_f64_bits(
    fdb_payload_v1_builder_t* builder,
    uint64_t value,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder,
            [=](auto& target) { return target.push_f64_bits(value); });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_str(
    fdb_payload_v1_builder_t* builder,
    const uint8_t* value,
    uint64_t value_size,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        auto valid_builder = fastdb::payload::abi::require_builder(builder);
        if (!valid_builder.has_value()) {
            return valid_builder;
        }
        auto span = fastdb::payload::abi::borrowed_string(
            value, value_size, "value");
        if (!span.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(span).error());
        }
        return builder->value->push_str(span.value());
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_wstr(
    fdb_payload_v1_builder_t* builder,
    const uint16_t* value,
    uint64_t value_size,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& target) {
                return target.push_wstr(value, value_size);
            });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_value_bytes(
    fdb_payload_v1_builder_t* builder,
    const uint8_t* value,
    uint64_t value_size,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& target) {
                return target.push_bytes(value, value_size);
            });
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_builder_value_fixed_run(
    fdb_payload_v1_builder_t* builder,
    const fdb_payload_v1_fixed_run_v1_t* run,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        auto valid_builder = fastdb::payload::abi::require_builder(builder);
        if (!valid_builder.has_value()) {
            return valid_builder;
        }
        auto converted = fastdb::payload::abi::fixed_run(run);
        if (!converted.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(converted).error());
        }
        return builder->value->push_fixed_run(converted.value());
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_builder_value_component_begin(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [](auto& value) { return value.begin_component(); });
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_builder_value_list_begin(
    fdb_payload_v1_builder_t* builder,
    uint64_t item_count,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        return fastdb::payload::abi::builder_call(
            builder, [=](auto& value) { return value.begin_list(item_count); });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_builder_freeze(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_plan_t** out_plan,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_plan != nullptr) {
            *out_plan = nullptr;
        }
        if (out_plan == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_plan", "null_output"));
        }
        auto valid = fastdb::payload::abi::require_builder(builder);
        if (!valid.has_value()) {
            return valid;
        }
        void* const storage = ::operator new(sizeof(fdb_payload_v1_plan_t));
        auto created = builder->value->freeze_plan();
        if (!created.has_value()) {
            ::operator delete(storage);
            return fastdb::payload::error::Result<void>::failure(
                std::move(created).error());
        }
        *out_plan = ::new (storage)
            fdb_payload_v1_plan_t(std::move(created).value());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" void fdb_payload_v1_plan_retain(fdb_payload_v1_plan_t* plan) {
    fastdb::payload::abi::guard_void([plan]() {
        fastdb::payload::abi::detail::retain_reference(plan);
    });
}

extern "C" void fdb_payload_v1_plan_release(fdb_payload_v1_plan_t* plan) {
    fastdb::payload::abi::guard_void([plan]() {
        fastdb::payload::abi::detail::release_reference(
            plan, [](fdb_payload_v1_plan_t* value) { delete value; });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_plan_info(
    const fdb_payload_v1_plan_t* plan,
    fdb_payload_v1_plan_info_t* out_info,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_info == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_info", "null_output"));
        }
        const std::uint32_t declared_size = out_info->struct_size;
        bool invalid_flags = false;
        std::optional<std::uint32_t> invalid_reserved;
        if (declared_size >= FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE) {
            invalid_flags = out_info->flags != UINT32_C(0);
            for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4);
                 ++index) {
                if (out_info->reserved[index] != UINT64_C(0)) {
                    invalid_reserved = index;
                    break;
                }
            }
        }
        fastdb::payload::abi::clear_output_prefix(
            out_info, declared_size, FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE);
        if (declared_size < FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_prefix(
                    "info", declared_size,
                    FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE));
        }
        if (invalid_flags) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_field("info", "flags"));
        }
        if (invalid_reserved.has_value()) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_field(
                    "info", "reserved[" +
                                std::to_string(*invalid_reserved) + "]"));
        }
        if (plan == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "plan", "null_handle"));
        }
        const auto& source = plan->value.info();
        out_info->total_bytes = source.total_bytes;
        out_info->region_count = source.region_count;
        out_info->logical_value_count = source.logical_value_count;
        out_info->list_element_count = source.list_element_count;
        out_info->text_bytes = source.text_bytes;
        out_info->opaque_bytes = source.opaque_bytes;
        out_info->validation_work = source.validation_work;
        out_info->max_alignment = source.max_alignment;
        out_info->direct_build_status = source.direct_build_status;
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_plan_execute(
    const fdb_payload_v1_plan_t* plan,
    uint32_t policy,
    const fdb_payload_v1_backing_v1_t* backing,
    fdb_payload_v1_payload_t** out_payload,
    fdb_payload_v1_execution_report_t* out_report,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_payload != nullptr) {
            *out_payload = nullptr;
        }
        std::uint32_t report_size = UINT32_C(0);
        bool invalid_reserved32 = false;
        std::optional<std::uint32_t> invalid_reserved64;
        if (out_report != nullptr) {
            report_size = out_report->struct_size;
            if (report_size >= FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE) {
                invalid_reserved32 =
                    out_report->reserved32 != UINT32_C(0);
                for (std::uint32_t index = UINT32_C(0); index < UINT32_C(2);
                     ++index) {
                    if (out_report->reserved64[index] != UINT64_C(0)) {
                        invalid_reserved64 = index;
                        break;
                    }
                }
            }
            fastdb::payload::abi::clear_output_prefix(
                out_report, report_size,
                FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE);
        }
        if (out_payload == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_payload", "null_output"));
        }
        if (out_report == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_report", "null_output"));
        }
        if (report_size < FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_prefix(
                    "report", report_size,
                    FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE));
        }
        if (invalid_reserved32) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_field(
                    "report", "reserved32"));
        }
        if (invalid_reserved64.has_value()) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_field(
                    "report", "reserved64[" +
                                  std::to_string(*invalid_reserved64) +
                                  "]"));
        }
        if (plan == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "plan", "null_handle"));
        }
        const fastdb::payload::backing::Callbacks* selected = nullptr;
        fastdb::payload::backing::Callbacks copied{};
        if (backing != nullptr) {
            auto callbacks =
                fastdb::payload::abi::backing_callbacks(backing, true);
            if (!callbacks.has_value()) {
                return fastdb::payload::error::Result<void>::failure(
                    std::move(callbacks).error());
            }
            copied = callbacks.value();
            selected = &copied;
        }
        auto executed = plan->value.execute(policy, selected);
        if (!executed.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(executed).error());
        }
        auto* const published = new fdb_payload_v1_payload_t(
            std::move(executed).value());
        const auto& report = published->value.execution_report();
        if (!report.has_value()) {
            delete published;
            return fastdb::payload::error::Result<void>::failure(
                fastdb::payload::abi::opened_payload_report_error());
        }
        out_report->mode = report->mode;
        out_report->fallback_reason = report->fallback_reason;
        out_report->requested_bytes = report->requested_bytes;
        out_report->used_bytes = report->used_bytes;
        out_report->staging_bytes = report->staging_bytes;
        out_report->region_count = report->region_count;
        out_report->backing_capacity = report->backing_capacity;
        *out_payload = published;
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_payload_open_copy(
    const fdb_payload_v1_spec_t* spec,
    const uint8_t* bytes,
    uint64_t byte_count,
    const fdb_payload_v1_open_options_t* options,
    fdb_payload_v1_payload_t** out_payload,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_payload != nullptr) {
            *out_payload = nullptr;
        }
        if (out_payload == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_payload", "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "spec", "null_handle"));
        }
        auto converted = fastdb::payload::abi::open_options(options);
        if (!converted.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(converted).error());
        }
        auto opened = fastdb::payload::view::PayloadOwner::open_copy(
            spec->compiled, bytes, byte_count, converted.value());
        if (!opened.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(opened).error());
        }
        *out_payload = new fdb_payload_v1_payload_t(
            std::move(opened).value());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_payload_open_external(
    const fdb_payload_v1_spec_t* spec,
    const uint8_t* bytes,
    uint64_t byte_count,
    const fdb_payload_v1_backing_v1_t* backing,
    void* owner_token,
    const fdb_payload_v1_open_options_t* options,
    fdb_payload_v1_payload_t** out_payload,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_payload != nullptr) {
            *out_payload = nullptr;
        }
        if (out_payload == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_payload", "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "spec", "null_handle"));
        }
        auto converted_options = fastdb::payload::abi::open_options(options);
        if (!converted_options.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(converted_options).error());
        }
        auto converted_backing =
            fastdb::payload::abi::backing_callbacks(backing, true);
        if (!converted_backing.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(converted_backing).error());
        }
        auto retained = fastdb::payload::backing::RetainedBacking::acquire(
            converted_backing.value(), owner_token, bytes, byte_count);
        if (!retained.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(retained).error());
        }
        auto opened = fastdb::payload::view::PayloadOwner::open_external(
            spec->compiled, bytes, byte_count, std::move(retained).value(),
            converted_options.value());
        if (!opened.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(opened).error());
        }
        *out_payload = new fdb_payload_v1_payload_t(
            std::move(opened).value());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" void fdb_payload_v1_payload_retain(
    fdb_payload_v1_payload_t* payload) {
    fastdb::payload::abi::guard_void([payload]() {
        fastdb::payload::abi::detail::retain_reference(payload);
    });
}

extern "C" void fdb_payload_v1_payload_release(
    fdb_payload_v1_payload_t* payload) {
    fastdb::payload::abi::guard_void([payload]() {
        fastdb::payload::abi::detail::release_reference(
            payload, [](fdb_payload_v1_payload_t* value) { delete value; });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_payload_sha256(
    const fdb_payload_v1_payload_t* payload,
    uint8_t out_digest[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        fastdb::payload::abi::clear_digest(out_digest);
        if (out_digest == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_digest", "null_output"));
        }
        if (payload == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "payload", "null_handle"));
        }
        std::memcpy(out_digest, payload->value.digest().data(),
                    static_cast<std::size_t>(FDB_PAYLOAD_V1_SHA256_SIZE));
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_payload_profile(
    const fdb_payload_v1_payload_t* payload,
    fdb_payload_v1_profile_t* out_profile,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_profile != nullptr) {
            *out_profile = UINT32_C(0);
        }
        if (out_profile == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_profile", "null_output"));
        }
        if (payload == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "payload", "null_handle"));
        }
        *out_profile =
            fastdb::payload::abi::public_profile(payload->value.profile());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_payload_execution_report(
    const fdb_payload_v1_payload_t* payload,
    fdb_payload_v1_execution_report_t* out_report,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_report == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_report", "null_output"));
        }
        const std::uint32_t report_size = out_report->struct_size;
        bool invalid_reserved32 = false;
        std::optional<std::uint32_t> invalid_reserved64;
        if (report_size >= FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE) {
            invalid_reserved32 = out_report->reserved32 != UINT32_C(0);
            for (std::uint32_t index = UINT32_C(0); index < UINT32_C(2);
                 ++index) {
                if (out_report->reserved64[index] != UINT64_C(0)) {
                    invalid_reserved64 = index;
                    break;
                }
            }
        }
        fastdb::payload::abi::clear_output_prefix(
            out_report, report_size,
            FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE);
        if (report_size < FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_prefix(
                    "report", report_size,
                    FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE));
        }
        if (invalid_reserved32) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_field(
                    "report", "reserved32"));
        }
        if (invalid_reserved64.has_value()) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_field(
                    "report", "reserved64[" +
                                  std::to_string(*invalid_reserved64) +
                                  "]"));
        }
        if (payload == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "payload", "null_handle"));
        }
        const auto& report = payload->value.execution_report();
        if (!report.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                fastdb::payload::abi::opened_payload_report_error());
        }
        out_report->mode = report->mode;
        out_report->fallback_reason = report->fallback_reason;
        out_report->requested_bytes = report->requested_bytes;
        out_report->used_bytes = report->used_bytes;
        out_report->staging_bytes = report->staging_bytes;
        out_report->region_count = report->region_count;
        out_report->backing_capacity = report->backing_capacity;
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_payload_binary_blob(
    const fdb_payload_v1_payload_t* payload,
    fdb_payload_v1_blob_t** out_blob,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_blob != nullptr) {
            *out_blob = nullptr;
        }
        if (out_blob == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_blob", "null_output"));
        }
        if (payload == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "payload", "null_handle"));
        }
        auto access = payload->value.acquire();
        if (!access.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(access).error());
        }
        auto bytes = access.value().payload_bytes();
        if (!bytes.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(bytes).error());
        }
        const auto span = bytes.value();
        if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
            if (span.size > static_cast<std::uint64_t>(
                                std::numeric_limits<std::size_t>::max())) {
                return fastdb::payload::abi::failure(
                    fastdb::payload::abi::invalid_argument(
                        "payload_size", "native_size_overflow"));
            }
        }
        std::vector<std::uint8_t> copied(
            static_cast<std::size_t>(span.size));
        if (span.size != UINT64_C(0)) {
            std::memcpy(copied.data(), span.data,
                        static_cast<std::size_t>(span.size));
        }
        *out_blob = fastdb::payload::abi::make_blob(std::move(copied));
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_payload_invalidate(
    fdb_payload_v1_payload_t* payload,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (payload == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "payload", "null_handle"));
        }
        return payload->value.invalidate();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_compile_json(
    const uint8_t* source,
    uint64_t source_size,
    const fdb_payload_v1_compile_options_t* options,
    fdb_payload_v1_spec_t** out_spec,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_spec != nullptr) {
            *out_spec = nullptr;
        }
        if (out_spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_spec",
                                                       "null_output"));
        }
        if (source == nullptr && source_size != UINT64_C(0)) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("source",
                                                       "null_data"));
        }
        auto limits = fastdb::payload::abi::compile_limits(options);
        if (!limits.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(limits).error());
        }
        if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
            if (source_size > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::size_t>::max())) {
                return fastdb::payload::abi::failure(
                    fastdb::payload::abi::invalid_argument(
                        "source_size", "native_size_overflow"));
            }
        }
        if (source_size > limits.value().json.max_source_bytes) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::resource_limit(
                    source_size, limits.value().json.max_source_bytes));
        }
        const std::string_view source_view =
            source == nullptr
                ? std::string_view{}
                : std::string_view{
                      reinterpret_cast<const char*>(source),
                      static_cast<std::size_t>(source_size)};
        auto compiled = fastdb::payload::spec::CompiledSpec::compile(
            source_view, limits.value());
        if (!compiled.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(compiled).error());
        }
        fdb_payload_v1_spec_t* const owned =
            new fdb_payload_v1_spec_t(std::move(compiled).value());
        *out_spec = owned;
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" void fdb_payload_v1_spec_retain(fdb_payload_v1_spec_t* spec) {
    fastdb::payload::abi::guard_void([spec]() {
        fastdb::payload::abi::detail::retain_reference(spec);
    });
}

extern "C" void fdb_payload_v1_spec_release(fdb_payload_v1_spec_t* spec) {
    fastdb::payload::abi::guard_void([spec]() {
        fastdb::payload::abi::detail::release_reference(
            spec, [](fdb_payload_v1_spec_t* value) noexcept { delete value; });
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_canonical_json(
    const fdb_payload_v1_spec_t* spec,
    fdb_payload_v1_blob_t** out_blob,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_blob != nullptr) {
            *out_blob = nullptr;
        }
        if (out_blob == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_blob",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        return fastdb::payload::abi::publish_blob(
            spec->compiled.canonical_bytes(), out_blob);
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_sha256(
    const fdb_payload_v1_spec_t* spec,
    uint8_t out_digest[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        fastdb::payload::abi::clear_digest(out_digest);
        if (out_digest == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_digest",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        const auto& digest = spec->compiled.digest();
        std::memcpy(out_digest, digest.data(), digest.size());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_manifest_json(
    const fdb_payload_v1_spec_t* spec,
    fdb_payload_v1_blob_t** out_blob,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_blob != nullptr) {
            *out_blob = nullptr;
        }
        if (out_blob == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_blob",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        return fastdb::payload::abi::publish_blob(
            spec->compiled.manifest_bytes(), out_blob);
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_profile(
    const fdb_payload_v1_spec_t* spec,
    fdb_payload_v1_profile_t* out_profile,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_profile != nullptr) {
            *out_profile = UINT32_C(0);
        }
        if (out_profile == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_profile",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        *out_profile =
            fastdb::payload::abi::public_profile(spec->compiled.profile());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_capabilities(
    const fdb_payload_v1_spec_t* spec,
    fdb_payload_v1_capabilities_t* out_capabilities,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_capabilities == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_capabilities", "null_output"));
        }
        const std::uint32_t declared_size = out_capabilities->struct_size;
        bool reserved32_nonzero = false;
        std::optional<std::uint32_t> reserved64_nonzero;
        if (declared_size >= FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE) {
            reserved32_nonzero =
                out_capabilities->reserved32 != UINT32_C(0);
            for (std::uint32_t index = UINT32_C(0); index < UINT32_C(4);
                 ++index) {
                if (out_capabilities->reserved64[index] != UINT64_C(0)) {
                    reserved64_nonzero = index;
                    break;
                }
            }
        }
        fastdb::payload::abi::clear_capabilities_prefix(out_capabilities,
                                                        declared_size);
        if (declared_size < FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_prefix(
                    "capabilities", declared_size,
                    FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE));
        }
        if (reserved32_nonzero) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_field(
                    "capabilities", "reserved32"));
        }
        if (reserved64_nonzero.has_value()) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::unsupported_field(
                    "capabilities",
                    "reserved64[" +
                        std::to_string(*reserved64_nonzero) + "]"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        out_capabilities->profile =
            fastdb::payload::abi::public_profile(spec->compiled.profile());
        out_capabilities->semantic_flags =
            spec->compiled.facts().semantic_flags;
        out_capabilities->operation_flags =
            spec->compiled.capabilities().operation_flags;
        out_capabilities->codegen_target_flags =
            spec->compiled.capabilities().codegen_target_flags;
        out_capabilities->direct_build_status =
            spec->compiled.capabilities().direct_build_status;
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_entry_count(
    const fdb_payload_v1_spec_t* spec,
    uint32_t* out_count,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_count != nullptr) {
            *out_count = UINT32_C(0);
        }
        if (out_count == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_count",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        *out_count = static_cast<std::uint32_t>(
            spec->compiled.resolved().entries().size());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_entry_id(
    const fdb_payload_v1_spec_t* spec,
    uint32_t entry_index,
    fdb_payload_v1_blob_t** out_id,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_id != nullptr) {
            *out_id = nullptr;
        }
        if (out_id == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_id",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        const auto& entries = spec->compiled.resolved().entries();
        const std::uint32_t count =
            static_cast<std::uint32_t>(entries.size());
        if (entry_index >= count) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::index_out_of_range(
                    "entries", entry_index, count));
        }
        return fastdb::payload::abi::publish_blob(
            entries[static_cast<std::size_t>(entry_index)].id, out_id);
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_entry_index(
    const fdb_payload_v1_spec_t* spec,
    const uint8_t* id,
    uint64_t id_size,
    uint32_t* out_entry_index,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_entry_index != nullptr) {
            *out_entry_index = UINT32_C(0);
        }
        if (out_entry_index == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_entry_index",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        auto validated = fastdb::payload::abi::validated_id(
            id, id_size, "id");
        if (!validated.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(validated).error());
        }
        const std::optional<std::uint32_t> found =
            spec->compiled.entry_index(validated.value());
        if (!found.has_value()) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::missing_id(validated.value(),
                                                 "entry"));
        }
        *out_entry_index = *found;
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_component_count(
    const fdb_payload_v1_spec_t* spec,
    uint32_t* out_count,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_count != nullptr) {
            *out_count = UINT32_C(0);
        }
        if (out_count == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_count",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        *out_count = static_cast<std::uint32_t>(
            spec->compiled.resolved().components().size());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_component_id(
    const fdb_payload_v1_spec_t* spec,
    uint32_t component_index,
    fdb_payload_v1_blob_t** out_id,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_id != nullptr) {
            *out_id = nullptr;
        }
        if (out_id == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_id",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        const auto& components = spec->compiled.resolved().components();
        const std::uint32_t count =
            static_cast<std::uint32_t>(components.size());
        if (component_index >= count) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::index_out_of_range(
                    "components", component_index, count));
        }
        return fastdb::payload::abi::publish_blob(
            components[static_cast<std::size_t>(component_index)].id, out_id);
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_spec_component_index(
    const fdb_payload_v1_spec_t* spec,
    const uint8_t* id,
    uint64_t id_size,
    uint32_t* out_component_index,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_component_index != nullptr) {
            *out_component_index = UINT32_C(0);
        }
        if (out_component_index == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument(
                    "out_component_index", "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        auto validated = fastdb::payload::abi::validated_id(
            id, id_size, "id");
        if (!validated.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(validated).error());
        }
        const std::optional<std::uint32_t> found =
            spec->compiled.component_index(validated.value());
        if (!found.has_value()) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::missing_id(validated.value(),
                                                 "component"));
        }
        *out_component_index = *found;
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_spec_component_field_count(
    const fdb_payload_v1_spec_t* spec,
    uint32_t component_index,
    uint32_t* out_count,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_count != nullptr) {
            *out_count = UINT32_C(0);
        }
        if (out_count == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_count",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        const auto& components = spec->compiled.resolved().components();
        const std::uint32_t component_count =
            static_cast<std::uint32_t>(components.size());
        if (component_index >= component_count) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::index_out_of_range(
                    "components", component_index, component_count));
        }
        *out_count = static_cast<std::uint32_t>(
            components[static_cast<std::size_t>(component_index)]
                .fields.size());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_spec_component_field_id(
    const fdb_payload_v1_spec_t* spec,
    uint32_t component_index,
    uint32_t field_index,
    fdb_payload_v1_blob_t** out_id,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_id != nullptr) {
            *out_id = nullptr;
        }
        if (out_id == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_id",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        const auto& components = spec->compiled.resolved().components();
        const std::uint32_t component_count =
            static_cast<std::uint32_t>(components.size());
        if (component_index >= component_count) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::index_out_of_range(
                    "components", component_index, component_count));
        }
        const auto& fields =
            components[static_cast<std::size_t>(component_index)].fields;
        const std::uint32_t field_count =
            static_cast<std::uint32_t>(fields.size());
        if (field_index >= field_count) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::index_out_of_range(
                    "component_fields", field_index, field_count));
        }
        return fastdb::payload::abi::publish_blob(
            fields[static_cast<std::size_t>(field_index)].id, out_id);
    });
}

extern "C" fdb_payload_v1_status_t
fdb_payload_v1_spec_component_field_index(
    const fdb_payload_v1_spec_t* spec,
    uint32_t component_index,
    const uint8_t* id,
    uint64_t id_size,
    uint32_t* out_field_index,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_field_index != nullptr) {
            *out_field_index = UINT32_C(0);
        }
        if (out_field_index == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_field_index",
                                                       "null_output"));
        }
        if (spec == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("spec",
                                                       "null_handle"));
        }
        const auto& components = spec->compiled.resolved().components();
        const std::uint32_t component_count =
            static_cast<std::uint32_t>(components.size());
        if (component_index >= component_count) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::index_out_of_range(
                    "components", component_index, component_count));
        }
        auto validated = fastdb::payload::abi::validated_id(
            id, id_size, "id");
        if (!validated.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(validated).error());
        }
        const std::optional<std::uint32_t> found =
            spec->compiled.component_field_index(component_index,
                                                 validated.value());
        if (!found.has_value()) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::missing_id(validated.value(), "field"));
        }
        *out_field_index = *found;
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_source_schema_json(
    fdb_payload_v1_blob_t** out_blob,
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        if (out_blob != nullptr) {
            *out_blob = nullptr;
        }
        if (out_blob == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_blob",
                                                       "null_output"));
        }
        auto schema =
            fastdb::payload::spec::SchemaRepository::payload_source_schema();
        if (!schema.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(schema).error());
        }
        return fastdb::payload::abi::publish_blob(
            schema.value().canonical_bytes, out_blob);
    });
}

extern "C" fdb_payload_v1_status_t fdb_payload_v1_source_schema_sha256(
    uint8_t out_digest[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error) {
    return fastdb::payload::abi::guard_status(out_error, [=]() {
        fastdb::payload::abi::clear_digest(out_digest);
        if (out_digest == nullptr) {
            return fastdb::payload::abi::failure(
                fastdb::payload::abi::invalid_argument("out_digest",
                                                       "null_output"));
        }
        auto schema =
            fastdb::payload::spec::SchemaRepository::payload_source_schema();
        if (!schema.has_value()) {
            return fastdb::payload::error::Result<void>::failure(
                std::move(schema).error());
        }
        std::memcpy(out_digest, schema.value().sha256.data(),
                    schema.value().sha256.size());
        return fastdb::payload::error::Result<void>::success();
    });
}

extern "C" void fdb_payload_v1_blob_retain(fdb_payload_v1_blob_t* blob) {
    fastdb::payload::abi::guard_void([blob]() {
        fastdb::payload::abi::detail::retain_reference(blob);
    });
}

extern "C" void fdb_payload_v1_blob_release(fdb_payload_v1_blob_t* blob) {
    fastdb::payload::abi::guard_void([blob]() {
        fastdb::payload::abi::detail::release_reference(
            blob, [](fdb_payload_v1_blob_t* value) noexcept { delete value; });
    });
}

extern "C" const uint8_t* fdb_payload_v1_blob_data(
    const fdb_payload_v1_blob_t* blob) {
    return fastdb::payload::abi::guard_value(
        static_cast<const std::uint8_t*>(nullptr), [blob]() {
            if (blob == nullptr || blob->bytes.empty()) {
                return static_cast<const std::uint8_t*>(nullptr);
            }
            return blob->bytes.data();
        });
}

extern "C" uint64_t fdb_payload_v1_blob_size(
    const fdb_payload_v1_blob_t* blob) {
    return fastdb::payload::abi::guard_value(UINT64_C(0), [blob]() {
        return blob == nullptr ? UINT64_C(0)
                               : static_cast<std::uint64_t>(blob->bytes.size());
    });
}

extern "C" void fdb_payload_v1_error_retain(fdb_payload_v1_error_t* error) {
    fastdb::payload::abi::guard_void([error]() {
        fastdb::payload::abi::detail::retain_reference(error);
    });
}

extern "C" void fdb_payload_v1_error_release(fdb_payload_v1_error_t* error) {
    fastdb::payload::abi::guard_void([error]() {
        fastdb::payload::abi::detail::release_reference(
            error, [](fdb_payload_v1_error_t* value) noexcept {
                if (value->destroy != nullptr) {
                    value->destroy(value);
                }
            });
    });
}

extern "C" uint32_t fdb_payload_v1_error_code(
    const fdb_payload_v1_error_t* error) {
    return fastdb::payload::abi::guard_value(
        UINT32_C(0), [error]() {
            return error == nullptr ? UINT32_C(0) : error->code;
        });
}

extern "C" void fdb_payload_v1_error_symbol(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size) {
    fastdb::payload::abi::guard_void([=]() {
        fastdb::payload::abi::write_error_bytes(
            error, &fdb_payload_v1_error_t::symbol, out_data, out_size);
    });
}

extern "C" void fdb_payload_v1_error_path(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size) {
    fastdb::payload::abi::guard_void([=]() {
        fastdb::payload::abi::write_error_bytes(
            error, &fdb_payload_v1_error_t::path, out_data, out_size);
    });
}

extern "C" void fdb_payload_v1_error_message(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size) {
    fastdb::payload::abi::guard_void([=]() {
        fastdb::payload::abi::write_error_bytes(
            error, &fdb_payload_v1_error_t::message, out_data, out_size);
    });
}

extern "C" void fdb_payload_v1_error_details_json(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size) {
    fastdb::payload::abi::guard_void([=]() {
        fastdb::payload::abi::write_error_bytes(
            error, &fdb_payload_v1_error_t::details_json, out_data, out_size);
    });
}
