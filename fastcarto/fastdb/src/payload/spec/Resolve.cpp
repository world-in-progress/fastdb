#include "payload/spec/Resolve.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/spec/Parse.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastdb::payload::spec {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonPointerBuilder;
using json::JsonValue;

JsonValue details(JsonValue::Object members) {
    return JsonValue::object(std::move(members));
}

JsonPointer entry_id_path(std::uint32_t source_index) {
    JsonPointerBuilder path;
    path.append("entries");
    path.append(static_cast<std::uint64_t>(source_index));
    path.append("id");
    return path.snapshot();
}

JsonPointer component_id_path(std::uint32_t source_index) {
    JsonPointerBuilder path;
    path.append("components");
    path.append(static_cast<std::uint64_t>(source_index));
    path.append("id");
    return path.snapshot();
}

JsonPointer field_id_path(std::uint32_t component_source_index,
                          std::uint32_t field_source_index) {
    JsonPointerBuilder path;
    path.append("components");
    path.append(static_cast<std::uint64_t>(component_source_index));
    path.append("fields");
    path.append(static_cast<std::uint64_t>(field_source_index));
    path.append("id");
    return path.snapshot();
}

enum class TypeRootKind : std::uint8_t { entry, field };

struct TypeLocation final {
    static TypeLocation entry(std::uint32_t source_index) noexcept {
        return TypeLocation{TypeRootKind::entry, source_index, UINT32_C(0)};
    }

    static TypeLocation field(std::uint32_t component_source_index,
                              std::uint32_t field_source_index) noexcept {
        return TypeLocation{TypeRootKind::field, component_source_index,
                            field_source_index};
    }

    TypeRootKind root_kind;
    std::uint32_t first_source_index;
    std::uint32_t second_source_index;
};

void append_type_root(JsonPointerBuilder& path, const TypeLocation& location) {
    if (location.root_kind == TypeRootKind::entry) {
        path.append("entries");
        path.append(
            static_cast<std::uint64_t>(location.first_source_index));
        path.append("type");
        return;
    }
    path.append("components");
    path.append(static_cast<std::uint64_t>(location.first_source_index));
    path.append("fields");
    path.append(static_cast<std::uint64_t>(location.second_source_index));
    path.append("type");
}

JsonPointer type_node_path(const TypeLocation& location,
                           std::uint64_t list_depth) {
    JsonPointerBuilder path;
    append_type_root(path, location);
    for (std::uint64_t depth = UINT64_C(0); depth < list_depth; ++depth) {
        path.append("items");
    }
    return path.snapshot();
}

JsonPointer type_member_path(const TypeLocation& location,
                             std::uint64_t list_depth,
                             std::string_view member) {
    JsonPointerBuilder path;
    append_type_root(path, location);
    for (std::uint64_t depth = UINT64_C(0); depth < list_depth; ++depth) {
        path.append("items");
    }
    path.append(member);
    return path.snapshot();
}

Error duplicate_identifier(JsonPointer duplicate_path,
                           std::string id,
                           const JsonPointer& first_path) {
    return Error::from_details(
        FDB_PAYLOAD_E_DUPLICATE_ID, std::move(duplicate_path),
        "Payload identifier is duplicated",
        details({
            JsonValue::Member{"reason", JsonValue{"duplicate_id"}},
            JsonValue::Member{"id", JsonValue{std::move(id)}},
            JsonValue::Member{"first_declaration",
                              JsonValue{first_path.value()}},
        }));
}

Error unresolved_component(const TypeLocation& location,
                           std::uint64_t list_depth,
                           const TypeNode& type) {
    const std::string_view member =
        type.kind == TypeKind::component ? std::string_view{"id"}
                                         : std::string_view{"target"};
    return Error::from_details(
        FDB_PAYLOAD_E_UNRESOLVED_COMPONENT,
        type_member_path(location, list_depth, member),
        "Payload component target is unresolved",
        details({
            JsonValue::Member{"reason",
                              JsonValue{"unresolved_component"}},
            JsonValue::Member{"id", JsonValue{type.source_id}},
        }));
}

