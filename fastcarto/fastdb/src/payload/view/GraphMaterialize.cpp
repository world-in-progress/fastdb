#include "payload/view/Materialize.hpp"

#include "payload/json/JsonValue.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/TextEncoding.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fastdb::payload::view {
namespace {

using build::NodeIndex;
using build::ValueArena;
using build::ValueNode;
using build::ValueTag;
using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonPointerBuilder;
using json::JsonValue;
using spec::TypeKind;

Error materialize_error(std::uint32_t code,
                        const JsonPointer& path,
                        const char* message,
                        const char* reason) {
    return Error::from_details(
        code, path, message,
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{reason}}}));
}

Error allocation_error() {
    return materialize_error(
        FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
        "Portable payload materialization allocation failed",
        "allocation_failed");
}

Error internal_error(const JsonPointer& path, const char* reason) {
    return materialize_error(
        FDB_PAYLOAD_E_INTERNAL, path,
        "Portable payload materialization state is inconsistent", reason);
}

std::uint64_t size_as_u64(std::size_t size) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (size > static_cast<std::size_t>(UINT64_MAX)) {
            throw std::length_error("graph materialize size overflow");
        }
    }
    return static_cast<std::uint64_t>(size);
}

template <typename Container>
void require_growth(const Container& container,
                    std::size_t count,
                    const char* message) {
    if (count > container.max_size() - container.size()) {
        throw std::length_error(message);
    }
}

NodeIndex append_node(ValueArena& arena, ValueNode node) {
    require_growth(arena.nodes_, 1U,
                   "graph materialized node arena exhausted");
    const NodeIndex index = size_as_u64(arena.nodes_.size());
    arena.nodes_.push_back(node);
    return index;
}

std::uint64_t append_bytes(ValueArena& arena,
                           const std::uint8_t* bytes,
                           std::uint64_t byte_count) {
    const std::uint64_t offset = size_as_u64(arena.byte_storage_.size());
    if (byte_count == UINT64_C(0)) {
        return offset;
    }
    if (bytes == nullptr || byte_count > PTRDIFF_MAX ||
        byte_count > arena.byte_storage_.max_size() -
                         arena.byte_storage_.size()) {
        throw std::length_error("graph materialized byte arena exhausted");
    }
    arena.byte_storage_.insert(
        arena.byte_storage_.end(), bytes,
        bytes + static_cast<std::ptrdiff_t>(byte_count));
    return offset;
}

ValueNode empty_node() noexcept {
    return ValueNode{
        UINT32_MAX, ValueTag::null_value,
        {UINT8_C(0), UINT8_C(0), UINT8_C(0)},
        UINT64_C(0), UINT64_C(0), build::invalid_node_index,
        build::invalid_node_index, UINT64_C(0), UINT32_MAX, UINT32_C(0),
        UINT64_MAX};
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

bool scalar_kind(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::u8n:
    case TypeKind::u16n:
    case TypeKind::f32:
    case TypeKind::f64:
        return true;
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
    case TypeKind::component:
    case TypeKind::list:
    case TypeKind::ref:
        return false;
    }
    return false;
}

bool variable_kind(TypeKind kind) noexcept {
    return kind == TypeKind::str || kind == TypeKind::wstr ||
           kind == TypeKind::bytes;
}

bool container_tag(ValueTag tag) noexcept {
    return tag == ValueTag::sequence || tag == ValueTag::component ||
           tag == ValueTag::list || tag == ValueTag::object_record;
}

struct Coordinate final {
    std::uint32_t component_index{UINT32_MAX};
    std::uint64_t object_id{UINT64_MAX};
};

enum class CoordinateRole : std::uint8_t { none, identity, reference };

struct CoordinateObservation final {
    CoordinateRole role{CoordinateRole::none};
    bool present{false};
    Coordinate coordinate;
};

enum class CursorRole : std::uint8_t {
    value,
    sequence,
    object_record,
    identity_occurrence,
};

struct Cursor final {
    CursorRole role{CursorRole::value};
    EntrySequenceCursor entry{UINT32_C(0)};
    ValueCursor value{InlineValueCursor{
        UINT32_C(0), TypeKind::boolean, UINT64_C(0), false}};
    NodeIndex detached_node{build::invalid_node_index};
    Coordinate object;
};

struct Source final {
    bool backed{false};
    std::shared_ptr<PayloadOwnerState> owner;
    std::optional<AccessPin> pin;
    const PayloadIndex* index{nullptr};
    const std::uint8_t* bytes{nullptr};
    std::uint64_t byte_count{UINT64_C(0)};
    std::shared_ptr<const DetachedViewState> detached;
    std::shared_ptr<const layout::RuntimeSchema> runtime_schema;
};

struct WalkFrame final {
    Cursor cursor;
    std::uint64_t child_count{UINT64_C(0)};
    std::uint64_t next_child{UINT64_C(0)};
    JsonPointerBuilder::Mark path_mark{0U};
};

struct CopyFrame final {
    Cursor cursor;
    NodeIndex destination{build::invalid_node_index};
    std::uint64_t child_count{UINT64_C(0)};
    bool children_allocated{false};
    std::uint64_t next_child{UINT64_C(0)};
    JsonPointerBuilder::Mark path_mark{0U};
};

const ValueNode* detached_node(const Source& source,
                               const Cursor& cursor) noexcept {
    if (source.detached == nullptr ||
        cursor.detached_node >= source.detached->arena.nodes().size()) {
        return nullptr;
    }
    return &source.detached->arena.nodes()[
        static_cast<std::size_t>(cursor.detached_node)];
}

bool is_null_identity(const ValueNode& node) noexcept {
    return node.tag == ValueTag::null_value &&
           node.runtime_type_id == UINT32_MAX &&
           node.object_component_index != UINT32_MAX &&
           node.object_id == UINT64_MAX;
}

bool is_detached_reference(const Source& source,
                           const ValueNode& node) noexcept {
    if (node.tag == ValueTag::reference) {
        return true;
    }
    if (node.tag != ValueTag::null_value ||
        node.runtime_type_id == UINT32_MAX ||
        source.runtime_schema == nullptr) {
        return false;
    }
    const layout::RuntimeType* const type =
        source.runtime_schema->find_type(node.runtime_type_id);
    return type != nullptr && type->source != nullptr &&
           type->source->kind == TypeKind::ref &&
           type->storage_role == spec::StorageRole::reference_id;
}

