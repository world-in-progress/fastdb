#include "TestSupport.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kRecordSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"record.v1",
  "entries":[
    {"id":"bool_value","cardinality":"one","type":{"kind":"bool"}},
    {"id":"u8_value","cardinality":"one","type":{"kind":"u8"}},
    {"id":"u16_value","cardinality":"one","type":{"kind":"u16"}},
    {"id":"u32_value","cardinality":"one","type":{"kind":"u32"}},
    {"id":"i32_value","cardinality":"one","type":{"kind":"i32"}},
    {"id":"u8n_value","cardinality":"one","type":{"kind":"u8n","min":-1,"max":1}},
    {"id":"u16n_value","cardinality":"one","type":{"kind":"u16n","min":0,"max":10}},
    {"id":"f32_value","cardinality":"one","type":{"kind":"f32"}},
    {"id":"f64_value","cardinality":"one","type":{"kind":"f64"}},
    {"id":"str_value","cardinality":"one","type":{"kind":"str"}},
    {"id":"wstr_value","cardinality":"one","type":{"kind":"wstr"}},
    {"id":"bytes_value","cardinality":"one","type":{"kind":"bytes"}},
    {"id":"component_value","cardinality":"one","type":{"kind":"component","id":"Pair"}},
    {"id":"list_value","cardinality":"one","type":{"kind":"list","items":{"kind":"u8"}}},
    {"id":"fixed_values","cardinality":"many","type":{"kind":"u16","nullable":true}},
    {"id":"fixed_strided","cardinality":"many","type":{"kind":"u16","nullable":true}}
  ],
  "components":[
    {"id":"Pair","kind":"record","fields":[
      {"id":"left","type":{"kind":"u8"}},
      {"id":"right","type":{"kind":"u8"}}
    ]}
  ]
})";

constexpr std::string_view kObjectGraphSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[{"id":"root","cardinality":"one","type":{"kind":"ref","target":"Node"}}],
  "components":[{"id":"Node","kind":"record","fields":[
    {"id":"name","type":{"kind":"str"}},
    {"id":"next","type":{"kind":"ref","target":"Node","nullable":true}}
  ]}]
})";

constexpr std::string_view kEmptySpansSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"record.v1",
  "entries":[
    {"id":"str_value","cardinality":"one","type":{"kind":"str"}},
    {"id":"wstr_value","cardinality":"one","type":{"kind":"wstr"}},
    {"id":"bytes_value","cardinality":"one","type":{"kind":"bytes"}}
  ],
  "components":[]
})";

static_assert(sizeof(fdb_payload_v1_builder_options_t) ==
              FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_open_options_t) ==
              FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_plan_info_t) ==
              FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_execution_report_t) ==
              FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE);
static_assert(offsetof(fdb_payload_v1_fixed_run_v1_t, data) <
              offsetof(fdb_payload_v1_fixed_run_v1_t, validity));
static_assert(offsetof(fdb_payload_v1_fixed_run_v1_t, validity) <
              offsetof(fdb_payload_v1_fixed_run_v1_t, reserved));
static_assert(offsetof(fdb_payload_v1_backing_v1_t, context) <
              offsetof(fdb_payload_v1_backing_v1_t, reserve));
static_assert(offsetof(fdb_payload_v1_backing_v1_t, reserve) <
              offsetof(fdb_payload_v1_backing_v1_t, release));
static_assert(offsetof(fdb_payload_v1_backing_v1_t, release) <
              offsetof(fdb_payload_v1_backing_v1_t, reserved));

struct ErrorRef final {
    fdb_payload_v1_error_t* value{nullptr};
    ~ErrorRef() { fdb_payload_v1_error_release(value); }
    void clear() {
        fdb_payload_v1_error_release(value);
        value = nullptr;
    }
};

fdb_payload_v1_spec_t* compile_spec(std::string_view source) {
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const auto status = fdb_payload_v1_spec_compile_json(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), nullptr, &spec, &error);
    if (status != UINT32_C(0) || error != nullptr) {
        fdb_payload_v1_error_release(error);
        return nullptr;
    }
    return spec;
}

bool owned_error(fdb_payload_v1_status_t status,
                 fdb_payload_v1_error_t* error) {
    return status != UINT32_C(0) && error != nullptr &&
           fdb_payload_v1_error_code(error) == status;
}

std::string_view error_details(fdb_payload_v1_error_t* error) {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    fdb_payload_v1_error_details_json(error, &data, &size);
    return data == nullptr
               ? std::string_view{}
               : std::string_view{reinterpret_cast<const char*>(data),
                                  static_cast<std::size_t>(size)};
}

int test_initializers_and_constants() {
    fdb_payload_v1_builder_options_t builder{};
    fdb_payload_v1_fixed_run_v1_t run{};
    fdb_payload_v1_open_options_t open{};
    fdb_payload_v1_plan_info_t info{};
    fdb_payload_v1_execution_report_t report{};
    fdb_payload_v1_backing_v1_t backing{};
    fdb_payload_v1_builder_options_init(&builder);
    fdb_payload_v1_fixed_run_init(&run);
    fdb_payload_v1_open_options_init(&open);
    fdb_payload_v1_plan_info_init(&info);
    fdb_payload_v1_execution_report_init(&report);
    fdb_payload_v1_backing_init(&backing);

    require(builder.struct_size == FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE);
    require(builder.flags == UINT32_C(0));
    require(builder.max_value_nodes == UINT64_C(10000000));
    require(builder.max_list_elements == UINT64_C(10000000));
    require(builder.max_text_bytes == (UINT64_C(1) << 30));
    require(builder.max_opaque_bytes == (UINT64_C(1) << 30));
    require(builder.max_nesting_depth == UINT64_C(1024));
    require(builder.max_total_builder_bytes == (UINT64_C(1) << 30));
    require(run.struct_size == sizeof(run));
    require(run.data == nullptr && run.validity == nullptr);
    require(open.struct_size == FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE);
    require(open.flags == FDB_PAYLOAD_OPEN_VALIDATE_TEXT_EAGER);
    require(open.max_total_bytes == (UINT64_C(1) << 30));
    require(open.max_regions == UINT64_C(1000000));
    require(open.max_entries == UINT64_C(65536));
    require(open.max_components == UINT64_C(65536));
    require(open.max_nesting_depth == UINT64_C(1024));
    require(open.max_list_elements == UINT64_C(10000000));
    require(open.max_graph_objects == UINT64_C(10000000));
    require(open.max_string_bytes == (UINT64_C(1) << 30));
    require(open.max_validation_work == UINT64_C(100000000));
    require(info.struct_size == FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE);
    require(report.struct_size == FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE);
    require(backing.struct_size == sizeof(backing));
    require(backing.context == nullptr && backing.reserve == nullptr &&
            backing.write == nullptr && backing.commit == nullptr &&
            backing.rollback == nullptr && backing.retain == nullptr &&
            backing.release == nullptr);

    require(FDB_PAYLOAD_OPERATION_BUILD == (UINT64_C(1) << 2));
    require(FDB_PAYLOAD_OPERATION_OPEN == (UINT64_C(1) << 3));
    require(FDB_PAYLOAD_OPERATION_INVALIDATE == (UINT64_C(1) << 6));
    require(FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE == UINT32_C(1));
    require(FDB_PAYLOAD_BUILD_ALLOW_STAGING == UINT32_C(1));
    require(FDB_PAYLOAD_BUILD_REQUIRE_DIRECT == UINT32_C(2));
    require(FDB_PAYLOAD_EXECUTION_DIRECT == UINT32_C(1));
    require(FDB_PAYLOAD_EXECUTION_STAGED == UINT32_C(2));
    require(FDB_PAYLOAD_FALLBACK_NONE == UINT32_C(0));

    fdb_payload_v1_builder_options_init(nullptr);
    fdb_payload_v1_fixed_run_init(nullptr);
    fdb_payload_v1_open_options_init(nullptr);
    fdb_payload_v1_plan_info_init(nullptr);
    fdb_payload_v1_execution_report_init(nullptr);
    fdb_payload_v1_backing_init(nullptr);
    fdb_payload_v1_builder_release(nullptr);
    fdb_payload_v1_plan_retain(nullptr);
    fdb_payload_v1_plan_release(nullptr);
    fdb_payload_v1_payload_retain(nullptr);
    fdb_payload_v1_payload_release(nullptr);
    return EXIT_SUCCESS;
}