Error profile_violation(const TypeLocation& location,
                        std::uint64_t list_depth) {
    return Error::from_details(
        FDB_PAYLOAD_E_PROFILE_VIOLATION,
        type_node_path(location, list_depth),
        "Payload profile rejects reference type",
        details({
            JsonValue::Member{"reason", JsonValue{"ref_not_allowed"}},
            JsonValue::Member{"profile", JsonValue{"record.v1"}},
        }));
}

bool ascii_id_less(std::string_view left, std::string_view right) noexcept {
    const std::size_t shared = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < shared; ++index) {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);
        if (left_byte != right_byte) {
            return left_byte < right_byte;
        }
    }
    return left.size() < right.size();
}

struct AsciiIdLess final {
    bool operator()(std::string_view left,
                    std::string_view right) const noexcept {
        return ascii_id_less(left, right);
    }
};

using FirstIndexMap =
    std::map<std::string_view, std::uint32_t, AsciiIdLess>;

Result<void> check_entry_ids(std::vector<Entry>& entries) {
    FirstIndexMap first_indexes;
    for (std::size_t position = 0; position < entries.size(); ++position) {
        const auto index = static_cast<std::uint32_t>(position);
        Entry& entry = entries[position];
        entry.index = index;
        entry.source_index = index;
        const auto inserted =
            first_indexes.emplace(std::string_view{entry.id}, index);
        if (!inserted.second) {
            const JsonPointer first = entry_id_path(inserted.first->second);
            return Result<void>::failure(duplicate_identifier(
                entry_id_path(index), entry.id, first));
        }
    }
    return Result<void>::success();
}

Result<void> check_component_ids(std::vector<Component>& components) {
    FirstIndexMap first_indexes;
    for (std::size_t position = 0; position < components.size();
         ++position) {
        const auto source_index = static_cast<std::uint32_t>(position);
        Component& component = components[position];
        component.source_index = source_index;
        const auto inserted = first_indexes.emplace(
            std::string_view{component.id}, source_index);
        if (!inserted.second) {
            const JsonPointer first =
                component_id_path(inserted.first->second);
            return Result<void>::failure(duplicate_identifier(
                component_id_path(source_index), component.id, first));
        }
    }
    return Result<void>::success();
}

Result<void> check_field_ids(std::vector<Component>& components) {
    for (Component& component : components) {
        FirstIndexMap first_indexes;
        for (std::size_t position = 0; position < component.fields.size();
             ++position) {
            const auto index = static_cast<std::uint32_t>(position);
            Field& field = component.fields[position];
            field.index = index;
            field.source_index = index;
            const auto inserted = first_indexes.emplace(
                std::string_view{field.id}, index);
            if (!inserted.second) {
                const JsonPointer first = field_id_path(
                    component.source_index, inserted.first->second);
                return Result<void>::failure(duplicate_identifier(
                    field_id_path(component.source_index, index), field.id,
                    first));
            }
        }
    }
    return Result<void>::success();
}

void sort_and_index_components(std::vector<Component>& components) {
    std::sort(components.begin(), components.end(),
              [](const Component& left, const Component& right) {
                  return ascii_id_less(left.id, right.id);
              });
    for (std::size_t position = 0; position < components.size();
         ++position) {
        const auto index = static_cast<std::uint32_t>(position);
        Component& component = components[position];
        component.index = index;
    }
}

Result<void> resolve_type(TypeNode& root,
                          const TypeLocation& location,
                          const std::vector<Component>& components) {
    TypeNode* current = &root;
    std::uint64_t list_depth = UINT64_C(0);
    while (true) {
        if (current->kind == TypeKind::component ||
            current->kind == TypeKind::ref) {
            const auto found = std::lower_bound(
                components.begin(), components.end(),
                std::string_view{current->source_id},
                [](const Component& component, std::string_view id) {
                    return ascii_id_less(component.id, id);
                });
            if (found == components.end()) {
                return Result<void>::failure(
                    unresolved_component(location, list_depth, *current));
            }
            if (found->id != current->source_id) {
                return Result<void>::failure(
                    unresolved_component(location, list_depth, *current));
            }
            current->resolved_component_index = found->index;
        }
        if (current->kind != TypeKind::list || current->items == nullptr) {
            break;
        }
        current = current->items.get();
        ++list_depth;
    }
    return Result<void>::success();
}

