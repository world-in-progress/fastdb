#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonPointer.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace fastdb::payload::layout {

error::Result<void> validate_utf8(std::string_view bytes,
                                  const json::JsonPointer& path);
error::Result<void> validate_utf16(const std::uint16_t* units,
                                   std::uint64_t count,
                                   const json::JsonPointer& path);
void append_utf16le(std::vector<std::uint8_t>& output,
                    const std::uint16_t* units,
                    std::uint64_t count);

}  // namespace fastdb::payload::layout
