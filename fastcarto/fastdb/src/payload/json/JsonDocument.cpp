#include "payload/json/JsonDocument.hpp"

#include "payload/json/JsonPointer.hpp"

#include <fastdb_payload.h>

#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastdb::payload::json {
namespace {

using error::Error;
using error::Result;

JsonValue object_details(JsonValue::Object members) {
    return JsonValue::object(std::move(members));
}

Error invalid_json(std::string reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_JSON, JsonPointer{}, "Invalid JSON source",
        object_details(
            {JsonValue::Member{"reason", JsonValue{std::move(reason)}}}));
}

Error invalid_number() {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_NUMBER, JsonPointer{},
        "JSON number is outside finite binary64 range",
        object_details({JsonValue::Member{"reason",
                                          JsonValue{"number_out_of_range"}}}));
}

Error allocation_failed() {
    return Error::from_details(FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
                               "FastDB payload allocation failed",
                               object_details({}));
}

Error resource_limit(const JsonPointer& path,
                     std::string message,
                     std::string kind,
                     std::uint64_t actual,
                     std::uint64_t limit) {
    return Error::from_details(
        FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, path, std::move(message),
        object_details({
            JsonValue::Member{"limit", JsonValue{static_cast<double>(limit)}},
            JsonValue::Member{"actual",
                              JsonValue{static_cast<double>(actual)}},
            JsonValue::Member{"kind", JsonValue{std::move(kind)}},
        }));
}

std::string parse_reason(yyjson_read_code code) {
    switch (code) {
    case YYJSON_READ_ERROR_INVALID_PARAMETER:
        return "invalid_parameter";
    case YYJSON_READ_ERROR_EMPTY_CONTENT:
        return "empty_content";
    case YYJSON_READ_ERROR_UNEXPECTED_CONTENT:
        return "unexpected_content";
    case YYJSON_READ_ERROR_UNEXPECTED_END:
        return "unexpected_end";
    case YYJSON_READ_ERROR_UNEXPECTED_CHARACTER:
        return "unexpected_character";
    case YYJSON_READ_ERROR_JSON_STRUCTURE:
        return "invalid_structure";
    case YYJSON_READ_ERROR_INVALID_NUMBER:
        return "invalid_number";
    case YYJSON_READ_ERROR_INVALID_STRING:
        return "invalid_string";
    case YYJSON_READ_ERROR_LITERAL:
        return "invalid_literal";
    default:
        return "parse_failure";
    }
}

struct AuditItem final {
    yyjson_val* value;
    JsonPointer path;
    std::uint64_t depth;
};

Result<void> audit_document(yyjson_val* root, const JsonParseLimits& limits) {
    std::vector<AuditItem> pending;
    pending.push_back(AuditItem{root, JsonPointer{}, UINT64_C(1)});
    std::uint64_t value_count = 0;

    while (!pending.empty()) {
        AuditItem item = std::move(pending.back());
        pending.pop_back();

        if (value_count == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::failure(resource_limit(
                item.path, "JSON value count exceeds configured limit",
                "json_values", value_count, limits.max_json_values));
        }
        ++value_count;
        if (value_count > limits.max_json_values) {
            return Result<void>::failure(resource_limit(
                item.path, "JSON value count exceeds configured limit",
                "json_values", value_count, limits.max_json_values));
        }
        if (item.depth > limits.max_nesting_depth) {
            return Result<void>::failure(resource_limit(
                item.path, "JSON nesting depth exceeds configured limit",
                "nesting_depth", item.depth,
                limits.max_nesting_depth));
        }

        std::vector<AuditItem> children;
        if (yyjson_is_arr(item.value)) {
            yyjson_arr_iter iterator = yyjson_arr_iter_with(item.value);
            std::uint64_t index = 0;
            while (yyjson_val* child = yyjson_arr_iter_next(&iterator)) {
                children.push_back(AuditItem{
                    child, item.path.append(index), item.depth + UINT64_C(1)});
                ++index;
            }
        } else if (yyjson_is_obj(item.value)) {
            std::map<std::string, std::uint64_t> first_indexes;
            yyjson_obj_iter iterator = yyjson_obj_iter_with(item.value);
            std::uint64_t member_index = 0;
            while (yyjson_val* key = yyjson_obj_iter_next(&iterator)) {
                const std::string name(yyjson_get_str(key), yyjson_get_len(key));
                const auto inserted =
                    first_indexes.emplace(name, member_index);
                if (!inserted.second) {
                    return Result<void>::failure(Error::from_details(
                        FDB_PAYLOAD_E_DUPLICATE_KEY,
                        item.path.append(std::string_view{name}),
                        "JSON object member name is duplicated",
                        object_details({
                            JsonValue::Member{
                                "second_member_index",
                                JsonValue{static_cast<double>(member_index)}},
                            JsonValue::Member{"key", JsonValue{name}},
                            JsonValue::Member{
                                "first_member_index",
                                JsonValue{static_cast<double>(
                                    inserted.first->second)}},
                        })));
                }
                children.push_back(AuditItem{
                    yyjson_obj_iter_get_val(key),
                    item.path.append(std::string_view{name}),
                    item.depth + UINT64_C(1)});
                ++member_index;
            }
        }

        for (auto iterator = children.rbegin(); iterator != children.rend();
             ++iterator) {
            pending.push_back(std::move(*iterator));
        }
    }
    return Result<void>::success();
}

