#include "GoldenCorpus.hpp"
#include "TestSupport.hpp"

#include "payload/build/PayloadBuilder.hpp"
#include "payload/build/RecordEncoder.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/RecordLayout.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Open.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace allocation_failure {

std::atomic<std::int64_t> fail_after{INT64_C(-1)};

void* allocate(std::size_t size) {
    const std::int64_t remaining =
        fail_after.load(std::memory_order_relaxed);
    if (remaining >= INT64_C(0)) {
        if (fail_after.fetch_sub(INT64_C(1), std::memory_order_relaxed) ==
            INT64_C(0)) {
            throw std::bad_alloc();
        }
    }
    void* const result = std::malloc(size == 0U ? 1U : size);
    if (result == nullptr) {
        throw std::bad_alloc();
    }
    return result;
}

}  // namespace allocation_failure

void* operator new(std::size_t size) {
    return allocation_failure::allocate(size);
}
void* operator new[](std::size_t size) {
    return allocation_failure::allocate(size);
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {

using fastdb::payload::build::ByteSink;
using fastdb::payload::build::LogicalPayload;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::layout::RecordLayout;
using fastdb::payload::layout::RuntimeSchema;
using fastdb::payload::spec::CompiledSpec;
using fastdb::test::payload::BinaryGoldenCase;

std::uint8_t hex_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    return static_cast<std::uint8_t>(value - 'a' + 10);
}

std::vector<std::uint8_t> decode_hex(std::string_view hexadecimal) {
    std::vector<std::uint8_t> bytes(hexadecimal.size() / 2U, UINT8_C(0));
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            (hex_nibble(hexadecimal[index * 2U]) << 4U) |
            hex_nibble(hexadecimal[index * 2U + 1U]));
    }
    return bytes;
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

class VectorSink final : public ByteSink {
public:
    explicit VectorSink(std::uint64_t size)
        : bytes_(static_cast<std::size_t>(size), UINT8_C(0xa5)) {}

    Result<void> write(std::uint64_t offset,
                       const std::uint8_t* data,
                       std::uint64_t size) override {
        if ((data == nullptr && size != UINT64_C(0)) ||
            offset != next_offset_ || size > bytes_.size() - next_offset_) {
            return Result<void>::failure(
                fastdb::payload::error::Error::from_details(
                    FDB_PAYLOAD_E_OUT_OF_BOUNDS,
                    JsonPointer{}.append("sink"), "Invalid test sink write",
                    fastdb::payload::json::JsonValue::object({})));
        }
        std::copy_n(data, static_cast<std::size_t>(size),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        next_offset_ += size;
        ++write_count_;
        return Result<void>::success();
    }

    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    std::uint64_t next_offset() const noexcept { return next_offset_; }
    std::uint64_t write_count() const noexcept { return write_count_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t next_offset_{UINT64_C(0)};
    std::uint64_t write_count_{UINT64_C(0)};
};

Result<LogicalPayload> build_scenario(CompiledSpec spec,
                                      std::string_view scenario) {
    auto created = PayloadBuilder::create(std::move(spec));
    if (!created.has_value()) {
        return Result<LogicalPayload>::failure(std::move(created).error());
    }
    PayloadBuilder& builder = created.value();
    if (scenario == "empty") {
        return builder.freeze();
    }

#define FASTDB_BINARY_SCENARIO_STEP(expression)                              \
    do {                                                                     \
        auto step_result = (expression);                                     \
        if (!step_result.has_value()) {                                      \
            return Result<LogicalPayload>::failure(                          \
                std::move(step_result).error());                             \
        }                                                                    \
    } while (false)

    if (scenario == "numeric_edges" ||
        scenario == "numeric_edges_alt_nan") {
        const bool alternate_nan = scenario == "numeric_edges_alt_nan";
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(1), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_MAX));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(2), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_MAX));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(3), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_MAX));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(4), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_MIN));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_MAX));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(5), UINT64_C(6)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0x80000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0x7f800000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0xff800000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(alternate_nan ? UINT32_C(0x7fffffff)
                                                : UINT32_C(0x7fa12345)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(alternate_nan ? UINT32_C(0xff800001)
                                                : UINT32_C(0xffdabcde)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(6), UINT64_C(6)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_f64_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x8000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0xfff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(
                alternate_nan ? UINT64_C(0x7fffffffffffffff)
                              : UINT64_C(0x7ff0000000000042)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(
                alternate_nan ? UINT64_C(0xfff0000000000001)
                              : UINT64_C(0xfff8000000001234)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(7), UINT64_C(6)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x0000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x8000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x406fe00000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x3ff8000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x0000000000000001)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(8), UINT64_C(4)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0xc0e0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x40dfffc000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0xc0dfffe000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0xc0dfffa000000000)));
        return builder.freeze();
    }

    if (scenario == "nested_components") {
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x40efffe000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x7ff0000000000042)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x3fe0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x8000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_MAX));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(1), UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x0000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x3ff8000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0xfff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(0)));

        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x40efffe000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_f64_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x3ff8000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0xfff8000000001234)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(42)));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(2), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        return builder.freeze();
    }

    if (scenario != "fixed_scalars") {
        return Result<LogicalPayload>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INVALID_ARGUMENT,
                JsonPointer{}.append("scenario"), "Unknown binary scenario",
                fastdb::payload::json::JsonValue::object({})));
    }

    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(0), UINT64_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(1), UINT64_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(0xab)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(2), UINT64_C(3)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(0x1234)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
    FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_MAX));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(3), UINT64_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(0x12345678)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(4), UINT64_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_C(-2)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(5), UINT64_C(4)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0x80000000)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0x7f800000)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0xff800000)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0x7fa12345)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(6), UINT64_C(4)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.push_f64_bits(UINT64_C(0x8000000000000000)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.push_f64_bits(UINT64_C(0xfff0000000000000)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.push_f64_bits(UINT64_C(0x7ff0000000000042)));
#undef FASTDB_BINARY_SCENARIO_STEP
    return builder.freeze();
}

