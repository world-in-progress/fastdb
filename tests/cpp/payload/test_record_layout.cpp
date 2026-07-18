#include "TestSupport.hpp"

#include "payload/build/PayloadBuilder.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/NormalizedInteger.hpp"
#include "payload/layout/RecordLayout.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"

#include <fastdb_payload.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <string_view>

namespace {

using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::layout::RuntimeSchema;
using fastdb::payload::spec::CompiledSpec;

Result<CompiledSpec> compile(std::string_view source) {
    return CompiledSpec::compile(source);
}

constexpr std::string_view runtime_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"record.v1",
  "entries":[
    {"id":"a_row","cardinality":"one","type":{"kind":"component","id":"Reach"}},
    {"id":"b_nested","cardinality":"one","type":{"kind":"list","items":{"kind":"list","items":{"kind":"component","id":"Reach"}}}}
  ],
  "components":[
    {"id":"AUnreachable","kind":"record","fields":[
      {"id":"ghost","type":{"kind":"list","items":{"kind":"u8"}}}
    ]},
    {"id":"Empty","kind":"record","fields":[]},
    {"id":"Leaf","kind":"record","fields":[
      {"id":"a_nullable","type":{"kind":"u16","nullable":true}},
      {"id":"b_f64","type":{"kind":"f64"}}
    ]},
    {"id":"Reach","kind":"record","fields":[
      {"id":"a_empty","type":{"kind":"component","id":"Empty"}},
      {"id":"b_leaf","type":{"kind":"component","id":"Leaf","nullable":true}},
      {"id":"c_list","type":{"kind":"list","items":{"kind":"component","id":"Leaf","nullable":true}}},
      {"id":"d_reuse","type":{"kind":"component","id":"Leaf"}}
    ]}
  ]
})";