Result<CoordinateObservation> observe_coordinate(
    const Source& source,
    const Cursor& cursor,
    const JsonPointerBuilder& path) {
    if (cursor.role == CursorRole::identity_occurrence) {
        const ValueNode* const node = detached_node(source, cursor);
        if (node == nullptr || node->tag != ValueTag::object_record) {
            return Result<CoordinateObservation>::failure(
                internal_error(path.snapshot(),
                               "detached_identity_root_invalid"));
        }
        return Result<CoordinateObservation>::success(
            CoordinateObservation{
                CoordinateRole::identity, true,
                Coordinate{node->object_component_index,
                           node->object_id}});
    }
    if (cursor.role != CursorRole::value) {
        return Result<CoordinateObservation>::success(
            CoordinateObservation{});
    }
    if (source.backed) {
        if (const auto* object =
                std::get_if<IdentityObjectCursor>(&cursor.value)) {
            return Result<CoordinateObservation>::success(
                CoordinateObservation{
                    CoordinateRole::identity, object->present,
                    Coordinate{object->component_index,
                               object->object_id}});
        }
        if (const auto* reference =
                std::get_if<RefCursor>(&cursor.value)) {
            return Result<CoordinateObservation>::success(
                CoordinateObservation{
                    CoordinateRole::reference, reference->present,
                    Coordinate{reference->target_component_index,
                               reference->object_id}});
        }
        return Result<CoordinateObservation>::success(
            CoordinateObservation{});
    }
    const ValueNode* const node = detached_node(source, cursor);
    if (node == nullptr) {
        return Result<CoordinateObservation>::failure(
            internal_error(path.snapshot(), "detached_node_missing"));
    }
    if (node->tag == ValueTag::object_root ||
        node->tag == ValueTag::object_record || is_null_identity(*node)) {
        return Result<CoordinateObservation>::success(
            CoordinateObservation{
                CoordinateRole::identity,
                node->tag != ValueTag::null_value,
                Coordinate{node->object_component_index,
                           node->object_id}});
    }
    if (is_detached_reference(source, *node)) {
        Coordinate coordinate;
        if (node->tag == ValueTag::reference) {
            coordinate = Coordinate{node->object_component_index,
                                    node->object_id};
        } else {
            const layout::RuntimeType* const type =
                source.runtime_schema->find_type(node->runtime_type_id);
            if (type == nullptr || type->source == nullptr) {
                return Result<CoordinateObservation>::failure(
                    internal_error(path.snapshot(),
                                   "detached_ref_type_missing"));
            }
            coordinate.component_index =
                type->source->resolved_component_index;
        }
        return Result<CoordinateObservation>::success(
            CoordinateObservation{
                CoordinateRole::reference,
                node->tag == ValueTag::reference, coordinate});
    }
    return Result<CoordinateObservation>::success(CoordinateObservation{});
}

Result<void> validate_coordinate(const Source& source,
                                 Coordinate coordinate,
                                 const JsonPointerBuilder& path) {
    if (source.runtime_schema == nullptr ||
        !source.runtime_schema->component_identity_bearing(
            coordinate.component_index)) {
        return Result<void>::failure(internal_error(
            path.snapshot(), "identity_component_invalid"));
    }
    if (source.backed) {
        if (source.index == nullptr) {
            return Result<void>::failure(
                internal_error(path.snapshot(), "source_index_missing"));
        }
        const auto pool = source.index->object_pool_metadata(
            coordinate.component_index);
        if (!pool.has_value() || coordinate.object_id >= pool->object_count) {
            return Result<void>::failure(internal_error(
                path.snapshot(), "source_object_coordinate_invalid"));
        }
        return Result<void>::success();
    }
    if (source.detached == nullptr ||
        source.detached->object_record_node(
            coordinate.component_index, coordinate.object_id) ==
            build::invalid_node_index) {
        return Result<void>::failure(internal_error(
            path.snapshot(), "detached_object_coordinate_invalid"));
    }
    return Result<void>::success();
}

Result<std::uint64_t> source_child_count(
    const Source& source,
    const Cursor& cursor,
    const JsonPointerBuilder& path) {
    if (cursor.role == CursorRole::identity_occurrence) {
        return Result<std::uint64_t>::success(UINT64_C(0));
    }
    if (source.backed) {
        if (source.index == nullptr) {
            return Result<std::uint64_t>::failure(
                internal_error(path.snapshot(), "source_index_missing"));
        }
        if (cursor.role == CursorRole::sequence) {
            return source.index->sequence_length(
                source.bytes, source.byte_count, cursor.entry);
        }
        if (cursor.role == CursorRole::object_record) {
            auto count = source.index->component_field_count(
                source.bytes, source.byte_count, cursor.value);
            return count.has_value()
                       ? Result<std::uint64_t>::success(count.value())
                       : Result<std::uint64_t>::failure(
                             std::move(count).error());
        }
        const InlineValueCursor* const inline_cursor =
            inline_value_cursor(cursor.value);
        if (inline_cursor == nullptr || !inline_cursor->present) {
            return Result<std::uint64_t>::success(UINT64_C(0));
        }
        if (inline_cursor->kind == TypeKind::component) {
            auto count = source.index->component_field_count(
                source.bytes, source.byte_count, cursor.value);
            return count.has_value()
                       ? Result<std::uint64_t>::success(count.value())
                       : Result<std::uint64_t>::failure(
                             std::move(count).error());
        }
        if (inline_cursor->kind == TypeKind::list) {
            return source.index->list_length(
                source.bytes, source.byte_count, cursor.value);
        }
        return Result<std::uint64_t>::success(UINT64_C(0));
    }
    const ValueNode* const node = detached_node(source, cursor);
    if (node == nullptr) {
        return Result<std::uint64_t>::failure(
            internal_error(path.snapshot(), "detached_node_missing"));
    }
    if (cursor.role == CursorRole::object_record) {
        if (node->tag != ValueTag::object_record) {
            return Result<std::uint64_t>::failure(internal_error(
                path.snapshot(), "detached_object_record_invalid"));
        }
        return Result<std::uint64_t>::success(node->child_count);
    }
    return Result<std::uint64_t>::success(
        node->tag == ValueTag::sequence ||
                node->tag == ValueTag::component ||
                node->tag == ValueTag::list
            ? node->child_count
            : UINT64_C(0));
}

