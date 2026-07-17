#include "TestSupport.hpp"

#include "payload/build/PayloadBuilder.hpp"
#include "payload/layout/InputSpan.hpp"
#include "payload/layout/NormalizedInteger.hpp"
#include "payload/layout/TextEncoding.hpp"
#include "payload/spec/CompiledSpec.hpp"

#include <fastdb_payload.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace allocation_failure {

thread_local bool fail_next = false;
thread_local std::size_t fail_next_at_least = 0U;
thread_local bool failure_triggered = false;

void* allocate(std::size_t size) {
    if (fail_next ||
        (fail_next_at_least != 0U && size >= fail_next_at_least)) {
        fail_next = false;
        fail_next_at_least = 0U;
        failure_triggered = true;
        throw std::bad_alloc();
    }
    void* const pointer = std::malloc(size == 0U ? 1U : size);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

}  // namespace allocation_failure

void* operator new(std::size_t size) {
    return allocation_failure::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_failure::allocate(size);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {

using fastdb::payload::build::BuilderLimits;
using fastdb::payload::build::FixedRun;
using fastdb::payload::build::LogicalPayload;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::build::ValueNode;
using fastdb::payload::build::ValueTag;
using fastdb::payload::build::default_builder_limits;
using fastdb::payload::error::Error;
using fastdb::payload::error::Result;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::spec::CompileLimits;
using fastdb::payload::spec::CompiledSpec;

std::uint64_t f64_bits(double value) {
    std::uint64_t bits = UINT64_C(0);
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

template <typename T>
bool exact_error(const Result<T>& result,
                 std::uint32_t code,
                 std::string_view path,
                 std::string_view details) {
    if (result.has_value()) {
        return false;
    }
    const Error& error = result.error();
    return error.code() == code && error.symbol() ==
               fastdb::payload::error::symbol_for_code(code) &&
           error.path() == path && error.details_json() == details;
}

Result<CompiledSpec> compile(std::string_view source,
                             std::uint32_t depth = UINT32_C(128)) {
    CompileLimits limits{};
    limits.json.max_source_bytes = UINT64_C(64) * UINT64_C(1024) *
                                   UINT64_C(1024);
    limits.json.max_json_values = UINT64_C(100000);
    limits.json.max_nesting_depth = depth;
    return CompiledSpec::compile(source, limits);
}

std::string deep_list_spec(std::uint32_t depth) {
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

int test_empty_and_default_limits() {
    const BuilderLimits defaults = default_builder_limits();
    require(defaults.max_value_nodes == UINT64_C(10000000));
    require(defaults.max_list_elements == UINT64_C(10000000));
    require(defaults.max_text_bytes == (UINT64_C(1) << 30));
    require(defaults.max_opaque_bytes == (UINT64_C(1) << 30));
    require(defaults.max_nesting_depth == UINT64_C(1024));
    require(defaults.max_total_builder_bytes == (UINT64_C(1) << 30));

    auto empty_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]})");
    require(empty_spec.has_value());
    auto empty_builder = PayloadBuilder::create(std::move(empty_spec).value());
    require(empty_builder.has_value());
    auto empty = empty_builder.value().freeze();
    require(empty.has_value());
    require(empty.value().nodes().empty());
    require(empty.value().entry_roots().empty());
    require(empty.value().byte_storage().empty());

    auto many_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"values","cardinality":"many","type":{"kind":"u8"}}],"components":[]})");
    require(many_spec.has_value());
    auto many_builder = PayloadBuilder::create(std::move(many_spec).value());
    require(many_builder.has_value());
    require(many_builder.value().begin_entry(UINT32_C(0), UINT64_C(0)).has_value());
    auto many = many_builder.value().freeze();
    require(many.has_value());
    require(many.value().entry_roots().size() == 1U);
    require(many.value().nodes().size() == 1U);
    const ValueNode& root = many.value().nodes().front();
    require(root.tag == ValueTag::sequence);
    require(root.child_count == UINT64_C(0));
    require(root.first_child == fastdb::payload::build::invalid_node_index);
    return EXIT_SUCCESS;
}

int test_shared_text_encoding_helpers() {
    const JsonPointer path = JsonPointer{}.append("entries").append("text");
    require(fastdb::payload::layout::validate_utf8(
                std::string_view("\xf0\x9f\x98\x80", 4U), path)
                .has_value());
    const std::array<std::string, 4> invalid_utf8{{
        std::string("\xc0\x80", 2U),
        std::string("\xed\xa0\x80", 3U),
        std::string("\xe2\x28\xa1", 3U),
        std::string("\xf0\x9f\x98", 3U),
    }};
    for (const std::string& bytes : invalid_utf8) {
        const auto result =
            fastdb::payload::layout::validate_utf8(bytes, path);
        require(exact_error(
            result, FDB_PAYLOAD_E_INVALID_TEXT_ENCODING, "/entries/text",
            R"({"encoding":"utf-8","reason":"invalid_sequence"})"));
    }

    const std::array<std::uint16_t, 3> valid_utf16{{
        UINT16_C(0x0041), UINT16_C(0xd83d), UINT16_C(0xde00)}};
    require(fastdb::payload::layout::validate_utf16(
                valid_utf16.data(), valid_utf16.size(), path)
                .has_value());
    const std::array<std::uint16_t, 1> lead{{UINT16_C(0xd800)}};
    const std::array<std::uint16_t, 1> trail{{UINT16_C(0xdc00)}};
    require(fastdb::payload::layout::validate_utf16(lead.data(), lead.size(),
                                                     path)
                .error()
                .code() == FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);
    require(fastdb::payload::layout::validate_utf16(
                trail.data(), trail.size(), path)
                .error()
                .code() == FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);
    std::vector<std::uint8_t> little_endian;
    little_endian.reserve(valid_utf16.size() * 2U);
    fastdb::payload::layout::append_utf16le(
        little_endian, valid_utf16.data(), valid_utf16.size());
    const std::array<std::uint8_t, 6> expected{{
        UINT8_C(0x41), UINT8_C(0x00), UINT8_C(0x3d), UINT8_C(0xd8),
        UINT8_C(0x00), UINT8_C(0xde)}};
    require(little_endian.size() == expected.size());
    require(std::memcmp(little_endian.data(), expected.data(),
                        expected.size()) == 0);
    return EXIT_SUCCESS;
}

