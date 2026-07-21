#include <stddef.h>
#include <stdint.h>

#include <fastdb_payload.h>

_Static_assert(sizeof(fdb_payload_v1_compile_options_t) ==
                   FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE,
               "compile options ABI size");
_Static_assert(sizeof(fdb_payload_v1_capabilities_t) ==
                   FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE,
               "capabilities ABI size");
_Static_assert(sizeof(fdb_payload_v1_builder_options_t) ==
                   FDB_PAYLOAD_V1_BUILDER_OPTIONS_V2_SIZE,
               "builder options ABI size");
_Static_assert(FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE == UINT32_C(88),
               "builder options V1 prefix size");
_Static_assert(FDB_PAYLOAD_V1_BUILDER_OPTIONS_V2_SIZE == UINT32_C(96),
               "builder options V2 size");
_Static_assert(offsetof(fdb_payload_v1_builder_options_t, max_graph_objects) ==
                   FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE,
               "builder graph limit tail offset");
_Static_assert(sizeof(fdb_payload_v1_open_options_t) ==
                   FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE,
               "open options ABI size");
_Static_assert(sizeof(fdb_payload_v1_plan_info_t) ==
                   FDB_PAYLOAD_V1_PLAN_INFO_V2_SIZE,
               "plan info ABI size");
_Static_assert(FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE == UINT32_C(104),
               "plan info V1 prefix size");
_Static_assert(FDB_PAYLOAD_V1_PLAN_INFO_V2_SIZE == UINT32_C(112),
               "plan info V2 size");
_Static_assert(offsetof(fdb_payload_v1_plan_info_t, graph_object_count) ==
                   FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE,
               "plan graph count tail offset");
_Static_assert(sizeof(fdb_payload_v1_object_handle_t) == sizeof(uint64_t),
               "builder object token width");
_Static_assert(FDB_PAYLOAD_V1_INVALID_OBJECT_HANDLE == UINT64_C(0),
               "invalid builder object token");
_Static_assert(sizeof(fdb_payload_v1_execution_report_t) ==
                   FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE,
               "execution report ABI size");
_Static_assert(offsetof(fdb_payload_v1_fixed_run_v1_t, data) <
                   offsetof(fdb_payload_v1_fixed_run_v1_t, validity),
               "fixed run pointer order");
_Static_assert(offsetof(fdb_payload_v1_backing_v1_t, context) <
                   offsetof(fdb_payload_v1_backing_v1_t, reserve),
               "backing callback order");
_Static_assert(offsetof(fdb_payload_v1_backing_v1_t, release) <
                   offsetof(fdb_payload_v1_backing_v1_t, reserved),
               "backing reserved tail order");

