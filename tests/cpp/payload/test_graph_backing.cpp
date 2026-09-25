#include "BackingTestSupport.hpp"
#include "TestSupport.hpp"
#include "WindowsDiagnosticSupport.hpp"

#include "payload/backing/HeapBacking.hpp"
#include "payload/build/PayloadBuilder.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace allocation_guard {

thread_local std::size_t reject_at_or_above =
    std::numeric_limits<std::size_t>::max();
thread_local std::int64_t fail_after = INT64_C(-1);

struct AllocationHeader final {
    void* raw;
};

constexpr std::size_t default_new_alignment =
    static_cast<std::size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);

void* allocate(std::size_t size,
               std::size_t alignment = default_new_alignment) {
    fastdb::test::diag::note_allocation();
    if (fail_after >= INT64_C(0)) {
        if (fail_after == INT64_C(0)) {
            fail_after = INT64_C(-1);
            fastdb::test::diag::note_injected(size);
            throw std::bad_alloc();
        }
        --fail_after;
    }
    if (size >= reject_at_or_above) {
        fastdb::test::diag::note_injected(size);
        throw std::bad_alloc();
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        throw std::bad_alloc();
    }
    alignment = std::max(
        {alignment, alignof(AllocationHeader), default_new_alignment});
    const std::size_t payload = size == 0U ? 1U : size;
    const std::size_t padding = alignment - 1U;
    constexpr std::size_t maximum =
        std::numeric_limits<std::size_t>::max();
    if (padding > maximum - sizeof(AllocationHeader)) {
        throw std::bad_alloc();
    }
    const std::size_t overhead = sizeof(AllocationHeader) + padding;
    if (payload > maximum - overhead) {
        throw std::bad_alloc();
    }
    const std::size_t allocation_size = payload + overhead;
    void* const raw = std::malloc(allocation_size);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    void* candidate = static_cast<void*>(
        static_cast<std::uint8_t*>(raw) + sizeof(AllocationHeader));
    std::size_t space = allocation_size - sizeof(AllocationHeader);
    void* const aligned =
        std::align(alignment, payload, candidate, space);
    if (aligned == nullptr) {
        std::free(raw);
        throw std::bad_alloc();
    }
    (static_cast<AllocationHeader*>(aligned) - 1)->raw = raw;
    return aligned;
}

void deallocate(void* value) noexcept {
    if (value != nullptr) {
        std::free((static_cast<AllocationHeader*>(value) - 1)->raw);
    }
}

class RejectLarge final {
public:
    explicit RejectLarge(std::size_t threshold) noexcept
        : previous_(reject_at_or_above) {
        reject_at_or_above = threshold;
    }

    RejectLarge(const RejectLarge&) = delete;
    RejectLarge& operator=(const RejectLarge&) = delete;

    ~RejectLarge() { reject_at_or_above = previous_; }

private:
    std::size_t previous_;
};

}  // namespace allocation_guard

