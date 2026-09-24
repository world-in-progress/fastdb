#include "TestSupport.hpp"

#include "payload/build/PayloadBuilder.hpp"
#include "payload/spec/CompiledSpec.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace allocation_failure {

thread_local std::int64_t fail_after = INT64_C(-1);

struct AllocationHeader final {
    void* raw;
};

constexpr std::size_t default_new_alignment =
    static_cast<std::size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);
static_assert(default_new_alignment != 0U);
static_assert((default_new_alignment & (default_new_alignment - 1U)) == 0U);

void* allocate(std::size_t size,
               std::size_t alignment = default_new_alignment) {
    if (fail_after >= INT64_C(0)) {
        if (fail_after == INT64_C(0)) {
            fail_after = INT64_C(-1);
            throw std::bad_alloc();
        }
        --fail_after;
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
    void* const aligned = std::align(alignment, payload, candidate, space);
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

struct Reset final {
    ~Reset() { fail_after = INT64_C(-1); }
};

}  // namespace allocation_failure

void* operator new(std::size_t size) {
    return allocation_failure::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_failure::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(
        size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(
        size, static_cast<std::size_t>(alignment));
}

void operator delete(void* value) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::size_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value,
                     std::size_t,
                     std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value,
                       std::size_t,
                       std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}

namespace {

using fastdb::payload::build::BuilderLimits;
using fastdb::payload::build::LogicalPayload;
using fastdb::payload::build::NodeIndex;
using fastdb::payload::build::ObjectHandle;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::build::PayloadBuilderTestAccess;
using fastdb::payload::build::ValueNode;
using fastdb::payload::build::ValueTag;
using fastdb::payload::build::default_builder_limits;
using fastdb::payload::build::invalid_node_index;
using fastdb::payload::error::Result;
using fastdb::payload::spec::CompileLimits;
using fastdb::payload::spec::CompiledSpec;

constexpr std::string_view authoring_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"a_root","cardinality":"one","type":{"kind":"component","id":"A","nullable":true}},
    {"id":"b_root","cardinality":"one","type":{"kind":"component","id":"B","nullable":true}},
    {"id":"nested","cardinality":"one","type":{"kind":"list","items":{"kind":"list","items":{"kind":"component","id":"A","nullable":true}}}},
    {"id":"ref_root","cardinality":"one","type":{"kind":"ref","target":"A","nullable":true}},
    {"id":"ref_list","cardinality":"one","type":{"kind":"list","items":{"kind":"ref","target":"B","nullable":true}}},
    {"id":"empty_root","cardinality":"one","type":{"kind":"component","id":"Empty","nullable":true}},
    {"id":"orphan_root","cardinality":"one","type":{"kind":"component","id":"Orphan","nullable":true}}
  ],
  "components":[
    {"id":"A","kind":"record","fields":[
      {"id":"value","type":{"kind":"u32"}},
      {"id":"self","type":{"kind":"ref","target":"A","nullable":true}},
      {"id":"peer","type":{"kind":"ref","target":"B","nullable":true}},
      {"id":"inline","type":{"kind":"component","id":"Value"}},
      {"id":"inline_list","type":{"kind":"list","items":{"kind":"component","id":"Value"}}}
    ]},
    {"id":"B","kind":"record","fields":[
      {"id":"label","type":{"kind":"str"}},
      {"id":"back","type":{"kind":"ref","target":"A","nullable":true}}
    ]},
    {"id":"Empty","kind":"record","fields":[]},
    {"id":"Orphan","kind":"record","fields":[
      {"id":"value","type":{"kind":"u8"}}
    ]},
    {"id":"Value","kind":"record","fields":[
      {"id":"x","type":{"kind":"u16"}}
    ]}
  ]
})";

constexpr std::string_view node_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"roots","cardinality":"one","type":{"kind":"list","items":{"kind":"component","id":"Node"}}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"value","type":{"kind":"u32"}},
      {"id":"next","type":{"kind":"ref","target":"Node","nullable":true}}
    ]}
  ]
})";

constexpr std::string_view empty_node_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one","type":{"kind":"component","id":"Node","nullable":true}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[]}
  ]
})";

constexpr std::string_view cycle_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one","type":{"kind":"component","id":"Node"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"next","type":{"kind":"ref","target":"Node"}}
    ]}
  ]
})";

