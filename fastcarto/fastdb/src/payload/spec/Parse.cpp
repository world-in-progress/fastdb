#include "payload/spec/Parse.hpp"

#include "payload/json/JsonPointer.hpp"

#include <fastdb_payload.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastdb::payload::spec {
namespace {

using error::Error;
using error::Result;
using json::JsonCursor;
using json::JsonPointer;
using json::JsonPointerBuilder;
using json::JsonValue;

struct MemberSlot final {
    bool present{false};
    JsonCursor value;
};

JsonPointer member_path(JsonPointerBuilder& path, std::string_view member) {
    const JsonPointerBuilder::Mark mark = path.mark();
    path.append(member);
    JsonPointer snapshot = path.snapshot();
    path.rewind(mark);
    return snapshot;
}

JsonValue details(JsonValue::Object members) {
    return JsonValue::object(std::move(members));
}

std::string_view json_type_name(const JsonCursor& value) noexcept {
    if (value.is_null()) {
        return "null";
    }
    if (value.is_boolean()) {
        return "boolean";
    }
    if (value.is_number()) {
        return "number";
    }
    if (value.is_string()) {
        return "string";
    }
    if (value.is_array()) {
        return "array";
    }
    if (value.is_object()) {
        return "object";
    }
    return "invalid";
}

Error unknown_field(const JsonPointer& path, std::string_view field) {
    return Error::from_details(
        FDB_PAYLOAD_E_UNKNOWN_FIELD, path,
        "Payload object contains unknown field",
        details({
            JsonValue::Member{"reason", JsonValue{"unknown_field"}},
            JsonValue::Member{"field", JsonValue{std::string(field)}},
        }));
}

Error missing_field(const JsonPointer& object_path, std::string_view field) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE, object_path.append(field),
        "Payload object is missing required field",
        details({
            JsonValue::Member{"reason", JsonValue{"missing_field"}},
            JsonValue::Member{"field", JsonValue{std::string(field)}},
        }));
}

Error invalid_field_type(const JsonPointer& path,
                         std::string_view expected,
                         const JsonCursor& actual) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE, path,
        "Payload field has invalid JSON type",
        details({
            JsonValue::Member{"reason", JsonValue{"invalid_field_type"}},
            JsonValue::Member{"expected", JsonValue{std::string(expected)}},
            JsonValue::Member{
                "actual", JsonValue{std::string(json_type_name(actual))}},
        }));
}

Error unsupported_schema(const JsonPointer& path, std::string_view actual) {
    return Error::from_details(
        FDB_PAYLOAD_E_UNSUPPORTED_SCHEMA, path,
        "Payload schema version is unsupported",
        details({
            JsonValue::Member{"expected", JsonValue{"fastdb.payload.v1"}},
            JsonValue::Member{"actual", JsonValue{std::string(actual)}},
        }));
}

Error invalid_profile(const JsonPointer& path, std::string_view actual) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE, path, "Payload profile is invalid",
        details({
            JsonValue::Member{"reason", JsonValue{"invalid_profile"}},
            JsonValue::Member{"actual", JsonValue{std::string(actual)}},
        }));
}

Error invalid_cardinality(const JsonPointer& path,
                          std::string_view actual) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE, path,
        "Payload cardinality is invalid",
        details({
            JsonValue::Member{"reason", JsonValue{"invalid_cardinality"}},
            JsonValue::Member{"actual", JsonValue{std::string(actual)}},
        }));
}

Error invalid_component_kind(const JsonPointer& path,
                             std::string_view actual) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE, path,
        "Payload component kind is invalid",
        details({
            JsonValue::Member{"reason",
                              JsonValue{"invalid_component_kind"}},
            JsonValue::Member{"actual", JsonValue{std::string(actual)}},
        }));
}

Error invalid_type_kind(const JsonPointer& path, std::string_view actual) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE, path, "Payload type kind is invalid",
        details({
            JsonValue::Member{"reason", JsonValue{"invalid_type_kind"}},
            JsonValue::Member{"actual", JsonValue{std::string(actual)}},
        }));
}