Result<void> resolve_targets(SourceSpec& source,
                             const std::vector<Component>& components) {
    for (Entry& entry : source.entries) {
        auto resolved = resolve_type(
            entry.type, TypeLocation::entry(entry.source_index), components);
        if (!resolved.has_value()) {
            return resolved;
        }
    }
    for (Component& component : source.components) {
        for (Field& field : component.fields) {
            auto resolved = resolve_type(
                field.type,
                TypeLocation::field(component.source_index,
                                    field.source_index),
                components);
            if (!resolved.has_value()) {
                return resolved;
            }
        }
    }
    return Result<void>::success();
}

struct ContainmentEdge final {
    std::uint32_t target;
    std::uint32_t component_source_index;
    std::uint32_t field_source_index;
    std::uint64_t list_depth;
};

using ContainmentGraph = std::vector<std::vector<ContainmentEdge>>;

ContainmentGraph build_containment_graph(
    const std::vector<Component>& components) {
    ContainmentGraph graph(components.size());
    for (const Component& component : components) {
        std::vector<ContainmentEdge>& edges = graph[component.index];
        edges.reserve(component.fields.size());
        for (const Field& field : component.fields) {
            const TypeNode* current = &field.type;
            std::uint64_t list_depth = UINT64_C(0);
            while (current->kind == TypeKind::list &&
                   current->items != nullptr) {
                current = current->items.get();
                ++list_depth;
            }
            if (current->kind == TypeKind::component) {
                edges.push_back(ContainmentEdge{
                    current->resolved_component_index,
                    component.source_index, field.source_index, list_depth});
            }
        }
    }
    return graph;
}

Error by_value_cycle(const ContainmentEdge& closing_edge,
                     const std::vector<std::uint32_t>& active_nodes,
                     std::size_t cycle_start,
                     const std::vector<Component>& components) {
    JsonValue::Array cycle;
    cycle.reserve(active_nodes.size() - cycle_start + 1U);
    for (std::size_t index = cycle_start; index < active_nodes.size();
         ++index) {
        cycle.push_back(JsonValue{components[active_nodes[index]].id});
    }
    cycle.push_back(JsonValue{components[closing_edge.target].id});
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_TYPE,
        type_node_path(
            TypeLocation::field(closing_edge.component_source_index,
                                closing_edge.field_source_index),
            closing_edge.list_depth),
        "Payload by-value component containment is cyclic",
        details({
            JsonValue::Member{"reason", JsonValue{"by_value_cycle"}},
            JsonValue::Member{"cycle",
                              JsonValue::array(std::move(cycle))},
        }));
}