Result<Cursor> source_child(const Source& source,
                            const Cursor& parent,
                            std::uint64_t index,
                            JsonPointerBuilder& path) {
    Cursor child;
    if (source.backed) {
        if (source.index == nullptr) {
            return Result<Cursor>::failure(
                internal_error(path.snapshot(), "source_index_missing"));
        }
        if (parent.role == CursorRole::sequence) {
            auto value = source.index->entry_value(
                source.bytes, source.byte_count, parent.entry, index);
            if (!value.has_value()) {
                return Result<Cursor>::failure(std::move(value).error());
            }
            child.value = value.value();
            path.append(index);
            return Result<Cursor>::success(std::move(child));
        }
        const bool component = parent.role == CursorRole::object_record ||
            (parent.role == CursorRole::value &&
             inline_value_cursor(parent.value) != nullptr &&
             inline_value_cursor(parent.value)->kind == TypeKind::component);
        if (component) {
            if (index > UINT32_MAX) {
                return Result<Cursor>::failure(internal_error(
                    path.snapshot(), "component_child_index_overflow"));
            }
            auto value = source.index->component_field(
                source.bytes, source.byte_count, parent.value,
                static_cast<std::uint32_t>(index));
            if (!value.has_value()) {
                return Result<Cursor>::failure(std::move(value).error());
            }
            child.value = value.value();
            path.append("fields");
            path.append(index);
            return Result<Cursor>::success(std::move(child));
        }
        auto value = source.index->list_item(
            source.bytes, source.byte_count, parent.value, index);
        if (!value.has_value()) {
            return Result<Cursor>::failure(std::move(value).error());
        }
        child.value = value.value();
        path.append(index);
        return Result<Cursor>::success(std::move(child));
    }
    const ValueNode* const node = detached_node(source, parent);
    if (node == nullptr || !container_tag(node->tag) ||
        index >= node->child_count) {
        return Result<Cursor>::failure(internal_error(
            path.snapshot(), "detached_child_index_invalid"));
    }
    const auto& nodes = source.detached->arena.nodes();
    if (node->first_child >= nodes.size() ||
        node->child_count > size_as_u64(nodes.size()) - node->first_child) {
        return Result<Cursor>::failure(internal_error(
            path.snapshot(), "detached_child_range_invalid"));
    }
    child.detached_node = node->first_child + index;
    const NodeIndex expected_next =
        index + UINT64_C(1) < node->child_count
            ? child.detached_node + UINT64_C(1)
            : build::invalid_node_index;
    if (nodes[static_cast<std::size_t>(child.detached_node)].next_sibling !=
        expected_next) {
        return Result<Cursor>::failure(internal_error(
            path.snapshot(), "detached_child_sequence_invalid"));
    }
    if (node->tag == ValueTag::component ||
        node->tag == ValueTag::object_record) {
        path.append("fields");
    }
    path.append(index);
    return Result<Cursor>::success(std::move(child));
}

Result<Cursor> object_cursor(const Source& source,
                             Coordinate coordinate,
                             const JsonPointerBuilder& path) {
    auto valid = validate_coordinate(source, coordinate, path);
    if (!valid.has_value()) {
        return Result<Cursor>::failure(std::move(valid).error());
    }
    Cursor cursor;
    cursor.role = CursorRole::object_record;
    cursor.object = coordinate;
    if (source.backed) {
        cursor.value = IdentityObjectCursor{
            coordinate.component_index, coordinate.object_id, true};
    } else {
        cursor.detached_node = source.detached->object_record_node(
            coordinate.component_index, coordinate.object_id);
    }
    return Result<Cursor>::success(std::move(cursor));
}

struct Discovery final {
    std::vector<std::unordered_set<std::uint64_t>> seen;
    std::vector<std::vector<std::uint64_t>> source_ids;
    std::vector<Coordinate> queue;
    std::size_t next{0U};
};

Result<void> enqueue_coordinate(
    const Source& source,
    const CoordinateObservation& observation,
    Discovery& discovery,
    const JsonPointerBuilder& path) {
    if (observation.role == CoordinateRole::none || !observation.present) {
        return Result<void>::success();
    }
    auto valid = validate_coordinate(source, observation.coordinate, path);
    if (!valid.has_value()) {
        return valid;
    }
    const std::size_t component =
        static_cast<std::size_t>(observation.coordinate.component_index);
    if (component >= discovery.seen.size() ||
        component >= discovery.source_ids.size()) {
        return Result<void>::failure(internal_error(
            path.snapshot(), "discovery_component_missing"));
    }
    auto& seen = discovery.seen[component];
    if (seen.size() == seen.max_size()) {
        throw std::length_error("graph discovery map exhausted");
    }
    const auto inserted = seen.insert(observation.coordinate.object_id);
    if (!inserted.second) {
        return Result<void>::success();
    }
    auto& ids = discovery.source_ids[component];
    require_growth(ids, 1U, "graph source-id list exhausted");
    require_growth(discovery.queue, 1U, "graph discovery queue exhausted");
    ids.push_back(observation.coordinate.object_id);
    discovery.queue.push_back(observation.coordinate);
    return Result<void>::success();
}

void observe_path_state(const JsonPointerBuilder& path,
                        const std::vector<WalkFrame>& stack,
                        MaterializeMetrics* metrics) {
    if (metrics == nullptr) {
        return;
    }
    const std::uint64_t path_bytes = size_as_u64(path.mark());
    const std::uint64_t frame_count = size_as_u64(stack.size());
    const std::uint64_t mark_size = static_cast<std::uint64_t>(
        sizeof(JsonPointerBuilder::Mark));
    const std::uint64_t mark_bytes =
        frame_count > UINT64_MAX / mark_size
            ? UINT64_MAX
            : frame_count * mark_size;
    const std::uint64_t retained =
        path_bytes > UINT64_MAX - mark_bytes
            ? UINT64_MAX
            : path_bytes + mark_bytes;
    metrics->peak_retained_diagnostic_path_bytes = std::max(
        metrics->peak_retained_diagnostic_path_bytes, retained);
}

