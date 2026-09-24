#include "TestSupport.hpp"

#include <fastdb_payload.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using fastdb::payload::v1::Access;
using fastdb::payload::v1::Blob;
using fastdb::payload::v1::Builder;
using fastdb::payload::v1::BuildPlan;
using fastdb::payload::v1::BuildPolicy;
using fastdb::payload::v1::ByteView;
using fastdb::payload::v1::CompiledSpec;
using fastdb::payload::v1::FixedRun;
using fastdb::payload::v1::GraphIdentity;
using fastdb::payload::v1::ObjectHandle;
using fastdb::payload::v1::Payload;
using fastdb::payload::v1::PayloadError;
using fastdb::payload::v1::View;
using fastdb::payload::v1::ViewKind;
using fastdb::payload::v1::WideView;

static_assert(std::is_trivially_copyable_v<ObjectHandle>);
static_assert(std::is_copy_constructible_v<ObjectHandle>);
static_assert(std::is_copy_assignable_v<ObjectHandle>);
static_assert(std::is_nothrow_move_constructible_v<ObjectHandle>);
static_assert(std::is_nothrow_move_assignable_v<ObjectHandle>);
static_assert(!std::is_constructible_v<ObjectHandle,
                                       fdb_payload_v1_object_handle_t>);
static_assert(std::is_trivially_copyable_v<GraphIdentity>);
static_assert(std::is_same_v<decltype(std::declval<Builder&>().declare_object(
                                 std::uint32_t{})),
                             ObjectHandle>);
static_assert(std::is_same_v<decltype(std::declval<Builder&>().begin_object_fill(
                                 std::declval<ObjectHandle>())),
                             Builder&>);
static_assert(std::is_same_v<decltype(std::declval<Builder&>().value_object(
                                 std::declval<ObjectHandle>())),
                             Builder&>);
static_assert(std::is_same_v<decltype(std::declval<Builder&>().value_ref(
                                 std::declval<ObjectHandle>())),
                             Builder&>);
static_assert(std::is_same_v<decltype(std::declval<const View&>().ref_target()),
                             View>);
static_assert(std::is_same_v<
              decltype(std::declval<const View&>().graph_identity()),
              GraphIdentity>);

static_assert(!std::is_copy_constructible_v<Builder>);
static_assert(!std::is_copy_assignable_v<Builder>);
static_assert(std::is_nothrow_move_constructible_v<Builder>);
static_assert(std::is_nothrow_move_assignable_v<Builder>);
static_assert(!std::is_copy_constructible_v<Access>);
static_assert(!std::is_copy_assignable_v<Access>);
static_assert(std::is_nothrow_move_constructible_v<Access>);
static_assert(std::is_nothrow_move_assignable_v<Access>);
static_assert(std::is_copy_constructible_v<BuildPlan>);
static_assert(std::is_copy_assignable_v<BuildPlan>);
static_assert(std::is_copy_constructible_v<Payload>);
static_assert(std::is_copy_assignable_v<Payload>);
static_assert(std::is_copy_constructible_v<View>);
static_assert(std::is_copy_assignable_v<View>);
static_assert(std::is_same_v<
              decltype(std::declval<const View&>().get_bool()), bool>);

namespace {

constexpr std::string_view kSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"record.v1",
  "entries":[
    {"id":"null_value","cardinality":"one","type":{"kind":"u8","nullable":true}},
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
    {"id":"list_value","cardinality":"one","type":{"kind":"list","items":{"kind":"u16"}}},
    {"id":"fixed_values","cardinality":"many","type":{"kind":"u16","nullable":true}}
  ],
  "components":[
    {"id":"Pair","kind":"record","fields":[
      {"id":"left","type":{"kind":"u8"}},
      {"id":"right","type":{"kind":"u8"}}
    ]}
  ]
})";