Result<std::vector<std::uint32_t>> detect_cycles(
    const ContainmentGraph& graph,
    const std::vector<Component>& components) {
    enum class Color : std::uint8_t { white, gray, black };
    struct Frame final {
        std::uint32_t node;
        std::size_t next_edge;
    };

    std::vector<Color> colors(components.size(), Color::white);
    std::vector<std::size_t> active_positions(
        components.size(), std::numeric_limits<std::size_t>::max());
    std::vector<Frame> stack;
    std::vector<std::uint32_t> active_nodes;
    std::vector<std::uint32_t> postorder;
    stack.reserve(components.size());
    active_nodes.reserve(components.size());
    postorder.reserve(components.size());

    for (std::size_t root_position = 0; root_position < components.size();
         ++root_position) {
        if (colors[root_position] != Color::white) {
            continue;
        }
        const auto root = static_cast<std::uint32_t>(root_position);
        colors[root_position] = Color::gray;
        active_positions[root_position] = active_nodes.size();
        active_nodes.push_back(root);
        stack.push_back(Frame{root, 0U});

        while (!stack.empty()) {
            Frame& frame = stack.back();
            const std::size_t node_position =
                static_cast<std::size_t>(frame.node);
            if (frame.next_edge >= graph[node_position].size()) {
                colors[node_position] = Color::black;
                active_positions[node_position] =
                    std::numeric_limits<std::size_t>::max();
                postorder.push_back(frame.node);
                stack.pop_back();
                active_nodes.pop_back();
                continue;
            }

            const ContainmentEdge edge =
                graph[node_position][frame.next_edge];
            ++frame.next_edge;
            const std::size_t target_position =
                static_cast<std::size_t>(edge.target);
            if (colors[target_position] == Color::white) {
                colors[target_position] = Color::gray;
                active_positions[target_position] = active_nodes.size();
                active_nodes.push_back(edge.target);
                stack.push_back(Frame{edge.target, 0U});
                continue;
            }
            if (colors[target_position] == Color::gray) {
                return Result<std::vector<std::uint32_t>>::failure(
                    by_value_cycle(edge, active_nodes,
                                   active_positions[target_position],
                                   components));
            }
        }
    }
    return Result<std::vector<std::uint32_t>>::success(
        std::move(postorder));
}

Result<void> validate_profile_type(const TypeNode& root,
                                   const TypeLocation& location) {
    const TypeNode* current = &root;
    std::uint64_t list_depth = UINT64_C(0);
    while (true) {
        if (current->kind == TypeKind::ref) {
            return Result<void>::failure(
                profile_violation(location, list_depth));
        }
        if (current->kind != TypeKind::list || current->items == nullptr) {
            break;
        }
        current = current->items.get();
        ++list_depth;
    }
    return Result<void>::success();
}

Result<void> validate_profile(const SourceSpec& source) {
    if (source.profile == Profile::object_graph_v1) {
        return Result<void>::success();
    }
    for (const Entry& entry : source.entries) {
        auto valid = validate_profile_type(
            entry.type, TypeLocation::entry(entry.source_index));
        if (!valid.has_value()) {
            return valid;
        }
    }
    for (const Component& component : source.components) {
        for (const Field& field : component.fields) {
            auto valid = validate_profile_type(
                field.type,
                TypeLocation::field(component.source_index,
                                    field.source_index));
            if (!valid.has_value()) {
                return valid;
            }
        }
    }
    return Result<void>::success();
}

bool has_intrinsic_variable_width(const TypeNode& root) noexcept {
    if (root.kind == TypeKind::list) {
        return true;
    }
    return root.kind == TypeKind::str || root.kind == TypeKind::wstr ||
           root.kind == TypeKind::bytes;
}

void derive_component_variable_width(
    std::vector<Component>& components,
    const ContainmentGraph& graph,
    const std::vector<std::uint32_t>& postorder) noexcept {
    for (Component& component : components) {
        component.variable_width = false;
        for (const Field& field : component.fields) {
            if (has_intrinsic_variable_width(field.type)) {
                component.variable_width = true;
                break;
            }
        }
    }
    for (const std::uint32_t component_index : postorder) {
        Component& component = components[component_index];
        for (const ContainmentEdge& edge : graph[component_index]) {
            if (components[edge.target].variable_width) {
                component.variable_width = true;
                break;
            }
        }
    }
}

void assign_type_variable_width(
    TypeNode& root,
    const std::vector<Component>& components) noexcept {
    TypeNode* current = &root;
    while (current->kind == TypeKind::list && current->items != nullptr) {
        current->variable_width = true;
        current = current->items.get();
    }
    if (current->kind == TypeKind::str || current->kind == TypeKind::wstr ||
        current->kind == TypeKind::bytes) {
        current->variable_width = true;
    } else if (current->kind == TypeKind::component) {
        current->variable_width =
            components[current->resolved_component_index].variable_width;
    } else {
        current->variable_width = false;
    }
}

