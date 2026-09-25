#include "TestSupport.hpp"

#include "payload/json/Jcs.hpp"
#include "payload/json/JsonDocument.hpp"
#include "payload/spec/Model.hpp"
#include "payload/spec/Parse.hpp"

#include <fastdb_payload.h>

#include <array>
#include <cmath>
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
using fastdb::payload::spec::Profile;
using fastdb::payload::spec::SourceParseLimits;
using fastdb::payload::spec::SourceSpec;
using fastdb::payload::spec::TypeKind;
using fastdb::payload::spec::TypeNode;
using fastdb::payload::spec::normalized_source_json;
using fastdb::payload::spec::parse_and_normalize_source;

static_assert(!std::is_copy_constructible_v<TypeNode>);
static_assert(!std::is_copy_assignable_v<TypeNode>);
static_assert(std::is_nothrow_move_constructible_v<TypeNode>);
static_assert(std::is_nothrow_move_assignable_v<TypeNode>);
static_assert(!std::is_copy_constructible_v<SourceSpec>);
static_assert(!std::is_copy_assignable_v<SourceSpec>);
static_assert(std::is_nothrow_move_constructible_v<SourceSpec>);
static_assert(std::is_nothrow_move_assignable_v<SourceSpec>);

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
                                SourceParseLimits limits = {}) {
    auto document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()));
    if (!document.has_value()) {
        return Result<SourceSpec>::failure(std::move(document).error());
    }
    return parse_and_normalize_source(document.value(), limits);
}

bool has_error(const Result<SourceSpec>& result,
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

std::string canonical_source(const SourceSpec& source) {
    const auto serialized = jcs_serialize(normalized_source_json(source));
    const auto* const bytes = std::get_if<std::string>(&serialized);
    return bytes == nullptr ? std::string{} : *bytes;
}

std::string deep_list_source(
    std::uint32_t list_depth,
    std::string_view terminal_type = R"({"kind":"u8"})") {
    std::string source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"deep","cardinality":"one","type":)";
    for (std::uint32_t depth = UINT32_C(0); depth < list_depth; ++depth) {
        source += R"({"kind":"list","items":)";
    }
    source += terminal_type;
    source.append(static_cast<std::size_t>(list_depth), '}');
    source += R"(}],"components":[]})";
    return source;
}

int test_empty_source_and_complete_record_algebra() {
    const std::string empty_source = fixture("valid/empty-record.source.json");
    require(!empty_source.empty());
    auto empty = parse_source(empty_source);
    require(empty.has_value());
    require(empty.value().profile == Profile::record_v1);
    require(empty.value().entries.empty());
    require(empty.value().components.empty());
    require(canonical_source(empty.value()) ==
            R"({"components":[],"entries":[],"profile":"record.v1","schema":"fastdb.payload.v1"})");

    const std::string all_types_source =
        fixture("valid/record-all-types.source.json");
    require(!all_types_source.empty());
    auto parsed = parse_source(all_types_source);
    require(parsed.has_value());
    const SourceSpec& source = parsed.value();
    require(source.profile == Profile::record_v1);
    require(source.entries.size() == 2U);
    require(source.entries[0].id == "single");
    require(source.entries[0].cardinality == Cardinality::one);
    require(source.entries[0].type.kind == TypeKind::component);
    require(source.entries[0].type.source_id == "AllTypes");
    require(!source.entries[0].type.nullable);
    require(source.entries[1].id == "series");
    require(source.entries[1].cardinality == Cardinality::many);
    require(source.entries[1].type.kind == TypeKind::list);
    require(source.entries[1].type.nullable);
    require(source.entries[1].type.items != nullptr);
    require(source.entries[1].type.items->kind == TypeKind::u8);
    require(source.entries[1].type.items->nullable);

    require(source.components.size() == 2U);
    require(source.components[0].id == "AllTypes");
    require(source.components[1].id == "Leaf");
    require(source.components[0].fields.size() == 14U);
    constexpr std::array<TypeKind, 14> expected_kinds = {
        TypeKind::boolean, TypeKind::u8,        TypeKind::u16,
        TypeKind::u32,     TypeKind::i32,       TypeKind::u8n,
        TypeKind::u16n,    TypeKind::f32,       TypeKind::f64,
        TypeKind::str,     TypeKind::wstr,      TypeKind::bytes,
        TypeKind::component, TypeKind::list,
    };
    for (std::size_t index = 0; index < expected_kinds.size(); ++index) {
        require(source.components[0].fields[index].type.kind ==
                expected_kinds[index]);
    }
    const TypeNode& u8n = source.components[0].fields[5].type;
    require(std::signbit(u8n.minimum));
    require(u8n.maximum == 1.0);
    const TypeNode& u16n = source.components[0].fields[6].type;
    require(u16n.minimum == -1.0);
    require(u16n.maximum == 1.0);
    const TypeNode& component = source.components[0].fields[12].type;
    require(component.source_id == "Leaf");
    const TypeNode& outer_list = source.components[0].fields[13].type;
    require(!outer_list.nullable);
    require(outer_list.items != nullptr);
    require(outer_list.items->kind == TypeKind::list);
    require(outer_list.items->nullable);
    require(outer_list.items->items != nullptr);
    require(outer_list.items->items->kind == TypeKind::str);
    require(outer_list.items->items->nullable);
    return EXIT_SUCCESS;
}

