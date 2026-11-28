
#pragma once

#include <jni.h>

#include "fastdb_payload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>

#define FASTDB_JNI(name) Java_io_fastdb_payload_FastDbNative_##name

namespace fastdb::java {

template<class T>
T* pointer(jlong value) {
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(value));
}

template<class T>
jlong address(T* value) {
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(value));
}

// ThrowNew is used only for fixed ASCII messages. Core text takes the
// standard UTF-8 byte-array path through FastDbException.
void throw_new(JNIEnv* env, const char* type, const char* message);
bool check(JNIEnv* env, fdb_payload_v1_status_t status, fdb_payload_v1_error_t* error);
bool fits_java_buffer(JNIEnv* env, uint64_t size);
jbyteArray bytes(JNIEnv* env, const uint8_t* data, uint64_t size);
jbyteArray blob(JNIEnv* env, fdb_payload_v1_blob_t* owned);
jlongArray longs(JNIEnv* env, std::initializer_list<jlong> fields);

template<class Fn, class... Args>
bool call(JNIEnv* env, Fn fn, Args... args) {
    fdb_payload_v1_error_t* error = nullptr;
    const auto status = fn(args..., &error);
    return check(env, status, error);
}

template<class Out, class Fn, class... Args>
Out query(JNIEnv* env, Fn fn, Args... args) {
    Out output{};
    call(env, fn, args..., &output);
    return output;
}

template<class Fn, class... Args>
jbyteArray digest(JNIEnv* env, Fn fn, Args... args) {
    std::array<uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> value{};
    if (!call(env, fn, args..., value.data())) {
        return nullptr;
    }
    return bytes(env, value.data(), value.size());
}

class ByteArray final {
public:
    ByteArray(JNIEnv* env, jbyteArray array, jint offset, jint size);
    ByteArray(const ByteArray&) = delete;
    ByteArray& operator=(const ByteArray&) = delete;

    bool valid() const { return valid_; }
    uint64_t size() const { return size_; }
    const uint8_t* data() const {
        return data_ ? reinterpret_cast<const uint8_t*>(data_.get()) : nullptr;
    }

private:
    std::unique_ptr<jbyte[]> data_;
    uint64_t size_ = 0;
    bool valid_ = false;
};

template<class Fn, class T>
void require_digest(JNIEnv* env, Fn fn, T* handle, jbyteArray expected) {
    if (env->GetArrayLength(expected) != static_cast<jsize>(FDB_PAYLOAD_V1_SHA256_SIZE)) {
        throw_new(env, "java/lang/IllegalArgumentException", "Expected a 32-byte SHA-256 digest");
        return;
    }
    std::array<jbyte, FDB_PAYLOAD_V1_SHA256_SIZE> value{};
    env->GetByteArrayRegion(expected, 0, static_cast<jsize>(value.size()), value.data());
    if (!env->ExceptionCheck()) {
        call(env, fn, handle, reinterpret_cast<const uint8_t*>(value.data()));
    }
}

template<std::size_t N>
bool option_values(JNIEnv* env, jlongArray source, std::array<jlong, N>& values) {
    if (env->GetArrayLength(source) != static_cast<jsize>(values.size())) {
        throw_new(env, "java/lang/IllegalArgumentException", "Invalid FastDB options");
        return false;
    }
    env->GetLongArrayRegion(source, 0, static_cast<jsize>(values.size()), values.data());
    return !env->ExceptionCheck();
}

struct DirectSpan {
    uint8_t* data = nullptr;
    bool valid = false;
};

DirectSpan direct_span(JNIEnv* env, jobject buffer, jint offset, jint length);

struct GlobalBuffer;
struct GlobalBufferDeleter {
    void operator()(GlobalBuffer* owner) const noexcept;
};
using BufferOwner = std::unique_ptr<GlobalBuffer, GlobalBufferDeleter>;
BufferOwner pin_buffer(JNIEnv* env, jobject buffer);
fdb_payload_v1_backing_v1_t java_buffer_backing();

inline std::array<jlong, 7> report_fields(const fdb_payload_v1_execution_report_t& report) {
    return {report.mode, report.fallback_reason, static_cast<jlong>(report.requested_bytes),
            static_cast<jlong>(report.used_bytes), static_cast<jlong>(report.staging_bytes),
            static_cast<jlong>(report.region_count), static_cast<jlong>(report.backing_capacity)};
}

}  // namespace fastdb::java
