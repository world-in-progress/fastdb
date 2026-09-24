#include <fastdb_payload.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {

[[noreturn]] void impossible(fdb_payload_v1_error_t* error = nullptr) {
    fdb_payload_v1_error_release(error);
    std::abort();
}

void require_success(fdb_payload_v1_status_t status,
                     fdb_payload_v1_error_t* error) {
    if (status != UINT32_C(0) || error != nullptr) {
        impossible(error);
    }
}

void inspect_blob(fdb_payload_v1_blob_t* blob) {
    if (blob == nullptr) {
        impossible();
    }
    const std::uint64_t size = fdb_payload_v1_blob_size(blob);
    const std::uint8_t* const data = fdb_payload_v1_blob_data(blob);
    if (size != UINT64_C(0) && data == nullptr) {
        impossible();
    }
}

using BlobQuery = fdb_payload_v1_status_t (*)(
    const fdb_payload_v1_spec_t*,
    fdb_payload_v1_blob_t**,
    fdb_payload_v1_error_t**);

void query_blob(const fdb_payload_v1_spec_t* spec, BlobQuery query) {
    fdb_payload_v1_blob_t* blob = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const fdb_payload_v1_status_t status = query(spec, &blob, &error);
    require_success(status, error);
    inspect_blob(blob);
    fdb_payload_v1_blob_release(blob);
}

void inspect_error_field(
    const fdb_payload_v1_error_t* error,
    void (*getter)(const fdb_payload_v1_error_t*, const std::uint8_t**,
                   std::uint64_t*)) {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    getter(error, &data, &size);
    if (size != UINT64_C(0) && data == nullptr) {
        impossible();
    }
}

const fdb_payload_v1_compile_options_t* compile_options_for_input(
    const std::uint8_t* data,
    std::size_t size,
    fdb_payload_v1_compile_options_t& options) {
    if (size == 0U || (data[0] & UINT8_C(7)) == UINT8_C(0)) {
        return nullptr;
    }

    fdb_payload_v1_compile_options_init(&options);
    switch (data[0] & UINT8_C(7)) {
        case UINT8_C(1):
            options.max_source_bytes = UINT64_C(64);
            break;
        case UINT8_C(2):
            options.max_json_values = UINT64_C(64);
            break;
        case UINT8_C(3):
            break;
        case UINT8_C(4):
            options.max_nesting_depth = UINT32_C(8);
            break;
        case UINT8_C(5):
            options.max_entries = UINT32_C(4);
            options.max_components = UINT32_C(4);
            options.max_fields_per_component = UINT32_C(8);
            break;
        case UINT8_C(6):
            options.max_total_fields = UINT64_C(8);
            break;
        case UINT8_C(7):
            options.max_source_bytes = UINT64_C(1024);
            options.max_json_values = UINT64_C(256);
            options.max_nesting_depth = UINT32_C(32);
            break;
        default:
            impossible();
    }
    return &options;
}

void query_entry(const fdb_payload_v1_spec_t* spec,
                 std::uint32_t entry_index) {
    fdb_payload_v1_blob_t* id = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    fdb_payload_v1_status_t status =
        fdb_payload_v1_spec_entry_id(spec, entry_index, &id, &error);
    require_success(status, error);
    inspect_blob(id);

    std::uint32_t round_trip = UINT32_MAX;
    const std::uint8_t* const data = fdb_payload_v1_blob_data(id);
    const std::uint64_t size = fdb_payload_v1_blob_size(id);
    error = nullptr;
    status = fdb_payload_v1_spec_entry_index(
        spec, data, size, &round_trip, &error);
    require_success(status, error);
    if (round_trip != entry_index) {
        fdb_payload_v1_blob_release(id);
        impossible();
    }
    fdb_payload_v1_blob_release(id);
}

void query_field(const fdb_payload_v1_spec_t* spec,
                 std::uint32_t component_index,
                 std::uint32_t field_index) {
    fdb_payload_v1_blob_t* id = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    fdb_payload_v1_status_t status =
        fdb_payload_v1_spec_component_field_id(
            spec, component_index, field_index, &id, &error);
    require_success(status, error);
    inspect_blob(id);

    std::uint32_t round_trip = UINT32_MAX;
    const std::uint8_t* const data = fdb_payload_v1_blob_data(id);
    const std::uint64_t size = fdb_payload_v1_blob_size(id);
    error = nullptr;
    status = fdb_payload_v1_spec_component_field_index(
        spec, component_index, data, size, &round_trip, &error);
    require_success(status, error);
    if (round_trip != field_index) {
        fdb_payload_v1_blob_release(id);
        impossible();
    }
    fdb_payload_v1_blob_release(id);
}

