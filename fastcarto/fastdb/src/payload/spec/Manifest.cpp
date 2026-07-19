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

struct RuntimeFacts final {
    bool utf8{false};
    bool utf16le{false};
    bool bytes{false};
    bool list_items{false};
    bool objects{false};
    bool references{false};
    bool roots{false};
    bool fixed_width_values_only{true};
    bool has_runtime_sized_regions{false};
    std::uint64_t reachable_type_count{UINT64_C(0)};
    std::uint64_t reachable_component_count{UINT64_C(0)};
    std::uint64_t reachable_list_type_count{UINT64_C(0)};
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

RuntimeFacts derive_runtime_facts(const ResolvedSpec& resolved) {
    RuntimeFacts facts;
    std::vector<const TypeNode*> pending;
    pending.reserve(resolved.entries().size());
    std::vector<bool> visited_components(resolved.components().size(), false);
    const bool graph = resolved.profile() == Profile::object_graph_v1;

    const auto enqueue_component = [&](std::uint32_t component_index) {
        const std::size_t index = static_cast<std::size_t>(component_index);
        if (index >= resolved.components().size() ||
            visited_components[index]) {
            return;
        }
        visited_components[index] = true;
        ++facts.reachable_component_count;
        if (graph) {
            facts.objects = true;
        }
        const auto& fields = resolved.components()[index].fields;
        for (auto field = fields.rbegin(); field != fields.rend(); ++field) {
            pending.push_back(&field->type);
        }
    };

    for (const Entry& entry : resolved.entries()) {
        pending.push_back(&entry.type);
        if (entry.cardinality == Cardinality::many) {
            facts.has_runtime_sized_regions = true;
        }
        if (graph) {
            const TypeNode* root = &entry.type;
            while (root->kind == TypeKind::list && root->items != nullptr) {
                root = root->items.get();
            }
            if (root->kind == TypeKind::component ||
                root->kind == TypeKind::ref) {
                facts.roots = true;
            }
        }
    }

    while (!pending.empty()) {
        const TypeNode* const type = pending.back();
        pending.pop_back();
        ++facts.reachable_type_count;
        switch (type->kind) {
        case TypeKind::str:
            facts.utf8 = true;
            facts.fixed_width_values_only = false;
            facts.has_runtime_sized_regions = true;
            break;
        case TypeKind::wstr:
            facts.utf16le = true;
            facts.fixed_width_values_only = false;
            facts.has_runtime_sized_regions = true;
            break;
        case TypeKind::bytes:
            facts.bytes = true;
            facts.fixed_width_values_only = false;
            facts.has_runtime_sized_regions = true;
            break;
        case TypeKind::list:
            facts.list_items = true;
            facts.fixed_width_values_only = false;
            facts.has_runtime_sized_regions = true;
            ++facts.reachable_list_type_count;
            if (type->items != nullptr) {
                pending.push_back(type->items.get());
            }
            break;
        case TypeKind::component:
            enqueue_component(type->resolved_component_index);
            break;
        case TypeKind::ref:
            facts.references = true;
            facts.fixed_width_values_only = false;
            facts.has_runtime_sized_regions = true;
            if (graph) {
                enqueue_component(type->resolved_component_index);
            }
            break;
        case TypeKind::boolean:
        case TypeKind::u8:
        case TypeKind::u16:
        case TypeKind::u32:
        case TypeKind::i32:
        case TypeKind::u8n:
        case TypeKind::u16n:
        case TypeKind::f32:
        case TypeKind::f64:
            break;
        }
    }

    if (graph && (facts.objects || facts.references || facts.roots)) {
        facts.has_runtime_sized_regions = true;
    }
    return facts;
}

JsonValue runtime_value(const ResolvedSpec& resolved) {
    const RuntimeFacts facts = derive_runtime_facts(resolved);
    JsonValue::Array pools;
    const auto append_pool = [&pools](bool required, const char* name) {
        if (required) {
            pools.push_back(JsonValue{name});
        }
    };
    append_pool(facts.utf8, "utf8");
    append_pool(facts.utf16le, "utf16le");
    append_pool(facts.bytes, "bytes");
    append_pool(facts.list_items, "list_items");
    append_pool(facts.objects, "objects");
    append_pool(facts.references, "references");
    append_pool(facts.roots, "roots");

    const bool record = resolved.profile() == Profile::record_v1;
    return JsonValue::object({
        JsonValue::Member{"status",
                          JsonValue{record ? "available"
                                           : "not_evaluated"}},
        JsonValue::Member{"layout_model",
                          JsonValue{record ? "record_aos"
                                           : "object_pool_aos"}},
        JsonValue::Member{"required_pools",
                          JsonValue::array(std::move(pools))},
        JsonValue::Member{"fixed_width_values_only",
                          JsonValue{facts.fixed_width_values_only}},
        JsonValue::Member{"has_runtime_sized_regions",
                          JsonValue{facts.has_runtime_sized_regions}},
        JsonValue::Member{
            "reachable_type_count",
            JsonValue{static_cast<double>(facts.reachable_type_count)}},
        JsonValue::Member{
            "reachable_component_count",
            JsonValue{static_cast<double>(facts.reachable_component_count)}},
        JsonValue::Member{
            "reachable_list_type_count",
            JsonValue{static_cast<double>(facts.reachable_list_type_count)}},
    });
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
    const bool record = resolved.profile() == Profile::record_v1;
    JsonValue::Array operations{
        JsonValue{"compile"},
        JsonValue{"query"},
    };
    if (record) {
        operations.push_back(JsonValue{"build"});
        operations.push_back(JsonValue{"open"});
        operations.push_back(JsonValue{"invalidate"});
    }
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
        JsonValue::Member{"runtime", runtime_value(resolved)},
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
                    JsonValue::array(std::move(operations))},
                JsonValue::Member{"codegen_targets", JsonValue::array({})},
                JsonValue::Member{
                    "direct_build",
                    JsonValue::object({
                        JsonValue::Member{"status",
                                          JsonValue{record ? "eligible"
                                                           : "not_evaluated"}},
                        JsonValue::Member{
                            "reason",
                            JsonValue{record
                                          ? "record_layout_exact"
                                          : "runtime_slice_not_implemented"}},
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
    constexpr std::array<MemberRule, 7> root_rules = {{
        {"schema", ValueClass::string},
        {"payload", ValueClass::object},
        {"entries", ValueClass::array},
        {"components", ValueClass::array},
        {"runtime", ValueClass::object},
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
    constexpr std::array<MemberRule, 8> runtime_rules = {{
        {"status", ValueClass::string},
        {"layout_model", ValueClass::string},
        {"required_pools", ValueClass::array},
        {"fixed_width_values_only", ValueClass::boolean},
        {"has_runtime_sized_regions", ValueClass::boolean},
        {"reachable_type_count", ValueClass::number},
        {"reachable_component_count", ValueClass::number},
        {"reachable_list_type_count", ValueClass::number},
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
    const JsonValue& runtime = *member(root, "runtime");
    if (!exact_object(runtime, runtime_rules)) {
        return false;
    }
    const JsonValue::Object& runtime_object = *object_value(runtime);
    const auto* runtime_status =
        string_value(*member(runtime_object, "status"));
    const auto* layout_model =
        string_value(*member(runtime_object, "layout_model"));
    const bool record = *profile == "record.v1";
    if (runtime_status == nullptr || layout_model == nullptr ||
        (record && (*runtime_status != "available" ||
                    *layout_model != "record_aos")) ||
        (!record && (*runtime_status != "not_evaluated" ||
                     *layout_model != "object_pool_aos")) ||
        !is_index(*member(runtime_object, "reachable_type_count")) ||
        !is_index(*member(runtime_object, "reachable_component_count")) ||
        !is_index(*member(runtime_object, "reachable_list_type_count"))) {
        return false;
    }
    constexpr std::array<std::string_view, 7> pool_order{{
        "utf8", "utf16le", "bytes", "list_items", "objects",
        "references", "roots"}};
    const JsonValue::Array& required_pools =
        *array_value(*member(runtime_object, "required_pools"));
    std::size_t previous = 0U;
    bool first_pool = true;
    for (const JsonValue& pool_value : required_pools) {
        const auto* pool = string_value(pool_value);
        if (pool == nullptr) {
            return false;
        }
        std::size_t current = pool_order.size();
        for (std::size_t index = 0U; index < pool_order.size(); ++index) {
            if (*pool == pool_order[index]) {
                current = index;
                break;
            }
        }
        if (current == pool_order.size() ||
            (!first_pool && current <= previous)) {
            return false;
        }
        first_pool = false;
        previous = current;
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
    const std::size_t expected_operations = record ? 5U : 2U;
    if (operations.size() != expected_operations || targets.size() != 0U ||
        string_value(operations[0]) == nullptr ||
        *string_value(operations[0]) != "compile" ||
        string_value(operations[1]) == nullptr ||
        *string_value(operations[1]) != "query") {
        return false;
    }
    if (record &&
        (string_value(operations[2]) == nullptr ||
         *string_value(operations[2]) != "build" ||
         string_value(operations[3]) == nullptr ||
         *string_value(operations[3]) != "open" ||
         string_value(operations[4]) == nullptr ||
         *string_value(operations[4]) != "invalidate")) {
        return false;
    }
    const JsonValue& direct = *member(capability_object, "direct_build");
    if (!exact_object(direct, direct_rules)) {
        return false;
    }
    const JsonValue::Object& direct_object = *object_value(direct);
    const auto* status = string_value(*member(direct_object, "status"));
    const auto* reason = string_value(*member(direct_object, "reason"));
    return status != nullptr && reason != nullptr &&
           ((record && *status == "eligible" &&
             *reason == "record_layout_exact") ||
            (!record && *status == "not_evaluated" &&
             *reason == "runtime_slice_not_implemented"));
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
    const bool record = resolved.profile() == Profile::record_v1;
    ManifestCapabilities capabilities{
        FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY |
            (record ? FDB_PAYLOAD_OPERATION_BUILD |
                          FDB_PAYLOAD_OPERATION_OPEN |
                          FDB_PAYLOAD_OPERATION_INVALIDATE
                    : UINT64_C(0)),
        UINT64_C(0),
        record ? FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE
               : FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED,
        record ? "record_layout_exact" : "runtime_slice_not_implemented"};
    return Result<ManifestArtifact>::success(ManifestArtifact{
        std::get<std::string>(std::move(serialized)),
        std::move(capabilities)});
}

}  // namespace fastdb::payload::spec
