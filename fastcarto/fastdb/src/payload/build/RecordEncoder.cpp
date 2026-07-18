#include "payload/build/RecordEncoder.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/NormalizedInteger.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace fastdb::payload::build {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;
using layout::EntryDescriptor;
using layout::RecordLayout;
using layout::RegionDescriptor;
using layout::RegionKind;
using spec::TypeKind;

Error encode_error(const JsonPointer& path, const char* reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_INTERNAL, path,
        "Logical payload cannot be encoded with this record layout",
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

Error allocation_error() {
    return Error::from_details(
        FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
        "Portable record encoding allocation failed",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"allocation_failed"}}}));
}

class AscendingWriter final {
public:
    explicit AscendingWriter(ByteSink& sink) : sink_(sink) {}

    Result<void> write(const std::uint8_t* data, std::uint64_t size) {
        auto next = layout::checked_add_u64(
            offset_, size, JsonPointer{}.append("sink"));
        if (!next.has_value()) {
            return Result<void>::failure(std::move(next).error());
        }
        auto written = sink_.write(offset_, data, size);
        if (!written.has_value()) {
            return written;
        }
        offset_ = next.value();
        return Result<void>::success();
    }

    Result<void> zero_until(std::uint64_t target) {
        if (target < offset_) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("sink"), "non_ascending_write"));
        }
        constexpr std::array<std::uint8_t, 256> zeros{};
        while (offset_ < target) {
            const std::uint64_t remaining = target - offset_;
            const std::uint64_t chunk = std::min<std::uint64_t>(
                remaining, static_cast<std::uint64_t>(zeros.size()));
            auto written = write(zeros.data(), chunk);
            if (!written.has_value()) {
                return written;
            }
        }
        return Result<void>::success();
    }

    std::uint64_t offset() const noexcept { return offset_; }

private:
    ByteSink& sink_;
    std::uint64_t offset_{UINT64_C(0)};
};

template <std::size_t Size>
Result<void> put_u16(std::array<std::uint8_t, Size>& bytes,
                     std::uint64_t offset,
                     std::uint16_t value) {
    return layout::store_u16_le(bytes.data(), bytes.size(), offset, value,
                                JsonPointer{}.append("encoder"));
}

template <std::size_t Size>
Result<void> put_u32(std::array<std::uint8_t, Size>& bytes,
                     std::uint64_t offset,
                     std::uint32_t value) {
    return layout::store_u32_le(bytes.data(), bytes.size(), offset, value,
                                JsonPointer{}.append("encoder"));
}

template <std::size_t Size>
Result<void> put_u64(std::array<std::uint8_t, Size>& bytes,
                     std::uint64_t offset,
                     std::uint64_t value) {
    return layout::store_u64_le(bytes.data(), bytes.size(), offset, value,
                                JsonPointer{}.append("encoder"));
}

std::uint32_t canonical_f32(std::uint32_t bits) noexcept {
    const bool nan = (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) &&
                     (bits & UINT32_C(0x007fffff)) != UINT32_C(0);
    return nan ? UINT32_C(0x7fc00000) : bits;
}

std::uint64_t canonical_f64(std::uint64_t bits) noexcept {
    const bool nan =
        (bits & UINT64_C(0x7ff0000000000000)) ==
            UINT64_C(0x7ff0000000000000) &&
        (bits & UINT64_C(0x000fffffffffffff)) != UINT64_C(0);
    return nan ? UINT64_C(0x7ff8000000000000) : bits;
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
    case TypeKind::f32:
        return tag == ValueTag::f32;
    case TypeKind::f64:
        return tag == ValueTag::f64;
    case TypeKind::u8n:
        return tag == ValueTag::u8n;
    case TypeKind::u16n:
        return tag == ValueTag::u16n;
    case TypeKind::str:
        return tag == ValueTag::str;
    case TypeKind::wstr:
        return tag == ValueTag::wstr;
    case TypeKind::bytes:
        return tag == ValueTag::bytes;
    case TypeKind::component:
        return tag == ValueTag::component;
    default:
        return false;
    }
}

struct PoolCursors final {
    std::uint64_t utf8{UINT64_C(0)};
    std::uint64_t utf16{UINT64_C(0)};
    std::uint64_t opaque{UINT64_C(0)};

    std::uint64_t& for_kind(TypeKind kind) noexcept {
        if (kind == TypeKind::str) {
            return utf8;
        }
        if (kind == TypeKind::wstr) {
            return utf16;
        }
        return opaque;
    }
};

