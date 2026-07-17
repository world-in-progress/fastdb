#include "payload/layout/TextEncoding.hpp"

#include "payload/json/JsonValue.hpp"

#include <fastdb_payload.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace fastdb::payload::layout {
namespace {

error::Error invalid_text(const json::JsonPointer& path,
                          const char* encoding,
                          const char* reason) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_INVALID_TEXT_ENCODING, path,
        "Payload text encoding is invalid",
        json::JsonValue::object({
            json::JsonValue::Member{"encoding", json::JsonValue{encoding}},
            json::JsonValue::Member{"reason", json::JsonValue{reason}},
        }));
}

bool continuation(const unsigned char byte) noexcept {
    return (byte & 0xc0U) == 0x80U;
}

}  // namespace

error::Result<void> validate_utf8(std::string_view bytes,
                                  const json::JsonPointer& path) {
    std::size_t index = 0U;
    while (index < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t width = 0U;
        if (first >= 0xc2U && first <= 0xdfU) {
            width = 2U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            width = 3U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            width = 4U;
        } else {
            return error::Result<void>::failure(
                invalid_text(path, "utf-8", "invalid_sequence"));
        }
        if (width > bytes.size() - index) {
            return error::Result<void>::failure(
                invalid_text(path, "utf-8", "invalid_sequence"));
        }
        const auto second = static_cast<unsigned char>(bytes[index + 1U]);
        if (!continuation(second)) {
            return error::Result<void>::failure(
                invalid_text(path, "utf-8", "invalid_sequence"));
        }
        if ((first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second >= 0xa0U) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second > 0x8fU)) {
            return error::Result<void>::failure(
                invalid_text(path, "utf-8", "invalid_sequence"));
        }
        for (std::size_t offset = 2U; offset < width; ++offset) {
            if (!continuation(
                    static_cast<unsigned char>(bytes[index + offset]))) {
                return error::Result<void>::failure(
                    invalid_text(path, "utf-8", "invalid_sequence"));
            }
        }
        index += width;
    }
    return error::Result<void>::success();
}

error::Result<void> validate_utf16(const std::uint16_t* units,
                                   std::uint64_t count,
                                   const json::JsonPointer& path) {
    std::uint64_t index = UINT64_C(0);
    while (index < count) {
        const std::uint16_t unit = units[index];
        if (unit >= UINT16_C(0xd800) && unit <= UINT16_C(0xdbff)) {
            if (index + UINT64_C(1) >= count) {
                return error::Result<void>::failure(
                    invalid_text(path, "utf-16", "unpaired_surrogate"));
            }
            const std::uint16_t trail = units[index + UINT64_C(1)];
            if (trail < UINT16_C(0xdc00) || trail > UINT16_C(0xdfff)) {
                return error::Result<void>::failure(
                    invalid_text(path, "utf-16", "unpaired_surrogate"));
            }
            index += UINT64_C(2);
            continue;
        }
        if (unit >= UINT16_C(0xdc00) && unit <= UINT16_C(0xdfff)) {
            return error::Result<void>::failure(
                invalid_text(path, "utf-16", "unpaired_surrogate"));
        }
        ++index;
    }
    return error::Result<void>::success();
}

void append_utf16le(std::vector<std::uint8_t>& output,
                    const std::uint16_t* units,
                    std::uint64_t count) {
    for (std::uint64_t index = UINT64_C(0); index < count; ++index) {
        const std::uint16_t unit = units[index];
        output.push_back(static_cast<std::uint8_t>(unit & UINT16_C(0x00ff)));
        output.push_back(static_cast<std::uint8_t>(unit >> 8U));
    }
}

}  // namespace fastdb::payload::layout
