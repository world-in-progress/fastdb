#include "GoldenCorpus.hpp"
#include "TestSupport.hpp"

#include "payload/identity/Sha256.hpp"
#include "payload/json/Jcs.hpp"
#include "payload/json/JsonDocument.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/spec/Manifest.hpp"
#include "payload/spec/RuntimeTopology.hpp"
#include "payload/spec/SchemaRepository.hpp"

#include <fastdb_payload.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace allocation_guard {

thread_local bool enabled = false;
thread_local std::size_t remaining = 0;
// Diagnostic scalars only; they never participate in budget accounting.
thread_local std::size_t budget_bytes = 0;
thread_local std::size_t requested_total = 0;
thread_local std::size_t allocation_count = 0;
thread_local std::size_t rejected_count = 0;
thread_local std::size_t first_rejected_size = 0;

void* allocate(std::size_t size) {
    if (enabled) {
        ++allocation_count;
        requested_total += size;
        if (size > remaining) {
            if (rejected_count == 0U) {
                first_rejected_size = size;
            }
            ++rejected_count;
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
        budget_bytes = bytes;
        requested_total = 0;
        allocation_count = 0;
        rejected_count = 0;
        first_rejected_size = 0;
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
using fastdb::payload::identity::sha256_lower_hex;
using fastdb::payload::json::jcs_serialize;
using fastdb::payload::json::JsonCursor;
using fastdb::payload::json::JsonDocument;
using fastdb::payload::spec::CompiledSpec;
using fastdb::payload::spec::CompileLimits;
using fastdb::payload::spec::derive_runtime_topology;
using fastdb::payload::spec::manifest_value_conforms;
using fastdb::payload::spec::Profile;
using fastdb::payload::spec::SchemaRepository;
using fastdb::test::payload::GoldenCase;
using fastdb::test::payload::GoldenError;
using fastdb::test::payload::GoldenSuccess;
using fastdb::test::payload::load_binary_file;
using fastdb::test::payload::load_spec_golden_corpus;

static_assert(std::is_copy_constructible_v<CompiledSpec>);
static_assert(std::is_copy_assignable_v<CompiledSpec>);
static_assert(std::is_nothrow_move_constructible_v<CompiledSpec>);
static_assert(std::is_nothrow_move_assignable_v<CompiledSpec>);

std::vector<std::uint8_t> decode_hex(std::string_view hexadecimal) {
    if (hexadecimal.size() % 2U != 0U) {
        return {};
    }
    const auto nibble = [](char value) -> std::optional<std::uint8_t> {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint8_t>(value - 'a' + 10);
        }
        return std::nullopt;
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hexadecimal.size() / 2U);
    for (std::size_t offset = 0; offset < hexadecimal.size(); offset += 2U) {
        const auto high = nibble(hexadecimal[offset]);
        const auto low = nibble(hexadecimal[offset + 1U]);
        if (!high.has_value() || !low.has_value()) {
            return {};
        }
        bytes.push_back(static_cast<std::uint8_t>(
            static_cast<std::uint8_t>(*high << 4U) | *low));
    }
    return bytes;
}

std::string decoded_string(std::string_view hexadecimal) {
    const std::vector<std::uint8_t> bytes = decode_hex(hexadecimal);
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

bool is_lower_hex(std::string_view text) {
    for (const char value : text) {
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

const GoldenCase* find_case(const std::vector<GoldenCase>& cases,
                            std::string_view name) {
    for (const GoldenCase& item : cases) {
        if (item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

Result<CompiledSpec> compile(std::string_view source,
                             CompileLimits limits = {}) {
    return CompiledSpec::compile(source, limits);
}

JsonCursor required_member(JsonCursor object, std::string_view wanted) {
    if (!object.is_object()) {
        return JsonCursor{};
    }
    auto members = object.members();
    std::string_view name;
    JsonCursor value;
    while (members.next(name, value)) {
        if (name == wanted) {
            return value;
        }
    }
    return JsonCursor{};
}

JsonCursor array_at(JsonCursor array, std::uint64_t wanted) {
    if (!array.is_array()) {
        return JsonCursor{};
    }
    auto values = array.elements();
    JsonCursor value;
    std::uint64_t index = UINT64_C(0);
    while (values.next(value)) {
        if (index == wanted) {
            return value;
        }
        ++index;
    }
    return JsonCursor{};
}

bool has_member(JsonCursor object, std::string_view wanted) {
    if (!object.is_object()) {
        return false;
    }
    auto members = object.members();
    std::string_view name;
    JsonCursor value;
    while (members.next(name, value)) {
        (void)value;
        if (name == wanted) {
            return true;
        }
    }
    return false;
}

bool array_contains_string(JsonCursor array, std::string_view wanted) {
    if (!array.is_array()) {
        return false;
    }
    auto values = array.elements();
    JsonCursor value;
    while (values.next(value)) {
        if (value.is_string() && value.string() == wanted) {
            return true;
        }
    }
    return false;
}

bool exact_error(const Error& actual, const GoldenError& expected) {
    return actual.code() == expected.status &&
           actual.symbol() == expected.symbol &&
           actual.path() == expected.path &&
           actual.message() == expected.message &&
           actual.details_json() == expected.details_json;
}

int test_ordered_golden_corpus() {
    const auto cases =
        load_spec_golden_corpus(FASTDB_PAYLOAD_SPEC_FIXTURE_DIR);
    require(cases.size() == 20U);
    for (const GoldenCase& item : cases) {
        auto result = compile(item.source);
        if (const auto* expected =
                std::get_if<GoldenSuccess>(&item.expected)) {
            require(result.has_value(), item.name);
            const CompiledSpec& actual = result.value();
            require(actual.canonical_bytes() ==
                        decoded_string(expected->canonical_hex),
                    item.name);
            require(sha256_lower_hex(actual.digest()) == expected->sha256,
                    item.name);
            require(actual.manifest_bytes() ==
                        decoded_string(expected->manifest_hex),
                    item.name);
        } else {
            const auto& error_expected =
                std::get<GoldenError>(item.expected);
            require(!result.has_value(), item.name);
            require(exact_error(result.error(), error_expected), item.name);
        }
    }
    return EXIT_SUCCESS;
}

int test_identity_normalization_and_significance() {
    const auto cases =
        load_spec_golden_corpus(FASTDB_PAYLOAD_SPEC_FIXTURE_DIR);
    const GoldenCase* omitted = find_case(cases, "nullable-omitted");
    const GoldenCase* explicit_false =
        find_case(cases, "nullable-explicit");
    const GoldenCase* components_a = find_case(cases, "component-order-a");
    const GoldenCase* components_b = find_case(cases, "component-order-b");
    const GoldenCase* entries_a = find_case(cases, "entry-order-a");
    const GoldenCase* entries_b = find_case(cases, "entry-order-b");
    require(omitted != nullptr && explicit_false != nullptr);
    require(components_a != nullptr && components_b != nullptr);
    require(entries_a != nullptr && entries_b != nullptr);

    auto omitted_result = compile(omitted->source);
    auto explicit_result = compile(explicit_false->source);
    require(omitted_result.has_value() && explicit_result.has_value());
    require(omitted_result.value().canonical_bytes() ==
            explicit_result.value().canonical_bytes());
    require(omitted_result.value().digest() == explicit_result.value().digest());

    auto component_result_a = compile(components_a->source);
    auto component_result_b = compile(components_b->source);
    require(component_result_a.has_value() && component_result_b.has_value());
    require(component_result_a.value().canonical_bytes() ==
            component_result_b.value().canonical_bytes());
    require(component_result_a.value().digest() ==
            component_result_b.value().digest());
    require(component_result_a.value().resolved().components()[0].id ==
            "Alpha");
    require(component_result_b.value().resolved().components()[0].id ==
            "Alpha");

    auto entry_result_a = compile(entries_a->source);
    auto entry_result_b = compile(entries_b->source);
    require(entry_result_a.has_value() && entry_result_b.has_value());
    require(entry_result_a.value().canonical_bytes() !=
            entry_result_b.value().canonical_bytes());
    require(entry_result_a.value().digest() != entry_result_b.value().digest());

    constexpr std::string_view compact =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]})";
    constexpr std::string_view reordered = R"(
      { "components" : [ ], "entries" : [ ],
        "profile" : "record.v1", "schema" : "fastdb.payload.v1" }
    )";
    auto compact_result = compile(compact);
    auto reordered_result = compile(reordered);
    require(compact_result.has_value() && reordered_result.has_value());
    require(compact_result.value().canonical_bytes() ==
            reordered_result.value().canonical_bytes());
    require(compact_result.value().digest() == reordered_result.value().digest());

    constexpr std::string_view fields_a = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[{"id":"Record","kind":"record","fields":[
        {"id":"first","type":{"kind":"u8"}},
        {"id":"second","type":{"kind":"u16"}}
      ]}]
    })";
    constexpr std::string_view fields_b = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1","entries":[],
      "components":[{"id":"Record","kind":"record","fields":[
        {"id":"second","type":{"kind":"u16"}},
        {"id":"first","type":{"kind":"u8"}}
      ]}]
    })";
    auto field_result_a = compile(fields_a);
    auto field_result_b = compile(fields_b);
    require(field_result_a.has_value() && field_result_b.has_value());
    require(field_result_a.value().canonical_bytes() !=
            field_result_b.value().canonical_bytes());
    require(field_result_a.value().digest() != field_result_b.value().digest());
    return EXIT_SUCCESS;
}