int test_scalars_text_and_out_of_order_entries() {
    auto compiled = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"b","cardinality":"one","type":{"kind":"bool"}},{"id":"u8","cardinality":"one","type":{"kind":"u8"}},{"id":"u16","cardinality":"one","type":{"kind":"u16"}},{"id":"u32","cardinality":"one","type":{"kind":"u32"}},{"id":"i32","cardinality":"one","type":{"kind":"i32"}},{"id":"u8n","cardinality":"one","type":{"kind":"u8n","min":-1,"max":1}},{"id":"u16n","cardinality":"one","type":{"kind":"u16n","min":0,"max":10}},{"id":"f32","cardinality":"one","type":{"kind":"f32"}},{"id":"f64","cardinality":"one","type":{"kind":"f64"}},{"id":"str","cardinality":"one","type":{"kind":"str","nullable":true}},{"id":"wstr","cardinality":"one","type":{"kind":"wstr"}},{"id":"bytes","cardinality":"one","type":{"kind":"bytes"}}],"components":[]})");
    require(compiled.has_value());
    auto created = PayloadBuilder::create(std::move(compiled).value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();

    const std::array<std::uint8_t, 4> opaque{{UINT8_C(0), UINT8_C(255),
                                              UINT8_C(7), UINT8_C(0)}};
    require(builder.begin_entry(UINT32_C(11), UINT64_C(1)).has_value());
    require(builder.push_bytes(opaque.data(), opaque.size()).has_value());
    const std::array<std::uint16_t, 4> wide{{UINT16_C(0x0041),
                                             UINT16_C(0xd83d),
                                             UINT16_C(0xde00),
                                             UINT16_C(0)}};
    require(builder.begin_entry(UINT32_C(10), UINT64_C(1)).has_value());
    require(builder.push_wstr(wide.data(), wide.size()).has_value());
    require(builder.begin_entry(UINT32_C(9), UINT64_C(1)).has_value());
    require(builder.push_null().has_value());
    require(builder.begin_entry(UINT32_C(8), UINT64_C(1)).has_value());
    require(builder.push_f64_bits(UINT64_C(0x7ff0000000000042)).has_value());
    require(builder.begin_entry(UINT32_C(7), UINT64_C(1)).has_value());
    require(builder.push_f32_bits(UINT32_C(0xff800001)).has_value());
    require(builder.begin_entry(UINT32_C(6), UINT64_C(1)).has_value());
    require(builder.push_u16n_bits(f64_bits(5.0)).has_value());
    require(builder.begin_entry(UINT32_C(5), UINT64_C(1)).has_value());
    require(builder.push_u8n_bits(f64_bits(-0.0)).has_value());
    require(builder.begin_entry(UINT32_C(4), UINT64_C(1)).has_value());
    require(builder.push_i32(std::numeric_limits<std::int32_t>::min()).has_value());
    require(builder.begin_entry(UINT32_C(3), UINT64_C(1)).has_value());
    require(builder.push_u32(UINT32_MAX).has_value());
    require(builder.begin_entry(UINT32_C(2), UINT64_C(1)).has_value());
    require(builder.push_u16(UINT16_MAX).has_value());
    require(builder.begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(builder.push_u8(UINT8_MAX).has_value());
    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(builder.push_bool(UINT8_C(1)).has_value());

    auto frozen = builder.freeze();
    require(frozen.has_value());
    require(frozen.value().entry_roots().size() == 12U);
    require(frozen.value().nodes().size() == 24U);
    for (std::uint32_t entry = UINT32_C(0); entry < UINT32_C(12); ++entry) {
        const auto root_index = frozen.value().entry_roots()[entry];
        require(frozen.value().nodes()[root_index].runtime_type_id == entry);
    }
    require(frozen.value().nodes()[7].scalar_bits_or_offset ==
            UINT64_C(0x7ff0000000000042));
    require(frozen.value().nodes()[9].scalar_bits_or_offset ==
            UINT64_C(0xff800001));
    const std::string_view bytes = frozen.value().byte_storage();
    require(bytes.size() == opaque.size() + wide.size() * 2U);
    require(static_cast<unsigned char>(bytes[4]) == 0x41U);
    require(static_cast<unsigned char>(bytes[6]) == 0x3dU);
    require(static_cast<unsigned char>(bytes[7]) == 0xd8U);
    require(static_cast<unsigned char>(bytes[8]) == 0x00U);
    require(static_cast<unsigned char>(bytes[9]) == 0xdeU);
    return EXIT_SUCCESS;
}

int test_record_batch_components_and_lists() {
    auto compiled = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"rows","cardinality":"many","type":{"kind":"component","id":"Row"}},{"id":"reused","cardinality":"one","type":{"kind":"list","items":{"kind":"component","id":"Empty"}}}],"components":[{"id":"Row","kind":"record","fields":[{"id":"x","type":{"kind":"u32"}},{"id":"labels","type":{"kind":"list","items":{"kind":"str","nullable":true}}},{"id":"empty","type":{"kind":"component","id":"Empty"}}]},{"id":"Empty","kind":"record","fields":[]}]})");
    require(compiled.has_value());
    auto created = PayloadBuilder::create(std::move(compiled).value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();

    require(builder.begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(builder.begin_list(UINT64_C(2)).has_value());
    require(builder.begin_component().has_value());
    require(builder.begin_component().has_value());

    require(builder.begin_entry(UINT32_C(0), UINT64_C(2)).has_value());
    for (std::uint32_t row = UINT32_C(0); row < UINT32_C(2); ++row) {
        require(builder.begin_component().has_value());
        require(builder.push_u32(row + UINT32_C(10)).has_value());
        require(builder.begin_list(UINT64_C(2)).has_value());
        require(builder.push_str(row == 0U ? "a" : "b").has_value());
        require(builder.push_null().has_value());
        require(builder.begin_component().has_value());
    }

    auto frozen = builder.freeze();
    require(frozen.has_value());
    require(frozen.value().nodes().size() == 17U);
    const ValueNode& row_sequence =
        frozen.value().nodes()[frozen.value().entry_roots()[0]];
    require(row_sequence.tag == ValueTag::sequence);
    require(row_sequence.child_count == UINT64_C(2));
    const ValueNode& first_row =
        frozen.value().nodes()[row_sequence.first_child];
    require(first_row.tag == ValueTag::component);
    require(first_row.child_count == UINT64_C(3));
    require(frozen.value().byte_storage() == "ab");
    return EXIT_SUCCESS;
}

int test_fixed_runs_and_transactional_failures() {
    auto compiled = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"values","cardinality":"many","type":{"kind":"u16","nullable":true}},{"id":"flags","cardinality":"many","type":{"kind":"bool"}},{"id":"normalized","cardinality":"many","type":{"kind":"u8n","min":-1,"max":1}},{"id":"list","cardinality":"one","type":{"kind":"list","items":{"kind":"f64"}}},{"id":"u8s","cardinality":"many","type":{"kind":"u8"}},{"id":"u32s","cardinality":"many","type":{"kind":"u32"}},{"id":"i32s","cardinality":"many","type":{"kind":"i32"}},{"id":"u16ns","cardinality":"many","type":{"kind":"u16n","min":0,"max":10}},{"id":"f32s","cardinality":"many","type":{"kind":"f32"}}],"components":[]})");
    require(compiled.has_value());
    auto created = PayloadBuilder::create(std::move(compiled).value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();

    require(builder.begin_entry(UINT32_C(0), UINT64_C(3)).has_value());
    const std::array<std::uint16_t, 6> strided{{UINT16_C(10), UINT16_C(99),
                                                UINT16_C(20), UINT16_C(99),
                                                UINT16_C(30), UINT16_C(99)}};
    const std::array<std::uint8_t, 2> validity{{UINT8_C(0), UINT8_C(0x0a)}};
    FixedRun u16_run{reinterpret_cast<const std::uint8_t*>(strided.data()),
                     sizeof(strided), UINT64_C(3), UINT64_C(4),
                     validity.data(), validity.size(), UINT64_C(9)};
    require(builder.push_fixed_run(u16_run).has_value());

    require(builder.begin_entry(UINT32_C(1), UINT64_C(2)).has_value());
    const std::array<std::uint8_t, 2> bad_flags{{UINT8_C(1), UINT8_C(2)}};
    FixedRun bad_bool{bad_flags.data(), bad_flags.size(), UINT64_C(2),
                      UINT64_C(0), nullptr, UINT64_C(0), UINT64_C(0)};
    auto invalid_bool = builder.push_fixed_run(bad_bool);
    require(exact_error(invalid_bool, FDB_PAYLOAD_E_OUT_OF_RANGE,
                        "/entries/flags/1",
                        R"({"kind":"bool","reason":"invalid_boolean_byte"})"));
    const std::array<std::uint8_t, 1> null_bits{{UINT8_C(0)}};
    FixedRun invalid_null{bad_flags.data(), bad_flags.size(), UINT64_C(2),
                          UINT64_C(0), null_bits.data(), null_bits.size(),
                          UINT64_C(0)};
    require(builder.push_fixed_run(invalid_null).error().code() ==
            FDB_PAYLOAD_E_UNEXPECTED_NULL);
    const std::array<std::uint8_t, 2> flags{{UINT8_C(1), UINT8_C(0)}};
    FixedRun bool_run{flags.data(), flags.size(), flags.size(), UINT64_C(0),
                      nullptr, UINT64_C(0), UINT64_C(0)};
    require(builder.push_fixed_run(bool_run).has_value());

    require(builder.begin_entry(UINT32_C(2), UINT64_C(4)).has_value());
    const std::array<std::uint64_t, 4> normalized{{
        f64_bits(-1.0), f64_bits(-1.0 + (1.0 / 255.0)), f64_bits(0.0),
        f64_bits(1.0)}};
    FixedRun normalized_run{
        reinterpret_cast<const std::uint8_t*>(normalized.data()),
        sizeof(normalized), normalized.size(), UINT64_C(0), nullptr,
        UINT64_C(0), UINT64_C(0)};
    require(builder.push_fixed_run(normalized_run).has_value());

    require(builder.begin_entry(UINT32_C(3), UINT64_C(1)).has_value());
    require(builder.begin_list(UINT64_C(4)).has_value());
    const std::array<std::uint64_t, 4> doubles{{
        UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
        UINT64_C(0x7ff0000000000042), UINT64_C(0xfff8000000001234)}};
    FixedRun double_run{reinterpret_cast<const std::uint8_t*>(doubles.data()),
                        sizeof(doubles), doubles.size(), UINT64_C(0), nullptr,
                        UINT64_C(0), UINT64_C(0)};
    require(builder.push_fixed_run(double_run).has_value());

    require(builder.begin_entry(UINT32_C(4), UINT64_C(3)).has_value());
    const std::array<std::uint8_t, 3> u8s{{UINT8_C(0), UINT8_C(42),
                                           UINT8_MAX}};
    FixedRun u8_run{u8s.data(), u8s.size(), u8s.size(), UINT64_C(0), nullptr,
                    UINT64_C(0), UINT64_C(0)};
    require(builder.push_fixed_run(u8_run).has_value());

    require(builder.begin_entry(UINT32_C(5), UINT64_C(2)).has_value());
    const std::array<std::uint32_t, 4> u32s{{UINT32_C(7), UINT32_C(99),
                                             UINT32_MAX, UINT32_C(99)}};
    FixedRun u32_run{reinterpret_cast<const std::uint8_t*>(u32s.data()),
                     sizeof(u32s), UINT64_C(2), UINT64_C(8), nullptr,
                     UINT64_C(0), UINT64_C(0)};
    require(builder.push_fixed_run(u32_run).has_value());

    require(builder.begin_entry(UINT32_C(6), UINT64_C(2)).has_value());
    const std::array<std::int32_t, 2> i32s{{
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()}};
    FixedRun i32_run{reinterpret_cast<const std::uint8_t*>(i32s.data()),
                     sizeof(i32s), i32s.size(), UINT64_C(0), nullptr,
                     UINT64_C(0), UINT64_C(0)};
    require(builder.push_fixed_run(i32_run).has_value());

    require(builder.begin_entry(UINT32_C(7), UINT64_C(3)).has_value());
    const std::array<std::uint64_t, 3> u16ns{{f64_bits(0.0), f64_bits(5.0),
                                              f64_bits(10.0)}};
    FixedRun u16n_run{reinterpret_cast<const std::uint8_t*>(u16ns.data()),
                      sizeof(u16ns), u16ns.size(), UINT64_C(0), nullptr,
                      UINT64_C(0), UINT64_C(0)};
    require(builder.push_fixed_run(u16n_run).has_value());

    require(builder.begin_entry(UINT32_C(8), UINT64_C(3)).has_value());
    const std::array<std::uint32_t, 3> f32s{{UINT32_C(0x80000000),
                                             UINT32_C(0x7f800000),
                                             UINT32_C(0x7fc00042)}};
    FixedRun f32_run{reinterpret_cast<const std::uint8_t*>(f32s.data()),
                     sizeof(f32s), f32s.size(), UINT64_C(0), nullptr,
                     UINT64_C(0), UINT64_C(0)};
    require(builder.push_fixed_run(f32_run).has_value());
    auto frozen = builder.freeze();
    require(frozen.has_value());

    auto overflow_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"many","type":{"kind":"u32"}}],"components":[]})");
    require(overflow_spec.has_value());
    auto overflow_builder = PayloadBuilder::create(std::move(overflow_spec).value());
    require(overflow_builder.has_value());
    require(overflow_builder.value().begin_entry(UINT32_C(0), UINT64_C(2)).has_value());
    std::uint32_t word = UINT32_C(7);
    FixedRun overflow{reinterpret_cast<const std::uint8_t*>(&word), UINT64_MAX,
                      UINT64_C(2), UINT64_MAX, nullptr, UINT64_C(0),
                      UINT64_C(0)};
    auto overflow_result = overflow_builder.value().push_fixed_run(overflow);
    require(exact_error(overflow_result,
                        FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW,
                        "/entries/v/0",
                        R"({"reason":"fixed_run_data_span_overflow"})"));
    FixedRun short_span{reinterpret_cast<const std::uint8_t*>(&word),
                        UINT64_C(3), UINT64_C(1), UINT64_C(0), nullptr,
                        UINT64_C(0), UINT64_C(0)};
    auto short_result = overflow_builder.value().push_fixed_run(short_span);
    require(exact_error(short_result, FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS,
                        "/entries/v/0",
                        R"({"available":"3","reason":"fixed_run_data_too_short","required":"4"})"));
    FixedRun zero_count{reinterpret_cast<const std::uint8_t*>(&word),
                        sizeof(word), UINT64_C(0), UINT64_C(0), nullptr,
                        UINT64_C(0), UINT64_C(0)};
    require(exact_error(overflow_builder.value().push_fixed_run(zero_count),
                        FDB_PAYLOAD_E_INVALID_ARGUMENT, "/entries/v/0",
                        R"({"reason":"fixed_run_zero_count"})"));
    FixedRun null_data{nullptr, sizeof(word), UINT64_C(1), UINT64_C(0),
                       nullptr, UINT64_C(0), UINT64_C(0)};
    require(exact_error(overflow_builder.value().push_fixed_run(null_data),
                        FDB_PAYLOAD_E_INVALID_ARGUMENT, "/entries/v/0",
                        R"({"reason":"fixed_run_null_data"})"));
    FixedRun invalid_null_validity{
        reinterpret_cast<const std::uint8_t*>(&word), sizeof(word),
        UINT64_C(1), UINT64_C(0), nullptr, UINT64_C(1), UINT64_C(0)};
    require(exact_error(
        overflow_builder.value().push_fixed_run(invalid_null_validity),
        FDB_PAYLOAD_E_INVALID_ARGUMENT, "/entries/v/0",
        R"({"reason":"fixed_run_null_validity"})"));
    const std::array<std::uint8_t, 1> bitmap{{UINT8_C(0xff)}};
    FixedRun short_bitmap{
        reinterpret_cast<const std::uint8_t*>(&word), sizeof(word),
        UINT64_C(1), UINT64_C(0), bitmap.data(), UINT64_C(0), UINT64_C(7)};
    require(exact_error(
        overflow_builder.value().push_fixed_run(short_bitmap),
        FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS, "/entries/v/0",
        R"({"available":"0","reason":"fixed_run_validity_too_short","required":"1"})"));
    FixedRun validity_overflow{
        reinterpret_cast<const std::uint8_t*>(&word), sizeof(word),
        UINT64_C(1), UINT64_C(0), bitmap.data(), bitmap.size(), UINT64_MAX};
    require(exact_error(
        overflow_builder.value().push_fixed_run(validity_overflow),
        FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, "/entries/v/0",
        R"({"reason":"fixed_run_validity_span_overflow"})"));
    FixedRun small_stride{
        reinterpret_cast<const std::uint8_t*>(&word), sizeof(word),
        UINT64_C(1), UINT64_C(2), nullptr, UINT64_C(0), UINT64_C(0)};
    require(exact_error(
        overflow_builder.value().push_fixed_run(small_stride),
        FDB_PAYLOAD_E_INVALID_ARGUMENT, "/entries/v/0",
        R"({"reason":"fixed_run_stride_too_small"})"));
    require(overflow_builder.value().push_u32(UINT32_C(7)).has_value());
    require(overflow_builder.value().push_u32(UINT32_C(8)).has_value());
    require(overflow_builder.value().freeze().has_value());

    auto allocation_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"many","type":{"kind":"u32"}}],"components":[]})");
    require(allocation_spec.has_value());
    auto allocation_builder =
        PayloadBuilder::create(std::move(allocation_spec).value());
    require(allocation_builder.has_value());
    constexpr std::size_t allocation_count = 4096U;
    const std::vector<std::uint32_t> allocation_values(allocation_count,
                                                       UINT32_C(7));
    require(allocation_builder.value()
                .begin_entry(UINT32_C(0), allocation_count)
                .has_value());
    FixedRun allocation_run{
        reinterpret_cast<const std::uint8_t*>(allocation_values.data()),
        allocation_values.size() * sizeof(std::uint32_t),
        allocation_values.size(), UINT64_C(0), nullptr, UINT64_C(0),
        UINT64_C(0)};
    allocation_failure::fail_next = true;
    auto fixed_allocation =
        allocation_builder.value().push_fixed_run(allocation_run);
    require(fixed_allocation.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(allocation_builder.value().push_fixed_run(allocation_run).has_value());
    require(allocation_builder.value().freeze().has_value());

    auto equivalent_spec_a = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"many","type":{"kind":"u32"}}],"components":[]})");
    auto equivalent_spec_b = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"many","type":{"kind":"u32"}}],"components":[]})");
    require(equivalent_spec_a.has_value() && equivalent_spec_b.has_value());
    auto bulk = PayloadBuilder::create(std::move(equivalent_spec_a).value());
    auto individual =
        PayloadBuilder::create(std::move(equivalent_spec_b).value());
    require(bulk.has_value() && individual.has_value());
    const std::array<std::uint32_t, 3> equivalent_values{{
        UINT32_C(1), UINT32_C(2), UINT32_C(3)}};
    FixedRun equivalent_run{
        reinterpret_cast<const std::uint8_t*>(equivalent_values.data()),
        sizeof(equivalent_values), equivalent_values.size(), UINT64_C(0),
        nullptr, UINT64_C(0), UINT64_C(0)};
    require(bulk.value().begin_entry(UINT32_C(0), UINT64_C(3)).has_value());
    require(bulk.value().push_fixed_run(equivalent_run).has_value());
    require(individual.value()
                .begin_entry(UINT32_C(0), UINT64_C(3))
                .has_value());
    for (const std::uint32_t value : equivalent_values) {
        require(individual.value().push_u32(value).has_value());
    }
    auto bulk_payload = bulk.value().freeze();
    auto individual_payload = individual.value().freeze();
    require(bulk_payload.has_value() && individual_payload.has_value());
    require(bulk_payload.value().nodes().size() ==
            individual_payload.value().nodes().size());
    for (std::size_t index = 0U; index < bulk_payload.value().nodes().size();
         ++index) {
        require(std::memcmp(&bulk_payload.value().nodes()[index],
                            &individual_payload.value().nodes()[index],
                            sizeof(ValueNode)) == 0);
    }

    auto mixed_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"mixed","cardinality":"one","type":{"kind":"component","id":"Mixed"}},{"id":"text","cardinality":"one","type":{"kind":"str"}}],"components":[{"id":"Mixed","kind":"record","fields":[{"id":"a","type":{"kind":"u8"}},{"id":"b","type":{"kind":"u16"}}]}]})");
    require(mixed_spec.has_value());
    auto mixed = PayloadBuilder::create(std::move(mixed_spec).value());
    require(mixed.has_value());
    require(mixed.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(mixed.value().begin_component().has_value());
    const std::array<std::uint8_t, 2> two_u8{{UINT8_C(1), UINT8_C(2)}};
    FixedRun crosses_type{two_u8.data(), two_u8.size(), UINT64_C(2),
                          UINT64_C(0), nullptr, UINT64_C(0), UINT64_C(0)};
    require(mixed.value().push_fixed_run(crosses_type).error().code() ==
            FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(mixed.value().push_u8(UINT8_C(1)).has_value());
    require(mixed.value().push_u16(UINT16_C(2)).has_value());
    require(mixed.value().begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(mixed.value().push_fixed_run(u8_run).error().code() ==
            FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(mixed.value().push_str("").has_value());
    require(mixed.value().freeze().has_value());
    return EXIT_SUCCESS;
}

int test_fixed_run_limits_precede_scratch_allocation() {
    auto compiled = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"many","type":{"kind":"u32"}}],"components":[]})");
    require(compiled.has_value());
    BuilderLimits limits = default_builder_limits();
    limits.max_value_nodes = UINT64_C(1);
    auto created = PayloadBuilder::create(std::move(compiled).value(), limits);
    require(created.has_value());

    constexpr std::size_t count = 4096U;
    const std::vector<std::uint32_t> values(count, UINT32_C(7));
    require(created.value().begin_entry(UINT32_C(0), count).has_value());
    const FixedRun run{
        reinterpret_cast<const std::uint8_t*>(values.data()),
        values.size() * sizeof(std::uint32_t), values.size(), UINT64_C(0),
        nullptr, UINT64_C(0), UINT64_C(0)};

    allocation_failure::failure_triggered = false;
    allocation_failure::fail_next_at_least = 4096U;
    const auto limited = created.value().push_fixed_run(run);
    allocation_failure::fail_next_at_least = 0U;
    require(!limited.has_value());
    require(limited.error().code() == FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);
    require(limited.error().path() == "/entries/v/0");
    require(!allocation_failure::failure_triggered);

    const auto retry = created.value().push_fixed_run(run);
    require(!retry.has_value());
    require(retry.error().code() == FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);
    require(retry.error().path() == "/entries/v/0");
    return EXIT_SUCCESS;
}