Result<CompiledSpec> compile(std::string_view source) {
    CompileLimits limits{};
    limits.json.max_source_bytes = UINT64_C(16) * UINT64_C(1024) *
                                   UINT64_C(1024);
    limits.json.max_json_values = UINT64_C(300000);
    limits.json.max_nesting_depth = UINT32_C(512);
    return CompiledSpec::compile(source, limits);
}

template <typename T>
bool exact_error(const Result<T>& result,
                 std::uint32_t code,
                 std::string_view path,
                 std::string_view details) {
    return !result.has_value() && result.error().code() == code &&
           result.error().path() == path &&
           result.error().details_json() == details;
}

std::uint32_t component_index(const CompiledSpec& spec,
                              std::string_view id) {
    const auto index = spec.component_index(id);
    if (!index.has_value()) {
        throw std::logic_error("required test component is absent");
    }
    return *index;
}

bool append_node_snapshot(const LogicalPayload& payload,
                          NodeIndex index,
                          std::string& output) {
    if (index == invalid_node_index || index >= payload.nodes().size()) {
        return false;
    }
    const ValueNode& node =
        payload.nodes()[static_cast<std::size_t>(index)];
    output += '(' + std::to_string(static_cast<unsigned>(node.tag)) + ':' +
              std::to_string(node.runtime_type_id) + ':' +
              std::to_string(node.scalar_bits_or_offset) + ':' +
              std::to_string(node.byte_length) + ':' +
              std::to_string(node.object_component_index) + ':' +
              std::to_string(node.object_id) + ':' +
              std::to_string(node.child_count);
    NodeIndex child = node.first_child;
    std::uint64_t observed = UINT64_C(0);
    while (child != invalid_node_index) {
        if (!append_node_snapshot(payload, child, output)) {
            return false;
        }
        child = payload.nodes()[static_cast<std::size_t>(child)].next_sibling;
        ++observed;
    }
    if (observed != node.child_count) {
        return false;
    }
    output += ')';
    return true;
}

bool logical_snapshot(const LogicalPayload& payload, std::string& output) {
    for (const NodeIndex entry : payload.entry_roots()) {
        output += "entry";
        if (!append_node_snapshot(payload, entry, output)) {
            return false;
        }
    }
    for (std::size_t component = 0U;
        component < payload.object_pools().size(); ++component) {
        output += "pool" + std::to_string(component);
        for (const NodeIndex object : payload.object_pools()[component]) {
            if (!append_node_snapshot(payload, object, output)) {
                return false;
            }
        }
    }
    output.append(payload.byte_storage());
    return true;
}

int test_allocation_harness_invariants() {
    void* const allocation = ::operator new(1U);
    const bool aligned =
        reinterpret_cast<std::uintptr_t>(allocation) %
            allocation_failure::default_new_alignment ==
        std::uintptr_t{0};
    ::operator delete(allocation);
    require(aligned);

    bool invalid_alignment_rejected = false;
    try {
        void* const invalid = allocation_failure::allocate(1U, 3U);
        allocation_failure::deallocate(invalid);
    } catch (const std::bad_alloc&) {
        invalid_alignment_rejected = true;
    }
    require(invalid_alignment_rejected);

    bool overflow_rejected = false;
    try {
        void* const overflow = allocation_failure::allocate(
            std::numeric_limits<std::size_t>::max());
        allocation_failure::deallocate(overflow);
    } catch (const std::bad_alloc&) {
        overflow_rejected = true;
    }
    require(overflow_rejected);
    return EXIT_SUCCESS;
}