struct Encoded final {
    CompiledSpec spec;
    RecordLayout layout;
    std::vector<std::uint8_t> bytes;
    std::uint64_t writes;
};

Result<Encoded> encode_case(const BinaryGoldenCase& item) {
    auto compiled = CompiledSpec::compile(item.success.source);
    if (!compiled.has_value()) {
        return Result<Encoded>::failure(std::move(compiled).error());
    }
    CompiledSpec spec = compiled.value();
    auto values = build_scenario(compiled.value(), item.success.scenario);
    if (!values.has_value()) {
        return Result<Encoded>::failure(std::move(values).error());
    }
    auto runtime = RuntimeSchema::compile(spec);
    if (!runtime.has_value()) {
        return Result<Encoded>::failure(std::move(runtime).error());
    }
    auto layout = RecordLayout::plan(runtime.value(), values.value());
    if (!layout.has_value()) {
        return Result<Encoded>::failure(std::move(layout).error());
    }
    VectorSink sink(layout.value().total_length());
    auto encoded = fastdb::payload::build::encode_record(
        layout.value(), values.value(), sink);
    if (!encoded.has_value()) {
        return Result<Encoded>::failure(std::move(encoded).error());
    }
    if (sink.next_offset() != layout.value().total_length()) {
        return Result<Encoded>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("sink"),
                "Encoder did not consume sink",
                fastdb::payload::json::JsonValue::object({})));
    }
    return Result<Encoded>::success(
        Encoded{std::move(spec), std::move(layout).value(), sink.bytes(),
                sink.write_count()});
}

const BinaryGoldenCase* find_case(const std::vector<BinaryGoldenCase>& cases,
                                  std::string_view name) {
    const auto found = std::find_if(
        cases.begin(), cases.end(), [name](const BinaryGoldenCase& item) {
            return item.name == name;
        });
    return found == cases.end() ? nullptr : &*found;
}