int test_all_fixed_kinds_match_individual_authoring() {
    const std::string source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"b","cardinality":"many","type":{"kind":"bool"}},{"id":"u8","cardinality":"many","type":{"kind":"u8"}},{"id":"u16","cardinality":"many","type":{"kind":"u16"}},{"id":"u32","cardinality":"many","type":{"kind":"u32"}},{"id":"i32","cardinality":"many","type":{"kind":"i32"}},{"id":"u8n","cardinality":"many","type":{"kind":"u8n","min":-1,"max":1}},{"id":"u16n","cardinality":"many","type":{"kind":"u16n","min":0,"max":10}},{"id":"f32","cardinality":"many","type":{"kind":"f32"}},{"id":"f64","cardinality":"many","type":{"kind":"f64"}}],"components":[]})";
    auto bulk_spec = compile(source);
    auto individual_spec = compile(source);
    require(bulk_spec.has_value() && individual_spec.has_value());
    auto bulk = PayloadBuilder::create(std::move(bulk_spec).value());
    auto individual =
        PayloadBuilder::create(std::move(individual_spec).value());
    require(bulk.has_value() && individual.has_value());

    const std::array<std::uint8_t, 2> booleans{{UINT8_C(0), UINT8_C(1)}};
    const std::array<std::uint8_t, 2> u8s{{UINT8_C(0x12), UINT8_C(0xfe)}};
    const std::array<std::uint16_t, 2> u16s{{UINT16_C(0x1234),
                                             UINT16_C(0xfedc)}};
    const std::array<std::uint32_t, 2> u32s{{UINT32_C(0x12345678),
                                             UINT32_C(0xfedcba98)}};
    const std::array<std::int32_t, 2> i32s{{INT32_C(-123456789),
                                            INT32_C(123456789)}};
    const std::array<std::uint64_t, 4> u8ns{{
        f64_bits(-1.0), f64_bits(-1.0 + (1.0 / 255.0)),
        f64_bits(1.0 - (1.0 / 255.0)), f64_bits(1.0)}};
    const std::array<std::uint64_t, 4> u16ns{{
        f64_bits(0.0), f64_bits(5.0 / 65535.0),
        f64_bits(10.0 - (5.0 / 65535.0)), f64_bits(10.0)}};
    const std::array<std::uint32_t, 2> f32s{{UINT32_C(0x80000000),
                                             UINT32_C(0x7fc01234)}};
    const std::array<std::uint64_t, 2> f64s{{
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000001234)}};

    require(fastdb::payload::layout::load_native_fixed_scalar_bits(
                fastdb::payload::spec::TypeKind::u16,
                reinterpret_cast<const std::uint8_t*>(u16s.data())) ==
            UINT64_C(0x1234));
    require(fastdb::payload::layout::load_native_fixed_scalar_bits(
                fastdb::payload::spec::TypeKind::u32,
                reinterpret_cast<const std::uint8_t*>(u32s.data())) ==
            UINT64_C(0x12345678));

    const auto push_run = [&](std::uint32_t entry, const void* data,
                              std::uint64_t byte_length,
                              std::uint64_t count) -> int {
        require(bulk.value().begin_entry(entry, count).has_value());
        const FixedRun run{static_cast<const std::uint8_t*>(data), byte_length,
                           count, UINT64_C(0), nullptr, UINT64_C(0),
                           UINT64_C(0)};
        require(bulk.value().push_fixed_run(run).has_value());
        return EXIT_SUCCESS;
    };
    require(push_run(UINT32_C(0), booleans.data(), sizeof(booleans),
                     booleans.size()) == EXIT_SUCCESS);
    require(push_run(UINT32_C(1), u8s.data(), sizeof(u8s), u8s.size()) ==
            EXIT_SUCCESS);
    require(push_run(UINT32_C(2), u16s.data(), sizeof(u16s), u16s.size()) ==
            EXIT_SUCCESS);
    require(push_run(UINT32_C(3), u32s.data(), sizeof(u32s), u32s.size()) ==
            EXIT_SUCCESS);
    require(push_run(UINT32_C(4), i32s.data(), sizeof(i32s), i32s.size()) ==
            EXIT_SUCCESS);
    require(push_run(UINT32_C(5), u8ns.data(), sizeof(u8ns), u8ns.size()) ==
            EXIT_SUCCESS);
    require(push_run(UINT32_C(6), u16ns.data(), sizeof(u16ns), u16ns.size()) ==
            EXIT_SUCCESS);
    require(push_run(UINT32_C(7), f32s.data(), sizeof(f32s), f32s.size()) ==
            EXIT_SUCCESS);
    require(push_run(UINT32_C(8), f64s.data(), sizeof(f64s), f64s.size()) ==
            EXIT_SUCCESS);

    require(individual.value().begin_entry(UINT32_C(0), booleans.size())
                .has_value());
    for (const auto value : booleans) {
        require(individual.value().push_bool(value).has_value());
    }
    require(individual.value().begin_entry(UINT32_C(1), u8s.size())
                .has_value());
    for (const auto value : u8s) {
        require(individual.value().push_u8(value).has_value());
    }
    require(individual.value().begin_entry(UINT32_C(2), u16s.size())
                .has_value());
    for (const auto value : u16s) {
        require(individual.value().push_u16(value).has_value());
    }
    require(individual.value().begin_entry(UINT32_C(3), u32s.size())
                .has_value());
    for (const auto value : u32s) {
        require(individual.value().push_u32(value).has_value());
    }
    require(individual.value().begin_entry(UINT32_C(4), i32s.size())
                .has_value());
    for (const auto value : i32s) {
        require(individual.value().push_i32(value).has_value());
    }
    require(individual.value().begin_entry(UINT32_C(5), u8ns.size())
                .has_value());
    for (const auto bits : u8ns) {
        require(individual.value().push_u8n_bits(bits).has_value());
    }
    require(individual.value().begin_entry(UINT32_C(6), u16ns.size())
                .has_value());
    for (const auto bits : u16ns) {
        require(individual.value().push_u16n_bits(bits).has_value());
    }
    require(individual.value().begin_entry(UINT32_C(7), f32s.size())
                .has_value());
    for (const auto bits : f32s) {
        require(individual.value().push_f32_bits(bits).has_value());
    }
    require(individual.value().begin_entry(UINT32_C(8), f64s.size())
                .has_value());
    for (const auto bits : f64s) {
        require(individual.value().push_f64_bits(bits).has_value());
    }

    auto bulk_payload = bulk.value().freeze();
    auto individual_payload = individual.value().freeze();
    require(bulk_payload.has_value() && individual_payload.has_value());
    require(bulk_payload.value().nodes().size() ==
            individual_payload.value().nodes().size());
    for (std::size_t index = 0U; index < bulk_payload.value().nodes().size();
         ++index) {
        require(std::memcmp(&bulk_payload.value().nodes()[index],
                            &individual_payload.value().nodes()[index],
                            sizeof(ValueNode)) == 0);
    }
    for (const auto [entry, expected] :
         {std::pair<std::uint32_t, const std::array<std::uint64_t, 4>*>{
              UINT32_C(5), &u8ns},
          std::pair<std::uint32_t, const std::array<std::uint64_t, 4>*>{
              UINT32_C(6), &u16ns}}) {
        const ValueNode& root = bulk_payload.value().nodes()[
            bulk_payload.value().entry_roots()[entry]];
        auto child = root.first_child;
        for (const std::uint64_t bits : *expected) {
            const ValueNode& node = bulk_payload.value().nodes()[child];
            require(node.scalar_bits_or_offset == bits);
            child = node.next_sibling;
        }
        require(child == fastdb::payload::build::invalid_node_index);
    }
    return EXIT_SUCCESS;
}