int test_complete_graph_authoring_and_coordinates() {
    auto compiled = compile(authoring_spec);
    require(compiled.has_value());
    const std::uint32_t a_index = component_index(compiled.value(), "A");
    const std::uint32_t b_index = component_index(compiled.value(), "B");
    const std::uint32_t empty_index =
        component_index(compiled.value(), "Empty");
    const std::uint32_t orphan_index =
        component_index(compiled.value(), "Orphan");
    const std::uint32_t value_index =
        component_index(compiled.value(), "Value");

    require(compiled.value().capabilities().direct_build_status ==
            FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE);
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();
    auto unavailable = builder.freeze_plan();
    require(exact_error(
        unavailable, FDB_PAYLOAD_E_MISSING_ENTRY, "/entries/a_root",
        R"({"reason":"entry_not_authored"})"));

    auto a0 = builder.declare_object(a_index);
    auto b0 = builder.declare_object(b_index);
    auto a1 = builder.declare_object(a_index);
    auto empty0 = builder.declare_object(empty_index);
    require(a0.has_value() && b0.has_value() && a1.has_value() &&
            empty0.has_value());
    require(a0.value() != UINT64_C(0));
    require(b0.value() != a0.value());
    require(a1.value() != a0.value());

    require(builder.begin_object_fill(b0.value()).has_value());
    require(builder.push_str("b").has_value());
    require(builder.push_ref(a0.value()).has_value());

    require(builder.begin_object_fill(a1.value()).has_value());
    require(builder.push_u32(UINT32_C(2)).has_value());
    require(builder.push_ref(a1.value()).has_value());
    require(builder.push_ref(b0.value()).has_value());
    require(builder.begin_component().has_value());
    require(builder.push_u16(UINT16_C(22)).has_value());
    require(builder.begin_list(UINT64_C(1)).has_value());
    require(builder.begin_component().has_value());
    require(builder.push_u16(UINT16_C(23)).has_value());

    require(builder.begin_object_fill(a0.value()).has_value());
    require(builder.push_u32(UINT32_C(1)).has_value());
    require(builder.push_null().has_value());
    require(builder.push_ref(b0.value()).has_value());
    require(builder.begin_component().has_value());
    require(builder.push_u16(UINT16_C(12)).has_value());
    require(builder.begin_list(UINT64_C(0)).has_value());
    require(builder.begin_object_fill(empty0.value()).has_value());

    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(builder.push_object(a0.value()).has_value());
    require(builder.begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(builder.push_object(b0.value()).has_value());
    require(builder.begin_entry(UINT32_C(2), UINT64_C(1)).has_value());
    require(builder.begin_list(UINT64_C(1)).has_value());
    require(builder.begin_list(UINT64_C(2)).has_value());
    require(builder.push_object(a1.value()).has_value());
    require(builder.push_null().has_value());
    require(builder.begin_entry(UINT32_C(3), UINT64_C(1)).has_value());
    require(builder.push_ref(a0.value()).has_value());
    require(builder.begin_entry(UINT32_C(4), UINT64_C(1)).has_value());
    require(builder.begin_list(UINT64_C(2)).has_value());
    require(builder.push_ref(b0.value()).has_value());
    require(builder.push_ref(b0.value()).has_value());
    require(builder.begin_entry(UINT32_C(5), UINT64_C(1)).has_value());
    require(builder.push_object(empty0.value()).has_value());
    require(builder.begin_entry(UINT32_C(6), UINT64_C(1)).has_value());
    require(builder.push_null().has_value());

    auto frozen = builder.freeze();
    require(frozen.has_value());
    const LogicalPayload& payload = frozen.value();
    require(payload.graph_object_count() == UINT64_C(4));
    require(payload.object_pools().size() ==
            compiled.value().resolved().components().size());
    require(payload.object_pools()[a_index].size() == 2U);
    require(payload.object_pools()[b_index].size() == 1U);
    require(payload.object_pools()[empty_index].size() == 1U);
    require(payload.object_pools()[orphan_index].empty());
    require(payload.object_pools()[value_index].empty());

    for (std::size_t component = 0U;
         component < payload.object_pools().size(); ++component) {
        for (std::size_t object = 0U;
             object < payload.object_pools()[component].size(); ++object) {
            const ValueNode& record = payload.nodes()[static_cast<std::size_t>(
                payload.object_pools()[component][object])];
            require(record.tag == ValueTag::object_record);
            require(record.runtime_type_id == UINT32_MAX);
            require(record.object_component_index == component);
            require(record.object_id == object);
        }
    }

    std::uint64_t root_count = UINT64_C(0);
    std::uint64_t ref_count = UINT64_C(0);
    for (const ValueNode& node : payload.nodes()) {
        if (node.tag == ValueTag::object_root) {
            ++root_count;
            require(node.runtime_type_id != UINT32_MAX);
            require(node.object_component_index != UINT32_MAX);
        } else if (node.tag == ValueTag::reference) {
            ++ref_count;
            require(node.runtime_type_id != UINT32_MAX);
            require(node.object_component_index != UINT32_MAX);
        } else if (node.tag != ValueTag::object_record) {
            require(node.object_component_index == UINT32_MAX);
            require(node.object_id == UINT64_MAX);
        }
    }
    require(root_count == UINT64_C(4));
    require(ref_count == UINT64_C(7));
    return EXIT_SUCCESS;
}

Result<LogicalPayload> build_two_nodes(bool reverse_fill) {
    auto compiled = compile(node_spec);
    if (!compiled.has_value()) {
        return Result<LogicalPayload>::failure(
            std::move(compiled).error());
    }
    const std::uint32_t node = component_index(compiled.value(), "Node");
    auto created = PayloadBuilder::create(compiled.value());
    if (!created.has_value()) {
        return Result<LogicalPayload>::failure(
            std::move(created).error());
    }
    PayloadBuilder& builder = created.value();
    auto first = builder.declare_object(node);
    if (!first.has_value()) {
        return Result<LogicalPayload>::failure(std::move(first).error());
    }
    auto second = builder.declare_object(node);
    if (!second.has_value()) {
        return Result<LogicalPayload>::failure(std::move(second).error());
    }
    auto started_entry = builder.begin_entry(UINT32_C(0), UINT64_C(1));
    if (!started_entry.has_value()) {
        return Result<LogicalPayload>::failure(
            std::move(started_entry).error());
    }
    auto started_list = builder.begin_list(UINT64_C(2));
    if (!started_list.has_value()) {
        return Result<LogicalPayload>::failure(
            std::move(started_list).error());
    }
    auto pushed_first = builder.push_object(first.value());
    if (!pushed_first.has_value()) {
        return Result<LogicalPayload>::failure(
            std::move(pushed_first).error());
    }
    auto pushed_second = builder.push_object(second.value());
    if (!pushed_second.has_value()) {
        return Result<LogicalPayload>::failure(
            std::move(pushed_second).error());
    }

    const auto fill = [&](ObjectHandle object, std::uint32_t value,
                          ObjectHandle next) {
        auto started = builder.begin_object_fill(object);
        if (!started.has_value()) {
            return started;
        }
        auto pushed_value = builder.push_u32(value);
        if (!pushed_value.has_value()) {
            return pushed_value;
        }
        return builder.push_ref(next);
    };
    Result<void> filled = Result<void>::success();
    if (reverse_fill) {
        filled = fill(second.value(), UINT32_C(2), first.value());
        if (filled.has_value()) {
            filled = fill(first.value(), UINT32_C(1), second.value());
        }
    } else {
        filled = fill(first.value(), UINT32_C(1), second.value());
        if (filled.has_value()) {
            filled = fill(second.value(), UINT32_C(2), first.value());
        }
    }
    if (!filled.has_value()) {
        return Result<LogicalPayload>::failure(std::move(filled).error());
    }
    return builder.freeze();
}

int test_fill_order_is_logically_neutral() {
    auto forward = build_two_nodes(false);
    auto reverse = build_two_nodes(true);
    require(forward.has_value() && reverse.has_value());
    std::string forward_snapshot;
    std::string reverse_snapshot;
    require(logical_snapshot(forward.value(), forward_snapshot));
    require(logical_snapshot(reverse.value(), reverse_snapshot));
    require(forward_snapshot == reverse_snapshot);
    return EXIT_SUCCESS;
}

int test_handle_scope_state_and_failure_order() {
    auto compiled = compile(authoring_spec);
    require(compiled.has_value());
    const std::uint32_t a = component_index(compiled.value(), "A");
    const std::uint32_t b = component_index(compiled.value(), "B");
    const std::uint32_t value = component_index(compiled.value(), "Value");

    auto record_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]})");
    require(record_spec.has_value());
    auto record = PayloadBuilder::create(record_spec.value());
    require(record.has_value());
    require(exact_error(
        record.value().declare_object(UINT32_C(0)),
        FDB_PAYLOAD_E_PROFILE_VIOLATION, "",
        R"({"reason":"graph_operation_requires_object_graph"})"));

    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();
    require(exact_error(
        builder.declare_object(value), FDB_PAYLOAD_E_TYPE_MISMATCH,
        "/components/Value",
        R"({"reason":"component_not_identity_bearing"})"));
    require(builder.declare_object(UINT32_MAX).error().code() ==
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    require(exact_error(
        builder.begin_object_fill(UINT64_C(0)),
        FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE, "/objects",
        R"({"reason":"invalid_object_handle"})"));
    constexpr ObjectHandle forged = UINT64_C(0xdeadbeef);
    const auto forged_result = builder.begin_object_fill(forged);
    require(exact_error(forged_result, FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE,
                        "/objects",
                        R"({"reason":"invalid_object_handle"})"));
    require(forged_result.error().message().find(std::to_string(forged)) ==
            std::string::npos);

    auto a0 = builder.declare_object(a);
    auto b0 = builder.declare_object(b);
    require(a0.has_value() && b0.has_value());
    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(exact_error(
        builder.declare_object(a), FDB_PAYLOAD_E_BUILDER_STATE,
        "/entries/a_root",
        R"({"reason":"active_authoring_scope"})"));
    require(exact_error(
        builder.begin_object_fill(forged), FDB_PAYLOAD_E_BUILDER_STATE,
        "/entries/a_root",
        R"({"reason":"active_authoring_scope"})"));
    require(builder.push_ref(a0.value()).error().code() ==
            FDB_PAYLOAD_E_TYPE_MISMATCH);
    const auto wrong = builder.push_object(b0.value());
    require(exact_error(
        wrong, FDB_PAYLOAD_E_TYPE_MISMATCH, "/entries/a_root",
        R"({"actual_component":"B","actual_operation":"push_object","expected_component":"A","reason":"object_component_mismatch"})"));
    require(exact_error(
        builder.push_object(UINT64_C(0)),
        FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE, "/entries/a_root",
        R"({"reason":"invalid_object_handle"})"));
    require(builder.begin_component().error().code() ==
            FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(builder.push_object(a0.value()).has_value());

    require(builder.begin_object_fill(a0.value()).has_value());
    require(builder.push_u32(UINT32_C(1)).has_value());
    require(builder.push_null().has_value());
    require(builder.push_ref(b0.value()).has_value());
    require(builder.begin_component().has_value());
    require(builder.push_u16(UINT16_C(1)).has_value());
    require(builder.begin_list(UINT64_C(0)).has_value());
    require(exact_error(
        builder.begin_object_fill(a0.value()),
        FDB_PAYLOAD_E_BUILDER_STATE, "/objects/A/0",
        R"({"reason":"object_already_filled"})"));

    auto foreign_created = PayloadBuilder::create(compiled.value());
    require(foreign_created.has_value());
    auto foreign = foreign_created.value().declare_object(a);
    require(foreign.has_value());
    require(builder.begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(exact_error(
        builder.push_object(foreign.value()),
        FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE, "/entries/b_root",
        R"({"reason":"invalid_object_handle"})"));
    require(builder.push_object(b0.value()).has_value());

    ObjectHandle stale = UINT64_C(0);
    {
        auto stale_builder = PayloadBuilder::create(compiled.value());
        require(stale_builder.has_value());
        auto issued = stale_builder.value().declare_object(a);
        require(issued.has_value());
        stale = issued.value();
    }
    auto replacement = PayloadBuilder::create(compiled.value());
    require(replacement.has_value());
    require(replacement.value()
                .begin_entry(UINT32_C(0), UINT64_C(1))
                .has_value());
    require(exact_error(
        replacement.value().push_object(stale),
        FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE, "/entries/a_root",
        R"({"reason":"invalid_object_handle"})"));

    auto missing = PayloadBuilder::create(compiled.value());
    require(missing.has_value());
    auto missing_a = missing.value().declare_object(a);
    require(missing_a.has_value());
    auto missing_entry = missing.value().freeze();
    require(exact_error(
        missing_entry, FDB_PAYLOAD_E_MISSING_ENTRY, "/entries/a_root",
        R"({"reason":"entry_not_authored"})"));

    auto active = PayloadBuilder::create(compiled.value());
    require(active.has_value());
    auto active_a = active.value().declare_object(a);
    require(active_a.has_value());
    require(active.value().begin_object_fill(active_a.value()).has_value());
    require(exact_error(
        active.value().begin_entry(UINT32_C(0), UINT64_C(1)),
        FDB_PAYLOAD_E_BUILDER_STATE, "/objects/A/0/value",
        R"({"reason":"active_authoring_scope"})"));
    require(active.value().push_u32(UINT32_C(1)).has_value());
    auto missing_field = active.value().freeze();
    require(exact_error(
        missing_field, FDB_PAYLOAD_E_MISSING_FIELD,
        "/objects/A/0/self", R"({"reason":"field_not_authored"})"));

    auto unfilled = PayloadBuilder::create(compiled.value());
    require(unfilled.has_value());
    auto unfilled_a = unfilled.value().declare_object(a);
    require(unfilled_a.has_value());
    require(unfilled.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(unfilled.value().push_object(unfilled_a.value()).has_value());
    for (std::uint32_t entry = UINT32_C(1); entry < UINT32_C(7); ++entry) {
        require(unfilled.value().begin_entry(entry, UINT64_C(1)).has_value());
        if (entry == UINT32_C(2) || entry == UINT32_C(4)) {
            require(unfilled.value().begin_list(UINT64_C(0)).has_value());
        } else {
            require(unfilled.value().push_null().has_value());
        }
    }
    require(exact_error(
        unfilled.value().freeze(), FDB_PAYLOAD_E_MISSING_FIELD,
        "/objects/A/0", R"({"reason":"object_not_filled"})"));

    auto orphan = PayloadBuilder::create(compiled.value());
    require(orphan.has_value());
    const std::uint32_t orphan_component =
        component_index(compiled.value(), "Orphan");
    auto orphan_object = orphan.value().declare_object(orphan_component);
    require(orphan_object.has_value());
    require(orphan.value()
                .begin_object_fill(orphan_object.value())
                .has_value());
    require(orphan.value().push_u8(UINT8_C(1)).has_value());
    for (std::uint32_t entry = UINT32_C(0); entry < UINT32_C(7); ++entry) {
        require(orphan.value().begin_entry(entry, UINT64_C(1)).has_value());
        if (entry == UINT32_C(2) || entry == UINT32_C(4)) {
            require(orphan.value().begin_list(UINT64_C(0)).has_value());
        } else {
            require(orphan.value().push_null().has_value());
        }
    }
    require(exact_error(
        orphan.value().freeze(), FDB_PAYLOAD_E_UNREACHABLE_OBJECT,
        "/objects/Orphan/0", R"({"reason":"unreachable_object"})"));
    return EXIT_SUCCESS;
}

int test_limits_terminal_tokens_and_concurrent_builders() {
    auto compiled = compile(empty_node_spec);
    require(compiled.has_value());
    const std::uint32_t node = component_index(compiled.value(), "Node");
    const BuilderLimits defaults = default_builder_limits();
    require(defaults.max_graph_objects == UINT64_C(10000000));

    BuilderLimits graph_limit = defaults;
    graph_limit.max_graph_objects = UINT64_C(0);
    auto graph_limited = PayloadBuilder::create(compiled.value(), graph_limit);
    require(graph_limited.has_value());
    require(exact_error(
        graph_limited.value().declare_object(node),
        FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT, "/objects/Node/0",
        R"({"actual":"1","limit":"0","resource":"graph_objects"})"));

    BuilderLimits node_limit = defaults;
    node_limit.max_value_nodes = UINT64_C(0);
    auto node_limited = PayloadBuilder::create(compiled.value(), node_limit);
    require(node_limited.has_value());
    require(exact_error(
        node_limited.value().declare_object(node),
        FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT, "/objects/Node/0",
        R"({"actual":"1","limit":"0","resource":"value_nodes"})"));

    BuilderLimits bytes_limit = defaults;
    bytes_limit.max_total_builder_bytes = UINT64_C(71);
    auto bytes_limited = PayloadBuilder::create(compiled.value(), bytes_limit);
    require(bytes_limited.has_value());
    require(exact_error(
        bytes_limited.value().declare_object(node),
        FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT, "/objects/Node/0",
        R"({"actual":"72","limit":"71","resource":"total_builder_bytes"})"));

    auto terminal = PayloadBuilder::create(compiled.value());
    require(terminal.has_value());
    require(PayloadBuilderTestAccess::use_object_handle_sequence(
        terminal.value(), UINT64_MAX));
    auto final_token = terminal.value().declare_object(node);
    require(final_token.has_value());
    require(final_token.value() == UINT64_MAX);
    require(exact_error(
        terminal.value().declare_object(node),
        FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT, "/objects/Node/1",
        R"({"actual":"18446744073709551615","limit":"18446744073709551615","resource":"object_handles"})"));
    require(terminal.value().begin_object_fill(final_token.value()).has_value());
    require(terminal.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(terminal.value().push_object(final_token.value()).has_value());
    auto terminal_payload = terminal.value().freeze();
    require(terminal_payload.has_value());
    require(terminal_payload.value().graph_object_count() == UINT64_C(1));
    const ValueNode& terminal_record = terminal_payload.value().nodes()[
        static_cast<std::size_t>(
            terminal_payload.value().object_pools()[node][0])];
    require(terminal_record.object_id == UINT64_C(0));

    constexpr std::size_t builder_count = 32U;
    std::vector<ObjectHandle> handles(builder_count, UINT64_C(0));
    std::vector<std::thread> workers;
    std::atomic<bool> workers_succeeded{true};
    workers.reserve(builder_count);
    for (std::size_t index = 0U; index < builder_count; ++index) {
        workers.emplace_back([&, index]() {
            auto local = PayloadBuilder::create(compiled.value());
            if (!local.has_value()) {
                workers_succeeded.store(false, std::memory_order_relaxed);
                return;
            }
            auto handle = local.value().declare_object(node);
            if (!handle.has_value()) {
                workers_succeeded.store(false, std::memory_order_relaxed);
                return;
            }
            handles[index] = handle.value();
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(workers_succeeded.load(std::memory_order_relaxed));
    std::sort(handles.begin(), handles.end());
    require(handles.front() != UINT64_C(0));
    require(std::adjacent_find(handles.begin(), handles.end()) ==
            handles.end());
    return EXIT_SUCCESS;
}

int test_allocation_failures_preserve_retryable_state() {
    allocation_failure::Reset reset;
    auto empty_compiled = compile(empty_node_spec);
    require(empty_compiled.has_value());
    const std::uint32_t empty_node =
        component_index(empty_compiled.value(), "Node");

    auto gated_plan = PayloadBuilder::create(empty_compiled.value());
    require(gated_plan.has_value());
    allocation_failure::fail_after = INT64_C(0);
    auto gated_allocation_failure = gated_plan.value().freeze_plan();
    allocation_failure::fail_after = INT64_C(-1);
    require(!gated_allocation_failure.has_value());
    require(gated_allocation_failure.error().code() ==
            FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(exact_error(
        gated_plan.value().freeze_plan(), FDB_PAYLOAD_E_MISSING_ENTRY,
        "/entries/root", R"({"reason":"entry_not_authored"})"));

    std::uint32_t declaration_failures = UINT32_C(0);
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(64);
         ++allocation) {
        auto builder = PayloadBuilder::create(empty_compiled.value());
        require(builder.has_value());
        allocation_failure::fail_after = allocation;
        auto declared = builder.value().declare_object(empty_node);
        allocation_failure::fail_after = INT64_C(-1);
        if (declared.has_value()) {
            break;
        }
        require(declared.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++declaration_failures;
        auto retry = builder.value().declare_object(empty_node);
        require(retry.has_value());
        require(builder.value().begin_object_fill(retry.value()).has_value());
        require(builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
        require(builder.value().push_object(retry.value()).has_value());
        auto payload = builder.value().freeze();
        require(payload.has_value());
        require(payload.value().object_pools()[empty_node].size() == 1U);
    }
    require(declaration_failures >= UINT32_C(3));

    auto node_compiled = compile(node_spec);
    require(node_compiled.has_value());
    const std::uint32_t node = component_index(node_compiled.value(), "Node");
    std::uint32_t fill_failures = UINT32_C(0);
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(32);
         ++allocation) {
        auto builder = PayloadBuilder::create(node_compiled.value());
        require(builder.has_value());
        auto object = builder.value().declare_object(node);
        require(object.has_value());
        allocation_failure::fail_after = allocation;
        auto begun = builder.value().begin_object_fill(object.value());
        allocation_failure::fail_after = INT64_C(-1);
        if (begun.has_value()) {
            break;
        }
        require(begun.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++fill_failures;
        require(builder.value().begin_object_fill(object.value()).has_value());
        require(builder.value().push_u32(UINT32_C(1)).has_value());
        require(builder.value().push_null().has_value());
        require(builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
        require(builder.value().begin_list(UINT64_C(1)).has_value());
        require(builder.value().push_object(object.value()).has_value());
        require(builder.value().freeze().has_value());
    }
    require(fill_failures >= UINT32_C(1));

    auto cycle_compiled = compile(cycle_spec);
    require(cycle_compiled.has_value());
    const std::uint32_t cycle_node =
        component_index(cycle_compiled.value(), "Node");
    std::uint32_t ref_failures = UINT32_C(0);
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(32);
         ++allocation) {
        auto builder = PayloadBuilder::create(cycle_compiled.value());
        require(builder.has_value());
        auto object = builder.value().declare_object(cycle_node);
        require(object.has_value());
        require(builder.value().begin_object_fill(object.value()).has_value());
        allocation_failure::fail_after = allocation;
        auto referred = builder.value().push_ref(object.value());
        allocation_failure::fail_after = INT64_C(-1);
        if (referred.has_value()) {
            break;
        }
        require(referred.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++ref_failures;
        require(builder.value().push_ref(object.value()).has_value());
        require(builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
        require(builder.value().push_object(object.value()).has_value());
        require(builder.value().freeze().has_value());
    }
    require(ref_failures >= UINT32_C(1));

    std::uint32_t freeze_failures = UINT32_C(0);
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(96);
         ++allocation) {
        auto builder = PayloadBuilder::create(cycle_compiled.value());
        require(builder.has_value());
        auto first = builder.value().declare_object(cycle_node);
        auto second = builder.value().declare_object(cycle_node);
        require(first.has_value() && second.has_value());
        require(builder.value().begin_object_fill(first.value()).has_value());
        require(builder.value().push_ref(second.value()).has_value());
        require(builder.value().begin_object_fill(second.value()).has_value());
        require(builder.value().push_ref(first.value()).has_value());
        require(builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
        require(builder.value().push_object(first.value()).has_value());
        allocation_failure::fail_after = allocation;
        auto frozen = builder.value().freeze();
        allocation_failure::fail_after = INT64_C(-1);
        if (frozen.has_value()) {
            break;
        }
        require(frozen.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++freeze_failures;
        auto retry = builder.value().freeze();
        require(retry.has_value());
        require(retry.value().graph_object_count() == UINT64_C(2));
    }
    require(freeze_failures >= UINT32_C(3));
    return EXIT_SUCCESS;
}

int test_fifty_thousand_object_cycle_is_iterative() {
    constexpr std::size_t object_count = 50000U;
    auto compiled = compile(cycle_spec);
    require(compiled.has_value());
    const std::uint32_t node = component_index(compiled.value(), "Node");
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();
    std::vector<ObjectHandle> objects;
    objects.reserve(object_count);
    for (std::size_t index = 0U; index < object_count; ++index) {
        auto declared = builder.declare_object(node);
        require(declared.has_value());
        objects.push_back(declared.value());
    }
    for (std::size_t index = 0U; index < object_count; ++index) {
        require(builder.begin_object_fill(objects[index]).has_value());
        require(builder
                    .push_ref(objects[(index + 1U) % object_count])
                    .has_value());
    }
    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(builder.push_object(objects.front()).has_value());
    auto frozen = builder.freeze();
    require(frozen.has_value());
    require(frozen.value().graph_object_count() == object_count);
    require(frozen.value().object_pools()[node].size() == object_count);
    require(frozen.value().nodes().size() == object_count * 2U + 2U);
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view selection = argc == 2 ? argv[1] : "";
    if (selection == "--allocation-sweeps") {
        return test_allocation_failures_preserve_retryable_state();
    }
    if (selection == "--large-cycle") {
        return test_fifty_thousand_object_cycle_is_iterative();
    }
    const bool semantic_only = selection == "--semantic";
    if (argc > 2 || (argc == 2 && !semantic_only)) {
        return EXIT_FAILURE;
    }
    if (test_allocation_harness_invariants() != EXIT_SUCCESS ||
        test_complete_graph_authoring_and_coordinates() != EXIT_SUCCESS ||
        test_fill_order_is_logically_neutral() != EXIT_SUCCESS ||
        test_handle_scope_state_and_failure_order() != EXIT_SUCCESS ||
        test_limits_terminal_tokens_and_concurrent_builders() != EXIT_SUCCESS ||
        (!semantic_only &&
         test_allocation_failures_preserve_retryable_state() != EXIT_SUCCESS)) {
        return EXIT_FAILURE;
    }
    return semantic_only ? EXIT_SUCCESS
                         : test_fifty_thousand_object_cycle_is_iterative();
}
