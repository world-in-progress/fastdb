#include "GoldenCorpus.hpp"
#include "TestSupport.hpp"

#include "payload/build/GraphEncoder.hpp"
#include "payload/build/PayloadBuilder.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/GraphLayout.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/GraphOpen.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using fastdb::payload::build::ByteSink;
using fastdb::payload::build::LogicalPayload;
using fastdb::payload::build::ObjectHandle;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::layout::GraphLayout;
using fastdb::payload::layout::RegionKind;
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
                    JsonPointer{}.append("sink"),
                    "Invalid graph test sink write",
                    fastdb::payload::json::JsonValue::object({})));
        }
        std::copy_n(data, static_cast<std::size_t>(size),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        next_offset_ += size;
        return Result<void>::success();
    }

    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    std::uint64_t next_offset() const noexcept { return next_offset_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t next_offset_{UINT64_C(0)};
};

std::uint32_t component_index(const CompiledSpec& spec,
                              std::string_view id) {
    const auto index = spec.component_index(id);
    if (!index.has_value()) {
        std::abort();
    }
    return *index;
}

Result<LogicalPayload> build_graph_scenario(
    CompiledSpec spec,
    std::string_view scenario,
    bool reverse_fill = false,
    bool reverse_declarations = false) {
    auto created = PayloadBuilder::create(spec);
    if (!created.has_value()) {
        return Result<LogicalPayload>::failure(std::move(created).error());
    }
    PayloadBuilder& builder = created.value();

#define FASTDB_GRAPH_STEP(expression)                                       \
    do {                                                                     \
        auto step_result = (expression);                                     \
        if (!step_result.has_value()) {                                      \
            return Result<LogicalPayload>::failure(                          \
                std::move(step_result).error());                             \
        }                                                                    \
    } while (false)

    if (scenario == "graph_empty") {
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(0)));
        return builder.freeze();
    }

    const std::uint32_t node = component_index(spec, "Node");
    if (scenario == "graph_single_root") {
        auto object = builder.declare_object(node);
        if (!object.has_value()) {
            return Result<LogicalPayload>::failure(std::move(object).error());
        }
        FASTDB_GRAPH_STEP(builder.begin_object_fill(object.value()));
        FASTDB_GRAPH_STEP(builder.push_u16(UINT16_C(0x1234)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_GRAPH_STEP(builder.push_object(object.value()));
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(1)));
        FASTDB_GRAPH_STEP(builder.push_null());
        return builder.freeze();
    }

    if (scenario != "graph_shared_cycle") {
        return Result<LogicalPayload>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INVALID_ARGUMENT,
                JsonPointer{}.append("scenario"),
                "Unknown graph binary scenario",
                fastdb::payload::json::JsonValue::object({})));
    }

    ObjectHandle first = UINT64_C(0);
    ObjectHandle second = UINT64_C(0);
    if (reverse_declarations) {
        auto second_result = builder.declare_object(node);
        auto first_result = builder.declare_object(node);
        if (!second_result.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(second_result).error());
        }
        if (!first_result.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(first_result).error());
        }
        second = second_result.value();
        first = first_result.value();
    } else {
        auto first_result = builder.declare_object(node);
        auto second_result = builder.declare_object(node);
        if (!first_result.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(first_result).error());
        }
        if (!second_result.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(second_result).error());
        }
        first = first_result.value();
        second = second_result.value();
    }

    const auto fill = [&](ObjectHandle object,
                          std::uint32_t value,
                          ObjectHandle self,
                          ObjectHandle shared,
                          std::uint8_t flag,
                          std::uint16_t code) -> Result<void> {
        auto started = builder.begin_object_fill(object);
        if (!started.has_value()) {
            return started;
        }
        auto pushed = builder.push_u32(value);
        if (!pushed.has_value()) {
            return pushed;
        }
        pushed = builder.push_ref(self);
        if (!pushed.has_value()) {
            return pushed;
        }
        pushed = builder.push_ref(shared);
        if (!pushed.has_value()) {
            return pushed;
        }
        pushed = builder.begin_component();
        if (!pushed.has_value()) {
            return pushed;
        }
        pushed = builder.push_bool(flag);
        if (!pushed.has_value()) {
            return pushed;
        }
        return builder.push_u16(code);
    };

    Result<void> filled = Result<void>::success();
    if (reverse_fill) {
        filled = fill(second, UINT32_C(200), second, second, UINT8_C(0),
                      UINT16_C(0xabcd));
        if (filled.has_value()) {
            filled = fill(first, UINT32_C(100), first, second, UINT8_C(1),
                          UINT16_C(0x1234));
        }
    } else {
        filled = fill(first, UINT32_C(100), first, second, UINT8_C(1),
                      UINT16_C(0x1234));
        if (filled.has_value()) {
            filled = fill(second, UINT32_C(200), second, second, UINT8_C(0),
                          UINT16_C(0xabcd));
        }
    }
    if (!filled.has_value()) {
        return Result<LogicalPayload>::failure(std::move(filled).error());
    }
    FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
    FASTDB_GRAPH_STEP(builder.push_object(first));
    FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(1)));
    FASTDB_GRAPH_STEP(builder.push_object(second));
