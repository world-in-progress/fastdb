#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/CheckedMath.hpp"

#include <fastdb_payload.h>

#include <array>
#include <cstdint>
#include <string>

namespace fastdb::payload::layout {

inline constexpr std::array<std::uint8_t, 8> binary_magic{{
    UINT8_C(0x46), UINT8_C(0x44), UINT8_C(0x42), UINT8_C(0x50),
    UINT8_C(0x41), UINT8_C(0x59), UINT8_C(0x31), UINT8_C(0x00)}};
inline constexpr std::uint16_t binary_major = UINT16_C(1);
inline constexpr std::uint16_t binary_minor = UINT16_C(0);
inline constexpr std::uint32_t header_size = UINT32_C(128);
inline constexpr std::uint32_t region_descriptor_size = UINT32_C(56);
inline constexpr std::uint32_t entry_descriptor_size = UINT32_C(40);

inline constexpr std::uint64_t header_magic_offset = UINT64_C(0);
inline constexpr std::uint64_t header_major_offset = UINT64_C(8);
inline constexpr std::uint64_t header_minor_offset = UINT64_C(10);
inline constexpr std::uint64_t header_size_offset = UINT64_C(12);
inline constexpr std::uint64_t header_profile_offset = UINT64_C(16);
inline constexpr std::uint64_t header_flags_offset = UINT64_C(20);
inline constexpr std::uint64_t header_total_length_offset = UINT64_C(24);
inline constexpr std::uint64_t header_spec_digest_offset = UINT64_C(32);
inline constexpr std::uint64_t header_region_directory_offset = UINT64_C(64);
inline constexpr std::uint64_t header_region_count_offset = UINT64_C(72);
inline constexpr std::uint64_t header_region_descriptor_size_offset =
    UINT64_C(76);
inline constexpr std::uint64_t header_entry_directory_offset = UINT64_C(80);
inline constexpr std::uint64_t header_entry_count_offset = UINT64_C(88);
inline constexpr std::uint64_t header_entry_descriptor_size_offset =
    UINT64_C(92);
inline constexpr std::uint64_t header_root_value_count_offset = UINT64_C(96);
inline constexpr std::uint64_t header_reserved_offset = UINT64_C(104);

inline constexpr std::uint64_t region_kind_offset = UINT64_C(0);
inline constexpr std::uint64_t region_flags_offset = UINT64_C(4);
inline constexpr std::uint64_t region_owner_index_offset = UINT64_C(8);
inline constexpr std::uint64_t region_runtime_type_id_offset = UINT64_C(12);
inline constexpr std::uint64_t region_data_offset_offset = UINT64_C(16);
inline constexpr std::uint64_t region_byte_length_offset = UINT64_C(24);
inline constexpr std::uint64_t region_element_count_offset = UINT64_C(32);
inline constexpr std::uint64_t region_stride_offset = UINT64_C(40);
inline constexpr std::uint64_t region_alignment_offset = UINT64_C(44);
inline constexpr std::uint64_t region_reserved_offset = UINT64_C(48);

inline constexpr std::uint64_t entry_index_offset = UINT64_C(0);
inline constexpr std::uint64_t entry_runtime_type_id_offset = UINT64_C(4);
inline constexpr std::uint64_t entry_cardinality_offset = UINT64_C(8);
inline constexpr std::uint64_t entry_flags_offset = UINT64_C(12);
inline constexpr std::uint64_t entry_value_count_offset = UINT64_C(16);
inline constexpr std::uint64_t entry_values_region_index_offset = UINT64_C(24);
inline constexpr std::uint64_t entry_validity_region_index_offset =
    UINT64_C(28);
inline constexpr std::uint64_t entry_reserved_offset = UINT64_C(32);

static_assert(header_reserved_offset + UINT64_C(24) == header_size);
static_assert(region_reserved_offset + UINT64_C(8) == region_descriptor_size);
static_assert(entry_reserved_offset + UINT64_C(8) == entry_descriptor_size);

enum class RegionKind : std::uint32_t {
    entry_values = FDB_PAYLOAD_REGION_ENTRY_VALUES,
    entry_validity = FDB_PAYLOAD_REGION_ENTRY_VALIDITY,
    list_items = FDB_PAYLOAD_REGION_LIST_ITEMS,
    list_validity = FDB_PAYLOAD_REGION_LIST_VALIDITY,
    utf8_pool = FDB_PAYLOAD_REGION_UTF8_POOL,
    utf16_pool = FDB_PAYLOAD_REGION_UTF16_POOL,
    bytes_pool = FDB_PAYLOAD_REGION_BYTES_POOL,
    object_values = FDB_PAYLOAD_REGION_OBJECT_VALUES,
};

enum class RegionCountUnit : std::uint8_t {
    values,
    items,
    bytes,
    utf16_code_units,
    objects,
};

enum class RegionOwnerRule : std::uint8_t {
    entry_index,
    list_runtime_type_id,
    component_index,
    sentinel,
};

enum class RegionTypeRule : std::uint8_t {
    entry_root_runtime_type_id,
    list_item_runtime_type_id,
    sentinel,
};

enum class RegionStrideRule : std::uint8_t {
    slot_stride,
    component_stride,
    zero,
};

enum class RegionAlignmentRule : std::uint8_t {
    slot_alignment,
    component_alignment,
    one,
    two,
};

struct RegionRule final {
    std::uint32_t kind_value;
    RegionCountUnit count_unit;
    RegionOwnerRule owner_rule;
    RegionTypeRule type_rule;
    RegionStrideRule stride_rule;
    RegionAlignmentRule alignment_rule;
    bool is_validity;
    bool is_pool;
};

constexpr RegionRule region_rule(RegionKind kind) noexcept {
    switch (kind) {
    case RegionKind::entry_values:
        return {UINT32_C(1),
                RegionCountUnit::values,
                RegionOwnerRule::entry_index,
                RegionTypeRule::entry_root_runtime_type_id,
                RegionStrideRule::slot_stride,
                RegionAlignmentRule::slot_alignment,
                false,
                false};
    case RegionKind::entry_validity:
        return {UINT32_C(2),
                RegionCountUnit::values,
                RegionOwnerRule::entry_index,
                RegionTypeRule::entry_root_runtime_type_id,
                RegionStrideRule::zero,
                RegionAlignmentRule::one,
                true,
                false};
    case RegionKind::list_items:
        return {UINT32_C(3),
                RegionCountUnit::items,
                RegionOwnerRule::list_runtime_type_id,
                RegionTypeRule::list_item_runtime_type_id,
                RegionStrideRule::slot_stride,
                RegionAlignmentRule::slot_alignment,
                false,
                false};
    case RegionKind::list_validity:
        return {UINT32_C(4),
                RegionCountUnit::items,
                RegionOwnerRule::list_runtime_type_id,
                RegionTypeRule::list_item_runtime_type_id,
                RegionStrideRule::zero,
                RegionAlignmentRule::one,
                true,
                false};
    case RegionKind::utf8_pool:
        return {UINT32_C(5),
                RegionCountUnit::bytes,
                RegionOwnerRule::sentinel,
                RegionTypeRule::sentinel,
                RegionStrideRule::zero,
                RegionAlignmentRule::one,
                false,
                true};
    case RegionKind::utf16_pool:
        return {UINT32_C(6),
                RegionCountUnit::utf16_code_units,
                RegionOwnerRule::sentinel,
                RegionTypeRule::sentinel,
                RegionStrideRule::zero,
                RegionAlignmentRule::two,
                false,
                true};
    case RegionKind::bytes_pool:
        return {UINT32_C(7),
                RegionCountUnit::bytes,
                RegionOwnerRule::sentinel,
                RegionTypeRule::sentinel,
                RegionStrideRule::zero,
                RegionAlignmentRule::one,
                false,
                true};
    case RegionKind::object_values:
        return {UINT32_C(8),
                RegionCountUnit::objects,
                RegionOwnerRule::component_index,
                RegionTypeRule::sentinel,
                RegionStrideRule::component_stride,
                RegionAlignmentRule::component_alignment,
                false,
                false};
    }
    return {UINT32_C(0),
            RegionCountUnit::bytes,
            RegionOwnerRule::sentinel,
            RegionTypeRule::sentinel,
            RegionStrideRule::zero,
            RegionAlignmentRule::one,
            false,
            false};
}

struct RegionDescriptor final {
    RegionKind kind;
    std::uint32_t flags;
    std::uint32_t owner_index;
    std::uint32_t runtime_type_id;
    std::uint64_t data_offset;
    std::uint64_t byte_length;
    std::uint64_t element_count;
    std::uint32_t stride;
    std::uint32_t alignment;
};

struct EntryDescriptor final {
    std::uint32_t entry_index;
    std::uint32_t runtime_type_id;
    std::uint32_t cardinality;
    std::uint32_t flags;
    std::uint64_t value_count;
    std::uint32_t values_region_index;
    std::uint32_t validity_region_index;
};

inline error::Error partition_error(const json::JsonPointer& path,
                                    const char* reason,
                                    std::uint64_t expected,
                                    std::uint64_t actual) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_NON_CANONICAL_BINARY, path,
        "Portable payload partition is not canonical",
        json::JsonValue::object({
            json::JsonValue::Member{"actual",
                                    json::JsonValue{std::to_string(actual)}},
            json::JsonValue::Member{"expected",
                                    json::JsonValue{std::to_string(expected)}},
            json::JsonValue::Member{"reason", json::JsonValue{reason}},
        }));
}

inline error::Result<std::uint64_t> checked_partition_advance(
    std::uint64_t cursor,
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t total,
    const json::JsonPointer& path) {
    if (offset != cursor) {
        return error::Result<std::uint64_t>::failure(
            partition_error(path, "partition_cursor_mismatch", cursor,
                            offset));
    }
    auto end = checked_range_end(offset, length, total, path);
    if (!end.has_value()) {
        return end;
    }
    return end;
}

inline error::Result<void> require_partition_consumed(
    std::uint64_t cursor,
    std::uint64_t total,
    const json::JsonPointer& path) {
    if (cursor != total) {
        return error::Result<void>::failure(
            partition_error(path, "partition_not_consumed", total, cursor));
    }
    return error::Result<void>::success();
}

}  // namespace fastdb::payload::layout
