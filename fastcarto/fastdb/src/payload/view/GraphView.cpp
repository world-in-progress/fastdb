#include "payload/view/View.hpp"

#include "payload/json/JsonValue.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <fastdb_payload.h>

#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <variant>

namespace fastdb::payload::view {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

Error graph_view_error(std::uint32_t code,
                       const JsonPointer& path,
                       const char* message,
                       const char* reason) {
    return Error::from_details(
        code, path, message,
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{reason}}}));
}

Error allocation_error() {
    return graph_view_error(
        FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
        "Portable payload view allocation failed", "allocation_failed");
}

Error type_mismatch(const JsonPointer& path) {
    return graph_view_error(
        FDB_PAYLOAD_E_TYPE_MISMATCH, path,
        "Portable payload view kind does not match the operation",
        "view_kind_mismatch");
}

Error unexpected_null(const JsonPointer& path) {
    return graph_view_error(
        FDB_PAYLOAD_E_UNEXPECTED_NULL, path,
        "Portable payload view is null", "unexpected_null");
}

Error internal_error(const JsonPointer& path, const char* reason) {
    return graph_view_error(
        FDB_PAYLOAD_E_INTERNAL, path,
        "Portable payload view state is inconsistent", reason);
}

}  // namespace

struct GraphViewInternals final {
    static Result<AccessPin> pin(const View::State& state) {
        if (!state.is_backed || state.owner == nullptr) {
            return Result<AccessPin>::failure(
                internal_error(state.diagnostic_path,
                               "backed_view_state_missing"));
        }
        auto pin =
            AccessPin::acquire(state.owner->barrier, state.generation);
        if (!pin.has_value()) {
            return Result<AccessPin>::failure(std::move(pin).error());
        }
        if (!state.owner->backing.has_value()) {
            return Result<AccessPin>::failure(
                internal_error(state.diagnostic_path,
                               "source_backing_missing"));
        }
        return Result<AccessPin>::success(std::move(pin).value());
    }

    static Result<const build::ValueNode*> detached_node(
        const View::State& state) {
        if (state.is_backed || state.detached == nullptr ||
            state.detached->runtime_schema == nullptr ||
            state.node >= state.detached->arena.nodes().size()) {
            return Result<const build::ValueNode*>::failure(
                internal_error(state.diagnostic_path,
                               "detached_graph_state_missing"));
        }
        return Result<const build::ValueNode*>::success(
            &state.detached->arena.nodes()[
                static_cast<std::size_t>(state.node)]);
    }

    static bool is_null_identity(const build::ValueNode& node) noexcept {
        return node.tag == build::ValueTag::null_value &&
               node.runtime_type_id == UINT32_MAX &&
               node.object_component_index != UINT32_MAX &&
               node.object_id == UINT64_MAX;
    }

    static bool is_identity(const build::ValueNode& node) noexcept {
        return node.tag == build::ValueTag::object_root ||
               node.tag == build::ValueTag::object_record ||
               is_null_identity(node);
    }

    static bool is_reference(const View::State& state,
                             const build::ValueNode& node) noexcept {
        if (node.tag == build::ValueTag::reference) {
            return true;
        }
        if (node.tag != build::ValueTag::null_value ||
            node.runtime_type_id == UINT32_MAX) {
            return false;
        }
        const layout::RuntimeType* const type =
            state.detached->runtime_schema->find_type(
                node.runtime_type_id);
        return type != nullptr && type->source != nullptr &&
               type->source->kind == spec::TypeKind::ref &&
               type->storage_role == spec::StorageRole::reference_id;
    }

    static Result<GraphIdentity> detached_identity(
        const View::State& state,
        const build::ValueNode& node) {
        if (!is_identity(node) && !is_reference(state, node)) {
            return Result<GraphIdentity>::failure(
                type_mismatch(state.diagnostic_path));
        }
        if (node.tag == build::ValueTag::null_value) {
            return Result<GraphIdentity>::failure(
                unexpected_null(state.diagnostic_path));
        }
        if (node.object_component_index == UINT32_MAX ||
            node.object_id == UINT64_MAX ||
            state.detached->object_record_node(
                node.object_component_index, node.object_id) ==
                build::invalid_node_index) {
            return Result<GraphIdentity>::failure(
                internal_error(state.diagnostic_path,
                               "detached_object_coordinate_invalid"));
        }
        return Result<GraphIdentity>::success(GraphIdentity{
            node.object_component_index, node.object_id});
    }