void accumulate_facts(const TypeNode& root, SemanticFacts& facts) noexcept {
    const TypeNode* current = &root;
    while (true) {
        facts.has_nullable = facts.has_nullable || current->nullable;
        facts.has_variable_width =
            facts.has_variable_width || current->variable_width;
        if (current->kind == TypeKind::list) {
            facts.has_lists = true;
        } else if (current->kind == TypeKind::ref) {
            facts.has_references = true;
        } else if (current->kind == TypeKind::u8n ||
                   current->kind == TypeKind::u16n) {
            facts.has_normalized_integers = true;
        }
        if (current->kind != TypeKind::list || current->items == nullptr) {
            break;
        }
        current = current->items.get();
    }
}

SemanticFacts derive_facts(SourceSpec& source,
                           const ContainmentGraph& graph,
                           const std::vector<std::uint32_t>& postorder) {
    derive_component_variable_width(source.components, graph, postorder);
    for (Entry& entry : source.entries) {
        assign_type_variable_width(entry.type, source.components);
    }
    for (Component& component : source.components) {
        for (Field& field : component.fields) {
            assign_type_variable_width(field.type, source.components);
        }
    }

    SemanticFacts facts;
    for (const Entry& entry : source.entries) {
        accumulate_facts(entry.type, facts);
    }
    for (const Component& component : source.components) {
        for (const Field& field : component.fields) {
            accumulate_facts(field.type, facts);
        }
    }
    facts.semantic_flags =
        (facts.has_nullable ? FDB_PAYLOAD_SEMANTIC_HAS_NULLABLE
                            : UINT64_C(0)) |
        (facts.has_lists ? FDB_PAYLOAD_SEMANTIC_HAS_LISTS : UINT64_C(0)) |
        (facts.has_references ? FDB_PAYLOAD_SEMANTIC_HAS_REFERENCES
                              : UINT64_C(0)) |
        (facts.has_variable_width
             ? FDB_PAYLOAD_SEMANTIC_HAS_VARIABLE_WIDTH
             : UINT64_C(0)) |
        (facts.has_normalized_integers
             ? FDB_PAYLOAD_SEMANTIC_HAS_NORMALIZED_INTEGERS
             : UINT64_C(0));
    return facts;
}

}  // namespace

Result<ResolvedSpec> resolve_source(SourceSpec source) {
    auto entry_ids = check_entry_ids(source.entries);
    if (!entry_ids.has_value()) {
        return Result<ResolvedSpec>::failure(std::move(entry_ids).error());
    }
    auto component_ids = check_component_ids(source.components);
    if (!component_ids.has_value()) {
        return Result<ResolvedSpec>::failure(
            std::move(component_ids).error());
    }
    auto field_ids = check_field_ids(source.components);
    if (!field_ids.has_value()) {
        return Result<ResolvedSpec>::failure(std::move(field_ids).error());
    }

    sort_and_index_components(source.components);
    auto targets = resolve_targets(source, source.components);
    if (!targets.has_value()) {
        return Result<ResolvedSpec>::failure(std::move(targets).error());
    }

    const ContainmentGraph graph =
        build_containment_graph(source.components);
    auto cycle_result = detect_cycles(graph, source.components);
    if (!cycle_result.has_value()) {
        return Result<ResolvedSpec>::failure(
            std::move(cycle_result).error());
    }
    std::vector<std::uint32_t> postorder =
        std::move(cycle_result).value();

    auto profile = validate_profile(source);
    if (!profile.has_value()) {
        return Result<ResolvedSpec>::failure(std::move(profile).error());
    }

    SemanticFacts facts = derive_facts(source, graph, postorder);
    return Result<ResolvedSpec>::success(
        ResolvedSpec(std::move(source), facts));
}

JsonValue normalized_source_json(const ResolvedSpec& source) {
    return normalized_source_json(source.source());
}

}  // namespace fastdb::payload::spec
