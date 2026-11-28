#include "JniSupport.hpp"

using namespace fastdb::java;

namespace {
bool compile_options(JNIEnv* env, jlongArray raw, fdb_payload_v1_compile_options_t& options) {
    fdb_payload_v1_compile_options_init(&options);
    if (raw == nullptr) {
        return true;
    }
    std::array<jlong, 8> values{};
    if (!option_values(env, raw, values)) {
        return false;
    }
    options.flags = static_cast<uint32_t>(values[0]);
    options.max_source_bytes = static_cast<uint64_t>(values[1]);
    options.max_json_values = static_cast<uint64_t>(values[2]);
    options.max_nesting_depth = static_cast<uint32_t>(values[3]);
    options.max_entries = static_cast<uint32_t>(values[4]);
    options.max_components = static_cast<uint32_t>(values[5]);
    options.max_fields_per_component = static_cast<uint32_t>(values[6]);
    options.max_total_fields = static_cast<uint64_t>(values[7]);
    return true;
}

bool codegen_options(JNIEnv* env, jlongArray raw, fdb_payload_v1_codegen_options_t& options) {
    fdb_payload_v1_codegen_options_init(&options);
    if (raw == nullptr) {
        return true;
    }
    std::array<jlong, 3> values{};
    if (!option_values(env, raw, values)) {
        return false;
    }
    options.flags = static_cast<uint32_t>(values[0]);
    options.max_artifacts = static_cast<uint64_t>(values[1]);
    options.max_total_bytes = static_cast<uint64_t>(values[2]);
    return true;
}
}  // namespace

extern "C" JNIEXPORT jlongArray JNICALL FASTDB_JNI(compileOptions)(
    JNIEnv* env, jclass) {
    fdb_payload_v1_compile_options_t options{};
    fdb_payload_v1_compile_options_init(&options);
    return longs(env, {
        static_cast<jlong>(options.flags),
        static_cast<jlong>(options.max_source_bytes),
        static_cast<jlong>(options.max_json_values),
        static_cast<jlong>(options.max_nesting_depth),
        static_cast<jlong>(options.max_entries),
        static_cast<jlong>(options.max_components),
        static_cast<jlong>(options.max_fields_per_component),
        static_cast<jlong>(options.max_total_fields)
    });
}