    static Result<View> detached_ref_target(
        const View::State& state) try {
        auto node = detached_node(state);
        if (!node.has_value()) {
            return Result<View>::failure(std::move(node).error());
        }
        if (!is_reference(state, *node.value())) {
            return Result<View>::failure(
                type_mismatch(state.diagnostic_path));
        }
        if (node.value()->tag == build::ValueTag::null_value) {
            return Result<View>::failure(
                unexpected_null(state.diagnostic_path));
        }
        const build::NodeIndex target =
            state.detached->object_record_node(
                node.value()->object_component_index,
                node.value()->object_id);
        if (target == build::invalid_node_index) {
            return Result<View>::failure(
                internal_error(state.diagnostic_path,
                               "detached_object_coordinate_invalid"));
        }
        auto child = std::make_shared<View::State>();
        child->detached = state.detached;
        child->node = target;
        child->diagnostic_path = state.diagnostic_path.append("target");
        return Result<View>::success(View{std::move(child)});
    } catch (const std::bad_alloc&) {
        return Result<View>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<View>::failure(allocation_error());
    }
};

Result<View> View::ref_target() const try {
    if (state_ == nullptr) {
        return Result<View>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (!state_->is_backed) {
        return GraphViewInternals::detached_ref_target(*state_);
    }
    auto pin = GraphViewInternals::pin(*state_);
    if (!pin.has_value()) {
        return Result<View>::failure(std::move(pin).error());
    }
    if (state_->is_sequence) {
        return Result<View>::failure(type_mismatch(state_->diagnostic_path));
    }
    const auto* const reference = std::get_if<RefCursor>(&state_->cursor);
    if (reference == nullptr) {
        return Result<View>::failure(type_mismatch(state_->diagnostic_path));
    }
    if (!reference->present) {
        return Result<View>::failure(
            unexpected_null(state_->diagnostic_path));
    }
    auto target = state_->owner->index.ref_target(*reference);
    if (!target.has_value()) {
        return Result<View>::failure(std::move(target).error());
    }
    auto child = std::make_shared<View::State>();
    child->is_backed = true;
    child->owner = state_->owner;
    child->generation = state_->generation;
    child->cursor = target.value();
    child->diagnostic_path = state_->diagnostic_path.append("target");
    return Result<View>::success(View{std::move(child)});
} catch (const std::bad_alloc&) {
    return Result<View>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<View>::failure(allocation_error());
}

Result<GraphIdentity> View::graph_identity() const try {
    if (state_ == nullptr) {
        return Result<GraphIdentity>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (!state_->is_backed) {
        auto node = GraphViewInternals::detached_node(*state_);
        if (!node.has_value()) {
            return Result<GraphIdentity>::failure(
                std::move(node).error());
        }
        return GraphViewInternals::detached_identity(*state_, *node.value());
    }
    auto pin = GraphViewInternals::pin(*state_);
    if (!pin.has_value()) {
        return Result<GraphIdentity>::failure(std::move(pin).error());
    }
    if (state_->is_sequence ||
        (!std::holds_alternative<IdentityObjectCursor>(state_->cursor) &&
         !std::holds_alternative<RefCursor>(state_->cursor))) {
        return Result<GraphIdentity>::failure(
            type_mismatch(state_->diagnostic_path));
    }
    if (!value_cursor_present(state_->cursor)) {
        return Result<GraphIdentity>::failure(
            unexpected_null(state_->diagnostic_path));
    }
    return state_->owner->index.graph_identity(state_->cursor);
} catch (const std::bad_alloc&) {
    return Result<GraphIdentity>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<GraphIdentity>::failure(allocation_error());
}

}  // namespace fastdb::payload::view