int main(void) {
    static const char valid_source[] =
        "{\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\","
        "\"entries\":[],\"components\":[]}";
    static const char duplicate_source[] =
        "{\"schema\":\"fastdb.payload.v1\","
        "\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\","
        "\"entries\":[],\"components\":[]}";
    static const char runtime_source[] =
        "{\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\","
        "\"entries\":[{\"id\":\"value\",\"cardinality\":\"one\","
        "\"type\":{\"kind\":\"u8\"}}],\"components\":[]}";
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
    fdb_payload_v1_builder_options_t builder_options = {0};
    fdb_payload_v1_fixed_run_v1_t fixed_run = {0};
    fdb_payload_v1_open_options_t open_options = {0};
    fdb_payload_v1_plan_info_t plan_info = {0};
    fdb_payload_v1_execution_report_t execution_report = {0};
    fdb_payload_v1_backing_v1_t backing = {0};
    fdb_payload_v1_builder_t* builder = (fdb_payload_v1_builder_t*)0;
    fdb_payload_v1_plan_t* plan = (fdb_payload_v1_plan_t*)0;
    fdb_payload_v1_payload_t* payload = (fdb_payload_v1_payload_t*)0;
    fdb_payload_v1_payload_t* opened = (fdb_payload_v1_payload_t*)0;
    fdb_payload_v1_blob_t* binary = (fdb_payload_v1_blob_t*)0;
    fdb_payload_v1_view_t* sequence = (fdb_payload_v1_view_t*)0;
    fdb_payload_v1_view_t* value_view = (fdb_payload_v1_view_t*)0;
    fdb_payload_v1_view_t* detached = (fdb_payload_v1_view_t*)0;
    fdb_payload_v1_access_t* access = (fdb_payload_v1_access_t*)0;
    const uint8_t* access_data = (const uint8_t*)0;
    uint64_t access_size = UINT64_C(0);
    uint64_t view_length = UINT64_C(0);
    uint32_t view_kind = UINT32_C(0);
    uint8_t view_u8 = UINT8_C(0);

    fdb_payload_v1_compile_options_init(&options);
    fdb_payload_v1_capabilities_init(&capabilities);
    fdb_payload_v1_builder_options_init(&builder_options);
    fdb_payload_v1_fixed_run_init(&fixed_run);
    fdb_payload_v1_open_options_init(&open_options);
    fdb_payload_v1_plan_info_init(&plan_info);
    fdb_payload_v1_execution_report_init(&execution_report);
    fdb_payload_v1_backing_init(&backing);

    if (builder_options.struct_size !=
            FDB_PAYLOAD_V1_BUILDER_OPTIONS_V2_SIZE ||
        builder_options.max_graph_objects != UINT64_C(10000000) ||
        fixed_run.struct_size != sizeof(fixed_run) ||
        fixed_run.data != NULL || fixed_run.validity != NULL ||
        open_options.struct_size != FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE ||
        plan_info.struct_size != FDB_PAYLOAD_V1_PLAN_INFO_V2_SIZE ||
        plan_info.graph_object_count != UINT64_C(0) ||
        execution_report.struct_size !=
            FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE ||
        backing.struct_size != sizeof(backing) || backing.context != NULL ||
        backing.reserve != NULL || backing.write != NULL ||
        backing.commit != NULL || backing.rollback != NULL ||
        backing.retain != NULL || backing.release != NULL) {
        return 16;
    }

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
    fdb_payload_v1_builder_options_init((fdb_payload_v1_builder_options_t*)0);
    fdb_payload_v1_fixed_run_init((fdb_payload_v1_fixed_run_v1_t*)0);
    fdb_payload_v1_open_options_init((fdb_payload_v1_open_options_t*)0);
    fdb_payload_v1_plan_info_init((fdb_payload_v1_plan_info_t*)0);
    fdb_payload_v1_execution_report_init(
        (fdb_payload_v1_execution_report_t*)0);
    fdb_payload_v1_backing_init((fdb_payload_v1_backing_v1_t*)0);
    fdb_payload_v1_blob_retain((fdb_payload_v1_blob_t*)0);
    fdb_payload_v1_blob_release((fdb_payload_v1_blob_t*)0);
    fdb_payload_v1_error_retain((fdb_payload_v1_error_t*)0);
    fdb_payload_v1_error_release((fdb_payload_v1_error_t*)0);
    fdb_payload_v1_view_retain((fdb_payload_v1_view_t*)0);
    fdb_payload_v1_view_release((fdb_payload_v1_view_t*)0);
    fdb_payload_v1_access_release((fdb_payload_v1_access_t*)0);

    /* Keep all graph ABI declarations in the strict C11 link smoke. */
    (void)&fdb_payload_v1_builder_object_declare;
    (void)&fdb_payload_v1_builder_object_fill_begin;
    (void)&fdb_payload_v1_builder_value_object;
    (void)&fdb_payload_v1_builder_value_ref;
    (void)&fdb_payload_v1_view_graph_identity;
    (void)&fdb_payload_v1_view_ref_target;

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
            (FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY |
             FDB_PAYLOAD_OPERATION_BUILD | FDB_PAYLOAD_OPERATION_OPEN |
             FDB_PAYLOAD_OPERATION_VIEW |
             FDB_PAYLOAD_OPERATION_MATERIALIZE |
             FDB_PAYLOAD_OPERATION_INVALIDATE) ||
        capabilities.codegen_target_flags != UINT64_C(0) ||
        capabilities.direct_build_status !=
            FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE ||
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

    error = (fdb_payload_v1_error_t*)0;
    status = fdb_payload_v1_spec_compile_json(
        (const uint8_t*)runtime_source,
        (uint64_t)(sizeof(runtime_source) - UINT64_C(1)),
        (const fdb_payload_v1_compile_options_t*)0, &spec, &error);
    if (status != UINT32_C(0) || spec == (fdb_payload_v1_spec_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 17;
    }
    status = fdb_payload_v1_builder_create(
        spec, &builder_options, &builder, &error);
    if (status != UINT32_C(0) || builder == (fdb_payload_v1_builder_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 18;
    }
    status = fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(0), UINT64_C(1), &error);
    if (status != UINT32_C(0) || error != (fdb_payload_v1_error_t*)0) {
        return 19;
    }
    status = fdb_payload_v1_builder_value_bool(builder, UINT8_C(1), &error);
    if (status != FDB_PAYLOAD_E_TYPE_MISMATCH ||
        error == (fdb_payload_v1_error_t*)0 ||
        fdb_payload_v1_error_code(error) != status) {
        return 20;
    }
    fdb_payload_v1_error_release(error);
    error = (fdb_payload_v1_error_t*)0;
    status = fdb_payload_v1_builder_value_u8(builder, UINT8_C(7), &error);
    if (status != UINT32_C(0) || error != (fdb_payload_v1_error_t*)0) {
        return 21;
    }
    status = fdb_payload_v1_builder_freeze(builder, &plan, &error);
    if (status != UINT32_C(0) || plan == (fdb_payload_v1_plan_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 22;
    }
    fdb_payload_v1_builder_release(builder);
    fdb_payload_v1_plan_retain(plan);
    fdb_payload_v1_plan_release(plan);
    status = fdb_payload_v1_plan_info(plan, &plan_info, &error);
    if (status != UINT32_C(0) || plan_info.total_bytes == UINT64_C(0) ||
        error != (fdb_payload_v1_error_t*)0) {
        return 23;
    }
    status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING,
        (const fdb_payload_v1_backing_v1_t*)0, &payload,
        &execution_report, &error);
    if (status != UINT32_C(0) || payload == (fdb_payload_v1_payload_t*)0 ||
        execution_report.mode != FDB_PAYLOAD_EXECUTION_DIRECT ||
        error != (fdb_payload_v1_error_t*)0) {
        return 24;
    }
    status = fdb_payload_v1_payload_binary_blob(payload, &binary, &error);
    if (status != UINT32_C(0) || binary == (fdb_payload_v1_blob_t*)0 ||
        fdb_payload_v1_blob_size(binary) == UINT64_C(0) ||
        error != (fdb_payload_v1_error_t*)0) {
        return 25;
    }
    status = fdb_payload_v1_payload_open_copy(
        spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), &open_options, &opened, &error);
    if (status != UINT32_C(0) || opened == (fdb_payload_v1_payload_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 26;
    }
    status = fdb_payload_v1_payload_sha256(opened, digest, &error);
    if (status != UINT32_C(0) || error != (fdb_payload_v1_error_t*)0) {
        return 27;
    }
    status = fdb_payload_v1_payload_profile(opened, &profile, &error);
    if (status != UINT32_C(0) || profile != FDB_PAYLOAD_PROFILE_RECORD_V1 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 28;
    }
    status = fdb_payload_v1_payload_acquire(opened, &access, &error);
    if (status != UINT32_C(0) || access == (fdb_payload_v1_access_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 30;
    }
    status = fdb_payload_v1_access_payload_bytes(
        access, &access_data, &access_size, &error);
    if (status != UINT32_C(0) || access_data == (const uint8_t*)0 ||
        access_size != fdb_payload_v1_blob_size(binary) ||
        error != (fdb_payload_v1_error_t*)0) {
        return 31;
    }
    fdb_payload_v1_access_release(access);
    status = fdb_payload_v1_payload_entry_view(
        opened, UINT32_C(0), &sequence, &error);
    if (status != UINT32_C(0) || sequence == (fdb_payload_v1_view_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 32;
    }
    status = fdb_payload_v1_view_kind(sequence, &view_kind, &error);
    if (status != UINT32_C(0) ||
        view_kind != FDB_PAYLOAD_VIEW_SEQUENCE ||
        error != (fdb_payload_v1_error_t*)0) {
        return 33;
    }
    status = fdb_payload_v1_view_length(sequence, &view_length, &error);
    if (status != UINT32_C(0) || view_length != UINT64_C(1) ||
        error != (fdb_payload_v1_error_t*)0) {
        return 34;
    }
    status = fdb_payload_v1_view_at(
        sequence, UINT64_C(0), &value_view, &error);
    if (status != UINT32_C(0) || value_view == (fdb_payload_v1_view_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 35;
    }
    status = fdb_payload_v1_view_get_u8(value_view, &view_u8, &error);
    if (status != UINT32_C(0) || view_u8 != UINT8_C(7) ||
        error != (fdb_payload_v1_error_t*)0) {
        return 36;
    }
    status = fdb_payload_v1_view_materialize(value_view, &detached, &error);
    if (status != UINT32_C(0) || detached == (fdb_payload_v1_view_t*)0 ||
        error != (fdb_payload_v1_error_t*)0) {
        return 37;
    }
    view_u8 = UINT8_C(0);
    status = fdb_payload_v1_view_get_u8(detached, &view_u8, &error);
    if (status != UINT32_C(0) || view_u8 != UINT8_C(7) ||
        error != (fdb_payload_v1_error_t*)0) {
        return 38;
    }
    fdb_payload_v1_view_release(detached);
    fdb_payload_v1_view_release(value_view);
    fdb_payload_v1_view_release(sequence);
    fdb_payload_v1_payload_retain(opened);
    fdb_payload_v1_payload_release(opened);
    status = fdb_payload_v1_payload_invalidate(opened, &error);
    if (status != UINT32_C(0) || error != (fdb_payload_v1_error_t*)0) {
        return 29;
    }
    fdb_payload_v1_payload_release(opened);
    fdb_payload_v1_blob_release(binary);
    fdb_payload_v1_payload_release(payload);
    fdb_payload_v1_plan_release(plan);
    fdb_payload_v1_spec_release(spec);

    return 0;
}