Result<void> discover_tree(const Source& source,
                           Cursor root,
                           JsonPointerBuilder& path,
                           Discovery& discovery,
                           MaterializeMetrics* metrics) {
    auto observation = observe_coordinate(source, root, path);
    if (!observation.has_value()) {
        return Result<void>::failure(std::move(observation).error());
    }
    auto enqueued = enqueue_coordinate(
        source, observation.value(), discovery, path);
    if (!enqueued.has_value()) {
        return enqueued;
    }
    auto root_count = source_child_count(source, root, path);
    if (!root_count.has_value()) {
        return Result<void>::failure(std::move(root_count).error());
    }
    const JsonPointerBuilder::Mark root_mark = path.mark();
    std::vector<WalkFrame> stack;
    if (root_count.value() != UINT64_C(0)) {
        require_growth(stack, 1U, "graph traversal stack exhausted");
        stack.push_back(WalkFrame{
            std::move(root), root_count.value(), UINT64_C(0), root_mark});
        observe_path_state(path, stack, metrics);
    }
    while (!stack.empty()) {
        WalkFrame& frame = stack.back();
        if (path.mark() != frame.path_mark) {
            return Result<void>::failure(internal_error(
                path.snapshot(), "diagnostic_path_mark_mismatch"));
        }
        if (frame.next_child == frame.child_count) {
            stack.pop_back();
            path.rewind(stack.empty() ? root_mark : stack.back().path_mark);
            continue;
        }
        const std::uint64_t child_index = frame.next_child++;
        auto child = source_child(source, frame.cursor, child_index, path);
        if (!child.has_value()) {
            return Result<void>::failure(std::move(child).error());
        }
        auto child_observation = observe_coordinate(source, child.value(), path);
        if (!child_observation.has_value()) {
            return Result<void>::failure(
                std::move(child_observation).error());
        }
        enqueued = enqueue_coordinate(
            source, child_observation.value(), discovery, path);
        if (!enqueued.has_value()) {
            return enqueued;
        }
        auto count = source_child_count(source, child.value(), path);
        if (!count.has_value()) {
            return Result<void>::failure(std::move(count).error());
        }
        if (count.value() == UINT64_C(0)) {
            path.rewind(frame.path_mark);
            continue;
        }
        require_growth(stack, 1U, "graph traversal stack exhausted");
        stack.push_back(WalkFrame{
            std::move(child).value(), count.value(), UINT64_C(0),
            path.mark()});
        observe_path_state(path, stack, metrics);
    }
    return Result<void>::success();
}

Result<Coordinate> remap_coordinate(
    const std::vector<std::vector<std::uint64_t>>& source_ids,
    Coordinate source,
    const JsonPointerBuilder& path) {
    if (source.component_index >= source_ids.size()) {
        return Result<Coordinate>::failure(internal_error(
            path.snapshot(), "remap_component_missing"));
    }
    const auto& ids = source_ids[source.component_index];
    const auto found = std::lower_bound(ids.begin(), ids.end(),
                                        source.object_id);
    if (found == ids.end() || *found != source.object_id) {
        return Result<Coordinate>::failure(internal_error(
            path.snapshot(), "remap_object_missing"));
    }
    return Result<Coordinate>::success(Coordinate{
        source.component_index,
        size_as_u64(static_cast<std::size_t>(found - ids.begin()))});
}

Result<void> validate_detached_text(TypeKind kind,
                                    const std::uint8_t* bytes,
                                    std::uint64_t byte_count,
                                    const JsonPointer& path) {
    if (kind == TypeKind::str) {
        const char* const text = byte_count == UINT64_C(0)
                                     ? ""
                                     : reinterpret_cast<const char*>(bytes);
        return layout::validate_utf8(
            std::string_view{text, static_cast<std::size_t>(byte_count)},
            path);
    }
    if (kind == TypeKind::wstr) {
        return layout::validate_utf16le(bytes, byte_count, path);
    }
    return Result<void>::success();
}

struct AppendedNode final {
    NodeIndex destination;
    std::uint64_t child_count;
};

