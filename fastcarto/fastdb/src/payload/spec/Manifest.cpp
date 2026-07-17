#include "payload/spec/Manifest.hpp"

#include "payload/identity/Sha256.hpp"
#include "payload/json/Jcs.hpp"
#include "payload/json/JsonPointer.hpp"

#include <fastdb_payload.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::payload::spec {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

enum class ValueClass : std::uint8_t {
    boolean,
    number,
    string,
    array,
    object,
};

struct MemberRule final {
    std::string_view name;
    ValueClass value_class;
};

bool is_class(const JsonValue& value, ValueClass expected) {
    const JsonValue::Storage& storage = value.storage();
    switch (expected) {
    case ValueClass::boolean:
        return std::holds_alternative<bool>(storage);
    case ValueClass::number:
        return std::holds_alternative<double>(storage);
    case ValueClass::string:
        return std::holds_alternative<std::string>(storage);
    case ValueClass::array:
        return std::holds_alternative<JsonValue::Array>(storage);
    case ValueClass::object:
        return std::holds_alternative<JsonValue::Object>(storage);
    }
    return false;
}

const JsonValue* member(const JsonValue::Object& object,
                        std::string_view name) {
    for (const JsonValue::Member& candidate : object) {
        if (candidate.first == name) {
            return &candidate.second;
        }
    }
    return nullptr;
}

template <std::size_t Size>
bool exact_object(const JsonValue& value,
                  const std::array<MemberRule, Size>& rules) {
    const auto* object = std::get_if<JsonValue::Object>(&value.storage());
    if (object == nullptr || object->size() != Size) {
        return false;
    }
    for (const MemberRule& rule : rules) {
        const JsonValue* candidate = member(*object, rule.name);
        if (candidate == nullptr || !is_class(*candidate, rule.value_class)) {
            return false;
        }
    }
    return true;
}

const JsonValue::Object* object_value(const JsonValue& value) {
    return std::get_if<JsonValue::Object>(&value.storage());
}

const JsonValue::Array* array_value(const JsonValue& value) {
    return std::get_if<JsonValue::Array>(&value.storage());
}

const std::string* string_value(const JsonValue& value) {
    return std::get_if<std::string>(&value.storage());
}

const double* number_value(const JsonValue& value) {
    return std::get_if<double>(&value.storage());
}

bool is_ascii_id(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const auto ascii_letter = [](unsigned char byte) {
        return (byte >= static_cast<unsigned char>('A') &&
                byte <= static_cast<unsigned char>('Z')) ||
               (byte >= static_cast<unsigned char>('a') &&
                byte <= static_cast<unsigned char>('z'));
    };
    const auto first = static_cast<unsigned char>(value.front());
    if (!ascii_letter(first) && first != static_cast<unsigned char>('_')) {
        return false;
    }
    for (std::size_t index = 1U; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (!ascii_letter(byte) &&
            !(byte >= static_cast<unsigned char>('0') &&
              byte <= static_cast<unsigned char>('9')) &&
            byte != static_cast<unsigned char>('_')) {
            return false;
        }
    }
    return true;
}

bool is_index(const JsonValue& value) {
    const double* number = number_value(value);
    return number != nullptr && std::isfinite(*number) && *number >= 0.0 &&
           *number <= static_cast<double>(
                          std::numeric_limits<std::uint32_t>::max()) &&
           std::floor(*number) == *number;
}