int test_binary_goldens_determinism_hash_and_headers() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    require(corpus.size() == 4U);
    for (const BinaryGoldenCase& item : corpus) {
        auto first = encode_case(item);
        auto second = encode_case(item);
        require(first.has_value(),
                first.has_value()
                    ? item.name
                    : item.name + ":" +
                          std::to_string(first.error().code()) + ":" +
                          std::string(first.error().path()) + ":" +
                          std::string(first.error().details_json()));
        require(second.has_value());
        const auto golden = decode_hex(item.success.binary_hex);
        require(first.value().bytes == golden, item.name);
        require(second.value().bytes == golden, item.name);
        require(first.value().bytes == second.value().bytes);
        require(first.value().writes > UINT64_C(0));
        require(fastdb::payload::identity::sha256_lower_hex(
                    fastdb::payload::identity::sha256(
                        golden.data(), golden.size())) == item.success.sha256);
        require(std::equal(first.value().spec.digest().begin(),
                           first.value().spec.digest().end(),
                           golden.begin() +
                               static_cast<std::ptrdiff_t>(
                                   fastdb::payload::layout::header_spec_digest_offset)));
        auto opened = fastdb::payload::view::open_record(
            first.value().spec, golden.data(), golden.size());
        require(opened.has_value());
        require(opened.value().total_length() == golden.size());
        require(opened.value().validation_work() ==
                    first.value().layout.validation_work(),
                item.name + ":opened=" +
                    std::to_string(opened.value().validation_work()) +
                    ":planned=" +
                    std::to_string(first.value().layout.validation_work()));
        auto exact_limits = fastdb::payload::view::default_open_limits();
        exact_limits.max_validation_work =
            first.value().layout.validation_work();
        require(fastdb::payload::view::open_record(
                    first.value().spec, golden.data(), golden.size(),
                    exact_limits)
                    .has_value());
        auto short_limits = exact_limits;
        short_limits.max_validation_work -= UINT64_C(1);
        auto limited = fastdb::payload::view::open_record(
            first.value().spec, golden.data(), golden.size(), short_limits);
        require(!limited.has_value());
        require(limited.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
        const std::string expected_short_path =
            item.name == "empty" ? "/header"
            : item.name == "fixed-scalars" ? "/regions/2"
            : item.name == "numeric-edges" ? "/padding"
                                             : "/entries/c_empty_many/1";
        require(limited.error().path() == expected_short_path,
                item.name + ":" + std::string(limited.error().path()));
        require(limited.error().details_json() ==
                "{\"actual\":\"" +
                    std::to_string(first.value().layout.validation_work()) +
                    "\",\"limit\":\"" +
                    std::to_string(
                        first.value().layout.validation_work() - UINT64_C(1)) +
                    "\",\"resource\":\"validation_work\"}");
    }

    const BinaryGoldenCase* empty = find_case(corpus, "empty");
    const BinaryGoldenCase* fixed = find_case(corpus, "fixed-scalars");
    require(empty != nullptr && fixed != nullptr);
    auto empty_encoded = encode_case(*empty);
    auto fixed_encoded = encode_case(*fixed);
    require(empty_encoded.value().layout.region_count() == UINT32_C(0));
    require(empty_encoded.value().layout.entry_count() == UINT32_C(0));
    require(empty_encoded.value().layout.total_length() == UINT64_C(128));
    require(fixed_encoded.value().layout.region_count() == UINT32_C(8));
    require(fixed_encoded.value().layout.entry_count() == UINT32_C(7));
    require(fastdb::payload::layout::load_u64_le(
                fixed_encoded.value().bytes.data(),
                fixed_encoded.value().bytes.size(),
                fastdb::payload::layout::header_total_length_offset,
                JsonPointer{})
                .value() == UINT64_C(928));
    require(fastdb::payload::layout::load_u64_le(
                fixed_encoded.value().bytes.data(),
                fixed_encoded.value().bytes.size(),
                fastdb::payload::layout::header_entry_directory_offset,
                JsonPointer{})
                .value() == UINT64_C(576));
    return EXIT_SUCCESS;
}

int test_digest_equal_independent_specs_share_stable_wire_layout() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* fixed = find_case(corpus, "fixed-scalars");
    require(fixed != nullptr);
    auto runtime_spec = CompiledSpec::compile(fixed->success.source);
    auto values_spec = CompiledSpec::compile(fixed->success.source);
    require(runtime_spec.has_value());
    require(values_spec.has_value());
    require(runtime_spec.value().digest() == values_spec.value().digest());
    require(&runtime_spec.value().resolved().entries()[0].type !=
            &values_spec.value().resolved().entries()[0].type);
    auto runtime = RuntimeSchema::compile(runtime_spec.value());
    auto values = build_scenario(values_spec.value(), fixed->success.scenario);
    require(runtime.has_value());
    require(values.has_value());
    auto planned = RecordLayout::plan(runtime.value(), values.value());
    require(planned.has_value());
    VectorSink sink(planned.value().total_length());
    require(fastdb::payload::build::encode_record(
                planned.value(), values.value(), sink)
                .has_value());
    require(sink.bytes() == decode_hex(fixed->success.binary_hex));
    return EXIT_SUCCESS;
}

std::string exact_resource_details(std::uint64_t actual,
                                   std::uint64_t limit,
                                   std::string_view resource) {
    return "{\"actual\":\"" + std::to_string(actual) +
           "\",\"limit\":\"" + std::to_string(limit) +
           "\",\"resource\":\"" + std::string(resource) + "\"}";
}