void* operator new(std::size_t size) {
    return allocation_guard::allocate(size);
}
void* operator new[](std::size_t size) {
    return allocation_guard::allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocation_guard::allocate(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_guard::allocate(
        size, static_cast<std::size_t>(alignment));
}
void operator delete(void* value) noexcept {
    allocation_guard::deallocate(value);
}
void operator delete[](void* value) noexcept {
    allocation_guard::deallocate(value);
}
void operator delete(void* value, std::size_t) noexcept {
    allocation_guard::deallocate(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    allocation_guard::deallocate(value);
}
void operator delete(void* value, std::align_val_t) noexcept {
    allocation_guard::deallocate(value);
}
void operator delete[](void* value, std::align_val_t) noexcept {
    allocation_guard::deallocate(value);
}
void operator delete(void* value,
                     std::size_t,
                     std::align_val_t) noexcept {
    allocation_guard::deallocate(value);
}
void operator delete[](void* value,
                       std::size_t,
                       std::align_val_t) noexcept {
    allocation_guard::deallocate(value);
}

namespace {

using fastdb::payload::backing::HeapReserveObservation;
using fastdb::payload::backing::heap_reserve_observation;
using fastdb::payload::backing::reset_heap_reserve_observation;
using fastdb::payload::build::BuildPlan;
using fastdb::payload::build::ObjectHandle;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::spec::CompiledSpec;
using namespace fastdb::payload::test;

constexpr std::string_view all_values_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one","type":{"kind":"component","id":"Node"}},
    {"id":"asset","cardinality":"one","type":{"kind":"component","id":"Asset"}},
    {"id":"refs","cardinality":"many","type":{"kind":"ref","target":"Node","nullable":true}},
    {"id":"u8n_values","cardinality":"many","type":{"kind":"u8n","min":0,"max":1}},
    {"id":"u16n_values","cardinality":"many","type":{"kind":"u16n","min":-1,"max":1}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"a_bool","type":{"kind":"bool"}},
      {"id":"b_u8","type":{"kind":"u8"}},
      {"id":"c_u16","type":{"kind":"u16"}},
      {"id":"d_u32","type":{"kind":"u32"}},
      {"id":"e_i32","type":{"kind":"i32"}},
      {"id":"f_u8n","type":{"kind":"u8n","min":0,"max":1}},
      {"id":"g_u16n","type":{"kind":"u16n","min":-1,"max":1}},
      {"id":"h_f32","type":{"kind":"f32"}},
      {"id":"i_f64","type":{"kind":"f64"}},
      {"id":"j_str","type":{"kind":"str"}},
      {"id":"k_wstr","type":{"kind":"wstr"}},
      {"id":"l_bytes","type":{"kind":"bytes"}},
      {"id":"m_inline","type":{"kind":"component","id":"Inline"}},
      {"id":"n_values","type":{"kind":"list","items":{"kind":"f32","nullable":true}}},
      {"id":"o_peer","type":{"kind":"ref","target":"Node"}},
      {"id":"p_asset","type":{"kind":"ref","target":"Asset"}}
    ]},
    {"id":"Inline","kind":"record","fields":[
      {"id":"flag","type":{"kind":"bool","nullable":true}},
      {"id":"code","type":{"kind":"u16"}}
    ]},
    {"id":"Asset","kind":"record","fields":[
      {"id":"name","type":{"kind":"str"}},
      {"id":"owner","type":{"kind":"ref","target":"Node","nullable":true}}
    ]}
  ]
})";

constexpr std::string_view large_graph_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one","type":{"kind":"component","id":"Node"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"payload","type":{"kind":"bytes"}},
      {"id":"label","type":{"kind":"str"}},
      {"id":"wide","type":{"kind":"wstr"}},
      {"id":"links","type":{"kind":"list","items":{"kind":"ref","target":"Node"}}},
      {"id":"self","type":{"kind":"ref","target":"Node"}}
    ]}
  ]
})";

std::uint32_t component_index(const CompiledSpec& spec,
                              std::string_view id) {
    const auto index = spec.component_index(id);
    if (!index.has_value()) {
        fastdb::test::diag::abort_marker(
            "test_graph_backing.cpp:component_index missing component");
    }
    return *index;
}

#define FASTDB_GRAPH_PLAN_STEP(expression)                                  \
    do {                                                                     \
        auto step_result = (expression);                                     \
        if (!step_result.has_value()) {                                      \
            return Result<BuildPlan>::failure(                               \
                std::move(step_result).error());                             \
        }                                                                    \
    } while (false)

