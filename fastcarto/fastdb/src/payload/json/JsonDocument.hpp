#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonValue.hpp"

#include <cstdint>
#include <string_view>

struct yyjson_doc;
struct yyjson_val;

namespace fastdb::payload::json {

struct JsonParseLimits final {
    std::uint64_t max_source_bytes{UINT64_C(16777216)};
    std::uint64_t max_json_values{UINT64_C(1000000)};
    std::uint32_t max_nesting_depth{UINT32_C(128)};
};

class JsonArrayCursor;
class JsonObjectCursor;

class JsonCursor final {
public:
    JsonCursor() = default;

    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    bool boolean() const noexcept;
    double number() const noexcept;
    std::string_view string() const noexcept;
    std::uint64_t size() const noexcept;
    JsonArrayCursor elements() const noexcept;
    JsonObjectCursor members() const noexcept;

private:
    friend class JsonDocument;
    friend class JsonArrayCursor;
    friend class JsonObjectCursor;

    explicit JsonCursor(yyjson_val* value) noexcept;

    yyjson_val* value_{nullptr};
};

class JsonArrayCursor final {
public:
    bool next(JsonCursor& value) noexcept;

private:
    friend class JsonCursor;

    JsonArrayCursor(yyjson_val* next_value, std::uint64_t remaining) noexcept;

    yyjson_val* next_value_{nullptr};
    std::uint64_t remaining_{0};
};

class JsonObjectCursor final {
public:
    bool next(std::string_view& name, JsonCursor& value) noexcept;

private:
    friend class JsonCursor;

    JsonObjectCursor(yyjson_val* next_key, std::uint64_t remaining) noexcept;

    yyjson_val* next_key_{nullptr};
    std::uint64_t remaining_{0};
};

class JsonDocument final {
public:
    static error::Result<JsonDocument> parse(
        const std::uint8_t* source,
        std::uint64_t source_size,
        JsonParseLimits limits = {});

    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;
    JsonDocument(JsonDocument&& other) noexcept;
    JsonDocument& operator=(JsonDocument&& other) noexcept;
    ~JsonDocument();

    JsonCursor root() const noexcept;
    JsonValue to_json_value() const;

private:
    explicit JsonDocument(yyjson_doc* document) noexcept;

    yyjson_doc* document_{nullptr};
};

}  // namespace fastdb::payload::json