constexpr std::string_view kGraphSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one","type":{"kind":"component","id":"Node"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"value","type":{"kind":"u32"}},
      {"id":"self","type":{"kind":"ref","target":"Node"}},
      {"id":"asset","type":{"kind":"ref","target":"Asset"}}
    ]},
    {"id":"Asset","kind":"record","fields":[
      {"id":"name","type":{"kind":"str"}},
      {"id":"owner","type":{"kind":"ref","target":"Node"}}
    ]}
  ]
})";

struct BackingContext final {
    std::vector<std::uint8_t> bytes;
    std::uint32_t retains{UINT32_C(0)};
    std::uint32_t releases{UINT32_C(0)};
};

fdb_payload_v1_status_t reserve_backing(
    void* opaque, std::uint32_t, std::uint64_t capacity, std::uint32_t,
    void** out_owner_token, std::uint8_t** out_writable_data,
    std::uint64_t* out_capacity) {
    auto& context = *static_cast<BackingContext*>(opaque);
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (capacity > static_cast<std::uint64_t>(
                           std::numeric_limits<std::size_t>::max())) {
            return FDB_PAYLOAD_E_ALLOCATION_FAILED;
        }
    }
    context.bytes.assign(static_cast<std::size_t>(capacity), UINT8_C(0));
    *out_owner_token = opaque;
    *out_writable_data = context.bytes.data();
    *out_capacity = capacity;
    return UINT32_C(0);
}

fdb_payload_v1_status_t commit_backing(
    void* opaque, void*, std::uint64_t size,
    const std::uint8_t** out_readable_data,
    std::uint64_t* out_readable_size) {
    auto& context = *static_cast<BackingContext*>(opaque);
    *out_readable_data = context.bytes.data();
    *out_readable_size = size;
    return UINT32_C(0);
}

fdb_payload_v1_status_t rollback_backing(void*, void*) {
    return UINT32_C(0);
}

fdb_payload_v1_status_t retain_backing(void* opaque, void*) {
    ++static_cast<BackingContext*>(opaque)->retains;
    return UINT32_C(0);
}

void release_backing(void* opaque, void*) {
    ++static_cast<BackingContext*>(opaque)->releases;
}

fdb_payload_v1_backing_v1_t backing_for(BackingContext& context) {
    fdb_payload_v1_backing_v1_t result{};
    fdb_payload_v1_backing_init(&result);
    result.context = &context;
    result.reserve = reserve_backing;
    result.commit = commit_backing;
    result.rollback = rollback_backing;
    result.retain = retain_backing;
    result.release = release_backing;
    return result;
}

