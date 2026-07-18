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

bool add_text_byte_values(PayloadBuilder& builder) {
#define FASTDB_WASM_STEP(expression)                                           \
    do {                                                                       \
        if (!(expression).has_value()) {                                       \
            return false;                                                      \
        }                                                                      \
    } while (false)
    const std::array<std::uint16_t, 1> wide_a{{UINT16_C(0x0041)}};
    const std::array<std::uint16_t, 2> wide_bmp_nul{
        {UINT16_C(0x4e2d), UINT16_C(0x0000)}};
    const std::array<std::uint16_t, 2> wide_supplementary{
        {UINT16_C(0xd83d), UINT16_C(0xde03)}};
    const std::array<std::uint16_t, 5> wide_nested{
        {UINT16_C(0x0041), UINT16_C(0x4e2d), UINT16_C(0xd83d), UINT16_C(0xde03),
         UINT16_C(0x0000)}};
    const std::array<std::uint8_t, 3> opaque{
        {UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x80)}};
    const std::array<std::uint8_t, 4> nested_opaque{
        {UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x80), UINT8_C(0x41)}};
    const std::array<std::uint8_t, 2> suffix{{UINT8_C(0x01), UINT8_C(0x02)}};

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(7)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_str(""));
    FASTDB_WASM_STEP(builder.push_str("ASCII"));
    FASTDB_WASM_STEP(builder.push_str(std::string_view{"\xe4\xb8\xad\0", 4U}));
    FASTDB_WASM_STEP(builder.push_str("repeat"));
    FASTDB_WASM_STEP(builder.push_str("repeat"));
    FASTDB_WASM_STEP(builder.push_str("tail"));

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(6)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_wstr(nullptr, UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_wstr(wide_a.data(), wide_a.size()));
    FASTDB_WASM_STEP(
        builder.push_wstr(wide_bmp_nul.data(), wide_bmp_nul.size()));
    FASTDB_WASM_STEP(builder.push_wstr(wide_supplementary.data(),
                                       wide_supplementary.size()));
    FASTDB_WASM_STEP(builder.push_wstr(wide_a.data(), wide_a.size()));

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(2), UINT64_C(6)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_bytes(nullptr, UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_bytes(opaque.data(), opaque.size()));
    FASTDB_WASM_STEP(builder.push_bytes(opaque.data(), opaque.size()));
    FASTDB_WASM_STEP(builder.push_bytes(
        reinterpret_cast<const std::uint8_t*>("A\0"), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_bytes(nullptr, UINT64_C(0)));

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(3), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_str("P"));
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_str("I"));
    FASTDB_WASM_STEP(builder.push_wstr(wide_nested.data(), wide_nested.size()));
    FASTDB_WASM_STEP(
        builder.push_bytes(nested_opaque.data(), nested_opaque.size()));
    FASTDB_WASM_STEP(builder.push_bytes(suffix.data(), suffix.size()));
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_str(""));
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_str("repeat"));
    FASTDB_WASM_STEP(builder.push_wstr(wide_a.data(), wide_a.size()));
    FASTDB_WASM_STEP(
        builder.push_bytes(nested_opaque.data(), nested_opaque.size()));
    FASTDB_WASM_STEP(builder.push_bytes(nullptr, UINT64_C(0)));
#undef FASTDB_WASM_STEP
    return true;
}

bool add_nested_list_values(PayloadBuilder& builder) {
#define FASTDB_WASM_STEP(expression)                                         \
    do {                                                                     \
        if (!(expression).has_value()) {                                     \
            return false;                                                    \
        }                                                                    \
    } while (false)
    const std::array<std::uint16_t, 3> wide{
        {UINT16_C(0x0041), UINT16_C(0xd83d), UINT16_C(0xde03)}};
    const std::array<std::uint8_t, 3> opaque{
        {UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x41)}};

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_str(""));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_str("alpha"));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));

    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_wstr(nullptr, UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_wstr(wide.data(), wide.size()));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_bytes(opaque.data(), opaque.size()));

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_u16(UINT16_C(7)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_u16(UINT16_C(9)));

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(2), UINT64_C(1)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_str(""));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_str("tail"));
#undef FASTDB_WASM_STEP
    return true;
}

