#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fastdb::payload::json {

class JsonPointer final {
public:
    JsonPointer() = default;

    const std::string& value() const noexcept;
    JsonPointer append(std::string_view token) const;
    JsonPointer append(std::uint64_t index) const;

private:
    explicit JsonPointer(std::string value);

    std::string value_;
};

}  // namespace fastdb::payload::json