int test_manifest_indexes_facts_and_capabilities() {
    const auto cases =
        load_spec_golden_corpus(FASTDB_PAYLOAD_SPEC_FIXTURE_DIR);
    const GoldenCase* item = find_case(cases, "component-order-a");
    require(item != nullptr);
    auto result = compile(item->source);
    require(result.has_value());
    const CompiledSpec& compiled = result.value();

    const std::string digest_text = sha256_lower_hex(compiled.digest());
    require(compiled.capabilities().operation_flags ==
            (FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY |
             FDB_PAYLOAD_OPERATION_BUILD | FDB_PAYLOAD_OPERATION_OPEN |
             FDB_PAYLOAD_OPERATION_VIEW | FDB_PAYLOAD_OPERATION_MATERIALIZE |
             FDB_PAYLOAD_OPERATION_INVALIDATE | FDB_PAYLOAD_OPERATION_CODEGEN));
    require(compiled.capabilities().codegen_target_flags ==
            (FDB_PAYLOAD_CODEGEN_TARGET_CPP | FDB_PAYLOAD_CODEGEN_TARGET_RUST |
             FDB_PAYLOAD_CODEGEN_TARGET_PYTHON |
             FDB_PAYLOAD_CODEGEN_TARGET_TYPESCRIPT));
    require(compiled.capabilities().direct_build_status ==
            FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE);
    require(compiled.capabilities().direct_build_reason ==
            "record_layout_exact");

    auto manifest = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(compiled.manifest_bytes().data()),
        static_cast<std::uint64_t>(compiled.manifest_bytes().size()));
    require(manifest.has_value());
    require(manifest_value_conforms(manifest.value().to_json_value()));
    const JsonCursor root = manifest.value().root();
    require(required_member(required_member(root, "payload"), "sha256")
                .string() == digest_text);
    const JsonCursor runtime = required_member(root, "runtime");
    require(required_member(runtime, "status").string() == "available");
    require(required_member(runtime, "layout_model").string() ==
            "record_aos");
    const JsonCursor required_pools =
        required_member(runtime, "required_pools");
    require(array_at(required_pools, UINT64_C(0)).string() == "utf8");
    require(!array_at(required_pools, UINT64_C(1)).is_string());
    require(!required_member(runtime, "fixed_width_values_only").boolean());
    require(required_member(runtime, "has_runtime_sized_regions").boolean());
    require(required_member(runtime, "reachable_type_count").number() ==
            4.0);
    require(required_member(runtime, "reachable_component_count").number() ==
            2.0);
    require(required_member(runtime, "reachable_list_type_count").number() ==
            0.0);
    std::string wrong_variant(compiled.manifest_bytes());
    const std::size_t component_index =
        wrong_variant.find(R"("component_index":0)");
    require(component_index != std::string::npos);
    wrong_variant.replace(component_index + 1U,
                          std::string_view{"component_index"}.size(),
                          "target_component_index");
    auto wrong_manifest = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(wrong_variant.data()),
        static_cast<std::uint64_t>(wrong_variant.size()));
    require(wrong_manifest.has_value());
    require(!manifest_value_conforms(wrong_manifest.value().to_json_value()));
    const auto rejects_replacement = [&compiled](std::string_view from,
                                                  std::string_view to) {
        std::string candidate(compiled.manifest_bytes());
        const std::size_t offset = candidate.find(from);
        if (offset == std::string::npos) {
            return false;
        }
        candidate.replace(offset, from.size(), to);
        auto parsed = JsonDocument::parse(
            reinterpret_cast<const std::uint8_t*>(candidate.data()),
            static_cast<std::uint64_t>(candidate.size()));
        return parsed.has_value() &&
               !manifest_value_conforms(parsed.value().to_json_value());
    };
    require(rejects_replacement(
        R"("reason":"record_layout_exact")",
        R"("reason":"runtime_slice_not_implemented")"));
    require(rejects_replacement(R"("status":"eligible")",
                                R"("status":"not_evaluated")"));
    require(rejects_replacement(R"("layout_model":"record_aos")",
                                R"("layout_model":"object_pool_aos")"));
    require(rejects_replacement(
        R"("operations":["compile","query","build","open","view","materialize","invalidate","codegen"])",
        R"("operations":["compile","query"])"));
    require(rejects_replacement(
        R"("codegen_targets":["cpp","rust","python","typescript"])",
        R"("codegen_targets":[])"));
    require(rejects_replacement(R"("profile":"record.v1")",
                                R"("profile":"object_graph.v1")"));
    const JsonCursor entries = required_member(root, "entries");
    const JsonCursor entry = array_at(entries, UINT64_C(0));
    require(required_member(entry, "index").number() == 0.0);
    require(required_member(entry, "id").string() == "root");
    require(required_member(required_member(entry, "type"),
                            "component_index")
                .number() == 1.0);

    const JsonCursor components = required_member(root, "components");
    const JsonCursor alpha = array_at(components, UINT64_C(0));
    const JsonCursor zed = array_at(components, UINT64_C(1));
    require(required_member(alpha, "id").string() == "Alpha");
    require(required_member(alpha, "index").number() == 0.0);
    require(required_member(zed, "id").string() == "Zed");
    require(required_member(zed, "index").number() == 1.0);
    const JsonCursor fields = required_member(zed, "fields");
    require(required_member(array_at(fields, UINT64_C(0)), "id").string() ==
            "second");
    require(required_member(array_at(fields, UINT64_C(0)), "index").number() ==
            0.0);
    require(required_member(array_at(fields, UINT64_C(1)), "id").string() ==
            "first");
    require(required_member(array_at(fields, UINT64_C(1)), "index").number() ==
            1.0);

    require(compiled.entry_index("root") == UINT32_C(0));
    require(compiled.component_index("Alpha") == UINT32_C(0));
    require(compiled.component_index("Zed") == UINT32_C(1));
    require(compiled.component_field_index(UINT32_C(1), "second") ==
            UINT32_C(0));
    require(compiled.component_field_index(UINT32_C(1), "first") ==
            UINT32_C(1));
    require(!compiled.entry_index("missing").has_value());
    require(!compiled.component_index("missing").has_value());

    const GoldenCase* graph_item = find_case(cases, "object-graph");
    require(graph_item != nullptr);
    auto graph_result = compile(graph_item->source);
    require(graph_result.has_value());
    const CompiledSpec& graph = graph_result.value();
    auto graph_topology = derive_runtime_topology(graph.resolved());
    require(graph_topology.has_value());
    require(graph.capabilities().operation_flags ==
            (FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY |
             FDB_PAYLOAD_OPERATION_BUILD | FDB_PAYLOAD_OPERATION_OPEN |
             FDB_PAYLOAD_OPERATION_VIEW | FDB_PAYLOAD_OPERATION_MATERIALIZE |
             FDB_PAYLOAD_OPERATION_INVALIDATE | FDB_PAYLOAD_OPERATION_CODEGEN));
    require(graph.capabilities().codegen_target_flags ==
            (FDB_PAYLOAD_CODEGEN_TARGET_CPP | FDB_PAYLOAD_CODEGEN_TARGET_RUST |
             FDB_PAYLOAD_CODEGEN_TARGET_PYTHON |
             FDB_PAYLOAD_CODEGEN_TARGET_TYPESCRIPT));
    require(graph.capabilities().direct_build_status ==
            FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE);
    auto graph_manifest = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(graph.manifest_bytes().data()),
        static_cast<std::uint64_t>(graph.manifest_bytes().size()));
    require(graph_manifest.has_value());
    const JsonCursor graph_runtime =
        required_member(graph_manifest.value().root(), "runtime");
    require(required_member(graph_runtime, "status").string() ==
            "available");
    require(required_member(graph_runtime, "layout_model").string() ==
            "object_pool_aos");
    const JsonCursor graph_pools =
        required_member(graph_runtime, "required_pools");
    constexpr std::array<std::string_view, 5> expected_pools{{
        "utf16le", "list_items", "objects", "references", "roots"}};
    for (std::uint64_t index = UINT64_C(0);
         index < expected_pools.size(); ++index) {
        require(array_at(graph_pools, index).string() ==
                expected_pools[static_cast<std::size_t>(index)]);
    }
    require(required_member(graph_runtime, "reachable_type_count").number() ==
            static_cast<double>(
                graph_topology.value().reachable_type_count));
    require(required_member(graph_runtime,
                            "reachable_component_count").number() ==
            static_cast<double>(
                graph_topology.value().reachable_component_count));
    require(required_member(graph_runtime,
                            "reachable_list_type_count").number() ==
            static_cast<double>(
                graph_topology.value().reachable_list_type_count));
    require(graph_topology.value().reachable_type_count == UINT32_C(9));
    require(graph_topology.value().reachable_component_count == UINT32_C(1));
    require(graph_topology.value().reachable_list_type_count == UINT32_C(3));
    require(graph_topology.value().has_objects);
    require(graph_topology.value().has_references);
    require(graph_topology.value().has_roots);
    return EXIT_SUCCESS;
}

