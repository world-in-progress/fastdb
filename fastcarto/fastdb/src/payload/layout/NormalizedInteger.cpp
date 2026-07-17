#include "payload/layout/NormalizedInteger.hpp"

#include "payload/json/JsonValue.hpp"

#include <fastdb_payload.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace fastdb::payload::layout {
namespace {

std::uint64_t double_bits(double value) noexcept {
    std::uint64_t bits = UINT64_C(0);
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool finite(std::uint64_t bits) noexcept {
    return (bits & UINT64_C(0x7ff0000000000000)) !=
           UINT64_C(0x7ff0000000000000);
}

std::uint64_t ordered_key(std::uint64_t bits) noexcept {
    if ((bits & UINT64_C(0x7fffffffffffffff)) == UINT64_C(0)) {
        bits = UINT64_C(0);
    }
    if ((bits & UINT64_C(0x8000000000000000)) != UINT64_C(0)) {
        return ~bits;
    }
    return bits | UINT64_C(0x8000000000000000);
}

error::Error range_error(std::uint64_t bits,
                         std::string_view kind,
                         const json::JsonPointer& path) {
    const char* const reason = finite(bits) ? "outside_declared_range"
                                            : "non_finite";
    return error::Error::from_details(
        FDB_PAYLOAD_E_OUT_OF_RANGE, path,
        "Normalized payload value is outside its finite declared range",
        json::JsonValue::object({
            json::JsonValue::Member{"kind",
                                    json::JsonValue{std::string(kind)}},
            json::JsonValue::Member{"reason", json::JsonValue{reason}},
        }));
}

}  // namespace

error::Result<void> validate_normalized_input(
    std::uint64_t binary64_bits,
    double minimum,
    double maximum,
    std::string_view kind,
    const json::JsonPointer& path) {
    if (!finite(binary64_bits)) {
        return error::Result<void>::failure(
            range_error(binary64_bits, kind, path));
    }
    const std::uint64_t value_key = ordered_key(binary64_bits);
    if (value_key < ordered_key(double_bits(minimum)) ||
        value_key > ordered_key(double_bits(maximum))) {
        return error::Result<void>::failure(
            range_error(binary64_bits, kind, path));
    }
    return error::Result<void>::success();
}

}  // namespace fastdb::payload::layout