int test_normalized_boundaries_are_transactional() {
    auto compiled = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"u8one","cardinality":"one","type":{"kind":"u8n","min":-1,"max":1}},{"id":"u16one","cardinality":"one","type":{"kind":"u16n","min":0,"max":10}},{"id":"u8run","cardinality":"many","type":{"kind":"u8n","min":-1,"max":1}},{"id":"u16run","cardinality":"many","type":{"kind":"u16n","min":0,"max":10}}],"components":[]})");
    require(compiled.has_value());
    auto created = PayloadBuilder::create(std::move(compiled).value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();

    const std::uint64_t u8_below =
        f64_bits(std::nextafter(-1.0, -std::numeric_limits<double>::infinity()));
    const std::uint64_t u8_above =
        f64_bits(std::nextafter(1.0, std::numeric_limits<double>::infinity()));
    const std::uint64_t u16_below =
        f64_bits(-std::numeric_limits<double>::denorm_min());
    const std::uint64_t u16_above =
        f64_bits(std::nextafter(10.0, std::numeric_limits<double>::infinity()));

    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(exact_error(
        builder.push_u8n_bits(u8_below), FDB_PAYLOAD_E_OUT_OF_RANGE,
        "/entries/u8one",
        R"({"kind":"u8n","reason":"outside_declared_range"})"));
    require(exact_error(
        builder.push_u8n_bits(u8_above), FDB_PAYLOAD_E_OUT_OF_RANGE,
        "/entries/u8one",
        R"({"kind":"u8n","reason":"outside_declared_range"})"));
    require(builder.push_u8n_bits(f64_bits(-1.0)).has_value());

    require(builder.begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(exact_error(
        builder.push_u16n_bits(u16_below), FDB_PAYLOAD_E_OUT_OF_RANGE,
        "/entries/u16one",
        R"({"kind":"u16n","reason":"outside_declared_range"})"));
    require(exact_error(
        builder.push_u16n_bits(u16_above), FDB_PAYLOAD_E_OUT_OF_RANGE,
        "/entries/u16one",
        R"({"kind":"u16n","reason":"outside_declared_range"})"));
    require(builder.push_u16n_bits(f64_bits(10.0)).has_value());

    const std::array<std::uint64_t, 2> bad_u8{{f64_bits(-1.0), u8_below}};
    const std::array<std::uint64_t, 2> good_u8{{f64_bits(-1.0),
                                                f64_bits(1.0)}};
    require(builder.begin_entry(UINT32_C(2), bad_u8.size()).has_value());
    FixedRun run_u8{reinterpret_cast<const std::uint8_t*>(bad_u8.data()),
                    sizeof(bad_u8), bad_u8.size(), UINT64_C(0), nullptr,
                    UINT64_C(0), UINT64_C(0)};
    require(exact_error(
        builder.push_fixed_run(run_u8), FDB_PAYLOAD_E_OUT_OF_RANGE,
        "/entries/u8run/1",
        R"({"kind":"u8n","reason":"outside_declared_range"})"));
    run_u8.data = reinterpret_cast<const std::uint8_t*>(good_u8.data());
    require(builder.push_fixed_run(run_u8).has_value());

    const std::array<std::uint64_t, 2> bad_u16{{f64_bits(0.0), u16_above}};
    const std::array<std::uint64_t, 2> good_u16{{f64_bits(0.0),
                                                 f64_bits(10.0)}};
    require(builder.begin_entry(UINT32_C(3), bad_u16.size()).has_value());
    FixedRun run_u16{reinterpret_cast<const std::uint8_t*>(bad_u16.data()),
                     sizeof(bad_u16), bad_u16.size(), UINT64_C(0), nullptr,
                     UINT64_C(0), UINT64_C(0)};
    require(exact_error(
        builder.push_fixed_run(run_u16), FDB_PAYLOAD_E_OUT_OF_RANGE,
        "/entries/u16run/1",
        R"({"kind":"u16n","reason":"outside_declared_range"})"));
    run_u16.data = reinterpret_cast<const std::uint8_t*>(good_u16.data());
    require(builder.push_fixed_run(run_u16).has_value());

    auto payload = builder.freeze();
    require(payload.has_value());
    for (std::uint32_t entry = UINT32_C(0); entry < UINT32_C(4); ++entry) {
        const ValueNode& root = payload.value().nodes()[
            payload.value().entry_roots()[entry]];
        require(root.child_count == (entry < UINT32_C(2) ? UINT64_C(1)
                                                         : UINT64_C(2)));
    }
    const ValueNode& u8_run_root = payload.value().nodes()[
        payload.value().entry_roots()[2]];
    const ValueNode& u8_first =
        payload.value().nodes()[u8_run_root.first_child];
    const ValueNode& u8_second =
        payload.value().nodes()[u8_first.next_sibling];
    require(u8_first.scalar_bits_or_offset == good_u8[0]);
    require(u8_second.scalar_bits_or_offset == good_u8[1]);
    const ValueNode& u16_run_root = payload.value().nodes()[
        payload.value().entry_roots()[3]];
    const ValueNode& u16_first =
        payload.value().nodes()[u16_run_root.first_child];
    const ValueNode& u16_second =
        payload.value().nodes()[u16_first.next_sibling];
    require(u16_first.scalar_bits_or_offset == good_u16[0]);
    require(u16_second.scalar_bits_or_offset == good_u16[1]);
    return EXIT_SUCCESS;
}

int test_input_spans_fail_before_pointer_use() {
    const std::uint64_t addressable =
        fastdb::payload::layout::max_addressable_input_span_bytes();
    require(fastdb::payload::layout::input_span_is_addressable(addressable));
    if (addressable != UINT64_MAX) {
        require(!fastdb::payload::layout::input_span_is_addressable(
            addressable + UINT64_C(1)));
    }

    auto fixed_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"many","type":{"kind":"u8"}}],"components":[]})");
    require(fixed_spec.has_value());
    auto fixed = PayloadBuilder::create(std::move(fixed_spec).value());
    require(fixed.has_value());
    require(fixed.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    const std::uint8_t byte = UINT8_C(7);
    FixedRun unaddressable{
        &byte, addressable + UINT64_C(1), UINT64_C(2), addressable, nullptr,
        UINT64_C(0), UINT64_C(0)};
    require(exact_error(
        fixed.value().push_fixed_run(unaddressable),
        FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, "/entries/v/0",
        R"({"reason":"fixed_run_data_span_overflow"})"));

    BuilderLimits unlimited = default_builder_limits();
    unlimited.max_text_bytes = UINT64_MAX;
    unlimited.max_opaque_bytes = UINT64_MAX;
    unlimited.max_total_builder_bytes = UINT64_MAX;
    auto bytes_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"one","type":{"kind":"bytes"}}],"components":[]})");
    require(bytes_spec.has_value());
    auto bytes = PayloadBuilder::create(std::move(bytes_spec).value(), unlimited);
    require(bytes.has_value());
    require(bytes.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(exact_error(
        bytes.value().push_bytes(&byte, addressable + UINT64_C(1)),
        FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, "/entries/v",
        R"({"reason":"input_span_length_overflow"})"));

    auto wstr_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"one","type":{"kind":"wstr"}}],"components":[]})");
    require(wstr_spec.has_value());
    auto wstr = PayloadBuilder::create(std::move(wstr_spec).value(), unlimited);
    require(wstr.has_value());
    require(wstr.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    const std::uint16_t invalid_lead = UINT16_C(0xd800);
    const std::uint64_t unaddressable_units =
        addressable / UINT64_C(2) + UINT64_C(1);
    require(exact_error(
        wstr.value().push_wstr(&invalid_lead, unaddressable_units),
        FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, "/entries/v",
        R"({"reason":"input_span_length_overflow"})"));

    auto str_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"one","type":{"kind":"str"}}],"components":[]})");
    require(str_spec.has_value());
    auto str = PayloadBuilder::create(std::move(str_spec).value(), unlimited);
    require(str.has_value());
    require(str.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    const char invalid_utf8 = static_cast<char>(0xff);
    require(exact_error(
        str.value().push_str(std::string_view(
            &invalid_utf8, static_cast<std::size_t>(addressable +
                                                    UINT64_C(1)))),
        FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, "/entries/v",
        R"({"reason":"input_span_length_overflow"})"));
    return EXIT_SUCCESS;
}

