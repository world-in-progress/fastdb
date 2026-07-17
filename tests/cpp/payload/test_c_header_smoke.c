#include <stdint.h>

#include <fastdb_payload.h>

_Static_assert(sizeof(fdb_payload_v1_compile_options_t) ==
                   FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE,
               "compile options ABI size");
_Static_assert(sizeof(fdb_payload_v1_capabilities_t) ==
                   FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE,
               "capabilities ABI size");

int main(void) {
    fdb_payload_v1_compile_options_t options = {0};
    fdb_payload_v1_capabilities_t capabilities = {0};
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

    return 0;
}
