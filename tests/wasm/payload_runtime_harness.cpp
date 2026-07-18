#include "payload/build/PayloadBuilder.hpp"
#include "payload/build/RecordEncoder.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/layout/NormalizedInteger.hpp"
#include "payload/layout/RecordLayout.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Open.hpp"

#include <algorithm>
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

int run() {
    const JsonPointer path = JsonPointer{}.append("wasm");
    auto lower = fastdb::payload::layout::quantize_normalized(
        UINT64_C(0x3fe0000000000000), 0.0, 255.0, UINT32_C(255),
        "u8n", path);
    auto upper = fastdb::payload::layout::quantize_normalized(
        UINT64_C(0x3ff8000000000000), 0.0, 255.0, UINT32_C(255),
        "u8n", path);
    auto decoded = fastdb::payload::layout::dequantize_normalized(
        UINT32_C(128), -0x1p1000, 0x1p1000, UINT32_C(255), "u8n",
        path);
    if (!lower.has_value() || lower.value() != UINT32_C(0) ||
        !upper.has_value() || upper.value() != UINT32_C(2) ||
        !decoded.has_value() ||
        decoded.value() != UINT64_C(0x7df0101010101010)) {
        return 1;
    }

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
    const auto golden = read_hex(root + "/valid/fixed-scalars.bin.hex");
    std::string expected_hash =
        read_file(root + "/valid/fixed-scalars.sha256");
    if (!expected_hash.empty() && expected_hash.back() == '\n') {
        expected_hash.pop_back();
    }
    if (sink.bytes_ != golden ||
        fastdb::payload::identity::sha256_lower_hex(
            fastdb::payload::identity::sha256(sink.bytes_.data(),
                                               sink.bytes_.size())) !=
            expected_hash) {
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
