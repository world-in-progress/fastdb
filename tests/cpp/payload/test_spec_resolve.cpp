#include "TestSupport.hpp"

#include "payload/json/Jcs.hpp"
#include "payload/json/JsonDocument.hpp"
#include "payload/spec/Model.hpp"
#include "payload/spec/Parse.hpp"
#include "payload/spec/Resolve.hpp"

#include <fastdb_payload.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace allocation_guard {

thread_local bool enabled = false;
thread_local std::size_t remaining = 0;

void* allocate(std::size_t size) {
    if (enabled) {
        if (size > remaining) {
            throw std::bad_alloc();
        }
        remaining -= size;
    }
    void* const allocation = std::malloc(size == 0U ? 1U : size);
    if (allocation == nullptr) {
        throw std::bad_alloc();
    }
    return allocation;
}

class Budget final {
public:
    explicit Budget(std::size_t bytes) {
        remaining = bytes;
        enabled = true;
    }

    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;

    ~Budget() { enabled = false; }
};

}  // namespace allocation_guard

void* operator new(std::size_t size) {
    return allocation_guard::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_guard::allocate(size);
}

void operator delete(void* allocation) noexcept { std::free(allocation); }

void operator delete[](void* allocation) noexcept { std::free(allocation); }

void operator delete(void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

namespace {

using fastdb::payload::error::Error;
using fastdb::payload::error::Result;
using fastdb::payload::json::JsonDocument;
using fastdb::payload::json::JsonParseLimits;
using fastdb::payload::json::jcs_serialize;
using fastdb::payload::spec::Cardinality;
using fastdb::payload::spec::Component;
using fastdb::payload::spec::Entry;
using fastdb::payload::spec::Field;
using fastdb::payload::spec::Profile;
using fastdb::payload::spec::ResolvedSpec;
using fastdb::payload::spec::SemanticFacts;
using fastdb::payload::spec::SourceSpec;
using fastdb::payload::spec::TypeKind;
using fastdb::payload::spec::TypeNode;
using fastdb::payload::spec::normalized_source_json;
using fastdb::payload::spec::parse_and_normalize_source;
using fastdb::payload::spec::resolve_source;

static_assert(!std::is_copy_constructible_v<ResolvedSpec>);
static_assert(!std::is_copy_assignable_v<ResolvedSpec>);
static_assert(std::is_nothrow_move_constructible_v<ResolvedSpec>);
static_assert(std::is_nothrow_move_assignable_v<ResolvedSpec>);

std::string load_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string fixture(std::string_view relative_path) {
    return load_file(std::string(FASTDB_PAYLOAD_SPEC_FIXTURE_DIR) + "/" +
                     std::string(relative_path));
}

Result<SourceSpec> parse_source(std::string_view source,
                                JsonParseLimits json_limits = {}) {
    auto document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), json_limits);
    if (!document.has_value()) {
        return Result<SourceSpec>::failure(std::move(document).error());
    }
    return parse_and_normalize_source(document.value());
}

Result<ResolvedSpec> parse_resolved(std::string_view source,
                                    JsonParseLimits json_limits = {}) {
    auto parsed = parse_source(source, json_limits);
    if (!parsed.has_value()) {
        return Result<ResolvedSpec>::failure(std::move(parsed).error());
    }
    return resolve_source(std::move(parsed).value());
}

template <typename T>
bool has_error(const Result<T>& result,
               std::uint32_t code,
               std::string_view path,
               std::string_view message,
               std::string_view details) {
    if (result.has_value()) {
        return false;
    }
    const Error& error = result.error();
    return error.code() == code && error.path() == path &&
           error.message() == message && error.details_json() == details;
}

std::string canonical_source(const ResolvedSpec& source) {
    const auto serialized = jcs_serialize(normalized_source_json(source));
    const auto* const bytes = std::get_if<std::string>(&serialized);
    return bytes == nullptr ? std::string{} : *bytes;
}

const TypeNode* terminal_type(const TypeNode& root) {
    const TypeNode* current = &root;
    while (current->kind == TypeKind::list && current->items != nullptr) {
        current = current->items.get();
    }
    return current;
}

