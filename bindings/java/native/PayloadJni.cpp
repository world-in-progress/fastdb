#include "JniSupport.hpp"

using namespace fastdb::java;

namespace {
bool open_options(JNIEnv* env, jlongArray raw, fdb_payload_v1_open_options_t& options) {
    fdb_payload_v1_open_options_init(&options);
    if (raw == nullptr) {
        return true;
    }
    std::array<jlong, 10> values{};
    if (!option_values(env, raw, values)) {
        return false;
    }
    options.flags = static_cast<uint32_t>(values[0]);
    options.max_total_bytes = static_cast<uint64_t>(values[1]);
    options.max_regions = static_cast<uint64_t>(values[2]);
    options.max_entries = static_cast<uint64_t>(values[3]);
    options.max_components = static_cast<uint64_t>(values[4]);
    options.max_nesting_depth = static_cast<uint64_t>(values[5]);
    options.max_list_elements = static_cast<uint64_t>(values[6]);
    options.max_graph_objects = static_cast<uint64_t>(values[7]);
    options.max_string_bytes = static_cast<uint64_t>(values[8]);
    options.max_validation_work = static_cast<uint64_t>(values[9]);
    return true;
}
}  // namespace

extern "C" JNIEXPORT jlongArray JNICALL FASTDB_JNI(openOptions)(
    JNIEnv* env, jclass) {
    fdb_payload_v1_open_options_t options{};
    fdb_payload_v1_open_options_init(&options);
    return longs(env, {
        static_cast<jlong>(options.flags),
        static_cast<jlong>(options.max_total_bytes),
        static_cast<jlong>(options.max_regions),
        static_cast<jlong>(options.max_entries),
        static_cast<jlong>(options.max_components),
        static_cast<jlong>(options.max_nesting_depth),
        static_cast<jlong>(options.max_list_elements),
        static_cast<jlong>(options.max_graph_objects),
        static_cast<jlong>(options.max_string_bytes),
        static_cast<jlong>(options.max_validation_work)
    });
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(payloadOpenArray)(
    JNIEnv* env, jclass, jlong handle, jbyteArray value, jint offset, jint length, jlongArray raw) {

    ByteArray input(env, value, offset, length);
    fdb_payload_v1_open_options_t options{};
    if (!input.valid() || !open_options(env, raw, options)) {
        return 0;
    }
    return address(query<fdb_payload_v1_payload_t*>(env, fdb_payload_v1_payload_open_copy,
        pointer<fdb_payload_v1_spec_t>(handle), input.data(), input.size(), &options));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(payloadOpenDirect)(
    JNIEnv* env, jclass, jlong handle, jobject buffer, jint offset, jint length, jlongArray raw, jboolean external) {

    const auto input = direct_span(env, buffer, offset, length);
    fdb_payload_v1_open_options_t options{};
    if (!input.valid || !open_options(env, raw, options)) {
        return 0;
    }
    if (external != JNI_TRUE) {
        return address(query<fdb_payload_v1_payload_t*>(env, fdb_payload_v1_payload_open_copy,
            pointer<fdb_payload_v1_spec_t>(handle), input.data, static_cast<uint64_t>(length), &options));
    }
    const auto owner = pin_buffer(env, buffer);
    if (!owner) {
        return 0;
    }
    const auto backing = java_buffer_backing();
    return address(query<fdb_payload_v1_payload_t*>(env, fdb_payload_v1_payload_open_external,
        pointer<fdb_payload_v1_spec_t>(handle), input.data, static_cast<uint64_t>(length), &backing, owner.get(), &options));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(payloadOpenNative)(
    JNIEnv* env, jclass, jlong handle, jlong data, jlong length, jlong retain, jlong release, jlong owner, jlongArray raw) {

    fdb_payload_v1_open_options_t options{};
    if (!open_options(env, raw, options)) {
        return 0;
    }
    fdb_payload_v1_backing_v1_t backing{};
    fdb_payload_v1_backing_init(&backing);
    backing.retain = reinterpret_cast<fdb_payload_v1_backing_retain_fn>(static_cast<std::uintptr_t>(retain));
    backing.release = reinterpret_cast<fdb_payload_v1_backing_release_fn>(static_cast<std::uintptr_t>(release));
    return address(query<fdb_payload_v1_payload_t*>(env, fdb_payload_v1_payload_open_external,
        pointer<fdb_payload_v1_spec_t>(handle), pointer<const uint8_t>(data), static_cast<uint64_t>(length),
        &backing, pointer<void>(owner), &options));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(payloadRetain)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_payload_retain(pointer<fdb_payload_v1_payload_t>(handle));
    return handle;
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(payloadRelease)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_payload_release(pointer<fdb_payload_v1_payload_t>(handle));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(payloadRequireDigest)(
    JNIEnv* env, jclass, jlong handle, jbyteArray expected) {
    require_digest(env, fdb_payload_v1_payload_require_spec_sha256, pointer<fdb_payload_v1_payload_t>(handle), expected);
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(payloadSha256)(
    JNIEnv* env, jclass, jlong handle) {
    return digest(env, fdb_payload_v1_payload_sha256, pointer<fdb_payload_v1_payload_t>(handle));
}

extern "C" JNIEXPORT jint JNICALL FASTDB_JNI(payloadProfile)(
    JNIEnv* env, jclass, jlong handle) {
    return static_cast<jint>(query<fdb_payload_v1_profile_t>(env, fdb_payload_v1_payload_profile, pointer<fdb_payload_v1_payload_t>(handle)));
}

extern "C" JNIEXPORT jlongArray JNICALL FASTDB_JNI(payloadReport)(
    JNIEnv* env, jclass, jlong handle) {

    fdb_payload_v1_execution_report_t report{};
    fdb_payload_v1_execution_report_init(&report);
    if (!call(env, fdb_payload_v1_payload_execution_report, pointer<fdb_payload_v1_payload_t>(handle), &report)) {
        return nullptr;
    }
    const auto fields = report_fields(report);
    return longs(env, {fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6]});
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(payloadAcquire)(
    JNIEnv* env, jclass, jlong handle) {
    return address(query<fdb_payload_v1_access_t*>(env, fdb_payload_v1_payload_acquire, pointer<fdb_payload_v1_payload_t>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(payloadEntryView)(
    JNIEnv* env, jclass, jlong handle, jint index) {
    return address(query<fdb_payload_v1_view_t*>(env, fdb_payload_v1_payload_entry_view, pointer<fdb_payload_v1_payload_t>(handle), static_cast<uint32_t>(index)));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(payloadInvalidate)(
    JNIEnv* env, jclass, jlong handle) {
    call(env, fdb_payload_v1_payload_invalidate, pointer<fdb_payload_v1_payload_t>(handle));
}