Result<AppendedNode> append_source_node(
    const Source& source,
    const Cursor& cursor,
    const std::vector<std::vector<std::uint64_t>>& source_ids,
    const JsonPointerBuilder& path,
    ValueArena& arena) {
    ValueNode node = empty_node();
    auto observation = observe_coordinate(source, cursor, path);
    if (!observation.has_value()) {
        return Result<AppendedNode>::failure(
            std::move(observation).error());
    }
    if (observation.value().role != CoordinateRole::none) {
        if (observation.value().role == CoordinateRole::reference) {
            if (source.backed) {
                const auto* const reference =
                    std::get_if<RefCursor>(&cursor.value);
                if (reference == nullptr) {
                    return Result<AppendedNode>::failure(internal_error(
                        path.snapshot(), "backed_ref_cursor_missing"));
                }
                node.runtime_type_id = reference->runtime_type_id;
            } else {
                const ValueNode* const source_node =
                    detached_node(source, cursor);
                if (source_node == nullptr) {
                    return Result<AppendedNode>::failure(internal_error(
                        path.snapshot(), "detached_ref_node_missing"));
                }
                node.runtime_type_id = source_node->runtime_type_id;
            }
        }
        if (!observation.value().present) {
            node.tag = ValueTag::null_value;
            if (observation.value().role == CoordinateRole::identity) {
                node.object_component_index =
                    observation.value().coordinate.component_index;
            }
            return Result<AppendedNode>::success(AppendedNode{
                append_node(arena, node), UINT64_C(0)});
        }
        auto remapped = remap_coordinate(
            source_ids, observation.value().coordinate, path);
        if (!remapped.has_value()) {
            return Result<AppendedNode>::failure(
                std::move(remapped).error());
        }
        node.tag = observation.value().role == CoordinateRole::identity
                       ? ValueTag::object_root
                       : ValueTag::reference;
        node.object_component_index = remapped.value().component_index;
        node.object_id = remapped.value().object_id;
        return Result<AppendedNode>::success(AppendedNode{
            append_node(arena, node), UINT64_C(0)});
    }

    auto child_count = source_child_count(source, cursor, path);
    if (!child_count.has_value()) {
        return Result<AppendedNode>::failure(
            std::move(child_count).error());
    }
    if (cursor.role == CursorRole::object_record) {
        auto remapped = remap_coordinate(source_ids, cursor.object, path);
        if (!remapped.has_value()) {
            return Result<AppendedNode>::failure(
                std::move(remapped).error());
        }
        node.tag = ValueTag::object_record;
        node.object_component_index = remapped.value().component_index;
        node.object_id = remapped.value().object_id;
        node.child_count = child_count.value();
        return Result<AppendedNode>::success(AppendedNode{
            append_node(arena, node), child_count.value()});
    }
    if (cursor.role == CursorRole::sequence) {
        if (source.backed) {
            auto slot = source.index->entry_slot(cursor.entry.entry_index);
            if (!slot.has_value()) {
                return Result<AppendedNode>::failure(
                    std::move(slot).error());
            }
            node.runtime_type_id = slot.value().runtime_type_id;
        } else {
            const ValueNode* const source_node =
                detached_node(source, cursor);
            if (source_node == nullptr ||
                source_node->tag != ValueTag::sequence) {
                return Result<AppendedNode>::failure(internal_error(
                    path.snapshot(), "detached_sequence_invalid"));
            }
            node.runtime_type_id = source_node->runtime_type_id;
        }
        node.tag = ValueTag::sequence;
        node.child_count = child_count.value();
        return Result<AppendedNode>::success(AppendedNode{
            append_node(arena, node), child_count.value()});
    }

    if (source.backed) {
        const InlineValueCursor* const inline_cursor =
            inline_value_cursor(cursor.value);
        if (inline_cursor == nullptr) {
            return Result<AppendedNode>::failure(internal_error(
                path.snapshot(), "backed_inline_cursor_missing"));
        }
        node.runtime_type_id = inline_cursor->runtime_type_id;
        if (!inline_cursor->present) {
            node.tag = ValueTag::null_value;
        } else if (scalar_kind(inline_cursor->kind)) {
            auto observed = source.index->scalar_observation(
                source.bytes, source.byte_count, cursor.value);
            if (!observed.has_value()) {
                return Result<AppendedNode>::failure(
                    std::move(observed).error());
            }
            node.tag = tag_for(inline_cursor->kind);
            node.scalar_bits_or_offset = observed.value().bits;
        } else if (variable_kind(inline_cursor->kind)) {
            auto span = inline_cursor->kind == TypeKind::bytes
                            ? source.index->variable_span(
                                  source.bytes, source.byte_count,
                                  cursor.value)
                            : source.index->text_span(
                                  source.bytes, source.byte_count,
                                  cursor.value, path.snapshot());
            if (!span.has_value()) {
                return Result<AppendedNode>::failure(
                    std::move(span).error());
            }
            const std::uint8_t* const data =
                span.value().byte_length == UINT64_C(0)
                    ? nullptr
                    : source.bytes + static_cast<std::ptrdiff_t>(
                                         span.value().data_offset);
            node.tag = tag_for(inline_cursor->kind);
            node.scalar_bits_or_offset =
                append_bytes(arena, data, span.value().byte_length);
            node.byte_length = span.value().byte_length;
        } else if (inline_cursor->kind == TypeKind::component ||
                   inline_cursor->kind == TypeKind::list) {
            node.tag = tag_for(inline_cursor->kind);
            node.child_count = child_count.value();
        } else {
            return Result<AppendedNode>::failure(internal_error(
                path.snapshot(), "unsupported_runtime_kind"));
        }
        return Result<AppendedNode>::success(AppendedNode{
            append_node(arena, node), child_count.value()});
    }

    const ValueNode* const source_node = detached_node(source, cursor);
    if (source_node == nullptr || source_node->tag == ValueTag::object_record ||
        source_node->tag == ValueTag::object_root ||
        source_node->tag == ValueTag::reference) {
        return Result<AppendedNode>::failure(internal_error(
            path.snapshot(), "detached_value_node_invalid"));
    }
    node = *source_node;
    node.first_child = build::invalid_node_index;
    node.next_sibling = build::invalid_node_index;
    node.object_component_index = UINT32_MAX;
    node.object_id = UINT64_MAX;
    node.child_count = child_count.value();
    if (node.tag == ValueTag::str || node.tag == ValueTag::wstr ||
        node.tag == ValueTag::bytes) {
        const auto& storage = source.detached->arena.byte_storage();
        auto bounded = layout::checked_range_end(
            source_node->scalar_bits_or_offset, source_node->byte_length,
            size_as_u64(storage.size()), path.snapshot());
        if (!bounded.has_value()) {
            return Result<AppendedNode>::failure(
                std::move(bounded).error());
        }
        const std::uint8_t* const data =
            source_node->byte_length == UINT64_C(0)
                ? nullptr
                : storage.data() + static_cast<std::ptrdiff_t>(
                                       source_node->scalar_bits_or_offset);
        const TypeKind kind = node.tag == ValueTag::str
                                  ? TypeKind::str
                              : node.tag == ValueTag::wstr
                                  ? TypeKind::wstr
                                  : TypeKind::bytes;
        auto valid = validate_detached_text(
            kind, data, source_node->byte_length, path.snapshot());
        if (!valid.has_value()) {
            return Result<AppendedNode>::failure(std::move(valid).error());
        }
        node.scalar_bits_or_offset =
            append_bytes(arena, data, source_node->byte_length);
    }
    return Result<AppendedNode>::success(AppendedNode{
        append_node(arena, node), child_count.value()});
}