std::string deep_list_source(std::uint32_t list_depth) {
    std::string source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"deep","cardinality":"one","type":)";
    for (std::uint32_t depth = UINT32_C(0); depth < list_depth; ++depth) {
        source += R"({"kind":"list","items":)";
    }
    source += R"({"kind":"component","id":"Leaf"})";
    source.append(static_cast<std::size_t>(list_depth), '}');
    source +=
        R"(}],"components":[{"id":"Leaf","kind":"record","fields":[]}]})";
    return source;
}

std::string chain_id(std::uint32_t index) {
    std::array<char, 16> buffer{};
    const int length = std::snprintf(buffer.data(), buffer.size(), "C%05u",
                                     static_cast<unsigned int>(index));
    if (length <= 0) {
        return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

SourceSpec component_chain(std::uint32_t component_count, bool cycle) {
    std::vector<Component> components;
    components.reserve(component_count);
    for (std::uint32_t index = UINT32_C(0); index < component_count;
         ++index) {
        TypeNode type(TypeKind::str, false);
        if (index + UINT32_C(1) < component_count || cycle) {
            type.kind = TypeKind::component;
            type.source_id =
                index + UINT32_C(1) < component_count
                    ? chain_id(index + UINT32_C(1))
                    : chain_id(UINT32_C(0));
        }
        std::vector<Field> fields;
        fields.emplace_back("next", std::move(type));
        components.emplace_back(chain_id(index), std::move(fields));
    }
    return SourceSpec(Profile::record_v1, {}, std::move(components));
}

bool facts_equal(const SemanticFacts& facts,
                 bool nullable,
                 bool lists,
                 bool references,
                 bool variable_width,
                 bool normalized_integers) {
    std::uint64_t expected_flags = UINT64_C(0);
    if (nullable) {
        expected_flags |= FDB_PAYLOAD_SEMANTIC_HAS_NULLABLE;
    }
    if (lists) {
        expected_flags |= FDB_PAYLOAD_SEMANTIC_HAS_LISTS;
    }
    if (references) {
        expected_flags |= FDB_PAYLOAD_SEMANTIC_HAS_REFERENCES;
    }
    if (variable_width) {
        expected_flags |= FDB_PAYLOAD_SEMANTIC_HAS_VARIABLE_WIDTH;
    }
    if (normalized_integers) {
        expected_flags |= FDB_PAYLOAD_SEMANTIC_HAS_NORMALIZED_INTEGERS;
    }
    return facts.has_nullable == nullable && facts.has_lists == lists &&
           facts.has_references == references &&
           facts.has_variable_width == variable_width &&
           facts.has_normalized_integers == normalized_integers &&
           facts.semantic_flags == expected_flags;
}

int test_stable_indexes_and_normalized_order() {
    auto order_a =
        parse_resolved(fixture("valid/component-order-a.source.json"));
    auto order_b =
        parse_resolved(fixture("valid/component-order-b.source.json"));
    require(order_a.has_value());
    require(order_b.has_value());

    const auto& components_a = order_a.value().components();
    const auto& components_b = order_b.value().components();
    require(components_a.size() == 2U);
    require(components_b.size() == 2U);
    require(components_a[0].id == "Alpha");
    require(components_a[0].index == UINT32_C(0));
    require(components_a[0].source_index == UINT32_C(1));
    require(components_a[1].id == "Zed");
    require(components_a[1].index == UINT32_C(1));
    require(components_a[1].source_index == UINT32_C(0));
    require(components_b[0].id == "Alpha");
    require(components_b[0].source_index == UINT32_C(0));
    require(components_b[1].id == "Zed");
    require(components_b[1].source_index == UINT32_C(1));
    require(components_a[1].fields[0].id == "second");
    require(components_a[1].fields[0].index == UINT32_C(0));
    require(components_a[1].fields[0].source_index == UINT32_C(0));
    require(components_a[1].fields[1].id == "first");
    require(components_a[1].fields[1].index == UINT32_C(1));
    require(components_a[1].fields[1].source_index == UINT32_C(1));
    require(components_a[1].fields[1].type.source_id == "Alpha");
    require(components_a[1].fields[1].type.resolved_component_index ==
            UINT32_C(0));
    require(order_a.value().entries()[0].index == UINT32_C(0));
    require(order_a.value().entries()[0].type.resolved_component_index ==
            UINT32_C(1));

    const std::string canonical_a = canonical_source(order_a.value());
    const std::string canonical_b = canonical_source(order_b.value());
    require(!canonical_a.empty());
    require(canonical_a == canonical_b);
    require(canonical_a.find("component_index") == std::string::npos);
    require(canonical_a.find("resolved_component_index") ==
            std::string::npos);
    require(canonical_a.find("source_index") == std::string::npos);
    require(canonical_a.find("variable_width") == std::string::npos);
    require(canonical_a.find(R"("index")") == std::string::npos);

    auto entries_a =
        parse_resolved(fixture("valid/entry-order-a.source.json"));
    auto entries_b =
        parse_resolved(fixture("valid/entry-order-b.source.json"));
    require(entries_a.has_value());
    require(entries_b.has_value());
    require(entries_a.value().entries()[0].id == "alpha");
    require(entries_a.value().entries()[0].index == UINT32_C(0));
    require(entries_a.value().entries()[1].id == "beta");
    require(entries_a.value().entries()[1].index == UINT32_C(1));
    require(entries_b.value().entries()[0].id == "beta");
    require(entries_b.value().entries()[0].index == UINT32_C(0));
    require(canonical_source(entries_a.value()) !=
            canonical_source(entries_b.value()));

    auto fields_a = parse_resolved(R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[{"id":"Record","kind":"record","fields":[
        {"id":"first","type":{"kind":"u8"}},
        {"id":"second","type":{"kind":"u16"}}
      ]}]
    })");
    auto fields_b = parse_resolved(R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[{"id":"Record","kind":"record","fields":[
        {"id":"second","type":{"kind":"u16"}},
        {"id":"first","type":{"kind":"u8"}}
      ]}]
    })");
    require(fields_a.has_value());
    require(fields_b.has_value());
    require(fields_a.value().components()[0].fields[0].id == "first");
    require(fields_b.value().components()[0].fields[0].id == "second");
    require(canonical_source(fields_a.value()) !=
            canonical_source(fields_b.value()));

    auto ascii = parse_resolved(R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[
        {"id":"a","kind":"record","fields":[]},
        {"id":"_Tail","kind":"record","fields":[]},
        {"id":"Z","kind":"record","fields":[]}
      ]
    })");
    require(ascii.has_value());
    require(ascii.value().components()[0].id == "Z");
    require(ascii.value().components()[1].id == "_Tail");
    require(ascii.value().components()[2].id == "a");
    return EXIT_SUCCESS;
}

