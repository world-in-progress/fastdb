#include "payload/build/PayloadBuilder.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/InputSpan.hpp"
#include "payload/layout/NormalizedInteger.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/layout/TextEncoding.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fastdb::payload::build {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonPointerBuilder;
using json::JsonValue;
using spec::Cardinality;
using spec::Component;
using spec::Entry;
using spec::TypeKind;
using spec::TypeNode;

constexpr std::uint64_t logical_node_bytes = UINT64_C(64);
constexpr std::uint64_t logical_frame_bytes = UINT64_C(64);
constexpr std::uint64_t logical_root_bytes = UINT64_C(8);

bool checked_add(std::uint64_t left,
                 std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (right > UINT64_MAX - left) {
        return false;
    }
    output = left + right;
    return true;
}

bool checked_multiply(std::uint64_t left,
                      std::uint64_t right,
                      std::uint64_t& output) noexcept {
    if (left != UINT64_C(0) && right > UINT64_MAX / left) {
        return false;
    }
    output = left * right;
    return true;
}

JsonValue details(JsonValue::Object members) {
    return JsonValue::object(std::move(members));
}

Error simple_error(std::uint32_t code,
                   JsonPointer path,
                   const char* message,
                   const char* reason) {
    return Error::from_details(
        code, std::move(path), message,
        details({JsonValue::Member{"reason", JsonValue{reason}}}));
}

Error allocation_error() {
    return simple_error(FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
                        "Payload builder allocation failed",
                        "allocation_failed");
}

const char* type_name(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return "bool";
    case TypeKind::u8:
        return "u8";
    case TypeKind::u16:
        return "u16";
    case TypeKind::u32:
        return "u32";
    case TypeKind::i32:
        return "i32";
    case TypeKind::u8n:
        return "u8n";
    case TypeKind::u16n:
        return "u16n";
    case TypeKind::f32:
        return "f32";
    case TypeKind::f64:
        return "f64";
    case TypeKind::str:
        return "str";
    case TypeKind::wstr:
        return "wstr";
    case TypeKind::bytes:
        return "bytes";
    case TypeKind::component:
        return "component";
    case TypeKind::ref:
        return "ref";
    case TypeKind::list:
        return "list";
    }
    return "unknown";
}

struct PathToken final {
    bool is_index;
    std::string_view text;
    std::uint64_t index;

    static PathToken name(std::string_view value) noexcept {
        return PathToken{false, value, UINT64_C(0)};
    }
    static PathToken position(std::uint64_t value) noexcept {
        return PathToken{true, {}, value};
    }
};

JsonPointer make_path(const std::vector<PathToken>& tokens) {
    JsonPointerBuilder builder;
    for (const PathToken& token : tokens) {
        if (token.is_index) {
            builder.append(token.index);
        } else {
            builder.append(token.text);
        }
    }
    return builder.snapshot();
}

enum class FrameKind : std::uint8_t { sequence, component, list };

struct ExpectationFrame final {
    FrameKind kind;
    std::uint32_t type_id;
    NodeIndex parent;
    NodeIndex last_child;
    std::uint64_t remaining;
    std::uint64_t next_index;
    std::size_t path_base_size;
    const TypeNode* repeated_type;
    const Component* component;
    bool sequence_many;
};

const TypeNode* expected_type(const ExpectationFrame& frame) noexcept {
    if (frame.kind == FrameKind::component) {
        return &frame.component->fields[static_cast<std::size_t>(
            frame.next_index)]
                    .type;
    }
    return frame.repeated_type;
}

void rebuild_path(std::vector<PathToken>& path,
                  const ExpectationFrame& frame) {
    path.resize(frame.path_base_size);
    if (frame.kind == FrameKind::sequence) {
        if (frame.sequence_many) {
            path.push_back(PathToken::position(frame.next_index));
        }
    } else if (frame.kind == FrameKind::component) {
        path.push_back(PathToken::name(
            frame.component
                ->fields[static_cast<std::size_t>(frame.next_index)]
                .id));
    } else {
        path.push_back(PathToken::position(frame.next_index));
    }
}

void close_completed(std::vector<ExpectationFrame>& frames,
                     std::vector<PathToken>& path) {
    while (!frames.empty() && frames.back().remaining == UINT64_C(0)) {
        frames.pop_back();
    }
    if (!frames.empty()) {
        rebuild_path(path, frames.back());
    }
}

void refresh_type_id(ExpectationFrame& frame,
                     const layout::RuntimeSchema& runtime_schema) noexcept {
    if (frame.remaining == UINT64_C(0)) {
        return;
    }
    frame.type_id = runtime_schema.runtime_id(*expected_type(frame));
}

void simulate_leaf(std::vector<ExpectationFrame>& frames,
                   std::vector<PathToken>& path,
                   const layout::RuntimeSchema& runtime_schema) {
    ExpectationFrame& frame = frames.back();
    --frame.remaining;
    ++frame.next_index;
    refresh_type_id(frame, runtime_schema);
    close_completed(frames, path);
}

std::uint64_t post_frame_count_after_values(
    const std::vector<ExpectationFrame>& frames,
    std::uint64_t consumed) noexcept {
    std::size_t post_count = frames.size();
    if (frames.empty() || consumed == UINT64_C(0) ||
        consumed > frames.back().remaining) {
        return static_cast<std::uint64_t>(post_count);
    }
    if (consumed < frames.back().remaining) {
        return static_cast<std::uint64_t>(post_count);
    }
    --post_count;
    while (post_count != 0U &&
           frames[post_count - 1U].remaining == UINT64_C(0)) {
        --post_count;
    }
    return static_cast<std::uint64_t>(post_count);
}

std::uint64_t structural_depth(
    const std::vector<ExpectationFrame>& frames) noexcept {
    std::uint64_t depth = UINT64_C(0);
    for (const ExpectationFrame& frame : frames) {
        if (frame.kind != FrameKind::sequence) {
            ++depth;
        }
    }
    return depth;
}

std::size_t fixed_width(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
        return 1U;
    case TypeKind::u16:
        return 2U;
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::f32:
        return 4U;
    case TypeKind::u8n:
    case TypeKind::u16n:
    case TypeKind::f64:
        return 8U;
    default:
        return 0U;
    }
}

