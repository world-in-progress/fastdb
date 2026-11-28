
#include "JniSupport.hpp"

#include <atomic>
#include <new>

namespace fastdb::java {
namespace {
JavaVM* vm = nullptr;

using ErrorText = void (*)(const fdb_payload_v1_error_t*, const uint8_t**, uint64_t*);

jbyteArray error_text(JNIEnv* env, const fdb_payload_v1_error_t* error, ErrorText field) {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    field(error, &data, &size);
    return bytes(env, data, size);
}
}  // namespace

void throw_new(JNIEnv* env, const char* type, const char* message) {
    if (auto cls = env->FindClass(type)) {
        env->ThrowNew(cls, message);
    }
}

bool check(JNIEnv* env, fdb_payload_v1_status_t status, fdb_payload_v1_error_t* raw_error) {
    std::unique_ptr<fdb_payload_v1_error_t, decltype(&fdb_payload_v1_error_release)>
        error(raw_error, fdb_payload_v1_error_release);
    if (status == 0) {
        return true;
    }
    if (!error) {
        throw_new(env, "java/lang/IllegalStateException", "FastDB C ABI did not publish an error");
        return false;
    }
    const auto cls = env->FindClass("io/fastdb/payload/FastDbException");
    if (cls == nullptr) {
        return false;
    }
    const auto ctor = env->GetMethodID(cls, "<init>", "(I[B[B[B[B)V");
    if (ctor == nullptr) {
        return false;
    }
    const auto symbol = error_text(env, error.get(), fdb_payload_v1_error_symbol);
    if (symbol == nullptr) {
        return false;
    }
    const auto path = error_text(env, error.get(), fdb_payload_v1_error_path);
    if (path == nullptr) {
        return false;
    }
    const auto message = error_text(env, error.get(), fdb_payload_v1_error_message);
    if (message == nullptr) {
        return false;
    }
    const auto details = error_text(env, error.get(), fdb_payload_v1_error_details_json);
    if (details == nullptr) {
        return false;
    }
    const auto exception = env->NewObject(cls, ctor, static_cast<jint>(fdb_payload_v1_error_code(error.get())),
                                          symbol, path, message, details);
    if (exception != nullptr) {
        env->Throw(static_cast<jthrowable>(exception));
    }
    return false;
}

bool fits_java_buffer(JNIEnv* env, uint64_t size) {
    if (size > static_cast<uint64_t>(std::numeric_limits<jsize>::max())) {
        throw_new(env, "java/lang/IllegalStateException", "FastDB span exceeds Java buffer capacity");
        return false;
    }
    return true;
}

jbyteArray bytes(JNIEnv* env, const uint8_t* data, uint64_t size) {
    if (!fits_java_buffer(env, size)) {
        return nullptr;
    }
    const auto length = static_cast<jsize>(size);
    auto result = env->NewByteArray(length);
    if (result != nullptr && length != 0) {
        env->SetByteArrayRegion(result, 0, length, reinterpret_cast<const jbyte*>(data));
    }
    return result;
}

jbyteArray blob(JNIEnv* env, fdb_payload_v1_blob_t* owned) {
    std::unique_ptr<fdb_payload_v1_blob_t, decltype(&fdb_payload_v1_blob_release)>
        blob_handle(owned, fdb_payload_v1_blob_release);
    if (!blob_handle) {
        return nullptr;  // An exception was posted by the ABI call.
    }
    return bytes(env, fdb_payload_v1_blob_data(owned), fdb_payload_v1_blob_size(owned));
}

jlongArray longs(JNIEnv* env, std::initializer_list<jlong> fields) {
    const auto size = static_cast<jsize>(fields.size());
    auto result = env->NewLongArray(size);
    if (result != nullptr) {
        env->SetLongArrayRegion(result, 0, size, fields.begin());
    }
    return result;
}

ByteArray::ByteArray(JNIEnv* env, jbyteArray array, jint offset, jint size) {
    const auto capacity = env->GetArrayLength(array);
    if (offset < 0 || size < 0 || offset > capacity || size > capacity - offset) {
        throw_new(env, "java/lang/IndexOutOfBoundsException", "Invalid byte array span");
        return;
    }
    size_ = static_cast<uint64_t>(size);
    if (size != 0) {
        data_.reset(new (std::nothrow) jbyte[static_cast<std::size_t>(size)]);
        if (!data_) {
            throw_new(env, "java/lang/OutOfMemoryError", "Cannot copy FastDB byte array span");
            return;
        }
        env->GetByteArrayRegion(array, offset, size, data_.get());
        if (env->ExceptionCheck()) {
            data_.reset();
            return;
        }
    }
    valid_ = true;
}

DirectSpan direct_span(JNIEnv* env, jobject buffer, jint offset, jint length) {
    const auto capacity = env->GetDirectBufferCapacity(buffer);
    if (offset < 0 || length < 0 || capacity < offset || length > capacity - offset) {
        throw_new(env, "java/lang/IllegalArgumentException", "Invalid direct ByteBuffer span");
        return {};
    }
    auto* data = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
    if (data == nullptr && length != 0) {
        throw_new(env, "java/lang/IllegalArgumentException", "Direct ByteBuffer has no accessible address");
        return {};
    }
    return {data == nullptr ? nullptr : data + offset, true};
}

struct GlobalBuffer {
    jobject reference = nullptr;
    std::atomic<unsigned> owners{1};
};

extern "C" fdb_payload_v1_status_t retain_java_buffer(void*, void* token) noexcept {
    auto* owner = static_cast<GlobalBuffer*>(token);
    owner->owners.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

extern "C" void release_java_buffer(void*, void* token) noexcept {
    auto* owner = static_cast<GlobalBuffer*>(token);
    if (owner->owners.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    void* raw_env = nullptr;
    const auto state = vm->GetEnv(&raw_env, JNI_VERSION_1_8);
    const bool needs_attach = state == JNI_EDETACHED;
    if (needs_attach) {
        if (vm->AttachCurrentThreadAsDaemon(&raw_env, nullptr) != JNI_OK) {
            // The JVM may refuse attachment during shutdown.
            delete owner;
            return;
        }
    } else if (state != JNI_OK) {
        delete owner;
        return;
    }
    static_cast<JNIEnv*>(raw_env)->DeleteGlobalRef(owner->reference);
    delete owner;
    if (needs_attach) {
        vm->DetachCurrentThread();
    }
}

void GlobalBufferDeleter::operator()(GlobalBuffer* owner) const noexcept {
    release_java_buffer(nullptr, owner);
}

BufferOwner pin_buffer(JNIEnv* env, jobject buffer) {
    auto owner = std::unique_ptr<GlobalBuffer>(new (std::nothrow) GlobalBuffer);
    if (!owner) {
        throw_new(env, "java/lang/OutOfMemoryError", "Cannot allocate FastDB buffer owner");
        return {};
    }
    owner->reference = env->NewGlobalRef(buffer);
    if (owner->reference == nullptr) {
        if (!env->ExceptionCheck()) {
            throw_new(env, "java/lang/OutOfMemoryError", "Cannot pin FastDB direct buffer");
        }
        return {};
    }
    return BufferOwner(owner.release());
}

fdb_payload_v1_backing_v1_t java_buffer_backing() {
    fdb_payload_v1_backing_v1_t backing{};
    fdb_payload_v1_backing_init(&backing);
    backing.retain = retain_java_buffer;
    backing.release = release_java_buffer;
    return backing;
}

}  // namespace fastdb::java

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    if (fdb_payload_v1_abi_version() != FDB_PAYLOAD_V1_ABI_VERSION) {
        return JNI_ERR;
    }
    fastdb::java::vm = vm;
    return JNI_VERSION_1_8;
}
