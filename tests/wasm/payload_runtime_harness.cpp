#include "payload/build/PayloadBuilder.hpp"
#include "payload/build/RecordEncoder.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/layout/NormalizedInteger.hpp"
#include "payload/layout/RecordLayout.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Open.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fastdb::payload::build::ByteSink;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::layout::RecordLayout;
using fastdb::payload::layout::RuntimeSchema;
using fastdb::payload::spec::CompiledSpec;

class VectorSink final : public ByteSink {
public:
    explicit VectorSink(std::uint64_t size)
        : bytes_(static_cast<std::size_t>(size), UINT8_C(0)) {}

    Result<void> write(std::uint64_t offset,
                       const std::uint8_t* data,
                       std::uint64_t size) override {
        if (offset != next_ || size > bytes_.size() - next_) {
            std::abort();
        }
        std::copy_n(data, static_cast<std::size_t>(size),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        next_ += size;
        return Result<void>::success();
    }

    std::vector<std::uint8_t> bytes_;
    std::uint64_t next_{UINT64_C(0)};
};

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::uint8_t nibble(char value) {
    return value <= '9' ? static_cast<std::uint8_t>(value - '0')
                        : static_cast<std::uint8_t>(value - 'a' + 10);
}

std::vector<std::uint8_t> read_hex(const std::string& path) {
    std::string text = read_file(path);
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    std::vector<std::uint8_t> bytes(text.size() / 2U, UINT8_C(0));
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            (nibble(text[index * 2U]) << 4U) |
            nibble(text[index * 2U + 1U]));
    }
    return bytes;
}

bool add_fixed_values(PayloadBuilder& builder) {
#define FASTDB_WASM_STEP(expression)                                         \
    do {                                                                     \
        if (!(expression).has_value()) {                                     \
            return false;                                                    \
        }                                                                    \
    } while (false)
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
    FASTDB_WASM_STEP(builder.push_bool(UINT8_C(1)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(1)));
    FASTDB_WASM_STEP(builder.push_u8(UINT8_C(0xab)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(2), UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_u16(UINT16_C(0x1234)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_u16(UINT16_MAX));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(3), UINT64_C(1)));
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(0x12345678)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(4), UINT64_C(1)));
    FASTDB_WASM_STEP(builder.push_i32(INT32_C(-2)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(5), UINT64_C(4)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x80000000)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x7f800000)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0xff800000)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x7fa12345)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(6), UINT64_C(4)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0x8000000000000000)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0xfff0000000000000)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0x7ff0000000000042)));
#undef FASTDB_WASM_STEP
    return true;
}

bool add_numeric_edge_values(PayloadBuilder& builder) {
#define FASTDB_WASM_STEP(expression)                                         \
    do {                                                                     \
        if (!(expression).has_value()) {                                     \
            return false;                                                    \
        }                                                                    \
    } while (false)
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_bool(UINT8_C(0)));
    FASTDB_WASM_STEP(builder.push_bool(UINT8_C(1)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_u8(UINT8_C(0)));
    FASTDB_WASM_STEP(builder.push_u8(UINT8_MAX));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(2), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_u16(UINT16_C(0)));
    FASTDB_WASM_STEP(builder.push_u16(UINT16_MAX));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(3), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(0)));
    FASTDB_WASM_STEP(builder.push_u32(UINT32_MAX));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(4), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_i32(INT32_MIN));
    FASTDB_WASM_STEP(builder.push_i32(INT32_MAX));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(5), UINT64_C(6)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x80000000)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x7f800000)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0xff800000)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x7fa12345)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0xffdabcde)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(6), UINT64_C(6)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0x8000000000000000)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0xfff0000000000000)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0x7ff0000000000042)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0xfff8000000001234)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(7), UINT64_C(6)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0x8000000000000000)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0x406fe00000000000)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0x3ff8000000000000)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(1)));
    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(8), UINT64_C(4)));
    FASTDB_WASM_STEP(builder.push_u16n_bits(UINT64_C(0xc0e0000000000000)));
    FASTDB_WASM_STEP(builder.push_u16n_bits(UINT64_C(0x40dfffc000000000)));
    FASTDB_WASM_STEP(builder.push_u16n_bits(UINT64_C(0xc0dfffe000000000)));
    FASTDB_WASM_STEP(builder.push_u16n_bits(UINT64_C(0xc0dfffa000000000)));
#undef FASTDB_WASM_STEP
    return true;
}

bool exact_golden(const std::string& root,
                  const std::string& name,
                  const std::vector<std::uint8_t>& bytes) {
    const auto golden = read_hex(root + "/valid/" + name + ".bin.hex");
    std::string expected_hash =
        read_file(root + "/valid/" + name + ".sha256");
    if (!expected_hash.empty() && expected_hash.back() == '\n') {
        expected_hash.pop_back();
    }
    return bytes == golden &&
           fastdb::payload::identity::sha256_lower_hex(
               fastdb::payload::identity::sha256(bytes.data(), bytes.size())) ==
               expected_hash;
}

struct NormalizedFacts final {
    double minimum;
    double maximum;
    std::uint32_t maximum_code;
    const char* kind;
};

NormalizedFacts normalized_facts(const fastdb::payload::spec::TypeNode& type) {
    if (type.kind == fastdb::payload::spec::TypeKind::u8n) {
        return NormalizedFacts{type.minimum, type.maximum, UINT32_C(255),
                               "u8n"};
    }
    if (type.kind == fastdb::payload::spec::TypeKind::u16n) {
        return NormalizedFacts{type.minimum, type.maximum, UINT32_C(65535),
                               "u16n"};
    }
    std::abort();
}