int test_exact_failures_and_retryable_state() {
    auto compiled = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"rows","cardinality":"many","type":{"kind":"component","id":"Row"}},{"id":"text","cardinality":"one","type":{"kind":"str"}},{"id":"wide","cardinality":"one","type":{"kind":"wstr"}},{"id":"n","cardinality":"one","type":{"kind":"u8n","min":0,"max":1}}],"components":[{"id":"Row","kind":"record","fields":[{"id":"x","type":{"kind":"u8"}},{"id":"optional","type":{"kind":"u16","nullable":true}}]}]})");
    require(compiled.has_value());
    auto created = PayloadBuilder::create(std::move(compiled).value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();

    auto bad_count = builder.begin_entry(UINT32_C(1), UINT64_C(0));
    require(exact_error(bad_count, FDB_PAYLOAD_E_OUT_OF_RANGE,
                        "/entries/text",
                        R"({"actual":"0","expected":"1","reason":"one_entry_value_count"})"));
    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(builder.begin_component().has_value());
    auto wrong = builder.push_u16(UINT16_C(1));
    require(exact_error(wrong, FDB_PAYLOAD_E_TYPE_MISMATCH,
                        "/entries/rows/0/x",
                        R"({"actual_operation":"push_u16","expected_kind":"u8"})"));
    require(builder.push_u8(UINT8_C(3)).has_value());
    require(builder.push_null().has_value());

    require(builder.begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    const std::string invalid_utf8("\xc0\x80", 2U);
    auto bad_utf8 = builder.push_str(invalid_utf8);
    require(exact_error(bad_utf8, FDB_PAYLOAD_E_INVALID_TEXT_ENCODING,
                        "/entries/text",
                        R"({"encoding":"utf-8","reason":"invalid_sequence"})"));
    const std::string large_text(4096U, 'x');
    allocation_failure::fail_next = true;
    auto allocation = builder.push_str(large_text);
    require(!allocation.has_value());
    require(allocation.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(builder.push_str("ok").has_value());

    require(builder.begin_entry(UINT32_C(2), UINT64_C(1)).has_value());
    const std::array<std::uint16_t, 1> surrogate{{UINT16_C(0xd800)}};
    auto bad_utf16 = builder.push_wstr(surrogate.data(), surrogate.size());
    require(exact_error(bad_utf16, FDB_PAYLOAD_E_INVALID_TEXT_ENCODING,
                        "/entries/wide",
                        R"({"encoding":"utf-16","reason":"unpaired_surrogate"})"));
    const std::array<std::uint16_t, 1> wide{{UINT16_C(0x0061)}};
    require(builder.push_wstr(wide.data(), wide.size()).has_value());

    require(builder.begin_entry(UINT32_C(3), UINT64_C(1)).has_value());
    auto non_finite = builder.push_u8n_bits(UINT64_C(0x7ff0000000000000));
    require(exact_error(non_finite, FDB_PAYLOAD_E_OUT_OF_RANGE,
                        "/entries/n",
                        R"({"kind":"u8n","reason":"non_finite"})"));
    require(builder.push_u8n_bits(f64_bits(0.5)).has_value());

    auto duplicate = builder.begin_entry(UINT32_C(1), UINT64_C(1));
    require(exact_error(duplicate, FDB_PAYLOAD_E_BUILDER_STATE,
                        "/entries/text",
                        R"({"reason":"duplicate_entry"})"));
    auto frozen = builder.freeze();
    require(frozen.has_value());
    auto after_freeze = builder.push_u8(UINT8_C(1));
    require(exact_error(after_freeze, FDB_PAYLOAD_E_BUILDER_STATE, "",
                        R"({"reason":"builder_frozen"})"));
    auto refreeze = builder.freeze();
    require(exact_error(refreeze, FDB_PAYLOAD_E_BUILDER_STATE, "",
                        R"({"reason":"builder_frozen"})"));
    return EXIT_SUCCESS;
}

int test_exact_uint64_error_details() {
    const std::string one_u8 =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"e","cardinality":"one","type":{"kind":"u8"}}],"components":[]})";
    constexpr std::uint64_t below_two53 = UINT64_C(9007199254740991);
    constexpr std::uint64_t above_two53 = UINT64_C(9007199254740993);
    for (const std::uint64_t count :
         {below_two53, above_two53, UINT64_MAX}) {
        auto spec = compile(one_u8);
        require(spec.has_value());
        auto builder = PayloadBuilder::create(std::move(spec).value());
        require(builder.has_value());
        const auto result = builder.value().begin_entry(UINT32_C(0), count);
        const std::string expected =
            std::string(R"({"actual":")") + std::to_string(count) +
            R"(","expected":"1","reason":"one_entry_value_count"})";
        require(exact_error(result, FDB_PAYLOAD_E_OUT_OF_RANGE,
                            "/entries/e", expected));
    }

    const std::string one_list =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"items","cardinality":"one","type":{"kind":"list","items":{"kind":"u8"}}}],"components":[]})";
    for (const auto [actual, limit] :
         {std::pair<std::uint64_t, std::uint64_t>{above_two53, below_two53},
          std::pair<std::uint64_t, std::uint64_t>{UINT64_MAX,
                                                  UINT64_MAX - UINT64_C(1)}}) {
        BuilderLimits limits = default_builder_limits();
        limits.max_list_elements = limit;
        auto spec = compile(one_list);
        require(spec.has_value());
        auto builder =
            PayloadBuilder::create(std::move(spec).value(), limits);
        require(builder.has_value());
        require(builder.value().begin_entry(UINT32_C(0), UINT64_C(1))
                    .has_value());
        const auto result = builder.value().begin_list(actual);
        const std::string expected =
            std::string(R"({"actual":")") + std::to_string(actual) +
            R"(","limit":")" + std::to_string(limit) +
            R"(","resource":"list_elements"})";
        require(exact_error(result, FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT,
                            "/entries/items", expected));
    }

    auto span_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"v","cardinality":"many","type":{"kind":"u32","nullable":true}}],"components":[]})");
    require(span_spec.has_value());
    auto span_builder = PayloadBuilder::create(std::move(span_spec).value());
    require(span_builder.has_value());
    require(span_builder.value().begin_entry(UINT32_C(0), UINT64_C(2))
                .has_value());
    const std::uint32_t word = UINT32_C(7);
    FixedRun large_short{
        reinterpret_cast<const std::uint8_t*>(&word), below_two53,
        UINT64_C(2), above_two53, nullptr, UINT64_C(0), UINT64_C(0)};
    require(exact_error(
        span_builder.value().push_fixed_run(large_short),
        FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS, "/entries/v/0",
        R"({"available":"9007199254740991","reason":"fixed_run_data_too_short","required":"9007199254740997"})"));

    FixedRun maximum_required{
        reinterpret_cast<const std::uint8_t*>(&word), above_two53,
        UINT64_C(2), UINT64_MAX - UINT64_C(4), nullptr, UINT64_C(0),
        UINT64_C(0)};
    require(exact_error(
        span_builder.value().push_fixed_run(maximum_required),
        FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS, "/entries/v/0",
        R"({"available":"9007199254740993","reason":"fixed_run_data_too_short","required":"18446744073709551615"})"));

    const std::uint8_t bitmap = UINT8_C(0xff);
    constexpr std::uint64_t required_bitmap = above_two53;
    FixedRun large_bitmap{
        reinterpret_cast<const std::uint8_t*>(&word), sizeof(word),
        UINT64_C(1), UINT64_C(0), &bitmap, below_two53,
        required_bitmap * UINT64_C(8) - UINT64_C(1)};
    require(exact_error(
        span_builder.value().push_fixed_run(large_bitmap),
        FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS, "/entries/v/0",
        R"({"available":"9007199254740991","reason":"fixed_run_validity_too_short","required":"9007199254740993"})"));
    return EXIT_SUCCESS;
}