int author_record(fdb_payload_v1_builder_t* builder) {
    ErrorRef error;
    const auto ok = [&error](fdb_payload_v1_status_t status) {
        const bool success = status == UINT32_C(0) && error.value == nullptr;
        error.clear();
        return success;
    };
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(0), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_bool(
        builder, UINT8_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(1), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(8), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(2), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u16(
        builder, UINT16_C(16), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(3), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u32(
        builder, UINT32_C(32), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(4), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_i32(
        builder, INT32_C(-32), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(5), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8n_f64_bits(
        builder, UINT64_C(0x0000000000000000), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(6), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u16n_f64_bits(
        builder, UINT64_C(0x4014000000000000), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(7), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_f32_bits(
        builder, UINT32_C(0x3fc00000), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(8), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_f64_bits(
        builder, UINT64_C(0x4004000000000000), &error.value)));

    static constexpr std::array<std::uint8_t, 3> text{{'a', 'b', 'c'}};
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(9), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_str(
        builder, text.data(), text.size(), &error.value)));
    static constexpr std::array<std::uint16_t, 2> wide{{
        UINT16_C(0x0041), UINT16_C(0x03a9)}};
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(10), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_wstr(
        builder, wide.data(), wide.size(), &error.value)));
    static constexpr std::array<std::uint8_t, 3> bytes{{
        UINT8_C(0), UINT8_C(1), UINT8_C(255)}};
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(11), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_bytes(
        builder, bytes.data(), bytes.size(), &error.value)));

    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(12), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_component_begin(
        builder, &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(2), &error.value)));

    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(13), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_list_begin(
        builder, UINT64_C(2), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(3), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(4), &error.value)));

    static constexpr std::array<std::uint16_t, 3> fixed{{
        UINT16_C(10), UINT16_C(20), UINT16_C(30)}};
    static constexpr std::array<std::uint8_t, 1> validity{{UINT8_C(0x05)}};
    fdb_payload_v1_fixed_run_v1_t run{};
    fdb_payload_v1_fixed_run_init(&run);
    run.data = fixed.data();
    run.data_byte_length = sizeof(fixed);
    run.count = fixed.size();
    run.stride_bytes = sizeof(std::uint16_t);
    run.validity = validity.data();
    run.validity_byte_length = validity.size();
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(14), fixed.size(), &error.value)));
    require(ok(fdb_payload_v1_builder_value_fixed_run(
        builder, &run, &error.value)));

    static constexpr std::array<std::uint16_t, 6> strided{{
        UINT16_C(100), UINT16_C(999), UINT16_C(200), UINT16_C(999),
        UINT16_C(300), UINT16_C(999)}};
    static constexpr std::array<std::uint8_t, 2> offset_validity{{
        UINT8_C(0), UINT8_C(0x0a)}};
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(15), UINT64_C(3), &error.value)));
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, nullptr, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    fdb_payload_v1_fixed_run_v1_t invalid{};
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.struct_size = UINT32_C(4);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.flags = UINT32_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.reserved[1] = UINT64_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.data_byte_length = sizeof(std::uint16_t);
    invalid.count = UINT64_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.data = strided.data();
    invalid.data_byte_length = sizeof(strided);
    invalid.count = UINT64_C(0);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    invalid.count = UINT64_C(3);
    invalid.stride_bytes = UINT64_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    invalid.stride_bytes = UINT64_C(4);
    invalid.validity = nullptr;
    invalid.validity_byte_length = UINT64_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    invalid.validity = offset_validity.data();
    invalid.validity_byte_length = UINT64_C(0);
    invalid.validity_bit_offset = UINT64_C(9);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS);
    error.clear();

    struct FutureRun final {
        fdb_payload_v1_fixed_run_v1_t known;
        std::array<std::uint8_t, 16> tail;
    } future_run{};
    fdb_payload_v1_fixed_run_init(&future_run.known);
    future_run.known.struct_size = sizeof(future_run);
    future_run.known.data = strided.data();
    future_run.known.data_byte_length = sizeof(strided);
    future_run.known.count = UINT64_C(3);
    future_run.known.stride_bytes = UINT64_C(4);
    future_run.known.validity = offset_validity.data();
    future_run.known.validity_byte_length = offset_validity.size();
    future_run.known.validity_bit_offset = UINT64_C(9);
    future_run.tail.fill(UINT8_C(0xa5));
    require(ok(fdb_payload_v1_builder_value_fixed_run(
        builder, &future_run.known, &error.value)));
    require(std::all_of(future_run.tail.begin(), future_run.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    return EXIT_SUCCESS;
}

struct BackingContext final {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> relocated_bytes;
    std::vector<std::uint32_t> calls;
    std::uint32_t retains{UINT32_C(0)};
    std::uint32_t releases{UINT32_C(0)};
    bool decline_direct{false};
    bool provide_writable{true};
    bool null_owner_token{false};
    bool poison_reserve_outputs_on_failure{false};
    bool poison_commit_outputs_on_failure{false};
    bool relocate_on_commit{false};
    bool tokens_match{true};
    std::uint32_t direct_reserve_status{UINT32_C(0)};
    std::uint32_t staged_reserve_status{UINT32_C(0)};
    std::uint32_t write_status{UINT32_C(0)};
    std::uint32_t commit_status{UINT32_C(0)};
    std::uint32_t rollback_status{UINT32_C(0)};
    std::uint32_t retain_status{UINT32_C(0)};
};

fdb_payload_v1_status_t reserve_backing(
    void* opaque, std::uint32_t mode, std::uint64_t minimum_capacity,
    std::uint32_t, void** out_owner_token, std::uint8_t** out_writable_data,
    std::uint64_t* out_capacity) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.calls.push_back(UINT32_C(10) + mode);
    if (mode == FDB_PAYLOAD_RESERVE_DIRECT && context.decline_direct) {
        if (context.poison_reserve_outputs_on_failure) {
            *out_owner_token = reinterpret_cast<void*>(std::uintptr_t{1});
            *out_writable_data =
                reinterpret_cast<std::uint8_t*>(std::uintptr_t{1});
            *out_capacity = UINT64_MAX;
        }
        return FDB_PAYLOAD_E_DIRECT_UNAVAILABLE;
    }
    const std::uint32_t configured_status =
        mode == FDB_PAYLOAD_RESERVE_DIRECT
            ? context.direct_reserve_status
            : context.staged_reserve_status;
    if (configured_status != UINT32_C(0)) {
        if (context.poison_reserve_outputs_on_failure) {
            *out_owner_token = reinterpret_cast<void*>(std::uintptr_t{1});
            *out_writable_data =
                reinterpret_cast<std::uint8_t*>(std::uintptr_t{1});
            *out_capacity = UINT64_MAX;
        }
        return configured_status;
    }
    context.bytes.assign(static_cast<std::size_t>(minimum_capacity),
                         UINT8_C(0));
    *out_owner_token = context.null_owner_token ? nullptr : opaque;
    *out_writable_data =
        context.provide_writable ? context.bytes.data() : nullptr;
    *out_capacity = minimum_capacity;
    return UINT32_C(0);
}

fdb_payload_v1_status_t write_backing(
    void* opaque, void* owner_token, std::uint64_t offset,
    const std::uint8_t* source,
    std::uint64_t source_size) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    context.calls.push_back(UINT32_C(20));
    if (context.write_status != UINT32_C(0)) {
        return context.write_status;
    }
    if (source_size != UINT64_C(0)) {
        std::memcpy(context.bytes.data() + static_cast<std::size_t>(offset),
                    source, static_cast<std::size_t>(source_size));
    }
    return UINT32_C(0);
}

