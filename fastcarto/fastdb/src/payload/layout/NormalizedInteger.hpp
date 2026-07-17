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

}  // namespace fastdb::payload::layout