int test_duplicate_ids_and_validation_order() {
    require(has_error(
        parse_resolved(fixture("invalid/duplicate-entry.source.json")),
        FDB_PAYLOAD_E_DUPLICATE_ID, "/entries/1/id",
        "Payload identifier is duplicated",
        R"({"first_declaration":"/entries/0/id","id":"same","reason":"duplicate_id"})"));
    require(has_error(
        parse_resolved(fixture("invalid/duplicate-component.source.json")),
        FDB_PAYLOAD_E_DUPLICATE_ID, "/components/1/id",
        "Payload identifier is duplicated",
        R"({"first_declaration":"/components/0/id","id":"Same","reason":"duplicate_id"})"));
    require(has_error(
        parse_resolved(fixture("invalid/duplicate-field.source.json")),
        FDB_PAYLOAD_E_DUPLICATE_ID, "/components/0/fields/1/id",
        "Payload identifier is duplicated",
        R"({"first_declaration":"/components/0/fields/0/id","id":"same","reason":"duplicate_id"})"));

    const std::string all_duplicates = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1",
      "entries":[
        {"id":"e","cardinality":"one","type":{"kind":"u8"}},
        {"id":"e","cardinality":"one","type":{"kind":"u8"}}
      ],
      "components":[
        {"id":"C","kind":"record","fields":[
          {"id":"f","type":{"kind":"u8"}},
          {"id":"f","type":{"kind":"u8"}}
        ]},
        {"id":"C","kind":"record","fields":[]}
      ]
    })";
    require(has_error(
        parse_resolved(all_duplicates), FDB_PAYLOAD_E_DUPLICATE_ID,
        "/entries/1/id", "Payload identifier is duplicated",
        R"({"first_declaration":"/entries/0/id","id":"e","reason":"duplicate_id"})"));

    const std::string component_before_field = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[
        {"id":"C","kind":"record","fields":[
          {"id":"f","type":{"kind":"u8"}},
          {"id":"f","type":{"kind":"u16"}}
        ]},
        {"id":"C","kind":"record","fields":[]}
      ]
    })";
    require(has_error(
        parse_resolved(component_before_field), FDB_PAYLOAD_E_DUPLICATE_ID,
        "/components/1/id", "Payload identifier is duplicated",
        R"({"first_declaration":"/components/0/id","id":"C","reason":"duplicate_id"})"));
    return EXIT_SUCCESS;
}

