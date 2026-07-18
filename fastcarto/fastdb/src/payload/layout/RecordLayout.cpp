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
#include <utility>

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

Error unavailable(const spec::Entry& entry, const char* reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE,
        JsonPointer{}.append("entries").append(entry.id),
        "Portable record layout does not implement this runtime type yet",
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

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

bool task4_record_kind(TypeKind kind) noexcept {
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
        return true;
    case TypeKind::ref:
    case TypeKind::list:
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

Result<void> collect_variable_values(
    const RuntimeSchema& runtime, const LogicalPayload& values, NodeIndex root,
    std::vector<NodeIndex>& output, std::uint64_t& utf8_bytes,
    std::uint64_t& utf16_bytes, std::uint64_t& opaque_bytes) {
    std::vector<NodeIndex> pending;
    pending.push_back(root);
    const std::uint64_t storage_size = values.byte_storage().size();
    while (!pending.empty()) {
        const NodeIndex node_index = pending.back();
        pending.pop_back();
        if (node_index == invalid_node_index ||
            node_index >= values.nodes().size()) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("pools"), "pool_node_out_of_range"));
        }
        const ValueNode& node =
            values.nodes()[static_cast<std::size_t>(node_index)];
        const RuntimeType* const runtime_type =
            runtime.find_type(node.runtime_type_id);
        if (runtime_type == nullptr) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("pools"), "pool_runtime_type_missing"));
        }
        const TypeKind kind = runtime_type->source->kind;
        if (kind == TypeKind::str || kind == TypeKind::wstr ||
            kind == TypeKind::bytes) {
            output.push_back(node_index);
            if (node.tag == ValueTag::null_value) {
                continue;
            }
            if (node.tag != variable_tag(kind)) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("pools"), "pool_value_tag_mismatch"));
            }
            auto storage_end =
                checked_range_end(node.scalar_bits_or_offset, node.byte_length,
                                  storage_size, JsonPointer{}.append("pools"));
            if (!storage_end.has_value()) {
                return Result<void>::failure(std::move(storage_end).error());
            }
            if (kind == TypeKind::wstr &&
                node.byte_length % UINT64_C(2) != UINT64_C(0)) {
                return Result<void>::failure(
                    layout_error(JsonPointer{}.append("pools"),
                                 "wide_text_storage_length_is_odd"));
            }
            std::uint64_t* total = kind == TypeKind::str    ? &utf8_bytes
                                   : kind == TypeKind::wstr ? &utf16_bytes
                                                            : &opaque_bytes;
            auto added = checked_accumulate_u64(*total, node.byte_length,
                                                JsonPointer{}.append("pools"));
            if (!added.has_value()) {
                return added;
            }
            continue;
        }
        if (kind != TypeKind::component || node.tag == ValueTag::null_value) {
            continue;
        }
        if (node.tag != ValueTag::component) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("pools"), "component_value_mismatch"));
        }
        const ComponentLayout* const component =
            runtime.component(runtime_type->source->resolved_component_index);
        if (component == nullptr ||
            node.child_count != component->fields.size()) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("pools"), "component_layout_mismatch"));
        }
        std::vector<NodeIndex> children;
        children.reserve(component->fields.size());
        NodeIndex child = node.first_child;
        for (std::size_t index = 0U; index < component->fields.size();
             ++index) {
            if (child == invalid_node_index || child >= values.nodes().size()) {
                return Result<void>::failure(
                    layout_error(JsonPointer{}.append("pools"),
                                 "component_child_out_of_range"));
            }
            children.push_back(child);
            child =
                values.nodes()[static_cast<std::size_t>(child)].next_sibling;
        }
        if (child != invalid_node_index) {
            return Result<void>::failure(
                layout_error(JsonPointer{}.append("pools"),
                             "component_child_chain_too_long"));
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            pending.push_back(*it);
        }
    }
    return Result<void>::success();
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