int test_schema_artifacts_and_manifest_schema_contract() {
    auto source_schema = SchemaRepository::payload_source_schema();
    auto manifest_schema = SchemaRepository::payload_manifest_schema();
    require(source_schema.has_value());
    require(manifest_schema.has_value());

    const std::string pin = load_binary_file(
        std::string(FASTDB_PAYLOAD_SCHEMA_DIR) +
        "/fastdb.payload.v1.schema.sha256");
    require(pin.size() == 65U);
    require(pin.back() == '\n');
    require(is_lower_hex(std::string_view(pin).substr(0U, 64U)));
    require(sha256_lower_hex(source_schema.value().sha256) ==
            std::string_view(pin).substr(0U, 64U));
    require(source_schema.value().canonical_bytes.size() == 2074U);
    auto source_document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(
            source_schema.value().canonical_bytes.data()),
        static_cast<std::uint64_t>(
            source_schema.value().canonical_bytes.size()));
    require(source_document.has_value());
    const auto source_canonical =
        jcs_serialize(source_document.value().to_json_value());
    const auto* source_canonical_bytes =
        std::get_if<std::string>(&source_canonical);
    require(source_canonical_bytes != nullptr);
    require(*source_canonical_bytes == source_schema.value().canonical_bytes);

    auto manifest_document = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(
            manifest_schema.value().canonical_bytes.data()),
        static_cast<std::uint64_t>(
            manifest_schema.value().canonical_bytes.size()));
    require(manifest_document.has_value());
    const JsonCursor root = manifest_document.value().root();
    require(required_member(root, "$schema").string() ==
            "https://json-schema.org/draft/2020-12/schema");
    require(required_member(root, "$id").string() ==
            "urn:fastdb:schema:fastdb.payload.manifest.v1");

    std::vector<JsonCursor> pending{root};
    std::size_t closed_object_shapes = 0U;
    while (!pending.empty()) {
        const JsonCursor value = pending.back();
        pending.pop_back();
        if (value.is_array()) {
            auto values = value.elements();
            JsonCursor child;
            while (values.next(child)) {
                pending.push_back(child);
            }
            continue;
        }
        if (!value.is_object()) {
            continue;
        }
        const JsonCursor type = required_member(value, "type");
        if (type.is_string() && type.string() == "object") {
            const JsonCursor closed =
                required_member(value, "additionalProperties");
            require(closed.is_boolean());
            require(!closed.boolean());
            ++closed_object_shapes;
        }
        auto members = value.members();
        std::string_view name;
        JsonCursor child;
        while (members.next(name, child)) {
            (void)name;
            pending.push_back(child);
        }
    }
    require(closed_object_shapes >= 10U);

    const JsonCursor definitions = required_member(root, "$defs");
    const JsonCursor type_definition = required_member(definitions, "type");
    const JsonCursor variants = required_member(type_definition, "oneOf");
    require(variants.is_array());
    require(variants.size() == UINT64_C(5));
    bool saw_component = false;
    bool saw_ref = false;
    auto variant_values = variants.elements();
    JsonCursor variant;
    while (variant_values.next(variant)) {
        const JsonCursor properties = required_member(variant, "properties");
        const JsonCursor required = required_member(variant, "required");
        const JsonCursor kind_schema = required_member(properties, "kind");
        const JsonCursor kind_constant = required_member(kind_schema, "const");
        const bool component =
            kind_constant.is_string() && kind_constant.string() == "component";
        const bool ref =
            kind_constant.is_string() && kind_constant.string() == "ref";
        const bool has_component_index =
            has_member(properties, "component_index");
        const bool has_target_component_index =
            has_member(properties, "target_component_index");
        require(has_component_index == component);
        require(has_target_component_index == ref);
        require(array_contains_string(required, "component_index") ==
                component);
        require(array_contains_string(required, "target_component_index") ==
                ref);
        saw_component = saw_component || component;
        saw_ref = saw_ref || ref;
    }
    require(saw_component);
    require(saw_ref);

    const JsonCursor conditional =
        array_at(required_member(root, "allOf"), UINT64_C(0));
    const JsonCursor if_payload = required_member(
        required_member(required_member(conditional, "if"), "properties"),
        "payload");
    require(required_member(
                required_member(required_member(if_payload, "properties"),
                                "profile"),
                "const")
                .string() == "record.v1");
    const auto conditional_branch = [&conditional](std::string_view branch) {
        return required_member(required_member(conditional, branch),
                               "properties");
    };
    const auto runtime_constant = [](JsonCursor branch,
                                     std::string_view field) {
        return required_member(
            required_member(required_member(branch, "runtime"),
                            "properties"),
            field);
    };
    const auto capability_properties = [](JsonCursor branch) {
        return required_member(required_member(branch, "capabilities"),
                               "properties");
    };
    const JsonCursor record_branch = conditional_branch("then");
    require(required_member(runtime_constant(record_branch, "status"),
                            "const")
                .string() == "available");
    require(required_member(runtime_constant(record_branch, "layout_model"),
                            "const")
                .string() == "record_aos");
    const JsonCursor record_capabilities =
        capability_properties(record_branch);
    const JsonCursor record_operations = required_member(
        required_member(record_capabilities, "operations"), "const");
    constexpr std::array<std::string_view, 8> record_operation_names{
        {"compile", "query", "build", "open", "view", "materialize",
         "invalidate", "codegen"}};
    for (std::uint64_t index = UINT64_C(0);
         index < record_operation_names.size(); ++index) {
        require(array_at(record_operations, index).string() ==
                record_operation_names[static_cast<std::size_t>(index)]);
    }
    constexpr std::array<std::string_view, 4> codegen_target_names{
        {"cpp", "rust", "python", "typescript"}};
    const JsonCursor record_codegen_targets = required_member(
        required_member(record_capabilities, "codegen_targets"), "const");
    for (std::uint64_t index = UINT64_C(0); index < codegen_target_names.size();
         ++index) {
        require(array_at(record_codegen_targets, index).string() ==
                codegen_target_names[static_cast<std::size_t>(index)]);
    }
    const JsonCursor record_direct = required_member(
        required_member(record_capabilities, "direct_build"), "properties");
    require(required_member(required_member(record_direct, "status"),
                            "const")
                .string() == "eligible");
    require(required_member(required_member(record_direct, "reason"),
                            "const")
                .string() == "record_layout_exact");

    const JsonCursor graph_branch = conditional_branch("else");
    require(required_member(runtime_constant(graph_branch, "status"),
                            "const")
                .string() == "available");
    require(required_member(runtime_constant(graph_branch, "layout_model"),
                            "const")
                .string() == "object_pool_aos");
    const JsonCursor graph_capabilities =
        capability_properties(graph_branch);
    const JsonCursor graph_operations = required_member(
        required_member(graph_capabilities, "operations"), "const");
    require(graph_operations.size() == UINT64_C(8));
    for (std::uint64_t index = UINT64_C(0);
         index < record_operation_names.size(); ++index) {
        require(array_at(graph_operations, index).string() ==
                record_operation_names[static_cast<std::size_t>(index)]);
    }
    const JsonCursor graph_codegen_targets = required_member(
        required_member(graph_capabilities, "codegen_targets"), "const");
    for (std::uint64_t index = UINT64_C(0); index < codegen_target_names.size();
         ++index) {
        require(array_at(graph_codegen_targets, index).string() ==
                codegen_target_names[static_cast<std::size_t>(index)]);
    }
    const JsonCursor graph_direct = required_member(
        required_member(graph_capabilities, "direct_build"), "properties");
    require(required_member(required_member(graph_direct, "status"),
                            "const")
                .string() == "eligible");
    require(required_member(required_member(graph_direct, "reason"),
                            "const")
                .string() == "graph_layout_exact_after_freeze");
    return EXIT_SUCCESS;
}