int test_target_resolution_and_object_graph_ref_cycles() {
    auto result = parse_resolved(R"({
      "schema":"fastdb.payload.v1","profile":"object_graph.v1",
      "entries":[
        {"id":"root","cardinality":"one","type":{"kind":"component","id":"Node"}},
        {"id":"pairs","cardinality":"many","type":{"kind":"list","items":{"kind":"component","id":"Pair"}}},
        {"id":"edge","cardinality":"one","type":{"kind":"ref","target":"Node","nullable":true}}
      ],
      "components":[
        {"id":"Pair","kind":"record","fields":[
          {"id":"peer","type":{"kind":"ref","target":"Pair"}},
          {"id":"node","type":{"kind":"ref","target":"Node"}}
        ]},
        {"id":"Node","kind":"record","fields":[
          {"id":"self","type":{"kind":"ref","target":"Node"}},
          {"id":"children","type":{"kind":"list","items":{"kind":"ref","target":"Node"}}},
          {"id":"pair_value","type":{"kind":"component","id":"Pair"}},
          {"id":"pair_ref","type":{"kind":"ref","target":"Pair"}}
        ]}
      ]
    })");
    require(result.has_value());
    const ResolvedSpec& resolved = result.value();
    require(resolved.profile() == Profile::object_graph_v1);
    require(resolved.components().size() == 2U);
    require(resolved.components()[0].id == "Node");
    require(resolved.components()[0].index == UINT32_C(0));
    require(resolved.components()[0].source_index == UINT32_C(1));
    require(resolved.components()[1].id == "Pair");
    require(resolved.components()[1].index == UINT32_C(1));
    require(resolved.components()[1].source_index == UINT32_C(0));

    require(resolved.entries()[0].type.source_id == "Node");
    require(resolved.entries()[0].type.resolved_component_index ==
            UINT32_C(0));
    const TypeNode* pairs = terminal_type(resolved.entries()[1].type);
    require(pairs->source_id == "Pair");
    require(pairs->resolved_component_index == UINT32_C(1));
    require(resolved.entries()[2].type.source_id == "Node");
    require(resolved.entries()[2].type.resolved_component_index ==
            UINT32_C(0));

    const Component& node = resolved.components()[0];
    require(node.fields[0].type.kind == TypeKind::ref);
    require(node.fields[0].type.resolved_component_index == UINT32_C(0));
    require(terminal_type(node.fields[1].type)->resolved_component_index ==
            UINT32_C(0));
    require(node.fields[2].type.kind == TypeKind::component);
    require(node.fields[2].type.resolved_component_index == UINT32_C(1));
    require(node.fields[3].type.kind == TypeKind::ref);
    require(node.fields[3].type.resolved_component_index == UINT32_C(1));
    const Component& pair = resolved.components()[1];
    require(pair.fields[0].type.resolved_component_index == UINT32_C(1));
    require(pair.fields[1].type.resolved_component_index == UINT32_C(0));
    return EXIT_SUCCESS;
}

