#include <fastdb_payload.h>

#include "payload/abi/Handles.hpp"
#include "payload/error/Error.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/spec/SchemaRepository.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
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
