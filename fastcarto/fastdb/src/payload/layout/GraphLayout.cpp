#include "payload/layout/GraphLayout.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/CheckedMath.hpp"

#include <fastdb_payload.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::payload::layout {
namespace {

using build::LogicalPayload;
using build::NodeIndex;
using build::ObjectCoordinate;
using build::ValueNode;
using build::ValueTag;
using build::invalid_node_index;
using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;
using spec::Cardinality;
using spec::Profile;
using spec::StorageRole;
using spec::TypeKind;

Error layout_error(const JsonPointer& path, const char* reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_INTERNAL, path,
        "Logical graph is inconsistent with its compiled runtime schema",
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

Error allocation_error() {
    return Error::from_details(
        FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
        "Portable graph layout allocation failed",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"allocation_failed"}}}));
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
    if (root_index == invalid_node_index ||
        root_index >= values.nodes().size()) {
        return Result<std::vector<NodeIndex>>::failure(layout_error(
            JsonPointer{}.append("entries").append(entry.id),
            "entry_root_out_of_range"));
    }
    const ValueNode& root =
        values.nodes()[static_cast<std::size_t>(root_index)];
    if (root.tag != ValueTag::sequence ||
        root.child_count > values.nodes().size()) {
        return Result<std::vector<NodeIndex>>::failure(layout_error(
            JsonPointer{}.append("entries").append(entry.id),
            "entry_root_not_sequence"));
    }
    std::vector<NodeIndex> result;
    result.reserve(static_cast<std::size_t>(root.child_count));
    NodeIndex child = root.first_child;
    for (std::uint64_t index = UINT64_C(0); index < root.child_count; ++index) {
        if (child == invalid_node_index || child >= values.nodes().size()) {
            return Result<std::vector<NodeIndex>>::failure(layout_error(
                JsonPointer{}.append("entries").append(entry.id),
                "entry_value_chain_out_of_range"));
        }
        result.push_back(child);
        child = values.nodes()[static_cast<std::size_t>(child)].next_sibling;
    }
    if (child != invalid_node_index) {
        return Result<std::vector<NodeIndex>>::failure(layout_error(
            JsonPointer{}.append("entries").append(entry.id),
            "entry_value_chain_too_long"));
    }
    return Result<std::vector<NodeIndex>>::success(std::move(result));
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

bool scalar_tag_matches(TypeKind kind, ValueTag tag) noexcept {
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
    case TypeKind::component:
    case TypeKind::ref:
    case TypeKind::list:
        return false;
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
        return tag == variable_tag(kind);
    }
    return false;
}

Result<void> add_work(std::uint64_t& work, std::uint64_t units) {
    return checked_accumulate_u64(
        work, units, JsonPointer{}.append("validation_work"));
}

struct PendingSlot final {
    NodeIndex node_index;
    std::uint32_t runtime_type_id;
};

struct PendingEntryRoots final {
    std::size_t entry_index;
    std::size_t next_value;
};

struct PendingObjectRoots final {
    std::size_t aggregate_index;
    std::size_t object_index;
};

struct PendingListItems final {
    NodeIndex next_child;
    std::uint64_t remaining;
    std::uint32_t runtime_type_id;
    std::uint32_t aggregate_index;
};

struct PendingComponent final {
    const ComponentLayout* component;
    NodeIndex next_child;
    std::size_t next_field;
    std::uint64_t cursor;
};

using Pending =
    std::variant<PendingSlot, PendingEntryRoots, PendingObjectRoots,
                 PendingListItems, PendingComponent>;

struct GraphLogicalFacts final {
    std::vector<NodeIndex> variable_values;
    std::vector<DescriptorFact> descriptors;
    std::vector<std::uint64_t> descriptor_indexes;
    std::vector<ListAggregate> lists;
    std::vector<std::uint32_t> list_indexes;
    std::uint64_t utf8_bytes{UINT64_C(0)};
    std::uint64_t utf16_bytes{UINT64_C(0)};
    std::uint64_t opaque_bytes{UINT64_C(0)};
    std::uint64_t nested_work{UINT64_C(0)};
};