Result<void> encode_scalar_slot(const RecordLayout& layout,
                                const LogicalPayload& values,
                                const ValueNode& node, AscendingWriter& writer,
                                PoolCursors& pool_cursors) {
    const layout::RuntimeType* const runtime =
        layout.runtime_schema().find_type(node.runtime_type_id);
    if (node.runtime_type_id == UINT32_MAX || runtime == nullptr) {
        return Result<void>::failure(encode_error(
            JsonPointer{}.append("values"), "runtime_type_id_out_of_range"));
    }
    std::array<std::uint8_t, 16> bytes{};
    if (node.tag == ValueTag::null_value) {
        return writer.write(bytes.data(), runtime->slot.stride);
    }
    if (!tag_matches(runtime->source->kind, node.tag)) {
        return Result<void>::failure(encode_error(
            JsonPointer{}.append("values"), "value_tag_mismatch"));
    }
    switch (runtime->source->kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
        bytes[0] = static_cast<std::uint8_t>(node.scalar_bits_or_offset);
        break;
    case TypeKind::u16:
        if (!put_u16(bytes, UINT64_C(0),
                     static_cast<std::uint16_t>(
                         node.scalar_bits_or_offset))
                 .has_value()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("values"), "slot_store_failed"));
        }
        break;
    case TypeKind::u32:
    case TypeKind::i32:
        if (!put_u32(bytes, UINT64_C(0),
                     static_cast<std::uint32_t>(
                         node.scalar_bits_or_offset))
                 .has_value()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("values"), "slot_store_failed"));
        }
        break;
    case TypeKind::f32:
        if (!put_u32(bytes, UINT64_C(0),
                     canonical_f32(static_cast<std::uint32_t>(
                         node.scalar_bits_or_offset)))
                 .has_value()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("values"), "slot_store_failed"));
        }
        break;
    case TypeKind::f64:
        if (!put_u64(bytes, UINT64_C(0),
                     canonical_f64(node.scalar_bits_or_offset))
                 .has_value()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("values"), "slot_store_failed"));
        }
        break;
    case TypeKind::u8n: {
        auto code = layout::quantize_normalized(
            node.scalar_bits_or_offset, runtime->source->minimum,
            runtime->source->maximum, UINT32_C(255), "u8n",
            JsonPointer{}.append("values"));
        if (!code.has_value()) {
            return Result<void>::failure(std::move(code).error());
        }
        bytes[0] = static_cast<std::uint8_t>(code.value());
        break;
    }
    case TypeKind::u16n: {
        auto code = layout::quantize_normalized(
            node.scalar_bits_or_offset, runtime->source->minimum,
            runtime->source->maximum, UINT32_C(65535), "u16n",
            JsonPointer{}.append("values"));
        if (!code.has_value()) {
            return Result<void>::failure(std::move(code).error());
        }
        if (!put_u16(bytes, UINT64_C(0),
                     static_cast<std::uint16_t>(code.value()))
                 .has_value()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("values"), "slot_store_failed"));
        }
        break;
    }
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes: {
        if (runtime->source->kind == TypeKind::wstr &&
            node.byte_length % UINT64_C(2) != UINT64_C(0)) {
            return Result<void>::failure(
                encode_error(JsonPointer{}.append("values"),
                             "wide_text_storage_length_is_odd"));
        }
        auto storage_end = layout::checked_range_end(
            node.scalar_bits_or_offset, node.byte_length,
            values.byte_storage().size(), JsonPointer{}.append("values"));
        if (!storage_end.has_value()) {
            return Result<void>::failure(std::move(storage_end).error());
        }
        std::uint64_t& cursor = pool_cursors.for_kind(runtime->source->kind);
        if (!put_u64(bytes, UINT64_C(0), cursor).has_value() ||
            !put_u64(bytes, UINT64_C(8), node.byte_length).has_value()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("values"), "descriptor_store_failed"));
        }
        auto advanced = layout::checked_add_u64(cursor, node.byte_length,
                                                JsonPointer{}.append("pools"));
        if (!advanced.has_value()) {
            return Result<void>::failure(std::move(advanced).error());
        }
        cursor = advanced.value();
        break;
    }
    default:
        return Result<void>::failure(encode_error(
            JsonPointer{}.append("values"), "unsupported_initial_slot"));
    }
    return writer.write(bytes.data(), runtime->slot.stride);
}