Error invalid_identifier(const JsonPointer& path, std::string_view value) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE, path, "Payload identifier is invalid",
        details({
            JsonValue::Member{"value", JsonValue{std::string(value)}},
            JsonValue::Member{"reason", JsonValue{"invalid_id"}},
        }));
}

Error invalid_range(const JsonPointer& path,
                    double minimum,
                    double maximum,
                    std::string reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_NUMBER, path,
        "Normalized integer bounds are invalid",
        details({
            JsonValue::Member{"reason", JsonValue{std::move(reason)}},
            JsonValue::Member{"min", JsonValue{minimum}},
            JsonValue::Member{"max", JsonValue{maximum}},
        }));
}

Error resource_limit(const JsonPointer& path,
                     std::string message,
                     std::string kind,
                     std::uint64_t actual,
                     std::uint64_t limit) {
    return Error::from_details(
        FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, path, std::move(message),
        details({
            JsonValue::Member{"limit", JsonValue{std::to_string(limit)}},
            JsonValue::Member{"actual", JsonValue{std::to_string(actual)}},
            JsonValue::Member{"kind", JsonValue{std::move(kind)}},
        }));
}

bool is_identifier(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto is_ascii_letter = [](unsigned char byte) noexcept {
        return (byte >= static_cast<unsigned char>('A') &&
                byte <= static_cast<unsigned char>('Z')) ||
               (byte >= static_cast<unsigned char>('a') &&
                byte <= static_cast<unsigned char>('z'));
    };
    const unsigned char first = static_cast<unsigned char>(value.front());
    if (!is_ascii_letter(first) &&
        first != static_cast<unsigned char>('_')) {
        return false;
    }
    for (std::size_t index = 1; index < value.size(); ++index) {
        const unsigned char byte =
            static_cast<unsigned char>(value[index]);
        if (!is_ascii_letter(byte) &&
            !(byte >= static_cast<unsigned char>('0') &&
              byte <= static_cast<unsigned char>('9')) &&
            byte != static_cast<unsigned char>('_')) {
            return false;
        }
    }
    return true;
}

Result<std::string> parse_identifier(const JsonCursor& value,
                                     const JsonPointerBuilder& path) {
    if (!value.is_string()) {
        return Result<std::string>::failure(
            invalid_field_type(path.snapshot(), "string", value));
    }
    const std::string_view source = value.string();
    if (!is_identifier(source)) {
        return Result<std::string>::failure(
            invalid_identifier(path.snapshot(), source));
    }
    return Result<std::string>::success(std::string(source));
}

Result<bool> parse_nullable(const MemberSlot& nullable,
                            JsonPointerBuilder& type_path) {
    if (!nullable.present) {
        return Result<bool>::success(false);
    }
    if (!nullable.value.is_boolean()) {
        return Result<bool>::failure(invalid_field_type(
            member_path(type_path, "nullable"), "boolean", nullable.value));
    }
    return Result<bool>::success(nullable.value.boolean());
}

std::optional<TypeKind> type_kind_from_name(std::string_view name) noexcept {
    if (name == "bool") {
        return TypeKind::boolean;
    }
    if (name == "u8") {
        return TypeKind::u8;
    }
    if (name == "u16") {
        return TypeKind::u16;
    }
    if (name == "u32") {
        return TypeKind::u32;
    }
    if (name == "i32") {
        return TypeKind::i32;
    }
    if (name == "u8n") {
        return TypeKind::u8n;
    }
    if (name == "u16n") {
        return TypeKind::u16n;
    }
    if (name == "f32") {
        return TypeKind::f32;
    }
    if (name == "f64") {
        return TypeKind::f64;
    }
    if (name == "str") {
        return TypeKind::str;
    }
    if (name == "wstr") {
        return TypeKind::wstr;
    }
    if (name == "bytes") {
        return TypeKind::bytes;
    }
    if (name == "component") {
        return TypeKind::component;
    }
    if (name == "ref") {
        return TypeKind::ref;
    }
    if (name == "list") {
        return TypeKind::list;
    }
    return std::nullopt;
}

bool is_normalized_integer(TypeKind kind) noexcept {
    return kind == TypeKind::u8n || kind == TypeKind::u16n;
}