ValueTag tag_for(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return ValueTag::boolean;
    case TypeKind::u8:
        return ValueTag::u8;
    case TypeKind::u16:
        return ValueTag::u16;
    case TypeKind::u32:
        return ValueTag::u32;
    case TypeKind::i32:
        return ValueTag::i32;
    case TypeKind::u8n:
        return ValueTag::u8n;
    case TypeKind::u16n:
        return ValueTag::u16n;
    case TypeKind::f32:
        return ValueTag::f32;
    case TypeKind::f64:
        return ValueTag::f64;
    case TypeKind::str:
        return ValueTag::str;
    case TypeKind::wstr:
        return ValueTag::wstr;
    case TypeKind::bytes:
        return ValueTag::bytes;
    case TypeKind::component:
        return ValueTag::component;
    case TypeKind::list:
        return ValueTag::list;
    case TypeKind::ref:
        break;
    }
    return ValueTag::null_value;
}

struct PendingScalar final {
    ValueTag tag;
    std::uint64_t bits;
    bool is_null;
};

}  // namespace

struct PayloadBuilder::State final {
    State(spec::CompiledSpec compiled,
          layout::RuntimeSchema compiled_runtime_schema,
          BuilderLimits builder_limits)
        : spec(std::move(compiled)),
          runtime_schema(std::move(compiled_runtime_schema)),
          limits(builder_limits) {}

    spec::CompiledSpec spec;
    layout::RuntimeSchema runtime_schema;
    BuilderLimits limits;
    ValueArena arena;
    std::vector<NodeIndex> entry_roots;
    std::vector<std::uint8_t> authored_entries;
    std::vector<ExpectationFrame> frames;
    std::vector<PathToken> path;
    std::uint64_t list_elements{UINT64_C(0)};
    std::uint64_t text_bytes{UINT64_C(0)};
    std::uint64_t opaque_bytes{UINT64_C(0)};
    std::uint64_t root_bytes{UINT64_C(0)};
    bool sealed{false};

    Result<void> initialize() {
        if (!checked_multiply(
                static_cast<std::uint64_t>(spec.resolved().entries().size()),
                logical_root_bytes, root_bytes)) {
            return Result<void>::failure(resource_error(
                JsonPointer{}.append("entries"), "total_builder_bytes",
                UINT64_MAX, limits.max_total_builder_bytes));
        }
        if (root_bytes > limits.max_total_builder_bytes) {
            return Result<void>::failure(resource_error(
                JsonPointer{}.append("entries"), "total_builder_bytes",
                root_bytes, limits.max_total_builder_bytes));
        }

        entry_roots.assign(spec.resolved().entries().size(),
                           invalid_node_index);
        authored_entries.assign(spec.resolved().entries().size(), UINT8_C(0));

        return Result<void>::success();
    }

    std::uint32_t runtime_id(const TypeNode& type) const noexcept {
        return runtime_schema.runtime_id(type);
    }

    JsonPointer current_path() const { return make_path(path); }

    Error resource_error(JsonPointer error_path,
                         const char* resource,
                         std::uint64_t actual,
                         std::uint64_t limit) const {
        return Error::from_details(
            FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT, std::move(error_path),
            "Payload builder resource limit exceeded",
            details({
                JsonValue::Member{"actual",
                                  JsonValue{std::to_string(actual)}},
                JsonValue::Member{"limit",
                                  JsonValue{std::to_string(limit)}},
                JsonValue::Member{"resource", JsonValue{resource}},
            }));
    }