void query_component(const fdb_payload_v1_spec_t* spec,
                     std::uint32_t component_index) {
    fdb_payload_v1_blob_t* id = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    fdb_payload_v1_status_t status = fdb_payload_v1_spec_component_id(
        spec, component_index, &id, &error);
    require_success(status, error);
    inspect_blob(id);

    std::uint32_t round_trip = UINT32_MAX;
    const std::uint8_t* const data = fdb_payload_v1_blob_data(id);
    const std::uint64_t size = fdb_payload_v1_blob_size(id);
    error = nullptr;
    status = fdb_payload_v1_spec_component_index(
        spec, data, size, &round_trip, &error);
    require_success(status, error);
    if (round_trip != component_index) {
        fdb_payload_v1_blob_release(id);
        impossible();
    }
    fdb_payload_v1_blob_release(id);

    std::uint32_t field_count = UINT32_C(0);
    error = nullptr;
    status = fdb_payload_v1_spec_component_field_count(
        spec, component_index, &field_count, &error);
    require_success(status, error);
    for (std::uint32_t field_index = UINT32_C(0);
         field_index < field_count; ++field_index) {
        query_field(spec, component_index, field_index);
    }
}

void query_compiled_spec(const fdb_payload_v1_spec_t* spec) {
    query_blob(spec, fdb_payload_v1_spec_canonical_json);
    query_blob(spec, fdb_payload_v1_spec_manifest_json);

    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
    fdb_payload_v1_error_t* error = nullptr;
    fdb_payload_v1_status_t status =
        fdb_payload_v1_spec_sha256(spec, digest.data(), &error);
    require_success(status, error);

    fdb_payload_v1_profile_t profile = UINT32_C(0);
    error = nullptr;
    status = fdb_payload_v1_spec_profile(spec, &profile, &error);
    require_success(status, error);

    fdb_payload_v1_capabilities_t capabilities{};
    fdb_payload_v1_capabilities_init(&capabilities);
    error = nullptr;
    status = fdb_payload_v1_spec_capabilities(spec, &capabilities, &error);
    require_success(status, error);

    std::uint32_t entry_count = UINT32_C(0);
    error = nullptr;
    status = fdb_payload_v1_spec_entry_count(spec, &entry_count, &error);
    require_success(status, error);
    for (std::uint32_t entry_index = UINT32_C(0); entry_index < entry_count;
         ++entry_index) {
        query_entry(spec, entry_index);
    }

    std::uint32_t component_count = UINT32_C(0);
    error = nullptr;
    status =
        fdb_payload_v1_spec_component_count(spec, &component_count, &error);
    require_success(status, error);
    for (std::uint32_t component_index = UINT32_C(0);
         component_index < component_count; ++component_index) {
        query_component(spec, component_index);
    }
}

void inspect_compile_failure(fdb_payload_v1_status_t status,
                             fdb_payload_v1_spec_t* spec,
                             fdb_payload_v1_error_t* error) {
    if (status == UINT32_C(0) || spec != nullptr || error == nullptr ||
        fdb_payload_v1_error_code(error) != status) {
        impossible(error);
    }
    inspect_error_field(error, fdb_payload_v1_error_symbol);
    inspect_error_field(error, fdb_payload_v1_error_path);
    inspect_error_field(error, fdb_payload_v1_error_message);
    inspect_error_field(error, fdb_payload_v1_error_details_json);
    fdb_payload_v1_error_release(error);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (size > static_cast<std::size_t>(
                       std::numeric_limits<std::uint64_t>::max())) {
            return 0;
        }
    }

    fdb_payload_v1_compile_options_t options{};
    const fdb_payload_v1_compile_options_t* const selected_options =
        compile_options_for_input(data, size, options);

    // The first byte remains included in the exact compile span. When present,
    // it also selects only supported V1 limit variants; no byte is discarded.
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const fdb_payload_v1_status_t status =
        fdb_payload_v1_spec_compile_json(data, static_cast<std::uint64_t>(size),
                                         selected_options, &spec, &error);
    if (status != UINT32_C(0)) {
        inspect_compile_failure(status, spec, error);
        return 0;
    }
    if (spec == nullptr || error != nullptr) {
        impossible(error);
    }

    query_compiled_spec(spec);
    fdb_payload_v1_spec_release(spec);
    return 0;
}