int run() {
    const std::string root = FASTDB_PAYLOAD_BINARY_FIXTURE_DIR;
    auto compiled = CompiledSpec::compile(
        read_file(root + "/spec/fixed-scalars.source.json"));
    if (!compiled.has_value()) {
        return 2;
    }
    auto created = PayloadBuilder::create(compiled.value());
    if (!created.has_value() || !add_fixed_values(created.value())) {
        return 3;
    }
    auto values = created.value().freeze();
    auto runtime = RuntimeSchema::compile(compiled.value());
    if (!values.has_value() || !runtime.has_value()) {
        return 4;
    }
    auto layout = RecordLayout::plan(runtime.value(), values.value());
    if (!layout.has_value()) {
        return 5;
    }
    VectorSink sink(layout.value().total_length());
    if (!fastdb::payload::build::encode_record(
             layout.value(), values.value(), sink)
             .has_value()) {
        return 6;
    }
    if (!exact_golden(root, "fixed-scalars", sink.bytes_)) {
        return 7;
    }
    auto opened = fastdb::payload::view::open_record(
        compiled.value(), sink.bytes_.data(), sink.bytes_.size());
    if (!opened.has_value() ||
        opened.value()
                .scalar(sink.bytes_.data(), sink.bytes_.size(), UINT32_C(6),
                        UINT64_C(3))
                .value()
                .bits != UINT64_C(0x7ff8000000000000)) {
        return 8;
    }

    auto numeric_spec = CompiledSpec::compile(
        read_file(root + "/spec/numeric-edges.source.json"));
    if (!numeric_spec.has_value()) {
        return 9;
    }
    const auto& numeric_entries = numeric_spec.value().resolved().entries();
    if (numeric_entries.size() != 9U ||
        numeric_entries[7].type.kind != fastdb::payload::spec::TypeKind::u8n ||
        numeric_entries[8].type.kind != fastdb::payload::spec::TypeKind::u16n) {
        return 10;
    }
    const NormalizedFacts u8n = normalized_facts(numeric_entries[7].type);
    const JsonPointer path = JsonPointer{}.append("wasm");
    const auto lower = fastdb::payload::layout::quantize_normalized(
        UINT64_C(0x3fe0000000000000), u8n.minimum, u8n.maximum,
        u8n.maximum_code, u8n.kind, path);
    const auto upper = fastdb::payload::layout::quantize_normalized(
        UINT64_C(0x3ff8000000000000), u8n.minimum, u8n.maximum,
        u8n.maximum_code, u8n.kind, path);
    if (!lower.has_value() || lower.value() != UINT32_C(0) ||
        !upper.has_value() || upper.value() != UINT32_C(2)) {
        return 11;
    }
    auto numeric_builder = PayloadBuilder::create(numeric_spec.value());
    if (!numeric_builder.has_value() ||
        !add_numeric_edge_values(numeric_builder.value())) {
        return 12;
    }
    auto numeric_values = numeric_builder.value().freeze();
    auto numeric_runtime = RuntimeSchema::compile(numeric_spec.value());
    if (!numeric_values.has_value() || !numeric_runtime.has_value()) {
        return 13;
    }
    auto numeric_layout =
        RecordLayout::plan(numeric_runtime.value(), numeric_values.value());
    if (!numeric_layout.has_value()) {
        return 14;
    }
    VectorSink numeric_sink(numeric_layout.value().total_length());
    if (!fastdb::payload::build::encode_record(
             numeric_layout.value(), numeric_values.value(), numeric_sink)
             .has_value()) {
        return 15;
    }
    if (!exact_golden(root, "numeric-edges", numeric_sink.bytes_)) {
        return 16;
    }
    auto numeric_opened = fastdb::payload::view::open_record(
        numeric_spec.value(), numeric_sink.bytes_.data(),
        numeric_sink.bytes_.size());
    if (!numeric_opened.has_value()) {
        return 17;
    }
    const std::array<std::uint64_t, 8> expected_bits{{
        UINT64_C(0x0000000000000000), UINT64_C(0x406fe00000000000),
        UINT64_C(0x0000000000000000), UINT64_C(0x4000000000000000),
        UINT64_C(0xc0e0000000000000), UINT64_C(0x40dfffc000000000),
        UINT64_C(0xc0e0000000000000), UINT64_C(0xc0dfff8000000000)}};
    const std::array<std::uint32_t, 8> entry_indexes{{
        UINT32_C(7), UINT32_C(7), UINT32_C(7), UINT32_C(7), UINT32_C(8),
        UINT32_C(8), UINT32_C(8), UINT32_C(8)}};
    const std::array<std::uint64_t, 8> value_indexes{{
        UINT64_C(0), UINT64_C(2), UINT64_C(3), UINT64_C(4), UINT64_C(0),
        UINT64_C(1), UINT64_C(2), UINT64_C(3)}};
    for (std::size_t index = 0U; index < expected_bits.size(); ++index) {
        auto observed = numeric_opened.value().scalar(
            numeric_sink.bytes_.data(), numeric_sink.bytes_.size(),
            entry_indexes[index], value_indexes[index]);
        if (!observed.has_value() ||
            observed.value().bits != expected_bits[index]) {
            return 18;
        }
    }
    return 0;
}

}  // namespace

int main() {
    const int status = run();
    if (status != 0) {
        std::cerr << "payload wasm runtime harness failed: " << status << '\n';
        return status;
    }
    std::cout << "payload wasm runtime harness passed\n";
    return 0;
}