    Result<void> check_state() const {
        if (sealed) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_BUILDER_STATE, JsonPointer{},
                "Payload builder is already frozen", "builder_frozen"));
        }
        return Result<void>::success();
    }

    Result<void> require_expectation() const {
        if (frames.empty()) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_BUILDER_STATE, JsonPointer{},
                "Payload builder has no pending value expectation",
                "no_pending_value"));
        }
        return Result<void>::success();
    }

    Error type_mismatch(TypeKind actual_kind,
                        std::string_view operation) const {
        const TypeNode* const expected = expected_type(frames.back());
        (void)actual_kind;
        return Error::from_details(
            FDB_PAYLOAD_E_TYPE_MISMATCH, current_path(),
            "Payload builder operation does not match the expected type",
            details({
                JsonValue::Member{"actual_operation",
                                  JsonValue{std::string(operation)}},
                JsonValue::Member{"expected_kind",
                                  JsonValue{type_name(expected->kind)}},
            }));
    }

    std::uint64_t total_bytes(std::uint64_t node_count,
                              std::uint64_t storage_count,
                              std::uint64_t frame_count) const noexcept {
        std::uint64_t node_bytes = UINT64_MAX;
        std::uint64_t frame_bytes = UINT64_MAX;
        std::uint64_t total = UINT64_MAX;
        if (!checked_multiply(node_count, logical_node_bytes, node_bytes) ||
            !checked_multiply(frame_count, logical_frame_bytes,
                              frame_bytes) ||
            !checked_add(root_bytes, node_bytes, total) ||
            !checked_add(total, storage_count, total) ||
            !checked_add(total, frame_bytes, total)) {
            return UINT64_MAX;
        }
        return total;
    }

    Result<void> check_growth(std::uint64_t added_nodes,
                              std::uint64_t added_storage,
                              std::uint64_t post_frame_count,
                              const JsonPointer& error_path) const {
        std::uint64_t post_nodes = UINT64_MAX;
        std::uint64_t post_storage = UINT64_MAX;
        if (!checked_add(static_cast<std::uint64_t>(arena.nodes_.size()),
                         added_nodes, post_nodes) ||
            !checked_add(
                static_cast<std::uint64_t>(arena.byte_storage_.size()),
                added_storage, post_storage)) {
            return Result<void>::failure(resource_error(
                error_path, "total_builder_bytes", UINT64_MAX,
                limits.max_total_builder_bytes));
        }
        if (post_nodes > limits.max_value_nodes) {
            return Result<void>::failure(resource_error(
                error_path, "value_nodes", post_nodes,
                limits.max_value_nodes));
        }
        const std::uint64_t node_capacity = static_cast<std::uint64_t>(
            arena.nodes_.max_size());
        if (post_nodes > node_capacity) {
            return Result<void>::failure(resource_error(
                error_path, "value_nodes", post_nodes, node_capacity));
        }
        const std::uint64_t storage_capacity = static_cast<std::uint64_t>(
            arena.byte_storage_.max_size());
        if (post_storage > storage_capacity) {
            return Result<void>::failure(resource_error(
                error_path, "byte_storage", post_storage,
                storage_capacity));
        }
        const std::uint64_t total =
            total_bytes(post_nodes, post_storage, post_frame_count);
        if (total > limits.max_total_builder_bytes) {
            return Result<void>::failure(resource_error(
                error_path, "total_builder_bytes", total,
                limits.max_total_builder_bytes));
        }
        return Result<void>::success();
    }

    Result<void> reserve(std::uint64_t added_nodes,
                         std::uint64_t added_storage,
                         std::uint64_t added_frames,
                         std::uint64_t added_path_tokens,
                         const JsonPointer& error_path) {
        const auto has_capacity = [](std::size_t current,
                                     std::uint64_t added,
                                     std::size_t maximum) noexcept {
            return current <= maximum &&
                   added <= static_cast<std::uint64_t>(maximum - current);
        };
        if (!has_capacity(arena.nodes_.size(), added_nodes,
                          arena.nodes_.max_size())) {
            return Result<void>::failure(resource_error(
                error_path, "value_nodes", UINT64_MAX,
                static_cast<std::uint64_t>(arena.nodes_.max_size())));
        }
        if (!has_capacity(arena.byte_storage_.size(), added_storage,
                          arena.byte_storage_.max_size())) {
            return Result<void>::failure(resource_error(
                error_path, "byte_storage", UINT64_MAX,
                static_cast<std::uint64_t>(arena.byte_storage_.max_size())));
        }
        if (!has_capacity(frames.size(), added_frames, frames.max_size())) {
            return Result<void>::failure(resource_error(
                error_path, "nesting_depth", UINT64_MAX,
                static_cast<std::uint64_t>(frames.max_size())));
        }
        if (!has_capacity(path.size(), added_path_tokens, path.max_size())) {
            return Result<void>::failure(resource_error(
                error_path, "path_tokens", UINT64_MAX,
                static_cast<std::uint64_t>(path.max_size())));
        }
        arena.nodes_.reserve(arena.nodes_.size() +
                             static_cast<std::size_t>(added_nodes));
        arena.byte_storage_.reserve(
            arena.byte_storage_.size() +
            static_cast<std::size_t>(added_storage));
        frames.reserve(frames.size() +
                       static_cast<std::size_t>(added_frames));
        path.reserve(path.size() +
                     static_cast<std::size_t>(added_path_tokens));
        return Result<void>::success();
    }

    NodeIndex append_node(ValueNode node) {
        const NodeIndex index =
            static_cast<NodeIndex>(arena.nodes_.size());
        arena.nodes_.push_back(node);
        return index;
    }

    void link_child(ExpectationFrame& frame, NodeIndex child) noexcept {
        ValueNode& parent =
            arena.nodes_[static_cast<std::size_t>(frame.parent)];
        if (frame.last_child == invalid_node_index) {
            parent.first_child = child;
        } else {
            arena.nodes_[static_cast<std::size_t>(frame.last_child)]
                .next_sibling = child;
        }
        frame.last_child = child;
        ++parent.child_count;
    }

    void commit_leaf(ValueNode node) {
        ExpectationFrame& frame = frames.back();
        const NodeIndex child = append_node(node);
        link_child(frame, child);
        --frame.remaining;
        ++frame.next_index;
        refresh_type_id(frame, runtime_schema);
        close_completed(frames, path);
    }

    Result<void> add_leaf(ValueTag tag,
                          std::uint64_t bits_or_offset,
                          std::uint64_t byte_length) {
        auto growth = check_growth(
            UINT64_C(1), byte_length,
            post_frame_count_after_values(frames, UINT64_C(1)),
            current_path());
        if (!growth.has_value()) {
            return growth;
        }
        auto capacity = reserve(UINT64_C(1), byte_length, UINT64_C(0),
                                UINT64_C(0), current_path());
        if (!capacity.has_value()) {
            return capacity;
        }
        const std::uint32_t type_id = frames.back().type_id;
        commit_leaf(ValueNode{type_id, tag, {0U, 0U, 0U},
                              bits_or_offset, byte_length,
                              invalid_node_index, invalid_node_index,
                              UINT64_C(0)});
        return Result<void>::success();
    }
};

BuilderLimits default_builder_limits() noexcept {
    return BuilderLimits{UINT64_C(10000000), UINT64_C(10000000),
                         UINT64_C(1) << 30, UINT64_C(1) << 30,
                         UINT64_C(1024), UINT64_C(1) << 30};
}

