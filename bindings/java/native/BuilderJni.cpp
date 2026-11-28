#include "JniSupport.hpp"

#include <new>
#include <type_traits>

using namespace fastdb::java;

namespace {
bool builder_options(JNIEnv* env, jlongArray raw, fdb_payload_v1_builder_options_t& options) {
    fdb_payload_v1_builder_options_init(&options);
    if (raw == nullptr) {
        return true;
    }
    std::array<jlong, 8> values{};
    if (!option_values(env, raw, values)) {
        return false;
    }
    options.flags = static_cast<uint32_t>(values[0]);
    options.max_value_nodes = static_cast<uint64_t>(values[1]);
    options.max_list_elements = static_cast<uint64_t>(values[2]);
    options.max_text_bytes = static_cast<uint64_t>(values[3]);
    options.max_opaque_bytes = static_cast<uint64_t>(values[4]);
    options.max_nesting_depth = static_cast<uint64_t>(values[5]);
    options.max_total_builder_bytes = static_cast<uint64_t>(values[6]);
    options.max_graph_objects = static_cast<uint64_t>(values[7]);
    return true;
}
}  // namespace

extern "C" JNIEXPORT jlongArray JNICALL FASTDB_JNI(builderOptions)(
    JNIEnv* env, jclass) {
    fdb_payload_v1_builder_options_t options{};
    fdb_payload_v1_builder_options_init(&options);
    return longs(env, {
        static_cast<jlong>(options.flags),
        static_cast<jlong>(options.max_value_nodes),
        static_cast<jlong>(options.max_list_elements),
        static_cast<jlong>(options.max_text_bytes),
        static_cast<jlong>(options.max_opaque_bytes),
        static_cast<jlong>(options.max_nesting_depth),
        static_cast<jlong>(options.max_total_builder_bytes),
        static_cast<jlong>(options.max_graph_objects)
    });
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(builderCreate)(
    JNIEnv* env, jclass, jlong handle, jlongArray raw) {

    fdb_payload_v1_builder_options_t options{};
    if (!builder_options(env, raw, options)) {
        return 0;
    }
    return address(query<fdb_payload_v1_builder_t*>(env, fdb_payload_v1_builder_create,
                                                   pointer<fdb_payload_v1_spec_t>(handle), &options));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderRelease)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_builder_release(pointer<fdb_payload_v1_builder_t>(handle));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderRequireDigest)(
    JNIEnv* env, jclass, jlong handle, jbyteArray expected) {
    require_digest(env, fdb_payload_v1_builder_require_spec_sha256, pointer<fdb_payload_v1_builder_t>(handle), expected);
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderEntryBegin)(
    JNIEnv* env, jclass, jlong handle, jint index, jlong count) {
    call(env, fdb_payload_v1_builder_entry_begin, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint32_t>(index), static_cast<uint64_t>(count));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(builderDeclareObject)(
    JNIEnv* env, jclass, jlong handle, jint component) {
    return static_cast<jlong>(query<fdb_payload_v1_object_handle_t>(env, fdb_payload_v1_builder_object_declare, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint32_t>(component)));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueNull)(
    JNIEnv* env, jclass, jlong handle) {
    call(env, fdb_payload_v1_builder_value_null, pointer<fdb_payload_v1_builder_t>(handle));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueBool)(
    JNIEnv* env, jclass, jlong handle, jboolean value) {
    call(env, fdb_payload_v1_builder_value_bool, pointer<fdb_payload_v1_builder_t>(handle), (value == JNI_TRUE ? uint8_t{1} : uint8_t{0}));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueU8)(
    JNIEnv* env, jclass, jlong handle, jint value) {
    call(env, fdb_payload_v1_builder_value_u8, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint8_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueU16)(
    JNIEnv* env, jclass, jlong handle, jint value) {
    call(env, fdb_payload_v1_builder_value_u16, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint16_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueU32)(
    JNIEnv* env, jclass, jlong handle, jlong value) {
    call(env, fdb_payload_v1_builder_value_u32, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint32_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueI32)(
    JNIEnv* env, jclass, jlong handle, jint value) {
    call(env, fdb_payload_v1_builder_value_i32, pointer<fdb_payload_v1_builder_t>(handle), static_cast<int32_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueU8n)(
    JNIEnv* env, jclass, jlong handle, jlong value) {
    call(env, fdb_payload_v1_builder_value_u8n_f64_bits, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint64_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueU16n)(
    JNIEnv* env, jclass, jlong handle, jlong value) {
    call(env, fdb_payload_v1_builder_value_u16n_f64_bits, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint64_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueF32)(
    JNIEnv* env, jclass, jlong handle, jint value) {
    call(env, fdb_payload_v1_builder_value_f32_bits, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint32_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueF64)(
    JNIEnv* env, jclass, jlong handle, jlong value) {
    call(env, fdb_payload_v1_builder_value_f64_bits, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint64_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueComponentBegin)(
    JNIEnv* env, jclass, jlong handle) {
    call(env, fdb_payload_v1_builder_value_component_begin, pointer<fdb_payload_v1_builder_t>(handle));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueListBegin)(
    JNIEnv* env, jclass, jlong handle, jlong value) {
    call(env, fdb_payload_v1_builder_value_list_begin, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint64_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderObjectFillBegin)(
    JNIEnv* env, jclass, jlong handle, jlong value) {
    call(env, fdb_payload_v1_builder_object_fill_begin, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint64_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueObject)(
    JNIEnv* env, jclass, jlong handle, jlong value) {
    call(env, fdb_payload_v1_builder_value_object, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint64_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueRef)(
    JNIEnv* env, jclass, jlong handle, jlong value) {
    call(env, fdb_payload_v1_builder_value_ref, pointer<fdb_payload_v1_builder_t>(handle), static_cast<uint64_t>(value));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueStr)(
    JNIEnv* env, jclass, jlong handle, jbyteArray value) {

    ByteArray input(env, value, 0, env->GetArrayLength(value));
    if (input.valid()) {
        call(env, fdb_payload_v1_builder_value_str, pointer<fdb_payload_v1_builder_t>(handle), input.data(), input.size());
    }
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueWstr)(
    JNIEnv* env, jclass, jlong handle, jstring value) {

    static_assert(std::is_same_v<jchar, uint16_t>);
    const auto count = env->GetStringLength(value);
    std::unique_ptr<jchar[]> units;
    if (count != 0) {
        units.reset(new (std::nothrow) jchar[static_cast<std::size_t>(count)]);
        if (!units) {
            throw_new(env, "java/lang/OutOfMemoryError", "Cannot copy FastDB UTF-16 code units");
            return;
        }
        env->GetStringRegion(value, 0, count, units.get());
        if (env->ExceptionCheck()) {
            return;
        }
    }
    call(env, fdb_payload_v1_builder_value_wstr, pointer<fdb_payload_v1_builder_t>(handle), units.get(), static_cast<uint64_t>(count));
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueBytes)(
    JNIEnv* env, jclass, jlong handle, jbyteArray value, jint offset, jint length) {

    ByteArray input(env, value, offset, length);
    if (input.valid()) {
        call(env, fdb_payload_v1_builder_value_bytes, pointer<fdb_payload_v1_builder_t>(handle), input.data(), input.size());
    }
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(builderValueDirectBytes)(
    JNIEnv* env, jclass, jlong handle, jobject buffer, jint offset, jint length) {

    const auto input = direct_span(env, buffer, offset, length);
    if (!input.valid) {
        return;
    }
    const auto owner = pin_buffer(env, buffer);
    if (!owner) {
        return;
    }
    const auto backing = java_buffer_backing();
    call(env, fdb_payload_v1_builder_value_external_bytes, pointer<fdb_payload_v1_builder_t>(handle), input.data,
         static_cast<uint64_t>(length), &backing, owner.get());
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(builderFreeze)(
    JNIEnv* env, jclass, jlong handle) {
    return address(query<fdb_payload_v1_plan_t*>(env, fdb_payload_v1_builder_freeze, pointer<fdb_payload_v1_builder_t>(handle)));
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(planRetain)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_plan_retain(pointer<fdb_payload_v1_plan_t>(handle));
    return handle;
}

extern "C" JNIEXPORT void JNICALL FASTDB_JNI(planRelease)(
    JNIEnv*, jclass, jlong handle) {
    fdb_payload_v1_plan_release(pointer<fdb_payload_v1_plan_t>(handle));
}

extern "C" JNIEXPORT jlongArray JNICALL FASTDB_JNI(planInfo)(
    JNIEnv* env, jclass, jlong handle) {

    fdb_payload_v1_plan_info_t info{};
    fdb_payload_v1_plan_info_init(&info);
    if (!call(env, fdb_payload_v1_plan_info, pointer<fdb_payload_v1_plan_t>(handle), &info)) {
        return nullptr;
    }
    return longs(env, {info.flags, static_cast<jlong>(info.total_bytes), static_cast<jlong>(info.region_count),
                       static_cast<jlong>(info.logical_value_count), static_cast<jlong>(info.list_element_count),
                       static_cast<jlong>(info.text_bytes), static_cast<jlong>(info.opaque_bytes),
                       static_cast<jlong>(info.validation_work), info.max_alignment, info.direct_build_status,
                       static_cast<jlong>(info.graph_object_count)});
}

extern "C" JNIEXPORT jlong JNICALL FASTDB_JNI(planExecute)(
    JNIEnv* env, jclass, jlong handle, jint policy, jlong table, jlongArray out_report) {

    fdb_payload_v1_execution_report_t report{};
    fdb_payload_v1_execution_report_init(&report);
    fdb_payload_v1_payload_t* payload = nullptr;
    if (!call(env, fdb_payload_v1_plan_execute, pointer<fdb_payload_v1_plan_t>(handle), static_cast<uint32_t>(policy),
              pointer<const fdb_payload_v1_backing_v1_t>(table), &payload, &report)) {
        return 0;
    }
    const auto fields = report_fields(report);
    env->SetLongArrayRegion(out_report, 0, static_cast<jsize>(fields.size()), fields.data());
    if (env->ExceptionCheck()) {
        fdb_payload_v1_payload_release(payload);
        return 0;
    }
    return address(payload);
}