int test_unresolved_targets_keep_original_paths() {
    require(has_error(
        parse_resolved(fixture("invalid/unresolved-component.source.json")),
        FDB_PAYLOAD_E_UNRESOLVED_COMPONENT, "/entries/0/type/items/id",
        "Payload component target is unresolved",
        R"({"id":"Missing","reason":"unresolved_component"})"));

    const std::string unresolved_ref_after_sort = R"({
      "schema":"fastdb.payload.v1","profile":"object_graph.v1","entries":[],
      "components":[
        {"id":"Zulu","kind":"record","fields":[
          {"id":"bad","type":{"kind":"list","items":{"kind":"list","items":{"kind":"ref","target":"Missing"}}}}
        ]},
        {"id":"Alpha","kind":"record","fields":[]}
      ]
    })";
    require(has_error(
        parse_resolved(unresolved_ref_after_sort),
        FDB_PAYLOAD_E_UNRESOLVED_COMPONENT,
        "/components/0/fields/0/type/items/items/target",
        "Payload component target is unresolved",
        R"({"id":"Missing","reason":"unresolved_component"})"));
    return EXIT_SUCCESS;
}

int test_by_value_cycles_are_deterministic() {
    require(has_error(
        parse_resolved(fixture("invalid/by-value-cycle.source.json")),
        FDB_PAYLOAD_E_INVALID_TYPE, "/components/0/fields/0/type",
        "Payload by-value component containment is cyclic",
        R"({"cycle":["A","A"],"reason":"by_value_cycle"})"));

    const std::string list_cycle = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[{"id":"A","kind":"record","fields":[
        {"id":"again","type":{"kind":"list","items":{"kind":"list","items":{"kind":"component","id":"A"}}}}
      ]}]
    })";
    require(has_error(
        parse_resolved(list_cycle), FDB_PAYLOAD_E_INVALID_TYPE,
        "/components/0/fields/0/type/items/items",
        "Payload by-value component containment is cyclic",
        R"({"cycle":["A","A"],"reason":"by_value_cycle"})"));

    const std::string multi_cycle = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[
        {"id":"C","kind":"record","fields":[{"id":"a","type":{"kind":"component","id":"A"}}]},
        {"id":"A","kind":"record","fields":[{"id":"b","type":{"kind":"component","id":"B"}}]},
        {"id":"B","kind":"record","fields":[{"id":"c","type":{"kind":"component","id":"C"}}]}
      ]
    })";
    require(has_error(
        parse_resolved(multi_cycle), FDB_PAYLOAD_E_INVALID_TYPE,
        "/components/0/fields/0/type",
        "Payload by-value component containment is cyclic",
        R"({"cycle":["A","B","C","A"],"reason":"by_value_cycle"})"));

    const std::string field_order_cycle = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[
        {"id":"C","kind":"record","fields":[{"id":"a","type":{"kind":"component","id":"A"}}]},
        {"id":"A","kind":"record","fields":[
          {"id":"first","type":{"kind":"component","id":"B"}},
          {"id":"second","type":{"kind":"component","id":"C"}}
        ]},
        {"id":"B","kind":"record","fields":[{"id":"a","type":{"kind":"component","id":"A"}}]}
      ]
    })";
    require(has_error(
        parse_resolved(field_order_cycle), FDB_PAYLOAD_E_INVALID_TYPE,
        "/components/2/fields/0/type",
        "Payload by-value component containment is cyclic",
        R"({"cycle":["A","B","A"],"reason":"by_value_cycle"})"));
    return EXIT_SUCCESS;
}