Result<void> allocate_direct_children(
    const Source& source,
    CopyFrame& frame,
    const std::vector<std::vector<std::uint64_t>>& source_ids,
    JsonPointerBuilder& path,
    ValueArena& arena) {
    if (frame.destination >= arena.nodes_.size()) {
        return Result<void>::failure(internal_error(
            path.snapshot(), "destination_parent_missing"));
    }
    const ValueNode& parent =
        arena.nodes_[static_cast<std::size_t>(frame.destination)];
    if (!container_tag(parent.tag) ||
        parent.child_count != frame.child_count ||
        parent.first_child != build::invalid_node_index) {
        return Result<void>::failure(internal_error(
            path.snapshot(), "destination_parent_invalid"));
    }
    if (frame.child_count == UINT64_C(0)) {
        frame.children_allocated = true;
        return Result<void>::success();
    }
    if (frame.child_count > arena.nodes_.max_size() - arena.nodes_.size()) {
        throw std::length_error("graph materialized child range exhausted");
    }
    const NodeIndex first_child = size_as_u64(arena.nodes_.size());
    arena.nodes_[static_cast<std::size_t>(frame.destination)].first_child =
        first_child;
    for (std::uint64_t index = UINT64_C(0); index < frame.child_count;
         ++index) {
        auto child_cursor = source_child(
            source, frame.cursor, index, path);
        if (!child_cursor.has_value()) {
            return Result<void>::failure(
                std::move(child_cursor).error());
        }
        auto child = append_source_node(
            source, child_cursor.value(), source_ids, path, arena);
        if (!child.has_value()) {
            return Result<void>::failure(std::move(child).error());
        }
        path.rewind(frame.path_mark);
        const NodeIndex expected = first_child + index;
        if (child.value().destination != expected) {
            return Result<void>::failure(internal_error(
                path.snapshot(), "destination_child_not_contiguous"));
        }
        arena.nodes_[static_cast<std::size_t>(expected)].next_sibling =
            index + UINT64_C(1) < frame.child_count
                ? expected + UINT64_C(1)
                : build::invalid_node_index;
    }
    frame.children_allocated = true;
    return Result<void>::success();
}

Result<NodeIndex> copy_tree(
    const Source& source,
    Cursor root,
    const std::vector<std::vector<std::uint64_t>>& source_ids,
    JsonPointerBuilder& path,
    ValueArena& arena) {
    auto root_node = append_source_node(
        source, root, source_ids, path, arena);
    if (!root_node.has_value()) {
        return Result<NodeIndex>::failure(std::move(root_node).error());
    }
    const NodeIndex root_destination = root_node.value().destination;
    const JsonPointerBuilder::Mark root_mark = path.mark();
    std::vector<CopyFrame> stack;
    if (root_node.value().child_count != UINT64_C(0)) {
        require_growth(stack, 1U, "graph copy stack exhausted");
        stack.push_back(CopyFrame{
            std::move(root), root_destination,
            root_node.value().child_count, false, UINT64_C(0), root_mark});
    }
    while (!stack.empty()) {
        CopyFrame& frame = stack.back();
        if (path.mark() != frame.path_mark) {
            return Result<NodeIndex>::failure(internal_error(
                path.snapshot(), "diagnostic_path_mark_mismatch"));
        }
        if (!frame.children_allocated) {
            auto allocated = allocate_direct_children(
                source, frame, source_ids, path, arena);
            if (!allocated.has_value()) {
                return Result<NodeIndex>::failure(
                    std::move(allocated).error());
            }
            continue;
        }
        if (frame.next_child == frame.child_count) {
            stack.pop_back();
            path.rewind(stack.empty() ? root_mark : stack.back().path_mark);
            continue;
        }
        const ValueNode& parent =
            arena.nodes_[static_cast<std::size_t>(frame.destination)];
        if (parent.first_child >= arena.nodes_.size() ||
            parent.child_count >
                size_as_u64(arena.nodes_.size()) - parent.first_child) {
            return Result<NodeIndex>::failure(internal_error(
                path.snapshot(), "destination_child_range_invalid"));
        }
        const std::uint64_t child_index = frame.next_child++;
        const NodeIndex child_destination = parent.first_child + child_index;
        const ValueNode& child_node =
            arena.nodes_[static_cast<std::size_t>(child_destination)];
        if (!container_tag(child_node.tag) ||
            child_node.child_count == UINT64_C(0)) {
            continue;
        }
        auto child_cursor = source_child(
            source, frame.cursor, child_index, path);
        if (!child_cursor.has_value()) {
            return Result<NodeIndex>::failure(
                std::move(child_cursor).error());
        }
        require_growth(stack, 1U, "graph copy stack exhausted");
        stack.push_back(CopyFrame{
            std::move(child_cursor).value(), child_destination,
            child_node.child_count, false, UINT64_C(0), path.mark()});
    }
    return Result<NodeIndex>::success(root_destination);
}

