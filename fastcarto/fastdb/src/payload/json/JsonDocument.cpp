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

Error invalid_number(std::string reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_NUMBER, JsonPointer{},
        "JSON number is outside finite binary64 range",
        object_details(
            {JsonValue::Member{"reason", JsonValue{std::move(reason)}}}));
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
            JsonValue::Member{"limit", JsonValue{std::to_string(limit)}},
            JsonValue::Member{"actual", JsonValue{std::to_string(actual)}},
            JsonValue::Member{"kind", JsonValue{std::move(kind)}},
        }));
}

bool is_json_whitespace(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(' ') ||
           value == static_cast<std::uint8_t>('\t') ||
           value == static_cast<std::uint8_t>('\n') ||
           value == static_cast<std::uint8_t>('\r');
}

bool has_value_boundary_before(const std::uint8_t* source,
                               std::uint64_t start) noexcept {
    if (start == 0U) {
        return true;
    }
    const std::uint8_t previous = source[start - UINT64_C(1)];
    return is_json_whitespace(previous) ||
           previous == static_cast<std::uint8_t>('[') ||
           previous == static_cast<std::uint8_t>(',') ||
           previous == static_cast<std::uint8_t>(':');
}

bool has_value_boundary_after(const std::uint8_t* source,
                              std::uint64_t source_size,
                              std::uint64_t end) noexcept {
    if (end == source_size) {
        return true;
    }
    const std::uint8_t next = source[end];
    return is_json_whitespace(next) ||
           next == static_cast<std::uint8_t>(',') ||
           next == static_cast<std::uint8_t>(']') ||
           next == static_cast<std::uint8_t>('}');
}

bool matches_at(const std::uint8_t* source,
                std::uint64_t source_size,
                std::uint64_t start,
                std::string_view token) noexcept {
    if (start > source_size ||
        token.size() > source_size - start) {
        return false;
    }
    for (std::size_t offset = 0; offset < token.size(); ++offset) {
        if (source[start + static_cast<std::uint64_t>(offset)] !=
            static_cast<std::uint8_t>(token[offset])) {
            return false;
        }
    }
    const std::uint64_t end =
        start + static_cast<std::uint64_t>(token.size());
    return has_value_boundary_before(source, start) &&
           has_value_boundary_after(source, source_size, end);
}

