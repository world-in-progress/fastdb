#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/InputSpan.hpp"

#include <fastdb_payload.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace fastdb::payload::layout {
namespace checked_math_detail {

inline error::Error arithmetic_error(const json::JsonPointer& path,
                                     const char* reason) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_LENGTH_OVERFLOW, path,
        "Portable payload wire arithmetic overflowed",
        json::JsonValue::object({json::JsonValue::Member{
            "reason", json::JsonValue{reason}}}));
}

inline error::Error bounds_error(const json::JsonPointer& path,
                                 std::uint64_t end,
                                 std::uint64_t available) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_OUT_OF_BOUNDS, path,
        "Portable payload wire range is outside the supplied bytes",
        json::JsonValue::object({
            json::JsonValue::Member{"available",
                                    json::JsonValue{std::to_string(available)}},
            json::JsonValue::Member{"end",
                                    json::JsonValue{std::to_string(end)}},
            json::JsonValue::Member{"reason",
                                    json::JsonValue{"wire_range_out_of_bounds"}},
        }));
}

}  // namespace checked_math_detail

inline error::Result<std::uint32_t> checked_narrow_u32(
    std::uint64_t value,
    const json::JsonPointer& path) {
    if (value > static_cast<std::uint64_t>(UINT32_MAX)) {
        return error::Result<std::uint32_t>::failure(
            error::Error::from_details(
                FDB_PAYLOAD_E_RESOURCE_LIMIT, path,
                "Portable payload value exceeds the uint32 wire limit",
                json::JsonValue::object({
                    json::JsonValue::Member{
                        "actual", json::JsonValue{std::to_string(value)}},
                    json::JsonValue::Member{
                        "limit",
                        json::JsonValue{std::to_string(UINT32_MAX)}},
                    json::JsonValue::Member{
                        "reason", json::JsonValue{"wire_uint32_limit"}},
                })));
    }
    return error::Result<std::uint32_t>::success(
        static_cast<std::uint32_t>(value));
}

inline error::Result<std::uint64_t> checked_add_u64(
    std::uint64_t left,
    std::uint64_t right,
    const json::JsonPointer& path) {
    if (right > UINT64_MAX - left) {
        return error::Result<std::uint64_t>::failure(
            checked_math_detail::arithmetic_error(path, "wire_add_overflow"));
    }
    return error::Result<std::uint64_t>::success(left + right);
}

inline error::Result<void> checked_accumulate_u64(
    std::uint64_t& total,
    std::uint64_t units,
    const json::JsonPointer& path) {
    auto added = checked_add_u64(total, units, path);
    if (!added.has_value()) {
        return error::Result<void>::failure(std::move(added).error());
    }
    total = added.value();
    return error::Result<void>::success();
}

inline error::Result<std::uint64_t> checked_multiply_u64(
    std::uint64_t left,
    std::uint64_t right,
    const json::JsonPointer& path) {
    if (left != UINT64_C(0) && right > UINT64_MAX / left) {
        return error::Result<std::uint64_t>::failure(
            checked_math_detail::arithmetic_error(
                path, "wire_multiply_overflow"));
    }
    return error::Result<std::uint64_t>::success(left * right);
}

inline error::Result<std::uint64_t> checked_align_up_u64(
    std::uint64_t value,
    std::uint32_t alignment,
    const json::JsonPointer& path) {
    if (alignment == UINT32_C(0) ||
        (alignment & (alignment - UINT32_C(1))) != UINT32_C(0)) {
        return error::Result<std::uint64_t>::failure(
            error::Error::from_details(
                FDB_PAYLOAD_E_MISALIGNED, path,
                "Portable payload alignment is invalid",
                json::JsonValue::object({json::JsonValue::Member{
                    "reason", json::JsonValue{"invalid_alignment"}}})));
    }
    const std::uint64_t mask =
        static_cast<std::uint64_t>(alignment - UINT32_C(1));
    auto added = checked_add_u64(value, mask, path);
    if (!added.has_value()) {
        return added;
    }
    return error::Result<std::uint64_t>::success(added.value() & ~mask);
}

inline error::Result<std::uint64_t> checked_range_end(
    std::uint64_t offset,
    std::uint64_t size,
    std::uint64_t available,
    const json::JsonPointer& path) {
    auto end = checked_add_u64(offset, size, path);
    if (!end.has_value()) {
        return end;
    }
    if (end.value() > available) {
        return error::Result<std::uint64_t>::failure(
            checked_math_detail::bounds_error(path, end.value(), available));
    }
    return end;
}

template <typename Integer>
inline error::Result<Integer> load_little_endian(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    const json::JsonPointer& path) {
    constexpr std::uint64_t width = sizeof(Integer);
    auto end = checked_range_end(offset, width, byte_count, path);
    if (!end.has_value()) {
        return error::Result<Integer>::failure(std::move(end).error());
    }
    if (bytes == nullptr || !input_span_is_addressable(end.value())) {
        return error::Result<Integer>::failure(
            checked_math_detail::bounds_error(path, end.value(), byte_count));
    }
    Integer value = 0;
    for (std::uint64_t index = UINT64_C(0); index < width; ++index) {
        const std::uint64_t shifted =
            static_cast<std::uint64_t>(bytes[static_cast<std::ptrdiff_t>(
                offset + index)])
            << (index * UINT64_C(8));
        value = static_cast<Integer>(value | static_cast<Integer>(shifted));
    }
    return error::Result<Integer>::success(value);
}

template <typename Integer>
inline error::Result<void> store_little_endian(
    std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    Integer value,
    const json::JsonPointer& path) {
    constexpr std::uint64_t width = sizeof(Integer);
    auto end = checked_range_end(offset, width, byte_count, path);
    if (!end.has_value()) {
        return error::Result<void>::failure(std::move(end).error());
    }
    if (bytes == nullptr || !input_span_is_addressable(end.value())) {
        return error::Result<void>::failure(
            checked_math_detail::bounds_error(path, end.value(), byte_count));
    }
    for (std::uint64_t index = UINT64_C(0); index < width; ++index) {
        bytes[static_cast<std::ptrdiff_t>(offset + index)] =
            static_cast<std::uint8_t>(
                (static_cast<std::uint64_t>(value) >>
                 (index * UINT64_C(8))) &
                UINT64_C(0xff));
    }
    return error::Result<void>::success();
}

inline error::Result<std::uint16_t> load_u16_le(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    const json::JsonPointer& path) {
    return load_little_endian<std::uint16_t>(bytes, byte_count, offset, path);
}
inline error::Result<std::uint32_t> load_u32_le(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    const json::JsonPointer& path) {
    return load_little_endian<std::uint32_t>(bytes, byte_count, offset, path);
}
inline error::Result<std::uint64_t> load_u64_le(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    const json::JsonPointer& path) {
    return load_little_endian<std::uint64_t>(bytes, byte_count, offset, path);
}
inline error::Result<void> store_u16_le(
    std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    std::uint16_t value,
    const json::JsonPointer& path) {
    return store_little_endian(bytes, byte_count, offset, value, path);
}
inline error::Result<void> store_u32_le(
    std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    std::uint32_t value,
    const json::JsonPointer& path) {
    return store_little_endian(bytes, byte_count, offset, value, path);
}
inline error::Result<void> store_u64_le(
    std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    std::uint64_t value,
    const json::JsonPointer& path) {
    return store_little_endian(bytes, byte_count, offset, value, path);
}

}  // namespace fastdb::payload::layout