Result<void> validate_detached_state(
    const ValueArena& arena,
    const std::vector<std::vector<NodeIndex>>& object_pools,
    const std::vector<std::vector<std::uint64_t>>& source_ids,
    const layout::RuntimeSchema& runtime_schema,
    NodeIndex root) {
    const auto& nodes = arena.nodes();
    const JsonPointer path = JsonPointer{}.append("detached");
    if (root >= nodes.size() || object_pools.size() != source_ids.size() ||
        object_pools.size() !=
            runtime_schema.spec().resolved().components().size() ||
        size_as_u64(object_pools.size()) > UINT32_MAX) {
        return Result<void>::failure(
            internal_error(path, "detached_inventory_invalid"));
    }
    for (std::size_t component = 0U; component < object_pools.size();
         ++component) {
        const std::uint32_t component_id =
            static_cast<std::uint32_t>(component);
        if (object_pools[component].size() != source_ids[component].size()) {
            return Result<void>::failure(
                internal_error(path, "detached_pool_size_mismatch"));
        }
        const layout::ComponentLayout* const component_layout =
            runtime_schema.component(component_id);
        for (std::size_t object = 0U;
             object < object_pools[component].size(); ++object) {
            const NodeIndex node_index = object_pools[component][object];
            if (node_index >= nodes.size() || component_layout == nullptr) {
                return Result<void>::failure(
                    internal_error(path, "detached_object_node_missing"));
            }
            const ValueNode& node =
                nodes[static_cast<std::size_t>(node_index)];
            if (node.tag != ValueTag::object_record ||
                node.object_component_index != component_id ||
                node.object_id != size_as_u64(object) ||
                node.child_count !=
                    size_as_u64(component_layout->fields.size())) {
                return Result<void>::failure(
                    internal_error(path, "detached_object_node_invalid"));
            }
        }
    }

    std::vector<std::uint8_t> visited_nodes;
    if (nodes.size() > visited_nodes.max_size()) {
        throw std::length_error("detached node markers exhausted");
    }
    visited_nodes.assign(nodes.size(), UINT8_C(0));
    std::vector<std::vector<std::uint8_t>> reached;
    if (object_pools.size() > reached.max_size()) {
        throw std::length_error("detached reachability inventory exhausted");
    }
    reached.reserve(object_pools.size());
    for (const auto& pool : object_pools) {
        std::vector<std::uint8_t> markers;
        if (pool.size() > markers.max_size()) {
            throw std::length_error("detached reachability pool exhausted");
        }
        markers.assign(pool.size(), UINT8_C(0));
        reached.push_back(std::move(markers));
    }
    std::vector<Coordinate> queue;
    std::vector<NodeIndex> stack;
    require_growth(stack, 1U, "detached validation stack exhausted");
    stack.push_back(root);
    std::size_t next_object = 0U;

    const auto enqueue = [&](const ValueNode& node) -> Result<void> {
        if (node.object_component_index >= object_pools.size() ||
            node.object_id >=
                object_pools[node.object_component_index].size()) {
            return Result<void>::failure(internal_error(
                path, "detached_coordinate_out_of_range"));
        }
        std::uint8_t& marker =
            reached[node.object_component_index]
                   [static_cast<std::size_t>(node.object_id)];
        if (marker == UINT8_C(0)) {
            marker = UINT8_C(1);
            require_growth(queue, 1U,
                           "detached validation queue exhausted");
            queue.push_back(Coordinate{
                node.object_component_index, node.object_id});
        }
        return Result<void>::success();
    };

    while (!stack.empty() || next_object < queue.size()) {
        if (stack.empty()) {
            const Coordinate coordinate = queue[next_object++];
            require_growth(stack, 1U,
                           "detached validation stack exhausted");
            stack.push_back(
                object_pools[coordinate.component_index]
                            [static_cast<std::size_t>(coordinate.object_id)]);
        }
        const NodeIndex index = stack.back();
        stack.pop_back();
        if (index >= nodes.size()) {
            return Result<void>::failure(
                internal_error(path, "detached_node_out_of_range"));
        }
        if (visited_nodes[static_cast<std::size_t>(index)] != UINT8_C(0)) {
            return Result<void>::failure(
                internal_error(path, "detached_node_reused"));
        }
        visited_nodes[static_cast<std::size_t>(index)] = UINT8_C(1);
        const ValueNode& node = nodes[static_cast<std::size_t>(index)];
        if (node.tag == ValueTag::object_root ||
            node.tag == ValueTag::reference) {
            auto enqueued = enqueue(node);
            if (!enqueued.has_value()) {
                return enqueued;
            }
        }
        if (container_tag(node.tag)) {
            if ((node.child_count == UINT64_C(0)) !=
                    (node.first_child == build::invalid_node_index) ||
                (node.child_count != UINT64_C(0) &&
                 (node.first_child >= nodes.size() ||
                  node.child_count >
                      size_as_u64(nodes.size()) - node.first_child))) {
                return Result<void>::failure(internal_error(
                    path, "detached_child_range_invalid"));
            }
            if (node.child_count > stack.max_size() - stack.size()) {
                throw std::length_error(
                    "detached validation stack exhausted");
            }
            for (std::uint64_t child = node.child_count;
                 child != UINT64_C(0); --child) {
                const NodeIndex child_index =
                    node.first_child + child - UINT64_C(1);
                const NodeIndex expected_next =
                    child < node.child_count
                        ? child_index + UINT64_C(1)
                        : build::invalid_node_index;
                if (nodes[static_cast<std::size_t>(child_index)]
                        .next_sibling != expected_next) {
                    return Result<void>::failure(internal_error(
                        path, "detached_child_sequence_invalid"));
                }
                stack.push_back(child_index);
            }
        } else if (node.child_count != UINT64_C(0) ||
                   node.first_child != build::invalid_node_index) {
            return Result<void>::failure(
                internal_error(path, "detached_leaf_has_children"));
        }
        if (node.tag == ValueTag::str || node.tag == ValueTag::wstr ||
            node.tag == ValueTag::bytes) {
            auto bounded = layout::checked_range_end(
                node.scalar_bits_or_offset, node.byte_length,
                size_as_u64(arena.byte_storage().size()), path);
            if (!bounded.has_value()) {
                return Result<void>::failure(std::move(bounded).error());
            }
        }
    }
    if (std::find(visited_nodes.begin(), visited_nodes.end(), UINT8_C(0)) !=
        visited_nodes.end()) {
        return Result<void>::failure(
            internal_error(path, "detached_unreachable_node"));
    }
    for (const auto& markers : reached) {
        if (std::find(markers.begin(), markers.end(), UINT8_C(0)) !=
            markers.end()) {
            return Result<void>::failure(
                internal_error(path, "detached_unreachable_object"));
        }
    }
    return Result<void>::success();
}

}  // namespace