int require_resource_limit(
    const Result<fastdb::payload::view::PayloadIndex>& result,
    std::string_view path,
    std::uint64_t actual,
    std::uint64_t limit,
    std::string_view resource) {
    require(!result.has_value());
    require(result.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(result.error().path() == path);
    require(result.error().details_json() ==
            exact_resource_details(actual, limit, resource));
    return EXIT_SUCCESS;
}

int test_open_preflights_static_spec_limits_and_known_work() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* fixed = find_case(corpus, "fixed-scalars");
    require(fixed != nullptr);
    auto compiled = CompiledSpec::compile(fixed->success.source);
    require(compiled.has_value());
    const auto bytes = decode_hex(fixed->success.binary_hex);

    auto limits = fastdb::payload::view::default_open_limits();
    limits.max_entries = UINT64_C(6);
    require(require_resource_limit(fastdb::payload::view::open_record(
                                       compiled.value(), bytes.data(),
                                       bytes.size(), limits),
                                   "/entries", UINT64_C(7), UINT64_C(6),
                                   "entries") == EXIT_SUCCESS);

    limits = fastdb::payload::view::default_open_limits();
    limits.max_regions = UINT64_C(7);
    require(require_resource_limit(fastdb::payload::view::open_record(
                                       compiled.value(), bytes.data(),
                                       bytes.size(), limits),
                                   "/regions", UINT64_C(8), UINT64_C(7),
                                   "regions") == EXIT_SUCCESS);

    limits = fastdb::payload::view::default_open_limits();
    limits.max_validation_work = UINT64_C(15);
    require(require_resource_limit(fastdb::payload::view::open_record(
                                       compiled.value(), bytes.data(),
                                       bytes.size(), limits),
                                   "/validation_work", UINT64_C(16),
                                   UINT64_C(15), "validation_work") ==
            EXIT_SUCCESS);

    std::string wide_source = doubling_component_spec(true);
    auto wide = CompiledSpec::compile(wide_source);
    require(wide.has_value());
    auto wide_bytes = bytes;
    std::copy(wide.value().digest().begin(), wide.value().digest().end(),
              wide_bytes.begin() + static_cast<std::ptrdiff_t>(
                                       fastdb::payload::layout::header_spec_digest_offset));
    limits = fastdb::payload::view::default_open_limits();
    limits.max_components = UINT64_C(33);
    require(require_resource_limit(fastdb::payload::view::open_record(
                                       wide.value(), wide_bytes.data(),
                                       wide_bytes.size(), limits),
                                   "/components", UINT64_C(34), UINT64_C(33),
                                   "components") == EXIT_SUCCESS);

    auto bad_magic = bytes;
    bad_magic[0] ^= UINT8_C(1);
    limits = fastdb::payload::view::default_open_limits();
    limits.max_entries = UINT64_C(0);
    require(fastdb::payload::view::open_record(
                compiled.value(), bad_magic.data(), bad_magic.size(), limits)
                .error()
                .code() == FDB_PAYLOAD_E_INVALID_MAGIC);
    return EXIT_SUCCESS;
}