bool is_plain_scalar(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::f32:
    case TypeKind::f64:
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
        return true;
    case TypeKind::u8n:
    case TypeKind::u16n:
    case TypeKind::component:
    case TypeKind::ref:
    case TypeKind::list:
        return false;
    }
    return false;
}

bool type_member_allowed(TypeKind kind, std::string_view name) noexcept {
    if (name == "kind" || name == "nullable") {
        return true;
    }
    if (is_normalized_integer(kind)) {
        return name == "min" || name == "max";
    }
    if (kind == TypeKind::component) {
        return name == "id";
    }
    if (kind == TypeKind::ref) {
        return name == "target";
    }
    if (kind == TypeKind::list) {
        return name == "items";
    }
    return false;
}

struct ParsedTypeLevel final {
    explicit ParsedTypeLevel(TypeNode parsed_node)
        : node(std::move(parsed_node)) {}

    ParsedTypeLevel(TypeNode parsed_node, JsonCursor parsed_items)
        : node(std::move(parsed_node)),
          has_items(true),
          items(parsed_items) {}

    ParsedTypeLevel(const ParsedTypeLevel&) = delete;
    ParsedTypeLevel& operator=(const ParsedTypeLevel&) = delete;
    ParsedTypeLevel(ParsedTypeLevel&&) noexcept = default;
    ParsedTypeLevel& operator=(ParsedTypeLevel&&) noexcept = default;
    ~ParsedTypeLevel() = default;

    TypeNode node;
    bool has_items{false};
    JsonCursor items;
};