struct GraphMaterializeInternals final {
    static Result<Source> make_source(const View& view) {
        Source source;
        if (view.state_->is_backed) {
            if (view.state_->owner == nullptr) {
                return Result<Source>::failure(internal_error(
                    view.state_->diagnostic_path, "source_owner_missing"));
            }
            source.backed = true;
            source.owner = view.state_->owner;
            auto acquired = AccessPin::acquire(
                source.owner->barrier, view.state_->generation);
            if (!acquired.has_value()) {
                return Result<Source>::failure(
                    std::move(acquired).error());
            }
            source.pin.emplace(std::move(acquired).value());
            if (!source.owner->backing.has_value()) {
                return Result<Source>::failure(internal_error(
                    view.state_->diagnostic_path,
                    "source_backing_missing"));
            }
            source.index = &source.owner->index;
            source.bytes = source.owner->backing->readable_data();
            source.byte_count = source.owner->backing->readable_size();
            source.runtime_schema = source.owner->index.runtime_schema();
        } else {
            if (view.state_->detached == nullptr) {
                return Result<Source>::failure(internal_error(
                    view.state_->diagnostic_path,
                    "detached_state_missing"));
            }
            source.detached = view.state_->detached;
            source.runtime_schema = source.detached->runtime_schema;
        }
        if (source.runtime_schema == nullptr ||
            source.runtime_schema->spec().profile() !=
                spec::Profile::object_graph_v1) {
            return Result<Source>::failure(internal_error(
                view.state_->diagnostic_path,
                "graph_runtime_schema_missing"));
        }
        return Result<Source>::success(std::move(source));
    }

    static Cursor root_cursor(const View& view, const Source& source) {
        Cursor root;
        if (source.backed) {
            root.role = view.state_->is_sequence ? CursorRole::sequence
                                                 : CursorRole::value;
            root.entry = view.state_->sequence;
            root.value = view.state_->cursor;
            return root;
        }
        root.detached_node = view.state_->node;
        const ValueNode* const node = detached_node(source, root);
        root.role = node != nullptr && node->tag == ValueTag::object_record
                        ? CursorRole::identity_occurrence
                        : CursorRole::value;
        return root;
    }
};

Result<View> materialize_graph_with_metrics(
    const View& view,
    MaterializeMetrics* metrics) try {
    if (metrics != nullptr) {
        metrics->peak_retained_diagnostic_path_bytes = UINT64_C(0);
    }
    if (view.state_ == nullptr) {
        return Result<View>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }

    auto source_result = GraphMaterializeInternals::make_source(view);
    if (!source_result.has_value()) {
        return Result<View>::failure(std::move(source_result).error());
    }
    Source source = std::move(source_result).value();
    Cursor root = GraphMaterializeInternals::root_cursor(view, source);
    if (!source.backed && detached_node(source, root) == nullptr) {
        return Result<View>::failure(internal_error(
            view.state_->diagnostic_path, "detached_root_missing"));
    }

    const std::size_t component_count =
        source.runtime_schema->spec().resolved().components().size();
    if (size_as_u64(component_count) > UINT32_MAX) {
        throw std::length_error("graph component inventory exhausted");
    }
    Discovery discovery;
    if (component_count > discovery.seen.max_size() ||
        component_count > discovery.source_ids.max_size()) {
        throw std::length_error("graph discovery inventory exhausted");
    }
    discovery.seen.resize(component_count);
    discovery.source_ids.resize(component_count);

    JsonPointerBuilder path;
    path.assign(view.state_->diagnostic_path);
    auto discovered = discover_tree(
        source, root, path, discovery, metrics);
    if (!discovered.has_value()) {
        return Result<View>::failure(std::move(discovered).error());
    }
    while (discovery.next < discovery.queue.size()) {
        const Coordinate coordinate = discovery.queue[discovery.next++];
        auto object = object_cursor(source, coordinate, path);
        if (!object.has_value()) {
            return Result<View>::failure(std::move(object).error());
        }
        path.assign(JsonPointer{}
                        .append("objects")
                        .append(coordinate.component_index)
                        .append(coordinate.object_id));
        discovered = discover_tree(
            source, std::move(object).value(), path, discovery, metrics);
        if (!discovered.has_value()) {
            return Result<View>::failure(std::move(discovered).error());
        }
    }
    for (auto& ids : discovery.source_ids) {
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
            return Result<View>::failure(internal_error(
                JsonPointer{}.append("objects"),
                "duplicate_discovered_object"));
        }
    }
    discovery.seen.clear();

    ValueArena arena;
    path.assign(view.state_->diagnostic_path);
    auto copied_root = copy_tree(
        source, root, discovery.source_ids, path, arena);
    if (!copied_root.has_value()) {
        return Result<View>::failure(std::move(copied_root).error());
    }

    std::vector<std::vector<NodeIndex>> object_pools;
    if (component_count > object_pools.max_size()) {
        throw std::length_error("detached object-pool inventory exhausted");
    }
    object_pools.resize(component_count);
    for (std::size_t component = 0U; component < component_count;
         ++component) {
        const std::uint32_t component_id =
            static_cast<std::uint32_t>(component);
        auto& pool = object_pools[component];
        const auto& source_pool = discovery.source_ids[component];
        if (source_pool.size() > pool.max_size()) {
            throw std::length_error("detached object pool exhausted");
        }
        pool.reserve(source_pool.size());
        for (std::size_t object = 0U; object < source_pool.size(); ++object) {
            const Coordinate coordinate{
                component_id, source_pool[object]};
            auto cursor = object_cursor(source, coordinate, path);
            if (!cursor.has_value()) {
                return Result<View>::failure(std::move(cursor).error());
            }
            path.assign(JsonPointer{}
                            .append("objects")
                            .append(component)
                            .append(source_pool[object]));
            auto copied = copy_tree(
                source, std::move(cursor).value(), discovery.source_ids,
                path, arena);
            if (!copied.has_value()) {
                return Result<View>::failure(std::move(copied).error());
            }
            require_growth(pool, 1U, "detached object pool exhausted");
            pool.push_back(copied.value());
        }
    }

    auto valid = validate_detached_state(
        arena, object_pools, discovery.source_ids,
        *source.runtime_schema, copied_root.value());
    if (!valid.has_value()) {
        return Result<View>::failure(std::move(valid).error());
    }

    auto detached = std::make_shared<const DetachedViewState>(
        std::move(arena), std::move(object_pools), source.runtime_schema,
        copied_root.value());
    auto output = std::make_shared<View::State>();
    output->detached = std::move(detached);
    output->node = copied_root.value();
    output->diagnostic_path = view.state_->diagnostic_path;
    return Result<View>::success(View{std::move(output)});
} catch (const std::bad_alloc&) {
    return Result<View>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<View>::failure(allocation_error());
}

}  // namespace fastdb::payload::view
