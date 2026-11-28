#include "JniSupport.hpp"

#include <type_traits>

using namespace fastdb::java;

namespace {
struct AccessSpan {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    bool valid = false;
};

AccessSpan access_span(JNIEnv* env, fdb_payload_v1_access_t* access, jint kind) {
    AccessSpan span;
    const auto code = static_cast<uint32_t>(kind);
    if (code == FDB_PAYLOAD_VIEW_WSTR) {
        const uint16_t* units = nullptr;
        uint64_t count = 0;
        span.valid = call(env, fdb_payload_v1_access_wstr, access, &units, &count);
        span.data = reinterpret_cast<const uint8_t*>(units);
        span.size = count * sizeof(*units);
        return span;
    }
    auto read = fdb_payload_v1_access_payload_bytes;
    if (code == FDB_PAYLOAD_VIEW_STR) {
        read = fdb_payload_v1_access_str;
    } else if (code == FDB_PAYLOAD_VIEW_BYTES) {
        read = fdb_payload_v1_access_bytes;
    }
    span.valid = call(env, read, access, &span.data, &span.size);
    return span;
}
}  // namespace

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(accessRelease)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_access_release(pointer<fdb_payload_v1_access_t>(handle));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(accessByteLength)(
    JNIEnv* env, jclass, jlong handle, jint kind) {

    const auto span = access_span(env, pointer<fdb_payload_v1_access_t>(handle), kind);
    if (!span.valid) {
        return 0;
    }
    return static_cast<jlong>(span.size);
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(accessDataAddress)(
    JNIEnv* env, jclass, jlong handle, jint kind) {

    const auto span = access_span(env, pointer<fdb_payload_v1_access_t>(handle), kind);
    if (!span.valid) {
        return 0;
    }
    return address(span.data);
}

extern "C" JNIEXPORT jbyteArray JNICALL FASTDB_JNI(accessToArray)(
    JNIEnv* env, jclass, jlong handle, jint kind) {

    const auto span = access_span(env, pointer<fdb_payload_v1_access_t>(handle), kind);
    if (!span.valid) {
        return nullptr;
    }
    return bytes(env, span.data, span.size);
}

extern "C" JNIEXPORT jobject JNICALL FASTDB_JNI(accessDirectBuffer)(
    JNIEnv* env, jclass, jlong handle, jint kind) {

    const auto span = access_span(env, pointer<fdb_payload_v1_access_t>(handle), kind);
    if (!span.valid) {
        return nullptr;
    }
    if (!fits_java_buffer(env, span.size)) {
        return nullptr;
    }
    // JNI requires a non-null address even for a zero-length buffer.
    static uint8_t empty = 0;
    auto* data = span.size == 0 ? &empty : const_cast<uint8_t*>(span.data);
    return env->NewDirectByteBuffer(data, static_cast<jlong>(span.size));
}

extern "C" JNIEXPORT jstring JNICALL FASTDB_JNI(accessWstr)(
    JNIEnv* env, jclass, jlong handle) {

    static_assert(std::is_same_v<jchar, uint16_t>);
    const uint16_t* units = nullptr;
    uint64_t count = 0;
    if (!call(env, fdb_payload_v1_access_wstr, pointer<fdb_payload_v1_access_t>(handle), &units, &count) ||
        !fits_java_buffer(env, count)) {
        return nullptr;
    }
    constexpr jchar empty = 0;
    return env->NewString(count == 0 ? &empty : units, static_cast<jsize>(count));
}