Result<void> append_descriptor(GraphLogicalFacts& facts,
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

Result<GraphLogicalFacts> collect_logical_facts(
    const RuntimeSchema& runtime,
    const LogicalPayload& values,
    const std::vector<std::vector<NodeIndex>>& entries,
    const std::vector<ObjectAggregate>& objects) {
    GraphLogicalFacts facts;
    facts.descriptor_indexes.assign(values.nodes().size(), UINT64_MAX);
    facts.list_indexes.assign(runtime.type_count(), UINT32_MAX);
    facts.lists.reserve(runtime.list_nodes().size());
    for (const ListNodeLayout& list : runtime.list_nodes()) {
        if (list.owner_runtime_type_id >= facts.list_indexes.size()) {
            return Result<GraphLogicalFacts>::failure(layout_error(
                JsonPointer{}.append("runtime").append("lists"),
                "list_runtime_type_out_of_range"));
        }
        const auto aggregate_index =
            static_cast<std::uint32_t>(facts.lists.size());
        facts.list_indexes[list.owner_runtime_type_id] = aggregate_index;
        facts.lists.push_back(ListAggregate{
            list.owner_runtime_type_id, list.item_runtime_type_id,
            UINT32_MAX, UINT32_MAX, {}});
    }

    const auto& source_entries = runtime.spec().resolved().entries();
    if (entries.size() != source_entries.size()) {
        return Result<GraphLogicalFacts>::failure(layout_error(
            JsonPointer{}.append("entries"),
            "entry_value_inventory_mismatch"));
    }
    std::vector<Pending> pending;
    pending.push_back(PendingObjectRoots{0U, 0U});
    if (!entries.empty()) {
        pending.push_back(PendingEntryRoots{0U, 0U});
    }

    const std::uint64_t storage_size = values.byte_storage().size();
    while (!pending.empty()) {
        if (auto* roots = std::get_if<PendingEntryRoots>(&pending.back())) {
            while (roots->entry_index < entries.size() &&
                   roots->next_value == entries[roots->entry_index].size()) {
                ++roots->entry_index;
                roots->next_value = 0U;
            }
            if (roots->entry_index == entries.size()) {
                pending.pop_back();
                continue;
            }
            const std::size_t entry_index = roots->entry_index;
            const NodeIndex node =
                entries[entry_index][roots->next_value++];
            pending.push_back(PendingSlot{
                node, runtime.runtime_id(source_entries[entry_index].type)});
            continue;
        }
        if (auto* roots = std::get_if<PendingObjectRoots>(&pending.back())) {
            while (roots->aggregate_index < objects.size() &&
                   roots->object_index ==
                       objects[roots->aggregate_index].object_nodes.size()) {
                ++roots->aggregate_index;
                roots->object_index = 0U;
            }
            if (roots->aggregate_index == objects.size()) {
                pending.pop_back();
                continue;
            }
            const ObjectAggregate& aggregate =
                objects[roots->aggregate_index];
            const std::size_t object_id = roots->object_index++;
            const NodeIndex node = aggregate.object_nodes[object_id];
            const ComponentLayout* const component =
                runtime.component(aggregate.component_index);
            if (component == nullptr || node >= values.nodes().size()) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("objects"),
                    "object_component_layout_missing"));
            }
            const ValueNode& object =
                values.nodes()[static_cast<std::size_t>(node)];
            if (object.tag != ValueTag::object_record ||
                object.runtime_type_id != UINT32_MAX ||
                object.object_component_index != aggregate.component_index ||
                object.object_id != object_id ||
                object.child_count != component->fields.size()) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("objects"),
                    "object_record_mismatch"));
            }
            auto validity =
                add_work(facts.nested_work, component->validity_bytes);
            if (!validity.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(validity).error());
            }
            pending.push_back(PendingComponent{
                component, object.first_child, 0U,
                component->validity_bytes});
            continue;
        }
        if (auto* list = std::get_if<PendingListItems>(&pending.back())) {
            if (list->remaining == UINT64_C(0)) {
                if (list->next_child != invalid_node_index) {
                    return Result<GraphLogicalFacts>::failure(layout_error(
                        JsonPointer{}.append("lists"),
                        "list_child_chain_too_long"));
                }
                pending.pop_back();
                continue;
            }
            if (list->next_child == invalid_node_index ||
                list->next_child >= values.nodes().size() ||
                list->aggregate_index >= facts.lists.size()) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("lists"),
                    "list_child_out_of_range"));
            }
            const NodeIndex child = list->next_child;
            list->next_child =
                values.nodes()[static_cast<std::size_t>(child)].next_sibling;
            --list->remaining;
            facts.lists[list->aggregate_index].item_nodes.push_back(child);
            pending.push_back(PendingSlot{child, list->runtime_type_id});
            continue;
        }
        if (auto* component = std::get_if<PendingComponent>(&pending.back())) {
            if (component->next_field == component->component->fields.size()) {
                if (component->next_child != invalid_node_index ||
                    component->cursor > component->component->stride) {
                    return Result<GraphLogicalFacts>::failure(layout_error(
                        JsonPointer{}.append("values"),
                        "component_child_chain_mismatch"));
                }
                auto tail = add_work(
                    facts.nested_work,
                    static_cast<std::uint64_t>(component->component->stride) -
                        component->cursor);
                if (!tail.has_value()) {
                    return Result<GraphLogicalFacts>::failure(
                        std::move(tail).error());
                }
                pending.pop_back();
                continue;
            }
            const ComponentFieldLayout& field =
                component->component->fields[component->next_field++];
            const NodeIndex child = component->next_child;
            if (child == invalid_node_index || child >= values.nodes().size() ||
                field.offset < component->cursor) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "component_child_layout_mismatch"));
            }
            auto padding = add_work(
                facts.nested_work,
                static_cast<std::uint64_t>(field.offset) - component->cursor);
            if (!padding.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(padding).error());
            }
            auto field_work = add_work(facts.nested_work, UINT64_C(1));
            if (!field_work.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(field_work).error());
            }
            auto field_end = checked_add_u64(
                field.offset, field.slot_stride,
                JsonPointer{}.append("validation_work"));
            if (!field_end.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(field_end).error());
            }
            component->cursor = field_end.value();
            component->next_child =
                values.nodes()[static_cast<std::size_t>(child)].next_sibling;
            pending.push_back(PendingSlot{child, field.runtime_type_id});
            continue;
        }

        const PendingSlot slot = std::get<PendingSlot>(pending.back());
        pending.pop_back();
        if (slot.node_index >= values.nodes().size()) {
            return Result<GraphLogicalFacts>::failure(layout_error(
                JsonPointer{}.append("values"), "slot_node_out_of_range"));
        }
        const ValueNode& node =
            values.nodes()[static_cast<std::size_t>(slot.node_index)];
        const RuntimeType* const type = runtime.find_type(slot.runtime_type_id);
        if (type == nullptr || node.runtime_type_id != slot.runtime_type_id) {
            return Result<GraphLogicalFacts>::failure(layout_error(
                JsonPointer{}.append("values"),
                "slot_runtime_type_mismatch"));
        }
        const TypeKind kind = type->source->kind;
        if (node.tag == ValueTag::null_value) {
            if (!type->source->nullable ||
                node.first_child != invalid_node_index ||
                node.child_count != UINT64_C(0)) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"), "invalid_null_value_node"));
            }
            if (type->storage_role == StorageRole::inline_component) {
                auto zero_scan =
                    add_work(facts.nested_work, type->slot.stride);
                if (!zero_scan.has_value()) {
                    return Result<GraphLogicalFacts>::failure(
                        std::move(zero_scan).error());
                }
            } else if (kind == TypeKind::str || kind == TypeKind::wstr ||
                       kind == TypeKind::bytes || kind == TypeKind::list) {
                auto appended = append_descriptor(
                    facts, slot.node_index, node.runtime_type_id,
                    UINT64_C(0), UINT64_C(0));
                if (!appended.has_value()) {
                    return Result<GraphLogicalFacts>::failure(
                        std::move(appended).error());
                }
                if (kind != TypeKind::list) {
                    facts.variable_values.push_back(slot.node_index);
                }
            }
            continue;
        }

        if (type->storage_role == StorageRole::object_root_id ||
            type->storage_role == StorageRole::reference_id) {
            const ValueTag expected =
                type->storage_role == StorageRole::object_root_id
                    ? ValueTag::object_root
                    : ValueTag::reference;
            if (node.tag != expected ||
                node.object_component_index !=
                    type->source->resolved_component_index ||
                node.first_child != invalid_node_index ||
                node.child_count != UINT64_C(0) ||
                node.object_component_index >= values.object_pools().size() ||
                node.object_id >=
                    values.object_pools()[node.object_component_index].size()) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "graph_coordinate_mismatch"));
            }
            continue;
        }

        if (kind == TypeKind::str || kind == TypeKind::wstr ||
            kind == TypeKind::bytes) {
            if (node.tag != variable_tag(kind)) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "value_tag_mismatch"));
            }
            auto storage_end = checked_range_end(
                node.scalar_bits_or_offset, node.byte_length, storage_size,
                JsonPointer{}.append("pools"));
            if (!storage_end.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(storage_end).error());
            }
            if (kind == TypeKind::wstr &&
                node.byte_length % UINT64_C(2) != UINT64_C(0)) {
                return Result<GraphLogicalFacts>::failure(layout_error(
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
                return Result<GraphLogicalFacts>::failure(
                    std::move(appended).error());
            }
            auto advanced = checked_accumulate_u64(
                *cursor, node.byte_length, JsonPointer{}.append("pools"));
            if (!advanced.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(advanced).error());
            }
            facts.variable_values.push_back(slot.node_index);
            if (kind == TypeKind::str || kind == TypeKind::wstr) {
                const std::uint64_t units =
                    kind == TypeKind::str
                        ? node.byte_length
                        : node.byte_length / UINT64_C(2);
                auto text_work = add_work(facts.nested_work, units);
                if (!text_work.has_value()) {
                    return Result<GraphLogicalFacts>::failure(
                        std::move(text_work).error());
                }
            }
            continue;
        }

        if (kind == TypeKind::list) {
            if (node.tag != ValueTag::list ||
                node.child_count > values.nodes().size() ||
                node.runtime_type_id >= facts.list_indexes.size()) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("lists"),
                    "list_child_count_out_of_range"));
            }
            const std::uint32_t aggregate_index =
                facts.list_indexes[node.runtime_type_id];
            if (aggregate_index >= facts.lists.size()) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("lists"),
                    "list_aggregate_missing"));
            }
            ListAggregate& aggregate = facts.lists[aggregate_index];
            const std::uint64_t first = aggregate.item_nodes.size();
            auto item_end = checked_add_u64(
                first, node.child_count, JsonPointer{}.append("lists"));
            if (!item_end.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(item_end).error());
            }
            auto appended = append_descriptor(
                facts, slot.node_index, node.runtime_type_id, first,
                node.child_count);
            if (!appended.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(appended).error());
            }
            auto item_work = add_work(facts.nested_work, node.child_count);
            if (!item_work.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(item_work).error());
            }
            pending.push_back(PendingListItems{
                node.first_child, node.child_count,
                aggregate.item_runtime_type_id, aggregate_index});
            continue;
        }

        if (type->storage_role == StorageRole::inline_component) {
            if (node.tag != ValueTag::component) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "inline_component_value_mismatch"));
            }
            const ComponentLayout* const component = runtime.component(
                type->source->resolved_component_index);
            if (component == nullptr ||
                node.child_count != component->fields.size()) {
                return Result<GraphLogicalFacts>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "component_layout_mismatch"));
            }
            auto validity =
                add_work(facts.nested_work, component->validity_bytes);
            if (!validity.has_value()) {
                return Result<GraphLogicalFacts>::failure(
                    std::move(validity).error());
            }
            pending.push_back(PendingComponent{
                component, node.first_child, 0U, component->validity_bytes});
            continue;
        }

        if (!scalar_tag_matches(kind, node.tag) ||
            node.first_child != invalid_node_index ||
            node.child_count != UINT64_C(0)) {
            return Result<GraphLogicalFacts>::failure(layout_error(
                JsonPointer{}.append("values"), "fixed_value_mismatch"));
        }
    }
    return Result<GraphLogicalFacts>::success(std::move(facts));
}