int test_region_matrix_zero_boundaries_and_partition_rules() {
    using fastdb::payload::layout::RegionCountUnit;
    using fastdb::payload::layout::RegionAlignmentRule;
    using fastdb::payload::layout::RegionKind;
    using fastdb::payload::layout::RegionOwnerRule;
    using fastdb::payload::layout::RegionStrideRule;
    using fastdb::payload::layout::RegionTypeRule;
    constexpr std::array<RegionKind, 7> kinds{{
        RegionKind::entry_values, RegionKind::entry_validity,
        RegionKind::list_items, RegionKind::list_validity,
        RegionKind::utf8_pool, RegionKind::utf16_pool,
        RegionKind::bytes_pool}};
    constexpr std::array<std::uint32_t, 7> numbers{{UINT32_C(1), UINT32_C(2),
                                                    UINT32_C(3), UINT32_C(4),
                                                    UINT32_C(5), UINT32_C(6),
                                                    UINT32_C(7)}};
    for (std::size_t index = 0U; index < kinds.size(); ++index) {
        const auto rule = fastdb::payload::layout::region_rule(kinds[index]);
        require(rule.kind_value == numbers[index]);
    }
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .count_unit == RegionCountUnit::values);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .owner_rule == RegionOwnerRule::entry_index);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .type_rule ==
            RegionTypeRule::entry_root_runtime_type_id);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .stride_rule == RegionStrideRule::slot_stride);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .alignment_rule == RegionAlignmentRule::slot_alignment);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_validity)
                .is_validity);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_validity)
                .stride_rule == RegionStrideRule::zero);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_validity)
                .alignment_rule == RegionAlignmentRule::one);
    require(fastdb::payload::layout::region_rule(RegionKind::list_items)
                .count_unit == RegionCountUnit::items);
    require(fastdb::payload::layout::region_rule(RegionKind::list_items)
                .owner_rule == RegionOwnerRule::list_runtime_type_id);
    require(fastdb::payload::layout::region_rule(RegionKind::list_items)
                .type_rule == RegionTypeRule::list_item_runtime_type_id);
    require(fastdb::payload::layout::region_rule(RegionKind::list_items)
                .stride_rule == RegionStrideRule::slot_stride);
    require(fastdb::payload::layout::region_rule(RegionKind::list_validity)
                .is_validity);
    require(fastdb::payload::layout::region_rule(RegionKind::list_validity)
                .alignment_rule == RegionAlignmentRule::one);
    require(fastdb::payload::layout::region_rule(RegionKind::utf8_pool)
                .count_unit == RegionCountUnit::bytes);
    require(fastdb::payload::layout::region_rule(RegionKind::utf8_pool)
                .owner_rule == RegionOwnerRule::sentinel);
    require(fastdb::payload::layout::region_rule(RegionKind::utf8_pool)
                .type_rule == RegionTypeRule::sentinel);
    require(fastdb::payload::layout::region_rule(RegionKind::utf16_pool)
                .count_unit == RegionCountUnit::utf16_code_units);
    require(fastdb::payload::layout::region_rule(RegionKind::utf16_pool)
                .alignment_rule == RegionAlignmentRule::two);
    require(fastdb::payload::layout::region_rule(RegionKind::bytes_pool)
                .count_unit == RegionCountUnit::bytes);
    require(fastdb::payload::layout::region_rule(RegionKind::bytes_pool)
                .alignment_rule == RegionAlignmentRule::one);

    const JsonPointer path = JsonPointer{}.append("partition");
    require(fastdb::payload::layout::checked_partition_advance(
                UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(4), path)
                .value() == UINT64_C(0));
    require(fastdb::payload::layout::checked_partition_advance(
                UINT64_C(0), UINT64_C(1), UINT64_C(1), UINT64_C(4), path)
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);
    require(fastdb::payload::layout::checked_partition_advance(
                UINT64_C(0), UINT64_C(0), UINT64_C(3), UINT64_C(4), path)
                .value() == UINT64_C(3));
    require(fastdb::payload::layout::require_partition_consumed(
                UINT64_C(3), UINT64_C(4), path)
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);
    require(fastdb::payload::layout::require_partition_consumed(
                UINT64_C(4), UINT64_C(4), path)
                .has_value());
    std::uint64_t pool_cursor = UINT64_C(0);
    pool_cursor = fastdb::payload::layout::checked_partition_advance(
                      pool_cursor, UINT64_C(0), UINT64_C(0), UINT64_C(3), path)
                      .value();
    pool_cursor = fastdb::payload::layout::checked_partition_advance(
                      pool_cursor, UINT64_C(0), UINT64_C(3), UINT64_C(3), path)
                      .value();
    require(fastdb::payload::layout::require_partition_consumed(
                pool_cursor, UINT64_C(3), path)
                .has_value());
    std::uint64_t list_cursor = UINT64_C(0);
    list_cursor = fastdb::payload::layout::checked_partition_advance(
                      list_cursor, UINT64_C(0), UINT64_C(2), UINT64_C(4), path)
                      .value();
    list_cursor = fastdb::payload::layout::checked_partition_advance(
                      list_cursor, UINT64_C(2), UINT64_C(2), UINT64_C(4), path)
                      .value();
    require(fastdb::payload::layout::require_partition_consumed(
                list_cursor, UINT64_C(4), path)
                .has_value());

    auto compiled = CompiledSpec::compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"empty","cardinality":"many","type":{"kind":"u8"}}],"components":[]})");
    require(compiled.has_value());
    auto values = build_scenario(compiled.value(), "empty");
    require(!values.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    require(created.value().begin_entry(UINT32_C(0), UINT64_C(0)).has_value());
    auto frozen = created.value().freeze();
    require(frozen.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto layout = RecordLayout::plan(runtime.value(), frozen.value());
    require(layout.has_value());
    require(layout.value().regions().size() == 1U);
    require(layout.value().regions()[0].byte_length == UINT64_C(0));
    require(layout.value().regions()[0].data_offset == UINT64_C(224));
    require(layout.value().total_length() == UINT64_C(224));
    return EXIT_SUCCESS;
}

int test_task3_metadata_observation_and_nan_canonicalization() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* numeric = find_case(corpus, "numeric-edges");
    const BinaryGoldenCase* nested = find_case(corpus, "nested-components");
    require(numeric != nullptr && nested != nullptr);

    auto numeric_encoded = encode_case(*numeric);
    require(numeric_encoded.has_value());
    BinaryGoldenCase alternate = *numeric;
    alternate.success.scenario = "numeric_edges_alt_nan";
    auto alternate_encoded = encode_case(alternate);
    require(alternate_encoded.has_value());
    require(alternate_encoded.value().bytes == numeric_encoded.value().bytes);

    auto numeric_opened = fastdb::payload::view::open_record(
        numeric_encoded.value().spec, numeric_encoded.value().bytes.data(),
        numeric_encoded.value().bytes.size());
    require(numeric_opened.has_value());
    const auto normalized_entry =
        numeric_opened.value().entry_slot(UINT32_C(7));
    require(normalized_entry.has_value());
    require(normalized_entry.value().runtime_type_id == UINT32_C(7));
    require(normalized_entry.value().value_count == UINT64_C(6));
    require(normalized_entry.value().stride == UINT32_C(1));
    require(!normalized_entry.value().has_validity);
    require(numeric_opened.value()
                .scalar(numeric_encoded.value().bytes.data(),
                        numeric_encoded.value().bytes.size(), UINT32_C(7),
                        UINT64_C(3))
                .value()
                .bits == UINT64_C(0x0000000000000000));
    require(numeric_opened.value()
                .scalar(numeric_encoded.value().bytes.data(),
                        numeric_encoded.value().bytes.size(), UINT32_C(7),
                        UINT64_C(4))
                .value()
                .bits == UINT64_C(0x4000000000000000));
    require(numeric_opened.value()
                .scalar(numeric_encoded.value().bytes.data(),
                        numeric_encoded.value().bytes.size(), UINT32_C(8),
                        UINT64_C(2))
                .value()
                .bits == UINT64_C(0xc0e0000000000000));
    require(numeric_opened.value()
                .scalar(numeric_encoded.value().bytes.data(),
                        numeric_encoded.value().bytes.size(), UINT32_C(8),
                        UINT64_C(3))
                .value()
                .bits == UINT64_C(0xc0dfff8000000000));

    auto nested_encoded = encode_case(*nested);
    require(nested_encoded.has_value());
    auto nested_opened = fastdb::payload::view::open_record(
        nested_encoded.value().spec, nested_encoded.value().bytes.data(),
        nested_encoded.value().bytes.size());
    require(nested_opened.has_value());
    const auto direct_entry = nested_opened.value().entry_slot(UINT32_C(0));
    require(direct_entry.has_value());
    require(direct_entry.value().data_offset == UINT64_C(472));
    require(direct_entry.value().value_count == UINT64_C(1));
    require(direct_entry.value().stride == UINT32_C(72));
    require(!direct_entry.value().has_validity);
    const std::array<std::uint32_t, 3> code_path{{UINT32_C(1), UINT32_C(1),
                                                  UINT32_C(1)}};
    const auto code_slot = nested_opened.value().field_slot(
        UINT32_C(0), code_path.data(), code_path.size());
    require(code_slot.has_value());
    require(code_slot.value().relative_offset == UINT32_C(34));
    require(code_slot.value().stride == UINT32_C(2));
    require(code_slot.value().kind ==
            fastdb::payload::spec::TypeKind::u16n);
    require(nested_opened.value()
                .field_scalar(nested_encoded.value().bytes.data(),
                              nested_encoded.value().bytes.size(),
                              UINT32_C(0), UINT64_C(0), code_path.data(),
                              code_path.size())
                .value()
                .bits == UINT64_C(0x40efffe000000000));
    const std::array<std::uint32_t, 2> signed_zero_path{{UINT32_C(2),
                                                        UINT32_C(2)}};
    require(nested_opened.value()
                .field_scalar(nested_encoded.value().bytes.data(),
                              nested_encoded.value().bytes.size(),
                              UINT32_C(0), UINT64_C(0),
                              signed_zero_path.data(),
                              signed_zero_path.size())
                .value()
                .bits == UINT64_C(0x8000000000000000));
    const auto null_descendant = nested_opened.value().field_scalar(
        nested_encoded.value().bytes.data(), nested_encoded.value().bytes.size(),
        UINT32_C(1), UINT64_C(0), code_path.data(), code_path.size());
    require(null_descendant.has_value());
    require(!null_descendant.value().present);
    return EXIT_SUCCESS;
}