int test_object_graph_structural_types_and_explicit_nullable() {
    const std::string source_text = fixture("valid/object-graph.source.json");
    require(!source_text.empty());
    auto parsed = parse_source(source_text);
    require(parsed.has_value());
    const SourceSpec& source = parsed.value();
    require(source.profile == Profile::object_graph_v1);
    require(source.entries.size() == 2U);
    require(source.entries[1].cardinality == Cardinality::many);
    require(source.entries[1].type.kind == TypeKind::ref);
    require(source.entries[1].type.source_id == "Node");
    require(source.entries[1].type.nullable);
    require(source.components.size() == 1U);
    require(source.components[0].fields.size() == 4U);
    const TypeNode& next = source.components[0].fields[1].type;
    require(next.kind == TypeKind::ref);
    require(next.source_id == "Node");
    require(next.nullable);
    const TypeNode& children = source.components[0].fields[2].type;
    require(children.kind == TypeKind::list);
    require(children.nullable);
    require(children.items != nullptr);
    require(children.items->kind == TypeKind::ref);
    require(!children.items->nullable);
    const TypeNode& labels = source.components[0].fields[3].type;
    require(labels.kind == TypeKind::list);
    require(!labels.nullable);
    require(labels.items != nullptr);
    require(labels.items->kind == TypeKind::list);
    require(labels.items->nullable);
    require(labels.items->items != nullptr);
    require(labels.items->items->kind == TypeKind::wstr);
    require(labels.items->items->nullable);
    return EXIT_SUCCESS;
}

int test_nullable_normalization_and_source_order() {
    auto omitted =
        parse_source(fixture("valid/nullable-omitted.source.json"));
    auto explicit_false =
        parse_source(fixture("valid/nullable-explicit.source.json"));
    require(omitted.has_value());
    require(explicit_false.has_value());
    const std::string omitted_canonical = canonical_source(omitted.value());
    require(!omitted_canonical.empty());
    require(omitted_canonical == canonical_source(explicit_false.value()));
    require(omitted_canonical ==
            R"({"components":[{"fields":[{"id":"names","type":{"items":{"kind":"str","nullable":false},"kind":"list","nullable":false}}],"id":"Thing","kind":"record"}],"entries":[{"cardinality":"many","id":"items","type":{"items":{"id":"Thing","kind":"component","nullable":false},"kind":"list","nullable":false}}],"profile":"record.v1","schema":"fastdb.payload.v1"})");

    auto source_order = parse_source(R"({
      "schema":"fastdb.payload.v1",
      "profile":"record.v1",
      "entries":[
        {"id":"same","cardinality":"one","type":{"kind":"ref","target":"Missing"}},
        {"id":"same","cardinality":"many","type":{"kind":"component","id":"Missing"}}
      ],
      "components":[
        {"id":"Zed","kind":"record","fields":[
          {"id":"second","type":{"kind":"u8"}},
          {"id":"first","type":{"kind":"u16"}}
        ]},
        {"id":"Alpha","kind":"record","fields":[]}
      ]
    })");
    require(source_order.has_value());
    require(source_order.value().entries[0].id == "same");
    require(source_order.value().entries[1].id == "same");
    require(source_order.value().components[0].id == "Zed");
    require(source_order.value().components[1].id == "Alpha");
    require(source_order.value().components[0].fields[0].id == "second");
    require(source_order.value().components[0].fields[1].id == "first");
    require(source_order.value().entries[0].type.kind == TypeKind::ref);
    require(source_order.value().entries[0].type.source_id == "Missing");
    return EXIT_SUCCESS;
}

