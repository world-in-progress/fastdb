#include "payload/layout/RecordLayout.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/CheckedMath.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace fastdb::payload::layout {
namespace {

using build::LogicalPayload;
using build::NodeIndex;
using build::ValueNode;
using build::ValueTag;
using build::invalid_node_index;
using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;
using spec::Cardinality;
using spec::TypeKind;
using spec::TypeNode;

Error layout_error(const JsonPointer& path, const char* reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_INTERNAL, path,
        "Logical payload is inconsistent with its compiled runtime schema",
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

Error allocation_error() {
    return Error::from_details(
        FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
        "Portable record layout allocation failed",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"allocation_failed"}}}));
}

bool record_kind(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::f32:
    case TypeKind::f64:
    case TypeKind::u8n:
    case TypeKind::u16n:
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
    case TypeKind::component:
    case TypeKind::list:
        return true;
    case TypeKind::ref:
        return false;
    }
    return false;
}

ValueTag variable_tag(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::str:
        return ValueTag::str;
    case TypeKind::wstr:
        return ValueTag::wstr;
    case TypeKind::bytes:
        return ValueTag::bytes;
    default:
        return ValueTag::null_value;
    }
}

Result<std::vector<NodeIndex>> collect_entry_values(
    const LogicalPayload& values,
    std::uint32_t entry_index,
    const spec::Entry& entry) {
    if (entry_index >= values.entry_roots().size()) {
        return Result<std::vector<NodeIndex>>::failure(layout_error(
            JsonPointer{}.append("entries").append(entry.id),
            "entry_root_missing"));
    }
    const NodeIndex root_index = values.entry_roots()[entry_index];
    if (root_index == build::invalid_node_index ||
        root_index >= values.nodes().size()) {
        return Result<std::vector<NodeIndex>>::failure(layout_error(
            JsonPointer{}.append("entries").append(entry.id),
            "entry_root_out_of_range"));
    }
    const ValueNode& root = values.nodes()[static_cast<std::size_t>(root_index)];
    if (root.tag != ValueTag::sequence) {
        return Result<std::vector<NodeIndex>>::failure(layout_error(
            JsonPointer{}.append("entries").append(entry.id),
            "entry_root_not_sequence"));
    }
    if (root.child_count > values.nodes().size()) {
        return Result<std::vector<NodeIndex>>::failure(layout_error(
            JsonPointer{}.append("entries").append(entry.id),
            "entry_value_count_out_of_range"));
    }
    std::vector<NodeIndex> result;
    result.reserve(static_cast<std::size_t>(root.child_count));
    NodeIndex current = root.first_child;
    for (std::uint64_t index = UINT64_C(0); index < root.child_count; ++index) {
        if (current == build::invalid_node_index ||
            current >= values.nodes().size()) {
            return Result<std::vector<NodeIndex>>::failure(layout_error(
                JsonPointer{}.append("entries").append(entry.id),
                "entry_value_chain_out_of_range"));
        }
        result.push_back(current);
        current = values.nodes()[static_cast<std::size_t>(current)].next_sibling;
    }
    if (current != build::invalid_node_index) {
        return Result<std::vector<NodeIndex>>::failure(layout_error(
            JsonPointer{}.append("entries").append(entry.id),
            "entry_value_chain_too_long"));
    }
    return Result<std::vector<NodeIndex>>::success(std::move(result));
}

bool tag_matches(TypeKind kind, ValueTag tag) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return tag == ValueTag::boolean;
    case TypeKind::u8:
        return tag == ValueTag::u8;
    case TypeKind::u16:
        return tag == ValueTag::u16;
    case TypeKind::u32:
        return tag == ValueTag::u32;
    case TypeKind::i32:
        return tag == ValueTag::i32;
    case TypeKind::u8n:
        return tag == ValueTag::u8n;
    case TypeKind::u16n:
        return tag == ValueTag::u16n;
    case TypeKind::f32:
        return tag == ValueTag::f32;
    case TypeKind::f64:
        return tag == ValueTag::f64;
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
        return tag == variable_tag(kind);
    case TypeKind::component:
        return tag == ValueTag::component;
    case TypeKind::list:
        return tag == ValueTag::list;
    case TypeKind::ref:
        return false;
    }
    return false;
}

