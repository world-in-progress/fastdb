#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace fastdb::payload::identity {

std::array<std::uint8_t, 32> sha256(const std::uint8_t* bytes,
                                    std::uint64_t size);

std::string sha256_lower_hex(const std::array<std::uint8_t, 32>& digest);

}  // namespace fastdb::payload::identity