fdb_payload_v1_status_t commit_backing(
    void* opaque, void* owner_token, std::uint64_t used_size,
    const std::uint8_t** out_readable_data, std::uint64_t* out_readable_size) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    context.calls.push_back(UINT32_C(30));
    if (context.commit_status != UINT32_C(0)) {
        if (context.poison_commit_outputs_on_failure) {
            *out_readable_data =
                reinterpret_cast<const std::uint8_t*>(std::uintptr_t{1});
            *out_readable_size = UINT64_MAX;
        }
        return context.commit_status;
    }
    if (context.relocate_on_commit) {
        context.relocated_bytes = context.bytes;
    }
    *out_readable_data = context.relocate_on_commit
                             ? context.relocated_bytes.data()
                             : context.bytes.data();
    *out_readable_size = used_size;
    return UINT32_C(0);
}

fdb_payload_v1_status_t rollback_backing(void* opaque, void* owner_token) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    context.calls.push_back(UINT32_C(40));
    return context.rollback_status;
}

fdb_payload_v1_status_t retain_backing(void* opaque, void* owner_token) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    ++context.retains;
    return context.retain_status;
}

void release_backing(void* opaque, void* owner_token) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    ++context.releases;
}

fdb_payload_v1_backing_v1_t backing_for(BackingContext& context) {
    fdb_payload_v1_backing_v1_t backing{};
    fdb_payload_v1_backing_init(&backing);
    backing.context = &context;
    backing.reserve = reserve_backing;
    backing.write = write_backing;
    backing.commit = commit_backing;
    backing.rollback = rollback_backing;
    backing.retain = retain_backing;
    backing.release = release_backing;
    return backing;
}