struct ComponentFrame final {
    const layout::RuntimeType* runtime;
    const layout::ComponentLayout* component;
    NodeIndex next_child;
    std::size_t next_field;
    std::uint64_t base_offset;
};

Result<ComponentFrame> begin_component_frame(
    const RecordLayout& layout,
    const LogicalPayload& values,
    const ValueNode& node,
    AscendingWriter& writer) {
    const layout::RuntimeType* runtime =
        layout.runtime_schema().find_type(node.runtime_type_id);
    if (runtime == nullptr || runtime->source->kind != TypeKind::component ||
        node.tag != ValueTag::component) {
        return Result<ComponentFrame>::failure(encode_error(
            JsonPointer{}.append("values"), "component_value_mismatch"));
    }
    const layout::ComponentLayout* component =
        layout.runtime_schema().component(
            runtime->source->resolved_component_index);
    if (component == nullptr ||
        node.child_count != component->fields.size()) {
        return Result<ComponentFrame>::failure(encode_error(
            JsonPointer{}.append("values"), "component_child_count"));
    }
    const std::uint64_t base = writer.offset();
    NodeIndex child = node.first_child;
    std::uint8_t validity = UINT8_C(0);
    std::uint32_t nullable_index = UINT32_C(0);
    std::uint32_t nullable_count = UINT32_C(0);
    for (const layout::ComponentFieldLayout& field : component->fields) {
        if (field.validity_bit != UINT32_MAX) {
            ++nullable_count;
        }
    }
    for (std::size_t field_index = 0U;
         field_index < component->fields.size(); ++field_index) {
        if (child == invalid_node_index || child >= values.nodes().size()) {
            return Result<ComponentFrame>::failure(encode_error(
                JsonPointer{}.append("values"),
                "component_child_out_of_range"));
        }
        const ValueNode& child_node =
            values.nodes()[static_cast<std::size_t>(child)];
        const layout::ComponentFieldLayout& field =
            component->fields[field_index];
        if (child_node.runtime_type_id != field.runtime_type_id) {
            return Result<ComponentFrame>::failure(encode_error(
                JsonPointer{}.append("values"),
                "component_child_type_mismatch"));
        }
        if (field.validity_bit != UINT32_MAX) {
            if (child_node.tag != ValueTag::null_value) {
                validity = static_cast<std::uint8_t>(
                    validity |
                    (UINT8_C(1) << (nullable_index % UINT32_C(8))));
            }
            ++nullable_index;
            if (nullable_index % UINT32_C(8) == UINT32_C(0) ||
                nullable_index == nullable_count) {
                auto written = writer.write(&validity, UINT64_C(1));
                if (!written.has_value()) {
                    return Result<ComponentFrame>::failure(
                        std::move(written).error());
                }
                validity = UINT8_C(0);
            }
        }
        child = child_node.next_sibling;
    }
    auto validity_end = layout::checked_add_u64(
        base, component->validity_bytes,
        JsonPointer{}.append("values"));
    if (!validity_end.has_value()) {
        return Result<ComponentFrame>::failure(
            std::move(validity_end).error());
    }
    if (child != invalid_node_index ||
        writer.offset() != validity_end.value()) {
        return Result<ComponentFrame>::failure(encode_error(
            JsonPointer{}.append("values"), "component_child_chain"));
    }
    return Result<ComponentFrame>::success(ComponentFrame{
        runtime, component, node.first_child, 0U, base});
}

