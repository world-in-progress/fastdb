#include "payload/build/RecordEncoder.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/CheckedMath.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

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
    default:
        return false;
    }
}

Result<void> encode_slot(const RecordLayout& layout,
                         const LogicalPayload& values,
                         const ValueNode& node,
                         AscendingWriter& writer) {
    const layout::RuntimeType& runtime =
        layout.runtime_schema().type(node.runtime_type_id);
    std::array<std::uint8_t, 8> bytes{};
    if (node.tag == ValueTag::null_value) {
        return writer.write(bytes.data(), runtime.slot.stride);
    }
    if (!tag_matches(runtime.source->kind, node.tag)) {
        return Result<void>::failure(encode_error(
            JsonPointer{}.append("values"), "value_tag_mismatch"));
    }
    switch (runtime.source->kind) {
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
    default:
        return Result<void>::failure(encode_error(
            JsonPointer{}.append("values"), "unsupported_initial_slot"));
    }
    (void)values;
    return writer.write(bytes.data(), runtime.slot.stride);
}

}  // namespace

Result<void> encode_record(const RecordLayout& layout,
                           const LogicalPayload& values,
                           ByteSink& sink) {
    if (layout.runtime_schema().spec().digest() != values.spec().digest()) {
        return Result<void>::failure(encode_error(
            JsonPointer{}, "encode_spec_digest_mismatch"));
    }
    AscendingWriter writer(sink);
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
        const auto& nodes = layout.entry_values()[region.owner_index];
        if (region.kind == RegionKind::entry_validity) {
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
            for (const NodeIndex node_index : nodes) {
                written = encode_slot(
                    layout, values,
                    values.nodes()[static_cast<std::size_t>(node_index)],
                    writer);
                if (!written.has_value()) {
                    return written;
                }
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
}

}  // namespace fastdb::payload::build