bool index_matches(const JsonValue& value, std::size_t expected) {
    if (expected > static_cast<std::size_t>(
                       std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    const double* number = number_value(value);
    return number != nullptr &&
           *number == static_cast<double>(
                          static_cast<std::uint32_t>(expected));
}

bool is_lower_sha256(std::string_view value) {
    if (value.size() != 64U) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool is_scalar_kind(std::string_view kind) {
    constexpr std::array<std::string_view, 10> scalars = {
        "bool", "u8", "u16", "u32", "i32",
        "f32",  "f64", "str", "wstr", "bytes",
    };
    for (const std::string_view scalar : scalars) {
        if (kind == scalar) {
            return true;
        }
    }
    return false;
}

bool validate_type(const JsonValue& root) {
    constexpr std::array<MemberRule, 2> scalar_rules = {{
        {"kind", ValueClass::string},
        {"nullable", ValueClass::boolean},
    }};
    constexpr std::array<MemberRule, 4> normalized_rules = {{
        {"kind", ValueClass::string},
        {"min", ValueClass::number},
        {"max", ValueClass::number},
        {"nullable", ValueClass::boolean},
    }};
    constexpr std::array<MemberRule, 4> component_rules = {{
        {"kind", ValueClass::string},
        {"id", ValueClass::string},
        {"component_index", ValueClass::number},
        {"nullable", ValueClass::boolean},
    }};
    constexpr std::array<MemberRule, 4> ref_rules = {{
        {"kind", ValueClass::string},
        {"target", ValueClass::string},
        {"target_component_index", ValueClass::number},
        {"nullable", ValueClass::boolean},
    }};
    constexpr std::array<MemberRule, 3> list_rules = {{
        {"kind", ValueClass::string},
        {"items", ValueClass::object},
        {"nullable", ValueClass::boolean},
    }};

    std::vector<const JsonValue*> pending{&root};
    while (!pending.empty()) {
        const JsonValue* current = pending.back();
        pending.pop_back();
        const JsonValue::Object* object = object_value(*current);
        if (object == nullptr) {
            return false;
        }
        const JsonValue* kind_value = member(*object, "kind");
        if (kind_value == nullptr) {
            return false;
        }
        const std::string* kind = string_value(*kind_value);
        if (kind == nullptr) {
            return false;
        }

        if (is_scalar_kind(*kind)) {
            if (!exact_object(*current, scalar_rules)) {
                return false;
            }
            continue;
        }
        if (*kind == "u8n" || *kind == "u16n") {
            if (!exact_object(*current, normalized_rules)) {
                return false;
            }
            const double* minimum = number_value(*member(*object, "min"));
            const double* maximum = number_value(*member(*object, "max"));
            if (minimum == nullptr || maximum == nullptr ||
                !std::isfinite(*minimum) || !std::isfinite(*maximum) ||
                !(*minimum < *maximum)) {
                return false;
            }
            continue;
        }
        if (*kind == "component") {
            if (!exact_object(*current, component_rules)) {
                return false;
            }
            const auto* id = string_value(*member(*object, "id"));
            if (id == nullptr || !is_ascii_id(*id) ||
                !is_index(*member(*object, "component_index"))) {
                return false;
            }
            continue;
        }
        if (*kind == "ref") {
            if (!exact_object(*current, ref_rules)) {
                return false;
            }
            const auto* target = string_value(*member(*object, "target"));
            if (target == nullptr || !is_ascii_id(*target) ||
                !is_index(*member(*object, "target_component_index"))) {
                return false;
            }
            continue;
        }
        if (*kind == "list") {
            if (!exact_object(*current, list_rules)) {
                return false;
            }
            pending.push_back(member(*object, "items"));
            continue;
        }
        return false;
    }
    return true;
}

std::string_view profile_name(Profile profile) {
    return profile == Profile::record_v1 ? std::string_view{"record.v1"}
                                         : std::string_view{"object_graph.v1"};
}

std::string_view cardinality_name(Cardinality cardinality) {
    return cardinality == Cardinality::one ? std::string_view{"one"}
                                           : std::string_view{"many"};
}

std::string_view type_name(TypeKind kind) {
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

JsonValue projected_terminal_type(const TypeNode& type) {
    JsonValue::Object members;
    members.emplace_back("kind", JsonValue{std::string(type_name(type.kind))});
    members.emplace_back("nullable", JsonValue{type.nullable});
    if (type.kind == TypeKind::u8n || type.kind == TypeKind::u16n) {
        members.emplace_back("min", JsonValue{type.minimum});
        members.emplace_back("max", JsonValue{type.maximum});
    } else if (type.kind == TypeKind::component) {
        members.emplace_back("id", JsonValue{type.source_id});
        members.emplace_back(
            "component_index",
            JsonValue{static_cast<double>(type.resolved_component_index)});
    } else if (type.kind == TypeKind::ref) {
        members.emplace_back("target", JsonValue{type.source_id});
        members.emplace_back(
            "target_component_index",
            JsonValue{static_cast<double>(type.resolved_component_index)});
    }
    return JsonValue::object(std::move(members));
}

JsonValue projected_type(const TypeNode& root) {
    std::vector<const TypeNode*> lists;
    const TypeNode* current = &root;
    while (current->kind == TypeKind::list && current->items != nullptr) {
        lists.push_back(current);
        current = current->items.get();
    }
    JsonValue projected = projected_terminal_type(*current);
    while (!lists.empty()) {
        const TypeNode* list = lists.back();
        lists.pop_back();
        projected = JsonValue::object({
            JsonValue::Member{"kind", JsonValue{"list"}},
            JsonValue::Member{"nullable", JsonValue{list->nullable}},
            JsonValue::Member{"items", std::move(projected)},
        });
    }
    return projected;
}

JsonValue manifest_value(const ResolvedSpec& resolved,
                         const std::array<std::uint8_t, 32>& digest) {
    JsonValue::Array entries;
    entries.reserve(resolved.entries().size());
    for (const Entry& entry : resolved.entries()) {
        entries.push_back(JsonValue::object({
            JsonValue::Member{
                "index", JsonValue{static_cast<double>(entry.index)}},
            JsonValue::Member{"id", JsonValue{entry.id}},
            JsonValue::Member{
                "cardinality",
                JsonValue{std::string(cardinality_name(entry.cardinality))}},
            JsonValue::Member{"type", projected_type(entry.type)},
        }));
    }

    JsonValue::Array components;
    components.reserve(resolved.components().size());
    for (const Component& component : resolved.components()) {
        JsonValue::Array fields;
        fields.reserve(component.fields.size());
        for (const Field& field : component.fields) {
            fields.push_back(JsonValue::object({
                JsonValue::Member{
                    "index", JsonValue{static_cast<double>(field.index)}},
                JsonValue::Member{"id", JsonValue{field.id}},
                JsonValue::Member{"type", projected_type(field.type)},
            }));
        }
        components.push_back(JsonValue::object({
            JsonValue::Member{
                "index", JsonValue{static_cast<double>(component.index)}},
            JsonValue::Member{"id", JsonValue{component.id}},
            JsonValue::Member{"kind", JsonValue{"record"}},
            JsonValue::Member{"fields", JsonValue::array(std::move(fields))},
        }));
    }

    const SemanticFacts& facts = resolved.facts();
    return JsonValue::object({
        JsonValue::Member{"schema",
                          JsonValue{"fastdb.payload.manifest.v1"}},
        JsonValue::Member{
            "payload",
            JsonValue::object({
                JsonValue::Member{"schema", JsonValue{"fastdb.payload.v1"}},
                JsonValue::Member{
                    "profile",
                    JsonValue{std::string(profile_name(resolved.profile()))}},
                JsonValue::Member{
                    "sha256",
                    JsonValue{identity::sha256_lower_hex(digest)}},
            })},
        JsonValue::Member{"entries", JsonValue::array(std::move(entries))},
        JsonValue::Member{"components",
                          JsonValue::array(std::move(components))},
        JsonValue::Member{
            "facts",
            JsonValue::object({
                JsonValue::Member{"has_lists", JsonValue{facts.has_lists}},
                JsonValue::Member{"has_normalized_integers",
                                  JsonValue{facts.has_normalized_integers}},
                JsonValue::Member{"has_nullable",
                                  JsonValue{facts.has_nullable}},
                JsonValue::Member{"has_references",
                                  JsonValue{facts.has_references}},
                JsonValue::Member{"has_variable_width",
                                  JsonValue{facts.has_variable_width}},
            })},
        JsonValue::Member{
            "capabilities",
            JsonValue::object({
                JsonValue::Member{
                    "operations",
                    JsonValue::array(
                        {JsonValue{"compile"}, JsonValue{"query"}})},
                JsonValue::Member{"codegen_targets", JsonValue::array({})},
                JsonValue::Member{
                    "direct_build",
                    JsonValue::object({
                        JsonValue::Member{"status",
                                          JsonValue{"not_evaluated"}},
                        JsonValue::Member{
                            "reason",
                            JsonValue{"runtime_slice_not_implemented"}},
                    })},
            })},
    });
}

Error invalid_derived_manifest() {
    return Error::from_details(
        FDB_PAYLOAD_E_INTERNAL, JsonPointer{},
        "Derived payload manifest violates the Core contract",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"invalid_derived_manifest"}}}));
}

}  // namespace