bool add_component_list_composition_values(PayloadBuilder& builder) {
#define FASTDB_WASM_STEP(expression)                                         \
    do {                                                                     \
        if (!(expression).has_value()) {                                     \
            return false;                                                    \
        }                                                                    \
    } while (false)
    const std::array<std::uint16_t, 1> wide{{UINT16_C(0x4e2d)}};
    const std::array<std::uint8_t, 3> opaque{
        {UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x80)}};

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(2)));
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_bool(UINT8_C(1)));
    FASTDB_WASM_STEP(builder.push_u8(UINT8_C(8)));
    FASTDB_WASM_STEP(builder.push_u16(UINT16_C(16)));
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(32)));
    FASTDB_WASM_STEP(builder.push_i32(INT32_C(-32)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
    FASTDB_WASM_STEP(builder.push_u16n_bits(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x3f800000)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0x4000000000000000)));
    FASTDB_WASM_STEP(builder.push_str("component"));
    FASTDB_WASM_STEP(builder.push_wstr(wide.data(), wide.size()));
    FASTDB_WASM_STEP(builder.push_bytes(opaque.data(), opaque.size()));

    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_bool(UINT8_C(1)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_bool(UINT8_C(0)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_u8(UINT8_C(0)));
    FASTDB_WASM_STEP(builder.push_u8(UINT8_MAX));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_u16(UINT16_C(0)));
    FASTDB_WASM_STEP(builder.push_u16(UINT16_MAX));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(0)));
    FASTDB_WASM_STEP(builder.push_u32(UINT32_MAX));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_i32(INT32_MIN));
    FASTDB_WASM_STEP(builder.push_i32(INT32_MAX));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0x3ff0000000000000)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_u16n_bits(UINT64_C(0xbff0000000000000)));
    FASTDB_WASM_STEP(builder.push_u16n_bits(UINT64_C(0x3ff0000000000000)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x80000000)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0x7fa12345)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0xfff8000000001234)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_str(""));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_str("list"));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_wstr(wide.data(), wide.size()));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_bytes(opaque.data(), opaque.size()));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(1)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(2)));
    FASTDB_WASM_STEP(builder.push_str("two"));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_u8(UINT8_C(1)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.push_u8(UINT8_C(2)));
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(9)));
    FASTDB_WASM_STEP(builder.push_str("root"));

    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_bool(UINT8_C(0)));
    FASTDB_WASM_STEP(builder.push_u8(UINT8_C(0)));
    FASTDB_WASM_STEP(builder.push_u16(UINT16_C(0)));
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(0)));
    FASTDB_WASM_STEP(builder.push_i32(INT32_C(0)));
    FASTDB_WASM_STEP(builder.push_u8n_bits(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_u16n_bits(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_f32_bits(UINT32_C(0)));
    FASTDB_WASM_STEP(builder.push_f64_bits(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_str(""));
    FASTDB_WASM_STEP(builder.push_wstr(nullptr, UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_bytes(nullptr, UINT64_C(0)));
    for (std::uint32_t field = UINT32_C(0); field < UINT32_C(12); ++field) {
        FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));
    }
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.push_null());

    FASTDB_WASM_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(3)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(0)));
    FASTDB_WASM_STEP(builder.begin_list(UINT64_C(2)));
    FASTDB_WASM_STEP(builder.push_null());
    FASTDB_WASM_STEP(builder.begin_component());
    FASTDB_WASM_STEP(builder.push_u32(UINT32_C(7)));
    FASTDB_WASM_STEP(builder.push_str("entry"));
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

    auto text_spec =
        CompiledSpec::compile(read_file(root + "/spec/text-bytes.source.json"));
    if (!text_spec.has_value()) {
        return 19;
    }
    auto text_builder = PayloadBuilder::create(text_spec.value());
    if (!text_builder.has_value() ||
        !add_text_byte_values(text_builder.value())) {
        return 20;
    }
    auto text_values = text_builder.value().freeze();
    auto text_runtime = RuntimeSchema::compile(text_spec.value());
    if (!text_values.has_value() || !text_runtime.has_value()) {
        return 21;
    }
    auto text_layout =
        RecordLayout::plan(text_runtime.value(), text_values.value());
    if (!text_layout.has_value() ||
        text_layout.value().regions().size() != 10U) {
        return 22;
    }
    VectorSink text_sink(text_layout.value().total_length());
    if (!fastdb::payload::build::encode_record(text_layout.value(),
                                               text_values.value(), text_sink)
             .has_value() ||
        !exact_golden(root, "text-bytes", text_sink.bytes_)) {
        return 23;
    }
    auto text_opened = fastdb::payload::view::open_record(
        text_spec.value(), text_sink.bytes_.data(), text_sink.bytes_.size());
    const std::array<std::uint8_t, 33> expected_utf8{
        {'A',           'S',           'C',
         'I',           'I',           UINT8_C(0xe4),
         UINT8_C(0xb8), UINT8_C(0xad), UINT8_C(0x00),
         'r',           'e',           'p',
         'e',           'a',           't',
         'r',           'e',           'p',
         'e',           'a',           't',
         't',           'a',           'i',
         'l',           'P',           'I',
         'r',           'e',           'p',
         'e',           'a',           't'}};
    const std::array<std::uint8_t, 24> expected_utf16le{
        {UINT8_C(0x41), UINT8_C(0x00), UINT8_C(0x2d), UINT8_C(0x4e),
         UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x3d), UINT8_C(0xd8),
         UINT8_C(0x03), UINT8_C(0xde), UINT8_C(0x41), UINT8_C(0x00),
         UINT8_C(0x41), UINT8_C(0x00), UINT8_C(0x2d), UINT8_C(0x4e),
         UINT8_C(0x3d), UINT8_C(0xd8), UINT8_C(0x03), UINT8_C(0xde),
         UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x41), UINT8_C(0x00)}};
    const std::array<std::uint8_t, 18> expected_bytes{
        {UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x80), UINT8_C(0x00),
         UINT8_C(0xff), UINT8_C(0x80), UINT8_C(0x41), UINT8_C(0x00),
         UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x80), UINT8_C(0x41),
         UINT8_C(0x01), UINT8_C(0x02), UINT8_C(0x00), UINT8_C(0xff),
         UINT8_C(0x80), UINT8_C(0x41)}};
    const auto utf8_pool =
        text_opened.has_value()
            ? text_opened.value().pool_metadata(
                  fastdb::payload::layout::RegionKind::utf8_pool)
            : std::nullopt;
    const auto utf16_pool =
        text_opened.has_value()
            ? text_opened.value().pool_metadata(
                  fastdb::payload::layout::RegionKind::utf16_pool)
            : std::nullopt;
    const auto bytes_pool =
        text_opened.has_value()
            ? text_opened.value().pool_metadata(
                  fastdb::payload::layout::RegionKind::bytes_pool)
            : std::nullopt;
    if (!text_opened.has_value() ||
        !text_opened.value().text_validated_eagerly() ||
        text_opened.value().variable_slots().size() != 29U ||
        !utf8_pool.has_value() || utf8_pool->data_offset != UINT64_C(1336) ||
        utf8_pool->byte_length != expected_utf8.size() ||
        utf8_pool->element_count != expected_utf8.size() ||
        !utf16_pool.has_value() || utf16_pool->data_offset != UINT64_C(1370) ||
        utf16_pool->byte_length != expected_utf16le.size() ||
        utf16_pool->element_count != UINT64_C(12) || !bytes_pool.has_value() ||
        bytes_pool->data_offset != UINT64_C(1394) ||
        bytes_pool->byte_length != expected_bytes.size() ||
        bytes_pool->element_count != expected_bytes.size() ||
        !std::equal(expected_utf8.begin(), expected_utf8.end(),
                    text_sink.bytes_.begin() +
                        static_cast<std::ptrdiff_t>(utf8_pool->data_offset)) ||
        !std::equal(expected_utf16le.begin(), expected_utf16le.end(),
                    text_sink.bytes_.begin() +
                        static_cast<std::ptrdiff_t>(utf16_pool->data_offset)) ||
        !std::equal(expected_bytes.begin(), expected_bytes.end(),
                    text_sink.bytes_.begin() +
                        static_cast<std::ptrdiff_t>(bytes_pool->data_offset))) {
        return 24;
    }
    auto invalid_utf8 = text_sink.bytes_;
    invalid_utf8[static_cast<std::size_t>(
        text_layout.value().regions()[7].data_offset)] = UINT8_C(0xff);
    auto rejected = fastdb::payload::view::open_record(
        text_spec.value(), invalid_utf8.data(), invalid_utf8.size());
    if (rejected.has_value() ||
        rejected.error().code() != FDB_PAYLOAD_E_INVALID_TEXT_ENCODING ||
        rejected.error().path() != "/entries/texts/2" ||
        rejected.error().details_json() !=
            "{\"encoding\":\"utf-8\",\"reason\":\"invalid_sequence\"}") {
        return 25;
    }
    auto lazy_limits = fastdb::payload::view::default_open_limits();
    lazy_limits.validate_text_eager = false;
    auto lazy_opened = fastdb::payload::view::open_record(
        text_spec.value(), invalid_utf8.data(), invalid_utf8.size(),
        lazy_limits);
    if (!lazy_opened.has_value() ||
        lazy_opened.value().text_validated_eagerly() ||
        lazy_opened.value().validation_work() + UINT64_C(45) !=
            text_opened.value().validation_work()) {
        return 26;
    }

    const auto exact_list_runtime =
        [&](std::string_view name,
            bool (*add_values)(PayloadBuilder&),
            std::uint32_t expected_region_count) {
            auto spec = CompiledSpec::compile(
                read_file(root + "/spec/" + std::string(name) +
                          ".source.json"));
            if (!spec.has_value()) {
                return false;
            }
            auto builder = PayloadBuilder::create(spec.value());
            if (!builder.has_value() || !add_values(builder.value())) {
                return false;
            }
            auto logical = builder.value().freeze();
            auto runtime = RuntimeSchema::compile(spec.value());
            if (!logical.has_value() || !runtime.has_value()) {
                return false;
            }
            auto record =
                RecordLayout::plan(runtime.value(), logical.value());
            if (!record.has_value() ||
                record.value().region_count() != expected_region_count) {
                return false;
            }
            VectorSink output(record.value().total_length());
            if (!fastdb::payload::build::encode_record(
                     record.value(), logical.value(), output)
                     .has_value() ||
                !exact_golden(root, std::string(name), output.bytes_)) {
                return false;
            }
            auto index = fastdb::payload::view::open_record(
                spec.value(), output.bytes_.data(), output.bytes_.size());
            return index.has_value() &&
                   index.value().total_length() == output.bytes_.size() &&
                   index.value().validation_work() ==
                       record.value().validation_work();
        };
    if (!exact_list_runtime("nested-lists", add_nested_list_values,
                            UINT32_C(21))) {
        return 27;
    }
    if (!exact_list_runtime("component-list-composition",
                            add_component_list_composition_values,
                            UINT32_C(38))) {
        return 28;
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