int test_missing_partial_and_logical_accounting() {
    auto missing_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"a","cardinality":"one","type":{"kind":"u8"}},{"id":"b","cardinality":"one","type":{"kind":"list","items":{"kind":"u8"}}}],"components":[]})");
    require(missing_spec.has_value());
    auto missing_builder = PayloadBuilder::create(std::move(missing_spec).value());
    require(missing_builder.has_value());
    auto missing = missing_builder.value().freeze();
    require(exact_error(missing, FDB_PAYLOAD_E_MISSING_ENTRY,
                        "/entries/a", R"({"reason":"entry_not_authored"})"));
    require(missing_builder.value().begin_entry(UINT32_C(1), UINT64_C(1)).has_value());
    require(missing_builder.value().begin_list(UINT64_C(2)).has_value());
    require(missing_builder.value().push_u8(UINT8_C(1)).has_value());
    auto partial = missing_builder.value().freeze();
    require(exact_error(partial, FDB_PAYLOAD_E_BUILDER_STATE,
                        "/entries/b/1",
                        R"({"reason":"incomplete_list"})"));
    require(missing_builder.value().push_u8(UINT8_C(2)).has_value());
    require(missing_builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(missing_builder.value().push_u8(UINT8_C(3)).has_value());
    require(missing_builder.value().freeze().has_value());

    auto field_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"row","cardinality":"one","type":{"kind":"component","id":"Row"}}],"components":[{"id":"Row","kind":"record","fields":[{"id":"x","type":{"kind":"u8"}},{"id":"y","type":{"kind":"u8"}}]}]})");
    require(field_spec.has_value());
    auto field_builder = PayloadBuilder::create(std::move(field_spec).value());
    require(field_builder.has_value());
    require(field_builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(field_builder.value().begin_component().has_value());
    require(field_builder.value().push_u8(UINT8_C(1)).has_value());
    auto missing_field = field_builder.value().freeze();
    require(exact_error(missing_field, FDB_PAYLOAD_E_MISSING_FIELD,
                        "/entries/row/y",
                        R"({"reason":"field_not_authored"})"));

    const std::string one_u8 =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"e","cardinality":"one","type":{"kind":"u8"}}],"components":[]})";
    BuilderLimits limits = default_builder_limits();
    limits.max_total_builder_bytes = UINT64_C(135);
    auto low_spec = compile(one_u8);
    require(low_spec.has_value());
    auto low = PayloadBuilder::create(std::move(low_spec).value(), limits);
    require(low.has_value());
    auto limited = low.value().begin_entry(UINT32_C(0), UINT64_C(1));
    require(exact_error(limited, FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT,
                        "/entries/e",
                        R"({"actual":"136","limit":"135","resource":"total_builder_bytes"})"));

    limits.max_total_builder_bytes = UINT64_C(136);
    auto exact_spec = compile(one_u8);
    require(exact_spec.has_value());
    auto exact = PayloadBuilder::create(std::move(exact_spec).value(), limits);
    require(exact.has_value());
    require(exact.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(exact.value().push_u8(UINT8_C(9)).has_value());
    require(exact.value().freeze().has_value());

    const std::string one_text =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"e","cardinality":"one","type":{"kind":"str"}}],"components":[]})";
    limits = default_builder_limits();
    limits.max_total_builder_bytes = UINT64_C(138);
    auto text_spec = compile(one_text);
    require(text_spec.has_value());
    auto text_builder = PayloadBuilder::create(std::move(text_spec).value(), limits);
    require(text_builder.has_value());
    require(text_builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    auto text_limit = text_builder.value().push_str("abc");
    require(exact_error(text_limit, FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT,
                        "/entries/e",
                        R"({"actual":"139","limit":"138","resource":"total_builder_bytes"})"));

    BuilderLimits root_limits = default_builder_limits();
    root_limits.max_total_builder_bytes = UINT64_C(7);
    auto root_spec = compile(one_u8);
    require(root_spec.has_value());
    auto root_limited =
        PayloadBuilder::create(std::move(root_spec).value(), root_limits);
    require(exact_error(root_limited, FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT,
                        "/entries",
                        R"({"actual":"8","limit":"7","resource":"total_builder_bytes"})"));

    BuilderLimits node_limits = default_builder_limits();
    node_limits.max_value_nodes = UINT64_C(1);
    auto node_spec = compile(one_u8);
    require(node_spec.has_value());
    auto node_builder =
        PayloadBuilder::create(std::move(node_spec).value(), node_limits);
    require(node_builder.has_value());
    require(node_builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    auto node_limit = node_builder.value().push_u8(UINT8_C(1));
    require(node_limit.error().code() == FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);
    node_limits.max_value_nodes = UINT64_C(2);
    auto node_ok_spec = compile(one_u8);
    require(node_ok_spec.has_value());
    auto node_ok =
        PayloadBuilder::create(std::move(node_ok_spec).value(), node_limits);
    require(node_ok.has_value());
    require(node_ok.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(node_ok.value().push_u8(UINT8_C(1)).has_value());

    BuilderLimits text_limits = default_builder_limits();
    text_limits.max_text_bytes = UINT64_C(2);
    auto separate_text_spec = compile(one_text);
    require(separate_text_spec.has_value());
    auto separate_text = PayloadBuilder::create(
        std::move(separate_text_spec).value(), text_limits);
    require(separate_text.has_value());
    require(separate_text.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(separate_text.value().push_str("abc").error().code() ==
            FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);
    text_limits.max_text_bytes = UINT64_C(3);
    auto text_ok_spec = compile(one_text);
    require(text_ok_spec.has_value());
    auto text_ok =
        PayloadBuilder::create(std::move(text_ok_spec).value(), text_limits);
    require(text_ok.has_value());
    require(text_ok.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(text_ok.value().push_str("abc").has_value());

    const std::string one_bytes =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"e","cardinality":"one","type":{"kind":"bytes"}}],"components":[]})";
    BuilderLimits opaque_limits = default_builder_limits();
    opaque_limits.max_opaque_bytes = UINT64_C(2);
    auto opaque_spec = compile(one_bytes);
    require(opaque_spec.has_value());
    auto opaque_builder = PayloadBuilder::create(
        std::move(opaque_spec).value(), opaque_limits);
    require(opaque_builder.has_value());
    require(opaque_builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    const std::array<std::uint8_t, 3> opaque{{UINT8_C(1), UINT8_C(2),
                                              UINT8_C(3)}};
    require(opaque_builder.value().push_bytes(opaque.data(), opaque.size())
                .error()
                .code() == FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);
    opaque_limits.max_opaque_bytes = UINT64_C(3);
    auto opaque_ok_spec = compile(one_bytes);
    require(opaque_ok_spec.has_value());
    auto opaque_ok = PayloadBuilder::create(std::move(opaque_ok_spec).value(),
                                             opaque_limits);
    require(opaque_ok.has_value());
    require(opaque_ok.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(opaque_ok.value().push_bytes(opaque.data(), opaque.size()).has_value());

    const std::string nested_lists =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"e","cardinality":"one","type":{"kind":"list","items":{"kind":"list","items":{"kind":"u8"}}}}],"components":[]})";
    BuilderLimits list_limits = default_builder_limits();
    list_limits.max_list_elements = UINT64_C(1);
    list_limits.max_nesting_depth = UINT64_C(2);
    auto list_spec = compile(nested_lists);
    require(list_spec.has_value());
    auto list_builder =
        PayloadBuilder::create(std::move(list_spec).value(), list_limits);
    require(list_builder.has_value());
    require(list_builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(list_builder.value().begin_list(UINT64_C(1)).has_value());
    require(list_builder.value().begin_list(UINT64_C(1)).error().code() ==
            FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);
    list_limits.max_list_elements = UINT64_C(2);
    auto list_ok_spec = compile(nested_lists);
    require(list_ok_spec.has_value());
    auto list_ok =
        PayloadBuilder::create(std::move(list_ok_spec).value(), list_limits);
    require(list_ok.has_value());
    require(list_ok.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(list_ok.value().begin_list(UINT64_C(1)).has_value());
    require(list_ok.value().begin_list(UINT64_C(1)).has_value());
    require(list_ok.value().push_u8(UINT8_C(1)).has_value());

    BuilderLimits depth_limits = default_builder_limits();
    depth_limits.max_nesting_depth = UINT64_C(1);
    auto depth_spec = compile(nested_lists);
    require(depth_spec.has_value());
    auto depth_builder =
        PayloadBuilder::create(std::move(depth_spec).value(), depth_limits);
    require(depth_builder.has_value());
    require(depth_builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(depth_builder.value().begin_list(UINT64_C(1)).has_value());
    require(depth_builder.value().begin_list(UINT64_C(1)).error().code() ==
            FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);
    depth_limits.max_nesting_depth = UINT64_C(2);
    auto depth_ok_spec = compile(nested_lists);
    require(depth_ok_spec.has_value());
    auto depth_ok =
        PayloadBuilder::create(std::move(depth_ok_spec).value(), depth_limits);
    require(depth_ok.has_value());
    require(depth_ok.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(depth_ok.value().begin_list(UINT64_C(1)).has_value());
    require(depth_ok.value().begin_list(UINT64_C(1)).has_value());
    require(depth_ok.value().push_u8(UINT8_C(1)).has_value());

    const std::string empty_containers =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"list","cardinality":"one","type":{"kind":"list","items":{"kind":"u8"}}},{"id":"component","cardinality":"one","type":{"kind":"component","id":"Empty"}}],"components":[{"id":"Empty","kind":"record","fields":[]}]})";
    BuilderLimits empty_depth_limits = default_builder_limits();
    empty_depth_limits.max_nesting_depth = UINT64_C(0);
    auto empty_container_spec = compile(empty_containers);
    require(empty_container_spec.has_value());
    auto empty_container_builder = PayloadBuilder::create(
        std::move(empty_container_spec).value(), empty_depth_limits);
    require(empty_container_builder.has_value());
    require(empty_container_builder.value()
                .begin_entry(UINT32_C(0), UINT64_C(1))
                .has_value());
    require(empty_container_builder.value().begin_list(UINT64_C(0))
                .error()
                .code() == FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);

    auto empty_component_spec = compile(empty_containers);
    require(empty_component_spec.has_value());
    auto empty_component_builder = PayloadBuilder::create(
        std::move(empty_component_spec).value(), empty_depth_limits);
    require(empty_component_builder.has_value());
    require(empty_component_builder.value()
                .begin_entry(UINT32_C(1), UINT64_C(1))
                .has_value());
    require(empty_component_builder.value().begin_component().error().code() ==
            FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT);
    return EXIT_SUCCESS;
}