Builder author(const CompiledSpec& spec) {
    Builder builder = Builder::create(spec);
    builder.entry_begin(UINT32_C(0), UINT64_C(1)).value_null();
    builder.entry_begin(UINT32_C(1), UINT64_C(1)).value_bool(true);
    builder.entry_begin(UINT32_C(2), UINT64_C(1)).value_u8(UINT8_C(7));
    builder.entry_begin(UINT32_C(3), UINT64_C(1))
        .value_u16(UINT16_C(0x1234));
    builder.entry_begin(UINT32_C(4), UINT64_C(1))
        .value_u32(UINT32_C(0x89abcdef));
    builder.entry_begin(UINT32_C(5), UINT64_C(1)).value_i32(INT32_C(-42));
    builder.entry_begin(UINT32_C(6), UINT64_C(1)).value_u8n(0.0);
    builder.entry_begin(UINT32_C(7), UINT64_C(1)).value_u16n(5.0);
    builder.entry_begin(UINT32_C(8), UINT64_C(1)).value_f32(1.5F);
    builder.entry_begin(UINT32_C(9), UINT64_C(1)).value_f64(2.5);
    builder.entry_begin(UINT32_C(10), UINT64_C(1)).value_str("abc");
    static constexpr std::array<std::uint16_t, 2> wide{{
        UINT16_C(0x0041), UINT16_C(0x03a9)}};
    builder.entry_begin(UINT32_C(11), UINT64_C(1))
        .value_wstr(WideView{wide.data(), wide.size()});
    static constexpr std::array<std::uint8_t, 3> bytes{{
        UINT8_C(0), UINT8_C(1), UINT8_C(255)}};
    builder.entry_begin(UINT32_C(12), UINT64_C(1))
        .value_bytes(ByteView{bytes.data(), bytes.size()});
    builder.entry_begin(UINT32_C(13), UINT64_C(1))
        .value_component_begin()
        .value_u8(UINT8_C(1))
        .value_u8(UINT8_C(2));
    builder.entry_begin(UINT32_C(14), UINT64_C(1))
        .value_list_begin(UINT64_C(2))
        .value_u16(UINT16_C(3))
        .value_u16(UINT16_C(4));
    static constexpr std::array<std::uint16_t, 3> fixed{{
        UINT16_C(10), UINT16_C(20), UINT16_C(30)}};
    static constexpr std::array<std::uint8_t, 1> validity{{UINT8_C(0x05)}};
    FixedRun run;
    run.value.data = fixed.data();
    run.value.data_byte_length = sizeof(fixed);
    run.value.count = fixed.size();
    run.value.stride_bytes = sizeof(std::uint16_t);
    run.value.validity = validity.data();
    run.value.validity_byte_length = validity.size();
    builder.entry_begin(UINT32_C(15), fixed.size()).value_fixed_run(run);
    return builder;
}

