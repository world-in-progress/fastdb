#include "payload/json/Jcs.hpp"

#include <double-conversion/double-conversion.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::payload::json {
namespace {

bool is_continuation(std::uint8_t byte) {
    return byte >= UINT8_C(0x80) && byte <= UINT8_C(0xbf);
}

bool decode_utf8_scalar(std::string_view text,
                        std::size_t& offset,
                        std::uint32_t& scalar) {
    if (offset >= text.size()) {
        return false;
    }

    const auto first = static_cast<std::uint8_t>(
        static_cast<unsigned char>(text[offset]));
    if (first <= UINT8_C(0x7f)) {
        scalar = first;
        ++offset;
        return true;
    }

    if (first >= UINT8_C(0xc2) && first <= UINT8_C(0xdf)) {
        if (text.size() - offset < 2U) {
            return false;
        }
        const auto second = static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[offset + 1U]));
        if (!is_continuation(second)) {
            return false;
        }
        scalar = (static_cast<std::uint32_t>(first & UINT8_C(0x1f)) << 6U) |
                 static_cast<std::uint32_t>(second & UINT8_C(0x3f));
        offset += 2U;
        return true;
    }

    if (first >= UINT8_C(0xe0) && first <= UINT8_C(0xef)) {
        if (text.size() - offset < 3U) {
            return false;
        }
        const auto second = static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[offset + 1U]));
        const auto third = static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[offset + 2U]));
        const bool valid_second =
            (first == UINT8_C(0xe0))
                ? (second >= UINT8_C(0xa0) && second <= UINT8_C(0xbf))
                : (first == UINT8_C(0xed))
                      ? (second >= UINT8_C(0x80) &&
                         second <= UINT8_C(0x9f))
                      : is_continuation(second);
        if (!valid_second || !is_continuation(third)) {
            return false;
        }
        scalar = (static_cast<std::uint32_t>(first & UINT8_C(0x0f)) << 12U) |
                 (static_cast<std::uint32_t>(second & UINT8_C(0x3f)) << 6U) |
                 static_cast<std::uint32_t>(third & UINT8_C(0x3f));
        offset += 3U;
        return true;
    }

    if (first >= UINT8_C(0xf0) && first <= UINT8_C(0xf4)) {
        if (text.size() - offset < 4U) {
            return false;
        }
        const auto second = static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[offset + 1U]));
        const auto third = static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[offset + 2U]));
        const auto fourth = static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[offset + 3U]));
        const bool valid_second =
            (first == UINT8_C(0xf0))
                ? (second >= UINT8_C(0x90) && second <= UINT8_C(0xbf))
                : (first == UINT8_C(0xf4))
                      ? (second >= UINT8_C(0x80) &&
                         second <= UINT8_C(0x8f))
                      : is_continuation(second);
        if (!valid_second || !is_continuation(third) ||
            !is_continuation(fourth)) {
            return false;
        }
        scalar = (static_cast<std::uint32_t>(first & UINT8_C(0x07)) << 18U) |
                 (static_cast<std::uint32_t>(second & UINT8_C(0x3f)) << 12U) |
                 (static_cast<std::uint32_t>(third & UINT8_C(0x3f)) << 6U) |
                 static_cast<std::uint32_t>(fourth & UINT8_C(0x3f));
        offset += 4U;
        return true;
    }

    return false;
}

bool is_strict_utf8(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::uint32_t scalar = 0;
        if (!decode_utf8_scalar(text, offset, scalar)) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<std::uint16_t>> decoded_utf16(
    std::string_view text) {
    std::vector<std::uint16_t> units;
    units.reserve(text.size());

    std::size_t offset = 0;
    while (offset < text.size()) {
        std::uint32_t scalar = 0;
        if (!decode_utf8_scalar(text, offset, scalar)) {
            return std::nullopt;
        }
        if (scalar <= UINT32_C(0xffff)) {
            units.push_back(static_cast<std::uint16_t>(scalar));
        } else {
            const std::uint32_t supplementary = scalar - UINT32_C(0x10000);
            units.push_back(static_cast<std::uint16_t>(
                UINT32_C(0xd800) + (supplementary >> 10U)));
            units.push_back(static_cast<std::uint16_t>(
                UINT32_C(0xdc00) + (supplementary & UINT32_C(0x3ff))));
        }
    }
    return units;
}

std::optional<JcsFailure> serialize_string(std::string_view value,
                                           std::string& output) {
    if (!is_strict_utf8(value)) {
        return JcsFailure::invalid_utf8;
    }

    constexpr char hex_digits[] = "0123456789abcdef";
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<std::uint8_t>(
            static_cast<unsigned char>(character));
        switch (byte) {
        case UINT8_C(0x08):
            output += "\\b";
            break;
        case UINT8_C(0x09):
            output += "\\t";
            break;
        case UINT8_C(0x0a):
            output += "\\n";
            break;
        case UINT8_C(0x0c):
            output += "\\f";
            break;
        case UINT8_C(0x0d):
            output += "\\r";
            break;
        case UINT8_C(0x22):
            output += "\\\"";
            break;
        case UINT8_C(0x5c):
            output += "\\\\";
            break;
        default:
            if (byte <= UINT8_C(0x1f)) {
                output += "\\u00";
                output.push_back(hex_digits[byte >> 4U]);
                output.push_back(hex_digits[byte & UINT8_C(0x0f)]);
            } else {
                output.push_back(character);
            }
            break;
        }
    }
    output.push_back('"');
    return std::nullopt;
}