Result<std::uint64_t> compute_reachability_work(
    const LogicalPayload& values,
    const std::vector<std::vector<NodeIndex>>& entry_values) {
    const auto& object_pools = values.object_pools();
    std::vector<std::vector<std::uint8_t>> reached;
    reached.reserve(object_pools.size());
    for (const auto& pool : object_pools) {
        reached.emplace_back(pool.size(), UINT8_C(0));
    }
    std::vector<ObjectCoordinate> queue;
    if (values.graph_object_count() >
        static_cast<std::uint64_t>(queue.max_size())) {
        return Result<std::uint64_t>::failure(layout_error(
            JsonPointer{}.append("objects"),
            "graph_object_queue_capacity"));
    }
    queue.reserve(static_cast<std::size_t>(values.graph_object_count()));
    std::vector<NodeIndex> stack;
    stack.reserve(values.nodes().size());
    std::vector<NodeIndex> children;
    std::uint64_t work = UINT64_C(0);

    const auto enqueue = [&](ObjectCoordinate coordinate) -> Result<void> {
        if (coordinate.component_index >= reached.size() ||
            coordinate.object_id >=
                reached[coordinate.component_index].size()) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("objects"),
                "reachability_coordinate_out_of_range"));
        }
        std::uint8_t& marker =
            reached[coordinate.component_index]
                   [static_cast<std::size_t>(coordinate.object_id)];
        if (marker == UINT8_C(0)) {
            marker = UINT8_C(1);
            queue.push_back(coordinate);
        }
        return Result<void>::success();
    };

    const auto walk = [&](NodeIndex root) -> Result<void> {
        stack.clear();
        stack.push_back(root);
        while (!stack.empty()) {
            const NodeIndex node_index = stack.back();
            stack.pop_back();
            auto charged = add_work(work, UINT64_C(1));
            if (!charged.has_value()) {
                return charged;
            }
            if (node_index >= values.nodes().size()) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "reachability_node_out_of_range"));
            }
            const ValueNode& node =
                values.nodes()[static_cast<std::size_t>(node_index)];
            if (node.tag == ValueTag::object_root ||
                node.tag == ValueTag::reference) {
                auto queued = enqueue(ObjectCoordinate{
                    node.object_component_index, node.object_id});
                if (!queued.has_value()) {
                    return queued;
                }
                continue;
            }
            if (node.tag != ValueTag::component &&
                node.tag != ValueTag::list &&
                node.tag != ValueTag::object_record) {
                continue;
            }
            children.clear();
            NodeIndex child = node.first_child;
            for (std::uint64_t index = UINT64_C(0);
                 index < node.child_count; ++index) {
                if (child == invalid_node_index ||
                    child >= values.nodes().size()) {
                    return Result<void>::failure(layout_error(
                        JsonPointer{}.append("values"),
                        "reachability_child_out_of_range"));
                }
                children.push_back(child);
                child = values.nodes()[static_cast<std::size_t>(child)]
                            .next_sibling;
            }
            if (child != invalid_node_index) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "reachability_child_chain_too_long"));
            }
            for (auto iterator = children.rbegin();
                 iterator != children.rend(); ++iterator) {
                stack.push_back(*iterator);
            }
        }
        return Result<void>::success();
    };

    for (const auto& entry : entry_values) {
        for (const NodeIndex root : entry) {
            auto walked = walk(root);
            if (!walked.has_value()) {
                return Result<std::uint64_t>::failure(
                    std::move(walked).error());
            }
        }
    }
    std::size_t next = 0U;
    while (next < queue.size()) {
        const ObjectCoordinate coordinate = queue[next++];
        const NodeIndex root =
            object_pools[coordinate.component_index]
                        [static_cast<std::size_t>(coordinate.object_id)];
        auto walked = walk(root);
        if (!walked.has_value()) {
            return Result<std::uint64_t>::failure(
                std::move(walked).error());
        }
    }
    for (std::size_t component = 0U; component < reached.size(); ++component) {
        for (std::size_t object = 0U; object < reached[component].size();
             ++object) {
            auto charged = add_work(work, UINT64_C(1));
            if (!charged.has_value()) {
                return Result<std::uint64_t>::failure(
                    std::move(charged).error());
            }
            if (reached[component][object] == UINT8_C(0)) {
                return Result<std::uint64_t>::failure(layout_error(
                    JsonPointer{}
                        .append("objects")
                        .append(component)
                        .append(object),
                    "unreachable_object"));
            }
        }
    }
    return Result<std::uint64_t>::success(work);
}

}  // namespace