struct ErrorCase final {
    std::string source;
    std::uint32_t code;
    std::string_view path;
    std::string_view message;
    std::string_view details;
};

int test_golden_invalid_sources() {
    const std::array<ErrorCase, 5> cases = {{
        {fixture("invalid/unknown-field.source.json"),
         FDB_PAYLOAD_E_UNKNOWN_FIELD, "/extra",
         "Payload object contains unknown field",
         R"({"field":"extra","reason":"unknown_field"})"},
        {fixture("invalid/missing-field.source.json"),
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/type",
         "Payload object is missing required field",
         R"({"field":"type","reason":"missing_field"})"},
        {fixture("invalid/bad-kind.source.json"),
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/type/items/kind",
         "Payload type kind is invalid",
         R"({"actual":"text","reason":"invalid_type_kind"})"},
        {fixture("invalid/bad-id.source.json"),
         FDB_PAYLOAD_E_INVALID_TYPE, "/components/0/fields/0/id",
         "Payload identifier is invalid",
         R"({"reason":"invalid_id","value":"9bad"})"},
        {fixture("invalid/bad-range.source.json"),
         FDB_PAYLOAD_E_INVALID_NUMBER, "/components/0/fields/0/type/max",
         "Normalized integer bounds are invalid",
         R"({"max":1,"min":1,"reason":"min_not_less_than_max"})"},
    }};
    for (const ErrorCase& expected : cases) {
        require(!expected.source.empty());
        require(has_error(parse_source(expected.source), expected.code,
                          expected.path, expected.message,
                          expected.details));
    }
    return EXIT_SUCCESS;
}

int test_exact_object_shapes_and_required_fields() {
    const std::string obsolete_profile = std::string("colum") + "nar.v1";
    const std::string obsolete_profile_source =
        R"({"schema":"fastdb.payload.v1","profile":")" +
        obsolete_profile + R"(","entries":[],"components":[]})";
    const std::string obsolete_profile_details =
        R"({"actual":")" + obsolete_profile +
        R"(","reason":"invalid_profile"})";
    const std::array<ErrorCase, 10> cases = {{
        {R"([])", FDB_PAYLOAD_E_INVALID_TYPE, "",
         "Payload field has invalid JSON type",
         R"({"actual":"array","expected":"object","reason":"invalid_field_type"})"},
        {R"({"schema":"fastdb.payload.v1","entries":[],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/profile",
         "Payload object is missing required field",
         R"({"field":"profile","reason":"missing_field"})"},
        {R"({"schema":"fastdb.payload.v2","profile":"record.v1","entries":[],"components":[]})",
         FDB_PAYLOAD_E_UNSUPPORTED_SCHEMA, "/schema",
         "Payload schema version is unsupported",
         R"({"actual":"fastdb.payload.v2","expected":"fastdb.payload.v1"})"},
        {obsolete_profile_source,
         FDB_PAYLOAD_E_INVALID_TYPE, "/profile",
         "Payload profile is invalid",
         obsolete_profile_details},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"one","type":{"kind":"u8"},"extra":0}],"components":[]})",
         FDB_PAYLOAD_E_UNKNOWN_FIELD, "/entries/0/extra",
         "Payload object contains unknown field",
         R"({"field":"extra","reason":"unknown_field"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[{"id":"C","kind":"record","fields":[],"extra":0}]})",
         FDB_PAYLOAD_E_UNKNOWN_FIELD, "/components/0/extra",
         "Payload object contains unknown field",
         R"({"field":"extra","reason":"unknown_field"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[{"id":"C","kind":"record","fields":[{"id":"f","type":{"kind":"u8"},"extra":0}]}]})",
         FDB_PAYLOAD_E_UNKNOWN_FIELD, "/components/0/fields/0/extra",
         "Payload object contains unknown field",
         R"({"field":"extra","reason":"unknown_field"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"one","type":{"kind":"list"}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/type/items",
         "Payload object is missing required field",
         R"({"field":"items","reason":"missing_field"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"one","type":{"kind":"list","items":{"kind":"u8","extra":0}}}],"components":[]})",
         FDB_PAYLOAD_E_UNKNOWN_FIELD, "/entries/0/type/items/extra",
         "Payload object contains unknown field",
         R"({"field":"extra","reason":"unknown_field"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"one","type":{"kind":"u8n","min":0,"max":1,"id":"extra"}}],"components":[]})",
         FDB_PAYLOAD_E_UNKNOWN_FIELD, "/entries/0/type/id",
         "Payload object contains unknown field",
         R"({"field":"id","reason":"unknown_field"})"},
    }};
    for (const ErrorCase& expected : cases) {
        require(has_error(parse_source(expected.source), expected.code,
                          expected.path, expected.message,
                          expected.details));
    }
    return EXIT_SUCCESS;
}