#undef FASTDB_GRAPH_STEP
    return builder.freeze();
}

struct EncodedGraph final {
    CompiledSpec spec;
    GraphLayout layout;
    std::vector<std::uint8_t> bytes;
};

Result<EncodedGraph> encode_graph_case(const BinaryGoldenCase& item,
                                       bool reverse_fill = false,
                                       bool reverse_declarations = false) {
    auto compiled = CompiledSpec::compile(item.success.source);
    if (!compiled.has_value()) {
        return Result<EncodedGraph>::failure(std::move(compiled).error());
    }
    CompiledSpec spec = compiled.value();
    auto values = build_graph_scenario(compiled.value(),
                                       item.success.scenario,
                                       reverse_fill,
                                       reverse_declarations);
    if (!values.has_value()) {
        return Result<EncodedGraph>::failure(std::move(values).error());
    }
    auto runtime = RuntimeSchema::compile(spec);
    if (!runtime.has_value()) {
        return Result<EncodedGraph>::failure(std::move(runtime).error());
    }
    auto planned = GraphLayout::plan(runtime.value(), values.value());
    if (!planned.has_value()) {
        return Result<EncodedGraph>::failure(std::move(planned).error());
    }
    VectorSink sink(planned.value().total_length());
    auto encoded = fastdb::payload::build::encode_graph(
        planned.value(), values.value(), sink);
    if (!encoded.has_value()) {
        return Result<EncodedGraph>::failure(std::move(encoded).error());
    }
    if (sink.next_offset() != planned.value().total_length()) {
        return Result<EncodedGraph>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("sink"),
                "Graph encoder did not consume sink",
                fastdb::payload::json::JsonValue::object({})));
    }
    return Result<EncodedGraph>::success(EncodedGraph{
        std::move(spec), std::move(planned).value(), sink.bytes()});
}

const BinaryGoldenCase* find_case(const std::vector<BinaryGoldenCase>& cases,
                                  std::string_view name) {
    const auto found = std::find_if(
        cases.begin(), cases.end(), [name](const BinaryGoldenCase& item) {
            return item.name == name;
        });
    return found == cases.end() ? nullptr : &*found;
}

std::uint64_t load_u64(const std::vector<std::uint8_t>& bytes,
                       std::uint64_t offset) {
    auto value = fastdb::payload::layout::load_u64_le(
        bytes.data(), bytes.size(), offset, JsonPointer{});
    require(value.has_value());
    return value.value();
}

std::uint32_t load_u32(const std::vector<std::uint8_t>& bytes,
                       std::uint64_t offset) {
    auto value = fastdb::payload::layout::load_u32_le(
        bytes.data(), bytes.size(), offset, JsonPointer{});
    require(value.has_value());
    return value.value();
}