Result<ParsedTypeLevel> parse_type_level(const JsonCursor& value,
                                         JsonPointerBuilder& path) {
    if (!value.is_object()) {
        return Result<ParsedTypeLevel>::failure(
            invalid_field_type(path.snapshot(), "object", value));
    }

    MemberSlot kind_member;
    auto first_pass = value.members();
    std::string_view name;
    JsonCursor member_value;
    while (first_pass.next(name, member_value)) {
        if (name == "kind") {
            kind_member = MemberSlot{true, member_value};
        }
    }
    if (!kind_member.present) {
        return Result<ParsedTypeLevel>::failure(
            missing_field(path.snapshot(), "kind"));
    }
    if (!kind_member.value.is_string()) {
        return Result<ParsedTypeLevel>::failure(invalid_field_type(
            member_path(path, "kind"), "string", kind_member.value));
    }
    const std::string_view kind_name = kind_member.value.string();
    const std::optional<TypeKind> parsed_kind =
        type_kind_from_name(kind_name);
    if (!parsed_kind.has_value()) {
        return Result<ParsedTypeLevel>::failure(
            invalid_type_kind(member_path(path, "kind"), kind_name));
    }
    const TypeKind kind = *parsed_kind;

    MemberSlot nullable;
    MemberSlot minimum;
    MemberSlot maximum;
    MemberSlot source_id;
    MemberSlot items;
    auto members = value.members();
    while (members.next(name, member_value)) {
        if (!type_member_allowed(kind, name)) {
            return Result<ParsedTypeLevel>::failure(
                unknown_field(member_path(path, name), name));
        }
        if (name == "nullable") {
            nullable = MemberSlot{true, member_value};
        } else if (name == "min") {
            minimum = MemberSlot{true, member_value};
        } else if (name == "max") {
            maximum = MemberSlot{true, member_value};
        } else if (name == "id" || name == "target") {
            source_id = MemberSlot{true, member_value};
        } else if (name == "items") {
            items = MemberSlot{true, member_value};
        }
    }

    auto parsed_nullable = parse_nullable(nullable, path);
    if (!parsed_nullable.has_value()) {
        return Result<ParsedTypeLevel>::failure(
            std::move(parsed_nullable).error());
    }
    TypeNode node(kind, parsed_nullable.value());

    if (is_plain_scalar(kind)) {
        return Result<ParsedTypeLevel>::success(
            ParsedTypeLevel(std::move(node)));
    }
    if (is_normalized_integer(kind)) {
        if (!minimum.present) {
            return Result<ParsedTypeLevel>::failure(
                missing_field(path.snapshot(), "min"));
        }
        if (!maximum.present) {
            return Result<ParsedTypeLevel>::failure(
                missing_field(path.snapshot(), "max"));
        }
        if (!minimum.value.is_number()) {
            return Result<ParsedTypeLevel>::failure(invalid_field_type(
                member_path(path, "min"), "number", minimum.value));
        }
        if (!maximum.value.is_number()) {
            return Result<ParsedTypeLevel>::failure(invalid_field_type(
                member_path(path, "max"), "number", maximum.value));
        }
        node.minimum = minimum.value.number();
        node.maximum = maximum.value.number();
        if (!std::isfinite(node.minimum)) {
            return Result<ParsedTypeLevel>::failure(invalid_range(
                member_path(path, "min"), node.minimum, node.maximum,
                "non_finite_min"));
        }
        if (!std::isfinite(node.maximum)) {
            return Result<ParsedTypeLevel>::failure(invalid_range(
                member_path(path, "max"), node.minimum, node.maximum,
                "non_finite_max"));
        }
        if (!(node.minimum < node.maximum)) {
            return Result<ParsedTypeLevel>::failure(invalid_range(
                member_path(path, "max"), node.minimum, node.maximum,
                "min_not_less_than_max"));
        }
        return Result<ParsedTypeLevel>::success(
            ParsedTypeLevel(std::move(node)));
    }
    if (kind == TypeKind::component || kind == TypeKind::ref) {
        const std::string_view member_name =
            kind == TypeKind::component ? std::string_view{"id"}
                                        : std::string_view{"target"};
        if (!source_id.present) {
            return Result<ParsedTypeLevel>::failure(
                missing_field(path.snapshot(), member_name));
        }
        const JsonPointerBuilder::Mark type_mark = path.mark();
        path.append(member_name);
        auto parsed_id = parse_identifier(source_id.value, path);
        path.rewind(type_mark);
        if (!parsed_id.has_value()) {
            return Result<ParsedTypeLevel>::failure(
                std::move(parsed_id).error());
        }
        node.source_id = std::move(parsed_id).value();
        return Result<ParsedTypeLevel>::success(
            ParsedTypeLevel(std::move(node)));
    }
    if (!items.present) {
        return Result<ParsedTypeLevel>::failure(
            missing_field(path.snapshot(), "items"));
    }
    return Result<ParsedTypeLevel>::success(
        ParsedTypeLevel(std::move(node), items.value));
}

Result<TypeNode> parse_type(const JsonCursor& value,
                            JsonPointerBuilder& path) {
    JsonCursor current_value = value;
    std::vector<TypeNode> parents;

    while (true) {
        auto parsed_level = parse_type_level(current_value, path);
        if (!parsed_level.has_value()) {
            return Result<TypeNode>::failure(
                std::move(parsed_level).error());
        }
        ParsedTypeLevel level = std::move(parsed_level).value();
        if (level.has_items) {
            current_value = level.items;
            path.append("items");
            parents.push_back(std::move(level.node));
            continue;
        }

        std::unique_ptr<TypeNode> completed =
            std::make_unique<TypeNode>(std::move(level.node));
        while (!parents.empty()) {
            TypeNode parent = std::move(parents.back());
            parents.pop_back();
            parent.items = std::move(completed);
            completed = std::make_unique<TypeNode>(std::move(parent));
        }
        return Result<TypeNode>::success(std::move(*completed));
    }
}