PayloadBuilder::PayloadBuilder(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PayloadBuilder::PayloadBuilder(PayloadBuilder&&) noexcept = default;
PayloadBuilder& PayloadBuilder::operator=(PayloadBuilder&&) noexcept = default;
PayloadBuilder::~PayloadBuilder() = default;

PayloadBuilder::State* PayloadBuilder::state_pointer() noexcept {
    return state_.get();
}

const PayloadBuilder::State* PayloadBuilder::state_pointer() const noexcept {
    return state_.get();
}

Result<PayloadBuilder> PayloadBuilder::create(spec::CompiledSpec spec,
                                               BuilderLimits limits) {
    try {
        if (spec.profile() == spec::Profile::object_graph_v1) {
            return Result<PayloadBuilder>::failure(Error::from_details(
                FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE, JsonPointer{},
                "Payload runtime is unavailable for this profile",
                details({
                    JsonValue::Member{"profile",
                                      JsonValue{"object_graph.v1"}},
                    JsonValue::Member{
                        "reason",
                        JsonValue{"runtime_slice_not_implemented"}},
                })));
        }
        auto runtime_schema = layout::RuntimeSchema::compile(spec);
        if (!runtime_schema.has_value()) {
            return Result<PayloadBuilder>::failure(
                std::move(runtime_schema).error());
        }
        auto state = std::make_unique<State>(
            std::move(spec), std::move(runtime_schema).value(), limits);
        auto initialized = state->initialize();
        if (!initialized.has_value()) {
            return Result<PayloadBuilder>::failure(
                std::move(initialized).error());
        }
        return Result<PayloadBuilder>::success(
            PayloadBuilder(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Result<PayloadBuilder>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::begin_entry(std::uint32_t entry_index,
                                         std::uint64_t value_count) {
    try {
        return begin_entry_impl(entry_index, value_count);
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::begin_entry_impl(std::uint32_t entry_index,
                                              std::uint64_t value_count) {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return valid_state;
    }
    if (!state.frames.empty()) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_BUILDER_STATE, state.current_path(),
            "Current payload entry is incomplete", "entry_incomplete"));
    }
    if (entry_index >= state.spec.resolved().entries().size()) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
            JsonPointer{}.append("entries").append(entry_index),
            "Payload entry index is out of range", "entry_index"));
    }
    const Entry& entry = state.spec.resolved().entries()[entry_index];
    const JsonPointer entry_path =
        JsonPointer{}.append("entries").append(entry.id);
    if (state.authored_entries[entry_index] != UINT8_C(0)) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_BUILDER_STATE, entry_path,
            "Payload entry was already authored", "duplicate_entry"));
    }
    if (entry.cardinality == Cardinality::one && value_count != UINT64_C(1)) {
        return Result<void>::failure(Error::from_details(
            FDB_PAYLOAD_E_OUT_OF_RANGE, entry_path,
            "Cardinality-one entry requires exactly one value",
            details({
                JsonValue::Member{"actual",
                                  JsonValue{std::to_string(value_count)}},
                JsonValue::Member{"expected", JsonValue{"1"}},
                JsonValue::Member{"reason",
                                  JsonValue{"one_entry_value_count"}},
            })));
    }

    const std::uint64_t post_frames = value_count == UINT64_C(0)
                                          ? UINT64_C(0)
                                          : UINT64_C(1);
    auto growth = state.check_growth(UINT64_C(1), UINT64_C(0), post_frames,
                                     entry_path);
    if (!growth.has_value()) {
        return growth;
    }

    std::vector<PathToken> new_path;
    new_path.reserve(3U);
    new_path.push_back(PathToken::name("entries"));
    new_path.push_back(PathToken::name(entry.id));
    if (entry.cardinality == Cardinality::many && value_count != UINT64_C(0)) {
        new_path.push_back(PathToken::position(UINT64_C(0)));
    }
    auto capacity = state.reserve(UINT64_C(1), UINT64_C(0), post_frames,
                                  UINT64_C(0), entry_path);
    if (!capacity.has_value()) {
        return capacity;
    }
    const NodeIndex root = state.append_node(ValueNode{
        state.runtime_id(entry.type), ValueTag::sequence, {0U, 0U, 0U},
        UINT64_C(0), UINT64_C(0), invalid_node_index, invalid_node_index,
        UINT64_C(0)});
    state.entry_roots[entry_index] = root;
    state.authored_entries[entry_index] = UINT8_C(1);
    state.path = std::move(new_path);
    if (value_count != UINT64_C(0)) {
        state.frames.push_back(ExpectationFrame{
            FrameKind::sequence, state.runtime_id(entry.type), root,
            invalid_node_index, value_count, UINT64_C(0), 2U, &entry.type,
            nullptr,
            entry.cardinality == Cardinality::many});
    }
    return Result<void>::success();
}

Result<void> PayloadBuilder::push_null() {
    try {
        return push_null_impl();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::push_null_impl() {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return valid_state;
    }
    auto expected = state.require_expectation();
    if (!expected.has_value()) {
        return expected;
    }
    const TypeNode& type = *expected_type(state.frames.back());
    if (!type.nullable) {
        return Result<void>::failure(Error::from_details(
            FDB_PAYLOAD_E_UNEXPECTED_NULL, state.current_path(),
            "Payload value is not nullable",
            details({
                JsonValue::Member{"kind", JsonValue{type_name(type.kind)}},
                JsonValue::Member{"reason",
                                  JsonValue{"non_nullable_value"}},
            })));
    }
    return state.add_leaf(ValueTag::null_value, UINT64_C(0), UINT64_C(0));
}

Result<void> PayloadBuilder::push_scalar_impl(TypeKind kind,
                                              ValueTag tag,
                                              std::uint64_t bits,
                                              std::string_view operation) {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return valid_state;
    }
    auto expected = state.require_expectation();
    if (!expected.has_value()) {
        return expected;
    }
    if (expected_type(state.frames.back())->kind != kind) {
        return Result<void>::failure(state.type_mismatch(kind, operation));
    }
    return state.add_leaf(tag, bits, UINT64_C(0));
}