bool is_symbolic_non_finite_value_failure(
    const std::uint8_t* source,
    std::uint64_t source_size,
    yyjson_read_code code,
    const char* message,
    std::uint64_t failure_position) noexcept {
    if (source == nullptr || failure_position > source_size) {
        return false;
    }
    const std::string_view error_message =
        message == nullptr ? std::string_view{} : std::string_view{message};
    // yyjson 0.12.0 distinguishes a value position from key/trailing syntax
    // through these pinned code/message pairs.
    const bool value_context =
        (code == YYJSON_READ_ERROR_UNEXPECTED_CHARACTER &&
         error_message == "unexpected character, expected a JSON value") ||
        (code == YYJSON_READ_ERROR_INVALID_NUMBER &&
         error_message == "no digit after sign");
    if (!value_context) {
        return false;
    }
    constexpr std::uint64_t maximum_token_size = UINT64_C(9);
    const std::uint64_t first_candidate =
        failure_position > maximum_token_size
            ? failure_position - maximum_token_size
            : UINT64_C(0);
    for (std::uint64_t start = first_candidate; start <= failure_position;
         ++start) {
        for (const std::string_view token : {
                 std::string_view{"NaN"},
                 std::string_view{"Infinity"},
                 std::string_view{"-Infinity"},
             }) {
            const std::uint64_t end =
                start + static_cast<std::uint64_t>(token.size());
            if (failure_position >= start && failure_position <= end &&
                matches_at(source, source_size, start, token)) {
                return true;
            }
        }
        if (start == std::numeric_limits<std::uint64_t>::max()) {
            break;
        }
    }
    return false;
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

struct AuditFrame final {
    AuditFrame(JsonPointerBuilder::Mark frame_path_mark,
               std::uint64_t frame_depth,
               bool frame_is_object)
        : path_mark(frame_path_mark),
          depth(frame_depth),
          is_object(frame_is_object) {}

    JsonPointerBuilder::Mark path_mark;
    std::uint64_t depth;
    bool is_object;
    yyjson_arr_iter array_iterator{};
    yyjson_obj_iter object_iterator{};
    std::uint64_t next_index{0};
    std::map<std::string, std::uint64_t> first_indexes;
};

AuditFrame make_audit_frame(yyjson_val* value,
                            JsonPointerBuilder::Mark path_mark,
                            std::uint64_t depth) {
    AuditFrame frame(path_mark, depth, yyjson_is_obj(value));
    if (frame.is_object) {
        frame.object_iterator = yyjson_obj_iter_with(value);
    } else {
        frame.array_iterator = yyjson_arr_iter_with(value);
    }
    return frame;
}

Result<void> check_audit_value(const JsonPointerBuilder& path,
                               std::uint64_t depth,
                               const JsonParseLimits& limits,
                               std::uint64_t& value_count) {
    ++value_count;
    if (value_count > limits.max_json_values) {
        return Result<void>::failure(resource_limit(
            path.snapshot(), "JSON value count exceeds configured limit",
            "json_values", value_count, limits.max_json_values));
    }
    if (depth > limits.max_nesting_depth) {
        return Result<void>::failure(resource_limit(
            path.snapshot(), "JSON nesting depth exceeds configured limit",
            "nesting_depth", depth, limits.max_nesting_depth));
    }
    return Result<void>::success();
}

Result<void> audit_document(yyjson_val* root, const JsonParseLimits& limits) {
    std::uint64_t value_count = 0;
    JsonPointerBuilder path;
    auto root_check =
        check_audit_value(path, UINT64_C(1), limits, value_count);
    if (!root_check.has_value()) {
        return root_check;
    }

    std::vector<AuditFrame> pending;
    if (yyjson_is_ctn(root)) {
        pending.push_back(make_audit_frame(root, path.mark(), UINT64_C(1)));
    }
    while (!pending.empty()) {
        AuditFrame& frame = pending.back();
        path.rewind(frame.path_mark);
        yyjson_val* child = nullptr;
        if (frame.is_object) {
            yyjson_val* const key =
                yyjson_obj_iter_next(&frame.object_iterator);
            if (key == nullptr) {
                pending.pop_back();
                continue;
            }
            const std::string name(yyjson_get_str(key), yyjson_get_len(key));
            const std::uint64_t member_index = frame.next_index++;
            path.append(std::string_view{name});
            const auto inserted =
                frame.first_indexes.emplace(name, member_index);
            if (!inserted.second) {
                return Result<void>::failure(Error::from_details(
                    FDB_PAYLOAD_E_DUPLICATE_KEY,
                    path.snapshot(),
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
            child = yyjson_obj_iter_get_val(key);
        } else {
            child = yyjson_arr_iter_next(&frame.array_iterator);
            if (child == nullptr) {
                pending.pop_back();
                continue;
            }
            path.append(frame.next_index++);
        }

        const std::uint64_t child_depth = frame.depth + UINT64_C(1);
        auto child_check = check_audit_value(path, child_depth, limits,
                                             value_count);
        if (!child_check.has_value()) {
            return child_check;
        }
        if (yyjson_is_ctn(child)) {
            pending.push_back(
                make_audit_frame(child, path.mark(), child_depth));
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
        if (is_symbolic_non_finite_value_failure(
                source, source_size, read_error.code, read_error.msg,
                static_cast<std::uint64_t>(read_error.pos))) {
            return Result<JsonDocument>::failure(
                invalid_number("symbolic_non_finite"));
        }
        if (read_error.code == YYJSON_READ_ERROR_INVALID_NUMBER &&
            read_error.msg != nullptr &&
            std::string_view(read_error.msg) ==
                "number is infinity when parsed as double") {
            return Result<JsonDocument>::failure(
                invalid_number("number_out_of_range"));
        }
        return Result<JsonDocument>::failure(
            invalid_json(parse_reason(read_error.code)));
    }

    JsonDocument parsed(document);
    auto audit = audit_document(parsed.root().value_, limits);
    if (!audit.has_value()) {
        return Result<JsonDocument>::failure(std::move(audit).error());
    }
    return Result<JsonDocument>::success(std::move(parsed));
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
