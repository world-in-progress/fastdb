#pragma once

#include <cstddef>
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
    friend class JsonPointerBuilder;

    explicit JsonPointer(std::string value);

    std::string value_;
};

class JsonPointerBuilder final {
public:
    using Mark = std::size_t;

    Mark mark() const noexcept;
    void rewind(Mark mark) noexcept;
    void append(std::string_view token);
    void append(std::uint64_t index);
    JsonPointer snapshot() const;

private:
    std::string value_;
};

}  // namespace fastdb::payload::json