Result<void> PayloadBuilder::push_bool(std::uint8_t value) {
    try {
        State& state = *state_;
        auto valid_state = state.check_state();
        if (!valid_state.has_value()) {
            return valid_state;
        }
        auto expected = state.require_expectation();
        if (!expected.has_value()) {
            return expected;
        }
        if (expected_type(state.frames.back())->kind != TypeKind::boolean) {
            return Result<void>::failure(
                state.type_mismatch(TypeKind::boolean, "push_bool"));
        }
        if (value > UINT8_C(1)) {
            return Result<void>::failure(Error::from_details(
                FDB_PAYLOAD_E_OUT_OF_RANGE, state.current_path(),
                "Payload Boolean byte must be zero or one",
                details({
                    JsonValue::Member{"kind", JsonValue{"bool"}},
                    JsonValue::Member{"reason",
                                      JsonValue{"invalid_boolean_byte"}},
                })));
        }
        return state.add_leaf(ValueTag::boolean, value, UINT64_C(0));
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

#define FASTDB_PAYLOAD_SCALAR_METHOD(method, cpp_type, type_kind, value_tag)  \
    Result<void> PayloadBuilder::method(cpp_type value) {                    \
        try {                                                                \
            std::uint64_t bits = UINT64_C(0);                                \
            if constexpr (sizeof(cpp_type) == sizeof(std::uint64_t)) {       \
                std::memcpy(&bits, &value, sizeof(value));                    \
            } else {                                                         \
                using Unsigned = std::make_unsigned_t<cpp_type>;             \
                Unsigned copied = {};                                        \
                std::memcpy(&copied, &value, sizeof(value));                  \
                bits = static_cast<std::uint64_t>(copied);                    \
            }                                                                \
            return push_scalar_impl(type_kind, value_tag, bits, #method);    \
        } catch (const std::bad_alloc&) {                                     \
            return Result<void>::failure(allocation_error());                \
        }                                                                    \
    }

FASTDB_PAYLOAD_SCALAR_METHOD(push_u8, std::uint8_t, TypeKind::u8,
                             ValueTag::u8)
FASTDB_PAYLOAD_SCALAR_METHOD(push_u16, std::uint16_t, TypeKind::u16,
                             ValueTag::u16)
FASTDB_PAYLOAD_SCALAR_METHOD(push_u32, std::uint32_t, TypeKind::u32,
                             ValueTag::u32)
FASTDB_PAYLOAD_SCALAR_METHOD(push_i32, std::int32_t, TypeKind::i32,
                             ValueTag::i32)
FASTDB_PAYLOAD_SCALAR_METHOD(push_f32_bits, std::uint32_t, TypeKind::f32,
                             ValueTag::f32)
FASTDB_PAYLOAD_SCALAR_METHOD(push_f64_bits, std::uint64_t, TypeKind::f64,
                             ValueTag::f64)

#undef FASTDB_PAYLOAD_SCALAR_METHOD

Result<void> PayloadBuilder::push_normalized_impl(TypeKind kind,
                                                  ValueTag tag,
                                                  std::uint64_t bits,
                                                  std::string_view operation) {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return valid_state;
    }
    auto expected = state.require_expectation();
    if (!expected.has_value()) {
        return expected;
    }
    const TypeNode& type = *expected_type(state.frames.back());
    if (type.kind != kind) {
        return Result<void>::failure(state.type_mismatch(kind, operation));
    }
    auto range = layout::validate_normalized_input(
        bits, type.minimum, type.maximum, type_name(kind),
        state.current_path());
    if (!range.has_value()) {
        return range;
    }
    return state.add_leaf(tag, bits, UINT64_C(0));
}

Result<void> PayloadBuilder::push_u8n_bits(std::uint64_t binary64_bits) {
    try {
        return push_normalized_impl(TypeKind::u8n, ValueTag::u8n,
                                    binary64_bits, "push_u8n_bits");
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::push_u16n_bits(std::uint64_t binary64_bits) {
    try {
        return push_normalized_impl(TypeKind::u16n, ValueTag::u16n,
                                    binary64_bits, "push_u16n_bits");
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::push_storage_impl(TypeKind kind,
                                               ValueTag tag,
                                               const std::uint8_t* bytes,
                                               std::uint64_t byte_count,
                                               bool text,
                                               std::string_view operation) {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return valid_state;
    }
    auto expected = state.require_expectation();
    if (!expected.has_value()) {
        return expected;
    }
    if (expected_type(state.frames.back())->kind != kind) {
        return Result<void>::failure(state.type_mismatch(kind, operation));
    }
    if (bytes == nullptr && byte_count != UINT64_C(0)) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_INVALID_ARGUMENT, state.current_path(),
            "Payload input span pointer is null", "null_nonempty_span"));
    }
    if (!layout::input_span_is_addressable(byte_count)) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, state.current_path(),
            "Payload input span length cannot be represented",
            "input_span_length_overflow"));
    }
    std::uint64_t updated = UINT64_MAX;
    const std::uint64_t current = text ? state.text_bytes : state.opaque_bytes;
    const std::uint64_t limit =
        text ? state.limits.max_text_bytes : state.limits.max_opaque_bytes;
    if (!checked_add(current, byte_count, updated) || updated > limit) {
        return Result<void>::failure(state.resource_error(
            state.current_path(), text ? "text_bytes" : "opaque_bytes",
            updated, limit));
    }

    const std::uint64_t offset =
        static_cast<std::uint64_t>(state.arena.byte_storage_.size());
    auto added = state.add_leaf(tag, offset, byte_count);
    if (!added.has_value()) {
        return added;
    }
    if (byte_count != UINT64_C(0)) {
        state.arena.byte_storage_.insert(
            state.arena.byte_storage_.end(), bytes,
            bytes + static_cast<std::ptrdiff_t>(byte_count));
    }
    if (text) {
        state.text_bytes = updated;
    } else {
        state.opaque_bytes = updated;
    }
    return Result<void>::success();
}

