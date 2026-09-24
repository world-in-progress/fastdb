#include "payload/view/Materialize.hpp"

#include "payload/json/JsonValue.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/TextEncoding.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <fastdb_payload.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
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
           tag == ValueTag::list;
}

struct Source final {
    bool backed{false};
    const PayloadIndex* index{nullptr};
    const std::uint8_t* bytes{nullptr};
    std::uint64_t byte_count{UINT64_C(0)};
    const DetachedViewState* detached{nullptr};
    std::shared_ptr<const layout::RuntimeSchema> runtime_schema;
};

struct Cursor final {
    bool sequence{false};
    EntrySequenceCursor entry{UINT32_C(0)};
    ValueCursor value{InlineValueCursor{
        UINT32_C(0), TypeKind::boolean, UINT64_C(0), false}};
    NodeIndex detached_node{build::invalid_node_index};
};

struct AppendedNode final {
    NodeIndex destination;
    std::uint64_t child_count;
};

struct TraversalFrame final {
    Cursor source;
    NodeIndex destination;
    std::uint64_t child_count{UINT64_C(0)};
    bool children_allocated{false};
    std::uint64_t next_child_to_expand{UINT64_C(0)};
    JsonPointerBuilder::Mark path_mark{0U};
};

std::uint64_t size_as_u64(std::size_t size);

void observe_retained_diagnostic_path_state(
    const JsonPointerBuilder& path,
    const std::vector<TraversalFrame>& stack,
    MaterializeMetrics* metrics) {
    if (metrics == nullptr) {
        return;
    }
    const std::uint64_t path_bytes = size_as_u64(path.mark());
    const std::uint64_t frame_count = size_as_u64(stack.size());
    const std::uint64_t mark_size = static_cast<std::uint64_t>(
        sizeof(JsonPointerBuilder::Mark));
    if (frame_count >
        std::numeric_limits<std::uint64_t>::max() / mark_size) {
        metrics->peak_retained_diagnostic_path_bytes =
            std::numeric_limits<std::uint64_t>::max();
        return;
    }
    const std::uint64_t mark_bytes = frame_count * mark_size;
    const std::uint64_t retained =
        path_bytes > std::numeric_limits<std::uint64_t>::max() - mark_bytes
            ? std::numeric_limits<std::uint64_t>::max()
            : path_bytes + mark_bytes;
    if (retained > metrics->peak_retained_diagnostic_path_bytes) {
        metrics->peak_retained_diagnostic_path_bytes = retained;
    }
}

std::uint64_t size_as_u64(std::size_t size) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (size > static_cast<std::size_t>(UINT64_MAX)) {
            throw std::length_error("materialized arena size overflow");
        }
    }
    return static_cast<std::uint64_t>(size);
}

NodeIndex append_node(ValueArena& arena, ValueNode node) {
    if (arena.nodes_.size() == arena.nodes_.max_size()) {
        throw std::length_error("materialized node arena exhausted");
    }
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
    if (bytes == nullptr ||
        byte_count > static_cast<std::uint64_t>(PTRDIFF_MAX) ||
        byte_count >
            static_cast<std::uint64_t>(arena.byte_storage_.max_size() -
                                       arena.byte_storage_.size())) {
        throw std::length_error("materialized byte arena exhausted");
    }
    arena.byte_storage_.insert(
        arena.byte_storage_.end(), bytes,
        bytes + static_cast<std::ptrdiff_t>(byte_count));
    return offset;
}