Result<GraphLayout> GraphLayout::plan(
    const RuntimeSchema& runtime_schema,
    const LogicalPayload& values) {
    try {
        if (runtime_schema.spec().profile() != Profile::object_graph_v1 ||
            values.spec().profile() != Profile::object_graph_v1) {
            return Result<GraphLayout>::failure(layout_error(
                JsonPointer{}, "graph_profile_required"));
        }
        if (runtime_schema.spec().digest() != values.spec().digest()) {
            return Result<GraphLayout>::failure(Error::from_details(
                FDB_PAYLOAD_E_DIGEST_MISMATCH, JsonPointer{},
                "Runtime schema and logical payload spec digests differ",
                JsonValue::object({JsonValue::Member{
                    "reason", JsonValue{"layout_spec_digest_mismatch"}}})));
        }
        const auto& source_entries =
            runtime_schema.spec().resolved().entries();
        const auto& source_components =
            runtime_schema.spec().resolved().components();
        if (source_entries.size() != values.entry_roots().size() ||
            source_components.size() != values.object_pools().size()) {
            return Result<GraphLayout>::failure(layout_error(
                JsonPointer{}, "logical_inventory_mismatch"));
        }

        GraphLayout result(runtime_schema);
        result.entries_.reserve(source_entries.size());
        result.entry_values_.reserve(source_entries.size());
        result.object_aggregates_.reserve(
            runtime_schema.identity_components().size());

        auto entry_count = checked_narrow_u32(
            source_entries.size(), JsonPointer{}.append("entries"));
        if (!entry_count.has_value()) {
            return Result<GraphLayout>::failure(
                std::move(entry_count).error());
        }
        for (std::uint32_t entry_index = UINT32_C(0);
             entry_index < entry_count.value(); ++entry_index) {
            const spec::Entry& entry = source_entries[entry_index];
            auto nodes = collect_entry_values(values, entry_index, entry);
            if (!nodes.has_value()) {
                return Result<GraphLayout>::failure(std::move(nodes).error());
            }
            const std::uint64_t value_count = nodes.value().size();
            if (entry.cardinality == Cardinality::one &&
                value_count != UINT64_C(1)) {
                return Result<GraphLayout>::failure(layout_error(
                    JsonPointer{}.append("entries").append(entry.id),
                    "one_entry_value_count"));
            }
            auto roots = checked_add_u64(
                result.root_value_count_, value_count,
                JsonPointer{}.append("entries").append(entry.id));
            if (!roots.has_value()) {
                return Result<GraphLayout>::failure(std::move(roots).error());
            }
            result.root_value_count_ = roots.value();
            const std::uint32_t type_id = runtime_schema.runtime_id(entry.type);
            const RuntimeType* const runtime_type =
                runtime_schema.find_type(type_id);
            if (type_id == UINT32_MAX || runtime_type == nullptr) {
                return Result<GraphLayout>::failure(layout_error(
                    JsonPointer{}.append("entries").append(entry.id),
                    "entry_runtime_type_unassigned"));
            }
            std::uint32_t validity_region = UINT32_MAX;
            if (entry.type.nullable) {
                auto validity_index = checked_narrow_u32(
                    result.regions_.size(),
                    JsonPointer{}.append("entries").append(entry.id));
                if (!validity_index.has_value()) {
                    return Result<GraphLayout>::failure(
                        std::move(validity_index).error());
                }
                auto rounded = checked_add_u64(
                    value_count, UINT64_C(7),
                    JsonPointer{}.append("entries").append(entry.id));
                if (!rounded.has_value()) {
                    return Result<GraphLayout>::failure(
                        std::move(rounded).error());
                }
                validity_region = validity_index.value();
                result.regions_.push_back(RegionDescriptor{
                    RegionKind::entry_validity, UINT32_C(0), entry_index,
                    type_id, UINT64_C(0), rounded.value() / UINT64_C(8),
                    value_count, UINT32_C(0), UINT32_C(1)});
            }
            auto values_index = checked_narrow_u32(
                result.regions_.size(),
                JsonPointer{}.append("entries").append(entry.id));
            if (!values_index.has_value()) {
                return Result<GraphLayout>::failure(
                    std::move(values_index).error());
            }
            auto byte_length = checked_multiply_u64(
                value_count, runtime_type->slot.stride,
                JsonPointer{}.append("entries").append(entry.id));
            if (!byte_length.has_value()) {
                return Result<GraphLayout>::failure(
                    std::move(byte_length).error());
            }
            result.regions_.push_back(RegionDescriptor{
                RegionKind::entry_values, UINT32_C(0), entry_index, type_id,
                UINT64_C(0), byte_length.value(), value_count,
                runtime_type->slot.stride, runtime_type->slot.alignment});
            result.entries_.push_back(EntryDescriptor{
                entry_index, type_id,
                entry.cardinality == Cardinality::one ? UINT32_C(1)
                                                      : UINT32_C(2),
                entry.type.nullable ? UINT32_C(1) : UINT32_C(0), value_count,
                values_index.value(), validity_region});
            result.entry_values_.push_back(std::move(nodes).value());
        }

        std::uint64_t counted_objects = UINT64_C(0);
        for (std::uint32_t component_index = UINT32_C(0);
             component_index < source_components.size(); ++component_index) {
            const auto& pool = values.object_pools()[component_index];
            if (!runtime_schema.component_identity_bearing(component_index)) {
                if (!pool.empty()) {
                    return Result<GraphLayout>::failure(layout_error(
                        JsonPointer{}.append("objects"),
                        "non_identity_object_pool"));
                }
                continue;
            }
            const ComponentLayout* const component =
                runtime_schema.component(component_index);
            if (component == nullptr) {
                return Result<GraphLayout>::failure(layout_error(
                    JsonPointer{}.append("objects"),
                    "object_component_layout_missing"));
            }
            if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
                if (pool.size() >
                    static_cast<std::size_t>(UINT64_MAX)) {
                    return Result<GraphLayout>::failure(Error::from_details(
                        FDB_PAYLOAD_E_LENGTH_OVERFLOW,
                        JsonPointer{}.append("objects"),
                        "Portable graph object count exceeds wire capacity",
                        JsonValue::object({JsonValue::Member{
                            "reason", JsonValue{"object_count_overflow"}}})));
                }
            }
            const std::uint64_t object_count =
                static_cast<std::uint64_t>(pool.size());
            auto sum = checked_add_u64(
                counted_objects, object_count,
                JsonPointer{}.append("objects"));
            if (!sum.has_value()) {
                return Result<GraphLayout>::failure(std::move(sum).error());
            }
            counted_objects = sum.value();
            auto region_index = checked_narrow_u32(
                result.regions_.size(), JsonPointer{}.append("objects"));
            if (!region_index.has_value()) {
                return Result<GraphLayout>::failure(
                    std::move(region_index).error());
            }
            auto byte_length = checked_multiply_u64(
                object_count, component->stride,
                JsonPointer{}.append("objects"));
            if (!byte_length.has_value()) {
                return Result<GraphLayout>::failure(
                    std::move(byte_length).error());
            }
            result.regions_.push_back(RegionDescriptor{
                RegionKind::object_values, UINT32_C(0), component_index,
                UINT32_MAX, UINT64_C(0), byte_length.value(),
                object_count, component->stride,
                component->alignment});
            result.object_aggregates_.push_back(ObjectAggregate{
                component_index, region_index.value(), pool});
        }
        if (counted_objects != values.graph_object_count()) {
            return Result<GraphLayout>::failure(layout_error(
                JsonPointer{}.append("objects"),
                "graph_object_count_mismatch"));
        }
        result.graph_object_count_ = counted_objects;

        auto logical = collect_logical_facts(
            runtime_schema, values, result.entry_values_,
            result.object_aggregates_);
        if (!logical.has_value()) {
            return Result<GraphLayout>::failure(std::move(logical).error());
        }
        const std::uint64_t utf8_bytes = logical.value().utf8_bytes;
        const std::uint64_t utf16_bytes = logical.value().utf16_bytes;
        const std::uint64_t opaque_bytes = logical.value().opaque_bytes;
        const std::uint64_t nested_work = logical.value().nested_work;
        auto reachability_work = compute_reachability_work(
            values, result.entry_values_);
        if (!reachability_work.has_value()) {
            return Result<GraphLayout>::failure(
                std::move(reachability_work).error());
        }
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
                return Result<GraphLayout>::failure(layout_error(
                    JsonPointer{}.append("runtime").append("lists"),
                    "list_item_runtime_type_missing"));
            }
            const std::uint64_t item_count = aggregate.item_nodes.size();
            const JsonPointer path =
                JsonPointer{}
                    .append("lists")
                    .append(aggregate.owner_runtime_type_id);
            if (item->source->nullable) {
                auto validity_index = checked_narrow_u32(
                    result.regions_.size(), path);
                if (!validity_index.has_value()) {
                    return Result<GraphLayout>::failure(
                        std::move(validity_index).error());
                }
                auto rounded =
                    checked_add_u64(item_count, UINT64_C(7), path);
                if (!rounded.has_value()) {
                    return Result<GraphLayout>::failure(
                        std::move(rounded).error());
                }
                aggregate.validity_region_index = validity_index.value();
                result.regions_.push_back(RegionDescriptor{
                    RegionKind::list_validity, UINT32_C(0),
                    aggregate.owner_runtime_type_id,
                    aggregate.item_runtime_type_id, UINT64_C(0),
                    rounded.value() / UINT64_C(8), item_count,
                    UINT32_C(0), UINT32_C(1)});
            }
            auto items_index = checked_narrow_u32(
                result.regions_.size(), path);
            if (!items_index.has_value()) {
                return Result<GraphLayout>::failure(
                    std::move(items_index).error());
            }
            auto byte_length = checked_multiply_u64(
                item_count, item->slot.stride, path);
            if (!byte_length.has_value()) {
                return Result<GraphLayout>::failure(
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

        auto add_pool = [&result](RegionKind kind,
                                  std::uint64_t byte_length,
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
            return Result<GraphLayout>::failure(
                std::move(region_count).error());
        }
        auto region_bytes = checked_multiply_u64(
            region_count.value(), region_descriptor_size,
            JsonPointer{}.append("regions"));
        if (!region_bytes.has_value()) {
            return Result<GraphLayout>::failure(
                std::move(region_bytes).error());
        }
        auto entry_directory_offset = checked_add_u64(
            header_size, region_bytes.value(), JsonPointer{}.append("entries"));
        if (!entry_directory_offset.has_value()) {
            return Result<GraphLayout>::failure(
                std::move(entry_directory_offset).error());
        }
        auto entry_bytes = checked_multiply_u64(
            entry_count.value(), entry_descriptor_size,
            JsonPointer{}.append("entries"));
        if (!entry_bytes.has_value()) {
            return Result<GraphLayout>::failure(
                std::move(entry_bytes).error());
        }
        auto entry_end = checked_add_u64(
            entry_directory_offset.value(), entry_bytes.value(),
            JsonPointer{}.append("entries"));
        if (!entry_end.has_value()) {
            return Result<GraphLayout>::failure(std::move(entry_end).error());
        }
        auto cursor = checked_align_up_u64(
            entry_end.value(), UINT32_C(8),
            JsonPointer{}.append("regions"));
        if (!cursor.has_value()) {
            return Result<GraphLayout>::failure(std::move(cursor).error());
        }
        for (RegionDescriptor& region : result.regions_) {
            auto aligned = checked_align_up_u64(
                cursor.value(), region.alignment,
                JsonPointer{}.append("regions").append(region.owner_index));
            if (!aligned.has_value()) {
                return Result<GraphLayout>::failure(std::move(aligned).error());
            }
            region.data_offset = aligned.value();
            cursor = checked_add_u64(
                region.data_offset, region.byte_length,
                JsonPointer{}.append("regions").append(region.owner_index));
            if (!cursor.has_value()) {
                return Result<GraphLayout>::failure(std::move(cursor).error());
            }
        }
        auto total = checked_align_up_u64(
            cursor.value(), UINT32_C(8),
            JsonPointer{}.append("total_length"));
        if (!total.has_value()) {
            return Result<GraphLayout>::failure(std::move(total).error());
        }
        result.total_length_ = total.value();

        std::uint64_t work = UINT64_C(1);
        auto accumulated = add_work(work, result.regions_.size());
        if (!accumulated.has_value()) {
            return Result<GraphLayout>::failure(std::move(accumulated).error());
        }
        accumulated = add_work(work, result.entries_.size());
        if (!accumulated.has_value()) {
            return Result<GraphLayout>::failure(std::move(accumulated).error());
        }
        accumulated = add_work(work, total.value() - cursor.value());
        if (!accumulated.has_value()) {
            return Result<GraphLayout>::failure(std::move(accumulated).error());
        }
        accumulated = add_work(
            work,
            (result.regions_.empty() ? total.value()
                                     : result.regions_.front().data_offset) -
                entry_end.value());
        if (!accumulated.has_value()) {
            return Result<GraphLayout>::failure(std::move(accumulated).error());
        }
        std::uint64_t previous_end = result.regions_.empty()
                                         ? total.value()
                                         : result.regions_.front().data_offset;
        for (const RegionDescriptor& region : result.regions_) {
            accumulated = add_work(work, region.data_offset - previous_end);
            if (!accumulated.has_value()) {
                return Result<GraphLayout>::failure(
                    std::move(accumulated).error());
            }
            if (region.kind == RegionKind::entry_validity ||
                region.kind == RegionKind::list_validity) {
                accumulated = add_work(work, region.byte_length);
            } else if (region.kind == RegionKind::entry_values ||
                       region.kind == RegionKind::object_values) {
                accumulated = add_work(work, region.element_count);
            }
            if (!accumulated.has_value()) {
                return Result<GraphLayout>::failure(
                    std::move(accumulated).error());
            }
            auto region_end = checked_add_u64(
                region.data_offset, region.byte_length,
                JsonPointer{}.append("validation_work"));
            if (!region_end.has_value()) {
                return Result<GraphLayout>::failure(
                    std::move(region_end).error());
            }
            previous_end = region_end.value();
        }
        accumulated = add_work(work, nested_work);
        if (!accumulated.has_value()) {
            return Result<GraphLayout>::failure(std::move(accumulated).error());
        }
        accumulated = add_work(work, reachability_work.value());
        if (!accumulated.has_value()) {
            return Result<GraphLayout>::failure(std::move(accumulated).error());
        }
        result.validation_work_ = work;
        return Result<GraphLayout>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<GraphLayout>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<GraphLayout>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::layout