int test_complete_record_runtime() {
    fdb_payload_v1_spec_t* spec = compile_spec(kRecordSpec);
    require(spec != nullptr);
    ErrorRef error;
    fdb_payload_v1_builder_options_t options{};
    fdb_payload_v1_builder_options_init(&options);
    fdb_payload_v1_builder_t* builder = nullptr;
    require(fdb_payload_v1_builder_create(
                spec, &options, &builder, &error.value) == UINT32_C(0));
    require(builder != nullptr && error.value == nullptr);
    fdb_payload_v1_plan_t* incomplete_plan =
        reinterpret_cast<fdb_payload_v1_plan_t*>(std::uintptr_t{1});
    const auto incomplete_status = fdb_payload_v1_builder_freeze(
        builder, &incomplete_plan, &error.value);
    require(incomplete_status == FDB_PAYLOAD_E_MISSING_ENTRY,
            std::to_string(incomplete_status));
    require(incomplete_plan == nullptr);
    require(owned_error(incomplete_status, error.value));
    error.clear();
    require(author_record(builder) == EXIT_SUCCESS);

    fdb_payload_v1_plan_t* plan = nullptr;
    require(fdb_payload_v1_builder_freeze(
                builder, &plan, &error.value) == UINT32_C(0));
    require(plan != nullptr && error.value == nullptr);

    fdb_payload_v1_error_t* state_error = nullptr;
    const auto state_status =
        fdb_payload_v1_builder_value_null(builder, &state_error);
    require(owned_error(state_status, state_error));
    require(fdb_payload_v1_error_code(state_error) ==
            FDB_PAYLOAD_E_BUILDER_STATE);
    fdb_payload_v1_error_release(state_error);
    fdb_payload_v1_builder_release(builder);

    fdb_payload_v1_plan_info_t info{};
    fdb_payload_v1_plan_info_init(&info);
    require(fdb_payload_v1_plan_info(plan, &info, &error.value) ==
            UINT32_C(0));
    require(info.total_bytes > UINT64_C(0));
    require(info.region_count > UINT64_C(0));
    require(info.logical_value_count > UINT64_C(0));
    require(info.direct_build_status == FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE);

    struct FutureInfo final {
        fdb_payload_v1_plan_info_t known;
        std::array<std::uint8_t, 16> tail;
    } future_info{};
    fdb_payload_v1_plan_info_init(&future_info.known);
    future_info.known.struct_size = sizeof(future_info);
    future_info.tail.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_plan_info(
                plan, &future_info.known, &error.value) == UINT32_C(0));
    require(future_info.known.total_bytes == info.total_bytes);
    require(std::all_of(future_info.tail.begin(), future_info.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_plan_info_t short_info{};
    fdb_payload_v1_plan_info_init(&short_info);
    short_info.struct_size = UINT32_C(4);
    short_info.total_bytes = UINT64_MAX;
    require(fdb_payload_v1_plan_info(
                plan, &short_info, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(short_info.struct_size == UINT32_C(4));
    error.clear();
    fdb_payload_v1_plan_info_t invalid_info{};
    fdb_payload_v1_plan_info_init(&invalid_info);
    invalid_info.flags = UINT32_C(1);
    require(fdb_payload_v1_plan_info(
                plan, &invalid_info, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_info.flags == UINT32_C(0));
    error.clear();
    fdb_payload_v1_plan_info_init(&invalid_info);
    invalid_info.reserved[3] = UINT64_C(1);
    require(fdb_payload_v1_plan_info(
                plan, &invalid_info, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_info.reserved[3] == UINT64_C(0));
    error.clear();

    fdb_payload_v1_execution_report_t cleared_report{};
    fdb_payload_v1_execution_report_init(&cleared_report);
    cleared_report.mode = UINT32_MAX;
    cleared_report.used_bytes = UINT64_MAX;
    fdb_payload_v1_error_t* clearing_error = nullptr;
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, nullptr,
                &cleared_report, &clearing_error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_report.mode == UINT32_C(0));
    require(cleared_report.used_bytes == UINT64_C(0));
    require(owned_error(FDB_PAYLOAD_E_INVALID_ARGUMENT, clearing_error));
    fdb_payload_v1_error_release(clearing_error);

    std::atomic<bool> consistent{true};
    std::vector<std::thread> workers;
    for (std::uint32_t worker = 0; worker < UINT32_C(4); ++worker) {
        workers.emplace_back([plan, info, &consistent]() {
            for (std::uint32_t iteration = 0; iteration < UINT32_C(50);
                 ++iteration) {
                fdb_payload_v1_plan_retain(plan);
                fdb_payload_v1_plan_info_t local{};
                fdb_payload_v1_plan_info_init(&local);
                fdb_payload_v1_error_t* local_error = nullptr;
                if (fdb_payload_v1_plan_info(plan, &local, &local_error) !=
                        UINT32_C(0) ||
                    local_error != nullptr ||
                    local.total_bytes != info.total_bytes ||
                    local.region_count != info.region_count) {
                    consistent.store(false, std::memory_order_relaxed);
                }
                fdb_payload_v1_error_release(local_error);
                fdb_payload_v1_plan_release(plan);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(consistent.load(std::memory_order_relaxed));

    fdb_payload_v1_execution_report_t report{};
    fdb_payload_v1_execution_report_init(&report);
    fdb_payload_v1_payload_t* payload = nullptr;
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, &payload,
                &report, &error.value) == UINT32_C(0));
    require(payload != nullptr && error.value == nullptr);
    require(report.mode == FDB_PAYLOAD_EXECUTION_DIRECT);
    require(report.fallback_reason == FDB_PAYLOAD_FALLBACK_NONE);
    require(report.used_bytes == info.total_bytes);

    struct FutureExecuteReport final {
        fdb_payload_v1_execution_report_t known;
        std::array<std::uint8_t, 16> tail;
    } future_execute_report{};
    fdb_payload_v1_execution_report_init(&future_execute_report.known);
    future_execute_report.known.struct_size = sizeof(future_execute_report);
    future_execute_report.tail.fill(UINT8_C(0xa5));
    fdb_payload_v1_payload_t* future_execute_payload = nullptr;
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr,
                &future_execute_payload, &future_execute_report.known,
                &error.value) == UINT32_C(0));
    require(future_execute_payload != nullptr);
    require(future_execute_report.known.used_bytes == info.total_bytes);
    require(std::all_of(
        future_execute_report.tail.begin(), future_execute_report.tail.end(),
        [](std::uint8_t value) { return value == UINT8_C(0xa5); }));
    fdb_payload_v1_payload_release(future_execute_payload);

    fdb_payload_v1_execution_report_t queried_report{};
    fdb_payload_v1_execution_report_init(&queried_report);
    require(fdb_payload_v1_payload_execution_report(
                payload, &queried_report, &error.value) == UINT32_C(0));
    require(queried_report.used_bytes == report.used_bytes);
    require(queried_report.mode == report.mode);
    struct FutureReport final {
        fdb_payload_v1_execution_report_t known;
        std::array<std::uint8_t, 16> tail;
    } future_report{};
    fdb_payload_v1_execution_report_init(&future_report.known);
    future_report.known.struct_size = sizeof(future_report);
    future_report.tail.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_payload_execution_report(
                payload, &future_report.known, &error.value) == UINT32_C(0));
    require(future_report.known.used_bytes == report.used_bytes);
    require(std::all_of(future_report.tail.begin(), future_report.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_execution_report_t invalid_report{};
    fdb_payload_v1_execution_report_init(&invalid_report);
    invalid_report.struct_size = UINT32_C(4);
    require(fdb_payload_v1_payload_execution_report(
                payload, &invalid_report, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_report.struct_size == UINT32_C(4));
    error.clear();
    fdb_payload_v1_execution_report_init(&invalid_report);
    invalid_report.reserved32 = UINT32_C(1);
    invalid_report.mode = UINT32_MAX;
    require(fdb_payload_v1_payload_execution_report(
                payload, &invalid_report, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_report.mode == UINT32_C(0));
    require(invalid_report.reserved32 == UINT32_C(0));
    error.clear();
    fdb_payload_v1_execution_report_init(&invalid_report);
    invalid_report.reserved64[1] = UINT64_C(1);
    invalid_report.used_bytes = UINT64_MAX;
    require(fdb_payload_v1_payload_execution_report(
                payload, &invalid_report, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_report.used_bytes == UINT64_C(0));
    require(invalid_report.reserved64[1] == UINT64_C(0));
    error.clear();

    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
    require(fdb_payload_v1_payload_sha256(
                payload, digest.data(), &error.value) == UINT32_C(0));
    const auto payload_digest = digest;
    fdb_payload_v1_profile_t profile = UINT32_C(0);
    require(fdb_payload_v1_payload_profile(
                payload, &profile, &error.value) == UINT32_C(0));
    require(profile == FDB_PAYLOAD_PROFILE_RECORD_V1);

    fdb_payload_v1_blob_t* binary = nullptr;
    require(fdb_payload_v1_payload_binary_blob(
                payload, &binary, &error.value) == UINT32_C(0));
    require(binary != nullptr);
    require(fdb_payload_v1_blob_size(binary) == info.total_bytes);

    fdb_payload_v1_open_options_t rejected_open_options{};
    fdb_payload_v1_open_options_init(&rejected_open_options);
    rejected_open_options.struct_size = UINT32_C(4);
    fdb_payload_v1_payload_t* rejected_opened =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    auto rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), &rejected_open_options,
        &rejected_opened, &error.value);
    require(rejected_open_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_opened == nullptr);
    require(owned_error(rejected_open_status, error.value));
    error.clear();
    fdb_payload_v1_open_options_init(&rejected_open_options);
    rejected_open_options.flags = UINT32_C(2);
    rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), &rejected_open_options,
        &rejected_opened, &error.value);
    require(rejected_open_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_opened == nullptr);
    error.clear();
    fdb_payload_v1_open_options_init(&rejected_open_options);
    rejected_open_options.reserved[0] = UINT64_C(1);
    rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), &rejected_open_options,
        &rejected_opened, &error.value);
    require(rejected_open_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_opened == nullptr);
    error.clear();

    rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, nullptr, UINT64_C(1), nullptr, &rejected_opened, &error.value);
    require(rejected_open_status == FDB_PAYLOAD_E_OUT_OF_BOUNDS);
    require(rejected_opened == nullptr);
    error.clear();
    rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, nullptr, UINT64_C(0), nullptr, &rejected_opened, &error.value);
    require(rejected_open_status != UINT32_C(0));
    require(rejected_open_status != FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(rejected_opened == nullptr);
    require(owned_error(rejected_open_status, error.value));
    error.clear();

    fdb_payload_v1_spec_t* graph_spec = compile_spec(kObjectGraphSpec);
    require(graph_spec != nullptr);
    fdb_payload_v1_payload_t* graph_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    auto graph_open_status = fdb_payload_v1_payload_open_copy(
        graph_spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), nullptr, &graph_payload,
        &error.value);
    require(graph_open_status == FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE,
            std::to_string(graph_open_status));
    require(graph_payload == nullptr);
    require(owned_error(graph_open_status, error.value));
    error.clear();

    BackingContext graph_external_context;
    graph_external_context.bytes.assign(
        fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_data(binary) + fdb_payload_v1_blob_size(binary));
    auto graph_external_backing = backing_for(graph_external_context);
    graph_payload = reinterpret_cast<fdb_payload_v1_payload_t*>(
        std::uintptr_t{1});
    graph_open_status = fdb_payload_v1_payload_open_external(
        graph_spec, graph_external_context.bytes.data(),
        graph_external_context.bytes.size(), &graph_external_backing,
        &graph_external_context, nullptr, &graph_payload, &error.value);
    require(graph_open_status == FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE,
            std::to_string(graph_open_status));
    require(graph_payload == nullptr);
    require(owned_error(graph_open_status, error.value));
    require(graph_external_context.retains == UINT32_C(1));
    require(graph_external_context.releases == UINT32_C(1));
    error.clear();
    fdb_payload_v1_spec_release(graph_spec);

    fdb_payload_v1_payload_t* opened = nullptr;
    fdb_payload_v1_open_options_t open{};
    fdb_payload_v1_open_options_init(&open);
    require(fdb_payload_v1_payload_open_copy(
                spec, fdb_payload_v1_blob_data(binary),
                fdb_payload_v1_blob_size(binary), &open, &opened,
                &error.value) == UINT32_C(0));
    require(opened != nullptr);

    struct FutureOpen final {
        fdb_payload_v1_open_options_t known;
        std::array<std::uint8_t, 16> tail;
    } future_open{};
    fdb_payload_v1_open_options_init(&future_open.known);
    future_open.known.struct_size = sizeof(future_open);
    future_open.known.flags = UINT32_C(0);
    future_open.tail.fill(UINT8_C(0xa5));
    fdb_payload_v1_payload_t* lazy_opened = nullptr;
    require(fdb_payload_v1_payload_open_copy(
                spec, fdb_payload_v1_blob_data(binary),
                fdb_payload_v1_blob_size(binary), &future_open.known,
                &lazy_opened, &error.value) == UINT32_C(0));
    require(lazy_opened != nullptr);
    require(std::all_of(future_open.tail.begin(), future_open.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_payload_release(lazy_opened);

    fdb_payload_v1_execution_report_init(&queried_report);
    queried_report.mode = UINT32_MAX;
    queried_report.used_bytes = UINT64_MAX;
    const auto missing_report = fdb_payload_v1_payload_execution_report(
        opened, &queried_report, &error.value);
    require(owned_error(missing_report, error.value));
    require(missing_report == FDB_PAYLOAD_E_PLAN_STATE);
    require(queried_report.mode == UINT32_C(0));
    require(queried_report.used_bytes == UINT64_C(0));
    require(error_details(error.value) ==
            R"({"reason":"opened_payload_has_no_execution_report"})");
    error.clear();

    BackingContext external_context;
    external_context.bytes.assign(fdb_payload_v1_blob_data(binary),
                                  fdb_payload_v1_blob_data(binary) +
                                      fdb_payload_v1_blob_size(binary));
    auto external_backing = backing_for(external_context);
    fdb_payload_v1_payload_t* external = nullptr;
    require(fdb_payload_v1_payload_open_external(
                spec, external_context.bytes.data(),
                external_context.bytes.size(), &external_backing,
                &external_context, nullptr, &external,
                &error.value) == UINT32_C(0));
    require(external != nullptr && external_context.retains == UINT32_C(1));
    fdb_payload_v1_payload_retain(external);
    fdb_payload_v1_payload_release(external);
    require(external_context.releases == UINT32_C(0));
    require(fdb_payload_v1_payload_invalidate(external, &error.value) ==
            UINT32_C(0));
    require(external_context.releases == UINT32_C(1));
    require(fdb_payload_v1_payload_invalidate(external, &error.value) ==
            UINT32_C(0));
    require(external_context.releases == UINT32_C(1));
    fdb_payload_v1_payload_release(external);

    BackingContext external_only_context;
    external_only_context.bytes.assign(fdb_payload_v1_blob_data(binary),
                                       fdb_payload_v1_blob_data(binary) +
                                           fdb_payload_v1_blob_size(binary));
    fdb_payload_v1_backing_v1_t external_only_backing{};
    fdb_payload_v1_backing_init(&external_only_backing);
    external_only_backing.struct_size = static_cast<std::uint32_t>(
        offsetof(fdb_payload_v1_backing_v1_t, release) +
        sizeof(fdb_payload_v1_backing_release_fn));
    external_only_backing.context = &external_only_context;
    external_only_backing.retain = retain_backing;
    external_only_backing.release = release_backing;
    fdb_payload_v1_payload_t* external_only = nullptr;
    require(fdb_payload_v1_payload_open_external(
                spec, external_only_context.bytes.data(),
                external_only_context.bytes.size(), &external_only_backing,
                &external_only_context, nullptr, &external_only,
                &error.value) == UINT32_C(0));
    require(external_only != nullptr);
    fdb_payload_v1_payload_release(external_only);
    require(external_only_context.retains == UINT32_C(1));
    require(external_only_context.releases == UINT32_C(1));

    BackingContext invalid_external_context;
    invalid_external_context.bytes.assign(
        fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_data(binary) + fdb_payload_v1_blob_size(binary));
    invalid_external_context.bytes.front() ^= UINT8_C(0xff);
    auto invalid_external_backing = external_only_backing;
    invalid_external_backing.context = &invalid_external_context;
    external_only = reinterpret_cast<fdb_payload_v1_payload_t*>(
        std::uintptr_t{1});
    const auto invalid_external_status =
        fdb_payload_v1_payload_open_external(
            spec, invalid_external_context.bytes.data(),
            invalid_external_context.bytes.size(), &invalid_external_backing,
            &invalid_external_context, nullptr, &external_only,
            &error.value);
    require(invalid_external_status != UINT32_C(0));
    require(external_only == nullptr);
    require(owned_error(invalid_external_status, error.value));
    require(invalid_external_context.retains == UINT32_C(1));
    require(invalid_external_context.releases == UINT32_C(1));
    error.clear();

    require(fdb_payload_v1_payload_invalidate(payload, &error.value) ==
            UINT32_C(0));
    digest.fill(UINT8_C(0));
    require(fdb_payload_v1_payload_sha256(
                payload, digest.data(), &error.value) == UINT32_C(0));
    require(digest == payload_digest);
    profile = UINT32_C(0);
    require(fdb_payload_v1_payload_profile(
                payload, &profile, &error.value) == UINT32_C(0));
    require(profile == FDB_PAYLOAD_PROFILE_RECORD_V1);
    fdb_payload_v1_execution_report_init(&queried_report);
    require(fdb_payload_v1_payload_execution_report(
                payload, &queried_report, &error.value) == UINT32_C(0));
    require(queried_report.used_bytes == report.used_bytes);
    fdb_payload_v1_blob_t* invalidated_blob =
        reinterpret_cast<fdb_payload_v1_blob_t*>(std::uintptr_t{1});
    const auto invalidated_blob_status = fdb_payload_v1_payload_binary_blob(
        payload, &invalidated_blob, &error.value);
    require(invalidated_blob_status == FDB_PAYLOAD_E_VIEW_INVALIDATED);
    require(invalidated_blob == nullptr);
    require(owned_error(invalidated_blob_status, error.value));
    error.clear();
    require(fdb_payload_v1_blob_size(binary) == info.total_bytes);
    fdb_payload_v1_payload_release(payload);
    fdb_payload_v1_payload_release(opened);

    BackingContext direct_context;
    auto direct_backing = backing_for(direct_context);
    fdb_payload_v1_payload_t* direct_payload = nullptr;
    fdb_payload_v1_execution_report_t direct_report{};
    fdb_payload_v1_execution_report_init(&direct_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &direct_backing,
                &direct_payload, &direct_report, &error.value) ==
            UINT32_C(0));
    require(direct_payload != nullptr);
    require(direct_report.mode == FDB_PAYLOAD_EXECUTION_DIRECT);
    require(direct_context.calls ==
            std::vector<std::uint32_t>({UINT32_C(11), UINT32_C(30)}));
    require(direct_context.releases == UINT32_C(0));
    require(fdb_payload_v1_payload_invalidate(
                direct_payload, &error.value) == UINT32_C(0));
    require(direct_context.releases == UINT32_C(1));
    fdb_payload_v1_payload_release(direct_payload);

    BackingContext future_backing_context;
    struct FutureBacking final {
        fdb_payload_v1_backing_v1_t known;
        std::array<std::uint8_t, 16> tail;
    } future_backing{};
    future_backing.known = backing_for(future_backing_context);
    future_backing.known.struct_size = sizeof(future_backing);
    future_backing.tail.fill(UINT8_C(0xa5));
    fdb_payload_v1_payload_t* future_backed_payload = nullptr;
    fdb_payload_v1_execution_report_t future_backing_report{};
    fdb_payload_v1_execution_report_init(&future_backing_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT,
                &future_backing.known, &future_backed_payload,
                &future_backing_report, &error.value) == UINT32_C(0));
    require(future_backed_payload != nullptr);
    require(std::all_of(future_backing.tail.begin(),
                        future_backing.tail.end(), [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_payload_release(future_backed_payload);
    require(future_backing_context.releases == UINT32_C(1));

    BackingContext staged_context;
    staged_context.decline_direct = true;
    auto staged_backing = backing_for(staged_context);
    fdb_payload_v1_payload_t* staged_payload = nullptr;
    fdb_payload_v1_execution_report_t staged_report{};
    fdb_payload_v1_execution_report_init(&staged_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, &staged_backing,
                &staged_payload, &staged_report, &error.value) ==
            UINT32_C(0));
    require(staged_payload != nullptr);
    require(staged_report.mode == FDB_PAYLOAD_EXECUTION_STAGED);
    require(staged_report.fallback_reason ==
            FDB_PAYLOAD_FALLBACK_BACKING_DECLINED_DIRECT);
    require(staged_context.calls == std::vector<std::uint32_t>(
                {UINT32_C(11), UINT32_C(12), UINT32_C(30)}));
    fdb_payload_v1_payload_release(staged_payload);
    require(staged_context.releases == UINT32_C(1));

    BackingContext declined_context;
    declined_context.decline_direct = true;
    auto declined_backing = backing_for(declined_context);
    fdb_payload_v1_payload_t* declined_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    fdb_payload_v1_execution_report_t declined_report{};
    fdb_payload_v1_execution_report_init(&declined_report);
    declined_report.mode = UINT32_MAX;
    const auto declined_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &declined_backing,
        &declined_payload, &declined_report, &error.value);
    require(declined_status == FDB_PAYLOAD_E_DIRECT_UNAVAILABLE);
    require(declined_payload == nullptr);
    require(declined_report.mode == UINT32_C(0));
    require(owned_error(declined_status, error.value));
    error.clear();
    require(declined_context.calls ==
            std::vector<std::uint32_t>({UINT32_C(11)}));
    require(declined_context.releases == UINT32_C(0));

    BackingContext range_context;
    range_context.provide_writable = false;
    auto range_backing = backing_for(range_context);
    range_backing.struct_size =
        static_cast<std::uint32_t>(
            offsetof(fdb_payload_v1_backing_v1_t, reserved));
    std::fill(std::begin(range_backing.reserved),
              std::end(range_backing.reserved), UINT64_MAX);
    fdb_payload_v1_payload_t* range_payload = nullptr;
    fdb_payload_v1_execution_report_t range_report{};
    fdb_payload_v1_execution_report_init(&range_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &range_backing,
                &range_payload, &range_report, &error.value) == UINT32_C(0));
    require(range_payload != nullptr);
    require(range_context.calls.front() == UINT32_C(11));
    require(range_context.calls.back() == UINT32_C(30));
    require(std::find(range_context.calls.begin(), range_context.calls.end(),
                      UINT32_C(20)) != range_context.calls.end());
    fdb_payload_v1_payload_release(range_payload);
    require(range_context.releases == UINT32_C(1));

    enum class CallbackFailureStep : std::uint8_t {
        direct_reserve,
        staged_reserve,
        write,
        commit,
        rollback,
    };
    struct CallbackFailureCase final {
        const char* name;
        CallbackFailureStep step;
        std::uint32_t injected_status;
        std::uint32_t expected_status;
    };
    static constexpr std::array<CallbackFailureCase, 13> failure_cases{{
        {"direct reserve allocation", CallbackFailureStep::direct_reserve,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"direct reserve unknown", CallbackFailureStep::direct_reserve,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"staged reserve allocation", CallbackFailureStep::staged_reserve,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"staged reserve direct unavailable",
         CallbackFailureStep::staged_reserve,
         FDB_PAYLOAD_E_DIRECT_UNAVAILABLE, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"staged reserve unknown", CallbackFailureStep::staged_reserve,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"write allocation", CallbackFailureStep::write,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"write unknown", CallbackFailureStep::write,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"commit allocation", CallbackFailureStep::commit,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"commit failed", CallbackFailureStep::commit,
         FDB_PAYLOAD_E_COMMIT_FAILED, FDB_PAYLOAD_E_COMMIT_FAILED},
        {"commit unknown", CallbackFailureStep::commit,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"rollback allocation", CallbackFailureStep::rollback,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ROLLBACK_FAILED},
        {"rollback failed", CallbackFailureStep::rollback,
         FDB_PAYLOAD_E_ROLLBACK_FAILED, FDB_PAYLOAD_E_ROLLBACK_FAILED},
        {"rollback unknown", CallbackFailureStep::rollback,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
    }};
    for (const CallbackFailureCase& test_case : failure_cases) {
        BackingContext context;
        context.poison_reserve_outputs_on_failure = true;
        context.poison_commit_outputs_on_failure = true;
        std::uint32_t policy = FDB_PAYLOAD_BUILD_REQUIRE_DIRECT;
        std::vector<std::uint32_t> expected_calls;
        switch (test_case.step) {
        case CallbackFailureStep::direct_reserve:
            context.direct_reserve_status = test_case.injected_status;
            expected_calls = {UINT32_C(11)};
            break;
        case CallbackFailureStep::staged_reserve:
            context.decline_direct = true;
            context.staged_reserve_status = test_case.injected_status;
            policy = FDB_PAYLOAD_BUILD_ALLOW_STAGING;
            expected_calls = {UINT32_C(11), UINT32_C(12)};
            break;
        case CallbackFailureStep::write:
            context.provide_writable = false;
            context.write_status = test_case.injected_status;
            expected_calls = {UINT32_C(11), UINT32_C(20), UINT32_C(40)};
            break;
        case CallbackFailureStep::commit:
            context.commit_status = test_case.injected_status;
            expected_calls = {UINT32_C(11), UINT32_C(30), UINT32_C(40)};
            break;
        case CallbackFailureStep::rollback:
            context.provide_writable = false;
            context.write_status = FDB_PAYLOAD_E_ALLOCATION_FAILED;
            context.rollback_status = test_case.injected_status;
            expected_calls = {UINT32_C(11), UINT32_C(20), UINT32_C(40)};
            break;
        }
        auto callback_table = backing_for(context);
        fdb_payload_v1_payload_t* failed_payload =
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
        fdb_payload_v1_execution_report_t failed_report{};
        fdb_payload_v1_execution_report_init(&failed_report);
        failed_report.mode = UINT32_MAX;
        const auto failure_status = fdb_payload_v1_plan_execute(
            plan, policy, &callback_table, &failed_payload, &failed_report,
            &error.value);
        require(failure_status == test_case.expected_status, test_case.name);
        require(owned_error(failure_status, error.value), test_case.name);
        require(failed_payload == nullptr, test_case.name);
        require(failed_report.mode == UINT32_C(0), test_case.name);
        require(context.calls == expected_calls, test_case.name);
        require(context.releases == UINT32_C(0), test_case.name);
        require(context.tokens_match, test_case.name);
        error.clear();
    }

    struct RetainFailureCase final {
        const char* name;
        std::uint32_t injected_status;
        std::uint32_t expected_status;
    };
    static constexpr std::array<RetainFailureCase, 2> retain_failures{{
        {"retain allocation", FDB_PAYLOAD_E_ALLOCATION_FAILED,
         FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"retain unknown", FDB_PAYLOAD_E_INTERNAL,
         FDB_PAYLOAD_E_BACKING_CONTRACT},
    }};
    for (const RetainFailureCase& test_case : retain_failures) {
        BackingContext context;
        context.bytes.assign(fdb_payload_v1_blob_data(binary),
                             fdb_payload_v1_blob_data(binary) +
                                 fdb_payload_v1_blob_size(binary));
        context.retain_status = test_case.injected_status;
        auto callback_table = backing_for(context);
        fdb_payload_v1_payload_t* retained_payload =
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
        const auto failure_status = fdb_payload_v1_payload_open_external(
            spec, context.bytes.data(), context.bytes.size(), &callback_table,
            &context, nullptr, &retained_payload, &error.value);
        require(failure_status == test_case.expected_status, test_case.name);
        require(owned_error(failure_status, error.value), test_case.name);
        require(retained_payload == nullptr, test_case.name);
        require(context.retains == UINT32_C(1), test_case.name);
        require(context.releases == UINT32_C(0), test_case.name);
        require(context.tokens_match, test_case.name);
        error.clear();
    }

    fdb_payload_v1_payload_t* failed_payload = nullptr;
    fdb_payload_v1_execution_report_t failed_report{};
    auto failure_status = UINT32_C(0);

    enum class RequiredBuildCallback : std::uint8_t {
        reserve,
        commit,
        rollback,
        release,
    };
    static constexpr std::array<RequiredBuildCallback, 4>
        required_build_callbacks{{
            RequiredBuildCallback::reserve,
            RequiredBuildCallback::commit,
            RequiredBuildCallback::rollback,
            RequiredBuildCallback::release,
        }};
    for (const RequiredBuildCallback missing : required_build_callbacks) {
        BackingContext context;
        auto callback_table = backing_for(context);
        switch (missing) {
        case RequiredBuildCallback::reserve:
            callback_table.reserve = nullptr;
            break;
        case RequiredBuildCallback::commit:
            callback_table.commit = nullptr;
            break;
        case RequiredBuildCallback::rollback:
            callback_table.rollback = nullptr;
            break;
        case RequiredBuildCallback::release:
            callback_table.release = nullptr;
            break;
        }
        failed_payload = reinterpret_cast<fdb_payload_v1_payload_t*>(
            std::uintptr_t{1});
        fdb_payload_v1_execution_report_init(&failed_report);
        failure_status = fdb_payload_v1_plan_execute(
            plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &callback_table,
            &failed_payload, &failed_report, &error.value);
        require(failure_status == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(owned_error(failure_status, error.value));
        require(failed_payload == nullptr);
        require(context.calls.empty());
        require(context.retains == UINT32_C(0));
        require(context.releases == UINT32_C(0));
        error.clear();
    }

    BackingContext missing_write_context;
    missing_write_context.provide_writable = false;
    auto missing_write_table = backing_for(missing_write_context);
    missing_write_table.write = nullptr;
    failed_payload = reinterpret_cast<fdb_payload_v1_payload_t*>(
        std::uintptr_t{1});
    fdb_payload_v1_execution_report_init(&failed_report);
    failure_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &missing_write_table,
        &failed_payload, &failed_report, &error.value);
    require(failure_status == FDB_PAYLOAD_E_BACKING_CONTRACT);
    require(failed_payload == nullptr);
    require(missing_write_context.calls ==
            std::vector<std::uint32_t>({UINT32_C(11), UINT32_C(40)}));
    require(missing_write_context.releases == UINT32_C(0));
    require(missing_write_context.tokens_match);
    error.clear();

    for (const bool missing_retain : {true, false}) {
        BackingContext context;
        context.bytes.assign(fdb_payload_v1_blob_data(binary),
                             fdb_payload_v1_blob_data(binary) +
                                 fdb_payload_v1_blob_size(binary));
        auto callback_table = backing_for(context);
        if (missing_retain) {
            callback_table.retain = nullptr;
        } else {
            callback_table.release = nullptr;
        }
        failed_payload = reinterpret_cast<fdb_payload_v1_payload_t*>(
            std::uintptr_t{1});
        failure_status = fdb_payload_v1_payload_open_external(
            spec, context.bytes.data(), context.bytes.size(), &callback_table,
            &context, nullptr, &failed_payload, &error.value);
        require(failure_status == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(owned_error(failure_status, error.value));
        require(failed_payload == nullptr);
        require(context.calls.empty());
        require(context.retains == UINT32_C(0));
        require(context.releases == UINT32_C(0));
        error.clear();
    }

    BackingContext null_build_token_context;
    null_build_token_context.null_owner_token = true;
    auto null_build_token_table = backing_for(null_build_token_context);
    fdb_payload_v1_payload_t* null_token_payload = nullptr;
    fdb_payload_v1_execution_report_t null_token_report{};
    fdb_payload_v1_execution_report_init(&null_token_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT,
                &null_build_token_table, &null_token_payload,
                &null_token_report, &error.value) == UINT32_C(0));
    require(null_token_payload != nullptr);
    require(null_build_token_context.tokens_match);
    fdb_payload_v1_payload_release(null_token_payload);
    require(null_build_token_context.releases == UINT32_C(1));

    BackingContext null_external_token_context;
    null_external_token_context.null_owner_token = true;
    null_external_token_context.bytes.assign(
        fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_data(binary) + fdb_payload_v1_blob_size(binary));
    auto null_external_token_table = backing_for(null_external_token_context);
    null_token_payload = nullptr;
    require(fdb_payload_v1_payload_open_external(
                spec, null_external_token_context.bytes.data(),
                null_external_token_context.bytes.size(),
                &null_external_token_table, nullptr, nullptr,
                &null_token_payload, &error.value) == UINT32_C(0));
    require(null_token_payload != nullptr);
    require(null_external_token_context.retains == UINT32_C(1));
    require(null_external_token_context.tokens_match);
    fdb_payload_v1_payload_release(null_token_payload);
    require(null_external_token_context.releases == UINT32_C(1));

    BackingContext relocated_context;
    relocated_context.relocate_on_commit = true;
    auto relocated_table = backing_for(relocated_context);
    fdb_payload_v1_payload_t* relocated_payload = nullptr;
    fdb_payload_v1_execution_report_t relocated_report{};
    fdb_payload_v1_execution_report_init(&relocated_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &relocated_table,
                &relocated_payload, &relocated_report,
                &error.value) == UINT32_C(0));
    require(relocated_payload != nullptr);
    require(relocated_context.bytes.data() !=
            relocated_context.relocated_bytes.data());
    fdb_payload_v1_blob_t* relocated_blob = nullptr;
    require(fdb_payload_v1_payload_binary_blob(
                relocated_payload, &relocated_blob,
                &error.value) == UINT32_C(0));
    require(relocated_blob != nullptr);
    require(fdb_payload_v1_blob_size(relocated_blob) ==
            fdb_payload_v1_blob_size(binary));
    require(std::equal(
        fdb_payload_v1_blob_data(relocated_blob),
        fdb_payload_v1_blob_data(relocated_blob) +
            fdb_payload_v1_blob_size(relocated_blob),
        fdb_payload_v1_blob_data(binary)));
    fdb_payload_v1_blob_release(relocated_blob);
    fdb_payload_v1_payload_release(relocated_payload);
    require(relocated_context.releases == UINT32_C(1));
    require(relocated_context.tokens_match);

    BackingContext rejected_table_context;
    auto rejected_table = backing_for(rejected_table_context);
    rejected_table.flags = UINT32_C(1);
    fdb_payload_v1_execution_report_init(&failed_report);
    failure_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &rejected_table,
        &failed_payload, &failed_report, &error.value);
    require(failure_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_table_context.calls.empty());
    error.clear();
    rejected_table = backing_for(rejected_table_context);
    rejected_table.reserved[0] = UINT64_C(1);
    fdb_payload_v1_execution_report_init(&failed_report);
    failure_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &rejected_table,
        &failed_payload, &failed_report, &error.value);
    require(failure_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_table_context.calls.empty());
    error.clear();
    rejected_table = backing_for(rejected_table_context);
    rejected_table.struct_size = static_cast<std::uint32_t>(
        offsetof(fdb_payload_v1_backing_v1_t, reserved) +
        sizeof(rejected_table.reserved[0]));
    rejected_table.reserved[0] = UINT64_C(1);
    fdb_payload_v1_execution_report_init(&failed_report);
    failure_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &rejected_table,
        &failed_payload, &failed_report, &error.value);
    require(failure_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(failed_payload == nullptr);
    require(rejected_table_context.calls.empty());
    error.clear();

    fdb_payload_v1_blob_release(binary);
    fdb_payload_v1_plan_release(plan);
    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

int test_null_error_sink_rule() {
    fdb_payload_v1_spec_t* spec = compile_spec(kRecordSpec);
    require(spec != nullptr);
#define REQUIRE_NULL_SINK_REJECTED(...)                                      \
    require((__VA_ARGS__) == FDB_PAYLOAD_E_INVALID_ARGUMENT)

    fdb_payload_v1_builder_t* builder =
        reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_create(spec, nullptr, &builder, nullptr));
    require(builder ==
            reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1}));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_entry_begin(
        nullptr, UINT32_C(0), UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_null(nullptr, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_bool(nullptr, UINT8_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_u8(nullptr, UINT8_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_u16(nullptr, UINT16_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_u32(nullptr, UINT32_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_i32(nullptr, INT32_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_u8n_f64_bits(
        nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_u16n_f64_bits(
        nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_f32_bits(
        nullptr, UINT32_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_f64_bits(
        nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_str(
        nullptr, nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_wstr(
        nullptr, nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_bytes(
        nullptr, nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_fixed_run(
        nullptr, nullptr, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_component_begin(nullptr, nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_list_begin(
        nullptr, UINT64_C(0), nullptr));
    fdb_payload_v1_plan_t* plan =
        reinterpret_cast<fdb_payload_v1_plan_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_freeze(nullptr, &plan, nullptr));
    require(plan ==
            reinterpret_cast<fdb_payload_v1_plan_t*>(std::uintptr_t{1}));

    fdb_payload_v1_plan_info_t info{};
    fdb_payload_v1_plan_info_init(&info);
    info.total_bytes = UINT64_MAX;
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_plan_info(nullptr, &info, nullptr));
    require(info.total_bytes == UINT64_MAX);
    fdb_payload_v1_payload_t* payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    fdb_payload_v1_execution_report_t report{};
    fdb_payload_v1_execution_report_init(&report);
    report.mode = UINT32_MAX;
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_plan_execute(
        nullptr, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, &payload, &report,
        nullptr));
    require(payload ==
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1}));
    require(report.mode == UINT32_MAX);

    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_payload_open_copy(
        spec, nullptr, UINT64_C(0), nullptr, &payload, nullptr));
    require(payload ==
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1}));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_payload_open_external(
        spec, nullptr, UINT64_C(0), nullptr, nullptr, nullptr, &payload,
        nullptr));
    require(payload ==
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1}));
    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
    digest.fill(UINT8_C(0xa5));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_sha256(nullptr, digest.data(), nullptr));
    require(std::all_of(digest.begin(), digest.end(), [](std::uint8_t value) {
        return value == UINT8_C(0xa5);
    }));
    fdb_payload_v1_profile_t profile = UINT32_MAX;
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_profile(nullptr, &profile, nullptr));
    require(profile == UINT32_MAX);
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_payload_execution_report(
        nullptr, &report, nullptr));
    require(report.mode == UINT32_MAX);
    fdb_payload_v1_blob_t* blob =
        reinterpret_cast<fdb_payload_v1_blob_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_binary_blob(nullptr, &blob, nullptr));
    require(blob ==
            reinterpret_cast<fdb_payload_v1_blob_t*>(std::uintptr_t{1}));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_invalidate(nullptr, nullptr));

    fdb_payload_v1_spec_release(spec);