JsonValue primitive_value(yyjson_val* value) {
    if (yyjson_is_null(value)) {
        return JsonValue{nullptr};
    }
    if (yyjson_is_bool(value)) {
        return JsonValue{yyjson_get_bool(value)};
    }
    if (yyjson_is_num(value)) {
        return JsonValue{yyjson_get_num(value)};
    }
    return JsonValue{
        std::string(yyjson_get_str(value), yyjson_get_len(value))};
}

}  // namespace

JsonCursor::JsonCursor(yyjson_val* value) noexcept : value_(value) {}

bool JsonCursor::is_null() const noexcept { return yyjson_is_null(value_); }
bool JsonCursor::is_boolean() const noexcept { return yyjson_is_bool(value_); }
bool JsonCursor::is_number() const noexcept { return yyjson_is_num(value_); }
bool JsonCursor::is_string() const noexcept { return yyjson_is_str(value_); }
bool JsonCursor::is_array() const noexcept { return yyjson_is_arr(value_); }
bool JsonCursor::is_object() const noexcept { return yyjson_is_obj(value_); }

bool JsonCursor::boolean() const noexcept { return yyjson_get_bool(value_); }
double JsonCursor::number() const noexcept { return yyjson_get_num(value_); }

std::string_view JsonCursor::string() const noexcept {
    if (!yyjson_is_str(value_)) {
        return {};
    }
    return std::string_view(yyjson_get_str(value_), yyjson_get_len(value_));
}

std::uint64_t JsonCursor::size() const noexcept {
    return static_cast<std::uint64_t>(yyjson_get_len(value_));
}

JsonArrayCursor JsonCursor::elements() const noexcept {
    const yyjson_arr_iter iterator = yyjson_arr_iter_with(value_);
    return JsonArrayCursor(iterator.cur,
                           static_cast<std::uint64_t>(iterator.max));
}

JsonObjectCursor JsonCursor::members() const noexcept {
    const yyjson_obj_iter iterator = yyjson_obj_iter_with(value_);
    return JsonObjectCursor(iterator.cur,
                            static_cast<std::uint64_t>(iterator.max));
}

JsonArrayCursor::JsonArrayCursor(yyjson_val* next_value,
                                 std::uint64_t remaining) noexcept
    : next_value_(next_value), remaining_(remaining) {}

bool JsonArrayCursor::next(JsonCursor& value) noexcept {
    if (remaining_ == 0U || next_value_ == nullptr) {
        value = JsonCursor{};
        return false;
    }
    yyjson_val* const current = next_value_;
    --remaining_;
    next_value_ = remaining_ == 0U ? nullptr : unsafe_yyjson_get_next(current);
    value = JsonCursor(current);
    return true;
}

JsonObjectCursor::JsonObjectCursor(yyjson_val* next_key,
                                   std::uint64_t remaining) noexcept
    : next_key_(next_key), remaining_(remaining) {}

bool JsonObjectCursor::next(std::string_view& name,
                            JsonCursor& value) noexcept {
    if (remaining_ == 0U || next_key_ == nullptr) {
        name = {};
        value = JsonCursor{};
        return false;
    }
    yyjson_val* const key = next_key_;
    yyjson_val* const member_value = yyjson_obj_iter_get_val(key);
    --remaining_;
    next_key_ = remaining_ == 0U
                    ? nullptr
                    : unsafe_yyjson_get_next(member_value);
    name = std::string_view(yyjson_get_str(key), yyjson_get_len(key));
    value = JsonCursor(member_value);
    return true;
}

