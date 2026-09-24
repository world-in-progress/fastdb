#include "payload/build/GraphAuthoring.hpp"

#include "payload/json/JsonValue.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fastdb::payload::build {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

std::atomic<ObjectHandle> next_process_handle{UINT64_C(1)};

Error simple_error(std::uint32_t code,
                   JsonPointer path,
                   const char* message,
                   const char* reason) {
    return Error::from_details(
        code, std::move(path), message,
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

Error resource_error(JsonPointer path,
                     const char* resource,
                     std::uint64_t actual,
                     std::uint64_t limit) {
    return Error::from_details(
        FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT, std::move(path),
        "Payload builder resource limit exceeded",
        JsonValue::object({
            JsonValue::Member{"actual", JsonValue{std::to_string(actual)}},
            JsonValue::Member{"limit", JsonValue{std::to_string(limit)}},
            JsonValue::Member{"resource", JsonValue{resource}},
        }));
}

JsonPointer object_path(const spec::ResolvedSpec& resolved,
                        std::uint32_t component_index,
                        std::uint64_t object_id) {
    return JsonPointer{}
        .append("objects")
        .append(resolved.components()[component_index].id)
        .append(object_id);
}

Error internal_graph_error(const char* reason) {
    return simple_error(FDB_PAYLOAD_E_INTERNAL,
                        JsonPointer{}.append("objects"),
                        "Logical graph state is inconsistent", reason);
}

Result<ObjectHandle> allocate_process_handle(const JsonPointer& path) {
    ObjectHandle current =
        next_process_handle.load(std::memory_order_relaxed);
    while (current != invalid_object_handle) {
        const ObjectHandle next = current == UINT64_MAX
                                      ? invalid_object_handle
                                      : current + UINT64_C(1);
        if (next_process_handle.compare_exchange_weak(
                current, next, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return Result<ObjectHandle>::success(current);
        }
    }
    return Result<ObjectHandle>::failure(resource_error(
        path, "object_handles", UINT64_MAX, UINT64_MAX));
}

}  // namespace

Result<void> GraphAuthoring::initialize(std::size_t component_count) {
    if (component_count > object_pools_.max_size() ||
        component_count > filled_.max_size()) {
        return Result<void>::failure(resource_error(
            JsonPointer{}.append("objects"), "graph_objects", UINT64_MAX,
            static_cast<std::uint64_t>(
                std::min(object_pools_.max_size(), filled_.max_size()))));
    }
    object_pools_.resize(component_count);
    filled_.resize(component_count);
    return Result<void>::success();
}

std::uint64_t GraphAuthoring::component_object_count(
    std::uint32_t component_index) const noexcept {
    const std::size_t index = static_cast<std::size_t>(component_index);
    return index < object_pools_.size()
               ? static_cast<std::uint64_t>(object_pools_[index].size())
               : UINT64_C(0);
}

Result<void> GraphAuthoring::prepare_declaration(
    std::uint32_t component_index,
    const JsonPointer& path) {
    const std::size_t index = static_cast<std::size_t>(component_index);
    if (index >= object_pools_.size() || index >= filled_.size()) {
        return Result<void>::failure(
            internal_graph_error("invalid_component_pool"));
    }
    std::vector<NodeIndex>& pool = object_pools_[index];
    std::vector<std::uint8_t>& fills = filled_[index];
    if (pool.size() >= pool.max_size() || fills.size() >= fills.max_size()) {
        return Result<void>::failure(resource_error(
            path, "graph_objects", UINT64_MAX,
            static_cast<std::uint64_t>(
                std::min(pool.max_size(), fills.max_size()))));
    }
    if (handles_.size() >= handles_.max_size()) {
        return Result<void>::failure(resource_error(
            path, "object_handles", UINT64_MAX,
            static_cast<std::uint64_t>(handles_.max_size())));
    }
    pool.reserve(pool.size() + 1U);
    fills.reserve(fills.size() + 1U);
    handles_.reserve(handles_.size() + 1U);
    return Result<void>::success();
}

Result<ObjectHandle> GraphAuthoring::allocate_handle(
    const JsonPointer& path) {
    if (!use_test_handle_sequence_) {
        return allocate_process_handle(path);
    }
    if (test_next_handle_ == invalid_object_handle) {
        return Result<ObjectHandle>::failure(resource_error(
            path, "object_handles", UINT64_MAX, UINT64_MAX));
    }
    const ObjectHandle current = test_next_handle_;
    test_next_handle_ = current == UINT64_MAX
                            ? invalid_object_handle
                            : current + UINT64_C(1);
    return Result<ObjectHandle>::success(current);
}

Result<ObjectHandle> GraphAuthoring::commit_declaration(
    std::uint32_t component_index,
    NodeIndex object_node,
    const JsonPointer& path) {
    auto token = allocate_handle(path);
    if (!token.has_value()) {
        return token;
    }
    const std::size_t index = static_cast<std::size_t>(component_index);
    const ObjectCoordinate coordinate{
        component_index,
        static_cast<std::uint64_t>(object_pools_[index].size())};
    const auto inserted = handles_.emplace(token.value(), coordinate);
    if (!inserted.second) {
        return Result<ObjectHandle>::failure(
            internal_graph_error("duplicate_object_handle"));
    }
    object_pools_[index].push_back(object_node);
    filled_[index].push_back(UINT8_C(0));
    ++graph_object_count_;
    return token;
}

Result<ObjectCoordinate> GraphAuthoring::resolve(
    ObjectHandle object,
    const JsonPointer& path) const {
    if (object == invalid_object_handle) {
        return Result<ObjectCoordinate>::failure(simple_error(
            FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE, path,
            "Temporary object handle is invalid", "invalid_object_handle"));
    }
    const auto found = handles_.find(object);
    if (found == handles_.end()) {
        return Result<ObjectCoordinate>::failure(simple_error(
            FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE, path,
            "Temporary object handle is invalid", "invalid_object_handle"));
    }
    return Result<ObjectCoordinate>::success(found->second);
}

bool GraphAuthoring::filled(ObjectCoordinate object) const noexcept {
    const std::size_t component =
        static_cast<std::size_t>(object.component_index);
    if (component >= filled_.size() ||
        object.object_id >= filled_[component].size()) {
        return false;
    }
    return filled_[component][static_cast<std::size_t>(object.object_id)] !=
           UINT8_C(0);
}

NodeIndex GraphAuthoring::object_node(ObjectCoordinate object) const noexcept {
    const std::size_t component =
        static_cast<std::size_t>(object.component_index);
    if (component >= object_pools_.size() ||
        object.object_id >= object_pools_[component].size()) {
        return invalid_node_index;
    }
    return object_pools_[component]
                        [static_cast<std::size_t>(object.object_id)];
}

void GraphAuthoring::mark_filled(ObjectCoordinate object) noexcept {
    const std::size_t component =
        static_cast<std::size_t>(object.component_index);
    if (component < filled_.size() &&
        object.object_id < filled_[component].size()) {
        filled_[component][static_cast<std::size_t>(object.object_id)] =
            UINT8_C(1);
    }
}

Result<void> GraphAuthoring::validate_filled(
    const spec::ResolvedSpec& resolved) const {
    if (object_pools_.size() != resolved.components().size() ||
        filled_.size() != resolved.components().size()) {
        return Result<void>::failure(
            internal_graph_error("invalid_component_inventory"));
    }
    for (std::size_t component = 0U; component < object_pools_.size();
         ++component) {
        if (object_pools_[component].size() != filled_[component].size()) {
            return Result<void>::failure(
                internal_graph_error("invalid_fill_inventory"));
        }
        for (std::size_t object = 0U;
             object < filled_[component].size(); ++object) {
            if (filled_[component][object] == UINT8_C(0)) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_MISSING_FIELD,
                    object_path(resolved,
                                static_cast<std::uint32_t>(component),
                                static_cast<std::uint64_t>(object)),
                    "Payload object was not filled", "object_not_filled"));
            }
        }
    }
    return Result<void>::success();
}