const fastdb::payload::layout::RegionDescriptor& require_region(
    const GraphLayout& layout,
    std::size_t index,
    RegionKind kind,
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t count,
    std::uint32_t stride,
    std::uint32_t alignment) {
    if (index >= layout.regions().size()) {
        std::abort();
    }
    const auto& region = layout.regions()[index];
    if (region.kind != kind || region.data_offset != offset ||
        region.byte_length != length || region.element_count != count ||
        region.stride != stride || region.alignment != alignment) {
        std::abort();
    }
    return region;
}

int test_region_rule_and_public_boundary() {
    static_assert(FDB_PAYLOAD_REGION_OBJECT_VALUES == UINT32_C(8));
    static_assert(static_cast<std::uint32_t>(RegionKind::object_values) ==
                  UINT32_C(8));
    const auto rule =
        fastdb::payload::layout::region_rule(RegionKind::object_values);
    require(rule.kind_value == UINT32_C(8));
    require(rule.count_unit ==
            fastdb::payload::layout::RegionCountUnit::objects);
    require(rule.owner_rule ==
            fastdb::payload::layout::RegionOwnerRule::component_index);
    require(rule.type_rule ==
            fastdb::payload::layout::RegionTypeRule::sentinel);
    require(rule.stride_rule ==
            fastdb::payload::layout::RegionStrideRule::component_stride);
    require(rule.alignment_rule ==
            fastdb::payload::layout::RegionAlignmentRule::component_alignment);
    require(!rule.is_validity && !rule.is_pool);
    return EXIT_SUCCESS;
}

int test_ordered_graph_goldens_and_strict_open() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    require(corpus.size() == 3U);
    const std::vector<std::string_view> names{
        "graph-empty", "graph-single-root", "graph-shared-cycle"};
    for (const std::string_view name : names) {
        const BinaryGoldenCase* item = find_case(corpus, name);
        require(item != nullptr);
        require(!item->success.layout_relative_path.empty());
        require(!item->success.layout_receipt.empty());
        auto encoded = encode_graph_case(*item);
        const std::string encode_diagnostic =
            encoded.has_value()
                ? std::string{name}
                : std::string{name} + ": " +
                      std::string{encoded.error().details_json()};
        require(encoded.has_value(), encode_diagnostic);
        const auto golden = decode_hex(item->success.binary_hex);
        require(encoded.value().bytes == golden);
        require(fastdb::payload::identity::sha256_lower_hex(
                    fastdb::payload::identity::sha256(
                        golden.data(), golden.size())) == item->success.sha256);
        auto opened = fastdb::payload::view::open_graph(
            encoded.value().spec, golden.data(), golden.size());
        auto repeated = fastdb::payload::view::open_graph(
            encoded.value().spec, golden.data(), golden.size());
        require(opened.has_value() && repeated.has_value());
        require(opened.value().total_length == golden.size());
        require(opened.value().total_length == repeated.value().total_length);
        require(opened.value().root_value_count ==
                encoded.value().layout.root_value_count());
        require(opened.value().graph_object_count ==
                encoded.value().layout.graph_object_count());
        require(opened.value().validation_work ==
                encoded.value().layout.validation_work());
        require(opened.value().region_count ==
                encoded.value().layout.region_count());
        require(opened.value().entry_count ==
                encoded.value().layout.entries().size());
    }
    return EXIT_SUCCESS;
}