struct MemberReference final {
    const JsonValue::Member* member;
    std::vector<std::uint16_t> sort_key;
};

std::optional<JcsFailure> prepare_object(
    const JsonValue::Object& members,
    std::vector<MemberReference>& sorted) {
    sorted.reserve(members.size());
    for (const JsonValue::Member& member : members) {
        auto sort_key = decoded_utf16(member.first);
        if (!sort_key.has_value()) {
            return JcsFailure::invalid_utf8;
        }
        sorted.push_back(MemberReference{&member, std::move(*sort_key)});
    }

    std::sort(sorted.begin(), sorted.end(),
              [](const MemberReference& left, const MemberReference& right) {
                  return left.sort_key < right.sort_key;
              });
    for (std::size_t index = 1; index < sorted.size(); ++index) {
        if (sorted[index - 1U].sort_key == sorted[index].sort_key) {
            return JcsFailure::duplicate_member;
        }
    }
    return std::nullopt;
}

std::optional<JcsFailure> serialize_number(double value,
                                           std::string& output) {
    if (!std::isfinite(value)) {
        return JcsFailure::non_finite_number;
    }

    char buffer[128]{};
    double_conversion::StringBuilder builder(
        buffer, static_cast<int>(sizeof(buffer)));
    const bool converted =
        double_conversion::DoubleToStringConverter::EcmaScriptConverter()
            .ToShortest(value, &builder);
    if (!converted) {
        throw std::runtime_error("ECMAScript number conversion failed");
    }
    output += builder.Finalize();
    return std::nullopt;
}

enum class FrameKind {
    value,
    array,
    object,
};

struct SerializationFrame final {
    explicit SerializationFrame(const JsonValue& initial_value)
        : value(&initial_value) {}

    FrameKind kind{FrameKind::value};
    const JsonValue* value;
    const JsonValue::Array* array{nullptr};
    std::vector<MemberReference> object_members;
    std::size_t next{0U};
};

std::optional<JcsFailure> serialize_value(const JsonValue& value,
                                          std::string& output) {
    // Chunked frames keep one live frame per open container without the
    // doubling reallocation churn of a vector on deeply nested values.
    std::deque<SerializationFrame> frames;
    frames.emplace_back(value);

    while (!frames.empty()) {
        SerializationFrame& frame = frames.back();
        if (frame.kind == FrameKind::value) {
            const JsonValue::Storage& storage = frame.value->storage();
            if (std::holds_alternative<std::nullptr_t>(storage)) {
                output += "null";
                frames.pop_back();
                continue;
            }
            if (const auto* boolean = std::get_if<bool>(&storage)) {
                output += *boolean ? "true" : "false";
                frames.pop_back();
                continue;
            }
            if (const auto* number = std::get_if<double>(&storage)) {
                const auto failure = serialize_number(*number, output);
                frames.pop_back();
                if (failure.has_value()) {
                    return failure;
                }
                continue;
            }
            if (const auto* string = std::get_if<std::string>(&storage)) {
                const auto failure = serialize_string(*string, output);
                frames.pop_back();
                if (failure.has_value()) {
                    return failure;
                }
                continue;
            }
            if (const auto* array =
                    std::get_if<JsonValue::Array>(&storage)) {
                output.push_back('[');
                frame.kind = FrameKind::array;
                frame.value = nullptr;
                frame.array = array;
                continue;
            }

            const auto& object = std::get<JsonValue::Object>(storage);
            if (const auto failure =
                    prepare_object(object, frame.object_members)) {
                return failure;
            }
            output.push_back('{');
            frame.kind = FrameKind::object;
            frame.value = nullptr;
            continue;
        }

        if (frame.kind == FrameKind::array) {
            if (frame.next == frame.array->size()) {
                output.push_back(']');
                frames.pop_back();
                continue;
            }
            if (frame.next != 0U) {
                output.push_back(',');
            }
            const JsonValue* const child = &(*frame.array)[frame.next];
            ++frame.next;
            frames.emplace_back(*child);
            continue;
        }

        if (frame.next == frame.object_members.size()) {
            output.push_back('}');
            frames.pop_back();
            continue;
        }
        if (frame.next != 0U) {
            output.push_back(',');
        }
        const MemberReference& reference = frame.object_members[frame.next];
        ++frame.next;
        if (const auto failure =
                serialize_string(reference.member->first, output)) {
            return failure;
        }
        output.push_back(':');
        frames.emplace_back(reference.member->second);
    }

    return std::nullopt;
}

}  // namespace

std::variant<std::string, JcsFailure> jcs_serialize(const JsonValue& value) {
    std::string output;
    if (const auto failure = serialize_value(value, output)) {
        return *failure;
    }
    return output;
}

}  // namespace fastdb::payload::json