Result<Field> parse_field(const JsonCursor& value,
                          JsonPointerBuilder& path) {
    if (!value.is_object()) {
        return Result<Field>::failure(
            invalid_field_type(path.snapshot(), "object", value));
    }
    MemberSlot id;
    MemberSlot type;
    auto members = value.members();
    std::string_view name;
    JsonCursor member_value;
    while (members.next(name, member_value)) {
        if (name == "id") {
            id = MemberSlot{true, member_value};
        } else if (name == "type") {
            type = MemberSlot{true, member_value};
        } else {
            return Result<Field>::failure(
                unknown_field(member_path(path, name), name));
        }
    }
    if (!id.present) {
        return Result<Field>::failure(missing_field(path.snapshot(), "id"));
    }
    if (!type.present) {
        return Result<Field>::failure(
            missing_field(path.snapshot(), "type"));
    }
    const JsonPointerBuilder::Mark field_mark = path.mark();
    path.append("id");
    auto parsed_id = parse_identifier(id.value, path);
    path.rewind(field_mark);
    if (!parsed_id.has_value()) {
        return Result<Field>::failure(std::move(parsed_id).error());
    }
    path.append("type");
    auto parsed_type = parse_type(type.value, path);
    path.rewind(field_mark);
    if (!parsed_type.has_value()) {
        return Result<Field>::failure(std::move(parsed_type).error());
    }
    return Result<Field>::success(
        Field(std::move(parsed_id).value(), std::move(parsed_type).value()));
}

Result<Component> parse_component(const JsonCursor& value,
                                  JsonPointerBuilder& path,
                                  const SourceParseLimits& limits,
                                  std::uint64_t& total_fields) {
    if (!value.is_object()) {
        return Result<Component>::failure(
            invalid_field_type(path.snapshot(), "object", value));
    }
    MemberSlot id;
    MemberSlot kind;
    MemberSlot fields;
    auto members = value.members();
    std::string_view name;
    JsonCursor member_value;
    while (members.next(name, member_value)) {
        if (name == "id") {
            id = MemberSlot{true, member_value};
        } else if (name == "kind") {
            kind = MemberSlot{true, member_value};
        } else if (name == "fields") {
            fields = MemberSlot{true, member_value};
        } else {
            return Result<Component>::failure(
                unknown_field(member_path(path, name), name));
        }
    }
    if (!id.present) {
        return Result<Component>::failure(
            missing_field(path.snapshot(), "id"));
    }
    if (!kind.present) {
        return Result<Component>::failure(
            missing_field(path.snapshot(), "kind"));
    }
    if (!fields.present) {
        return Result<Component>::failure(
            missing_field(path.snapshot(), "fields"));
    }
    const JsonPointerBuilder::Mark component_mark = path.mark();
    path.append("id");
    auto parsed_id = parse_identifier(id.value, path);
    path.rewind(component_mark);
    if (!parsed_id.has_value()) {
        return Result<Component>::failure(std::move(parsed_id).error());
    }
    if (!kind.value.is_string()) {
        return Result<Component>::failure(invalid_field_type(
            member_path(path, "kind"), "string", kind.value));
    }
    if (kind.value.string() != "record") {
        return Result<Component>::failure(invalid_component_kind(
            member_path(path, "kind"), kind.value.string()));
    }
    if (!fields.value.is_array()) {
        return Result<Component>::failure(invalid_field_type(
            member_path(path, "fields"), "array", fields.value));
    }

    const std::uint64_t field_count = fields.value.size();
    if (field_count >
        static_cast<std::uint64_t>(limits.max_fields_per_component)) {
        return Result<Component>::failure(resource_limit(
            member_path(path, "fields"),
            "Payload component field count exceeds configured limit",
            "fields_per_component", field_count,
            static_cast<std::uint64_t>(limits.max_fields_per_component)));
    }
    const bool total_overflows =
        field_count >
        std::numeric_limits<std::uint64_t>::max() - total_fields;
    const std::uint64_t total_after =
        total_overflows ? std::numeric_limits<std::uint64_t>::max()
                        : total_fields + field_count;
    if (total_overflows || total_after > limits.max_total_fields) {
        return Result<Component>::failure(resource_limit(
            member_path(path, "fields"),
            "Payload total field count exceeds configured limit",
            "total_fields", total_after, limits.max_total_fields));
    }

    std::vector<Field> parsed_fields;
    parsed_fields.reserve(static_cast<std::size_t>(field_count));
    auto elements = fields.value.elements();
    JsonCursor field_value;
    std::uint64_t field_index = UINT64_C(0);
    path.append("fields");
    const JsonPointerBuilder::Mark fields_mark = path.mark();
    while (elements.next(field_value)) {
        path.append(field_index);
        auto parsed_field = parse_field(field_value, path);
        path.rewind(fields_mark);
        if (!parsed_field.has_value()) {
            return Result<Component>::failure(
                std::move(parsed_field).error());
        }
        parsed_fields.push_back(std::move(parsed_field).value());
        ++field_index;
    }
    path.rewind(component_mark);
    total_fields = total_after;
    return Result<Component>::success(Component(
        std::move(parsed_id).value(), std::move(parsed_fields)));
}

