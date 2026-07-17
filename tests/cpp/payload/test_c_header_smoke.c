#include <stdint.h>

#include <fastdb_payload.h>

_Static_assert(sizeof(fdb_payload_v1_compile_options_t) ==
                   FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE,
               "compile options ABI size");
_Static_assert(sizeof(fdb_payload_v1_capabilities_t) ==
                   FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE,
               "capabilities ABI size");

int main(void) {
    static const char valid_source[] =
        "{\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\","
        "\"entries\":[],\"components\":[]}";
    static const char duplicate_source[] =
        "{\"schema\":\"fastdb.payload.v1\","
        "\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\","
        "\"entries\":[],\"components\":[]}";
    fdb_payload_v1_compile_options_t options = {0};
    fdb_payload_v1_capabilities_t capabilities = {0};
    fdb_payload_v1_spec_t* spec = (fdb_payload_v1_spec_t*)0;
    fdb_payload_v1_blob_t* canonical = (fdb_payload_v1_blob_t*)0;
    fdb_payload_v1_blob_t* manifest = (fdb_payload_v1_blob_t*)0;
    fdb_payload_v1_error_t* error = (fdb_payload_v1_error_t*)0;
    fdb_payload_v1_status_t status = UINT32_C(0);
    fdb_payload_v1_profile_t profile = UINT32_C(0);
    uint8_t digest[FDB_PAYLOAD_V1_SHA256_SIZE] = {0};
    uint32_t count = UINT32_C(1);
    uint32_t index = UINT32_C(0);

    fdb_payload_v1_compile_options_init(&options);
    fdb_payload_v1_capabilities_init(&capabilities);

    if (fdb_payload_v1_abi_version() != FDB_PAYLOAD_V1_ABI_VERSION ||
        FDB_PAYLOAD_V1_ABI_VERSION != UINT32_C(1) ||
        FDB_PAYLOAD_V1_SHA256_SIZE != UINT32_C(32)) {
        return 1;
    }
    if (options.struct_size != FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE ||
        options.flags != UINT32_C(0) ||
        options.max_source_bytes != UINT64_C(16777216) ||
        options.max_json_values != UINT64_C(1000000) ||
        options.max_nesting_depth != UINT32_C(128) ||
        options.max_entries != UINT32_C(65536) ||
        options.max_components != UINT32_C(65536) ||
        options.max_fields_per_component != UINT32_C(65536) ||
        options.max_total_fields != UINT64_C(1000000)) {
        return 2;
    }
    for (index = UINT32_C(0); index < UINT32_C(4); ++index) {
        if (options.reserved[index] != UINT64_C(0)) {
            return 3;
        }
    }
    if (capabilities.struct_size != FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE ||
        capabilities.profile != UINT32_C(0) ||
        capabilities.semantic_flags != UINT64_C(0) ||
        capabilities.operation_flags != UINT64_C(0) ||
        capabilities.codegen_target_flags != UINT64_C(0) ||
        capabilities.direct_build_status !=
            FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED ||
        capabilities.reserved32 != UINT32_C(0)) {
        return 4;
    }
    for (index = UINT32_C(0); index < UINT32_C(4); ++index) {
        if (capabilities.reserved64[index] != UINT64_C(0)) {
            return 5;
        }
    }
    if (FDB_PAYLOAD_PROFILE_RECORD_V1 != UINT32_C(1) ||
        FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1 != UINT32_C(2) ||
        FDB_PAYLOAD_SEMANTIC_HAS_NULLABLE != (UINT64_C(1) << 0) ||
        FDB_PAYLOAD_SEMANTIC_HAS_LISTS != (UINT64_C(1) << 1) ||
        FDB_PAYLOAD_SEMANTIC_HAS_REFERENCES != (UINT64_C(1) << 2) ||
        FDB_PAYLOAD_SEMANTIC_HAS_VARIABLE_WIDTH != (UINT64_C(1) << 3) ||
        FDB_PAYLOAD_SEMANTIC_HAS_NORMALIZED_INTEGERS !=
            (UINT64_C(1) << 4) ||
        FDB_PAYLOAD_OPERATION_COMPILE != (UINT64_C(1) << 0) ||
        FDB_PAYLOAD_OPERATION_QUERY != (UINT64_C(1) << 1)) {
        return 6;
    }

    fdb_payload_v1_compile_options_init((fdb_payload_v1_compile_options_t*)0);
    fdb_payload_v1_capabilities_init((fdb_payload_v1_capabilities_t*)0);
    fdb_payload_v1_blob_retain((fdb_payload_v1_blob_t*)0);
    fdb_payload_v1_blob_release((fdb_payload_v1_blob_t*)0);
    fdb_payload_v1_error_retain((fdb_payload_v1_error_t*)0);
    fdb_payload_v1_error_release((fdb_payload_v1_error_t*)0);

    status = fdb_payload_v1_spec_compile_json(
        (const uint8_t*)valid_source,
        (uint64_t)(sizeof(valid_source) - UINT64_C(1)), &options, &spec,
        &error);
    if (status != UINT32_C(0) || spec == (fdb_payload_v1_spec_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 7;
    }
    fdb_payload_v1_spec_retain(spec);
    fdb_payload_v1_spec_release(spec);

    status = fdb_payload_v1_spec_canonical_json(spec, &canonical, &error);
    if (status != UINT32_C(0) || canonical == (fdb_payload_v1_blob_t*)0 ||
        error != (fdb_payload_v1_error_t*)0 ||
        fdb_payload_v1_blob_size(canonical) == UINT64_C(0) ||
        fdb_payload_v1_blob_data(canonical) == (const uint8_t*)0) {
        return 8;
    }
    status = fdb_payload_v1_spec_sha256(spec, digest, &error);
    if (status != UINT32_C(0) || error != (fdb_payload_v1_error_t*)0) {
        return 9;
    }
    status = fdb_payload_v1_spec_manifest_json(spec, &manifest, &error);
    if (status != UINT32_C(0) || manifest == (fdb_payload_v1_blob_t*)0 ||
        error != (fdb_payload_v1_error_t*)0 ||
        fdb_payload_v1_blob_size(manifest) == UINT64_C(0)) {
        return 10;
    }
    status = fdb_payload_v1_spec_profile(spec, &profile, &error);
    if (status != UINT32_C(0) || profile != FDB_PAYLOAD_PROFILE_RECORD_V1 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 11;
    }
    fdb_payload_v1_capabilities_init(&capabilities);
    status =
        fdb_payload_v1_spec_capabilities(spec, &capabilities, &error);
    if (status != UINT32_C(0) ||
        capabilities.struct_size != FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE ||
        capabilities.profile != FDB_PAYLOAD_PROFILE_RECORD_V1 ||
        capabilities.semantic_flags != UINT64_C(0) ||
        capabilities.operation_flags !=
            (FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY) ||
        capabilities.codegen_target_flags != UINT64_C(0) ||
        capabilities.direct_build_status !=
            FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED ||
        error != (fdb_payload_v1_error_t*)0) {
        return 12;
    }
    status = fdb_payload_v1_spec_entry_count(spec, &count, &error);
    if (status != UINT32_C(0) || count != UINT32_C(0) ||
        error != (fdb_payload_v1_error_t*)0) {
        return 13;
    }
    count = UINT32_C(1);
    status = fdb_payload_v1_spec_component_count(spec, &count, &error);
    if (status != UINT32_C(0) || count != UINT32_C(0) ||
        error != (fdb_payload_v1_error_t*)0) {
        return 14;
    }

    fdb_payload_v1_blob_release(canonical);
    fdb_payload_v1_blob_release(manifest);
    fdb_payload_v1_spec_release(spec);
    spec = (fdb_payload_v1_spec_t*)0;

    status = fdb_payload_v1_spec_compile_json(
        (const uint8_t*)duplicate_source,
        (uint64_t)(sizeof(duplicate_source) - UINT64_C(1)),
        (const fdb_payload_v1_compile_options_t*)0, &spec, &error);
    if (status != FDB_PAYLOAD_E_DUPLICATE_KEY ||
        spec != (fdb_payload_v1_spec_t*)0 ||
        error == (fdb_payload_v1_error_t*)0 ||
        fdb_payload_v1_error_code(error) != status) {
        return 15;
    }
    fdb_payload_v1_error_release(error);

    return 0;
}