int test_null_empty_distinctions_and_high_fanout_teardown() {
    auto compiled = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"strings","cardinality":"many","type":{"kind":"str","nullable":true}},{"id":"blobs","cardinality":"many","type":{"kind":"bytes","nullable":true}},{"id":"lists","cardinality":"many","type":{"kind":"list","nullable":true,"items":{"kind":"u8"}}}],"components":[]})");
    require(compiled.has_value());
    auto created = PayloadBuilder::create(std::move(compiled).value());
    require(created.has_value());
    require(created.value().begin_entry(UINT32_C(0), UINT64_C(2)).has_value());
    require(created.value().push_null().has_value());
    require(created.value().push_str("").has_value());
    require(created.value().begin_entry(UINT32_C(1), UINT64_C(2)).has_value());
    require(created.value().push_null().has_value());
    require(created.value().push_bytes(nullptr, UINT64_C(0)).has_value());
    require(created.value().begin_entry(UINT32_C(2), UINT64_C(2)).has_value());
    require(created.value().push_null().has_value());
    require(created.value().begin_list(UINT64_C(0)).has_value());
    auto frozen = created.value().freeze();
    require(frozen.has_value());
    std::size_t null_count = 0U;
    std::size_t empty_count = 0U;
    for (const ValueNode& node : frozen.value().nodes()) {
        if (node.tag == ValueTag::null_value) {
            ++null_count;
        }
        if ((node.tag == ValueTag::str || node.tag == ValueTag::bytes ||
             node.tag == ValueTag::list) &&
            node.byte_length == UINT64_C(0) &&
            node.child_count == UINT64_C(0)) {
            ++empty_count;
        }
    }
    require(null_count == 3U);
    require(empty_count == 3U);

    constexpr std::uint64_t rows = UINT64_C(20000);
    auto fanout_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"rows","cardinality":"many","type":{"kind":"component","id":"Empty"}}],"components":[{"id":"Empty","kind":"record","fields":[]}]})");
    require(fanout_spec.has_value());
    auto fanout = PayloadBuilder::create(std::move(fanout_spec).value());
    require(fanout.has_value());
    require(fanout.value().begin_entry(UINT32_C(0), rows).has_value());
    for (std::uint64_t row = UINT64_C(0); row < rows; ++row) {
        require(fanout.value().begin_component().has_value());
    }
    auto fanout_payload = fanout.value().freeze();
    require(fanout_payload.has_value());
    require(fanout_payload.value().nodes().size() ==
            static_cast<std::size_t>(rows + UINT64_C(1)));
    return EXIT_SUCCESS;
}