extern "C" JNIEXPORT jlongArray JNICALL FASTDB_JNI(codegenOptions)(
    JNIEnv* env, jclass) {
    fdb_payload_v1_codegen_options_t options{};
    fdb_payload_v1_codegen_options_init(&options);
    return longs(env, {
        static_cast<jlong>(options.flags),
        static_cast<jlong>(options.max_artifacts),
        static_cast<jlong>(options.max_total_bytes)
    });
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(specCompile)(
    JNIEnv* env, jclass, jbyteArray source, jint length, jlongArray raw) {

    ByteArray input(env, source, 0, length);
    fdb_payload_v1_compile_options_t options{};
    if (!input.valid() || !compile_options(env, raw, options)) {
        return 0;
    }
    return address(query<fdb_payload_v1_spec_t*>(env, fdb_payload_v1_spec_compile_json,
                                                input.data(), input.size(), &options));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(specRetain)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_spec_retain(pointer<fdb_payload_v1_spec_t>(handle));
    return handle;
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(specRelease)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_spec_release(pointer<fdb_payload_v1_spec_t>(handle));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(artifactResultRelease)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_codegen_result_release(pointer<fdb_payload_v1_codegen_result_t>(handle));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(specSha256)(
    JNIEnv* env, jclass, jlong handle) {
    return digest(env, fdb_payload_v1_spec_sha256, pointer<fdb_payload_v1_spec_t>(handle));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(specCanonicalJson)(
    JNIEnv* env, jclass, jlong handle) {
    return blob(env, query<fdb_payload_v1_blob_t*>(env, fdb_payload_v1_spec_canonical_json, pointer<fdb_payload_v1_spec_t>(handle)));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(specManifestJson)(
    JNIEnv* env, jclass, jlong handle) {
    return blob(env, query<fdb_payload_v1_blob_t*>(env, fdb_payload_v1_spec_manifest_json, pointer<fdb_payload_v1_spec_t>(handle)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(specProfile)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<fdb_payload_v1_profile_t>(env, fdb_payload_v1_spec_profile, pointer<fdb_payload_v1_spec_t>(handle)));
}

extern "C" JNIEXPORT jlongArray JNICALL FASTDB_JNI(specCapabilities)(
    JNIEnv* env, jclass, jlong handle) {

    fdb_payload_v1_capabilities_t value{};
    fdb_payload_v1_capabilities_init(&value);
    if (!call(env, fdb_payload_v1_spec_capabilities, pointer<fdb_payload_v1_spec_t>(handle), &value)) {
        return nullptr;
    }
    return longs(env, {value.profile, static_cast<jlong>(value.semantic_flags),
                       static_cast<jlong>(value.operation_flags), static_cast<jlong>(value.codegen_target_flags),
                       value.direct_build_status});
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(specEntryCount)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_spec_entry_count, pointer<fdb_payload_v1_spec_t>(handle)));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(specEntryId)(
    JNIEnv* env, jclass, jlong handle, jint index) {
    return blob(env, query<fdb_payload_v1_blob_t*>(env, fdb_payload_v1_spec_entry_id, pointer<fdb_payload_v1_spec_t>(handle), static_cast<uint32_t>(index)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(specEntryIndex)(
    JNIEnv* env, jclass, jlong handle, jbyteArray id) {

    ByteArray input(env, id, 0, env->GetArrayLength(id));
    if (!input.valid()) {
        return 0;
    }
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_spec_entry_index,
        pointer<fdb_payload_v1_spec_t>(handle), input.data(), input.size()));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(specComponentCount)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_spec_component_count, pointer<fdb_payload_v1_spec_t>(handle)));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(specComponentId)(
    JNIEnv* env, jclass, jlong handle, jint index) {
    return blob(env, query<fdb_payload_v1_blob_t*>(env, fdb_payload_v1_spec_component_id, pointer<fdb_payload_v1_spec_t>(handle), static_cast<uint32_t>(index)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(specComponentIndex)(
    JNIEnv* env, jclass, jlong handle, jbyteArray id) {

    ByteArray input(env, id, 0, env->GetArrayLength(id));
    if (!input.valid()) {
        return 0;
    }
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_spec_component_index,
        pointer<fdb_payload_v1_spec_t>(handle), input.data(), input.size()));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(specComponentFieldCount)(
    JNIEnv* env, jclass, jlong handle, jint component) {
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_spec_component_field_count, pointer<fdb_payload_v1_spec_t>(handle), static_cast<uint32_t>(component)));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(specComponentFieldId)(
    JNIEnv* env, jclass, jlong handle, jint component, jint index) {
    return blob(env, query<fdb_payload_v1_blob_t*>(env, fdb_payload_v1_spec_component_field_id, pointer<fdb_payload_v1_spec_t>(handle), static_cast<uint32_t>(component), static_cast<uint32_t>(index)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(specComponentFieldIndex)(
    JNIEnv* env, jclass, jlong handle, jint component, jbyteArray id) {

    ByteArray input(env, id, 0, env->GetArrayLength(id));
    if (!input.valid()) {
        return 0;
    }
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_spec_component_field_index,
        pointer<fdb_payload_v1_spec_t>(handle), static_cast<uint32_t>(component), input.data(), input.size()));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(sourceSchemaJson)(
    JNIEnv* env, jclass) {
    return blob(env, query<fdb_payload_v1_blob_t*>(env, fdb_payload_v1_source_schema_json));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(sourceSchemaSha256)(
    JNIEnv* env, jclass) {
    return digest(env, fdb_payload_v1_source_schema_sha256);
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(specGenerate)(
    JNIEnv* env, jclass, jlong handle, jlong target, jlongArray raw) {

    fdb_payload_v1_codegen_options_t options{};
    if (!codegen_options(env, raw, options)) {
        return 0;
    }
    return address(query<fdb_payload_v1_codegen_result_t*>(env, fdb_payload_v1_spec_codegen,
        pointer<fdb_payload_v1_spec_t>(handle), static_cast<uint64_t>(target), &options));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(artifactCount)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jlong>(query<uint64_t>(env, fdb_payload_v1_codegen_result_artifact_count, pointer<fdb_payload_v1_codegen_result_t>(handle)));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(artifactPath)(
    JNIEnv* env, jclass, jlong handle, jlong index) {
    return blob(env, query<fdb_payload_v1_blob_t*>(env, fdb_payload_v1_codegen_result_artifact_relative_path, pointer<fdb_payload_v1_codegen_result_t>(handle), static_cast<uint64_t>(index)));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(artifactBytes)(
    JNIEnv* env, jclass, jlong handle, jlong index) {
    return blob(env, query<fdb_payload_v1_blob_t*>(env, fdb_payload_v1_codegen_result_artifact_bytes, pointer<fdb_payload_v1_codegen_result_t>(handle), static_cast<uint64_t>(index)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(artifactKind)(
    JNIEnv* env, jclass, jlong handle, jlong index) {
    return static_cast<jint>(query<fdb_payload_v1_artifact_kind_t>(env, fdb_payload_v1_codegen_result_artifact_kind, pointer<fdb_payload_v1_codegen_result_t>(handle), static_cast<uint64_t>(index)));
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(artifactSha256)(
    JNIEnv* env, jclass, jlong handle, jlong index) {
    return digest(env, fdb_payload_v1_codegen_result_artifact_sha256, pointer<fdb_payload_v1_codegen_result_t>(handle), static_cast<uint64_t>(index));
}