Result<void> add_slot_validation_work(const RuntimeSchema& runtime,
                                      const LogicalPayload& values,
                                      NodeIndex root,
                                      std::uint64_t& work) {
    std::vector<NodeIndex> pending;
    pending.push_back(root);
    while (!pending.empty()) {
        const NodeIndex node_index = pending.back();
        pending.pop_back();
        if (node_index == invalid_node_index ||
            node_index >= values.nodes().size()) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("validation_work"),
                "slot_node_out_of_range"));
        }
        const ValueNode& node =
            values.nodes()[static_cast<std::size_t>(node_index)];
        const RuntimeType* const runtime_type =
            runtime.find_type(node.runtime_type_id);
        if (runtime_type == nullptr) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("validation_work"),
                "slot_runtime_type_missing"));
        }
        if (node.tag == ValueTag::null_value) {
            if (runtime_type->source->kind == TypeKind::component) {
                auto added = checked_accumulate_u64(
                    work, runtime_type->slot.stride,
                    JsonPointer{}.append("validation_work"));
                if (!added.has_value()) {
                    return added;
                }
            }
            continue;
        }
        if (runtime_type->source->kind != TypeKind::component) {
            if (node.tag != ValueTag::null_value &&
                (runtime_type->source->kind == TypeKind::str ||
                 runtime_type->source->kind == TypeKind::wstr)) {
                const std::uint64_t units =
                    runtime_type->source->kind == TypeKind::str
                        ? node.byte_length
                        : node.byte_length / UINT64_C(2);
                auto added = checked_accumulate_u64(
                    work, units, JsonPointer{}.append("validation_work"));
                if (!added.has_value()) {
                    return added;
                }
            }
            continue;
        }
        if (node.tag != ValueTag::component) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("validation_work"),
                "component_value_mismatch"));
        }
        const ComponentLayout* const component = runtime.component(
            runtime_type->source->resolved_component_index);
        if (component == nullptr ||
            node.child_count != component->fields.size()) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("validation_work"),
                "component_layout_mismatch"));
        }
        auto added = checked_accumulate_u64(
            work, component->validity_bytes,
            JsonPointer{}.append("validation_work"));
        if (!added.has_value()) {
            return added;
        }
        std::uint64_t cursor = component->validity_bytes;
        NodeIndex child = node.first_child;
        for (const ComponentFieldLayout& field : component->fields) {
            if (field.offset < cursor || child == invalid_node_index ||
                child >= values.nodes().size()) {
                return Result<void>::failure(layout_error(
                    JsonPointer{}.append("validation_work"),
                    "component_child_layout_mismatch"));
            }
            added = checked_accumulate_u64(
                work, static_cast<std::uint64_t>(field.offset) - cursor,
                JsonPointer{}.append("validation_work"));
            if (!added.has_value()) {
                return added;
            }
            added = checked_accumulate_u64(
                work, UINT64_C(1),
                JsonPointer{}.append("validation_work"));
            if (!added.has_value()) {
                return added;
            }
            auto field_end = checked_add_u64(
                field.offset, field.slot_stride,
                JsonPointer{}.append("validation_work"));
            if (!field_end.has_value()) {
                return Result<void>::failure(std::move(field_end).error());
            }
            cursor = field_end.value();
            pending.push_back(child);
            child = values.nodes()[static_cast<std::size_t>(child)].next_sibling;
        }
        if (child != invalid_node_index || cursor > component->stride) {
            return Result<void>::failure(layout_error(
                JsonPointer{}.append("validation_work"),
                "component_child_chain_mismatch"));
        }
        added = checked_accumulate_u64(
            work, static_cast<std::uint64_t>(component->stride) - cursor,
            JsonPointer{}.append("validation_work"));
        if (!added.has_value()) {
            return added;
        }
    }
    return Result<void>::success();
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
        std::uint64_t utf8_bytes = UINT64_C(0);
        std::uint64_t utf16_bytes = UINT64_C(0);
        std::uint64_t opaque_bytes = UINT64_C(0);

        for (std::uint32_t entry_index = UINT32_C(0);
             entry_index < entry_count.value(); ++entry_index) {
            const spec::Entry& entry = source_entries[entry_index];
            if (!task4_record_kind(entry.type.kind)) {
                return Result<RecordLayout>::failure(
                    unavailable(entry, "initial_record_layout_type_unavailable"));
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
            for (const NodeIndex node : nodes.value()) {
                auto collected = collect_variable_values(
                    runtime_schema, values, node, result.variable_values_,
                    utf8_bytes, utf16_bytes, opaque_bytes);
                if (!collected.has_value()) {
                    return Result<RecordLayout>::failure(
                        std::move(collected).error());
                }
            }
            result.entry_values_.push_back(std::move(nodes).value());
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
            if (region.kind == RegionKind::entry_validity) {
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
        for (const auto& entry_nodes : result.entry_values_) {
            for (const NodeIndex node : entry_nodes) {
                work_result = add_slot_validation_work(
                    runtime_schema, values, node, work);
                if (!work_result.has_value()) {
                    return Result<RecordLayout>::failure(
                        std::move(work_result).error());
                }
            }
        }
        result.validation_work_ = work;
        return Result<RecordLayout>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<RecordLayout>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::layout