Result<const ValueNode*> detached_node(const Source& source,
                                       const Cursor& cursor,
                                       const JsonPointerBuilder& path) {
    if (source.detached == nullptr ||
        cursor.detached_node >= source.detached->arena.nodes().size()) {
        return Result<const ValueNode*>::failure(
            internal_error(path.snapshot(), "detached_node_missing"));
    }
    return Result<const ValueNode*>::success(
        &source.detached->arena.nodes()[
            static_cast<std::size_t>(cursor.detached_node)]);
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

Result<AppendedNode> append_backed_node(const Source& source,
                                        const Cursor& cursor,
                                        const JsonPointerBuilder& path,
                                        ValueArena& arena) {
    if (source.index == nullptr) {
        return Result<AppendedNode>::failure(
            internal_error(path.snapshot(), "source_index_missing"));
    }

    ValueNode node{
        UINT32_C(0), ValueTag::null_value, {UINT8_C(0), UINT8_C(0),
                                           UINT8_C(0)},
        UINT64_C(0), UINT64_C(0), build::invalid_node_index,
        build::invalid_node_index, UINT64_C(0)};
    std::uint64_t expected_children = UINT64_C(0);

    if (cursor.sequence) {
        auto slot = source.index->entry_slot(cursor.entry.entry_index);
        if (!slot.has_value()) {
            return Result<AppendedNode>::failure(std::move(slot).error());
        }
        auto length = source.index->sequence_length(
            source.bytes, source.byte_count, cursor.entry);
        if (!length.has_value()) {
            return Result<AppendedNode>::failure(std::move(length).error());
        }
        node.runtime_type_id = slot.value().runtime_type_id;
        node.tag = ValueTag::sequence;
        expected_children = length.value();
    } else {
        const InlineValueCursor* const inline_cursor =
            inline_value_cursor(cursor.value);
        if (inline_cursor == nullptr) {
            return Result<AppendedNode>::failure(internal_error(
                path.snapshot(), "graph_materialization_not_available"));
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
        } else if (inline_cursor->kind == TypeKind::component) {
            auto count = source.index->component_field_count(
                source.bytes, source.byte_count, cursor.value);
            if (!count.has_value()) {
                return Result<AppendedNode>::failure(
                    std::move(count).error());
            }
            node.tag = ValueTag::component;
            expected_children = count.value();
        } else if (inline_cursor->kind == TypeKind::list) {
            auto count = source.index->list_length(
                source.bytes, source.byte_count, cursor.value);
            if (!count.has_value()) {
                return Result<AppendedNode>::failure(
                    std::move(count).error());
            }
            node.tag = ValueTag::list;
            expected_children = count.value();
        } else {
            return Result<AppendedNode>::failure(
                internal_error(path.snapshot(),
                               "unsupported_runtime_kind"));
        }
    }

    node.child_count = expected_children;
    const NodeIndex destination = append_node(arena, node);
    return Result<AppendedNode>::success(
        AppendedNode{destination, expected_children});
}

Result<AppendedNode> append_detached_node(const Source& source,
                                          const Cursor& cursor,
                                          const JsonPointerBuilder& path,
                                          ValueArena& arena) {
    auto source_node = detached_node(source, cursor, path);
    if (!source_node.has_value()) {
        return Result<AppendedNode>::failure(
            std::move(source_node).error());
    }
    ValueNode node = *source_node.value();
    node.first_child = build::invalid_node_index;
    node.next_sibling = build::invalid_node_index;

    std::uint64_t expected_children = source_node.value()->child_count;
    const NodeIndex first_child = source_node.value()->first_child;
    if (!container_tag(source_node.value()->tag)) {
        if (expected_children != UINT64_C(0) ||
            first_child != build::invalid_node_index) {
            return Result<AppendedNode>::failure(
                internal_error(path.snapshot(),
                               "detached_leaf_has_children"));
        }
        expected_children = UINT64_C(0);
    } else if ((expected_children == UINT64_C(0)) !=
               (first_child == build::invalid_node_index)) {
        return Result<AppendedNode>::failure(
            internal_error(path.snapshot(),
                           "detached_child_chain_mismatch"));
    }

    if (source_node.value()->tag == ValueTag::str ||
        source_node.value()->tag == ValueTag::wstr ||
        source_node.value()->tag == ValueTag::bytes) {
        if (source.detached == nullptr) {
            return Result<AppendedNode>::failure(
                internal_error(path.snapshot(), "detached_state_missing"));
        }
        const auto& storage = source.detached->arena.byte_storage();
        const JsonPointer diagnostic_path = path.snapshot();
        auto bounded = layout::checked_range_end(
            source_node.value()->scalar_bits_or_offset,
            source_node.value()->byte_length,
            size_as_u64(storage.size()), diagnostic_path);
        if (!bounded.has_value()) {
            return Result<AppendedNode>::failure(
                std::move(bounded).error());
        }
        const std::uint8_t* const data =
            source_node.value()->byte_length == UINT64_C(0)
                ? nullptr
                : storage.data() + static_cast<std::ptrdiff_t>(
                                       source_node.value()
                                           ->scalar_bits_or_offset);
        const TypeKind kind =
            source_node.value()->tag == ValueTag::str
                ? TypeKind::str
                : source_node.value()->tag == ValueTag::wstr
                      ? TypeKind::wstr
                      : TypeKind::bytes;
        auto valid = validate_detached_text(
            kind, data, source_node.value()->byte_length, diagnostic_path);
        if (!valid.has_value()) {
            return Result<AppendedNode>::failure(std::move(valid).error());
        }
        node.scalar_bits_or_offset = append_bytes(
            arena, data, source_node.value()->byte_length);
    }

    const NodeIndex destination = append_node(arena, node);
    return Result<AppendedNode>::success(
        AppendedNode{destination, expected_children});
}

Result<AppendedNode> append_source_node(const Source& source,
                                        const Cursor& cursor,
                                        const JsonPointerBuilder& path,
                                        ValueArena& arena) {
    return source.backed ? append_backed_node(source, cursor, path, arena)
                         : append_detached_node(source, cursor, path, arena);
}

Result<Cursor> source_child(const Source& source,
                            const Cursor& parent,
                            std::uint64_t index,
                            JsonPointerBuilder& path) {
    Cursor child;
    bool component =
        source.backed && !parent.sequence &&
        value_cursor_kind(parent.value) == TypeKind::component;
    const ValueNode* detached_parent = nullptr;
    if (!source.backed) {
        auto parent_node = detached_node(source, parent, path);
        if (!parent_node.has_value()) {
            return Result<Cursor>::failure(
                std::move(parent_node).error());
        }
        detached_parent = parent_node.value();
        component = detached_parent->tag == ValueTag::component;
    }

    if (!source.backed) {
        if (source.detached == nullptr || detached_parent == nullptr ||
            !container_tag(detached_parent->tag) ||
            index >= detached_parent->child_count) {
            return Result<Cursor>::failure(internal_error(
                path.snapshot(), "detached_child_index_invalid"));
        }
        const auto& nodes = source.detached->arena.nodes();
        if (detached_parent->first_child >= nodes.size() ||
            detached_parent->child_count >
                static_cast<std::uint64_t>(nodes.size()) -
                    detached_parent->first_child) {
            return Result<Cursor>::failure(internal_error(
                path.snapshot(), "detached_child_range_invalid"));
        }
        child.detached_node = detached_parent->first_child + index;
        const NodeIndex expected_next =
            index + UINT64_C(1) < detached_parent->child_count
                ? child.detached_node + UINT64_C(1)
                : build::invalid_node_index;
        if (nodes[static_cast<std::size_t>(child.detached_node)]
                .next_sibling != expected_next) {
            return Result<Cursor>::failure(internal_error(
                path.snapshot(), "detached_child_sequence_invalid"));
        }
        if (component) {
            path.append("fields");
        }
        path.append(index);
        return Result<Cursor>::success(std::move(child));
    }

    if (source.index == nullptr) {
        return Result<Cursor>::failure(
            internal_error(path.snapshot(), "source_index_missing"));
    }
    if (parent.sequence) {
        auto value = source.index->entry_value(
            source.bytes, source.byte_count, parent.entry, index);
        if (!value.has_value()) {
            return Result<Cursor>::failure(std::move(value).error());
        }
        child.value = value.value();
        path.append(index);
        return Result<Cursor>::success(std::move(child));
    }
    if (value_cursor_kind(parent.value) == TypeKind::component) {
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
    if (value_cursor_kind(parent.value) == TypeKind::list) {
        auto value = source.index->list_item(
            source.bytes, source.byte_count, parent.value, index);
        if (!value.has_value()) {
            return Result<Cursor>::failure(std::move(value).error());
        }
        child.value = value.value();
        path.append(index);
        return Result<Cursor>::success(std::move(child));
    }
    return Result<Cursor>::failure(
        internal_error(path.snapshot(), "backed_non_container_frame"));
}

Result<void> allocate_direct_children(const Source& source,
                                      TraversalFrame& frame,
                                      JsonPointerBuilder& path,
                                      ValueArena& arena) {
    if (frame.destination >= arena.nodes_.size()) {
        return Result<void>::failure(internal_error(
            path.snapshot(), "destination_parent_missing"));
    }
    const ValueNode& parent =
        arena.nodes_[static_cast<std::size_t>(frame.destination)];
    if (!container_tag(parent.tag) || parent.child_count != frame.child_count ||
        parent.first_child != build::invalid_node_index) {
        return Result<void>::failure(internal_error(
            path.snapshot(), "destination_parent_invalid"));
    }
    if (frame.child_count == UINT64_C(0)) {
        frame.children_allocated = true;
        return Result<void>::success();
    }

    const std::uint64_t remaining_capacity = size_as_u64(
        arena.nodes_.max_size() - arena.nodes_.size());
    if (frame.child_count > remaining_capacity) {
        throw std::length_error("materialized child range exhausted");
    }
    const NodeIndex first_child = size_as_u64(arena.nodes_.size());
    arena.nodes_[static_cast<std::size_t>(frame.destination)].first_child =
        first_child;

    /*
     * Direct children are appended as one uninterrupted node range before
     * any child container is expanded. Detached lookup can therefore use
     * checked first_child + index without a persistent adjacency index.
     */
    for (std::uint64_t index = UINT64_C(0); index < frame.child_count;
         ++index) {
        auto child_cursor = source_child(
            source, frame.source, index, path);
        if (!child_cursor.has_value()) {
            return Result<void>::failure(
                std::move(child_cursor).error());
        }
        auto child = append_source_node(
            source, child_cursor.value(), path, arena);
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

Result<NodeIndex> copy_tree(const Source& source,
                            Cursor root,
                            JsonPointerBuilder& path,
                            ValueArena& arena,
                            MaterializeMetrics* metrics) {
    auto root_node = append_source_node(source, root, path, arena);
    if (!root_node.has_value()) {
        return Result<NodeIndex>::failure(std::move(root_node).error());
    }
    const NodeIndex root_destination = root_node.value().destination;
    const JsonPointerBuilder::Mark root_path_mark = path.mark();
    std::vector<TraversalFrame> stack;
    if (root_node.value().child_count != UINT64_C(0)) {
        stack.push_back(TraversalFrame{
            std::move(root), root_destination,
            root_node.value().child_count, false, UINT64_C(0),
            root_path_mark});
        observe_retained_diagnostic_path_state(path, stack, metrics);
    }

    while (!stack.empty()) {
        TraversalFrame& frame = stack.back();
        if (path.mark() != frame.path_mark) {
            return Result<NodeIndex>::failure(internal_error(
                path.snapshot(), "diagnostic_path_mark_mismatch"));
        }
        if (!frame.children_allocated) {
            auto allocated =
                allocate_direct_children(source, frame, path, arena);
            if (!allocated.has_value()) {
                return Result<NodeIndex>::failure(
                    std::move(allocated).error());
            }
            continue;
        }
        if (frame.next_child_to_expand == frame.child_count) {
            stack.pop_back();
            path.rewind(stack.empty() ? root_path_mark
                                      : stack.back().path_mark);
            continue;
        }

        if (frame.destination >= arena.nodes_.size()) {
            return Result<NodeIndex>::failure(internal_error(
                path.snapshot(), "destination_parent_missing"));
        }
        const ValueNode& destination_parent =
            arena.nodes_[static_cast<std::size_t>(frame.destination)];
        if (destination_parent.first_child >= arena.nodes_.size() ||
            destination_parent.child_count >
                size_as_u64(arena.nodes_.size()) -
                    destination_parent.first_child) {
            return Result<NodeIndex>::failure(internal_error(
                path.snapshot(), "destination_child_range_invalid"));
        }
        const std::uint64_t child_index = frame.next_child_to_expand;
        const NodeIndex child_destination =
            destination_parent.first_child + child_index;
        const ValueNode& child_node =
            arena.nodes_[static_cast<std::size_t>(child_destination)];
        ++frame.next_child_to_expand;
        if (container_tag(child_node.tag) &&
            child_node.child_count != UINT64_C(0)) {
            auto child_cursor = source_child(
                source, frame.source, child_index, path);
            if (!child_cursor.has_value()) {
                return Result<NodeIndex>::failure(
                    std::move(child_cursor).error());
            }
            stack.push_back(TraversalFrame{
                std::move(child_cursor).value(), child_destination,
                child_node.child_count, false, UINT64_C(0), path.mark()});
            observe_retained_diagnostic_path_state(path, stack, metrics);
        }
    }
    return Result<NodeIndex>::success(root_destination);
}

}  // namespace

Result<View> materialize_with_metrics(
    const View& view,
    MaterializeMetrics* metrics) try {
    if (metrics != nullptr) {
        metrics->peak_retained_diagnostic_path_bytes = UINT64_C(0);
    }
    if (view.state_ == nullptr) {
        return Result<View>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    const bool graph_source =
        view.state_->is_backed
            ? view.state_->owner != nullptr &&
                  view.state_->owner->spec.profile() ==
                      spec::Profile::object_graph_v1
            : view.state_->detached != nullptr &&
                  view.state_->detached->runtime_schema != nullptr &&
                  view.state_->detached->runtime_schema->spec().profile() ==
                      spec::Profile::object_graph_v1;
    if (graph_source) {
        return materialize_graph_with_metrics(view, metrics);
    }

    Source source;
    Cursor root;
    JsonPointerBuilder path;
    path.assign(view.state_->diagnostic_path);
    std::shared_ptr<PayloadOwnerState> retained_owner;
    std::optional<AccessPin> source_pin;

    if (view.state_->is_backed) {
        if (view.state_->owner == nullptr) {
            return Result<View>::failure(internal_error(
                view.state_->diagnostic_path, "source_owner_missing"));
        }
        retained_owner = view.state_->owner;
        auto acquired = AccessPin::acquire(retained_owner->barrier,
                                           view.state_->generation);
        if (!acquired.has_value()) {
            return Result<View>::failure(std::move(acquired).error());
        }
        source_pin.emplace(std::move(acquired).value());
        if (!retained_owner->backing.has_value()) {
            return Result<View>::failure(internal_error(
                view.state_->diagnostic_path,
                "source_backing_missing"));
        }
        source.backed = true;
        source.index = &retained_owner->index;
        source.bytes = retained_owner->backing->readable_data();
        source.byte_count = retained_owner->backing->readable_size();
        source.runtime_schema = retained_owner->index.runtime_schema();
        root.sequence = view.state_->is_sequence;
        root.entry = view.state_->sequence;
        root.value = view.state_->cursor;
    } else {
        if (view.state_->detached == nullptr) {
            return Result<View>::failure(internal_error(
                view.state_->diagnostic_path,
                "detached_state_missing"));
        }
        source.detached = view.state_->detached.get();
        source.runtime_schema = view.state_->detached->runtime_schema;
        root.detached_node = view.state_->node;
    }
    if (source.runtime_schema == nullptr) {
        return Result<View>::failure(internal_error(
            view.state_->diagnostic_path, "runtime_schema_missing"));
    }

    ValueArena arena;
    auto copied_root =
        copy_tree(source, std::move(root), path, arena, metrics);
    if (!copied_root.has_value()) {
        return Result<View>::failure(std::move(copied_root).error());
    }

    auto detached = std::make_shared<const DetachedViewState>(
        std::move(arena), source.runtime_schema, copied_root.value());
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

Result<View> materialize(const View& view) {
    return materialize_with_metrics(view, nullptr);
}

}  // namespace fastdb::payload::view