#undef REQUIRE_NULL_SINK_REJECTED
    return EXIT_SUCCESS;
}

int test_errors_prefixes_callbacks_and_graph_unavailable() {
    fdb_payload_v1_spec_t* graph = compile_spec(kObjectGraphSpec);
    require(graph != nullptr);
    ErrorRef error;
    fdb_payload_v1_builder_t* builder =
        reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1});
    const auto graph_status = fdb_payload_v1_builder_create(
        graph, nullptr, &builder, &error.value);
    require(graph_status == FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE);
    require(builder == nullptr);
    require(owned_error(graph_status, error.value));
    error.clear();
    fdb_payload_v1_spec_release(graph);

    static constexpr std::string_view builder_priority_details =
        R"({"argument":"builder","reason":"null_handle"})";
    auto priority_status = fdb_payload_v1_builder_value_str(
        nullptr, nullptr, UINT64_C(1), &error.value);
    require(priority_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(error_details(error.value) == builder_priority_details,
            std::string(error_details(error.value)));
    error.clear();
    priority_status = fdb_payload_v1_builder_value_wstr(
        nullptr, nullptr, UINT64_C(1), &error.value);
    require(priority_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(error_details(error.value) == builder_priority_details);
    error.clear();
    priority_status = fdb_payload_v1_builder_value_bytes(
        nullptr, nullptr, UINT64_C(1), &error.value);
    require(priority_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(error_details(error.value) == builder_priority_details);
    error.clear();

    fdb_payload_v1_spec_t* record = compile_spec(kRecordSpec);
    require(record != nullptr);
    builder = reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1});
    require(fdb_payload_v1_builder_create(record, nullptr, &builder, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(builder ==
            reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1}));

    fdb_payload_v1_builder_options_t short_options{};
    fdb_payload_v1_builder_options_init(&short_options);
    short_options.struct_size = UINT32_C(4);
    builder = reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1});
    require(fdb_payload_v1_builder_create(
                record, &short_options, &builder, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(builder == nullptr);
    error.clear();

    struct FutureOptions final {
        fdb_payload_v1_builder_options_t known;
        std::array<std::uint8_t, 16> tail;
    } future{};
    fdb_payload_v1_builder_options_init(&future.known);
    future.known.struct_size = sizeof(future);
    future.tail.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_builder_create(
                record, &future.known, &builder, &error.value) == UINT32_C(0));
    require(std::all_of(future.tail.begin(), future.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_builder_release(builder);

    fdb_payload_v1_builder_options_init(&future.known);
    future.known.flags = UINT32_C(1);
    require(fdb_payload_v1_builder_create(
                record, &future.known, &builder, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();
    fdb_payload_v1_builder_options_init(&future.known);
    future.known.reserved[2] = UINT64_C(1);
    require(fdb_payload_v1_builder_create(
                record, &future.known, &builder, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();

    require(fdb_payload_v1_builder_create(
                record, nullptr, &builder, &error.value) == UINT32_C(0));
    fdb_payload_v1_error_t* owned = nullptr;
    require(fdb_payload_v1_builder_entry_begin(
                builder, UINT32_C(1), UINT64_C(1), &owned) == UINT32_C(0));
    const auto mismatch =
        fdb_payload_v1_builder_value_bool(builder, UINT8_C(1), &owned);
    require(mismatch == FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(owned_error(mismatch, owned));
    fdb_payload_v1_error_release(owned);

    require(fdb_payload_v1_builder_value_str(
                builder, nullptr, UINT64_C(1), &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    fdb_payload_v1_builder_release(builder);

    fdb_payload_v1_spec_t* empty_spans = compile_spec(kEmptySpansSpec);
    require(empty_spans != nullptr);
    builder = nullptr;
    require(fdb_payload_v1_builder_create(
                empty_spans, nullptr, &builder, &error.value) == UINT32_C(0));
    require(fdb_payload_v1_builder_entry_begin(
                builder, UINT32_C(0), UINT64_C(1), &error.value) ==
            UINT32_C(0));
    require(fdb_payload_v1_builder_value_str(
                builder, nullptr, UINT64_C(1), &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    require(fdb_payload_v1_builder_value_str(
                builder, nullptr, UINT64_C(0), &error.value) == UINT32_C(0));
    require(fdb_payload_v1_builder_entry_begin(
                builder, UINT32_C(1), UINT64_C(1), &error.value) ==
            UINT32_C(0));
    require(fdb_payload_v1_builder_value_wstr(
                builder, nullptr, UINT64_C(1), &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    require(fdb_payload_v1_builder_value_wstr(
                builder, nullptr, UINT64_C(0), &error.value) == UINT32_C(0));
    require(fdb_payload_v1_builder_entry_begin(
                builder, UINT32_C(2), UINT64_C(1), &error.value) ==
            UINT32_C(0));
    require(fdb_payload_v1_builder_value_bytes(
                builder, nullptr, UINT64_C(1), &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    require(fdb_payload_v1_builder_value_bytes(
                builder, nullptr, UINT64_C(0), &error.value) == UINT32_C(0));
    fdb_payload_v1_plan_t* empty_plan = nullptr;
    require(fdb_payload_v1_builder_freeze(
                builder, &empty_plan, &error.value) == UINT32_C(0));
    require(empty_plan != nullptr);
    fdb_payload_v1_builder_release(builder);
    fdb_payload_v1_plan_release(empty_plan);
    fdb_payload_v1_spec_release(empty_spans);

    fdb_payload_v1_plan_t* cleared_plan =
        reinterpret_cast<fdb_payload_v1_plan_t*>(std::uintptr_t{1});
    auto clearing_status = fdb_payload_v1_builder_freeze(
        nullptr, &cleared_plan, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_plan == nullptr);
    require(owned_error(clearing_status, error.value));
    error.clear();

    fdb_payload_v1_plan_info_t cleared_info{};
    fdb_payload_v1_plan_info_init(&cleared_info);
    cleared_info.total_bytes = UINT64_MAX;
    clearing_status =
        fdb_payload_v1_plan_info(nullptr, &cleared_info, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_info.total_bytes == UINT64_C(0));
    error.clear();

    fdb_payload_v1_payload_t* cleared_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    fdb_payload_v1_execution_report_t cleared_report{};
    fdb_payload_v1_execution_report_init(&cleared_report);
    cleared_report.mode = UINT32_MAX;
    clearing_status = fdb_payload_v1_plan_execute(
        nullptr, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, &cleared_payload,
        &cleared_report, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_payload == nullptr);
    require(cleared_report.mode == UINT32_C(0));
    error.clear();

    cleared_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    clearing_status = fdb_payload_v1_payload_open_copy(
        nullptr, nullptr, UINT64_C(0), nullptr, &cleared_payload,
        &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_payload == nullptr);
    error.clear();
    cleared_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    clearing_status = fdb_payload_v1_payload_open_external(
        nullptr, nullptr, UINT64_C(0), nullptr, nullptr, nullptr,
        &cleared_payload, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_payload == nullptr);
    error.clear();

    std::array<std::uint8_t, 32> digest;
    digest.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_payload_sha256(nullptr, digest.data(),
                                          &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(std::all_of(digest.begin(), digest.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0);
                        }));
    error.clear();

    fdb_payload_v1_profile_t cleared_profile = UINT32_MAX;
    clearing_status = fdb_payload_v1_payload_profile(
        nullptr, &cleared_profile, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_profile == UINT32_C(0));
    error.clear();
    fdb_payload_v1_execution_report_init(&cleared_report);
    cleared_report.mode = UINT32_MAX;
    clearing_status = fdb_payload_v1_payload_execution_report(
        nullptr, &cleared_report, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_report.mode == UINT32_C(0));
    error.clear();
    fdb_payload_v1_blob_t* cleared_blob =
        reinterpret_cast<fdb_payload_v1_blob_t*>(std::uintptr_t{1});
    clearing_status = fdb_payload_v1_payload_binary_blob(
        nullptr, &cleared_blob, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_blob == nullptr);
    error.clear();
    fdb_payload_v1_spec_release(record);
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    require(test_initializers_and_constants() == EXIT_SUCCESS);
    require(test_complete_record_runtime() == EXIT_SUCCESS);
    require(test_null_error_sink_rule() == EXIT_SUCCESS);
    require(test_errors_prefixes_callbacks_and_graph_unavailable() ==
            EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