Result<Entry> parse_entry(const JsonCursor& value,
                          JsonPointerBuilder& path) {
    if (!value.is_object()) {
        return Result<Entry>::failure(
            invalid_field_type(path.snapshot(), "object", value));
    }
    MemberSlot id;
    MemberSlot cardinality;
    MemberSlot type;
    auto members = value.members();
    std::string_view name;
    JsonCursor member_value;
    while (members.next(name, member_value)) {
        if (name == "id") {
            id = MemberSlot{true, member_value};
        } else if (name == "cardinality") {
            cardinality = MemberSlot{true, member_value};
        } else if (name == "type") {
            type = MemberSlot{true, member_value};
        } else {
            return Result<Entry>::failure(
                unknown_field(member_path(path, name), name));
        }
    }
    if (!id.present) {
        return Result<Entry>::failure(missing_field(path.snapshot(), "id"));
    }
    if (!cardinality.present) {
        return Result<Entry>::failure(
            missing_field(path.snapshot(), "cardinality"));
    }
    if (!type.present) {
        return Result<Entry>::failure(
            missing_field(path.snapshot(), "type"));
    }
    const JsonPointerBuilder::Mark entry_mark = path.mark();
    path.append("id");
    auto parsed_id = parse_identifier(id.value, path);
    path.rewind(entry_mark);
    if (!parsed_id.has_value()) {
        return Result<Entry>::failure(std::move(parsed_id).error());
    }
    if (!cardinality.value.is_string()) {
        return Result<Entry>::failure(invalid_field_type(
            member_path(path, "cardinality"), "string", cardinality.value));
    }
    Cardinality parsed_cardinality;
    if (cardinality.value.string() == "one") {
        parsed_cardinality = Cardinality::one;
    } else if (cardinality.value.string() == "many") {
        parsed_cardinality = Cardinality::many;
    } else {
        return Result<Entry>::failure(invalid_cardinality(
            member_path(path, "cardinality"), cardinality.value.string()));
    }
    path.append("type");
    auto parsed_type = parse_type(type.value, path);
    path.rewind(entry_mark);
    if (!parsed_type.has_value()) {
        return Result<Entry>::failure(std::move(parsed_type).error());
    }
    return Result<Entry>::success(Entry(
        std::move(parsed_id).value(), parsed_cardinality,
        std::move(parsed_type).value()));
}

std::string_view profile_name(Profile profile) noexcept {
    switch (profile) {
    case Profile::record_v1:
        return "record.v1";
    case Profile::object_graph_v1:
        return "object_graph.v1";
    }
    return {};
}

std::string_view cardinality_name(Cardinality cardinality) noexcept {
    switch (cardinality) {
    case Cardinality::one:
        return "one";
    case Cardinality::many:
        return "many";
    }
    return {};
}

std::string_view type_kind_name(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return "bool";
    case TypeKind::u8:
        return "u8";
    case TypeKind::u16:
        return "u16";
    case TypeKind::u32:
        return "u32";
    case TypeKind::i32:
        return "i32";
    case TypeKind::u8n:
        return "u8n";
    case TypeKind::u16n:
        return "u16n";
    case TypeKind::f32:
        return "f32";
    case TypeKind::f64:
        return "f64";
    case TypeKind::str:
        return "str";
    case TypeKind::wstr:
        return "wstr";
    case TypeKind::bytes:
        return "bytes";
    case TypeKind::component:
        return "component";
    case TypeKind::ref:
        return "ref";
    case TypeKind::list:
        return "list";
    }
    return {};
}

