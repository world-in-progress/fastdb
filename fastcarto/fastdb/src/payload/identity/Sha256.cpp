#include "payload/identity/Sha256.hpp"

#include <picosha2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace fastdb::payload::identity {

std::array<std::uint8_t, 32> sha256(const std::uint8_t* bytes,
                                    std::uint64_t size) {
    if (bytes == nullptr && size != 0U) {
        throw std::invalid_argument("SHA-256 bytes must not be null");
    }
    if constexpr (sizeof(std::ptrdiff_t) <= sizeof(std::uint64_t)) {
        if (size > static_cast<std::uint64_t>(
                       std::numeric_limits<std::ptrdiff_t>::max())) {
            throw std::length_error("SHA-256 byte span exceeds iterator range");
        }
    }

    std::array<std::uint8_t, 32> digest{};
    if (size == 0U) {
        const std::uint8_t empty = 0;
        picosha2::hash256(&empty, &empty, digest.begin(), digest.end());
        return digest;
    }

    const auto iterator_distance = static_cast<std::ptrdiff_t>(size);
    picosha2::hash256(bytes, bytes + iterator_distance, digest.begin(),
                      digest.end());
    return digest;
}

std::string sha256_lower_hex(
    const std::array<std::uint8_t, 32>& digest) {
    constexpr char hex_digits[] = "0123456789abcdef";
    std::string hexadecimal(digest.size() * 2U, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const std::uint8_t byte = digest[index];
        hexadecimal[index * 2U] =
            hex_digits[static_cast<std::size_t>(byte >> 4U)];
        hexadecimal[index * 2U + 1U] =
            hex_digits[static_cast<std::size_t>(byte & UINT8_C(0x0f))];
    }
    return hexadecimal;
}

}  // namespace fastdb::payload::identity
