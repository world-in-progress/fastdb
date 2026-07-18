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

bool initial_fixed_kind(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::f32:
    case TypeKind::f64:
        return true;
    case TypeKind::u8n:
    case TypeKind::u16n:
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

        for (std::uint32_t entry_index = UINT32_C(0);
             entry_index < entry_count.value(); ++entry_index) {
            const spec::Entry& entry = source_entries[entry_index];
            if (entry.type.kind == TypeKind::u8n ||
                entry.type.kind == TypeKind::u16n) {
                return Result<RecordLayout>::failure(unavailable(
                    entry, "normalized_wire_quantization_unavailable"));
            }
            if (!initial_fixed_kind(entry.type.kind)) {
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
            result.entry_values_.push_back(std::move(nodes).value());
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
            } else {
                work_result = add_work(region.element_count);
            }
            if (!work_result.has_value()) {
                return Result<RecordLayout>::failure(
                    std::move(work_result).error());
            }
            previous_end = region.data_offset + region.byte_length;
        }
        result.validation_work_ = work;
        return Result<RecordLayout>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<RecordLayout>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::layout