int test_record_profile_rejects_every_reference_root() {
    require(has_error(
        parse_resolved(fixture("invalid/record-ref.source.json")),
        FDB_PAYLOAD_E_PROFILE_VIOLATION, "/entries/0/type",
        "Payload profile rejects reference type",
        R"({"profile":"record.v1","reason":"ref_not_allowed"})"));

    const std::string unused_field_ref = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[
        {"id":"Zulu","kind":"record","fields":[
          {"id":"hidden","type":{"kind":"ref","target":"Alpha"}}
        ]},
        {"id":"Alpha","kind":"record","fields":[]}
      ]
    })";
    require(has_error(
        parse_resolved(unused_field_ref), FDB_PAYLOAD_E_PROFILE_VIOLATION,
        "/components/0/fields/0/type",
        "Payload profile rejects reference type",
        R"({"profile":"record.v1","reason":"ref_not_allowed"})"));

    const std::string nested_ref = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[
        {"id":"Node","kind":"record","fields":[
          {"id":"hidden","type":{"kind":"list","items":{"kind":"list","items":{"kind":"ref","target":"Node"}}}}
        ]}
      ]
    })";
    require(has_error(
        parse_resolved(nested_ref), FDB_PAYLOAD_E_PROFILE_VIOLATION,
        "/components/0/fields/0/type/items/items",
        "Payload profile rejects reference type",
        R"({"profile":"record.v1","reason":"ref_not_allowed"})"));
    return EXIT_SUCCESS;
}

int test_all_five_semantic_facts_and_variable_width_propagation() {
    auto fixed = parse_resolved(R"({
      "schema":"fastdb.payload.v1","profile":"record.v1",
      "entries":[{"id":"x","cardinality":"one","type":{"kind":"u32"}}],
      "components":[]
    })");
    require(fixed.has_value());
    require(facts_equal(fixed.value().facts(), false, false, false, false,
                        false));

    auto reference_only = parse_resolved(R"({
      "schema":"fastdb.payload.v1","profile":"object_graph.v1",
      "entries":[{"id":"x","cardinality":"one","type":{"kind":"ref","target":"Empty"}}],
      "components":[{"id":"Empty","kind":"record","fields":[]}]
    })");
    require(reference_only.has_value());
    require(facts_equal(reference_only.value().facts(), false, false, true,
                        false, false));
    require(!reference_only.value().entries()[0].type.variable_width);

    auto all = parse_resolved(R"({
      "schema":"fastdb.payload.v1","profile":"object_graph.v1","entries":[],
      "components":[
        {"id":"Empty","kind":"record","fields":[]},
        {"id":"Unused","kind":"record","fields":[
          {"id":"nullable","type":{"kind":"u8","nullable":true}},
          {"id":"items","type":{"kind":"list","items":{"kind":"u16"}}},
          {"id":"normalized","type":{"kind":"u8n","min":0,"max":1}},
          {"id":"reference","type":{"kind":"ref","target":"Empty"}},
          {"id":"bytes","type":{"kind":"bytes"}}
        ]}
      ]
    })");
    require(all.has_value());
    require(facts_equal(all.value().facts(), true, true, true, true, true));

    auto transitive = parse_resolved(R"({
      "schema":"fastdb.payload.v1","profile":"record.v1",
      "entries":[{"id":"root","cardinality":"one","type":{"kind":"component","id":"A"}}],
      "components":[
        {"id":"A","kind":"record","fields":[{"id":"b","type":{"kind":"component","id":"B"}}]},
        {"id":"B","kind":"record","fields":[{"id":"c","type":{"kind":"component","id":"C"}}]},
        {"id":"C","kind":"record","fields":[{"id":"text","type":{"kind":"str"}}]}
      ]
    })");
    require(transitive.has_value());
    require(facts_equal(transitive.value().facts(), false, false, false, true,
                        false));
    require(transitive.value().entries()[0].type.variable_width);
    require(transitive.value().components()[0].variable_width);
    require(transitive.value().components()[1].variable_width);
    require(transitive.value().components()[2].variable_width);
    require(transitive.value().components()[0]
                .fields[0]
                .type.variable_width);
    require(transitive.value().components()[1]
                .fields[0]
                .type.variable_width);
    return EXIT_SUCCESS;
}