int test_large_many_component_stride_and_count_overflow() {
    auto compiled = CompiledSpec::compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"rows","cardinality":"many","type":{"kind":"component","id":"Wide"}}],"components":[{"id":"Wide","kind":"record","fields":[{"id":"a","type":{"kind":"f64"}},{"id":"b","type":{"kind":"u32"}}]}]})");
    require(compiled.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    constexpr std::uint64_t row_count = UINT64_C(4096);
    require(created.value().begin_entry(UINT32_C(0), row_count).has_value());
    for (std::uint64_t index = UINT64_C(0); index < row_count; ++index) {
        require(created.value().begin_component().has_value());
        require(created.value()
                    .push_f64_bits(UINT64_C(0x3ff0000000000000) + index)
                    .has_value());
        require(created.value().push_u32(static_cast<std::uint32_t>(index))
                    .has_value());
    }
    auto values = created.value().freeze();
    require(values.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto planned = RecordLayout::plan(runtime.value(), values.value());
    require(planned.has_value());
    require(planned.value().regions()[0].stride == UINT32_C(16));
    require(planned.value().regions()[0].byte_length == UINT64_C(65536));
    VectorSink sink(planned.value().total_length());
    require(fastdb::payload::build::encode_record(
                planned.value(), values.value(), sink)
                .has_value());
    require(fastdb::payload::view::open_record(
                compiled.value(), sink.bytes().data(), sink.bytes().size())
                .has_value());

    auto overflow = sink.bytes();
    require(fastdb::payload::layout::store_u64_le(
                overflow.data(), overflow.size(),
                fastdb::payload::layout::header_root_value_count_offset,
                UINT64_MAX, JsonPointer{})
                .has_value());
    const std::uint64_t entry_base =
        fastdb::payload::layout::header_size +
        fastdb::payload::layout::region_descriptor_size;
    require(fastdb::payload::layout::store_u64_le(
                overflow.data(), overflow.size(),
                entry_base + fastdb::payload::layout::entry_value_count_offset,
                UINT64_MAX, JsonPointer{})
                .has_value());
    const auto rejected = fastdb::payload::view::open_record(
        compiled.value(), overflow.data(), overflow.size());
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_LENGTH_OVERFLOW);
    require(rejected.error().path() == "/regions/0",
            std::string(rejected.error().path()) + ":" +
                std::string(rejected.error().details_json()));
    require(rejected.error().details_json() ==
            "{\"reason\":\"wire_multiply_overflow\"}");
    return EXIT_SUCCESS;
}