int test_field_types_ids_cardinality_nullable_and_bounds() {
    const std::array<ErrorCase, 11> cases = {{
        {R"({"schema":1,"profile":"record.v1","entries":[],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/schema",
         "Payload field has invalid JSON type",
         R"({"actual":"number","expected":"string","reason":"invalid_field_type"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":{},"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries",
         "Payload field has invalid JSON type",
         R"({"actual":"object","expected":"array","reason":"invalid_field_type"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":1,"cardinality":"one","type":{"kind":"u8"}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/id",
         "Payload field has invalid JSON type",
         R"({"actual":"number","expected":"string","reason":"invalid_field_type"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"optional","type":{"kind":"u8"}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/cardinality",
         "Payload cardinality is invalid",
         R"({"actual":"optional","reason":"invalid_cardinality"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"one","type":{"kind":"u8","nullable":0}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/type/nullable",
         "Payload field has invalid JSON type",
         R"({"actual":"number","expected":"boolean","reason":"invalid_field_type"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"one","type":{"kind":"u8n","min":"0","max":1}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/type/min",
         "Payload field has invalid JSON type",
         R"({"actual":"string","expected":"number","reason":"invalid_field_type"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"one","type":{"kind":"u8n","max":1}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/type/min",
         "Payload object is missing required field",
         R"({"field":"min","reason":"missing_field"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[{"id":"C","kind":"object","fields":[]}]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/components/0/kind",
         "Payload component kind is invalid",
         R"({"actual":"object","reason":"invalid_component_kind"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"","cardinality":"one","type":{"kind":"u8"}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/id",
         "Payload identifier is invalid",
         R"({"reason":"invalid_id","value":""})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"é","cardinality":"one","type":{"kind":"u8"}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_TYPE, "/entries/0/id",
         "Payload identifier is invalid",
         R"({"reason":"invalid_id","value":"é"})"},
        {R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"x","cardinality":"one","type":{"kind":"u8n","min":2,"max":1}}],"components":[]})",
         FDB_PAYLOAD_E_INVALID_NUMBER, "/entries/0/type/max",
         "Normalized integer bounds are invalid",
         R"({"max":1,"min":2,"reason":"min_not_less_than_max"})"},
    }};
    // One parse per row; the failed-row diagnostic never re-parses.
    for (std::size_t row = 0; row < cases.size(); ++row) {
        const ErrorCase& expected = cases[row];
        const Result<SourceSpec> parsed = parse_source(expected.source);
        if (!has_error(parsed, expected.code, expected.path, expected.message,
                       expected.details)) {
            std::fprintf(stderr,
                         "[fastdb-diag] spec_parse failed row=%zu "
                         "expected_code=%u expected_path=%.*s\n",
                         row, expected.code,
                         static_cast<int>(expected.path.size()),
                         expected.path.empty() ? "" : expected.path.data());
            if (parsed.has_value()) {
                std::fputs(
                    "[fastdb-diag] spec_parse actual=<parsed successfully>\n",
                    stderr);
            } else {
                const Error& actual = parsed.error();
                const std::string_view actual_path = actual.path();
                const std::string_view actual_message = actual.message();
                const std::string_view actual_details = actual.details_json();
                std::fprintf(
                    stderr,
                    "[fastdb-diag] spec_parse actual_code=%u "
                    "actual_path=%.*s actual_message=%.*s "
                    "actual_details=%.*s\n",
                    actual.code(), static_cast<int>(actual_path.size()),
                    actual_path.empty() ? "" : actual_path.data(),
                    static_cast<int>(actual_message.size()),
                    actual_message.empty() ? "" : actual_message.data(),
                    static_cast<int>(actual_details.size()),
                    actual_details.empty() ? "" : actual_details.data());
            }
            std::fflush(stderr);
        }
        require(has_error(parsed, expected.code, expected.path,
                          expected.message, expected.details));
    }

    auto valid_ids = parse_source(R"({
      "schema":"fastdb.payload.v1","profile":"record.v1",
      "entries":[{"id":"_A0","cardinality":"one","type":{"kind":"component","id":"keyword"}}],
      "components":[{"id":"keyword","kind":"record","fields":[{"id":"a9_","type":{"kind":"u8"}}]}]
    })");
    require(valid_ids.has_value());
    return EXIT_SUCCESS;
}