int test_deep_iterative_builder_and_runtime_unavailable() {
    constexpr std::uint32_t depth = UINT32_C(20000);
    std::string source = deep_list_spec(depth);
    auto compiled = compile(source, depth + UINT32_C(16));
    require(compiled.has_value());
    BuilderLimits limits = default_builder_limits();
    limits.max_nesting_depth = depth;
    limits.max_total_builder_bytes = UINT64_C(8) * UINT64_C(1024) *
                                     UINT64_C(1024);
    auto created = PayloadBuilder::create(std::move(compiled).value(), limits);
    require(created.has_value());
    require(created.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    for (std::uint32_t index = UINT32_C(0); index < depth; ++index) {
        require(created.value().begin_list(UINT64_C(1)).has_value());
    }
    require(created.value().push_u8(UINT8_C(42)).has_value());
    auto frozen = created.value().freeze();
    require(frozen.has_value());
    require(frozen.value().nodes().size() ==
            static_cast<std::size_t>(depth) + 2U);

    auto graph_spec = compile(
        R"({"schema":"fastdb.payload.v1","profile":"object_graph.v1","entries":[],"components":[]})");
    require(graph_spec.has_value());
    auto graph = PayloadBuilder::create(std::move(graph_spec).value());
    require(exact_error(
        graph, FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE, "",
        R"({"profile":"object_graph.v1","reason":"runtime_slice_not_implemented"})"));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    const std::array<int (*)(), 14> tests{{
        test_empty_and_default_limits,
        test_shared_text_encoding_helpers,
        test_scalars_text_and_out_of_order_entries,
        test_record_batch_components_and_lists,
        test_fixed_runs_and_transactional_failures,
        test_fixed_run_limits_precede_scratch_allocation,
        test_all_fixed_kinds_match_individual_authoring,
        test_normalized_boundaries_are_transactional,
        test_input_spans_fail_before_pointer_use,
        test_exact_failures_and_retryable_state,
        test_exact_uint64_error_details,
        test_missing_partial_and_logical_accounting,
        test_null_empty_distinctions_and_high_fanout_teardown,
        test_deep_iterative_builder_and_runtime_unavailable,
    }};
    for (const auto test : tests) {
        const int status = test();
        if (status != EXIT_SUCCESS) {
            return status;
        }
    }
    return EXIT_SUCCESS;
}