Result<void> PayloadBuilder::push_str(std::string_view utf8) {
    try {
        State& state = *state_;
        auto valid_state = state.check_state();
        if (!valid_state.has_value()) {
            return valid_state;
        }
        auto expected = state.require_expectation();
        if (!expected.has_value()) {
            return expected;
        }
        if (expected_type(state.frames.back())->kind != TypeKind::str) {
            return Result<void>::failure(
                state.type_mismatch(TypeKind::str, "push_str"));
        }
        if (!layout::input_span_is_addressable(
                static_cast<std::uint64_t>(utf8.size()))) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW,
                state.current_path(),
                "Payload input span length cannot be represented",
                "input_span_length_overflow"));
        }
        auto valid = layout::validate_utf8(utf8, state.current_path());
        if (!valid.has_value()) {
            return valid;
        }
        return push_storage_impl(
            TypeKind::str, ValueTag::str,
            reinterpret_cast<const std::uint8_t*>(utf8.data()),
            static_cast<std::uint64_t>(utf8.size()), true, "push_str");
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::push_wstr(const std::uint16_t* units,
                                       std::uint64_t unit_count) {
    try {
        State& state = *state_;
        auto valid_state = state.check_state();
        if (!valid_state.has_value()) {
            return valid_state;
        }
        auto expected = state.require_expectation();
        if (!expected.has_value()) {
            return expected;
        }
        if (expected_type(state.frames.back())->kind != TypeKind::wstr) {
            return Result<void>::failure(
                state.type_mismatch(TypeKind::wstr, "push_wstr"));
        }
        if (units == nullptr && unit_count != UINT64_C(0)) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INVALID_ARGUMENT, state.current_path(),
                "Payload UTF-16 input pointer is null",
                "null_nonempty_span"));
        }
        std::uint64_t byte_count = UINT64_MAX;
        if (!checked_multiply(unit_count, UINT64_C(2), byte_count)) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW,
                state.current_path(), "Payload UTF-16 length overflowed",
                "input_span_length_overflow"));
        }
        if (!layout::input_span_is_addressable(byte_count)) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW,
                state.current_path(),
                "Payload UTF-16 input span cannot be addressed",
                "input_span_length_overflow"));
        }
        auto valid =
            layout::validate_utf16(units, unit_count, state.current_path());
        if (!valid.has_value()) {
            return valid;
        }
        std::uint64_t updated = UINT64_MAX;
        if (!checked_add(state.text_bytes, byte_count, updated) ||
            updated > state.limits.max_text_bytes) {
            return Result<void>::failure(state.resource_error(
                state.current_path(), "text_bytes", updated,
                state.limits.max_text_bytes));
        }
        auto growth = state.check_growth(
            UINT64_C(1), byte_count,
            post_frame_count_after_values(state.frames, UINT64_C(1)),
            state.current_path());
        if (!growth.has_value()) {
            return growth;
        }
        auto capacity = state.reserve(UINT64_C(1), byte_count, UINT64_C(0),
                                      UINT64_C(0), state.current_path());
        if (!capacity.has_value()) {
            return capacity;
        }
        const std::uint64_t offset =
            static_cast<std::uint64_t>(state.arena.byte_storage_.size());
        const std::uint32_t type_id = state.frames.back().type_id;
        state.commit_leaf(ValueNode{
            type_id, ValueTag::wstr, {0U, 0U, 0U}, offset,
            byte_count, invalid_node_index, invalid_node_index, UINT64_C(0)});
        layout::append_utf16le(state.arena.byte_storage_, units, unit_count);
        state.text_bytes = updated;
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::push_bytes(const std::uint8_t* bytes,
                                        std::uint64_t byte_count) {
    try {
        return push_storage_impl(TypeKind::bytes, ValueTag::bytes, bytes,
                                 byte_count, false, "push_bytes");
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::begin_component() {
    try {
        return begin_component_impl();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::begin_component_impl() {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return valid_state;
    }
    auto expected = state.require_expectation();
    if (!expected.has_value()) {
        return expected;
    }
    const TypeNode& type = *expected_type(state.frames.back());
    if (type.kind != TypeKind::component) {
        return Result<void>::failure(
            state.type_mismatch(TypeKind::component, "begin_component"));
    }
    const Component& component =
        state.spec.resolved().components()[type.resolved_component_index];
    const bool has_fields = !component.fields.empty();
    const std::uint64_t post_depth =
        structural_depth(state.frames) + UINT64_C(1);
    if (post_depth > state.limits.max_nesting_depth) {
        return Result<void>::failure(state.resource_error(
            state.current_path(), "nesting_depth", post_depth,
            state.limits.max_nesting_depth));
    }

    const std::uint64_t post_frames =
        has_fields
            ? static_cast<std::uint64_t>(state.frames.size()) + UINT64_C(1)
            : post_frame_count_after_values(state.frames, UINT64_C(1));
    auto growth = state.check_growth(
        UINT64_C(1), UINT64_C(0), post_frames, state.current_path());
    if (!growth.has_value()) {
        return growth;
    }
    auto capacity = state.reserve(
        UINT64_C(1), UINT64_C(0),
        has_fields ? UINT64_C(1) : UINT64_C(0),
        has_fields ? UINT64_C(1) : UINT64_C(0), state.current_path());
    if (!capacity.has_value()) {
        return capacity;
    }
    ExpectationFrame& parent = state.frames.back();
    const NodeIndex node = state.append_node(ValueNode{
        state.frames.back().type_id, ValueTag::component, {0U, 0U, 0U},
        UINT64_C(0), UINT64_C(0), invalid_node_index, invalid_node_index,
        UINT64_C(0)});
    state.link_child(parent, node);
    --parent.remaining;
    ++parent.next_index;
    refresh_type_id(parent, state.runtime_schema);
    if (has_fields) {
        const std::size_t base = state.path.size();
        state.frames.push_back(ExpectationFrame{
            FrameKind::component,
            state.runtime_id(component.fields.front().type), node,
            invalid_node_index,
            static_cast<std::uint64_t>(component.fields.size()), UINT64_C(0),
            base, nullptr, &component, false});
        state.path.push_back(PathToken::name(component.fields.front().id));
    } else {
        close_completed(state.frames, state.path);
    }
    return Result<void>::success();
}

Result<void> PayloadBuilder::begin_list(std::uint64_t item_count) {
    try {
        return begin_list_impl(item_count);
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::begin_list_impl(std::uint64_t item_count) {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return valid_state;
    }
    auto expected = state.require_expectation();
    if (!expected.has_value()) {
        return expected;
    }
    const TypeNode& type = *expected_type(state.frames.back());
    if (type.kind != TypeKind::list || type.items == nullptr) {
        return Result<void>::failure(
            state.type_mismatch(TypeKind::list, "begin_list"));
    }
    std::uint64_t post_elements = UINT64_MAX;
    if (!checked_add(state.list_elements, item_count, post_elements) ||
        post_elements > state.limits.max_list_elements) {
        return Result<void>::failure(state.resource_error(
            state.current_path(), "list_elements", post_elements,
            state.limits.max_list_elements));
    }
    const bool has_items = item_count != UINT64_C(0);
    const std::uint64_t post_depth =
        structural_depth(state.frames) + UINT64_C(1);
    if (post_depth > state.limits.max_nesting_depth) {
        return Result<void>::failure(state.resource_error(
            state.current_path(), "nesting_depth", post_depth,
            state.limits.max_nesting_depth));
    }

    const std::uint64_t post_frames =
        has_items
            ? static_cast<std::uint64_t>(state.frames.size()) + UINT64_C(1)
            : post_frame_count_after_values(state.frames, UINT64_C(1));
    auto growth = state.check_growth(
        UINT64_C(1), UINT64_C(0), post_frames, state.current_path());
    if (!growth.has_value()) {
        return growth;
    }
    auto capacity = state.reserve(
        UINT64_C(1), UINT64_C(0),
        has_items ? UINT64_C(1) : UINT64_C(0),
        has_items ? UINT64_C(1) : UINT64_C(0), state.current_path());
    if (!capacity.has_value()) {
        return capacity;
    }
    ExpectationFrame& parent = state.frames.back();
    const NodeIndex node = state.append_node(ValueNode{
        state.frames.back().type_id, ValueTag::list, {0U, 0U, 0U},
        UINT64_C(0),
        UINT64_C(0), invalid_node_index, invalid_node_index, UINT64_C(0)});
    state.link_child(parent, node);
    --parent.remaining;
    ++parent.next_index;
    refresh_type_id(parent, state.runtime_schema);
    if (has_items) {
        const std::size_t base = state.path.size();
        state.frames.push_back(ExpectationFrame{
            FrameKind::list, state.runtime_id(*type.items), node,
            invalid_node_index, item_count, UINT64_C(0), base,
            type.items.get(), nullptr, false});
        state.path.push_back(PathToken::position(UINT64_C(0)));
    } else {
        close_completed(state.frames, state.path);
    }
    state.list_elements = post_elements;
    return Result<void>::success();
}

Result<void> PayloadBuilder::push_fixed_run(const FixedRun& run) {
    try {
        return push_fixed_run_impl(run);
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(allocation_error());
    }
}

Result<void> PayloadBuilder::push_fixed_run_impl(const FixedRun& run) {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return valid_state;
    }
    auto pending_expectation = state.require_expectation();
    if (!pending_expectation.has_value()) {
        return pending_expectation;
    }
    if (run.count == UINT64_C(0)) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_INVALID_ARGUMENT, state.current_path(),
            "Fixed run count must be non-zero", "fixed_run_zero_count"));
    }
    const TypeNode& first_type = *expected_type(state.frames.back());
    const std::size_t width = fixed_width(first_type.kind);
    if (width == 0U) {
        return Result<void>::failure(
            state.type_mismatch(first_type.kind, "push_fixed_run"));
    }
    if (run.data == nullptr) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_INVALID_ARGUMENT, state.current_path(),
            "Fixed run data pointer is null", "fixed_run_null_data"));
    }
    if (run.validity == nullptr &&
        (run.validity_byte_length != UINT64_C(0) ||
         run.validity_bit_offset != UINT64_C(0))) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_INVALID_ARGUMENT, state.current_path(),
            "Null fixed-run validity pointer requires zero length and offset",
            "fixed_run_null_validity"));
    }
    const std::uint64_t stride =
        run.stride_bytes == UINT64_C(0)
            ? static_cast<std::uint64_t>(width)
            : run.stride_bytes;
    if (stride < static_cast<std::uint64_t>(width)) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_INVALID_ARGUMENT, state.current_path(),
            "Fixed run stride is smaller than the scalar width",
            "fixed_run_stride_too_small"));
    }
    std::uint64_t last_offset = UINT64_MAX;
    std::uint64_t required_data = UINT64_MAX;
    if (!checked_multiply(run.count - UINT64_C(1), stride, last_offset) ||
        !checked_add(last_offset, static_cast<std::uint64_t>(width),
                     required_data)) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, state.current_path(),
            "Fixed run data span arithmetic overflowed",
            "fixed_run_data_span_overflow"));
    }
    if (required_data > run.data_byte_length) {
        return Result<void>::failure(Error::from_details(
            FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS, state.current_path(),
            "Fixed run data span is shorter than declared elements",
            details({
                JsonValue::Member{
                    "available",
                    JsonValue{std::to_string(run.data_byte_length)}},
                JsonValue::Member{"reason",
                                  JsonValue{"fixed_run_data_too_short"}},
                JsonValue::Member{
                    "required",
                    JsonValue{std::to_string(required_data)}},
            })));
    }
    if (!layout::input_span_is_addressable(required_data)) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, state.current_path(),
            "Fixed run data span cannot be addressed on this platform",
            "fixed_run_data_span_overflow"));
    }
    if (run.validity != nullptr) {
        std::uint64_t bit_end = UINT64_MAX;
        std::uint64_t rounded = UINT64_MAX;
        if (!checked_add(run.validity_bit_offset, run.count, bit_end) ||
            !checked_add(bit_end, UINT64_C(7), rounded)) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW,
                state.current_path(),
                "Fixed run validity span arithmetic overflowed",
                "fixed_run_validity_span_overflow"));
        }
        const std::uint64_t required_validity = rounded / UINT64_C(8);
        if (required_validity > run.validity_byte_length) {
            return Result<void>::failure(Error::from_details(
                FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS,
                state.current_path(),
                "Fixed run validity span is shorter than declared elements",
                details({
                    JsonValue::Member{
                        "available",
                        JsonValue{std::to_string(
                            run.validity_byte_length)}},
                    JsonValue::Member{
                        "reason",
                        JsonValue{"fixed_run_validity_too_short"}},
                    JsonValue::Member{
                        "required",
                        JsonValue{std::to_string(required_validity)}},
                })));
        }
        if (!layout::input_span_is_addressable(required_validity)) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW,
                state.current_path(),
                "Fixed run validity span cannot be addressed on this platform",
                "fixed_run_validity_span_overflow"));
        }
    }
    if (run.count > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max())) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW, state.current_path(),
            "Fixed run count cannot be represented",
            "fixed_run_count_overflow"));
    }

    std::vector<PendingScalar> pending;
    if (run.count >
        static_cast<std::uint64_t>(pending.max_size())) {
        return Result<void>::failure(state.resource_error(
            state.current_path(), "fixed_run_values", run.count,
            static_cast<std::uint64_t>(pending.max_size())));
    }
    std::uint64_t post_frame_count =
        static_cast<std::uint64_t>(state.frames.size());
    if (run.count <= state.frames.back().remaining) {
        post_frame_count =
            post_frame_count_after_values(state.frames, run.count);
    }
    auto growth = state.check_growth(run.count, UINT64_C(0),
                                     post_frame_count,
                                     state.current_path());
    if (!growth.has_value()) {
        return growth;
    }

    std::vector<ExpectationFrame> simulated_frames = state.frames;
    std::vector<PathToken> simulated_path = state.path;
    pending.reserve(static_cast<std::size_t>(run.count));
    const std::uint32_t first_runtime_id = state.frames.back().type_id;
    for (std::uint64_t index = UINT64_C(0); index < run.count; ++index) {
        if (simulated_frames.empty()) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_TYPE_MISMATCH, make_path(simulated_path),
                "Fixed run exceeds the pending homogeneous expectation",
                "fixed_run_expectation_exhausted"));
        }
        const TypeNode& type = *expected_type(simulated_frames.back());
        if (simulated_frames.back().type_id != first_runtime_id ||
            fixed_width(type.kind) != width) {
            return Result<void>::failure(Error::from_details(
                FDB_PAYLOAD_E_TYPE_MISMATCH, make_path(simulated_path),
                "Fixed run crossed a different value expectation",
                details({
                    JsonValue::Member{"actual_operation",
                                      JsonValue{"push_fixed_run"}},
                    JsonValue::Member{"expected_kind",
                                      JsonValue{type_name(type.kind)}},
                })));
        }

        bool present = true;
        if (run.validity != nullptr) {
            const std::uint64_t bit = run.validity_bit_offset + index;
            const std::uint8_t byte = run.validity[static_cast<std::ptrdiff_t>(
                bit / UINT64_C(8))];
            present = ((byte >> (bit % UINT64_C(8))) & UINT8_C(1)) !=
                      UINT8_C(0);
        }
        if (!present) {
            if (!type.nullable) {
                return Result<void>::failure(Error::from_details(
                    FDB_PAYLOAD_E_UNEXPECTED_NULL,
                    make_path(simulated_path),
                    "Fixed run null bit targets a non-nullable value",
                    details({
                        JsonValue::Member{"kind",
                                          JsonValue{type_name(type.kind)}},
                        JsonValue::Member{
                            "reason", JsonValue{"non_nullable_value"}},
                    })));
            }
            pending.push_back(
                PendingScalar{ValueTag::null_value, UINT64_C(0), true});
            simulate_leaf(simulated_frames, simulated_path,
                          state.runtime_schema);
            continue;
        }

        const std::uint64_t offset = index * stride;
        const std::uint64_t bits = layout::load_native_fixed_scalar_bits(
            type.kind,
            run.data + static_cast<std::ptrdiff_t>(offset));
        if (type.kind == TypeKind::boolean && bits > UINT64_C(1)) {
            return Result<void>::failure(Error::from_details(
                FDB_PAYLOAD_E_OUT_OF_RANGE, make_path(simulated_path),
                "Payload Boolean byte must be zero or one",
                details({
                    JsonValue::Member{"kind", JsonValue{"bool"}},
                    JsonValue::Member{"reason",
                                      JsonValue{"invalid_boolean_byte"}},
                })));
        }
        if (type.kind == TypeKind::u8n || type.kind == TypeKind::u16n) {
            auto range = layout::validate_normalized_input(
                bits, type.minimum, type.maximum, type_name(type.kind),
                make_path(simulated_path));
            if (!range.has_value()) {
                return range;
            }
        }
        pending.push_back(PendingScalar{tag_for(type.kind), bits, false});
        simulate_leaf(simulated_frames, simulated_path,
                      state.runtime_schema);
    }

    auto capacity = state.reserve(run.count, UINT64_C(0), UINT64_C(0),
                                  UINT64_C(0), state.current_path());
    if (!capacity.has_value()) {
        return capacity;
    }
    for (const PendingScalar& scalar : pending) {
        const std::uint32_t type_id = state.frames.back().type_id;
        state.commit_leaf(ValueNode{
            type_id, scalar.tag, {0U, 0U, 0U}, scalar.bits,
            UINT64_C(0), invalid_node_index, invalid_node_index,
            UINT64_C(0)});
    }
    return Result<void>::success();
}