Result<void> encode_value_slot(const RecordLayout& layout,
                               const LogicalPayload& values,
                               NodeIndex node_index, AscendingWriter& writer,
                               PoolCursors& pool_cursors) {
    try {
        if (node_index >= values.nodes().size()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("values"), "node_index_out_of_range"));
        }
        const ValueNode* node =
            &values.nodes()[static_cast<std::size_t>(node_index)];
        const layout::RuntimeType* runtime =
            layout.runtime_schema().find_type(node->runtime_type_id);
        if (runtime == nullptr) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("values"),
                "runtime_type_id_out_of_range"));
        }
        if (node->tag == ValueTag::null_value) {
            auto slot_end = layout::checked_add_u64(
                writer.offset(), runtime->slot.stride,
                JsonPointer{}.append("values"));
            if (!slot_end.has_value()) {
                return Result<void>::failure(std::move(slot_end).error());
            }
            return writer.zero_until(slot_end.value());
        }
        if (runtime->source->kind != TypeKind::component) {
            return encode_scalar_slot(layout, values, *node, writer,
                                      pool_cursors);
        }

        auto root = begin_component_frame(layout, values, *node, writer);
        if (!root.has_value()) {
            return Result<void>::failure(std::move(root).error());
        }
        std::vector<ComponentFrame> frames;
        frames.push_back(std::move(root).value());
        while (!frames.empty()) {
            ComponentFrame& frame = frames.back();
            if (frame.next_field == frame.component->fields.size()) {
                if (frame.next_child != invalid_node_index) {
                    return Result<void>::failure(encode_error(
                        JsonPointer{}.append("values"),
                        "component_child_chain_too_long"));
                }
                auto frame_end = layout::checked_add_u64(
                    frame.base_offset, frame.component->stride,
                    JsonPointer{}.append("values"));
                if (!frame_end.has_value()) {
                    return Result<void>::failure(
                        std::move(frame_end).error());
                }
                auto padded = writer.zero_until(frame_end.value());
                if (!padded.has_value()) {
                    return padded;
                }
                frames.pop_back();
                continue;
            }
            if (frame.next_child == invalid_node_index ||
                frame.next_child >= values.nodes().size()) {
                return Result<void>::failure(encode_error(
                    JsonPointer{}.append("values"),
                    "component_child_out_of_range"));
            }
            const layout::ComponentFieldLayout& field =
                frame.component->fields[frame.next_field];
            const ValueNode& child = values.nodes()[static_cast<std::size_t>(
                frame.next_child)];
            auto field_offset = layout::checked_add_u64(
                frame.base_offset, field.offset,
                JsonPointer{}.append("values"));
            if (!field_offset.has_value()) {
                return Result<void>::failure(
                    std::move(field_offset).error());
            }
            auto aligned = writer.zero_until(field_offset.value());
            if (!aligned.has_value()) {
                return aligned;
            }
            ++frame.next_field;
            frame.next_child = child.next_sibling;
            if (child.tag == ValueTag::null_value) {
                auto field_end = layout::checked_add_u64(
                    writer.offset(), field.slot_stride,
                    JsonPointer{}.append("values"));
                if (!field_end.has_value()) {
                    return Result<void>::failure(
                        std::move(field_end).error());
                }
                auto zero = writer.zero_until(field_end.value());
                if (!zero.has_value()) {
                    return zero;
                }
                continue;
            }
            const layout::RuntimeType* child_runtime =
                layout.runtime_schema().find_type(child.runtime_type_id);
            if (child_runtime == nullptr ||
                child.runtime_type_id != field.runtime_type_id) {
                return Result<void>::failure(encode_error(
                    JsonPointer{}.append("values"),
                    "component_child_type_mismatch"));
            }
            if (child_runtime->source->kind != TypeKind::component) {
                auto encoded = encode_scalar_slot(layout, values, child, writer,
                                                  pool_cursors);
                if (!encoded.has_value()) {
                    return encoded;
                }
                continue;
            }
            auto nested =
                begin_component_frame(layout, values, child, writer);
            if (!nested.has_value()) {
                return Result<void>::failure(std::move(nested).error());
            }
            frames.push_back(std::move(nested).value());
        }
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

}  // namespace

