#include "payload/view/OpenCommon.hpp"

#include "payload/json/JsonValue.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/CheckedMath.hpp"

#include <fastdb_payload.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace fastdb::payload::view::open_common {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

Error simple_error(std::uint32_t code,
                   const JsonPointer& path,
                   const char* message,
                   const char* reason) {
    return Error::from_details(
        code, path, message,
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

Error noncanonical(const JsonPointer& path, const char* reason) {
    return simple_error(FDB_PAYLOAD_E_NON_CANONICAL_BINARY, path,
                        "Portable payload binary is not canonical", reason);
}

Error noncanonical_exact(const JsonPointer& path,
                         const char* reason,
                         std::string actual,
                         std::string expected) {
    return Error::from_details(
        FDB_PAYLOAD_E_NON_CANONICAL_BINARY, path,
        "Portable payload binary is not canonical",
        JsonValue::object({
            JsonValue::Member{"actual", JsonValue{std::move(actual)}},
            JsonValue::Member{"expected", JsonValue{std::move(expected)}},
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

Result<void> require_canonical_u64(std::uint64_t actual,
                                   std::uint64_t expected,
                                   const JsonPointer& path,
                                   const char* reason) {
    if (actual == expected) {
        return Result<void>::success();
    }
    return Result<void>::failure(noncanonical_exact(
        path, reason, std::to_string(actual), std::to_string(expected)));
}

Error resource_error(const JsonPointer& path,
                     const char* resource,
                     std::uint64_t actual,
                     std::uint64_t limit) {
    return Error::from_details(
        FDB_PAYLOAD_E_RESOURCE_LIMIT, path,
        "Portable payload open resource limit exceeded",
        JsonValue::object({
            JsonValue::Member{"actual", JsonValue{std::to_string(actual)}},
            JsonValue::Member{"limit", JsonValue{std::to_string(limit)}},
            JsonValue::Member{"resource", JsonValue{resource}},
        }));
}

Error allocation_error() {
    return simple_error(FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
                        "Portable payload open allocation failed",
                        "allocation_failed");
}

JsonPointer binary_header_path() {
    return JsonPointer{}.append("binary").append("header");
}

JsonPointer binary_region_path(std::uint32_t index) {
    return JsonPointer{}.append("binary").append("regions").append(index);
}

JsonPointer binary_entry_path(std::uint32_t index) {
    return JsonPointer{}.append("binary").append("entries").append(index);
}

std::string byte_hex(const std::uint8_t* bytes, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2U, '0');
    for (std::size_t index = 0U; index < size; ++index) {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & UINT8_C(0x0f)];
    }
    return result;
}

Result<void> WorkCounter::charge(std::uint64_t units,
                                 const JsonPointer& path) {
    std::uint64_t next = value_;
    auto accumulated = layout::checked_accumulate_u64(next, units, path);
    if (!accumulated.has_value()) {
        return accumulated;
    }
    if (next > limit_) {
        return Result<void>::failure(resource_error(
            path, "validation_work", next, limit_));
    }
    value_ = next;
    return Result<void>::success();
}

Result<void> require_zero(const std::uint8_t* bytes,
                          std::uint64_t byte_count,
                          std::uint64_t begin,
                          std::uint64_t end,
                          WorkCounter& work,
                          const JsonPointer& path,
                          const char* reason,
                          bool charge_units,
                          bool exact_facts) {
    if (end < begin) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_LENGTH_OVERFLOW, path,
            "Portable payload zero range is reversed",
            "reversed_zero_range"));
    }
    auto span = layout::checked_range_end(begin, end - begin, byte_count, path);
    if (!span.has_value()) {
        return Result<void>::failure(std::move(span).error());
    }
    if (charge_units) {
        auto charged = work.charge(end - begin, path);
        if (!charged.has_value()) {
            return charged;
        }
    }
    for (std::uint64_t offset = begin; offset < end; ++offset) {
        if (bytes[static_cast<std::ptrdiff_t>(offset)] != UINT8_C(0)) {
            if (exact_facts) {
                return Result<void>::failure(noncanonical_exact(
                    path, reason,
                    std::to_string(bytes[static_cast<std::ptrdiff_t>(offset)]),
                    "0"));
            }
            return Result<void>::failure(noncanonical(path, reason));
        }
    }
    return Result<void>::success();
}

Result<std::uint32_t> read_u32(const std::uint8_t* bytes,
                               std::uint64_t byte_count,
                               std::uint64_t offset,
                               const JsonPointer& path) {
    return layout::load_u32_le(bytes, byte_count, offset, path);
}

Result<std::uint64_t> read_u64(const std::uint8_t* bytes,
                               std::uint64_t byte_count,
                               std::uint64_t offset,
                               const JsonPointer& path) {
    return layout::load_u64_le(bytes, byte_count, offset, path);
}

Result<RegionFields> read_region_fields(const std::uint8_t* bytes,
                                        std::uint64_t byte_count,
                                        std::uint32_t region_index,
                                        const JsonPointer& path) {
    const std::uint64_t base =
        layout::header_size + static_cast<std::uint64_t>(region_index) *
                                  layout::region_descriptor_size;
    auto kind = read_u32(bytes, byte_count, base + layout::region_kind_offset,
                         path.append("kind"));
    if (!kind.has_value()) {
        return Result<RegionFields>::failure(std::move(kind).error());
    }
    auto flags = read_u32(bytes, byte_count,
                          base + layout::region_flags_offset,
                          path.append("flags"));
    if (!flags.has_value()) {
        return Result<RegionFields>::failure(std::move(flags).error());
    }
    auto owner = read_u32(bytes, byte_count,
                          base + layout::region_owner_index_offset,
                          path.append("owner_index"));
    if (!owner.has_value()) {
        return Result<RegionFields>::failure(std::move(owner).error());
    }
    auto type_id = read_u32(bytes, byte_count,
                            base + layout::region_runtime_type_id_offset,
                            path.append("runtime_type_id"));
    if (!type_id.has_value()) {
        return Result<RegionFields>::failure(std::move(type_id).error());
    }
    auto data_offset = read_u64(bytes, byte_count,
                                base + layout::region_data_offset_offset,
                                path.append("data_offset"));
    if (!data_offset.has_value()) {
        return Result<RegionFields>::failure(std::move(data_offset).error());
    }
    auto byte_length = read_u64(bytes, byte_count,
                                base + layout::region_byte_length_offset,
                                path.append("byte_length"));
    if (!byte_length.has_value()) {
        return Result<RegionFields>::failure(std::move(byte_length).error());
    }
    auto element_count = read_u64(
        bytes, byte_count, base + layout::region_element_count_offset,
        path.append("element_count"));
    if (!element_count.has_value()) {
        return Result<RegionFields>::failure(std::move(element_count).error());
    }
    auto stride = read_u32(bytes, byte_count,
                           base + layout::region_stride_offset,
                           path.append("stride"));
    if (!stride.has_value()) {
        return Result<RegionFields>::failure(std::move(stride).error());
    }
    auto alignment = read_u32(bytes, byte_count,
                              base + layout::region_alignment_offset,
                              path.append("alignment"));
    if (!alignment.has_value()) {
        return Result<RegionFields>::failure(std::move(alignment).error());
    }
    auto reserved = read_u64(bytes, byte_count,
                             base + layout::region_reserved_offset,
                             path.append("reserved"));
    if (!reserved.has_value()) {
        return Result<RegionFields>::failure(std::move(reserved).error());
    }
    return Result<RegionFields>::success(RegionFields{
        kind.value(), flags.value(), owner.value(), type_id.value(),
        data_offset.value(), byte_length.value(), element_count.value(),
        stride.value(), alignment.value(), reserved.value()});
}

Result<void> validate_region_fields(const RegionFields& actual,
                                    const RegionFields& expected,
                                    const JsonPointer& path) {
    struct Field final {
        std::uint64_t actual;
        std::uint64_t expected;
        const char* name;
        const char* reason;
    };
    const std::array<Field, 10> fields{{
        {actual.kind, expected.kind, "kind", "region_kind"},
        {actual.flags, expected.flags, "flags", "region_flags"},
        {actual.owner, expected.owner, "owner_index", "region_owner_index"},
        {actual.runtime_type_id, expected.runtime_type_id,
         "runtime_type_id", "region_runtime_type_id"},
        {actual.data_offset, expected.data_offset, "data_offset",
         "region_data_offset"},
        {actual.byte_length, expected.byte_length, "byte_length",
         "region_byte_length"},
        {actual.element_count, expected.element_count, "element_count",
         "region_element_count"},
        {actual.stride, expected.stride, "stride", "region_stride"},
        {actual.alignment, expected.alignment, "alignment",
         "region_alignment"},
        {actual.reserved, expected.reserved, "reserved", "region_reserved"},
    }};
    for (const Field& field : fields) {
        auto valid = require_canonical_u64(
            field.actual, field.expected, path.append(field.name),
            field.reason);
        if (!valid.has_value()) {
            return valid;
        }
    }
    return Result<void>::success();
}

}  // namespace fastdb::payload::view::open_common
