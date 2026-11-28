#include "JniSupport.hpp"

using namespace fastdb::java;

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewRetain)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_view_retain(pointer<fdb_payload_v1_view_t>(handle));
    return handle;
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(viewRelease)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_view_release(pointer<fdb_payload_v1_view_t>(handle));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(viewRequireDigest)(
    JNIEnv* env, jclass, jlong handle, jbyteArray expected) {
    require_digest(env, fdb_payload_v1_view_require_spec_sha256, pointer<fdb_payload_v1_view_t>(handle), expected);
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(viewKind)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_view_kind, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jboolean JNICALL FASTDB_JNI(viewIsNull)(
    JNIEnv* env, jclass, jlong handle) {
    return (query<uint8_t>(env, fdb_payload_v1_view_is_null, pointer<fdb_payload_v1_view_t>(handle)) != 0) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewLength)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jlong>(query<uint64_t>(env, fdb_payload_v1_view_length, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(viewComponentIndex)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_view_component_index, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(viewFieldCount)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_view_field_count, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jboolean JNICALL FASTDB_JNI(viewGetBool)(
    JNIEnv* env, jclass, jlong handle) {
    return (query<uint8_t>(env, fdb_payload_v1_view_get_bool, pointer<fdb_payload_v1_view_t>(handle)) != 0) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(viewGetU8)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<uint8_t>(env, fdb_payload_v1_view_get_u8, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(viewGetU16)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<uint16_t>(env, fdb_payload_v1_view_get_u16, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewGetU32)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jlong>(query<uint32_t>(env, fdb_payload_v1_view_get_u32, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(viewGetI32)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<int32_t>(env, fdb_payload_v1_view_get_i32, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewGetU8n)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jlong>(query<uint64_t>(env, fdb_payload_v1_view_get_u8n_f64_bits, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewGetU16n)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jlong>(query<uint64_t>(env, fdb_payload_v1_view_get_u16n_f64_bits, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(viewGetF32)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<uint32_t>(env, fdb_payload_v1_view_get_f32_bits, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewGetF64)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jlong>(query<uint64_t>(env, fdb_payload_v1_view_get_f64_bits, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewAt)(
    JNIEnv* env, jclass, jlong handle, jlong index) {
    return address(query<fdb_payload_v1_view_t*>(env, fdb_payload_v1_view_at, pointer<fdb_payload_v1_view_t>(handle), static_cast<uint64_t>(index)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewField)(
    JNIEnv* env, jclass, jlong handle, jint index) {
    return address(query<fdb_payload_v1_view_t*>(env, fdb_payload_v1_view_field, pointer<fdb_payload_v1_view_t>(handle), static_cast<uint32_t>(index)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewRefTarget)(
    JNIEnv* env, jclass, jlong handle) {
    return address(query<fdb_payload_v1_view_t*>(env, fdb_payload_v1_view_ref_target, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewMaterialize)(
    JNIEnv* env, jclass, jlong handle) {
    return address(query<fdb_payload_v1_view_t*>(env, fdb_payload_v1_view_materialize, pointer<fdb_payload_v1_view_t>(handle)));
}

extern "C" JNIEXPORT jlongArray JNICALL FASTDB_JNI(viewGraphIdentity)(
    JNIEnv* env, jclass, jlong handle) {

    uint32_t component = 0;
    uint64_t object = 0;
    if (!call(env, fdb_payload_v1_view_graph_identity, pointer<fdb_payload_v1_view_t>(handle), &component, &object)) {
        return nullptr;
    }
    return longs(env, {component, static_cast<jlong>(object)});
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(viewAcquire)(
    JNIEnv* env, jclass, jlong handle) {
    return address(query<fdb_payload_v1_access_t*>(env, fdb_payload_v1_view_acquire, pointer<fdb_payload_v1_view_t>(handle)));
}