JsonValue normalized_type_level_json(const TypeNode& type,
                                     JsonValue* normalized_items) {
    JsonValue::Object members;
    members.emplace_back("kind",
                         JsonValue{std::string(type_kind_name(type.kind))});
    members.emplace_back("nullable", JsonValue{type.nullable});
    if (is_normalized_integer(type.kind)) {
        members.emplace_back("min", JsonValue{type.minimum});
        members.emplace_back("max", JsonValue{type.maximum});
    } else if (type.kind == TypeKind::component) {
        members.emplace_back("id", JsonValue{type.source_id});
    } else if (type.kind == TypeKind::ref) {
        members.emplace_back("target", JsonValue{type.source_id});
    } else if (type.kind == TypeKind::list) {
        members.emplace_back("items",
                             normalized_items == nullptr
                                 ? JsonValue{nullptr}
                                 : std::move(*normalized_items));
    }
    return JsonValue::object(std::move(members));
}

JsonValue normalized_type_json(const TypeNode& type) {
    std::vector<const TypeNode*> parents;
    const TypeNode* current = &type;
    while (current->kind == TypeKind::list && current->items != nullptr) {
        parents.push_back(current);
        current = current->items.get();
    }

    JsonValue completed = normalized_type_level_json(*current, nullptr);
    while (!parents.empty()) {
        const TypeNode* const parent = parents.back();
        parents.pop_back();
        completed = normalized_type_level_json(*parent, &completed);
    }
    return completed;
}

}  // namespace

Result<SourceSpec> parse_and_normalize_source(
    const json::JsonDocument& document,
    SourceParseLimits limits) {
    const JsonCursor root = document.root();
    JsonPointerBuilder path;
    if (!root.is_object()) {
        return Result<SourceSpec>::failure(
            invalid_field_type(path.snapshot(), "object", root));
    }

    MemberSlot schema;
    MemberSlot profile;
    MemberSlot entries;
    MemberSlot components;
    auto members = root.members();
    std::string_view name;
    JsonCursor value;
    while (members.next(name, value)) {
        if (name == "schema") {
            schema = MemberSlot{true, value};
        } else if (name == "profile") {
            profile = MemberSlot{true, value};
        } else if (name == "entries") {
            entries = MemberSlot{true, value};
        } else if (name == "components") {
            components = MemberSlot{true, value};
        } else {
            return Result<SourceSpec>::failure(
                unknown_field(member_path(path, name), name));
        }
    }
    if (!schema.present) {
        return Result<SourceSpec>::failure(
            missing_field(path.snapshot(), "schema"));
    }
    if (!profile.present) {
        return Result<SourceSpec>::failure(
            missing_field(path.snapshot(), "profile"));
    }
    if (!entries.present) {
        return Result<SourceSpec>::failure(
            missing_field(path.snapshot(), "entries"));
    }
    if (!components.present) {
        return Result<SourceSpec>::failure(
            missing_field(path.snapshot(), "components"));
    }
    if (!schema.value.is_string()) {
        return Result<SourceSpec>::failure(invalid_field_type(
            member_path(path, "schema"), "string", schema.value));
    }
    if (schema.value.string() != "fastdb.payload.v1") {
        return Result<SourceSpec>::failure(unsupported_schema(
            member_path(path, "schema"), schema.value.string()));
    }
    if (!profile.value.is_string()) {
        return Result<SourceSpec>::failure(invalid_field_type(
            member_path(path, "profile"), "string", profile.value));
    }
    Profile parsed_profile;
    if (profile.value.string() == "record.v1") {
        parsed_profile = Profile::record_v1;
    } else if (profile.value.string() == "object_graph.v1") {
        parsed_profile = Profile::object_graph_v1;
    } else {
        return Result<SourceSpec>::failure(invalid_profile(
            member_path(path, "profile"), profile.value.string()));
    }
    if (!entries.value.is_array()) {
        return Result<SourceSpec>::failure(invalid_field_type(
            member_path(path, "entries"), "array", entries.value));
    }
    if (!components.value.is_array()) {
        return Result<SourceSpec>::failure(invalid_field_type(
            member_path(path, "components"), "array", components.value));
    }

    const std::uint64_t entry_count = entries.value.size();
    if (entry_count > static_cast<std::uint64_t>(limits.max_entries)) {
        return Result<SourceSpec>::failure(resource_limit(
            member_path(path, "entries"),
            "Payload entry count exceeds configured limit", "entries",
            entry_count, static_cast<std::uint64_t>(limits.max_entries)));
    }
    const std::uint64_t component_count = components.value.size();
    if (component_count >
        static_cast<std::uint64_t>(limits.max_components)) {
        return Result<SourceSpec>::failure(resource_limit(
            member_path(path, "components"),
            "Payload component count exceeds configured limit", "components",
            component_count,
            static_cast<std::uint64_t>(limits.max_components)));
    }

    std::vector<Entry> parsed_entries;
    parsed_entries.reserve(static_cast<std::size_t>(entry_count));
    auto entry_values = entries.value.elements();
    JsonCursor entry_value;
    std::uint64_t entry_index = UINT64_C(0);
    const JsonPointerBuilder::Mark root_mark = path.mark();
    path.append("entries");
    const JsonPointerBuilder::Mark entries_mark = path.mark();
    while (entry_values.next(entry_value)) {
        path.append(entry_index);
        auto parsed_entry = parse_entry(entry_value, path);
        path.rewind(entries_mark);
        if (!parsed_entry.has_value()) {
            return Result<SourceSpec>::failure(
                std::move(parsed_entry).error());
        }
        parsed_entries.push_back(std::move(parsed_entry).value());
        ++entry_index;
    }
    path.rewind(root_mark);

    std::vector<Component> parsed_components;
    parsed_components.reserve(static_cast<std::size_t>(component_count));
    auto component_values = components.value.elements();
    JsonCursor component_value;
    std::uint64_t component_index = UINT64_C(0);
    std::uint64_t total_fields = UINT64_C(0);
    path.append("components");
    const JsonPointerBuilder::Mark components_mark = path.mark();
    while (component_values.next(component_value)) {
        path.append(component_index);
        auto parsed_component =
            parse_component(component_value, path, limits, total_fields);
        path.rewind(components_mark);
        if (!parsed_component.has_value()) {
            return Result<SourceSpec>::failure(
                std::move(parsed_component).error());
        }
        parsed_components.push_back(std::move(parsed_component).value());
        ++component_index;
    }
    path.rewind(root_mark);

    return Result<SourceSpec>::success(SourceSpec(
        parsed_profile, std::move(parsed_entries),
        std::move(parsed_components)));
}