int test_typed_authoring_heap_open_views_and_lifetimes() {
    const CompiledSpec spec = CompiledSpec::compile(kSpec);
    Builder builder = author(spec);
    Builder moved_builder(std::move(builder));
    Builder* const moved_builder_alias = &moved_builder;
    moved_builder = std::move(*moved_builder_alias);
    BuildPlan plan = moved_builder.freeze();
    BuildPlan plan_copy(plan);
    Builder replacement_builder = author(spec);
    BuildPlan plan_assigned = replacement_builder.freeze();
    plan_assigned = plan;
    plan_assigned = plan_assigned;
    BuildPlan moved_plan(std::move(plan_copy));
    BuildPlan* const moved_plan_alias = &moved_plan;
    moved_plan = std::move(*moved_plan_alias);
    require(plan.info().value.total_bytes > UINT64_C(0));
    require(moved_plan.info().value.total_bytes ==
            plan.info().value.total_bytes);
    require(plan_assigned.info().value.total_bytes ==
            plan.info().value.total_bytes);

    auto built = plan.execute(BuildPolicy::allow_staging);
    require(built.report.value.mode == FDB_PAYLOAD_EXECUTION_DIRECT);
    Payload payload = built.payload;
    Payload payload_copy(payload);
    auto replacement_built = moved_plan.execute(BuildPolicy::allow_staging);
    Payload payload_assigned = replacement_built.payload;
    payload_assigned = payload;
    payload_assigned = payload_assigned;
    Payload moved_payload(std::move(payload_copy));
    Payload* const moved_payload_alias = &moved_payload;
    moved_payload = std::move(*moved_payload_alias);
    const Blob binary = payload.binary_blob();
    require(binary.bytes().data != nullptr && binary.bytes().size > 0U);
    Payload opened = Payload::open_copy(spec, binary.bytes());

    View u8_sequence = opened.entry_view(UINT32_C(2));
    View view_copy(u8_sequence);
    View view_assigned = opened.entry_view(UINT32_C(1));
    view_assigned = u8_sequence;
    view_assigned = view_assigned;
    View moved_view(std::move(view_copy));
    View* const moved_view_alias = &moved_view;
    moved_view = std::move(*moved_view_alias);
    require(moved_view.kind() == ViewKind::sequence);
    require(moved_view.length() == UINT64_C(1));
    require(view_assigned.at(UINT64_C(0)).get_u8() == UINT8_C(7));
    require(opened.entry_view(UINT32_C(0)).at(UINT64_C(0)).is_null());
    require(opened.entry_view(UINT32_C(1)).at(UINT64_C(0)).get_bool());
    require(opened.entry_view(UINT32_C(3)).at(UINT64_C(0)).get_u16() ==
            UINT16_C(0x1234));
    require(opened.entry_view(UINT32_C(4)).at(UINT64_C(0)).get_u32() ==
            UINT32_C(0x89abcdef));
    require(opened.entry_view(UINT32_C(5)).at(UINT64_C(0)).get_i32() ==
            INT32_C(-42));
    const double u8n =
        opened.entry_view(UINT32_C(6)).at(UINT64_C(0)).get_u8n();
    require(u8n >= 0.0 && u8n < 0.01);
    const double u16n =
        opened.entry_view(UINT32_C(7)).at(UINT64_C(0)).get_u16n();
    require(u16n > 4.99 && u16n < 5.01);
    require(opened.entry_view(UINT32_C(8)).at(UINT64_C(0)).get_f32() ==
            1.5F);
    require(opened.entry_view(UINT32_C(9)).at(UINT64_C(0)).get_f64() ==
            2.5);

    View text = opened.entry_view(UINT32_C(10)).at(UINT64_C(0));
    Access text_access = text.acquire();
    Access moved_access(std::move(text_access));
    Access* const moved_access_alias = &moved_access;
    moved_access = std::move(*moved_access_alias);
    require(moved_access.str() == "abc");

    Access wide_access =
        opened.entry_view(UINT32_C(11)).at(UINT64_C(0)).acquire();
    const WideView wide = wide_access.wstr();
    require(wide.size == 2U);
    require(wide.data[0] == UINT16_C(0x0041));
    require(wide.data[1] == UINT16_C(0x03a9));
    require(reinterpret_cast<std::uintptr_t>(wide.data) %
                alignof(std::uint16_t) == std::uintptr_t{0});

    Access bytes_access =
        opened.entry_view(UINT32_C(12)).at(UINT64_C(0)).acquire();
    const ByteView bytes = bytes_access.bytes();
    require(bytes.size == 3U && bytes.data[0] == UINT8_C(0) &&
            bytes.data[1] == UINT8_C(1) && bytes.data[2] == UINT8_C(255));

    View component = opened.entry_view(UINT32_C(13)).at(UINT64_C(0));
    require(component.kind() == ViewKind::component);
    require(component.component_index() == UINT32_C(0));
    require(component.field_count() == UINT32_C(2));
    require(component.field(UINT32_C(1)).get_u8() == UINT8_C(2));
    View list = opened.entry_view(UINT32_C(14)).at(UINT64_C(0));
    require(list.kind() == ViewKind::list);
    require(list.length() == UINT64_C(2));
    require(list.at(UINT64_C(1)).get_u16() == UINT16_C(4));
    View fixed = opened.entry_view(UINT32_C(15));
    require(fixed.length() == UINT64_C(3));
    require(fixed.at(UINT64_C(0)).get_u16() == UINT16_C(10));
    require(fixed.at(UINT64_C(1)).is_null());
    require(fixed.at(UINT64_C(2)).get_u16() == UINT16_C(30));

    std::optional<PayloadError> copied_error;
    try {
        static_cast<void>(component.get_u8());
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_TYPE_MISMATCH);
        require(error.symbol() == "TYPE_MISMATCH");
        copied_error.emplace(error);
    }
    require(copied_error.has_value());
    require(copied_error->code() == FDB_PAYLOAD_E_TYPE_MISMATCH);

    View detached = component.materialize();
    moved_access = Access{};
    wide_access = Access{};
    bytes_access = Access{};
    opened.invalidate();
    require(detached.field(UINT32_C(0)).get_u8() == UINT8_C(1));
    try {
        static_cast<void>(component.kind());
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_VIEW_INVALIDATED);
    }
    require(moved_payload.sha256() == payload.sha256());
    require(payload_assigned.sha256() == payload.sha256());
    require(payload.execution_report().value.mode ==
            FDB_PAYLOAD_EXECUTION_DIRECT);
    return EXIT_SUCCESS;
}