Result<LogicalPayload> PayloadBuilder::freeze() {
    try {
        return freeze_impl();
    } catch (const std::bad_alloc&) {
        return Result<LogicalPayload>::failure(allocation_error());
    }
}

Result<LogicalPayload> PayloadBuilder::freeze_impl() {
    State& state = *state_;
    auto valid_state = state.check_state();
    if (!valid_state.has_value()) {
        return Result<LogicalPayload>::failure(
            std::move(valid_state).error());
    }
    if (!state.frames.empty()) {
        const ExpectationFrame& frame = state.frames.back();
        std::uint32_t code = FDB_PAYLOAD_E_BUILDER_STATE;
        const char* reason = "incomplete_list";
        const char* message = "Payload list is incomplete";
        if (frame.kind == FrameKind::component) {
            code = FDB_PAYLOAD_E_MISSING_FIELD;
            reason = "field_not_authored";
            message = "Payload component field is missing";
        } else if (frame.kind == FrameKind::sequence) {
            code = FDB_PAYLOAD_E_MISSING_ENTRY;
            reason = "entry_incomplete";
            message = "Payload entry is incomplete";
        }
        return Result<LogicalPayload>::failure(simple_error(
            code, state.current_path(), message, reason));
    }
    const auto& entries = state.spec.resolved().entries();
    for (std::size_t index = 0U; index < entries.size(); ++index) {
        if (state.authored_entries[index] == UINT8_C(0)) {
            return Result<LogicalPayload>::failure(simple_error(
                FDB_PAYLOAD_E_MISSING_ENTRY,
                JsonPointer{}.append("entries").append(entries[index].id),
                "Payload entry was not authored", "entry_not_authored"));
        }
    }

    LogicalPayload payload(state.spec, std::move(state.arena),
                           std::move(state.entry_roots));
    state.sealed = true;
    return Result<LogicalPayload>::success(std::move(payload));
}

}  // namespace fastdb::payload::build