struct LogicalFacts final {
    std::vector<DescriptorFact> descriptors;
    std::vector<std::uint64_t> descriptor_indexes;
    std::vector<ListAggregate> lists;
    std::vector<std::uint32_t> list_indexes;
    std::vector<NodeIndex> variable_values;
    std::uint64_t utf8_bytes{UINT64_C(0)};
    std::uint64_t utf16_bytes{UINT64_C(0)};
    std::uint64_t opaque_bytes{UINT64_C(0)};
    std::uint64_t nested_validation_work{UINT64_C(0)};
};

struct PendingSlot final {
    NodeIndex node_index;
    std::uint32_t expected_runtime_type_id;
};

struct PendingListItems final {
    NodeIndex next_child;
    std::uint64_t remaining;
    std::uint32_t expected_runtime_type_id;
    std::uint32_t aggregate_index;
};

struct PendingEntryRoots final {
    std::size_t entry_index;
    std::size_t next_value;
};

struct PendingComponentFields final {
    const ComponentLayout* component;
    NodeIndex next_child;
    std::size_t next_field;
    std::uint64_t cursor;
};

using PendingLogical =
    std::variant<PendingSlot, PendingListItems, PendingEntryRoots,
                 PendingComponentFields>;

Result<void> add_nested_work(std::uint64_t& work,
                             std::uint64_t units) {
    return checked_accumulate_u64(
        work, units, JsonPointer{}.append("validation_work"));
}

Result<void> append_descriptor(LogicalFacts& facts,
                               NodeIndex node_index,
                               std::uint32_t runtime_type_id,
                               std::uint64_t first,
                               std::uint64_t count) {
    if (node_index >= facts.descriptor_indexes.size() ||
        facts.descriptor_indexes[static_cast<std::size_t>(node_index)] !=
            UINT64_MAX) {
        return Result<void>::failure(layout_error(
            JsonPointer{}.append("descriptors"),
            "descriptor_fact_duplicate"));
    }
    facts.descriptor_indexes[static_cast<std::size_t>(node_index)] =
        facts.descriptors.size();
    facts.descriptors.push_back(
        DescriptorFact{node_index, runtime_type_id, first, count});
    return Result<void>::success();
}