Result<void> GraphAuthoring::validate_reachable(
    const spec::ResolvedSpec& resolved,
    const ValueArena& arena,
    const std::vector<NodeIndex>& entry_roots) const {
    std::vector<std::vector<std::uint8_t>> reached;
    reached.reserve(object_pools_.size());
    for (const auto& pool : object_pools_) {
        reached.emplace_back(pool.size(), UINT8_C(0));
    }

    std::vector<ObjectCoordinate> queue;
    if (graph_object_count_ > queue.max_size()) {
        return Result<void>::failure(resource_error(
            JsonPointer{}.append("objects"), "graph_objects",
            graph_object_count_,
            static_cast<std::uint64_t>(queue.max_size())));
    }
    queue.reserve(static_cast<std::size_t>(graph_object_count_));
    std::vector<NodeIndex> stack;
    stack.reserve(arena.nodes_.size());

    const auto enqueue = [&](ObjectCoordinate coordinate) -> Result<void> {
        const std::size_t component =
            static_cast<std::size_t>(coordinate.component_index);
        if (component >= object_pools_.size() ||
            coordinate.object_id >= object_pools_[component].size()) {
            return Result<void>::failure(
                internal_graph_error("invalid_object_coordinate"));
        }
        std::uint8_t& marker =
            reached[component][static_cast<std::size_t>(coordinate.object_id)];
        if (marker == UINT8_C(0)) {
            marker = UINT8_C(1);
            queue.push_back(coordinate);
        }
        return Result<void>::success();
    };

    const auto walk = [&](NodeIndex root) -> Result<void> {
        stack.clear();
        stack.push_back(root);
        while (!stack.empty()) {
            const NodeIndex index = stack.back();
            stack.pop_back();
            if (index >= arena.nodes_.size()) {
                return Result<void>::failure(
                    internal_graph_error("invalid_value_node"));
            }
            const ValueNode& node =
                arena.nodes_[static_cast<std::size_t>(index)];
            switch (node.tag) {
            case ValueTag::sequence:
            case ValueTag::component:
            case ValueTag::list:
            case ValueTag::object_record: {
                NodeIndex child = node.first_child;
                std::uint64_t observed = UINT64_C(0);
                while (child != invalid_node_index) {
                    if (child >= arena.nodes_.size() ||
                        observed >= node.child_count) {
                        return Result<void>::failure(
                            internal_graph_error("invalid_value_tree"));
                    }
                    stack.push_back(child);
                    child = arena.nodes_[static_cast<std::size_t>(child)]
                                .next_sibling;
                    ++observed;
                }
                if (observed != node.child_count) {
                    return Result<void>::failure(
                        internal_graph_error("invalid_value_tree"));
                }
                break;
            }
            case ValueTag::object_root:
            case ValueTag::reference: {
                auto queued = enqueue(ObjectCoordinate{
                    node.object_component_index, node.object_id});
                if (!queued.has_value()) {
                    return queued;
                }
                break;
            }
            case ValueTag::null_value:
            case ValueTag::boolean:
            case ValueTag::u8:
            case ValueTag::u16:
            case ValueTag::u32:
            case ValueTag::i32:
            case ValueTag::u8n:
            case ValueTag::u16n:
            case ValueTag::f32:
            case ValueTag::f64:
            case ValueTag::str:
            case ValueTag::wstr:
            case ValueTag::bytes:
                break;
            }
        }
        return Result<void>::success();
    };

    for (const NodeIndex root : entry_roots) {
        auto walked = walk(root);
        if (!walked.has_value()) {
            return walked;
        }
    }
    std::size_t next = 0U;
    while (next < queue.size()) {
        const ObjectCoordinate coordinate = queue[next++];
        const NodeIndex object_node =
            object_pools_[coordinate.component_index]
                         [static_cast<std::size_t>(coordinate.object_id)];
        auto walked = walk(object_node);
        if (!walked.has_value()) {
            return walked;
        }
    }

    for (std::size_t component = 0U; component < reached.size();
         ++component) {
        for (std::size_t object = 0U; object < reached[component].size();
             ++object) {
            if (reached[component][object] == UINT8_C(0)) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_UNREACHABLE_OBJECT,
                    object_path(resolved,
                                static_cast<std::uint32_t>(component),
                                static_cast<std::uint64_t>(object)),
                    "Payload object is unreachable", "unreachable_object"));
            }
        }
    }
    return Result<void>::success();
}

std::vector<std::vector<NodeIndex>>
GraphAuthoring::take_object_pools() noexcept {
    return std::move(object_pools_);
}

void GraphAuthoring::restore_object_pools(
    std::vector<std::vector<NodeIndex>> object_pools) noexcept {
    object_pools_ = std::move(object_pools);
}

void GraphAuthoring::commit_frozen_state() noexcept {
    handles_.clear();
    filled_.clear();
    graph_object_count_ = UINT64_C(0);
    test_next_handle_ = invalid_object_handle;
    use_test_handle_sequence_ = false;
    object_pools_.clear();
}

bool GraphAuthoring::use_test_sequence(ObjectHandle next) noexcept {
    if (graph_object_count_ != UINT64_C(0) || !handles_.empty() ||
        next == invalid_object_handle) {
        return false;
    }
    use_test_handle_sequence_ = true;
    test_next_handle_ = next;
    return true;
}

bool GraphAuthoringTestAccess::use_object_handle_sequence(
    GraphAuthoring& graph,
    ObjectHandle next) noexcept {
    return graph.use_test_sequence(next);
}

}  // namespace fastdb::payload::build