int test_typed_count_limits_and_pre_growth_failure() {
    const std::string two_entries = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1",
      "entries":[
        {"id":"a","cardinality":"one","type":{"kind":"u8"}},
        {"id":"b","cardinality":"many","type":{"kind":"list","items":{"kind":"u16"}}}
      ],
      "components":[]
    })";
    SourceParseLimits limits;
    limits.max_entries = UINT32_C(1);
    require(has_error(
        parse_source(two_entries, limits), FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT,
        "/entries", "Payload entry count exceeds configured limit",
        R"({"actual":"2","kind":"entries","limit":"1"})"));

    const std::string two_components = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[
        {"id":"A","kind":"record","fields":[]},
        {"id":"B","kind":"record","fields":[]}
      ]
    })";
    limits = SourceParseLimits{};
    limits.max_components = UINT32_C(1);
    require(has_error(
        parse_source(two_components, limits),
        FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "/components",
        "Payload component count exceeds configured limit",
        R"({"actual":"2","kind":"components","limit":"1"})"));

    const std::string two_fields = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[{"id":"A","kind":"record","fields":[
        {"id":"x","type":{"kind":"u8"}},
        {"id":"y","type":{"kind":"list","items":{"kind":"list","items":{"kind":"u16"}}}}
      ]}]
    })";
    limits = SourceParseLimits{};
    limits.max_fields_per_component = UINT32_C(1);
    require(has_error(
        parse_source(two_fields, limits),
        FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "/components/0/fields",
        "Payload component field count exceeds configured limit",
        R"({"actual":"2","kind":"fields_per_component","limit":"1"})"));

    const std::string total_fields = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[
        {"id":"A","kind":"record","fields":[{"id":"x","type":{"kind":"u8"}}]},
        {"id":"B","kind":"record","fields":[{"id":"y","type":{"kind":"u16"}}]}
      ]
    })";
    limits = SourceParseLimits{};
    limits.max_total_fields = UINT64_C(1);
    require(has_error(
        parse_source(total_fields, limits),
        FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "/components/1/fields",
        "Payload total field count exceeds configured limit",
        R"({"actual":"2","kind":"total_fields","limit":"1"})"));

    limits = SourceParseLimits{};
    limits.max_entries = UINT32_C(2);
    auto exact_entries = parse_source(two_entries, limits);
    require(exact_entries.has_value());
    limits = SourceParseLimits{};
    limits.max_total_fields = UINT64_C(2);
    auto exact_fields = parse_source(total_fields, limits);
    require(exact_fields.has_value());

    std::string many_entries =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[)";
    constexpr std::size_t count = 10000U;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0U) {
            many_entries.push_back(',');
        }
        many_entries +=
            R"({"id":"entry","cardinality":"one","type":{"kind":"u8"}})";
    }
    many_entries += R"(],"components":[]})";
    auto document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(many_entries.data()),
        static_cast<std::uint64_t>(many_entries.size()),
        {UINT64_C(16777216), UINT64_C(1000000), UINT32_C(128)});
    require(document.has_value());
    bool threw = false;
    bool returned_exact_limit = false;
    try {
        allocation_guard::Budget budget(16U * 1024U);
        SourceParseLimits low;
        low.max_entries = UINT32_C(1);
        const auto result = parse_and_normalize_source(document.value(), low);
        returned_exact_limit = has_error(
            result, FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "/entries",
            "Payload entry count exceeds configured limit",
            R"({"actual":"10000","kind":"entries","limit":"1"})");
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    require(!threw);
    require(returned_exact_limit);
    return EXIT_SUCCESS;
}