int test_external_execute_and_open() {
    const CompiledSpec spec = CompiledSpec::compile(kSpec);
    Builder builder = author(spec);
    BuildPlan plan = builder.freeze();

    BackingContext execute_context;
    auto execute_backing = backing_for(execute_context);
    auto executed = plan.execute(BuildPolicy::require_direct,
                                 &execute_backing);
    require(executed.report.value.mode == FDB_PAYLOAD_EXECUTION_DIRECT);
    require(execute_context.releases == UINT32_C(0));
    const Blob binary = executed.payload.binary_blob();
    executed.payload.invalidate();
    require(execute_context.releases == UINT32_C(1));

    BackingContext open_context;
    const ByteView binary_bytes = binary.bytes();
    open_context.bytes.assign(binary_bytes.data,
                              binary_bytes.data + binary_bytes.size);
    auto open_backing = backing_for(open_context);
    Payload opened = Payload::open_external(
        spec, ByteView{open_context.bytes.data(), open_context.bytes.size()},
        open_backing, &open_context);
    require(open_context.retains == UINT32_C(1));
    Access access = opened.acquire();
    const ByteView backing_bytes = access.payload_bytes();
    require(backing_bytes.data == open_context.bytes.data());
    require(backing_bytes.size == open_context.bytes.size());
    access = Access{};
    opened.invalidate();
    require(open_context.releases == UINT32_C(1));
    return EXIT_SUCCESS;
}

int test_checked_native_sizes() {
    require(fastdb::payload::v1::detail::checked_size(
                UINT64_C(7), "test_size") == std::size_t{7});
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        try {
            static_cast<void>(fastdb::payload::v1::detail::checked_size(
                UINT64_MAX, "test_size"));
            require(false);
        } catch (const PayloadError& error) {
            require(error.code() == FDB_PAYLOAD_E_INVALID_ARGUMENT);
            require(error.symbol() == "INVALID_ARGUMENT");
            require(error.details_json().find("native_size_overflow") !=
                    std::string_view::npos);
        }
    } else {
        require(fastdb::payload::v1::detail::checked_size(
                    UINT64_MAX, "test_size") ==
                std::numeric_limits<std::size_t>::max());
    }
    return EXIT_SUCCESS;
}