Result<LogicalFacts> collect_logical_facts(
    const RuntimeSchema& runtime,
    const LogicalPayload& values,
    const std::vector<std::vector<NodeIndex>>& entry_values) {
    LogicalFacts facts;
    facts.descriptor_indexes.assign(values.nodes().size(), UINT64_MAX);
    facts.list_indexes.assign(runtime.type_count(), UINT32_MAX);
    facts.lists.reserve(runtime.list_nodes().size());
    for (const ListNodeLayout& list : runtime.list_nodes()) {
        if (list.owner_runtime_type_id >= facts.list_indexes.size()) {
            return Result<LogicalFacts>::failure(layout_error(
                JsonPointer{}.append("runtime").append("lists"),
                "list_runtime_type_out_of_range"));
        }
        const auto aggregate_index =
            static_cast<std::uint32_t>(facts.lists.size());
        facts.list_indexes[list.owner_runtime_type_id] = aggregate_index;
        facts.lists.push_back(ListAggregate{
            list.owner_runtime_type_id, list.item_runtime_type_id, UINT32_MAX,
            UINT32_MAX, {}});
    }

    const auto& entries = runtime.spec().resolved().entries();
    if (entries.size() != entry_values.size()) {
        return Result<LogicalFacts>::failure(layout_error(
            JsonPointer{}.append("entries"),
            "entry_value_inventory_mismatch"));
    }
    std::vector<PendingLogical> pending;
    if (!entries.empty()) {
        pending.push_back(PendingEntryRoots{0U, 0U});
    }

    const std::uint64_t storage_size = values.byte_storage().size();
    while (!pending.empty()) {
        if (auto* roots =
                std::get_if<PendingEntryRoots>(&pending.back())) {
            while (roots->entry_index < entry_values.size() &&
                   roots->next_value ==
                       entry_values[roots->entry_index].size()) {
                ++roots->entry_index;
                roots->next_value = 0U;
            }
            if (roots->entry_index == entry_values.size()) {
                pending.pop_back();
                continue;
            }
            const std::size_t entry_index = roots->entry_index;
            const NodeIndex node_index =
                entry_values[entry_index][roots->next_value++];
            pending.push_back(PendingSlot{
                node_index,
                runtime.runtime_id(entries[entry_index].type)});
            continue;
        }
        if (auto* continuation =
                std::get_if<PendingListItems>(&pending.back())) {
            if (continuation->remaining == UINT64_C(0)) {
                if (continuation->next_child != invalid_node_index) {
                    return Result<LogicalFacts>::failure(layout_error(
                        JsonPointer{}.append("lists"),
                        "list_child_chain_too_long"));
                }
                pending.pop_back();
                continue;
            }
            if (continuation->next_child == invalid_node_index ||
                continuation->next_child >= values.nodes().size() ||
                continuation->aggregate_index >= facts.lists.size()) {
                return Result<LogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("lists"),
                    "list_child_out_of_range"));
            }
            const NodeIndex child = continuation->next_child;
            continuation->next_child =
                values.nodes()[static_cast<std::size_t>(child)].next_sibling;
            --continuation->remaining;
            facts.lists[continuation->aggregate_index]
                .item_nodes.push_back(child);
            pending.push_back(PendingSlot{
                child, continuation->expected_runtime_type_id});
            continue;
        }
        if (auto* continuation =
                std::get_if<PendingComponentFields>(&pending.back())) {
            if (continuation->next_field ==
                continuation->component->fields.size()) {
                if (continuation->next_child != invalid_node_index ||
                    continuation->cursor >
                        continuation->component->stride) {
                    return Result<LogicalFacts>::failure(layout_error(
                        JsonPointer{}.append("values"),
                        "component_child_chain_mismatch"));
                }
                auto tail = add_nested_work(
                    facts.nested_validation_work,
                    static_cast<std::uint64_t>(
                        continuation->component->stride) -
                        continuation->cursor);
                if (!tail.has_value()) {
                    return Result<LogicalFacts>::failure(
                        std::move(tail).error());
                }
                pending.pop_back();
                continue;
            }
            const ComponentFieldLayout& field =
                continuation->component
                    ->fields[continuation->next_field++];
            const NodeIndex child = continuation->next_child;
            if (field.offset < continuation->cursor ||
                child == invalid_node_index ||
                child >= values.nodes().size()) {
                return Result<LogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "component_child_layout_mismatch"));
            }
            auto added = add_nested_work(
                facts.nested_validation_work,
                static_cast<std::uint64_t>(field.offset) -
                    continuation->cursor);
            if (!added.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(added).error());
            }
            added = add_nested_work(facts.nested_validation_work,
                                    UINT64_C(1));
            if (!added.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(added).error());
            }
            auto field_end = checked_add_u64(
                field.offset, field.slot_stride,
                JsonPointer{}.append("validation_work"));
            if (!field_end.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(field_end).error());
            }
            continuation->cursor = field_end.value();
            continuation->next_child =
                values.nodes()[static_cast<std::size_t>(child)].next_sibling;
            pending.push_back(
                PendingSlot{child, field.runtime_type_id});
            continue;
        }
        const PendingSlot slot = std::get<PendingSlot>(pending.back());
        pending.pop_back();
        if (slot.node_index == invalid_node_index ||
            slot.node_index >= values.nodes().size()) {
            return Result<LogicalFacts>::failure(layout_error(
                JsonPointer{}.append("values"), "slot_node_out_of_range"));
        }
        const ValueNode& node =
            values.nodes()[static_cast<std::size_t>(slot.node_index)];
        const RuntimeType* const runtime_type =
            runtime.find_type(slot.expected_runtime_type_id);
        if (runtime_type == nullptr ||
            node.runtime_type_id != slot.expected_runtime_type_id) {
            return Result<LogicalFacts>::failure(layout_error(
                JsonPointer{}.append("values"),
                "slot_runtime_type_mismatch"));
        }
        const TypeKind kind = runtime_type->source->kind;
        if (node.tag == ValueTag::null_value) {
            if (!runtime_type->source->nullable ||
                node.first_child != invalid_node_index ||
                node.child_count != UINT64_C(0)) {
                return Result<LogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "invalid_null_value_node"));
            }
            if (kind == TypeKind::str || kind == TypeKind::wstr ||
                kind == TypeKind::bytes || kind == TypeKind::list) {
                auto appended = append_descriptor(
                    facts, slot.node_index, node.runtime_type_id,
                    UINT64_C(0), UINT64_C(0));
                if (!appended.has_value()) {
                    return Result<LogicalFacts>::failure(
                        std::move(appended).error());
                }
                if (kind != TypeKind::list) {
                    facts.variable_values.push_back(slot.node_index);
                }
            } else if (kind == TypeKind::component) {
                auto added = add_nested_work(facts.nested_validation_work,
                                             runtime_type->slot.stride);
                if (!added.has_value()) {
                    return Result<LogicalFacts>::failure(
                        std::move(added).error());
                }
            }
            continue;
        }
        if (!tag_matches(kind, node.tag)) {
            return Result<LogicalFacts>::failure(layout_error(
                JsonPointer{}.append("values"), "value_tag_mismatch"));
        }

        if (kind == TypeKind::str || kind == TypeKind::wstr ||
            kind == TypeKind::bytes) {
            auto storage_end = checked_range_end(
                node.scalar_bits_or_offset, node.byte_length, storage_size,
                JsonPointer{}.append("pools"));
            if (!storage_end.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(storage_end).error());
            }
            if (kind == TypeKind::wstr &&
                node.byte_length % UINT64_C(2) != UINT64_C(0)) {
                return Result<LogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("pools"),
                    "wide_text_storage_length_is_odd"));
            }
            std::uint64_t* cursor =
                kind == TypeKind::str    ? &facts.utf8_bytes
                : kind == TypeKind::wstr ? &facts.utf16_bytes
                                         : &facts.opaque_bytes;
            auto appended = append_descriptor(
                facts, slot.node_index, node.runtime_type_id, *cursor,
                node.byte_length);
            if (!appended.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(appended).error());
            }
            auto advanced = checked_accumulate_u64(
                *cursor, node.byte_length, JsonPointer{}.append("pools"));
            if (!advanced.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(advanced).error());
            }
            facts.variable_values.push_back(slot.node_index);
            if (kind == TypeKind::str || kind == TypeKind::wstr) {
                const std::uint64_t units =
                    kind == TypeKind::str ? node.byte_length
                                          : node.byte_length / UINT64_C(2);
                auto added =
                    add_nested_work(facts.nested_validation_work, units);
                if (!added.has_value()) {
                    return Result<LogicalFacts>::failure(
                        std::move(added).error());
                }
            }
            continue;
        }

        if (kind == TypeKind::list) {
            if (node.child_count > values.nodes().size() ||
                node.runtime_type_id >= facts.list_indexes.size()) {
                return Result<LogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("lists"),
                    "list_child_count_out_of_range"));
            }
            const std::uint32_t aggregate_index =
                facts.list_indexes[node.runtime_type_id];
            if (aggregate_index >= facts.lists.size()) {
                return Result<LogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("lists"),
                    "list_aggregate_missing"));
            }
            ListAggregate& aggregate = facts.lists[aggregate_index];
            const std::uint64_t first = aggregate.item_nodes.size();
            auto item_end = checked_add_u64(
                first, node.child_count, JsonPointer{}.append("lists"));
            if (!item_end.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(item_end).error());
            }
            auto appended = append_descriptor(
                facts, slot.node_index, node.runtime_type_id, first,
                node.child_count);
            if (!appended.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(appended).error());
            }
            auto item_work = add_nested_work(
                facts.nested_validation_work, node.child_count);
            if (!item_work.has_value()) {
                return Result<LogicalFacts>::failure(
                    std::move(item_work).error());
            }
            pending.push_back(PendingListItems{
                node.first_child, node.child_count,
                aggregate.item_runtime_type_id, aggregate_index});
            continue;
        }

        if (kind != TypeKind::component) {
            if (node.first_child != invalid_node_index ||
                node.child_count != UINT64_C(0)) {
                return Result<LogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "scalar_child_invariant"));
            }
            continue;
        }

        const ComponentLayout* const component = runtime.component(
            runtime_type->source->resolved_component_index);
        if (component == nullptr ||
            node.child_count != component->fields.size()) {
            return Result<LogicalFacts>::failure(layout_error(
                JsonPointer{}.append("values"),
                "component_layout_mismatch"));
        }
        auto added = add_nested_work(facts.nested_validation_work,
                                     component->validity_bytes);
        if (!added.has_value()) {
            return Result<LogicalFacts>::failure(std::move(added).error());
        }
        pending.push_back(PendingComponentFields{
            component, node.first_child, 0U,
            component->validity_bytes});
    }
    return Result<LogicalFacts>::success(std::move(facts));
}

}  // namespace