Result<void> encode_record(const RecordLayout& layout,
                           const LogicalPayload& values,
                           ByteSink& sink) try {
    if (layout.runtime_schema().spec().digest() != values.spec().digest()) {
        return Result<void>::failure(encode_error(
            JsonPointer{}, "encode_spec_digest_mismatch"));
    }
    AscendingWriter writer(sink);
    PoolCursors pool_cursors;
    auto region_bytes = layout::checked_multiply_u64(
        layout.region_count(), layout::region_descriptor_size,
        JsonPointer{}.append("header").append("region_directory"));
    if (!region_bytes.has_value()) {
        return Result<void>::failure(std::move(region_bytes).error());
    }
    auto entry_directory_offset = layout::checked_add_u64(
        layout::header_size, region_bytes.value(),
        JsonPointer{}.append("header").append("entry_directory"));
    if (!entry_directory_offset.has_value()) {
        return Result<void>::failure(
            std::move(entry_directory_offset).error());
    }
    std::array<std::uint8_t, layout::header_size> header{};
    std::copy(layout::binary_magic.begin(), layout::binary_magic.end(),
              header.begin());
    if (!put_u16(header, layout::header_major_offset, layout::binary_major)
             .has_value() ||
        !put_u16(header, layout::header_minor_offset, layout::binary_minor)
             .has_value() ||
        !put_u32(header, layout::header_size_offset, layout::header_size)
             .has_value() ||
        !put_u32(header, layout::header_profile_offset,
                 FDB_PAYLOAD_PROFILE_RECORD_V1)
             .has_value() ||
        !put_u32(header, layout::header_flags_offset, UINT32_C(0))
             .has_value() ||
        !put_u64(header, layout::header_total_length_offset,
                 layout.total_length())
             .has_value() ||
        !put_u64(header, layout::header_region_directory_offset,
                 layout::header_size)
             .has_value() ||
        !put_u32(header, layout::header_region_count_offset,
                 layout.region_count())
             .has_value() ||
        !put_u32(header, layout::header_region_descriptor_size_offset,
                 layout::region_descriptor_size)
             .has_value() ||
        !put_u64(header, layout::header_entry_directory_offset,
                 entry_directory_offset.value())
             .has_value() ||
        !put_u32(header, layout::header_entry_count_offset,
                 layout.entry_count())
             .has_value() ||
        !put_u32(header, layout::header_entry_descriptor_size_offset,
                 layout::entry_descriptor_size)
             .has_value() ||
        !put_u64(header, layout::header_root_value_count_offset,
                 layout.root_value_count())
             .has_value()) {
        return Result<void>::failure(encode_error(
            JsonPointer{}.append("header"), "header_store_failed"));
    }
    std::copy(layout.runtime_schema().spec().digest().begin(),
              layout.runtime_schema().spec().digest().end(),
              header.begin() + static_cast<std::ptrdiff_t>(
                                   layout::header_spec_digest_offset));
    auto written = writer.write(header.data(), header.size());
    if (!written.has_value()) {
        return written;
    }

    for (const RegionDescriptor& region : layout.regions()) {
        std::array<std::uint8_t, layout::region_descriptor_size> bytes{};
        if (!put_u32(bytes, layout::region_kind_offset,
                     static_cast<std::uint32_t>(region.kind))
                 .has_value() ||
            !put_u32(bytes, layout::region_flags_offset, region.flags)
                 .has_value() ||
            !put_u32(bytes, layout::region_owner_index_offset,
                     region.owner_index)
                 .has_value() ||
            !put_u32(bytes, layout::region_runtime_type_id_offset,
                     region.runtime_type_id)
                 .has_value() ||
            !put_u64(bytes, layout::region_data_offset_offset,
                     region.data_offset)
                 .has_value() ||
            !put_u64(bytes, layout::region_byte_length_offset,
                     region.byte_length)
                 .has_value() ||
            !put_u64(bytes, layout::region_element_count_offset,
                     region.element_count)
                 .has_value() ||
            !put_u32(bytes, layout::region_stride_offset, region.stride)
                 .has_value() ||
            !put_u32(bytes, layout::region_alignment_offset,
                     region.alignment)
                 .has_value()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("regions"),
                "region_descriptor_store_failed"));
        }
        written = writer.write(bytes.data(), bytes.size());
        if (!written.has_value()) {
            return written;
        }
    }
    for (const EntryDescriptor& entry : layout.entries()) {
        std::array<std::uint8_t, layout::entry_descriptor_size> bytes{};
        if (!put_u32(bytes, layout::entry_index_offset, entry.entry_index)
                 .has_value() ||
            !put_u32(bytes, layout::entry_runtime_type_id_offset,
                     entry.runtime_type_id)
                 .has_value() ||
            !put_u32(bytes, layout::entry_cardinality_offset,
                     entry.cardinality)
                 .has_value() ||
            !put_u32(bytes, layout::entry_flags_offset, entry.flags)
                 .has_value() ||
            !put_u64(bytes, layout::entry_value_count_offset,
                     entry.value_count)
                 .has_value() ||
            !put_u32(bytes, layout::entry_values_region_index_offset,
                     entry.values_region_index)
                 .has_value() ||
            !put_u32(bytes, layout::entry_validity_region_index_offset,
                     entry.validity_region_index)
                 .has_value()) {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("entries"),
                "entry_descriptor_store_failed"));
        }
        written = writer.write(bytes.data(), bytes.size());
        if (!written.has_value()) {
            return written;
        }
    }

    for (const RegionDescriptor& region : layout.regions()) {
        auto padded = writer.zero_until(region.data_offset);
        if (!padded.has_value()) {
            return padded;
        }
        if (region.kind == RegionKind::entry_validity ||
            region.kind == RegionKind::entry_values) {
            if (region.owner_index >= layout.entry_values().size()) {
                return Result<void>::failure(
                    encode_error(JsonPointer{}.append("regions"),
                                 "entry_region_owner_out_of_range"));
            }
        }
        if (region.kind == RegionKind::entry_validity) {
            const auto& nodes = layout.entry_values()[region.owner_index];
            std::uint8_t byte = UINT8_C(0);
            for (std::uint64_t index = UINT64_C(0);
                 index < region.element_count; ++index) {
                const ValueNode& node = values.nodes()[static_cast<std::size_t>(
                    nodes[static_cast<std::size_t>(index)])];
                if (node.tag != ValueTag::null_value) {
                    byte = static_cast<std::uint8_t>(
                        byte | (UINT8_C(1) << (index % UINT64_C(8))));
                }
                if (index % UINT64_C(8) == UINT64_C(7) ||
                    index + UINT64_C(1) == region.element_count) {
                    written = writer.write(&byte, UINT64_C(1));
                    if (!written.has_value()) {
                        return written;
                    }
                    byte = UINT8_C(0);
                }
            }
        } else if (region.kind == RegionKind::entry_values) {
            const auto& nodes = layout.entry_values()[region.owner_index];
            for (const NodeIndex node_index : nodes) {
                written = encode_value_slot(layout, values, node_index, writer,
                                            pool_cursors);
                if (!written.has_value()) {
                    return written;
                }
            }
        } else if (region.kind == RegionKind::utf8_pool ||
                   region.kind == RegionKind::utf16_pool ||
                   region.kind == RegionKind::bytes_pool) {
            const TypeKind wanted =
                region.kind == RegionKind::utf8_pool    ? TypeKind::str
                : region.kind == RegionKind::utf16_pool ? TypeKind::wstr
                                                        : TypeKind::bytes;
            if (pool_cursors.for_kind(wanted) != region.byte_length) {
                return Result<void>::failure(
                    encode_error(JsonPointer{}.append("pools"),
                                 "pool_descriptor_cursor_mismatch"));
            }
            const std::string_view storage = values.byte_storage();
            for (const NodeIndex node_index : layout.variable_values()) {
                if (node_index >= values.nodes().size()) {
                    return Result<void>::failure(
                        encode_error(JsonPointer{}.append("pools"),
                                     "pool_node_out_of_range"));
                }
                const ValueNode& node =
                    values.nodes()[static_cast<std::size_t>(node_index)];
                const layout::RuntimeType* const runtime =
                    layout.runtime_schema().find_type(node.runtime_type_id);
                if (runtime == nullptr || runtime->source->kind != wanted ||
                    node.tag == ValueTag::null_value) {
                    continue;
                }
                auto storage_end = layout::checked_range_end(
                    node.scalar_bits_or_offset, node.byte_length,
                    storage.size(), JsonPointer{}.append("pools"));
                if (!storage_end.has_value()) {
                    return Result<void>::failure(
                        std::move(storage_end).error());
                }
                if (node.byte_length != UINT64_C(0)) {
                    written = writer.write(
                        reinterpret_cast<const std::uint8_t*>(storage.data()) +
                            static_cast<std::ptrdiff_t>(
                                node.scalar_bits_or_offset),
                        node.byte_length);
                    if (!written.has_value()) {
                        return written;
                    }
                }
            }
            auto pool_end =
                layout::checked_add_u64(region.data_offset, region.byte_length,
                                        JsonPointer{}.append("pools"));
            if (!pool_end.has_value()) {
                return Result<void>::failure(std::move(pool_end).error());
            }
            if (writer.offset() != pool_end.value()) {
                return Result<void>::failure(
                    encode_error(JsonPointer{}.append("pools"),
                                 "pool_write_length_mismatch"));
            }
        } else {
            return Result<void>::failure(encode_error(
                JsonPointer{}.append("regions"),
                "unsupported_initial_region"));
        }
    }
    auto final_padding = writer.zero_until(layout.total_length());
    if (!final_padding.has_value()) {
        return final_padding;
    }
    if (writer.offset() != layout.total_length()) {
        return Result<void>::failure(encode_error(
            JsonPointer{}.append("sink"), "encoder_length_mismatch"));
    }
    return Result<void>::success();
} catch (const std::bad_alloc&) {
    return Result<void>::failure(allocation_error());
}

}  // namespace fastdb::payload::build