Result<BuildPlan> make_all_values_plan() {
    auto compiled = CompiledSpec::compile(all_values_spec);
    if (!compiled.has_value()) {
        return Result<BuildPlan>::failure(std::move(compiled).error());
    }
    const std::uint32_t node = component_index(compiled.value(), "Node");
    const std::uint32_t asset = component_index(compiled.value(), "Asset");
    auto created = PayloadBuilder::create(std::move(compiled).value());
    if (!created.has_value()) {
        return Result<BuildPlan>::failure(std::move(created).error());
    }
    PayloadBuilder& builder = created.value();
    auto node_object = builder.declare_object(node);
    auto asset_object = builder.declare_object(asset);
    if (!node_object.has_value()) {
        return Result<BuildPlan>::failure(std::move(node_object).error());
    }
    if (!asset_object.has_value()) {
        return Result<BuildPlan>::failure(std::move(asset_object).error());
    }

    std::string text{"same"};
    std::array<std::uint16_t, 3> wide{{
        UINT16_C(0x0041), UINT16_C(0xd83d), UINT16_C(0xde00)}};
    std::array<std::uint8_t, 3> opaque{{
        UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x7e)}};
    FASTDB_GRAPH_PLAN_STEP(
        builder.begin_object_fill(node_object.value()));
    FASTDB_GRAPH_PLAN_STEP(builder.push_bool(UINT8_C(1)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_u8(UINT8_C(0x12)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_u16(UINT16_C(0x3456)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_u32(UINT32_C(0x789abcde)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_i32(INT32_C(-1234567)));
    FASTDB_GRAPH_PLAN_STEP(
        builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_u16n_bits(UINT64_C(0)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_f32_bits(UINT32_C(0x7fa12345)));
    FASTDB_GRAPH_PLAN_STEP(
        builder.push_f64_bits(UINT64_C(0xfff8000000001234)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_str(text));
    FASTDB_GRAPH_PLAN_STEP(builder.push_wstr(wide.data(), wide.size()));
    FASTDB_GRAPH_PLAN_STEP(
        builder.push_bytes(opaque.data(), opaque.size()));
    FASTDB_GRAPH_PLAN_STEP(builder.begin_component());
    FASTDB_GRAPH_PLAN_STEP(builder.push_null());
    FASTDB_GRAPH_PLAN_STEP(builder.push_u16(UINT16_C(0xbeef)));
    FASTDB_GRAPH_PLAN_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_f32_bits(UINT32_C(0x80000000)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_null());
    FASTDB_GRAPH_PLAN_STEP(builder.push_f32_bits(UINT32_C(0xff800001)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_ref(node_object.value()));
    FASTDB_GRAPH_PLAN_STEP(builder.push_ref(asset_object.value()));

    FASTDB_GRAPH_PLAN_STEP(
        builder.begin_object_fill(asset_object.value()));
    FASTDB_GRAPH_PLAN_STEP(builder.push_str(text));
    FASTDB_GRAPH_PLAN_STEP(builder.push_ref(node_object.value()));

    FASTDB_GRAPH_PLAN_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_object(node_object.value()));
    FASTDB_GRAPH_PLAN_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(1)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_object(asset_object.value()));
    FASTDB_GRAPH_PLAN_STEP(builder.begin_entry(UINT32_C(2), UINT64_C(2)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_ref(node_object.value()));
    FASTDB_GRAPH_PLAN_STEP(builder.push_null());
    FASTDB_GRAPH_PLAN_STEP(builder.begin_entry(UINT32_C(3), UINT64_C(3)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_u8n_bits(UINT64_C(0)));
    FASTDB_GRAPH_PLAN_STEP(
        builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
    FASTDB_GRAPH_PLAN_STEP(
        builder.push_u8n_bits(UINT64_C(0x3ff0000000000000)));
    FASTDB_GRAPH_PLAN_STEP(builder.begin_entry(UINT32_C(4), UINT64_C(3)));
    FASTDB_GRAPH_PLAN_STEP(
        builder.push_u16n_bits(UINT64_C(0xbff0000000000000)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_u16n_bits(UINT64_C(0)));
    FASTDB_GRAPH_PLAN_STEP(
        builder.push_u16n_bits(UINT64_C(0x3ff0000000000000)));

    std::fill(text.begin(), text.end(), 'x');
    wide.fill(UINT16_C(0));
    opaque.fill(UINT8_C(0));
    return builder.freeze_plan();
}

Result<BuildPlan> make_large_graph_plan(std::size_t byte_count) {
    auto compiled = CompiledSpec::compile(large_graph_spec);
    if (!compiled.has_value()) {
        return Result<BuildPlan>::failure(std::move(compiled).error());
    }
    const std::uint32_t node = component_index(compiled.value(), "Node");
    auto created = PayloadBuilder::create(std::move(compiled).value());
    if (!created.has_value()) {
        return Result<BuildPlan>::failure(std::move(created).error());
    }
    PayloadBuilder& builder = created.value();
    auto first = builder.declare_object(node);
    auto second = builder.declare_object(node);
    if (!first.has_value()) {
        return Result<BuildPlan>::failure(std::move(first).error());
    }
    if (!second.has_value()) {
        return Result<BuildPlan>::failure(std::move(second).error());
    }
    std::vector<std::uint8_t> bytes(byte_count, UINT8_C(0x5a));
    const std::array<std::uint16_t, 2> wide{{
        UINT16_C(0x4e2d), UINT16_C(0x6587)}};
    const auto fill = [&](ObjectHandle object,
                          ObjectHandle peer,
                          std::string_view label) -> Result<void> {
        auto step = builder.begin_object_fill(object);
        if (!step.has_value()) {
            return step;
        }
        step = builder.push_bytes(bytes.data(), bytes.size());
        if (!step.has_value()) {
            return step;
        }
        step = builder.push_str(label);
        if (!step.has_value()) {
            return step;
        }
        step = builder.push_wstr(wide.data(), wide.size());
        if (!step.has_value()) {
            return step;
        }
        step = builder.begin_list(UINT64_C(2));
        if (!step.has_value()) {
            return step;
        }
        step = builder.push_ref(object);
        if (!step.has_value()) {
            return step;
        }
        step = builder.push_ref(peer);
        if (!step.has_value()) {
            return step;
        }
        return builder.push_ref(object);
    };
    auto filled = fill(first.value(), second.value(), "first");
    if (!filled.has_value()) {
        return Result<BuildPlan>::failure(std::move(filled).error());
    }
    filled = fill(second.value(), first.value(), "second");
    if (!filled.has_value()) {
        return Result<BuildPlan>::failure(std::move(filled).error());
    }
    FASTDB_GRAPH_PLAN_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
    FASTDB_GRAPH_PLAN_STEP(builder.push_object(first.value()));
    return builder.freeze_plan();
}

Result<PayloadBuilder> make_retry_builder() {
    auto compiled = CompiledSpec::compile(large_graph_spec);
    if (!compiled.has_value()) {
        return Result<PayloadBuilder>::failure(
            std::move(compiled).error());
    }
    const std::uint32_t node = component_index(compiled.value(), "Node");
    auto created = PayloadBuilder::create(std::move(compiled).value());
    if (!created.has_value()) {
        return created;
    }
    PayloadBuilder& builder = created.value();
    auto object = builder.declare_object(node);
    if (!object.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(object).error());
    }
    const std::array<std::uint8_t, 4> bytes{{
        UINT8_C(1), UINT8_C(2), UINT8_C(3), UINT8_C(4)}};
    const std::array<std::uint16_t, 1> wide{{UINT16_C(0x4e2d)}};
    auto step = builder.begin_object_fill(object.value());
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    step = builder.push_bytes(bytes.data(), bytes.size());
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    step = builder.push_str("retry");
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    step = builder.push_wstr(wide.data(), wide.size());
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    step = builder.begin_list(UINT64_C(1));
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    step = builder.push_ref(object.value());
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    step = builder.push_ref(object.value());
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    step = builder.begin_entry(UINT32_C(0), UINT64_C(1));
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    step = builder.push_object(object.value());
    if (!step.has_value()) {
        return Result<PayloadBuilder>::failure(std::move(step).error());
    }
    return Result<PayloadBuilder>::success(std::move(created).value());
}

#undef FASTDB_GRAPH_PLAN_STEP

bool byte_equal(const fastdb::payload::view::PayloadOwner& left,
                const fastdb::payload::view::PayloadOwner& right) {
    return owner_size(left) == owner_size(right) &&
           std::equal(owner_data(left),
                      owner_data(left) + owner_size(left),
                      owner_data(right));
}

int require_complete_range_writes(const FakeBacking& backing,
                                  std::uint64_t total_bytes) {
    std::uint64_t next_offset = UINT64_C(0);
    std::uint64_t writes = UINT64_C(0);
    for (const CallbackReceipt& receipt : backing.receipts()) {
        if (receipt.callback != CallbackKind::write) {
            continue;
        }
        require(receipt.offset == next_offset);
        require(receipt.source_size <= UINT64_C(65536));
        require(receipt.source_size <= total_bytes - next_offset);
        next_offset += receipt.source_size;
        ++writes;
    }
    require(writes > UINT64_C(0));
    require(next_offset == total_bytes);
    return EXIT_SUCCESS;
}

int test_graph_plan_facts_and_backing_identity() {
    auto planned = make_all_values_plan();
    require(planned.has_value(),
            planned.has_value() ? std::string_view{}
                                : planned.error().details_json());
    const auto& info = planned.value().info();
    require(info.total_bytes == UINT64_C(1304));
    require(info.region_count == UINT64_C(13));
    require(info.logical_value_count == UINT64_C(40));
    require(info.list_element_count == UINT64_C(3));
    require(info.text_bytes == UINT64_C(14));
    require(info.opaque_bytes == UINT64_C(3));
    require(info.validation_work == UINT64_C(146));
    require(info.max_alignment == UINT32_C(8));
    require(info.direct_build_status == FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE);
    require(info.graph_object_count == UINT64_C(2));

    auto heap = planned.value().execute(require_direct, nullptr);
    require(heap.has_value(),
            heap.has_value() ? std::string_view{}
                             : heap.error().details_json());
    require(heap.value().profile() ==
            fastdb::payload::spec::Profile::object_graph_v1);
    require(owner_size(heap.value()) == info.total_bytes);
    require(fastdb::payload::identity::sha256_lower_hex(
                fastdb::payload::identity::sha256(
                    owner_data(heap.value()), owner_size(heap.value()))) ==
            "728c4880df6797954f36e6af1ad0991e739f1c4da93b33e2917cceaac91f866c");
    const auto& heap_report = owner_report(heap.value());
    require(heap_report.mode == direct_mode);
    require(heap_report.fallback_reason == FDB_PAYLOAD_FALLBACK_NONE);
    require(heap_report.requested_bytes == info.total_bytes);
    require(heap_report.used_bytes == info.total_bytes);
    require(heap_report.staging_bytes == UINT64_C(0));
    require(heap_report.region_count == info.region_count);
    require(heap_report.backing_capacity >= info.total_bytes);

    FakeBacking stable(BackingShape::stable_span, info.total_bytes);
    auto stable_callbacks = stable.production_callbacks(false);
    auto stable_result = planned.value().execute(
        require_direct, &stable_callbacks);
    require(stable_result.has_value());
    require(byte_equal(heap.value(), stable_result.value()));
    require(callback_count(stable, CallbackKind::reserve) == UINT64_C(1));
    require(callback_count(stable, CallbackKind::write) == UINT64_C(0));
    require(callback_count(stable, CallbackKind::commit) == UINT64_C(1));
    require(callback_count(stable, CallbackKind::rollback) == UINT64_C(0));
    const auto& stable_report = owner_report(stable_result.value());
    require(stable_report.mode == direct_mode);
    require(stable_report.fallback_reason == FDB_PAYLOAD_FALLBACK_NONE);
    require(stable_report.requested_bytes == info.total_bytes);
    require(stable_report.used_bytes == info.total_bytes);
    require(stable_report.staging_bytes == UINT64_C(0));
    require(stable_report.region_count == info.region_count);
    require(stable_report.backing_capacity == info.total_bytes);

    FakeBacking range(BackingShape::range_write_only, info.total_bytes);
    auto range_callbacks = range.production_callbacks(true);
    auto range_result = planned.value().execute(
        require_direct, &range_callbacks);
    require(range_result.has_value());
    require(byte_equal(heap.value(), range_result.value()));
    require(require_complete_range_writes(range, info.total_bytes) ==
            EXIT_SUCCESS);
    require(callback_count(range, CallbackKind::reserve) == UINT64_C(1));
    require(callback_count(range, CallbackKind::commit) == UINT64_C(1));
    require(callback_count(range, CallbackKind::rollback) == UINT64_C(0));
    const auto& range_report = owner_report(range_result.value());
    require(range_report.mode == direct_mode);
    require(range_report.fallback_reason == FDB_PAYLOAD_FALLBACK_NONE);
    require(range_report.requested_bytes == info.total_bytes);
    require(range_report.used_bytes == info.total_bytes);
    require(range_report.staging_bytes == UINT64_C(0));
    require(range_report.region_count == info.region_count);
    require(range_report.backing_capacity == info.total_bytes);
    return EXIT_SUCCESS;
}

int test_graph_staging_policy_and_cleanup() {
    auto planned = make_all_values_plan();
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;
    auto heap = planned.value().execute(require_direct, nullptr);
    require(heap.has_value());

    FakeBacking staged(BackingShape::decline_direct_then_staged, total);
    auto staged_callbacks = staged.production_callbacks(false);
    reset_heap_reserve_observation();
    {
        auto result = planned.value().execute(
            allow_staging, &staged_callbacks);
        require(result.has_value());
        require(byte_equal(heap.value(), result.value()));
        const auto& report = owner_report(result.value());
        require(report.mode == staged_mode);
        require(report.fallback_reason ==
                FDB_PAYLOAD_FALLBACK_BACKING_DECLINED_DIRECT);
        require(report.requested_bytes == total);
        require(report.used_bytes == total);
        require(report.staging_bytes == total);
        require(report.region_count == planned.value().info().region_count);
        require(report.backing_capacity == total);
        require(callback_count(staged, CallbackKind::reserve) ==
                UINT64_C(2));
    }
    const HeapReserveObservation staged_heap = heap_reserve_observation();
    require(staged_heap.direct_reserves == UINT64_C(1));
    require(staged_heap.staged_reserves == UINT64_C(0));
    require(callback_count(staged, CallbackKind::write) == UINT64_C(0));
    require(callback_count(staged, CallbackKind::commit) == UINT64_C(1));
    require(callback_count(staged, CallbackKind::rollback) == UINT64_C(0));
    require(callback_count(staged, CallbackKind::release) == UINT64_C(1));

    FakeBacking required(BackingShape::decline_direct_then_staged, total);
    auto required_callbacks = required.production_callbacks(false);
    auto unavailable = planned.value().execute(
        require_direct, &required_callbacks);
    require(!unavailable.has_value());
    require(unavailable.error().code() == FDB_PAYLOAD_E_DIRECT_UNAVAILABLE);
    require(callback_count(required, CallbackKind::reserve) == UINT64_C(1));
    require(callback_count(required, CallbackKind::write) == UINT64_C(0));
    require(callback_count(required, CallbackKind::commit) == UINT64_C(0));
    require(callback_count(required, CallbackKind::rollback) == UINT64_C(0));

    FakeBacking write_failure(BackingShape::range_write_only, total);
    write_failure.inject(FailureInjection{
        CallbackKind::write, UINT64_C(1),
        FDB_PAYLOAD_E_ALLOCATION_FAILED, false});
    auto write_callbacks = write_failure.production_callbacks(true);
    auto write_result = planned.value().execute(
        allow_staging, &write_callbacks);
    require(!write_result.has_value());
    require(write_result.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(callback_count(write_failure, CallbackKind::reserve) ==
            UINT64_C(1));
    require(callback_count(write_failure, CallbackKind::rollback) ==
            UINT64_C(1));
    require(callback_count(write_failure, CallbackKind::release) ==
            UINT64_C(0));

    FakeBacking commit_failure(BackingShape::stable_span, total);
    commit_failure.inject(FailureInjection{
        CallbackKind::commit, UINT64_C(1), FDB_PAYLOAD_E_COMMIT_FAILED,
        false});
    auto commit_callbacks = commit_failure.production_callbacks(false);
    auto commit_result = planned.value().execute(
        allow_staging, &commit_callbacks);
    require(!commit_result.has_value());
    require(commit_result.error().code() == FDB_PAYLOAD_E_COMMIT_FAILED);
    require(callback_count(commit_failure, CallbackKind::reserve) ==
            UINT64_C(1));
    require(callback_count(commit_failure, CallbackKind::rollback) ==
            UINT64_C(1));
    require(callback_count(commit_failure, CallbackKind::release) ==
            UINT64_C(0));

    FakeBacking corrupt(BackingShape::corrupt_committed_image, total);
    auto corrupt_callbacks = corrupt.production_callbacks(false);
    auto corrupt_result = planned.value().execute(
        require_direct, &corrupt_callbacks);
    require(!corrupt_result.has_value());
    require(corrupt_result.error().code() == FDB_PAYLOAD_E_BACKING_CONTRACT);
    require(callback_count(corrupt, CallbackKind::rollback) == UINT64_C(0));
    require(callback_count(corrupt, CallbackKind::release) == UINT64_C(1));
    return EXIT_SUCCESS;
}

int test_repeatable_concurrent_graph_execution() {
    auto planned = make_all_values_plan();
    require(planned.has_value());
    auto expected = planned.value().execute(require_direct, nullptr);
    require(expected.has_value());
    constexpr std::size_t thread_count = 8U;
    std::array<std::unique_ptr<FakeBacking>, thread_count> backings{};
    std::array<bool, thread_count> passed{};
    for (auto& backing : backings) {
        backing = std::make_unique<FakeBacking>(
            BackingShape::range_write_only,
            planned.value().info().total_bytes);
    }
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0U; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            auto callbacks = backings[index]->production_callbacks(true);
            auto result = planned.value().execute(
                require_direct, &callbacks);
            passed[index] = result.has_value() &&
                            byte_equal(expected.value(), result.value());
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t index = 0U; index < thread_count; ++index) {
        require(passed[index]);
        require(require_complete_range_writes(
                    *backings[index],
                    planned.value().info().total_bytes) == EXIT_SUCCESS);
        require(callback_count(*backings[index], CallbackKind::release) ==
                UINT64_C(1));
    }
    return EXIT_SUCCESS;
}

int test_graph_plan_allocation_failure_is_retryable() {
    std::uint64_t observed_failures = UINT64_C(0);
    bool reached_success = false;
    for (std::int64_t allocation = INT64_C(0);
         allocation < INT64_C(2048); ++allocation) {
        auto builder = make_retry_builder();
        require(builder.has_value());
        allocation_guard::fail_after = allocation;
        auto planned = builder.value().freeze_plan();
        allocation_guard::fail_after = INT64_C(-1);
        if (planned.has_value()) {
            reached_success = true;
            break;
        }
        require(planned.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++observed_failures;
        auto retry = builder.value().freeze_plan();
        require(retry.has_value(),
                retry.has_value() ? std::string_view{}
                                  : retry.error().details_json());
        require(retry.value().info().graph_object_count == UINT64_C(1));
        auto executed = retry.value().execute(require_direct, nullptr);
        require(executed.has_value());
    }
    require(reached_success);
    require(observed_failures > UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_graph_plan_ownership_moves_are_allocation_free() {
    // Graph plans travel through the same noexcept ownership transfers as
    // record plans: Result<GraphLayout>, the ProfileLayout variant and
    // Result<BuildPlan>.  The armed moves below reject the very next
    // allocation, so a member move that allocates turns an injected
    // std::bad_alloc into std::terminate and fails this test.
    auto builder = make_retry_builder();
    require(builder.has_value());
    auto planned = builder.value().freeze_plan();
    require(planned.has_value(), planned.has_value()
                                     ? std::string_view{}
                                     : planned.error().details_json());
    const std::uint64_t total = planned.value().info().total_bytes;
    const std::uint64_t objects =
        planned.value().info().graph_object_count;
    std::vector<std::uint8_t> expected;
    {
        auto executed = planned.value().execute(require_direct, nullptr);
        require(executed.has_value());
        expected.assign(
            owner_data(executed.value()),
            owner_data(executed.value()) + owner_size(executed.value()));
    }

    bool moves_completed = false;
    try {
        // Arm the injector to reject the very next allocation: none of the
        // ownership transfers below may allocate.
        allocation_guard::fail_after = INT64_C(0);
        Result<BuildPlan> relocated = std::move(planned);
        Result<BuildPlan> twice_relocated = std::move(relocated);
        planned = std::move(twice_relocated);
        allocation_guard::fail_after = INT64_C(-1);
        moves_completed = true;
    } catch (const std::bad_alloc&) {
        allocation_guard::fail_after = INT64_C(-1);
        std::fprintf(stderr,
                     "[fastdb-diag] graph_backing plan move allocated\n");
        std::fflush(stderr);
        require(false, "graph plan ownership move allocated");
    }
    allocation_guard::fail_after = INT64_C(-1);
    require(moves_completed);
    require(planned.has_value());
    require(planned.value().info().total_bytes == total);
    require(planned.value().info().graph_object_count == objects);

    // The relocated graph plan stays reusable and byte identical.
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto executed = planned.value().execute(require_direct, nullptr);
        require(executed.has_value());
        require(owner_size(executed.value()) == expected.size());
        require(std::equal(expected.begin(), expected.end(),
                           owner_data(executed.value())));
    }

    // A GraphLayout crosses Result and is moved directly as well; keep that
    // transfer armed on its own so a layout-local regression cannot hide
    // behind the plan.
    auto layout_builder = make_retry_builder();
    require(layout_builder.has_value());
    auto layout_spec = CompiledSpec::compile(large_graph_spec);
    require(layout_spec.has_value());
    auto layout_values = layout_builder.value().freeze();
    require(layout_values.has_value());
    auto layout_runtime =
        fastdb::payload::layout::RuntimeSchema::compile(layout_spec.value());
    require(layout_runtime.has_value());
    auto graph_layout = fastdb::payload::layout::GraphLayout::plan(
        layout_runtime.value(), layout_values.value());
    require(graph_layout.has_value());
    const std::uint64_t layout_total = graph_layout.value().total_length();
    const std::uint32_t layout_regions = graph_layout.value().region_count();

    bool layout_moves_completed = false;
    try {
        allocation_guard::fail_after = INT64_C(0);
        auto relocated_layout = std::move(graph_layout);
        auto twice_relocated_layout = std::move(relocated_layout);
        fastdb::payload::layout::GraphLayout direct_layout =
            std::move(twice_relocated_layout).value();
        graph_layout = Result<fastdb::payload::layout::GraphLayout>::success(
            std::move(direct_layout));
        allocation_guard::fail_after = INT64_C(-1);
        layout_moves_completed = true;
    } catch (const std::bad_alloc&) {
        allocation_guard::fail_after = INT64_C(-1);
        std::fprintf(stderr,
                     "[fastdb-diag] graph_backing layout move allocated\n");
        std::fflush(stderr);
        require(false, "graph layout ownership move allocated");
    }
    allocation_guard::fail_after = INT64_C(-1);
    require(layout_moves_completed);
    require(graph_layout.has_value());
    require(graph_layout.value().total_length() == layout_total);
    require(graph_layout.value().region_count() == layout_regions);
    return EXIT_SUCCESS;
}

int test_graph_execution_allocation_cleanup() {
    auto planned = make_all_values_plan();
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;
    std::uint64_t precommit_failures = UINT64_C(0);
    std::uint64_t postcommit_failures = UINT64_C(0);
    bool reached_success = false;
    for (std::int64_t allocation = INT64_C(0);
         allocation < INT64_C(2048); ++allocation) {
        FakeBacking backing(BackingShape::stable_span, total);
        backing.reserve_receipts(256U);
        auto callbacks = backing.production_callbacks(false);
        allocation_guard::fail_after = allocation;
        auto executed = planned.value().execute(require_direct, &callbacks);
        allocation_guard::fail_after = INT64_C(-1);
        if (executed.has_value()) {
            reached_success = true;
            break;
        }
        require(executed.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        const std::uint64_t commits =
            callback_count(backing, CallbackKind::commit);
        if (commits == UINT64_C(0)) {
            require(callback_count(backing, CallbackKind::rollback) ==
                    UINT64_C(1));
            require(callback_count(backing, CallbackKind::release) ==
                    UINT64_C(0));
            ++precommit_failures;
        } else {
            require(commits == UINT64_C(1));
            require(callback_count(backing, CallbackKind::rollback) ==
                    UINT64_C(0));
            require(callback_count(backing, CallbackKind::release) ==
                    UINT64_C(1));
            ++postcommit_failures;
        }

        FakeBacking retry(BackingShape::stable_span, total);
        auto retry_callbacks = retry.production_callbacks(false);
        auto retried = planned.value().execute(
            require_direct, &retry_callbacks);
        require(retried.has_value());
    }
    require(reached_success);
    require(precommit_failures > UINT64_C(0));
    require(postcommit_failures > UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_direct_graph_has_no_full_image_allocation() {
    auto planned = make_large_graph_plan(2U * 1024U * 1024U);
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;
    require(total > UINT64_C(4) * UINT64_C(1024) * UINT64_C(1024));
    FakeBacking range(BackingShape::range_write_only, total);
    auto callbacks = range.production_callbacks(true);
    reset_heap_reserve_observation();
    bool deliberate_full_image_failed = false;
    {
        const std::size_t threshold =
            static_cast<std::size_t>(total / UINT64_C(2));
        allocation_guard::RejectLarge guard(threshold);
        auto direct = planned.value().execute(require_direct, &callbacks);
        require(direct.has_value(),
                direct.has_value() ? std::string_view{}
                                   : direct.error().details_json());
        require(owner_report(direct.value()).mode == direct_mode);
        try {
            std::vector<std::uint8_t> forbidden(
                static_cast<std::size_t>(total), UINT8_C(0));
            static_cast<void>(forbidden);
        } catch (const std::bad_alloc&) {
            deliberate_full_image_failed = true;
        }
    }
    require(deliberate_full_image_failed);
    const HeapReserveObservation observed = heap_reserve_observation();
    require(observed.direct_reserves == UINT64_C(0));
    require(observed.staged_reserves == UINT64_C(0));
    require(callback_count(range, CallbackKind::reserve) == UINT64_C(1));
    require(callback_count(range, CallbackKind::commit) == UINT64_C(1));
    require(callback_count(range, CallbackKind::rollback) == UINT64_C(0));
    require(callback_count(range, CallbackKind::release) == UINT64_C(1));
    require(require_complete_range_writes(range, total) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    fastdb::test::diag::install_terminate_handler();
    fastdb::test::diag::test_marker(
        "test_graph_plan_facts_and_backing_identity");
    if (test_graph_plan_facts_and_backing_identity() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    fastdb::test::diag::test_marker("test_graph_staging_policy_and_cleanup");
    if (test_graph_staging_policy_and_cleanup() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    fastdb::test::diag::test_marker(
        "test_repeatable_concurrent_graph_execution");
    if (test_repeatable_concurrent_graph_execution() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    fastdb::test::diag::test_marker(
        "test_graph_plan_allocation_failure_is_retryable");
    if (test_graph_plan_allocation_failure_is_retryable() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    fastdb::test::diag::test_marker(
        "test_graph_plan_ownership_moves_are_allocation_free");
    if (test_graph_plan_ownership_moves_are_allocation_free() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    fastdb::test::diag::test_marker(
        "test_graph_execution_allocation_cleanup");
    if (test_graph_execution_allocation_cleanup() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    fastdb::test::diag::test_marker(
        "test_direct_graph_has_no_full_image_allocation");
    return test_direct_graph_has_no_full_image_allocation();
}