Result<JsonDocument> JsonDocument::parse(const std::uint8_t* source,
                                         std::uint64_t source_size,
                                         JsonParseLimits limits) {
    if (source_size > limits.max_source_bytes) {
        return Result<JsonDocument>::failure(resource_limit(
            JsonPointer{}, "JSON source exceeds configured limit",
            "source_bytes", source_size, limits.max_source_bytes));
    }
    if (source_size > std::numeric_limits<std::size_t>::max()) {
        return Result<JsonDocument>::failure(resource_limit(
            JsonPointer{}, "JSON source exceeds platform address limit",
            "source_bytes", source_size,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())));
    }
    if (source_size == 0U) {
        return Result<JsonDocument>::failure(invalid_json("empty_content"));
    }
    if (source == nullptr && source_size != 0U) {
        return Result<JsonDocument>::failure(invalid_json("invalid_parameter"));
    }

    yyjson_read_err read_error{};
    yyjson_doc* const document = yyjson_read_opts(
        const_cast<char*>(reinterpret_cast<const char*>(source)),
        static_cast<std::size_t>(source_size), YYJSON_READ_NOFLAG, nullptr,
        &read_error);
    if (document == nullptr) {
        if (read_error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) {
            return Result<JsonDocument>::failure(allocation_failed());
        }
        if (read_error.code == YYJSON_READ_ERROR_INVALID_NUMBER &&
            read_error.msg != nullptr &&
            std::string_view(read_error.msg) ==
                "number is infinity when parsed as double") {
            return Result<JsonDocument>::failure(invalid_number());
        }
        return Result<JsonDocument>::failure(
            invalid_json(parse_reason(read_error.code)));
    }

    auto audit = audit_document(yyjson_doc_get_root(document), limits);
    if (!audit.has_value()) {
        yyjson_doc_free(document);
        return Result<JsonDocument>::failure(std::move(audit).error());
    }
    return Result<JsonDocument>::success(JsonDocument(document));
}

JsonDocument::JsonDocument(yyjson_doc* document) noexcept
    : document_(document) {}

JsonDocument::JsonDocument(JsonDocument&& other) noexcept
    : document_(std::exchange(other.document_, nullptr)) {}

JsonDocument& JsonDocument::operator=(JsonDocument&& other) noexcept {
    if (this != &other) {
        yyjson_doc_free(document_);
        document_ = std::exchange(other.document_, nullptr);
    }
    return *this;
}

JsonDocument::~JsonDocument() { yyjson_doc_free(document_); }

JsonCursor JsonDocument::root() const noexcept {
    return JsonCursor(document_ == nullptr ? nullptr
                                           : yyjson_doc_get_root(document_));
}

JsonValue JsonDocument::to_json_value() const {
    yyjson_val* const root_value =
        document_ == nullptr ? nullptr : yyjson_doc_get_root(document_);
    if (root_value == nullptr) {
        return JsonValue{nullptr};
    }

    struct ConvertItem final {
        yyjson_val* value;
        bool expanded;
    };
    std::vector<ConvertItem> pending;
    pending.push_back(ConvertItem{root_value, false});
    std::unordered_map<yyjson_val*, JsonValue> converted;

    while (!pending.empty()) {
        const ConvertItem item = pending.back();
        pending.pop_back();

        if (!yyjson_is_ctn(item.value)) {
            converted.emplace(item.value, primitive_value(item.value));
            continue;
        }
        if (!item.expanded) {
            pending.push_back(ConvertItem{item.value, true});
            if (yyjson_is_arr(item.value)) {
                std::vector<yyjson_val*> children;
                yyjson_arr_iter iterator = yyjson_arr_iter_with(item.value);
                while (yyjson_val* child = yyjson_arr_iter_next(&iterator)) {
                    children.push_back(child);
                }
                for (auto iterator = children.rbegin();
                     iterator != children.rend(); ++iterator) {
                    pending.push_back(ConvertItem{*iterator, false});
                }
            } else {
                std::vector<yyjson_val*> children;
                yyjson_obj_iter iterator = yyjson_obj_iter_with(item.value);
                while (yyjson_val* key = yyjson_obj_iter_next(&iterator)) {
                    children.push_back(yyjson_obj_iter_get_val(key));
                }
                for (auto iterator = children.rbegin();
                     iterator != children.rend(); ++iterator) {
                    pending.push_back(ConvertItem{*iterator, false});
                }
            }
            continue;
        }

        if (yyjson_is_arr(item.value)) {
            JsonValue::Array values;
            values.reserve(yyjson_arr_size(item.value));
            yyjson_arr_iter iterator = yyjson_arr_iter_with(item.value);
            while (yyjson_val* child = yyjson_arr_iter_next(&iterator)) {
                auto converted_child = converted.find(child);
                values.push_back(std::move(converted_child->second));
                converted.erase(converted_child);
            }
            converted.emplace(item.value, JsonValue::array(std::move(values)));
        } else {
            JsonValue::Object members;
            members.reserve(yyjson_obj_size(item.value));
            yyjson_obj_iter iterator = yyjson_obj_iter_with(item.value);
            while (yyjson_val* key = yyjson_obj_iter_next(&iterator)) {
                yyjson_val* const child = yyjson_obj_iter_get_val(key);
                auto converted_child = converted.find(child);
                members.emplace_back(
                    std::string(yyjson_get_str(key), yyjson_get_len(key)),
                    std::move(converted_child->second));
                converted.erase(converted_child);
            }
            converted.emplace(item.value,
                              JsonValue::object(std::move(members)));
        }
    }
    return std::move(converted.find(root_value)->second);
}

}  // namespace fastdb::payload::json
