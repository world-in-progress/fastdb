#include "payload/view/View.hpp"

#include "payload/json/JsonValue.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <fastdb_payload.h>

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
};

Result<View> View::ref_target() const try {
    if (state_ == nullptr) {
        return Result<View>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (!state_->is_backed) {
        return Result<View>::failure(type_mismatch(state_->diagnostic_path));
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
        return Result<GraphIdentity>::failure(
            type_mismatch(state_->diagnostic_path));
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