int test_deep_list_resolution_is_stack_safe_and_linear() {
    constexpr std::uint32_t list_depth = UINT32_C(20000);
    const std::string source = deep_list_source(list_depth);
    JsonParseLimits limits;
    limits.max_nesting_depth = list_depth + UINT32_C(16);

    auto parsed = parse_source(source, limits);
    require(parsed.has_value());
    bool threw = false;
    bool resolved_exactly = false;
    try {
        allocation_guard::Budget budget(24U * 1024U * 1024U);
        auto resolved = resolve_source(std::move(parsed).value());
        if (resolved.has_value()) {
            const TypeNode* terminal =
                terminal_type(resolved.value().entries()[0].type);
            resolved_exactly = terminal->kind == TypeKind::component &&
                               terminal->source_id == "Leaf" &&
                               terminal->resolved_component_index ==
                                   UINT32_C(0) &&
                               resolved.value().facts().has_lists &&
                               resolved.value().facts().has_variable_width;
        }
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    require(!threw);
    require(resolved_exactly);

    auto normalized = parse_resolved(source, limits);
    require(normalized.has_value());
    const std::string canonical = canonical_source(normalized.value());
    require(!canonical.empty());
    require(canonical.find(R"("id":"Leaf","kind":"component","nullable":false)") !=
            std::string::npos);
    return EXIT_SUCCESS;
}

int test_long_component_chains_are_stack_safe_and_linear() {
    constexpr std::uint32_t chain_length = UINT32_C(20000);
    bool acyclic_threw = false;
    bool acyclic_exact = false;
    SourceSpec acyclic = component_chain(chain_length, false);
    try {
        allocation_guard::Budget budget(128U * 1024U * 1024U);
        auto resolved = resolve_source(std::move(acyclic));
        if (resolved.has_value()) {
            const auto& components = resolved.value().components();
            acyclic_exact = components.size() == chain_length &&
                            components.front().id == "C00000" &&
                            components.back().id == "C19999" &&
                            components.front().variable_width &&
                            components.front().fields[0].type.variable_width &&
                            resolved.value().facts().has_variable_width;
        }
    } catch (const std::bad_alloc&) {
        acyclic_threw = true;
    }
    require(!acyclic_threw);
    require(acyclic_exact);

    constexpr std::uint32_t cycle_length = UINT32_C(12000);
    bool cycle_threw = false;
    bool cycle_exact = false;
    SourceSpec cyclic = component_chain(cycle_length, true);
    try {
        allocation_guard::Budget budget(128U * 1024U * 1024U);
        auto result = resolve_source(std::move(cyclic));
        if (!result.has_value()) {
            const Error& error = result.error();
            const std::string_view details = error.details_json();
            cycle_exact =
                error.code() == FDB_PAYLOAD_E_INVALID_TYPE &&
                error.path() == "/components/11999/fields/0/type" &&
                error.message() ==
                    "Payload by-value component containment is cyclic" &&
                details.find(R"("cycle":["C00000","C00001")") !=
                    std::string_view::npos &&
                details.find(R"("C11999","C00000"],"reason":"by_value_cycle")") !=
                    std::string_view::npos;
        }
    } catch (const std::bad_alloc&) {
        cycle_threw = true;
    }
    require(!cycle_threw);
    require(cycle_exact);
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string_view selected{argv[1]};
        if (selected == "deep") {
            return test_deep_list_resolution_is_stack_safe_and_linear();
        }
        if (selected == "chains") {
            return test_long_component_chains_are_stack_safe_and_linear();
        }
        return EXIT_FAILURE;
    }
    require(test_stable_indexes_and_normalized_order() == EXIT_SUCCESS);
    require(test_duplicate_ids_and_validation_order() == EXIT_SUCCESS);
    require(test_target_resolution_and_object_graph_ref_cycles() ==
            EXIT_SUCCESS);
    require(test_unresolved_targets_keep_original_paths() == EXIT_SUCCESS);
    require(test_by_value_cycles_are_deterministic() == EXIT_SUCCESS);
    require(test_record_profile_rejects_every_reference_root() ==
            EXIT_SUCCESS);
    require(test_all_five_semantic_facts_and_variable_width_propagation() ==
            EXIT_SUCCESS);
    require(test_deep_list_resolution_is_stack_safe_and_linear() ==
            EXIT_SUCCESS);
    require(test_long_component_chains_are_stack_safe_and_linear() ==
            EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