int test_strict_open_rejects_malformed_graph_bytes_and_limits() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* single = find_case(corpus, "graph-single-root");
    const BinaryGoldenCase* cycle = find_case(corpus, "graph-shared-cycle");
    const BinaryGoldenCase* empty = find_case(corpus, "graph-empty");
    require(single != nullptr && cycle != nullptr && empty != nullptr);
    auto single_encoded = encode_graph_case(*single);
    auto cycle_encoded = encode_graph_case(*cycle);
    auto empty_encoded = encode_graph_case(*empty);
    require(single_encoded.has_value() && cycle_encoded.has_value() &&
            empty_encoded.has_value());

    const auto open_single = [&](const std::vector<std::uint8_t>& bytes) {
        return fastdb::payload::view::open_graph(
            single_encoded.value().spec, bytes.data(), bytes.size());
    };

    auto wrong_profile = single_encoded.value().bytes;
    wrong_profile[static_cast<std::size_t>(
        fastdb::payload::layout::header_profile_offset)] = UINT8_C(1);
    auto rejected = open_single(wrong_profile);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto wrong_object_region = single_encoded.value().bytes;
    constexpr std::uint64_t object_region_descriptor =
        fastdb::payload::layout::header_size +
        UINT64_C(4) * fastdb::payload::layout::region_descriptor_size;
    wrong_object_region[static_cast<std::size_t>(
        object_region_descriptor +
        fastdb::payload::layout::region_runtime_type_id_offset)] = UINT8_C(0);
    rejected = open_single(wrong_object_region);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto invalid_root = single_encoded.value().bytes;
    invalid_root[496U] = UINT8_C(1);
    rejected = open_single(invalid_root);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_INVALID_REFERENCE);
    require(rejected.error().details_json() ==
            R"({"object_id":"1","reason":"root_object_id_out_of_range"})");

    auto nonzero_entry_padding = single_encoded.value().bytes;
    nonzero_entry_padding[489U] = UINT8_C(1);
    rejected = open_single(nonzero_entry_padding);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto nonzero_validity_tail = single_encoded.value().bytes;
    nonzero_validity_tail[520U] = UINT8_C(0x80);
    rejected = open_single(nonzero_validity_tail);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto nonzero_object_padding = single_encoded.value().bytes;
    nonzero_object_padding[521U] = UINT8_C(1);
    rejected = open_single(nonzero_object_padding);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto object_depth = fastdb::payload::view::default_open_options();
    object_depth.max_nesting_depth = UINT64_C(0);
    rejected = fastdb::payload::view::open_graph(
        single_encoded.value().spec, single_encoded.value().bytes.data(),
        single_encoded.value().bytes.size(), object_depth);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    object_depth.max_nesting_depth = UINT64_C(1);
    require(fastdb::payload::view::open_graph(
                single_encoded.value().spec,
                single_encoded.value().bytes.data(),
                single_encoded.value().bytes.size(), object_depth)
                .has_value());

    auto missing_empty_pool = empty_encoded.value().bytes;
    missing_empty_pool[static_cast<std::size_t>(
        fastdb::payload::layout::header_region_count_offset)] = UINT8_C(1);
    auto empty_rejected = fastdb::payload::view::open_graph(
        empty_encoded.value().spec, missing_empty_pool.data(),
        missing_empty_pool.size());
    require(!empty_rejected.has_value());
    require(empty_rejected.error().code() ==
            FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto limited = fastdb::payload::view::default_open_options();
    limited.max_graph_objects = UINT64_C(1);
    auto cycle_rejected = fastdb::payload::view::open_graph(
        cycle_encoded.value().spec, cycle_encoded.value().bytes.data(),
        cycle_encoded.value().bytes.size(), limited);
    require(!cycle_rejected.has_value());
    require(cycle_rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);

    auto invalid_reference = cycle_encoded.value().bytes;
    invalid_reference[408U] = UINT8_C(2);
    cycle_rejected = fastdb::payload::view::open_graph(
        cycle_encoded.value().spec, invalid_reference.data(),
        invalid_reference.size());
    require(!cycle_rejected.has_value());
    require(cycle_rejected.error().code() == FDB_PAYLOAD_E_INVALID_REFERENCE);
    require(cycle_rejected.error().details_json() ==
            R"({"object_id":"2","reason":"reference_object_id_out_of_range"})");

    auto shallow = fastdb::payload::view::default_open_options();
    shallow.max_nesting_depth = UINT64_C(1);
    cycle_rejected = fastdb::payload::view::open_graph(
        cycle_encoded.value().spec, cycle_encoded.value().bytes.data(),
        cycle_encoded.value().bytes.size(), shallow);
    require(!cycle_rejected.has_value());
    require(cycle_rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    shallow.max_nesting_depth = UINT64_C(2);
    require(fastdb::payload::view::open_graph(
                cycle_encoded.value().spec,
                cycle_encoded.value().bytes.data(),
                cycle_encoded.value().bytes.size(), shallow)
                .has_value());

    auto exact_work = fastdb::payload::view::default_open_options();
    exact_work.max_validation_work =
        cycle_encoded.value().layout.validation_work();
    require(fastdb::payload::view::open_graph(
                cycle_encoded.value().spec,
                cycle_encoded.value().bytes.data(),
                cycle_encoded.value().bytes.size(), exact_work)
                .has_value());
    --exact_work.max_validation_work;
    cycle_rejected = fastdb::payload::view::open_graph(
        cycle_encoded.value().spec, cycle_encoded.value().bytes.data(),
        cycle_encoded.value().bytes.size(), exact_work);
    require(!cycle_rejected.has_value());
    require(cycle_rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    return EXIT_SUCCESS;
}

int test_empty_zero_count_identity_pool() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* item = find_case(corpus, "graph-empty");
    require(item != nullptr);
    auto encoded = encode_graph_case(*item);
    require(encoded.has_value());
    const GraphLayout& layout = encoded.value().layout;
    require(layout.total_length() == UINT64_C(280));
    require(layout.root_value_count() == UINT64_C(0));
    require(layout.graph_object_count() == UINT64_C(0));
    require(layout.region_count() == UINT32_C(2));
    require(layout.entries().size() == 1U);
    require(layout.object_aggregates().size() == 1U);
    require_region(layout, 0U, RegionKind::entry_values, UINT64_C(280),
                   UINT64_C(0), UINT64_C(0), UINT32_C(8), UINT32_C(8));
    const auto& objects = require_region(
        layout, 1U, RegionKind::object_values, UINT64_C(280), UINT64_C(0),
        UINT64_C(0), UINT32_C(1), UINT32_C(1));
    require(objects.owner_index == UINT32_C(0));
    require(objects.runtime_type_id == UINT32_MAX);
    require(layout.object_aggregates()[0].component_index == UINT32_C(0));
    require(layout.object_aggregates()[0].region_index == UINT32_C(1));
    require(layout.object_aggregates()[0].object_nodes.empty());
    require(load_u32(encoded.value().bytes,
                     fastdb::payload::layout::header_profile_offset) ==
            FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1);
    require(load_u64(encoded.value().bytes,
                     fastdb::payload::layout::header_root_value_count_offset) ==
            UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_object_zero_null_and_component_record_layout() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* item = find_case(corpus, "graph-single-root");
    require(item != nullptr);
    auto encoded = encode_graph_case(*item);
    require(encoded.has_value());
    const GraphLayout& layout = encoded.value().layout;
    require(layout.total_length() == UINT64_C(536));
    require(layout.root_value_count() == UINT64_C(2));
    require(layout.graph_object_count() == UINT64_C(1));
    require(layout.region_count() == UINT32_C(5));
    require_region(layout, 0U, RegionKind::entry_validity, UINT64_C(488),
                   UINT64_C(1), UINT64_C(1), UINT32_C(0), UINT32_C(1));
    require_region(layout, 1U, RegionKind::entry_values, UINT64_C(496),
                   UINT64_C(8), UINT64_C(1), UINT32_C(8), UINT32_C(8));
    require_region(layout, 2U, RegionKind::entry_validity, UINT64_C(504),
                   UINT64_C(1), UINT64_C(1), UINT32_C(0), UINT32_C(1));
    require_region(layout, 3U, RegionKind::entry_values, UINT64_C(512),
                   UINT64_C(8), UINT64_C(1), UINT32_C(8), UINT32_C(8));
    const auto& objects = require_region(
        layout, 4U, RegionKind::object_values, UINT64_C(520), UINT64_C(16),
        UINT64_C(1), UINT32_C(16), UINT32_C(8));
    require(objects.owner_index == UINT32_C(0));
    require(objects.runtime_type_id == UINT32_MAX);
    const auto& bytes = encoded.value().bytes;
    require(bytes[488] == UINT8_C(1));
    require(bytes[504] == UINT8_C(0));
    require(load_u64(bytes, UINT64_C(496)) == UINT64_C(0));
    require(load_u64(bytes, UINT64_C(512)) == UINT64_C(0));
    require(bytes[520] == UINT8_C(0));
    require(bytes[521] == UINT8_C(0));
    require(bytes[522] == UINT8_C(0x34));
    require(bytes[523] == UINT8_C(0x12));
    for (std::size_t index = 524U; index < 536U; ++index) {
        require(bytes[index] == UINT8_C(0));
    }
    return EXIT_SUCCESS;
}

int test_shared_cycle_fill_order_and_declaration_ids() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* item = find_case(corpus, "graph-shared-cycle");
    require(item != nullptr);
    auto encoded = encode_graph_case(*item);
    auto reverse_fill = encode_graph_case(*item, true, false);
    auto reverse_declarations = encode_graph_case(*item, false, true);
    require(encoded.has_value() && reverse_fill.has_value() &&
            reverse_declarations.has_value());
    require(encoded.value().bytes == reverse_fill.value().bytes);
    require(encoded.value().bytes != reverse_declarations.value().bytes);
    const GraphLayout& layout = encoded.value().layout;
    require(layout.total_length() == UINT64_C(456));
    require(layout.root_value_count() == UINT64_C(2));
    require(layout.graph_object_count() == UINT64_C(2));
    require(layout.region_count() == UINT32_C(3));
    require_region(layout, 0U, RegionKind::entry_values, UINT64_C(376),
                   UINT64_C(8), UINT64_C(1), UINT32_C(8), UINT32_C(8));
    require_region(layout, 1U, RegionKind::entry_values, UINT64_C(384),
                   UINT64_C(8), UINT64_C(1), UINT32_C(8), UINT32_C(8));
    const auto& objects = require_region(
        layout, 2U, RegionKind::object_values, UINT64_C(392), UINT64_C(64),
        UINT64_C(2), UINT32_C(32), UINT32_C(8));
    require(objects.owner_index == UINT32_C(0));
    require(objects.runtime_type_id == UINT32_MAX);
    const auto& bytes = encoded.value().bytes;
    require(load_u64(bytes, UINT64_C(376)) == UINT64_C(0));
    require(load_u64(bytes, UINT64_C(384)) == UINT64_C(1));
    require(load_u32(bytes, UINT64_C(392)) == UINT32_C(100));
    require(load_u64(bytes, UINT64_C(400)) == UINT64_C(0));
    require(load_u64(bytes, UINT64_C(408)) == UINT64_C(1));
    require(bytes[416] == UINT8_C(1));
    require(bytes[418] == UINT8_C(0x34));
    require(bytes[419] == UINT8_C(0x12));
    require(load_u32(bytes, UINT64_C(424)) == UINT32_C(200));
    require(load_u64(bytes, UINT64_C(432)) == UINT64_C(1));
    require(load_u64(bytes, UINT64_C(440)) == UINT64_C(1));
    require(bytes[448] == UINT8_C(0));
    require(bytes[450] == UINT8_C(0xcd));
    require(bytes[451] == UINT8_C(0xab));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_region_rule_and_public_boundary() != EXIT_SUCCESS ||
        test_ordered_graph_goldens_and_strict_open() != EXIT_SUCCESS ||
        test_strict_open_rejects_malformed_graph_bytes_and_limits() !=
            EXIT_SUCCESS ||
        test_empty_zero_count_identity_pool() != EXIT_SUCCESS ||
        test_object_zero_null_and_component_record_layout() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_shared_cycle_fill_order_and_declaration_ids();
}