int test_deep_list_type_is_stack_safe() {
    constexpr std::uint32_t list_depth = UINT32_C(20000);
    const std::string source = deep_list_source(list_depth);

    JsonParseLimits json_limits;
    json_limits.max_nesting_depth = list_depth + UINT32_C(16);
    auto document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), json_limits);
    require(document.has_value());
    auto parsed = parse_and_normalize_source(document.value());
    require(parsed.has_value());

    const TypeNode* node = &parsed.value().entries[0].type;
    std::uint32_t observed_depth = UINT32_C(0);
    while (node->kind == TypeKind::list) {
        require(node->items != nullptr);
        node = node->items.get();
        ++observed_depth;
    }
    require(observed_depth == list_depth);
    require(node->kind == TypeKind::u8);

    const std::string canonical = canonical_source(parsed.value());
    require(!canonical.empty());
    require(canonical.find(R"("kind":"u8","nullable":false)") !=
            std::string::npos);
    return EXIT_SUCCESS;
}

int test_deep_typed_parse_has_linear_allocation() {
    constexpr std::uint32_t list_depth = UINT32_C(4000);
    const std::string source = deep_list_source(list_depth);
    JsonParseLimits json_limits;
    json_limits.max_nesting_depth = list_depth + UINT32_C(16);
    auto document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), json_limits);
    require(document.has_value());

    bool threw = false;
    bool parsed_successfully = false;
    try {
        allocation_guard::Budget budget(16U * 1024U * 1024U);
        const auto parsed = parse_and_normalize_source(document.value());
        parsed_successfully = parsed.has_value();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    require(!threw);
    require(parsed_successfully);
    return EXIT_SUCCESS;
}

int test_deep_normalized_emission_has_linear_allocation() {
    constexpr std::uint32_t list_depth = UINT32_C(4000);
    const std::string source = deep_list_source(list_depth);
    JsonParseLimits json_limits;
    json_limits.max_nesting_depth = list_depth + UINT32_C(16);
    auto document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), json_limits);
    require(document.has_value());
    auto parsed = parse_and_normalize_source(document.value());
    require(parsed.has_value());

    bool threw = false;
    bool emitted_object = false;
    try {
        allocation_guard::Budget budget(16U * 1024U * 1024U);
        const auto normalized = normalized_source_json(parsed.value());
        emitted_object =
            std::holds_alternative<fastdb::payload::json::JsonValue::Object>(
                normalized.storage());
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    require(!threw);
    require(emitted_object);
    return EXIT_SUCCESS;
}

int test_deep_invalid_type_path_is_exact() {
    constexpr std::uint32_t list_depth = UINT32_C(4000);
    const std::string source =
        deep_list_source(list_depth, R"({"kind":"text"})");
    JsonParseLimits json_limits;
    json_limits.max_nesting_depth = list_depth + UINT32_C(16);
    auto document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), json_limits);
    require(document.has_value());

    std::string expected_path = "/entries/0/type";
    for (std::uint32_t depth = UINT32_C(0); depth < list_depth; ++depth) {
        expected_path += "/items";
    }
    expected_path += "/kind";
    require(has_error(
        parse_and_normalize_source(document.value()),
        FDB_PAYLOAD_E_INVALID_TYPE, expected_path,
        "Payload type kind is invalid",
        R"({"actual":"text","reason":"invalid_type_kind"})"));
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string_view selected{argv[1]};
        if (selected == "deep") {
            return test_deep_list_type_is_stack_safe();
        }
        if (selected == "deep-parse-allocation") {
            return test_deep_typed_parse_has_linear_allocation();
        }
        if (selected == "deep-normalize-allocation") {
            return test_deep_normalized_emission_has_linear_allocation();
        }
        if (selected == "deep-invalid-path") {
            return test_deep_invalid_type_path_is_exact();
        }
        return EXIT_FAILURE;
    }
    require(test_empty_source_and_complete_record_algebra() == EXIT_SUCCESS);
    require(test_object_graph_structural_types_and_explicit_nullable() ==
            EXIT_SUCCESS);
    require(test_nullable_normalization_and_source_order() == EXIT_SUCCESS);
    require(test_golden_invalid_sources() == EXIT_SUCCESS);
    require(test_exact_object_shapes_and_required_fields() == EXIT_SUCCESS);
    require(test_field_types_ids_cardinality_nullable_and_bounds() ==
            EXIT_SUCCESS);
    require(test_typed_count_limits_and_pre_growth_failure() == EXIT_SUCCESS);
    require(test_deep_list_type_is_stack_safe() == EXIT_SUCCESS);
    require(test_deep_typed_parse_has_linear_allocation() == EXIT_SUCCESS);
    require(test_deep_normalized_emission_has_linear_allocation() ==
            EXIT_SUCCESS);
    require(test_deep_invalid_type_path_is_exact() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
