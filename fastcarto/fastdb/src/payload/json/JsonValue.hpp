#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::payload::json {

class JsonValue final {
public:
    using Array = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;
    using Storage =
        std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    JsonValue() noexcept : storage_(nullptr) {}
    JsonValue(std::nullptr_t) noexcept : storage_(nullptr) {}
    explicit JsonValue(bool value) noexcept : storage_(value) {}
    explicit JsonValue(double value) noexcept : storage_(value) {}
    explicit JsonValue(std::string value) : storage_(std::move(value)) {}
    explicit JsonValue(const char* value) : storage_(std::string(value)) {}

    static JsonValue array(Array values) {
        return JsonValue(Storage(std::in_place_type<Array>, std::move(values)));
    }

    static JsonValue object(Object members) {
        return JsonValue(
            Storage(std::in_place_type<Object>, std::move(members)));
    }

    const Storage& storage() const noexcept { return storage_; }

private:
    explicit JsonValue(Storage storage) : storage_(std::move(storage)) {}

    Storage storage_;
};

}  // namespace fastdb::payload::json
