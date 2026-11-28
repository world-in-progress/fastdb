#include <jni.h>

#include "fastdb_payload.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <system_error>
#include <thread>

namespace {
struct Fixture {
    ~Fixture() {
        free_data();
    }

    void free_data() noexcept {
        if (data != nullptr) {
            ::operator delete(data, std::align_val_t(alignment));
            data = nullptr;
        }
    }

    bool allocate(uint64_t bytes, std::size_t required_alignment) noexcept {
        alignment = std::max(required_alignment, alignof(std::max_align_t));
        data = static_cast<uint8_t*>(::operator new(static_cast<std::size_t>(bytes),
                                                     std::align_val_t(alignment), std::nothrow));
        capacity = bytes;
        return data != nullptr;
    }

    uint8_t* data = nullptr;
    uint64_t capacity = 0;
    std::size_t alignment = alignof(std::max_align_t);
    bool writable = false;
    std::atomic<jlong> retains{0};
    std::atomic<jlong> releases{0};
    std::atomic<jlong> reserves{0};
    std::atomic<jlong> commits{0};
    std::atomic<jlong> rollbacks{0};
    fdb_payload_v1_backing_v1_t table{};
};

Fixture* fixture(jlong value) {
    return reinterpret_cast<Fixture*>(static_cast<std::uintptr_t>(value));
}

template<class T>
jlong address(T* value) {
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(value));
}

void out_of_memory(JNIEnv* env) {
    if (const auto cls = env->FindClass("java/lang/OutOfMemoryError")) {
        env->ThrowNew(cls, "Cannot allocate backing fixture");
    }
}

jlongArray longs(JNIEnv* env, const jlong* values, jsize count) {
    auto result = env->NewLongArray(count);
    if (result != nullptr) {
        env->SetLongArrayRegion(result, 0, count, values);
    }
    return result;
}
}  // namespace

extern "C" fdb_payload_v1_status_t fixture_retain(void*, void* owner) noexcept {
    ++static_cast<Fixture*>(owner)->retains;
    return 0;
}

extern "C" void fixture_release(void*, void* owner) noexcept {
    auto& state = *static_cast<Fixture*>(owner);
    if (state.writable) {
        state.free_data();
    }
    ++state.releases;
}

extern "C" fdb_payload_v1_status_t fixture_reserve(
    void* context, uint32_t mode, uint64_t minimum, uint32_t alignment,
    void** owner, uint8_t** writable, uint64_t* capacity) noexcept {
    auto& state = *static_cast<Fixture*>(context);
    if (mode != FDB_PAYLOAD_RESERVE_DIRECT) {
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }
    if (state.data != nullptr || !state.allocate(minimum, alignment)) {
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }
    ++state.reserves;
    *owner = &state;
    *writable = state.data;
    *capacity = state.capacity;
    return 0;
}

extern "C" fdb_payload_v1_status_t fixture_commit(
    void*, void* token, uint64_t used, const uint8_t** data, uint64_t* size) noexcept {
    auto& state = *static_cast<Fixture*>(token);
    if (used > state.capacity) {
        return FDB_PAYLOAD_E_COMMIT_FAILED;
    }
    ++state.commits;
    *data = state.data;
    *size = used;
    return 0;
}

extern "C" fdb_payload_v1_status_t fixture_rollback(void*, void* owner) noexcept {
    auto& state = *static_cast<Fixture*>(owner);
    state.free_data();
    ++state.rollbacks;
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL Java_io_fastdb_payload_NativeBackingTest_create(
    JNIEnv* env, jclass, jbyteArray bytes) {
    auto* state = new (std::nothrow) Fixture;
    if (state == nullptr) {
        out_of_memory(env);
        return 0;
    }
    if (bytes != nullptr) {
        const auto length = env->GetArrayLength(bytes);
        if (!state->allocate(static_cast<uint64_t>(length), alignof(std::max_align_t))) {
            delete state;
            out_of_memory(env);
            return 0;
        }
        env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(state->data));
        if (env->ExceptionCheck()) {
            delete state;
            return 0;
        }
    } else {
        state->writable = true;
        state->table.struct_size = static_cast<uint32_t>(sizeof(state->table));
        state->table.context = state;
        state->table.reserve = fixture_reserve;
        state->table.commit = fixture_commit;
        state->table.rollback = fixture_rollback;
        state->table.release = fixture_release;
    }
    return address(state);
}

extern "C" JNIEXPORT void JNICALL Java_io_fastdb_payload_NativeBackingTest_destroy(
    JNIEnv*, jclass, jlong handle) {
    delete fixture(handle);
}

extern "C" JNIEXPORT jlongArray JNICALL Java_io_fastdb_payload_NativeBackingTest_bytesBacking(
    JNIEnv* env, jclass, jlong handle) {
    const auto* state = fixture(handle);
    const jlong values[] = {address(state->data), static_cast<jlong>(state->capacity),
                            address(&fixture_retain), address(&fixture_release), handle};
    return longs(env, values, 5);
}

extern "C" JNIEXPORT jlong JNICALL Java_io_fastdb_payload_NativeBackingTest_tableAddress(
    JNIEnv*, jclass, jlong handle) {
    return address(&fixture(handle)->table);
}

extern "C" JNIEXPORT jlongArray JNICALL Java_io_fastdb_payload_NativeBackingTest_counts(
    JNIEnv* env, jclass, jlong handle) {
    const auto* state = fixture(handle);
    const jlong values[] = {state->retains.load(), state->releases.load(), state->reserves.load(),
                            state->commits.load(), state->rollbacks.load()};
    return longs(env, values, 5);
}

extern "C" JNIEXPORT jlong JNICALL Java_io_fastdb_payload_NativeBackingTest_dataAddress(
    JNIEnv*, jclass, jlong handle) {
    return address(fixture(handle)->data);
}

extern "C" JNIEXPORT jlong JNICALL Java_io_fastdb_payload_NativeBackingTest_directAddress(
    JNIEnv* env, jclass, jobject buffer) {
    return address(env->GetDirectBufferAddress(buffer));
}

extern "C" JNIEXPORT jlongArray JNICALL Java_io_fastdb_payload_NativeBackingTest_invalidateDetached(
    JNIEnv* env, jclass, jlong address) {
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) {
        return nullptr;
    }
    auto* payload = reinterpret_cast<fdb_payload_v1_payload_t*>(static_cast<std::uintptr_t>(address));
    jlong results[3]{};
    try {
        std::thread worker([&] {
            void* thread_env = nullptr;
            results[0] = vm->GetEnv(&thread_env, JNI_VERSION_1_8);
            fdb_payload_v1_error_t* error = nullptr;
            results[1] = fdb_payload_v1_payload_invalidate(payload, &error);
            fdb_payload_v1_error_release(error);
            results[2] = vm->GetEnv(&thread_env, JNI_VERSION_1_8);
            if (results[2] == JNI_OK) {
                vm->DetachCurrentThread();
            }
        });
        worker.join();
    } catch (const std::system_error&) {
        if (const auto cls = env->FindClass("java/lang/IllegalStateException")) {
            env->ThrowNew(cls, "Cannot run detached invalidation thread");
        }
        return nullptr;
    }
    return longs(env, results, 3);
}
