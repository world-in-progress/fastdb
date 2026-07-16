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

    JsonValue() noexcept;
    JsonValue(std::nullptr_t) noexcept;
    explicit JsonValue(bool value);
    explicit JsonValue(double value);
    explicit JsonValue(std::string value);
    explicit JsonValue(const char* value);

    JsonValue(const JsonValue& other) noexcept;
    JsonValue(JsonValue&& other) noexcept;
    ~JsonValue();

    JsonValue& operator=(const JsonValue& other) noexcept;
    JsonValue& operator=(JsonValue&& other) noexcept;

    static JsonValue array(Array values);
    static JsonValue object(Object members);

    const Storage& storage() const noexcept;

private:
    struct Node;

    explicit JsonValue(Storage storage);

    static Node* retain_node(Node* node) noexcept;
    static void release_node(Node* node) noexcept;

    Node* node_{nullptr};
};

}  // namespace fastdb::payload::json