int require_reason_error(
    const Result<fastdb::payload::view::PayloadIndex>& result,
    std::uint32_t code,
    std::string_view path,
    std::string_view reason) {
    require(!result.has_value());
    require(result.error().code() == code);
    require(result.error().path() == path);
    require(result.error().details_json() ==
            "{\"reason\":\"" + std::string(reason) + "\"}");
    return EXIT_SUCCESS;
}

int test_component_malformed_bytes_have_exact_diagnostics() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* nested = find_case(corpus, "nested-components");
    require(nested != nullptr);
    auto encoded = encode_case(*nested);
    require(encoded.has_value());
    const auto& layout = encoded.value().layout;
    const auto& entry0 = layout.entries()[0];
    const auto& entry1 = layout.entries()[1];
    const auto& value0 = layout.regions()[entry0.values_region_index];
    const auto& validity1 = layout.regions()[entry1.validity_region_index];

    auto bad_tail_bit = encoded.value().bytes;
    bad_tail_bit[static_cast<std::size_t>(validity1.data_offset)] |=
        UINT8_C(0x80);
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, bad_tail_bit.data(),
                    bad_tail_bit.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/regions/1",
                "nonzero_validity_tail") == EXIT_SUCCESS);

    auto bad_field_padding = encoded.value().bytes;
    bad_field_padding[static_cast<std::size_t>(value0.data_offset +
                                                UINT64_C(2))] = UINT8_C(1);
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, bad_field_padding.data(),
                    bad_field_padding.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/entries/a_direct/0",
                "nonzero_component_padding") == EXIT_SUCCESS);

    auto bad_tail_padding = encoded.value().bytes;
    bad_tail_padding[static_cast<std::size_t>(value0.data_offset +
                                               UINT64_C(68))] = UINT8_C(1);
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, bad_tail_padding.data(),
                    bad_tail_padding.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/entries/a_direct/0",
                "nonzero_component_padding") == EXIT_SUCCESS);

    auto bad_null = encoded.value().bytes;
    const auto& value1 = layout.regions()[entry1.values_region_index];
    bad_null[static_cast<std::size_t>(value1.data_offset)] = UINT8_C(1);
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, bad_null.data(), bad_null.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/entries/b_nullable_many/0", "nonzero_null_storage") ==
            EXIT_SUCCESS);

    const std::uint64_t region0 = fastdb::payload::layout::header_size;
    auto wrong_stride = encoded.value().bytes;
    require(fastdb::payload::layout::store_u32_le(
                wrong_stride.data(), wrong_stride.size(),
                region0 + fastdb::payload::layout::region_stride_offset,
                value0.stride + UINT32_C(1), JsonPointer{})
                .has_value());
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, wrong_stride.data(),
                    wrong_stride.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/regions/0",
                "region_descriptor_contract") == EXIT_SUCCESS);

    auto wrong_alignment = encoded.value().bytes;
    require(fastdb::payload::layout::store_u32_le(
                wrong_alignment.data(), wrong_alignment.size(),
                region0 + fastdb::payload::layout::region_alignment_offset,
                UINT32_C(4), JsonPointer{})
                .has_value());
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, wrong_alignment.data(),
                    wrong_alignment.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/regions/0",
                "region_descriptor_contract") == EXIT_SUCCESS);

    auto wrong_type = encoded.value().bytes;
    require(fastdb::payload::layout::store_u32_le(
                wrong_type.data(), wrong_type.size(),
                region0 +
                    fastdb::payload::layout::region_runtime_type_id_offset,
                UINT32_C(999), JsonPointer{})
                .has_value());
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, wrong_type.data(), wrong_type.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/regions/0",
                "region_descriptor_contract") == EXIT_SUCCESS);

    auto truncated = encoded.value().bytes;
    truncated.resize(static_cast<std::size_t>(value0.data_offset +
                                               value0.byte_length -
                                               UINT64_C(1)));
    require(fastdb::payload::layout::store_u64_le(
                truncated.data(), truncated.size(),
                fastdb::payload::layout::header_total_length_offset,
                truncated.size(), JsonPointer{})
                .has_value());
    const auto truncated_result = fastdb::payload::view::open_record(
        encoded.value().spec, truncated.data(), truncated.size());
    require(!truncated_result.has_value());
    require(truncated_result.error().code() == FDB_PAYLOAD_E_OUT_OF_BOUNDS);
    require(truncated_result.error().path() == "/regions/0");
    require(truncated_result.error().details_json() ==
            "{\"available\":\"543\",\"end\":\"544\",\"reason\":"
            "\"wire_range_out_of_bounds\"}");
    return EXIT_SUCCESS;
}

