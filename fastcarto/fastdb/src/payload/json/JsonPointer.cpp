#include "payload/json/JsonPointer.hpp"

#include <cstddef>
#include <utility>

namespace fastdb::payload::json {
namespace {

void append_token(std::string& output, std::string_view token) {
    output.push_back('/');
    for (const char character : token) {
        if (character == '~') {
            output += "~0";
        } else if (character == '/') {
            output += "~1";
        } else {
            output.push_back(character);
        }
    }
}

void append_index(std::string& output, std::uint64_t index) {
    char digits[20]{};
    std::size_t position = sizeof(digits);
    do {
        const auto digit = static_cast<int>(index % UINT64_C(10));
        digits[--position] = static_cast<char>('0' + digit);
        index /= UINT64_C(10);
    } while (index != 0);

    output.push_back('/');
    output.append(digits + position, sizeof(digits) - position);
}

}  // namespace

JsonPointer::JsonPointer(std::string value) : value_(std::move(value)) {}

const std::string& JsonPointer::value() const noexcept { return value_; }

JsonPointer JsonPointer::append(std::string_view token) const {
    std::string appended = value_;
    append_token(appended, token);
    return JsonPointer(std::move(appended));
}

JsonPointer JsonPointer::append(std::uint64_t index) const {
    std::string appended = value_;
    append_index(appended, index);
    return JsonPointer(std::move(appended));
}

JsonPointerBuilder::Mark JsonPointerBuilder::mark() const noexcept {
    return value_.size();
}

void JsonPointerBuilder::rewind(Mark path_mark) noexcept {
    if (path_mark <= value_.size()) {
        value_.resize(path_mark);
    }
}

void JsonPointerBuilder::append(std::string_view token) {
    append_token(value_, token);
}

void JsonPointerBuilder::append(std::uint64_t index) {
    append_index(value_, index);
}

JsonPointer JsonPointerBuilder::snapshot() const {
    return JsonPointer(value_);
}

}  // namespace fastdb::payload::json