std::string deep_list_source(std::uint32_t depth) {
    std::string source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"deep","cardinality":"one","type":)";
    for (std::uint32_t index = UINT32_C(0); index < depth; ++index) {
        source += R"({"kind":"list","items":)";
    }
    source += R"({"kind":"u8"})";
    source.append(static_cast<std::size_t>(depth), '}');
    source += R"(}],"components":[]})";
    return source;
}

int test_deep_manifest_allocation() {
    constexpr std::uint32_t depth = UINT32_C(20000);
    const std::string source = deep_list_source(depth);
    CompileLimits limits;
    limits.json.max_source_bytes = UINT64_C(4) * UINT64_C(1024) *
                                   UINT64_C(1024);
    limits.json.max_json_values = UINT64_C(50000);
    limits.json.max_nesting_depth = depth + UINT32_C(16);

    bool compiled = false;
    const char* outcome = "ok";
    std::uint32_t failure_code = UINT32_C(0);
    try {
        allocation_guard::Budget budget(64U * 1024U * 1024U);
        auto result = compile(source, limits);
        if (!result.has_value()) {
            outcome = "failed_result";
            failure_code = result.error().code();
        } else if (result.value().manifest_bytes().find(
                       "record_layout_exact") == std::string::npos) {
            outcome = "manifest_missing_record_layout_exact";
        } else {
            compiled = true;
        }
    } catch (const std::bad_alloc&) {
        outcome = "caught_bad_alloc";
    }
    // Printed after the guard scope has ended, so no diagnostic allocation is
    // ever charged to the 64 MiB budget.
    if (!compiled) {
        const std::size_t remaining = allocation_guard::remaining;
        const std::size_t budget = allocation_guard::budget_bytes;
        std::fprintf(
            stderr,
            "[fastdb-diag] compiled_spec deep_manifest_allocation failed: "
            "outcome=%s error_code=%u budget=%zu requested=%zu consumed=%zu "
            "remaining=%zu allocations=%zu rejected=%zu "
            "first_rejected_size=%zu\n",
            outcome, failure_code, budget,
            allocation_guard::requested_total,
            budget > remaining ? budget - remaining : 0U, remaining,
            allocation_guard::allocation_count,
            allocation_guard::rejected_count,
            allocation_guard::first_rejected_size);
        std::fflush(stderr);
    }
    require(compiled);
    return EXIT_SUCCESS;
}