int test_graph_facade_is_thin_and_complete() {
    const CompiledSpec spec = CompiledSpec::compile(kGraphSpec);
    const std::uint32_t node_component = spec.component_index("Node");
    const std::uint32_t asset_component = spec.component_index("Asset");
    Builder builder = Builder::create(spec);
    try {
        builder.begin_object_fill(ObjectHandle{});
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE);
        require(error.symbol() == "INVALID_OBJECT_HANDLE");
    }
    const ObjectHandle node = builder.declare_object(node_component);
    const ObjectHandle asset = builder.declare_object(asset_component);
    builder.begin_object_fill(node)
        .value_u32(UINT32_C(42))
        .value_ref(node)
        .value_ref(asset);
    builder.begin_object_fill(asset).value_str("map").value_ref(node);
    builder.entry_begin(UINT32_C(0), UINT64_C(1)).value_object(node);

    BuildPlan plan = builder.freeze();
    require(plan.info().value.graph_object_count == UINT64_C(2));
    auto built = plan.execute(BuildPolicy::allow_staging);
    const Blob facade_binary = built.payload.binary_blob();
    require(built.payload.profile() ==
            fastdb::payload::v1::Profile::object_graph_v1);

    fdb_payload_v1_spec_t* raw_spec = nullptr;
    fdb_payload_v1_error_t* raw_error = nullptr;
    const auto raw_ok = [&raw_error](fdb_payload_v1_status_t status) {
        return status == UINT32_C(0) && raw_error == nullptr;
    };
    require(raw_ok(fdb_payload_v1_spec_compile_json(
        reinterpret_cast<const std::uint8_t*>(kGraphSpec.data()),
        static_cast<std::uint64_t>(kGraphSpec.size()), nullptr, &raw_spec,
        &raw_error)));
    fdb_payload_v1_builder_t* raw_builder = nullptr;
    require(raw_ok(fdb_payload_v1_builder_create(
        raw_spec, nullptr, &raw_builder, &raw_error)));
    std::uint32_t raw_node_component = UINT32_MAX;
    std::uint32_t raw_asset_component = UINT32_MAX;
    require(raw_ok(fdb_payload_v1_spec_component_index(
        raw_spec, reinterpret_cast<const std::uint8_t*>("Node"), UINT64_C(4),
        &raw_node_component, &raw_error)));
    require(raw_ok(fdb_payload_v1_spec_component_index(
        raw_spec, reinterpret_cast<const std::uint8_t*>("Asset"), UINT64_C(5),
        &raw_asset_component, &raw_error)));
    fdb_payload_v1_object_handle_t raw_node = UINT64_C(0);
    fdb_payload_v1_object_handle_t raw_asset = UINT64_C(0);
    require(raw_ok(fdb_payload_v1_builder_object_declare(
        raw_builder, raw_node_component, &raw_node, &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_object_declare(
        raw_builder, raw_asset_component, &raw_asset, &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_object_fill_begin(
        raw_builder, raw_node, &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_value_u32(
        raw_builder, UINT32_C(42), &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_value_ref(
        raw_builder, raw_node, &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_value_ref(
        raw_builder, raw_asset, &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_object_fill_begin(
        raw_builder, raw_asset, &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_value_str(
        raw_builder, reinterpret_cast<const std::uint8_t*>("map"),
        UINT64_C(3), &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_value_ref(
        raw_builder, raw_node, &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_entry_begin(
        raw_builder, UINT32_C(0), UINT64_C(1), &raw_error)));
    require(raw_ok(fdb_payload_v1_builder_value_object(
        raw_builder, raw_node, &raw_error)));
    fdb_payload_v1_plan_t* raw_plan = nullptr;
    require(raw_ok(fdb_payload_v1_builder_freeze(
        raw_builder, &raw_plan, &raw_error)));
    fdb_payload_v1_payload_t* raw_payload = nullptr;
    fdb_payload_v1_execution_report_t raw_report{};
    fdb_payload_v1_execution_report_init(&raw_report);
    require(raw_ok(fdb_payload_v1_plan_execute(
        raw_plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, &raw_payload,
        &raw_report, &raw_error)));
    fdb_payload_v1_blob_t* raw_binary = nullptr;
    require(raw_ok(fdb_payload_v1_payload_binary_blob(
        raw_payload, &raw_binary, &raw_error)));
    const ByteView facade_bytes = facade_binary.bytes();
    require(fdb_payload_v1_blob_size(raw_binary) == facade_bytes.size);
    require(std::memcmp(fdb_payload_v1_blob_data(raw_binary),
                        facade_bytes.data, facade_bytes.size) == 0);
    require(raw_report.mode == built.report.value.mode);
    require(raw_report.used_bytes == built.report.value.used_bytes);
    require(raw_report.region_count == built.report.value.region_count);
    fdb_payload_v1_blob_release(raw_binary);
    fdb_payload_v1_payload_release(raw_payload);
    fdb_payload_v1_plan_release(raw_plan);
    fdb_payload_v1_builder_release(raw_builder);
    fdb_payload_v1_spec_release(raw_spec);

    View root = built.payload.entry_view(UINT32_C(0)).at(UINT64_C(0));
    const GraphIdentity root_identity = root.graph_identity();
    require(root_identity.component_index == node_component);
    require(root_identity.object_id == UINT64_C(0));
    require(root.field(UINT32_C(0)).get_u32() == UINT32_C(42));

    View self = root.field(UINT32_C(1));
    const GraphIdentity self_identity = self.graph_identity();
    require(self_identity.component_index == node_component);
    require(self_identity.object_id == root_identity.object_id);
    require(self.ref_target().graph_identity().object_id ==
            root_identity.object_id);

    View asset_view = root.field(UINT32_C(2)).ref_target();
    const GraphIdentity asset_identity = asset_view.graph_identity();
    require(asset_identity.component_index == asset_component);
    require(asset_identity.object_id == UINT64_C(0));
    require(asset_view.field(UINT32_C(1)).ref_target().graph_identity().object_id ==
            root_identity.object_id);

    View detached = root.materialize();
    built.payload.invalidate();
    require(detached.graph_identity().component_index == node_component);
    require(detached.field(UINT32_C(1))
                .ref_target()
                .graph_identity()
                .object_id == UINT64_C(0));
    try {
        static_cast<void>(root.graph_identity());
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_VIEW_INVALIDATED);
    }
    return EXIT_SUCCESS;
}

int test_provenance_guards_reject_same_indexes_from_another_spec() {
    constexpr std::string_view spec_a_source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"value","cardinality":"one","type":{"kind":"u8"}}],"components":[]})";
    constexpr std::string_view spec_b_source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"other","cardinality":"one","type":{"kind":"u8"}}],"components":[]})";
    const CompiledSpec spec_a = CompiledSpec::compile(spec_a_source);
    const CompiledSpec spec_b = CompiledSpec::compile(spec_b_source);
    const auto digest_a = spec_a.sha256();
    const auto digest_b = spec_b.sha256();
    const auto mismatch_matches = [](const PayloadError& error,
                                     std::string_view path) {
        return error.code() == FDB_PAYLOAD_E_DIGEST_MISMATCH &&
               error.symbol() == "DIGEST_MISMATCH" && error.path() == path &&
               std::string_view(error.what()) ==
                   "Portable payload spec digest does not match" &&
               error.details_json().find(
                   R"("reason":"spec_digest_mismatch")") !=
                   std::string_view::npos;
    };

    Builder builder = Builder::create(spec_a);
    builder.require_spec_sha256(digest_a);
    try {
        builder.require_spec_sha256(digest_b);
        require(false);
    } catch (const PayloadError& error) {
        require(mismatch_matches(error, "/builder/spec_sha256"));
    }
    builder.entry_begin(UINT32_C(0), UINT64_C(1)).value_u8(UINT8_C(7));
    auto built = builder.freeze().execute(BuildPolicy::allow_staging);
    built.payload.require_spec_sha256(digest_a);
    try {
        built.payload.require_spec_sha256(digest_b);
        require(false);
    } catch (const PayloadError& error) {
        require(mismatch_matches(error, "/payload/spec_sha256"));
    }

    View view = built.payload.entry_view(UINT32_C(0));
    View detached = view.materialize();
    for (View* candidate : {&view, &detached}) {
        candidate->require_spec_sha256(digest_a);
        try {
            candidate->require_spec_sha256(digest_b);
            require(false);
        } catch (const PayloadError& error) {
            require(mismatch_matches(error, "/view/spec_sha256"));
        }
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    require(test_typed_authoring_heap_open_views_and_lifetimes() ==
            EXIT_SUCCESS);
    require(test_external_execute_and_open() == EXIT_SUCCESS);
    require(test_checked_native_sizes() == EXIT_SUCCESS);
    require(test_graph_facade_is_thin_and_complete() == EXIT_SUCCESS);
    require(test_provenance_guards_reject_same_indexes_from_another_spec() ==
            EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
