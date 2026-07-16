#include "payload/json/JsonPointer.hpp"

#include <cstddef>
#include <utility>

namespace fastdb::payload::json {

JsonPointer::JsonPointer(std::string value) : value_(std::move(value)) {}

const std::string& JsonPointer::value() const noexcept { return value_; }

JsonPointer JsonPointer::append(std::string_view token) const {
    std::string appended = value_;
    appended.push_back('/');
    for (const char character : token) {
        if (character == '~') {
            appended += "~0";
        } else if (character == '/') {
            appended += "~1";
        } else {
            appended.push_back(character);
        }
    }
    return JsonPointer(std::move(appended));
}

JsonPointer JsonPointer::append(std::uint64_t index) const {
    char digits[20]{};
    std::size_t position = sizeof(digits);
    do {
        const auto digit = static_cast<int>(index % UINT64_C(10));
        digits[--position] = static_cast<char>('0' + digit);
        index /= UINT64_C(10);
    } while (index != 0);

    std::string appended = value_;
    appended.push_back('/');
    appended.append(digits + position, sizeof(digits) - position);
    return JsonPointer(std::move(appended));
}

}  // namespace fastdb::payload::json
