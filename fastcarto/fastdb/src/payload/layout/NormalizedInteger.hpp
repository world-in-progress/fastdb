#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonPointer.hpp"

#include <cstdint>
#include <string_view>

namespace fastdb::payload::layout {

error::Result<void> validate_normalized_input(
    std::uint64_t binary64_bits,
    double minimum,
    double maximum,
    std::string_view kind,
    const json::JsonPointer& path);

error::Result<std::uint32_t> quantize_normalized(
    std::uint64_t binary64_bits,
    double minimum,
    double maximum,
    std::uint32_t maximum_code,
    std::string_view kind,
    const json::JsonPointer& path);

error::Result<std::uint64_t> dequantize_normalized(
    std::uint32_t code,
    double minimum,
    double maximum,
    std::uint32_t maximum_code,
    std::string_view kind,
    const json::JsonPointer& path);

}  // namespace fastdb::payload::layout