Result<RecordLayout> RecordLayout::plan(
    const RuntimeSchema& runtime_schema,
    const LogicalPayload& values) {
    try {
        if (runtime_schema.spec().digest() != values.spec().digest()) {
            return Result<RecordLayout>::failure(Error::from_details(
                FDB_PAYLOAD_E_DIGEST_MISMATCH, JsonPointer{},
                "Runtime schema and logical payload spec digests differ",
                JsonValue::object({JsonValue::Member{
                    "reason", JsonValue{"layout_spec_digest_mismatch"}}})));
        }
        auto list_metadata = runtime_schema.validate_list_metadata();
        if (!list_metadata.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(list_metadata).error());
        }
        RecordLayout result(runtime_schema);
        const auto& source_entries =
            runtime_schema.spec().resolved().entries();
        if (source_entries.size() !=
            values.spec().resolved().entries().size()) {
            return Result<RecordLayout>::failure(layout_error(
                JsonPointer{}.append("entries"),
                "digest_equal_entry_inventory_mismatch"));
        }
        auto entry_count = checked_narrow_u32(
            source_entries.size(), JsonPointer{}.append("entries"));
        if (!entry_count.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(entry_count).error());
        }
        result.entries_.reserve(source_entries.size());
        result.entry_values_.reserve(source_entries.size());

        for (std::uint32_t entry_index = UINT32_C(0);
             entry_index < entry_count.value(); ++entry_index) {
            const spec::Entry& entry = source_entries[entry_index];
            if (!record_kind(entry.type.kind)) {
                return Result<RecordLayout>::failure(layout_error(
                    JsonPointer{}.append("entries").append(entry.id),
                    "record_ref_invariant"));
            }
            auto nodes = collect_entry_values(values, entry_index, entry);
            if (!nodes.has_value()) {
                return Result<RecordLayout>::failure(std::move(nodes).error());
            }
            const std::uint64_t value_count = nodes.value().size();
            if (entry.cardinality == Cardinality::one &&
                value_count != UINT64_C(1)) {
                return Result<RecordLayout>::failure(layout_error(
                    JsonPointer{}.append("entries").append(entry.id),
                    "one_entry_value_count"));
            }
            auto new_root_count = checked_add_u64(
                result.root_value_count_, value_count,
                JsonPointer{}.append("entries").append(entry.id));
            if (!new_root_count.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(new_root_count).error());
            }
            result.root_value_count_ = new_root_count.value();
            const std::uint32_t type_id = runtime_schema.runtime_id(entry.type);
            const RuntimeType* const runtime_type =
                runtime_schema.find_type(type_id);
            if (type_id == UINT32_MAX || runtime_type == nullptr) {
                return Result<RecordLayout>::failure(layout_error(
                    JsonPointer{}.append("entries").append(entry.id),
                    "entry_runtime_type_unassigned"));
            }
            const SlotLayout slot = runtime_type->slot;
            std::uint32_t validity_region = UINT32_MAX;
            if (entry.type.nullable) {
                auto validity_index = checked_narrow_u32(
                    result.regions_.size(),
                    JsonPointer{}.append("entries").append(entry.id));
                if (!validity_index.has_value()) {
                    return Result<RecordLayout>::failure(
                        std::move(validity_index).error());
                }
                validity_region = validity_index.value();
                auto rounded = checked_add_u64(
                    value_count, UINT64_C(7),
                    JsonPointer{}.append("entries").append(entry.id));
                if (!rounded.has_value()) {
                    return Result<RecordLayout>::failure(
                        std::move(rounded).error());
                }
                result.regions_.push_back(RegionDescriptor{
                    RegionKind::entry_validity, UINT32_C(0), entry_index,
                    type_id, UINT64_C(0), rounded.value() / UINT64_C(8),
                    value_count, UINT32_C(0), UINT32_C(1)});
            }
            auto values_index = checked_narrow_u32(
                result.regions_.size(),
                JsonPointer{}.append("entries").append(entry.id));
            if (!values_index.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(values_index).error());
            }
            auto byte_length = checked_multiply_u64(
                value_count, slot.stride,
                JsonPointer{}.append("entries").append(entry.id));
            if (!byte_length.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(byte_length).error());
            }
            result.regions_.push_back(RegionDescriptor{
                RegionKind::entry_values, UINT32_C(0), entry_index, type_id,
                UINT64_C(0), byte_length.value(), value_count, slot.stride,
                slot.alignment});
            result.entries_.push_back(EntryDescriptor{
                entry_index, type_id,
                entry.cardinality == Cardinality::one ? UINT32_C(1)
                                                      : UINT32_C(2),
                entry.type.nullable ? UINT32_C(1) : UINT32_C(0), value_count,
                values_index.value(), validity_region});
            result.entry_values_.push_back(std::move(nodes).value());
        }

        auto logical = collect_logical_facts(
            runtime_schema, values, result.entry_values_);
        if (!logical.has_value()) {
            return Result<RecordLayout>::failure(std::move(logical).error());
        }
        const std::uint64_t utf8_bytes = logical.value().utf8_bytes;
        const std::uint64_t utf16_bytes = logical.value().utf16_bytes;
        const std::uint64_t opaque_bytes = logical.value().opaque_bytes;
        const std::uint64_t nested_validation_work =
            logical.value().nested_validation_work;
        result.variable_values_ =
            std::move(logical.value().variable_values);
        result.descriptor_facts_ =
            std::move(logical.value().descriptors);
        result.descriptor_fact_indexes_ =
            std::move(logical.value().descriptor_indexes);
        result.list_aggregates_ = std::move(logical.value().lists);
        result.list_aggregate_indexes_ =
            std::move(logical.value().list_indexes);

        for (ListAggregate& aggregate : result.list_aggregates_) {
            const RuntimeType* const item = runtime_schema.find_type(
                aggregate.item_runtime_type_id);
            if (item == nullptr) {
                return Result<RecordLayout>::failure(layout_error(
                    JsonPointer{}.append("runtime").append("lists"),
                    "list_item_runtime_type_missing"));
            }
            const std::uint64_t item_count = aggregate.item_nodes.size();
            const JsonPointer path = JsonPointer{}
                                         .append("lists")
                                         .append(aggregate.owner_runtime_type_id);
            if (item->source->nullable) {
                auto validity_index = checked_narrow_u32(
                    result.regions_.size(), path);
                if (!validity_index.has_value()) {
                    return Result<RecordLayout>::failure(
                        std::move(validity_index).error());
                }
                auto rounded =
                    checked_add_u64(item_count, UINT64_C(7), path);
                if (!rounded.has_value()) {
                    return Result<RecordLayout>::failure(
                        std::move(rounded).error());
                }
                aggregate.validity_region_index = validity_index.value();
                result.regions_.push_back(RegionDescriptor{
                    RegionKind::list_validity, UINT32_C(0),
                    aggregate.owner_runtime_type_id,
                    aggregate.item_runtime_type_id, UINT64_C(0),
                    rounded.value() / UINT64_C(8), item_count, UINT32_C(0),
                    UINT32_C(1)});
            }
            auto items_index = checked_narrow_u32(
                result.regions_.size(), path);
            if (!items_index.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(items_index).error());
            }
            auto byte_length = checked_multiply_u64(
                item_count, item->slot.stride, path);
            if (!byte_length.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(byte_length).error());
            }
            aggregate.items_region_index = items_index.value();
            result.regions_.push_back(RegionDescriptor{
                RegionKind::list_items, UINT32_C(0),
                aggregate.owner_runtime_type_id,
                aggregate.item_runtime_type_id, UINT64_C(0),
                byte_length.value(), item_count, item->slot.stride,
                item->slot.alignment});
        }

        auto add_pool = [&result](RegionKind kind, std::uint64_t byte_length,
                                  std::uint64_t element_count,
                                  std::uint32_t alignment) {
            result.regions_.push_back(RegionDescriptor{
                kind, UINT32_C(0), UINT32_MAX, UINT32_MAX, UINT64_C(0),
                byte_length, element_count, UINT32_C(0), alignment});
        };
        if (runtime_schema.has_utf8_pool()) {
            add_pool(RegionKind::utf8_pool, utf8_bytes, utf8_bytes,
                     UINT32_C(1));
        }
        if (runtime_schema.has_utf16_pool()) {
            add_pool(RegionKind::utf16_pool, utf16_bytes,
                     utf16_bytes / UINT64_C(2), UINT32_C(2));
        }
        if (runtime_schema.has_bytes_pool()) {
            add_pool(RegionKind::bytes_pool, opaque_bytes, opaque_bytes,
                     UINT32_C(1));
        }

        auto region_count = checked_narrow_u32(
            result.regions_.size(), JsonPointer{}.append("regions"));
        if (!region_count.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(region_count).error());
        }
        auto region_bytes = checked_multiply_u64(
            region_count.value(), region_descriptor_size,
            JsonPointer{}.append("regions"));
        if (!region_bytes.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(region_bytes).error());
        }
        auto entry_directory_offset = checked_add_u64(
            header_size, region_bytes.value(), JsonPointer{}.append("entries"));
        if (!entry_directory_offset.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(entry_directory_offset).error());
        }
        auto entry_bytes = checked_multiply_u64(
            entry_count.value(), entry_descriptor_size,
            JsonPointer{}.append("entries"));
        if (!entry_bytes.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(entry_bytes).error());
        }
        auto entry_end = checked_add_u64(
            entry_directory_offset.value(), entry_bytes.value(),
            JsonPointer{}.append("entries"));
        if (!entry_end.has_value()) {
            return Result<RecordLayout>::failure(std::move(entry_end).error());
        }
        auto cursor = checked_align_up_u64(
            entry_end.value(), UINT32_C(8), JsonPointer{}.append("regions"));
        if (!cursor.has_value()) {
            return Result<RecordLayout>::failure(std::move(cursor).error());
        }
        for (RegionDescriptor& region : result.regions_) {
            auto aligned = checked_align_up_u64(
                cursor.value(), region.alignment,
                JsonPointer{}.append("regions").append(region.owner_index));
            if (!aligned.has_value()) {
                return Result<RecordLayout>::failure(std::move(aligned).error());
            }
            region.data_offset = aligned.value();
            cursor = checked_add_u64(
                region.data_offset, region.byte_length,
                JsonPointer{}.append("regions").append(region.owner_index));
            if (!cursor.has_value()) {
                return Result<RecordLayout>::failure(std::move(cursor).error());
            }
        }
        auto total = checked_align_up_u64(
            cursor.value(), UINT32_C(8), JsonPointer{}.append("total_length"));
        if (!total.has_value()) {
            return Result<RecordLayout>::failure(std::move(total).error());
        }
        result.total_length_ = total.value();

        std::uint64_t work = UINT64_C(1);
        auto add_work = [&work](std::uint64_t units) -> Result<void> {
            return checked_accumulate_u64(
                work, units, JsonPointer{}.append("validation_work"));
        };
        auto work_result = add_work(result.regions_.size());
        if (!work_result.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(work_result).error());
        }
        work_result = add_work(result.entries_.size());
        if (!work_result.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(work_result).error());
        }
        work_result = add_work(total.value() - cursor.value());
        if (!work_result.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(work_result).error());
        }
        work_result = add_work(
            (result.regions_.empty() ? total.value()
                                     : result.regions_.front().data_offset) -
            entry_end.value());
        if (!work_result.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(work_result).error());
        }
        std::uint64_t previous_end = result.regions_.empty()
                                         ? total.value()
                                         : result.regions_.front().data_offset;
        for (const RegionDescriptor& region : result.regions_) {
            work_result = add_work(region.data_offset - previous_end);
            if (!work_result.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(work_result).error());
            }
            if (region.kind == RegionKind::entry_validity ||
                region.kind == RegionKind::list_validity) {
                work_result = add_work(region.byte_length);
            } else if (region.kind == RegionKind::entry_values) {
                work_result = add_work(region.element_count);
            }
            if (!work_result.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(work_result).error());
            }
            auto region_end = checked_add_u64(
                region.data_offset, region.byte_length,
                JsonPointer{}.append("validation_work"));
            if (!region_end.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(region_end).error());
            }
            previous_end = region_end.value();
        }
        work_result = add_work(nested_validation_work);
        if (!work_result.has_value()) {
            return Result<RecordLayout>::failure(
                std::move(work_result).error());
        }
        result.validation_work_ = work;
        return Result<RecordLayout>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<RecordLayout>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::layout

// A RecordLayout travels through the noexcept plan ownership transfers
// (ProfileLayout, Result<BuildPlan>), so its move must not allocate or throw;
// see RuntimeSchema.cpp for the failure this guards against.
static_assert(
    std::is_nothrow_move_constructible_v<
        fastdb::payload::layout::RecordLayout>,
    "RecordLayout must be nothrow move constructible");
static_assert(
    std::is_nothrow_move_assignable_v<
        fastdb::payload::layout::RecordLayout>,
    "RecordLayout must be nothrow move assignable");
