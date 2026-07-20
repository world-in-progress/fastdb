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
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
    case TypeKind::component:
    case TypeKind::ref:
    case TypeKind::list:
        return false;
    }
    return false;
}

bool initial_fixed_kind(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::u8n:
    case TypeKind::u16n:
    case TypeKind::f32:
    case TypeKind::f64:
    case TypeKind::component:
    case TypeKind::ref:
        return true;
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
    case TypeKind::list:
        return false;
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

struct PendingComponent final {
    const ComponentLayout* component;
    NodeIndex next_child;
    std::size_t next_field;
    std::uint64_t cursor;
};

using Pending = std::variant<PendingSlot, PendingComponent>;

Result<void> validate_fixed_occurrences(
    const RuntimeSchema& runtime,
    const LogicalPayload& values,
    const std::vector<NodeIndex>& roots,
    const std::vector<ObjectAggregate>& objects,
    std::uint64_t& nested_work) {
    std::vector<Pending> pending;
    pending.reserve(values.nodes().size());
    for (auto aggregate = objects.rbegin(); aggregate != objects.rend();
         ++aggregate) {
        const ComponentLayout* const component =
            runtime.component(aggregate->component_index);
        if (component == nullptr) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("objects"),
                "object_component_layout_missing"));
        }
        for (std::size_t object_count = aggregate->object_nodes.size();
             object_count != 0U; --object_count) {
            const std::size_t object_id = object_count - 1U;
            const NodeIndex node = aggregate->object_nodes[object_id];
            if (node >= values.nodes().size()) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("objects"),
                    "object_node_out_of_range"));
            }
            const ValueNode& object =
                values.nodes()[static_cast<std::size_t>(node)];
            if (object.tag != ValueTag::object_record ||
                object.runtime_type_id != UINT32_MAX ||
                object.object_component_index != aggregate->component_index ||
                object.object_id != object_id ||
                object.child_count != component->fields.size()) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("objects"),
                    "object_record_mismatch"));
            }
            auto validity = add_work(nested_work, component->validity_bytes);
            if (!validity.has_value()) {
                return validity;
            }
            pending.push_back(PendingComponent{
                component, object.first_child, 0U,
                component->validity_bytes});
        }
    }
    for (auto root = roots.rbegin(); root != roots.rend(); ++root) {
        if (*root >= values.nodes().size()) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("values"), "slot_node_out_of_range"));
        }
        pending.push_back(PendingSlot{
            *root, values.nodes()[static_cast<std::size_t>(*root)]
                       .runtime_type_id});
    }

    while (!pending.empty()) {
        if (auto* component = std::get_if<PendingComponent>(&pending.back())) {
            if (component->next_field == component->component->fields.size()) {
                if (component->next_child != invalid_node_index ||
                    component->cursor > component->component->stride) {
                    return Result<void>::failure(layout_error(
                        JsonPointer{}.append("values"),
                        "component_child_chain_mismatch"));
                }
                auto tail = add_work(
                    nested_work,
                    static_cast<std::uint64_t>(component->component->stride) -
                        component->cursor);
                if (!tail.has_value()) {
                    return tail;
                }
                pending.pop_back();
                continue;
            }
            const ComponentFieldLayout& field =
                component->component->fields[component->next_field++];
            const NodeIndex child = component->next_child;
            if (child == invalid_node_index || child >= values.nodes().size() ||
                field.offset < component->cursor) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "component_child_layout_mismatch"));
            }
            auto padding = add_work(
                nested_work,
                static_cast<std::uint64_t>(field.offset) - component->cursor);
            if (!padding.has_value()) {
                return padding;
            }
            auto field_work = add_work(nested_work, UINT64_C(1));
            if (!field_work.has_value()) {
                return field_work;
            }
            auto field_end = checked_add_u64(
                field.offset, field.slot_stride,
                JsonPointer{}.append("validation_work"));
            if (!field_end.has_value()) {
                return Result<void>::failure(std::move(field_end).error());
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
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("values"), "slot_node_out_of_range"));
        }
        const ValueNode& node =
            values.nodes()[static_cast<std::size_t>(slot.node_index)];
        const RuntimeType* const type = runtime.find_type(slot.runtime_type_id);
        if (type == nullptr || node.runtime_type_id != slot.runtime_type_id ||
            !initial_fixed_kind(type->source->kind)) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("values"),
                "initial_graph_binary_slice_unavailable"));
        }
        if (node.tag == ValueTag::null_value) {
            if (!type->source->nullable ||
                node.first_child != invalid_node_index ||
                node.child_count != UINT64_C(0)) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("values"), "invalid_null_value_node"));
            }
            if (type->storage_role == StorageRole::inline_component) {
                auto zero_scan = add_work(nested_work, type->slot.stride);
                if (!zero_scan.has_value()) {
                    return zero_scan;
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
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "graph_coordinate_mismatch"));
            }
            continue;
        }

        if (type->storage_role == StorageRole::inline_component) {
            if (node.tag != ValueTag::component) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "inline_component_value_mismatch"));
            }
            const ComponentLayout* const component = runtime.component(
                type->source->resolved_component_index);
            if (component == nullptr ||
                node.child_count != component->fields.size()) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("values"),
                    "component_layout_mismatch"));
            }
            auto validity = add_work(nested_work, component->validity_bytes);
            if (!validity.has_value()) {
                return validity;
            }
            pending.push_back(PendingComponent{
                component, node.first_child, 0U, component->validity_bytes});
            continue;
        }

        if (!scalar_tag_matches(type->source->kind, node.tag) ||
            node.first_child != invalid_node_index ||
            node.child_count != UINT64_C(0)) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("values"), "fixed_value_mismatch"));
        }
    }
    return Result<void>::success();
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
        if (!runtime_schema.list_nodes().empty() ||
            runtime_schema.has_utf8_pool() ||
            runtime_schema.has_utf16_pool() ||
            runtime_schema.has_bytes_pool()) {
            return Result<GraphLayout>::failure(layout_error(
                JsonPointer{}, "initial_graph_binary_slice_unavailable"));
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
            if (type_id == UINT32_MAX || runtime_type == nullptr ||
                !initial_fixed_kind(entry.type.kind)) {
                return Result<GraphLayout>::failure(layout_error(
                    JsonPointer{}.append("entries").append(entry.id),
                    "initial_graph_binary_slice_unavailable"));
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

        std::vector<NodeIndex> all_entries;
        for (const auto& entry : result.entry_values_) {
            all_entries.insert(all_entries.end(), entry.begin(), entry.end());
        }
        std::uint64_t nested_work = UINT64_C(0);
        auto logical = validate_fixed_occurrences(
            runtime_schema, values, all_entries, result.object_aggregates_,
            nested_work);
        if (!logical.has_value()) {
            return Result<GraphLayout>::failure(std::move(logical).error());
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
            if (region.kind == RegionKind::entry_validity) {
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
        result.validation_work_ = work;
        return Result<GraphLayout>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<GraphLayout>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<GraphLayout>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::layout