int test_shared_const_reads() {
    constexpr std::string_view source = R"({
      "schema":"fastdb.payload.v1","profile":"record.v1",
      "entries":[{"id":"value","cardinality":"one","type":{"kind":"u8"}}],
      "components":[]
    })";
    auto result = compile(source);
    require(result.has_value());
    const CompiledSpec shared = result.value();
    const std::string expected_canonical(shared.canonical_bytes());
    const std::string expected_manifest(shared.manifest_bytes());
    const auto expected_digest = shared.digest();

    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < 8U; ++index) {
        threads.emplace_back([shared, &ok, &expected_canonical,
                              &expected_manifest, expected_digest]() {
            for (std::size_t iteration = 0; iteration < 1000U; ++iteration) {
                if (shared.canonical_bytes() != expected_canonical ||
                    shared.manifest_bytes() != expected_manifest ||
                    shared.digest() != expected_digest ||
                    shared.entry_index("value") != UINT32_C(0)) {
                    ok.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    require(ok.load(std::memory_order_relaxed));
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "deep") {
            return test_deep_manifest_allocation();
        }
        if (argc != 1) {
            return EXIT_FAILURE;
        }
        if (const int status = test_ordered_golden_corpus();
            status != EXIT_SUCCESS) {
            return status;
        }
        if (const int status = test_identity_normalization_and_significance();
            status != EXIT_SUCCESS) {
            return status;
        }
        if (const int status = test_manifest_indexes_facts_and_capabilities();
            status != EXIT_SUCCESS) {
            return status;
        }
        if (const int status =
                test_schema_artifacts_and_manifest_schema_contract();
            status != EXIT_SUCCESS) {
            return status;
        }
        if (const int status =
                test_shared_const_reads();
            status != EXIT_SUCCESS) {
            return status;
        }
        return test_deep_manifest_allocation();
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return EXIT_FAILURE;
    }
}