int test_task3_layout_encode_open_allocation_sweeps() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* nested = find_case(corpus, "nested-components");
    require(nested != nullptr);
    auto compiled = CompiledSpec::compile(nested->success.source);
    auto values = build_scenario(compiled.value(), nested->success.scenario);
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(compiled.has_value() && values.has_value() && runtime.has_value());

    std::uint64_t layout_failures = UINT64_C(0);
    Result<RecordLayout> planned = Result<RecordLayout>::failure(
        fastdb::payload::error::Error::from_details(
            FDB_PAYLOAD_E_INTERNAL, JsonPointer{}, "uninitialized",
            fastdb::payload::json::JsonValue::object({})));
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(4096);
         ++allocation) {
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        try {
            planned = RecordLayout::plan(runtime.value(), values.value());
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "layout escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (planned.has_value()) {
            break;
        }
        require(planned.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++layout_failures;
    }
    require(planned.has_value());
    require(layout_failures > UINT64_C(0));

    std::uint64_t encode_failures = UINT64_C(0);
    std::vector<std::uint8_t> encoded;
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(4096);
         ++allocation) {
        VectorSink sink(planned.value().total_length());
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        Result<void> result = Result<void>::success();
        try {
            result = fastdb::payload::build::encode_record(
                planned.value(), values.value(), sink);
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "encode escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (result.has_value()) {
            encoded = sink.bytes();
            break;
        }
        require(result.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++encode_failures;
    }
    require(!encoded.empty());
    require(encode_failures > UINT64_C(0));

    std::uint64_t open_failures = UINT64_C(0);
    Result<fastdb::payload::view::PayloadIndex> opened =
        Result<fastdb::payload::view::PayloadIndex>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}, "uninitialized",
                fastdb::payload::json::JsonValue::object({})));
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(4096);
         ++allocation) {
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        try {
            opened = fastdb::payload::view::open_record(
                compiled.value(), encoded.data(), encoded.size());
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "open escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (opened.has_value()) {
            break;
        }
        require(opened.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++open_failures;
    }
    require(opened.has_value());
    require(open_failures > UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_open_observation_and_malformed_canonical_values() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* fixed = find_case(corpus, "fixed-scalars");
    require(fixed != nullptr);
    auto encoded = encode_case(*fixed);
    require(encoded.has_value());

    std::vector<std::uint8_t> unaligned(encoded.value().bytes.size() + 1U,
                                        UINT8_C(0));
    std::copy(encoded.value().bytes.begin(), encoded.value().bytes.end(),
              unaligned.begin() + 1);
    auto opened = fastdb::payload::view::open_record(
        encoded.value().spec, unaligned.data() + 1,
        encoded.value().bytes.size());
    require(opened.has_value());
    require(opened.value().entry_count() == UINT32_C(7));
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(0), UINT64_C(0))
                .value()
                .bits ==
            UINT64_C(1));
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(2), UINT64_C(1))
                .value()
                .present ==
            false);
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(5), UINT64_C(0))
                .value()
                .bits ==
            UINT64_C(0x80000000));
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(5), UINT64_C(3))
                .value()
                .bits ==
            UINT64_C(0x7fc00000));
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(6), UINT64_C(3))
                .value()
                .bits ==
            UINT64_C(0x7ff8000000000000));

    auto bad_bool = encoded.value().bytes;
    bad_bool[856] = UINT8_C(2);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_bool.data(), bad_bool.size())
                .error()
                .code() == FDB_PAYLOAD_E_INVALID_BINARY_VALUE);

    auto bad_null = encoded.value().bytes;
    bad_null[862] = UINT8_C(1);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_null.data(), bad_null.size())
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto bad_padding = encoded.value().bytes;
    bad_padding[859] = UINT8_C(1);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_padding.data(), bad_padding.size())
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto bad_nan = encoded.value().bytes;
    bad_nan[888] = UINT8_C(1);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_nan.data(), bad_nan.size())
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto bad_digest = encoded.value().bytes;
    bad_digest[32] ^= UINT8_C(1);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_digest.data(), bad_digest.size())
                .error()
                .code() == FDB_PAYLOAD_E_DIGEST_MISMATCH);

    auto limits = fastdb::payload::view::default_open_limits();
    limits.max_total_bytes = UINT64_C(128);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, encoded.value().bytes.data(),
                encoded.value().bytes.size(), limits)
                .error()
                .code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(fastdb::payload::error::symbol_for_code(
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY) ==
            "NON_CANONICAL_BINARY");
    require(fastdb::payload::error::symbol_for_code(
                FDB_PAYLOAD_E_INVALID_BINARY_VALUE) ==
            "INVALID_BINARY_VALUE");
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_binary_goldens_determinism_hash_and_headers() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_digest_equal_independent_specs_share_stable_wire_layout() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_open_preflights_static_spec_limits_and_known_work() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_region_matrix_zero_boundaries_and_partition_rules() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task3_metadata_observation_and_nan_canonicalization() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_large_many_component_stride_and_count_overflow() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_component_malformed_bytes_have_exact_diagnostics() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task3_layout_encode_open_allocation_sweeps() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_open_observation_and_malformed_canonical_values();
}
