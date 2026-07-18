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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    if (scenario != "fixed_scalars") {
        return Result<LogicalPayload>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INVALID_ARGUMENT,
                JsonPointer{}.append("scenario"), "Unknown binary scenario",
                fastdb::payload::json::JsonValue::object({})));
    }

#define FASTDB_BINARY_SCENARIO_STEP(expression)                              \
    do {                                                                     \
        auto step_result = (expression);                                     \
        if (!step_result.has_value()) {                                      \
            return Result<LogicalPayload>::failure(                          \
                std::move(step_result).error());                             \
        }                                                                    \
    } while (false)

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
    require(corpus.size() == 2U);
    for (const BinaryGoldenCase& item : corpus) {
        auto first = encode_case(item);
        auto second = encode_case(item);
        require(first.has_value());
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
                first.value().layout.validation_work());
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
        require(limited.error().path() ==
                (item.name == "empty" ? "/header" : "/regions/2"));
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
    require(opened.value().scalar(UINT32_C(0), UINT64_C(0)).value().bits ==
            UINT64_C(1));
    require(opened.value().scalar(UINT32_C(2), UINT64_C(1)).value().present ==
            false);
    require(opened.value().scalar(UINT32_C(5), UINT64_C(0)).value().bits ==
            UINT64_C(0x80000000));
    require(opened.value().scalar(UINT32_C(5), UINT64_C(3)).value().bits ==
            UINT64_C(0x7fc00000));
    require(opened.value().scalar(UINT32_C(6), UINT64_C(3)).value().bits ==
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
    return test_open_observation_and_malformed_canonical_values();
}