bool manifest_value_conforms(const JsonValue& manifest) {
    constexpr std::array<MemberRule, 6> root_rules = {{
        {"schema", ValueClass::string},
        {"payload", ValueClass::object},
        {"entries", ValueClass::array},
        {"components", ValueClass::array},
        {"facts", ValueClass::object},
        {"capabilities", ValueClass::object},
    }};
    constexpr std::array<MemberRule, 3> payload_rules = {{
        {"schema", ValueClass::string},
        {"profile", ValueClass::string},
        {"sha256", ValueClass::string},
    }};
    constexpr std::array<MemberRule, 4> entry_rules = {{
        {"index", ValueClass::number},
        {"id", ValueClass::string},
        {"cardinality", ValueClass::string},
        {"type", ValueClass::object},
    }};
    constexpr std::array<MemberRule, 4> component_rules = {{
        {"index", ValueClass::number},
        {"id", ValueClass::string},
        {"kind", ValueClass::string},
        {"fields", ValueClass::array},
    }};
    constexpr std::array<MemberRule, 3> field_rules = {{
        {"index", ValueClass::number},
        {"id", ValueClass::string},
        {"type", ValueClass::object},
    }};
    constexpr std::array<MemberRule, 5> fact_rules = {{
        {"has_lists", ValueClass::boolean},
        {"has_normalized_integers", ValueClass::boolean},
        {"has_nullable", ValueClass::boolean},
        {"has_references", ValueClass::boolean},
        {"has_variable_width", ValueClass::boolean},
    }};
    constexpr std::array<MemberRule, 3> capability_rules = {{
        {"operations", ValueClass::array},
        {"codegen_targets", ValueClass::array},
        {"direct_build", ValueClass::object},
    }};
    constexpr std::array<MemberRule, 2> direct_rules = {{
        {"status", ValueClass::string},
        {"reason", ValueClass::string},
    }};

    if (!exact_object(manifest, root_rules)) {
        return false;
    }
    const JsonValue::Object& root = *object_value(manifest);
    const auto* schema = string_value(*member(root, "schema"));
    if (schema == nullptr || *schema != "fastdb.payload.manifest.v1") {
        return false;
    }

    const JsonValue& payload = *member(root, "payload");
    if (!exact_object(payload, payload_rules)) {
        return false;
    }
    const JsonValue::Object& payload_object = *object_value(payload);
    const auto* payload_schema =
        string_value(*member(payload_object, "schema"));
    const auto* profile = string_value(*member(payload_object, "profile"));
    const auto* digest = string_value(*member(payload_object, "sha256"));
    if (payload_schema == nullptr || *payload_schema != "fastdb.payload.v1" ||
        profile == nullptr ||
        (*profile != "record.v1" && *profile != "object_graph.v1") ||
        digest == nullptr || !is_lower_sha256(*digest)) {
        return false;
    }

    const JsonValue::Array& entries = *array_value(*member(root, "entries"));
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const JsonValue& entry = entries[index];
        if (!exact_object(entry, entry_rules)) {
            return false;
        }
        const JsonValue::Object& object = *object_value(entry);
        const auto* id = string_value(*member(object, "id"));
        const auto* cardinality =
            string_value(*member(object, "cardinality"));
        if (!index_matches(*member(object, "index"), index) ||
            id == nullptr || !is_ascii_id(*id) || cardinality == nullptr ||
            (*cardinality != "one" && *cardinality != "many") ||
            !validate_type(*member(object, "type"))) {
            return false;
        }
    }

    const JsonValue::Array& components =
        *array_value(*member(root, "components"));
    for (std::size_t index = 0; index < components.size(); ++index) {
        const JsonValue& component = components[index];
        if (!exact_object(component, component_rules)) {
            return false;
        }
        const JsonValue::Object& object = *object_value(component);
        const auto* id = string_value(*member(object, "id"));
        const auto* kind = string_value(*member(object, "kind"));
        if (!index_matches(*member(object, "index"), index) ||
            id == nullptr || !is_ascii_id(*id) || kind == nullptr ||
            *kind != "record") {
            return false;
        }
        const JsonValue::Array& fields =
            *array_value(*member(object, "fields"));
        for (std::size_t field_index = 0; field_index < fields.size();
             ++field_index) {
            const JsonValue& field = fields[field_index];
            if (!exact_object(field, field_rules)) {
                return false;
            }
            const JsonValue::Object& field_object = *object_value(field);
            const auto* field_id =
                string_value(*member(field_object, "id"));
            if (!index_matches(*member(field_object, "index"), field_index) ||
                field_id == nullptr || !is_ascii_id(*field_id) ||
                !validate_type(*member(field_object, "type"))) {
                return false;
            }
        }
    }

    if (!exact_object(*member(root, "facts"), fact_rules)) {
        return false;
    }
    const JsonValue& capabilities = *member(root, "capabilities");
    if (!exact_object(capabilities, capability_rules)) {
        return false;
    }
    const JsonValue::Object& capability_object = *object_value(capabilities);
    const JsonValue::Array& operations =
        *array_value(*member(capability_object, "operations"));
    const JsonValue::Array& targets =
        *array_value(*member(capability_object, "codegen_targets"));
    if (operations.size() != 2U || targets.size() != 0U ||
        string_value(operations[0]) == nullptr ||
        *string_value(operations[0]) != "compile" ||
        string_value(operations[1]) == nullptr ||
        *string_value(operations[1]) != "query") {
        return false;
    }
    const JsonValue& direct = *member(capability_object, "direct_build");
    if (!exact_object(direct, direct_rules)) {
        return false;
    }
    const JsonValue::Object& direct_object = *object_value(direct);
    const auto* status = string_value(*member(direct_object, "status"));
    const auto* reason = string_value(*member(direct_object, "reason"));
    return status != nullptr && *status == "not_evaluated" &&
           reason != nullptr && *reason == "runtime_slice_not_implemented";
}

Result<ManifestArtifact> build_manifest(
    const ResolvedSpec& resolved,
    const std::array<std::uint8_t, 32>& payload_digest) {
    JsonValue value = manifest_value(resolved, payload_digest);
    if (!manifest_value_conforms(value)) {
        return Result<ManifestArtifact>::failure(invalid_derived_manifest());
    }
    auto serialized = json::jcs_serialize(value);
    if (auto* failure = std::get_if<json::JcsFailure>(&serialized)) {
        return Result<ManifestArtifact>::failure(Error::from_jcs_failure(
            *failure, JsonPointer{},
            "Derived payload manifest cannot be canonicalized",
            JsonValue::object({JsonValue::Member{
                "reason", JsonValue{"manifest_jcs_failure"}}})));
    }
    ManifestCapabilities capabilities{
        FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY,
        UINT64_C(0), FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED,
        "runtime_slice_not_implemented"};
    return Result<ManifestArtifact>::success(ManifestArtifact{
        std::get<std::string>(std::move(serialized)),
        std::move(capabilities)});
}

}  // namespace fastdb::payload::spec