int test_runtime_ids_reachability_and_component_layout() {
    auto compiled = compile(runtime_spec);
    require(compiled.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    const RuntimeSchema& schema = runtime.value();

    require(schema.type_count() == UINT32_C(13));
    for (std::uint32_t id = UINT32_C(0); id < UINT32_C(13); ++id) {
        require(schema.find_type(id) != nullptr);
        require(schema.find_type(id)->runtime_type_id == id);
    }

    const auto& entries = compiled.value().resolved().entries();
    require(schema.runtime_id(entries[0].type) == UINT32_C(0));
    require(schema.runtime_id(entries[1].type) == UINT32_C(1));
    require(schema.runtime_id(*entries[1].type.items) == UINT32_C(2));
    require(schema.runtime_id(*entries[1].type.items->items) ==
            UINT32_C(3));

    const auto& components = compiled.value().resolved().components();
    require(schema.runtime_id(components[0].fields[0].type) == UINT32_C(4));
    require(schema.runtime_id(*components[0].fields[0].type.items) ==
            UINT32_C(5));
    require(schema.runtime_id(components[2].fields[0].type) == UINT32_C(6));
    require(schema.runtime_id(components[2].fields[1].type) == UINT32_C(7));
    require(schema.runtime_id(components[3].fields[0].type) == UINT32_C(8));
    require(schema.runtime_id(components[3].fields[1].type) == UINT32_C(9));
    require(schema.runtime_id(components[3].fields[2].type) == UINT32_C(10));
    require(schema.runtime_id(*components[3].fields[2].type.items) ==
            UINT32_C(11));
    require(schema.runtime_id(components[3].fields[3].type) == UINT32_C(12));

    require(!schema.component_reachable(UINT32_C(0)));
    require(schema.component_reachable(UINT32_C(1)));
    require(schema.component_reachable(UINT32_C(2)));
    require(schema.component_reachable(UINT32_C(3)));
    require(schema.components().size() == 3U);
    require(schema.component(UINT32_C(0)) == nullptr);

    require(schema.list_nodes().size() == 3U);
    require(schema.list_nodes()[0].owner_runtime_type_id == UINT32_C(1));
    require(schema.list_nodes()[0].item_runtime_type_id == UINT32_C(2));
    require(schema.list_nodes()[1].owner_runtime_type_id == UINT32_C(2));
    require(schema.list_nodes()[1].item_runtime_type_id == UINT32_C(3));
    require(schema.list_nodes()[2].owner_runtime_type_id == UINT32_C(10));
    require(schema.list_nodes()[2].item_runtime_type_id == UINT32_C(11));

    const auto* empty = schema.component(UINT32_C(1));
    require(empty != nullptr);
    require(empty->stride == UINT32_C(1));
    require(empty->alignment == UINT32_C(1));
    require(empty->validity_bytes == UINT32_C(0));

    const auto* leaf = schema.component(UINT32_C(2));
    require(leaf != nullptr);
    require(leaf->validity_bytes == UINT32_C(1));
    require(leaf->fields.size() == 2U);
    require(leaf->fields[0].offset == UINT32_C(2));
    require(leaf->fields[0].validity_bit == UINT32_C(0));
    require(leaf->fields[1].offset == UINT32_C(8));
    require(leaf->fields[1].validity_bit == UINT32_MAX);
    require(leaf->stride == UINT32_C(16));
    require(leaf->alignment == UINT32_C(8));

    const auto* reach = schema.component(UINT32_C(3));
    require(reach != nullptr);
    require(reach->validity_bytes == UINT32_C(1));
    require(reach->fields.size() == 4U);
    require(reach->fields[0].offset == UINT32_C(1));
    require(reach->fields[1].offset == UINT32_C(8));
    require(reach->fields[1].validity_bit == UINT32_C(0));
    require(reach->fields[2].offset == UINT32_C(24));
    require(reach->fields[3].offset == UINT32_C(40));
    require(reach->fields[1].slot_stride == leaf->stride);
    require(reach->fields[3].slot_stride == leaf->stride);
    require(reach->stride == UINT32_C(56));
    require(reach->alignment == UINT32_C(8));
    return EXIT_SUCCESS;
}

int test_builder_consumes_the_same_runtime_schema_ids() {
    auto compiled = compile(runtime_spec);
    require(compiled.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();

    require(builder.begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(builder.begin_list(UINT64_C(0)).has_value());
    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(builder.begin_component().has_value());
    require(builder.begin_component().has_value());
    require(builder.push_null().has_value());
    require(builder.begin_list(UINT64_C(0)).has_value());
    require(builder.begin_component().has_value());
    require(builder.push_null().has_value());
    require(builder.push_f64_bits(UINT64_C(0x3ff0000000000000)).has_value());
    auto frozen = builder.freeze();
    require(frozen.has_value());

    std::set<std::uint32_t> observed;
    for (const auto& node : frozen.value().nodes()) {
        require(node.runtime_type_id < runtime.value().type_count());
        require(runtime.value().find_type(node.runtime_type_id) != nullptr);
        require(runtime.value()
                    .find_type(node.runtime_type_id)
                    ->runtime_type_id == node.runtime_type_id);
        observed.insert(node.runtime_type_id);
    }
    const std::set<std::uint32_t> expected{
        UINT32_C(0), UINT32_C(1), UINT32_C(6), UINT32_C(7),
        UINT32_C(8), UINT32_C(9), UINT32_C(10), UINT32_C(12)};
    require(observed == expected);
    return EXIT_SUCCESS;
}

int test_checked_math_and_little_endian_boundaries() {
    const JsonPointer path = JsonPointer{}.append("wire");
    auto narrow_exact = fastdb::payload::layout::checked_narrow_u32(
        UINT32_MAX, path);
    require(narrow_exact.has_value());
    require(narrow_exact.value() == UINT32_MAX);
    require(fastdb::payload::layout::checked_narrow_u32(
                static_cast<std::uint64_t>(UINT32_MAX) + UINT64_C(1), path)
                .error()
                .code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);

    auto zero_add = fastdb::payload::layout::checked_add_u64(
        UINT64_C(0), UINT64_C(0), path);
    require(zero_add.has_value() && zero_add.value() == UINT64_C(0));
    require(fastdb::payload::layout::checked_add_u64(
                UINT64_MAX, UINT64_C(1), path)
                .error()
                .code() == FDB_PAYLOAD_E_LENGTH_OVERFLOW);
    std::uint64_t accumulated = UINT64_C(0);
    require(fastdb::payload::layout::checked_accumulate_u64(
                accumulated, UINT64_C(0), path)
                .has_value());
    require(accumulated == UINT64_C(0));
    accumulated = UINT64_MAX - UINT64_C(1);
    require(fastdb::payload::layout::checked_accumulate_u64(
                accumulated, UINT64_C(1), path)
                .has_value());
    require(accumulated == UINT64_MAX);
    require(fastdb::payload::layout::checked_accumulate_u64(
                accumulated, UINT64_C(1), path)
                .error()
                .code() == FDB_PAYLOAD_E_LENGTH_OVERFLOW);
    require(accumulated == UINT64_MAX);
    require(fastdb::payload::layout::checked_multiply_u64(
                UINT64_MAX, UINT64_C(0), path)
                .value() == UINT64_C(0));
    require(fastdb::payload::layout::checked_multiply_u64(
                UINT64_MAX, UINT64_C(2), path)
                .error()
                .code() == FDB_PAYLOAD_E_LENGTH_OVERFLOW);
    require(fastdb::payload::layout::checked_align_up_u64(
                UINT64_C(16), UINT32_C(8), path)
                .value() == UINT64_C(16));
    require(fastdb::payload::layout::checked_align_up_u64(
                UINT64_MAX, UINT32_C(8), path)
                .error()
                .code() == FDB_PAYLOAD_E_LENGTH_OVERFLOW);
    require(fastdb::payload::layout::checked_range_end(
                UINT64_C(4), UINT64_C(4), UINT64_C(8), path)
                .value() == UINT64_C(8));
    require(fastdb::payload::layout::checked_range_end(
                UINT64_C(5), UINT64_C(4), UINT64_C(8), path)
                .error()
                .code() == FDB_PAYLOAD_E_OUT_OF_BOUNDS);

    std::array<std::uint8_t, 8> bytes{};
    require(fastdb::payload::layout::store_u64_le(
                bytes.data(), bytes.size(), UINT64_C(0),
                UINT64_C(0x0123456789abcdef), path)
                .has_value());
    require(bytes[0] == UINT8_C(0xef));
    require(bytes[7] == UINT8_C(0x01));
    require(fastdb::payload::layout::load_u64_le(
                bytes.data(), bytes.size(), UINT64_C(0), path)
                .value() == UINT64_C(0x0123456789abcdef));
    require(fastdb::payload::layout::load_u32_le(
                bytes.data(), bytes.size(), UINT64_MAX, path)
                .error()
                .code() == FDB_PAYLOAD_E_LENGTH_OVERFLOW);
    return EXIT_SUCCESS;
}

int test_exact_normalized_integer_authority() {
    using fastdb::payload::layout::dequantize_normalized;
    using fastdb::payload::layout::quantize_normalized;
    const JsonPointer path = JsonPointer{}.append("normalized");

    require(quantize_normalized(UINT64_C(0x0000000000000000), 0.0,
                                255.0, UINT32_C(255), "u8n", path)
                .value() == UINT32_C(0));
    require(quantize_normalized(UINT64_C(0x406fe00000000000), 0.0,
                                255.0, UINT32_C(255), "u8n", path)
                .value() == UINT32_C(255));
    require(quantize_normalized(UINT64_C(0x3fe0000000000000), 0.0,
                                255.0, UINT32_C(255), "u8n", path)
                .value() == UINT32_C(0));
    require(quantize_normalized(UINT64_C(0x3ff8000000000000), 0.0,
                                255.0, UINT32_C(255), "u8n", path)
                .value() == UINT32_C(2));
    require(quantize_normalized(UINT64_C(0x0000000000000001), 0.0,
                                255.0, UINT32_C(255), "u8n", path)
                .value() == UINT32_C(0));
    require(quantize_normalized(UINT64_C(0x8000000000000000), 0.0,
                                255.0, UINT32_C(255), "u8n", path)
                .value() == UINT32_C(0));

    require(dequantize_normalized(UINT32_C(0), 0.0, 255.0,
                                  UINT32_C(255), "u8n", path)
                .value() == UINT64_C(0x0000000000000000));
    require(dequantize_normalized(UINT32_C(255), 0.0, 255.0,
                                  UINT32_C(255), "u8n", path)
                .value() == UINT64_C(0x406fe00000000000));
    require(dequantize_normalized(UINT32_C(0), -32768.0, 32767.0,
                                  UINT32_C(65535), "u16n", path)
                .value() == UINT64_C(0xc0e0000000000000));
    require(dequantize_normalized(UINT32_C(65535), -32768.0, 32767.0,
                                  UINT32_C(65535), "u16n", path)
                .value() == UINT64_C(0x40dfffc000000000));

    constexpr double large_min = -0x1p1000;
    constexpr double large_max = 0x1p1000;
    require(dequantize_normalized(UINT32_C(127), large_min, large_max,
                                  UINT32_C(255), "u8n", path)
                .value() == UINT64_C(0xfdf0101010101010));
    require(dequantize_normalized(UINT32_C(128), large_min, large_max,
                                  UINT32_C(255), "u8n", path)
                .value() == UINT64_C(0x7df0101010101010));
    require(quantize_normalized(UINT64_C(0xfe70000000000000), large_min,
                                large_max, UINT32_C(255), "u8n", path)
                .value() == UINT32_C(0));
    require(quantize_normalized(UINT64_C(0x7e70000000000000), large_min,
                                large_max, UINT32_C(255), "u8n", path)
                .value() == UINT32_C(255));

    require(!quantize_normalized(UINT64_C(0xbca0000000000000), 0.0,
                                 255.0, UINT32_C(255), "u8n", path)
                 .has_value());
    require(!quantize_normalized(UINT64_C(0x406fe00000000001), 0.0,
                                 255.0, UINT32_C(255), "u8n", path)
                 .has_value());
    require(!quantize_normalized(UINT64_C(0x7ff0000000000000), 0.0,
                                 255.0, UINT32_C(255), "u8n", path)
                 .has_value());

    auto compiled = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"norm","cardinality":"one","type":{"kind":"u8n","min":-1,"max":1}}],"components":[]})");
    require(compiled.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    require(created.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(created.value()
                .push_u8n_bits(UINT64_C(0x0000000000000000))
                .has_value());
    auto frozen = created.value().freeze();
    require(frozen.has_value());
    auto layout = fastdb::payload::layout::RecordLayout::plan(
        runtime.value(), frozen.value());
    require(layout.has_value());
    require(layout.value().regions().size() == 1U);
    require(layout.value().regions()[0].stride == UINT32_C(1));
    return EXIT_SUCCESS;
}

std::string component_id(std::uint32_t index) {
    std::string digits = std::to_string(index);
    return "c" + std::string(5U - digits.size(), '0') + digits;
}

std::string doubling_component_spec(bool reachable) {
    constexpr std::uint32_t component_count = UINT32_C(34);
    std::string source =
        "{\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\",";
    if (reachable) {
        source +=
            "\"entries\":[{\"id\":\"root\",\"cardinality\":\"one\","
            "\"type\":{\"kind\":\"component\",\"id\":\"c00000\"}}],";
    } else {
        source +=
            "\"entries\":[{\"id\":\"root\",\"cardinality\":\"one\","
            "\"type\":{\"kind\":\"u8\"}}],";
    }
    source += "\"components\":[";
    for (std::uint32_t index = UINT32_C(0); index < component_count; ++index) {
        if (index != UINT32_C(0)) {
            source += ',';
        }
        source += "{\"id\":\"" + component_id(index) +
                  "\",\"kind\":\"record\",\"fields\":[";
        if (index + UINT32_C(1) < component_count) {
            const std::string next = component_id(index + UINT32_C(1));
            source +=
                "{\"id\":\"left\",\"type\":{\"kind\":\"component\","
                "\"id\":\"" +
                next +
                "\"}},{\"id\":\"right\",\"type\":{\"kind\":"
                "\"component\",\"id\":\"" +
                next + "\"}}";
        }
        source += "]}";
    }
    source += "]}";
    return source;
}

int test_unreachable_component_layout_overflow_is_not_compiled() {
    auto compiled = compile(doubling_component_spec(false));
    require(compiled.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    require(runtime.value().type_count() == UINT32_C(67));
    require(runtime.value().components().empty());
    require(!runtime.value().component_reachable(UINT32_C(0)));

    auto reachable = compile(doubling_component_spec(true));
    require(reachable.has_value());
    auto rejected = RuntimeSchema::compile(reachable.value());
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    return EXIT_SUCCESS;
}

int test_reverse_component_chain_layout_is_iterative_and_linear() {
    constexpr std::uint32_t component_count = UINT32_C(2048);
    std::string source =
        "{\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\","
        "\"entries\":[{\"id\":\"root\",\"cardinality\":\"one\","
        "\"type\":{\"kind\":\"component\",\"id\":\"" +
        component_id(UINT32_C(0)) + "\"}}],\"components\":[";
    for (std::uint32_t index = UINT32_C(0); index < component_count; ++index) {
        if (index != UINT32_C(0)) {
            source += ',';
        }
        source += "{\"id\":\"" + component_id(index) +
                  "\",\"kind\":\"record\",\"fields\":[";
        if (index + UINT32_C(1) < component_count) {
            source += "{\"id\":\"next\",\"type\":{\"kind\":"
                      "\"component\",\"id\":\"" +
                      component_id(index + UINT32_C(1)) + "\"}}";
        }
        source += "]}";
    }
    source += "]}";
    auto compiled = compile(source);
    require(compiled.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    require(runtime.value().components().size() == component_count);
    const auto* first = runtime.value().component(UINT32_C(0));
    const auto* last =
        runtime.value().component(component_count - UINT32_C(1));
    require(first != nullptr);
    require(last != nullptr);
    require(first->stride == UINT32_C(1));
    require(last->stride == UINT32_C(1));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_runtime_ids_reachability_and_component_layout() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_builder_consumes_the_same_runtime_schema_ids() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_checked_math_and_little_endian_boundaries() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_exact_normalized_integer_authority() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_unreachable_component_layout_overflow_is_not_compiled() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_reverse_component_chain_layout_is_iterative_and_linear();
}