JsonValue normalized_source_json(const SourceSpec& source) {
    JsonValue::Array entries;
    entries.reserve(source.entries.size());
    for (const Entry& entry : source.entries) {
        entries.push_back(JsonValue::object({
            JsonValue::Member{"id", JsonValue{entry.id}},
            JsonValue::Member{
                "cardinality",
                JsonValue{std::string(cardinality_name(entry.cardinality))}},
            JsonValue::Member{"type", normalized_type_json(entry.type)},
        }));
    }

    JsonValue::Array components;
    components.reserve(source.components.size());
    for (const Component& component : source.components) {
        JsonValue::Array fields;
        fields.reserve(component.fields.size());
        for (const Field& field : component.fields) {
            fields.push_back(JsonValue::object({
                JsonValue::Member{"id", JsonValue{field.id}},
                JsonValue::Member{"type", normalized_type_json(field.type)},
            }));
        }
        components.push_back(JsonValue::object({
            JsonValue::Member{"id", JsonValue{component.id}},
            JsonValue::Member{"kind", JsonValue{"record"}},
            JsonValue::Member{"fields", JsonValue::array(std::move(fields))},
        }));
    }

    return JsonValue::object({
        JsonValue::Member{"schema", JsonValue{"fastdb.payload.v1"}},
        JsonValue::Member{
            "profile",
            JsonValue{std::string(profile_name(source.profile))}},
        JsonValue::Member{"entries", JsonValue::array(std::move(entries))},
        JsonValue::Member{"components",
                          JsonValue::array(std::move(components))},
    });
}

}  // namespace fastdb::payload::spec
