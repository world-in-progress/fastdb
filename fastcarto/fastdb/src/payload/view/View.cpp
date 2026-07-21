#include "payload/view/View.hpp"

#include "payload/json/JsonValue.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/TextEncoding.hpp"
#include "payload/view/Materialize.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <fastdb_payload.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fastdb::payload::view {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;
using spec::TypeKind;

Error view_error(std::uint32_t code,
                 const JsonPointer& path,
                 const char* message,
                 const char* reason) {
    return Error::from_details(
        code, path, message,
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{reason}}}));
}

Error allocation_error() {
    return view_error(FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
                      "Portable payload view allocation failed",
                      "allocation_failed");
}

Error type_mismatch(const JsonPointer& path) {
    return view_error(FDB_PAYLOAD_E_TYPE_MISMATCH, path,
                      "Portable payload view kind does not match the operation",
                      "view_kind_mismatch");
}

Error unexpected_null(const JsonPointer& path) {
    return view_error(FDB_PAYLOAD_E_UNEXPECTED_NULL, path,
                      "Portable payload view is null", "unexpected_null");
}

Error internal_error(const JsonPointer& path, const char* reason) {
    return view_error(FDB_PAYLOAD_E_INTERNAL, path,
                      "Portable payload view state is inconsistent", reason);
}

ViewKind view_kind(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return ViewKind::boolean;
    case TypeKind::u8:
        return ViewKind::u8;
    case TypeKind::u16:
        return ViewKind::u16;
    case TypeKind::u32:
        return ViewKind::u32;
    case TypeKind::i32:
        return ViewKind::i32;
    case TypeKind::u8n:
        return ViewKind::u8n;
    case TypeKind::u16n:
        return ViewKind::u16n;
    case TypeKind::f32:
        return ViewKind::f32;
    case TypeKind::f64:
        return ViewKind::f64;
    case TypeKind::str:
        return ViewKind::str;
    case TypeKind::wstr:
        return ViewKind::wstr;
    case TypeKind::bytes:
        return ViewKind::bytes;
    case TypeKind::component:
        return ViewKind::component;
    case TypeKind::list:
        return ViewKind::list;
    case TypeKind::ref:
        return ViewKind::ref;
    }
    return ViewKind::ref;
}

bool variable_kind(TypeKind kind) noexcept {
    return kind == TypeKind::str || kind == TypeKind::wstr ||
           kind == TypeKind::bytes;
}

struct PinnedImage final {
    PinnedImage(std::shared_ptr<PayloadOwnerState> owner_value,
                AccessPin pin_value,
                const std::uint8_t* bytes_value,
                std::uint64_t byte_count_value) noexcept
        : owner(std::move(owner_value)),
          pin(std::move(pin_value)),
          bytes(bytes_value),
          byte_count(byte_count_value) {}

    PinnedImage(const PinnedImage&) = delete;
    PinnedImage& operator=(const PinnedImage&) = delete;
    PinnedImage(PinnedImage&&) noexcept = default;
    PinnedImage& operator=(PinnedImage&&) = delete;

    std::shared_ptr<PayloadOwnerState> owner;
    AccessPin pin;
    const std::uint8_t* bytes;
    std::uint64_t byte_count;
};

Result<void> decode_wide(std::vector<std::uint16_t>& output,
                         const std::uint8_t* bytes,
                         std::uint64_t byte_count,
                         const JsonPointer& path) {
    if ((byte_count & UINT64_C(1)) != UINT64_C(0)) {
        return Result<void>::failure(view_error(
            FDB_PAYLOAD_E_INVALID_TEXT_ENCODING, path,
            "Payload text encoding is invalid", "odd_byte_length"));
    }
    const std::uint64_t count = byte_count / UINT64_C(2);
    if (count > static_cast<std::uint64_t>(output.max_size())) {
        return Result<void>::failure(allocation_error());
    }
    output.resize(static_cast<std::size_t>(count));
    for (std::uint64_t index = UINT64_C(0); index < count; ++index) {
        auto offset = layout::checked_multiply_u64(index, UINT64_C(2), path);
        if (!offset.has_value()) {
            return Result<void>::failure(std::move(offset).error());
        }
        auto unit = layout::load_u16_le(bytes, byte_count, offset.value(),
                                        path);
        if (!unit.has_value()) {
            return Result<void>::failure(std::move(unit).error());
        }
        output[static_cast<std::size_t>(index)] = unit.value();
    }
    return Result<void>::success();
}

}  // namespace

struct ViewInternals final {
    static Result<PinnedImage> pin(const View::State& state) {
        if (!state.is_backed || state.owner == nullptr) {
            return Result<PinnedImage>::failure(
                internal_error(state.diagnostic_path,
                               "backed_view_state_missing"));
        }
        auto pin_result =
            AccessPin::acquire(state.owner->barrier, state.generation);
        if (!pin_result.has_value()) {
            return Result<PinnedImage>::failure(
                std::move(pin_result).error());
        }
        if (!state.owner->backing.has_value()) {
            return Result<PinnedImage>::failure(
                internal_error(state.diagnostic_path,
                               "committed_backing_missing"));
        }
        return Result<PinnedImage>::success(PinnedImage{
            state.owner, std::move(pin_result).value(),
            state.owner->backing->readable_data(),
            state.owner->backing->readable_size()});
    }

    static Result<const build::ValueNode*> detached_node(
        const View::State& state) {
        if (state.is_backed || state.detached == nullptr ||
            state.node >= state.detached->arena.nodes().size()) {
            return Result<const build::ValueNode*>::failure(
                internal_error(state.diagnostic_path,
                               "detached_node_missing"));
        }
        return Result<const build::ValueNode*>::success(
            &state.detached->arena.nodes()[
                static_cast<std::size_t>(state.node)]);
    }

    static Result<ViewKind> detached_kind(const View::State& state) {
        auto node = detached_node(state);
        if (!node.has_value()) {
            return Result<ViewKind>::failure(std::move(node).error());
        }
        if (node.value()->tag == build::ValueTag::sequence) {
            return Result<ViewKind>::success(ViewKind::sequence);
        }
        if (node.value()->tag == build::ValueTag::object_record ||
            node.value()->tag == build::ValueTag::object_root ||
            (node.value()->tag == build::ValueTag::null_value &&
             node.value()->runtime_type_id == UINT32_MAX &&
             node.value()->object_component_index != UINT32_MAX)) {
            return Result<ViewKind>::success(ViewKind::component);
        }
        if (node.value()->tag == build::ValueTag::reference) {
            return Result<ViewKind>::success(ViewKind::ref);
        }
        if (state.detached->runtime_schema == nullptr) {
            return Result<ViewKind>::failure(
                internal_error(state.diagnostic_path,
                               "detached_runtime_schema_missing"));
        }
        const layout::RuntimeType* const type =
            state.detached->runtime_schema->find_type(
                node.value()->runtime_type_id);
        if (type == nullptr || type->source == nullptr) {
            return Result<ViewKind>::failure(
                internal_error(state.diagnostic_path,
                               "detached_runtime_type_missing"));
        }
        return Result<ViewKind>::success(view_kind(type->source->kind));
    }

    static Result<const build::ValueNode*> detached_component_node(
        const View::State& state) {
        auto kind = detached_kind(state);
        if (!kind.has_value()) {
            return Result<const build::ValueNode*>::failure(
                std::move(kind).error());
        }
        if (kind.value() != ViewKind::component) {
            return Result<const build::ValueNode*>::failure(
                type_mismatch(state.diagnostic_path));
        }
        auto node = detached_node(state);
        if (!node.has_value()) {
            return Result<const build::ValueNode*>::failure(
                std::move(node).error());
        }
        if (node.value()->tag == build::ValueTag::null_value) {
            return Result<const build::ValueNode*>::failure(
                unexpected_null(state.diagnostic_path));
        }
        if (node.value()->tag == build::ValueTag::component) {
            return node;
        }
        if (node.value()->tag != build::ValueTag::object_root &&
            node.value()->tag != build::ValueTag::object_record) {
            return Result<const build::ValueNode*>::failure(
                internal_error(state.diagnostic_path,
                               "detached_component_tag_invalid"));
        }
        const build::ValueNode* const record =
            state.detached->object_record(
                node.value()->object_component_index,
                node.value()->object_id);
        if (record == nullptr ||
            (node.value()->tag == build::ValueTag::object_record &&
             record != node.value())) {
            return Result<const build::ValueNode*>::failure(
                internal_error(state.diagnostic_path,
                               "detached_object_coordinate_invalid"));
        }
        return Result<const build::ValueNode*>::success(record);
    }

    static Result<View> backed_value(const View::State& parent,
                                     ValueCursor cursor,
                                     JsonPointer path) try {
        auto state = std::make_shared<View::State>();
        state->is_backed = true;
        state->owner = parent.owner;
        state->generation = parent.generation;
        state->cursor = cursor;
        state->diagnostic_path = std::move(path);
        return Result<View>::success(View{std::move(state)});
    } catch (const std::bad_alloc&) {
        return Result<View>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<View>::failure(allocation_error());
    }

    static Result<View> detached_value(const View::State& parent,
                                       build::NodeIndex node,
                                       JsonPointer path) try {
        auto state = std::make_shared<View::State>();
        state->detached = parent.detached;
        state->node = node;
        state->diagnostic_path = std::move(path);
        return Result<View>::success(View{std::move(state)});
    } catch (const std::bad_alloc&) {
        return Result<View>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<View>::failure(allocation_error());
    }

    static Result<build::NodeIndex> detached_child(
        const View::State& state,
        const build::ValueNode& parent,
        std::uint64_t index) {
        if (index >= parent.child_count) {
            return Result<build::NodeIndex>::failure(view_error(
                FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
                state.diagnostic_path.append(index),
                "Portable payload child index is out of range",
                "child_index"));
        }
        const auto& nodes = state.detached->arena.nodes();
        if (parent.first_child >= nodes.size() ||
            parent.child_count >
                static_cast<std::uint64_t>(nodes.size()) -
                    parent.first_child) {
            return Result<build::NodeIndex>::failure(
                internal_error(state.diagnostic_path,
                               "detached_child_range_invalid"));
        }
        const build::NodeIndex child = parent.first_child + index;
        const build::NodeIndex expected_next =
            index + UINT64_C(1) < parent.child_count
                ? child + UINT64_C(1)
                : build::invalid_node_index;
        if (nodes[static_cast<std::size_t>(child)].next_sibling !=
            expected_next) {
            return Result<build::NodeIndex>::failure(
                internal_error(state.diagnostic_path,
                               "detached_child_sequence_invalid"));
        }
        return Result<build::NodeIndex>::success(child);
    }

    static Result<std::uint64_t> scalar_bits(const View::State& state,
                                             TypeKind expected) {
        if (state.is_backed) {
            auto image = pin(state);
            if (!image.has_value()) {
                return Result<std::uint64_t>::failure(
                    std::move(image).error());
            }
            if (state.is_sequence ||
                value_cursor_kind(state.cursor) != expected) {
                return Result<std::uint64_t>::failure(
                    type_mismatch(state.diagnostic_path));
            }
            if (!value_cursor_present(state.cursor)) {
                return Result<std::uint64_t>::failure(
                    unexpected_null(state.diagnostic_path));
            }
            auto observed = image.value().owner->index.scalar_observation(
                image.value().bytes, image.value().byte_count,
                state.cursor);
            if (!observed.has_value()) {
                return Result<std::uint64_t>::failure(
                    std::move(observed).error());
            }
            return Result<std::uint64_t>::success(observed.value().bits);
        }
        auto kind = detached_kind(state);
        if (!kind.has_value()) {
            return Result<std::uint64_t>::failure(std::move(kind).error());
        }
        if (kind.value() != view_kind(expected)) {
            return Result<std::uint64_t>::failure(
                type_mismatch(state.diagnostic_path));
        }
        auto node = detached_node(state);
        if (!node.has_value()) {
            return Result<std::uint64_t>::failure(std::move(node).error());
        }
        if (node.value()->tag == build::ValueTag::null_value) {
            return Result<std::uint64_t>::failure(
                unexpected_null(state.diagnostic_path));
        }
        return Result<std::uint64_t>::success(
            node.value()->scalar_bits_or_offset);
    }

    static Result<std::uint64_t> checked_scalar_bits(
        const std::shared_ptr<const View::State>& state,
        TypeKind expected) try {
        if (state == nullptr) {
            return Result<std::uint64_t>::failure(
                internal_error(JsonPointer{}, "view_state_missing"));
        }
        return scalar_bits(*state, expected);
    } catch (const std::bad_alloc&) {
        return Result<std::uint64_t>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<std::uint64_t>::failure(allocation_error());
    }

    static Result<Access> backed_access(const View::State& state) try {
        auto image = pin(state);
        if (!image.has_value()) {
            return Result<Access>::failure(std::move(image).error());
        }
        if (state.is_sequence ||
            !variable_kind(value_cursor_kind(state.cursor))) {
            return Result<Access>::failure(
                type_mismatch(state.diagnostic_path));
        }
        if (!value_cursor_present(state.cursor)) {
            return Result<Access>::failure(
                unexpected_null(state.diagnostic_path));
        }
        auto span = value_cursor_kind(state.cursor) == TypeKind::bytes
            ? image.value().owner->index.variable_span(
                  image.value().bytes, image.value().byte_count,
                  state.cursor)
            : image.value().owner->index.text_span(
                  image.value().bytes, image.value().byte_count,
                  state.cursor, state.diagnostic_path);
        if (!span.has_value()) {
            return Result<Access>::failure(std::move(span).error());
        }
        const std::shared_ptr<PayloadOwnerState> owner = image.value().owner;
        const std::uint8_t* const image_bytes = image.value().bytes;
        auto access = std::make_unique<Access::State>();
        access->owner = owner;
        access->kind = span.value().kind == TypeKind::str
                           ? AccessKind::str
                       : span.value().kind == TypeKind::wstr
                           ? AccessKind::wstr
                           : AccessKind::bytes;
        access->bytes =
            image_bytes +
            static_cast<std::ptrdiff_t>(span.value().data_offset);
        access->byte_count = span.value().byte_length;
        if (span.value().kind == TypeKind::wstr) {
            auto decoded = decode_wide(access->wide_units, access->bytes,
                                       access->byte_count,
                                       state.diagnostic_path);
            if (!decoded.has_value()) {
                return Result<Access>::failure(std::move(decoded).error());
            }
        }
        access->pin.emplace(std::move(image).value().pin);
        return Result<Access>::success(Access{std::move(access)});
    } catch (const std::bad_alloc&) {
        return Result<Access>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<Access>::failure(allocation_error());
    }

    static Result<Access> detached_access(const View::State& state) try {
        auto kind = detached_kind(state);
        if (!kind.has_value()) {
            return Result<Access>::failure(std::move(kind).error());
        }
        if (kind.value() != ViewKind::str && kind.value() != ViewKind::wstr &&
            kind.value() != ViewKind::bytes) {
            return Result<Access>::failure(
                type_mismatch(state.diagnostic_path));
        }
        auto node = detached_node(state);
        if (!node.has_value()) {
            return Result<Access>::failure(std::move(node).error());
        }
        if (node.value()->tag == build::ValueTag::null_value) {
            return Result<Access>::failure(
                unexpected_null(state.diagnostic_path));
        }
        const auto& storage = state.detached->arena.byte_storage();
        auto bounded = layout::checked_range_end(
            node.value()->scalar_bits_or_offset, node.value()->byte_length,
            static_cast<std::uint64_t>(storage.size()),
            state.diagnostic_path);
        if (!bounded.has_value()) {
            return Result<Access>::failure(std::move(bounded).error());
        }
        const std::uint8_t* const data =
            storage.empty()
                ? nullptr
                : storage.data() + static_cast<std::ptrdiff_t>(
                                       node.value()->scalar_bits_or_offset);
        if (kind.value() == ViewKind::str) {
            const char* const text =
                node.value()->byte_length == UINT64_C(0)
                    ? ""
                    : reinterpret_cast<const char*>(data);
            auto valid = layout::validate_utf8(
                std::string_view{
                    text,
                    static_cast<std::size_t>(node.value()->byte_length)},
                state.diagnostic_path);
            if (!valid.has_value()) {
                return Result<Access>::failure(std::move(valid).error());
            }
        } else if (kind.value() == ViewKind::wstr) {
            auto valid = layout::validate_utf16le(
                data, node.value()->byte_length, state.diagnostic_path);
            if (!valid.has_value()) {
                return Result<Access>::failure(std::move(valid).error());
            }
        }
        auto access = std::make_unique<Access::State>();
        access->detached = state.detached;
        access->kind = kind.value() == ViewKind::str
                           ? AccessKind::str
                       : kind.value() == ViewKind::wstr
                           ? AccessKind::wstr
                           : AccessKind::bytes;
        access->bytes = data;
        access->byte_count = node.value()->byte_length;
        if (kind.value() == ViewKind::wstr) {
            auto decoded = decode_wide(access->wide_units, data,
                                       node.value()->byte_length,
                                       state.diagnostic_path);
            if (!decoded.has_value()) {
                return Result<Access>::failure(std::move(decoded).error());
            }
        }
        return Result<Access>::success(Access{std::move(access)});
    } catch (const std::bad_alloc&) {
        return Result<Access>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<Access>::failure(allocation_error());
    }
};

Access::Access(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

Access::Access(Access&&) noexcept = default;
Access& Access::operator=(Access&&) noexcept = default;
Access::~Access() = default;

Result<ByteSpan> Access::payload_bytes() const try {
    if (state_ == nullptr || state_->kind != AccessKind::payload) {
        return Result<ByteSpan>::failure(
            type_mismatch(JsonPointer{}.append("access")));
    }
    return Result<ByteSpan>::success(
        ByteSpan{state_->bytes, state_->byte_count});
} catch (const std::bad_alloc&) {
    return Result<ByteSpan>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<ByteSpan>::failure(allocation_error());
}

Result<ByteSpan> Access::str() const try {
    if (state_ == nullptr || state_->kind != AccessKind::str) {
        return Result<ByteSpan>::failure(
            type_mismatch(JsonPointer{}.append("access")));
    }
    return Result<ByteSpan>::success(
        ByteSpan{state_->bytes, state_->byte_count});
} catch (const std::bad_alloc&) {
    return Result<ByteSpan>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<ByteSpan>::failure(allocation_error());
}

Result<WideSpan> Access::wstr() const try {
    if (state_ == nullptr || state_->kind != AccessKind::wstr) {
        return Result<WideSpan>::failure(
            type_mismatch(JsonPointer{}.append("access")));
    }
    return Result<WideSpan>::success(WideSpan{
        state_->wide_units.data(),
        static_cast<std::uint64_t>(state_->wide_units.size())});
} catch (const std::bad_alloc&) {
    return Result<WideSpan>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<WideSpan>::failure(allocation_error());
}

Result<ByteSpan> Access::bytes() const try {
    if (state_ == nullptr || state_->kind != AccessKind::bytes) {
        return Result<ByteSpan>::failure(
            type_mismatch(JsonPointer{}.append("access")));
    }
    return Result<ByteSpan>::success(
        ByteSpan{state_->bytes, state_->byte_count});
} catch (const std::bad_alloc&) {
    return Result<ByteSpan>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<ByteSpan>::failure(allocation_error());
}

View::View(std::shared_ptr<const State> state) noexcept
    : state_(std::move(state)) {}

Result<ViewKind> View::kind() const try {
    if (state_ == nullptr) {
        return Result<ViewKind>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (!state_->is_backed) {
        return ViewInternals::detached_kind(*state_);
    }
    auto image = ViewInternals::pin(*state_);
    if (!image.has_value()) {
        return Result<ViewKind>::failure(std::move(image).error());
    }
    return Result<ViewKind>::success(
        state_->is_sequence ? ViewKind::sequence
                            : view_kind(value_cursor_kind(state_->cursor)));
} catch (const std::bad_alloc&) {
    return Result<ViewKind>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<ViewKind>::failure(allocation_error());
}

Result<bool> View::is_null() const try {
    if (state_ == nullptr) {
        return Result<bool>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (state_->is_backed) {
        auto image = ViewInternals::pin(*state_);
        if (!image.has_value()) {
            return Result<bool>::failure(std::move(image).error());
        }
        return Result<bool>::success(
            !state_->is_sequence && !value_cursor_present(state_->cursor));
    }
    auto node = ViewInternals::detached_node(*state_);
    if (!node.has_value()) {
        return Result<bool>::failure(std::move(node).error());
    }
    return Result<bool>::success(
        node.value()->tag == build::ValueTag::null_value);
} catch (const std::bad_alloc&) {
    return Result<bool>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<bool>::failure(allocation_error());
}

Result<std::uint64_t> View::length() const try {
    if (state_ == nullptr) {
        return Result<std::uint64_t>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (state_->is_backed) {
        auto image = ViewInternals::pin(*state_);
        if (!image.has_value()) {
            return Result<std::uint64_t>::failure(
                std::move(image).error());
        }
        if (state_->is_sequence) {
            return image.value().owner->index.sequence_length(
                image.value().bytes, image.value().byte_count,
                state_->sequence);
        }
        if (value_cursor_kind(state_->cursor) != TypeKind::list) {
            return Result<std::uint64_t>::failure(
                type_mismatch(state_->diagnostic_path));
        }
        if (!value_cursor_present(state_->cursor)) {
            return Result<std::uint64_t>::failure(
                unexpected_null(state_->diagnostic_path));
        }
        return image.value().owner->index.list_length(
            image.value().bytes, image.value().byte_count, state_->cursor);
    }
    auto kind_result = ViewInternals::detached_kind(*state_);
    if (!kind_result.has_value()) {
        return Result<std::uint64_t>::failure(
            std::move(kind_result).error());
    }
    if (kind_result.value() != ViewKind::sequence &&
        kind_result.value() != ViewKind::list) {
        return Result<std::uint64_t>::failure(
            type_mismatch(state_->diagnostic_path));
    }
    auto node = ViewInternals::detached_node(*state_);
    if (!node.has_value()) {
        return Result<std::uint64_t>::failure(std::move(node).error());
    }
    if (node.value()->tag == build::ValueTag::null_value) {
        return Result<std::uint64_t>::failure(
            unexpected_null(state_->diagnostic_path));
    }
    return Result<std::uint64_t>::success(node.value()->child_count);
} catch (const std::bad_alloc&) {
    return Result<std::uint64_t>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<std::uint64_t>::failure(allocation_error());
}

Result<View> View::at(std::uint64_t index) const try {
    if (state_ == nullptr) {
        return Result<View>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (state_->is_backed) {
        auto image = ViewInternals::pin(*state_);
        if (!image.has_value()) {
            return Result<View>::failure(std::move(image).error());
        }
        if (state_->is_sequence) {
            auto count = image.value().owner->index.sequence_length(
                image.value().bytes, image.value().byte_count,
                state_->sequence);
            if (!count.has_value()) {
                return Result<View>::failure(std::move(count).error());
            }
            if (index >= count.value()) {
                return Result<View>::failure(view_error(
                    FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
                    state_->diagnostic_path.append(index),
                    "Portable payload child index is out of range",
                    "child_index"));
            }
            auto child = image.value().owner->index.entry_value(
                image.value().bytes, image.value().byte_count,
                state_->sequence, index);
            if (!child.has_value()) {
                return Result<View>::failure(std::move(child).error());
            }
            return ViewInternals::backed_value(
                *state_, child.value(),
                state_->diagnostic_path.append(index));
        }
        if (value_cursor_kind(state_->cursor) != TypeKind::list) {
            return Result<View>::failure(
                type_mismatch(state_->diagnostic_path));
        }
        if (!value_cursor_present(state_->cursor)) {
            return Result<View>::failure(
                unexpected_null(state_->diagnostic_path));
        }
        auto count = image.value().owner->index.list_length(
            image.value().bytes, image.value().byte_count,
            state_->cursor);
        if (!count.has_value()) {
            return Result<View>::failure(std::move(count).error());
        }
        if (index >= count.value()) {
            return Result<View>::failure(view_error(
                FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
                state_->diagnostic_path.append(index),
                "Portable payload child index is out of range",
                "child_index"));
        }
        auto child = image.value().owner->index.list_item(
            image.value().bytes, image.value().byte_count,
            state_->cursor, index);
        if (!child.has_value()) {
            return Result<View>::failure(std::move(child).error());
        }
        return ViewInternals::backed_value(
            *state_, child.value(), state_->diagnostic_path.append(index));
    }
    auto kind_result = ViewInternals::detached_kind(*state_);
    if (!kind_result.has_value()) {
        return Result<View>::failure(std::move(kind_result).error());
    }
    if (kind_result.value() != ViewKind::sequence &&
        kind_result.value() != ViewKind::list) {
        return Result<View>::failure(type_mismatch(state_->diagnostic_path));
    }
    auto node = ViewInternals::detached_node(*state_);
    if (!node.has_value()) {
        return Result<View>::failure(std::move(node).error());
    }
    if (node.value()->tag == build::ValueTag::null_value) {
        return Result<View>::failure(unexpected_null(state_->diagnostic_path));
    }
    auto child = ViewInternals::detached_child(*state_, *node.value(), index);
    if (!child.has_value()) {
        return Result<View>::failure(std::move(child).error());
    }
    return ViewInternals::detached_value(
        *state_, child.value(), state_->diagnostic_path.append(index));
} catch (const std::bad_alloc&) {
    return Result<View>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<View>::failure(allocation_error());
}

Result<std::uint32_t> View::component_index() const try {
    if (state_ == nullptr) {
        return Result<std::uint32_t>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (state_->is_backed) {
        auto image = ViewInternals::pin(*state_);
        if (!image.has_value()) {
            return Result<std::uint32_t>::failure(
                std::move(image).error());
        }
        if (state_->is_sequence ||
            value_cursor_kind(state_->cursor) != TypeKind::component) {
            return Result<std::uint32_t>::failure(
                type_mismatch(state_->diagnostic_path));
        }
        if (!value_cursor_present(state_->cursor)) {
            return Result<std::uint32_t>::failure(
                unexpected_null(state_->diagnostic_path));
        }
        return image.value().owner->index.component_index(
            image.value().bytes, image.value().byte_count, state_->cursor);
    }
    auto kind_result = ViewInternals::detached_kind(*state_);
    if (!kind_result.has_value()) {
        return Result<std::uint32_t>::failure(
            std::move(kind_result).error());
    }
    if (kind_result.value() != ViewKind::component) {
        return Result<std::uint32_t>::failure(
            type_mismatch(state_->diagnostic_path));
    }
    auto node = ViewInternals::detached_node(*state_);
    if (!node.has_value()) {
        return Result<std::uint32_t>::failure(std::move(node).error());
    }
    if (node.value()->tag == build::ValueTag::null_value) {
        return Result<std::uint32_t>::failure(
            unexpected_null(state_->diagnostic_path));
    }
    if (node.value()->tag == build::ValueTag::object_root ||
        node.value()->tag == build::ValueTag::object_record) {
        const build::ValueNode* const record =
            state_->detached->object_record(
                node.value()->object_component_index,
                node.value()->object_id);
        if (record == nullptr ||
            (node.value()->tag == build::ValueTag::object_record &&
             record != node.value())) {
            return Result<std::uint32_t>::failure(
                internal_error(state_->diagnostic_path,
                               "detached_object_coordinate_invalid"));
        }
        return Result<std::uint32_t>::success(
            node.value()->object_component_index);
    }
    const layout::RuntimeType* const type =
        state_->detached->runtime_schema->find_type(
            node.value()->runtime_type_id);
    if (type == nullptr || type->source == nullptr) {
        return Result<std::uint32_t>::failure(
            internal_error(state_->diagnostic_path,
                           "detached_runtime_type_missing"));
    }
    return Result<std::uint32_t>::success(
        type->source->resolved_component_index);
} catch (const std::bad_alloc&) {
    return Result<std::uint32_t>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<std::uint32_t>::failure(allocation_error());
}

Result<std::uint32_t> View::field_count() const try {
    if (state_ == nullptr) {
        return Result<std::uint32_t>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (state_->is_backed) {
        auto image = ViewInternals::pin(*state_);
        if (!image.has_value()) {
            return Result<std::uint32_t>::failure(
                std::move(image).error());
        }
        if (state_->is_sequence ||
            value_cursor_kind(state_->cursor) != TypeKind::component) {
            return Result<std::uint32_t>::failure(
                type_mismatch(state_->diagnostic_path));
        }
        if (!value_cursor_present(state_->cursor)) {
            return Result<std::uint32_t>::failure(
                unexpected_null(state_->diagnostic_path));
        }
        return image.value().owner->index.component_field_count(
            image.value().bytes, image.value().byte_count, state_->cursor);
    }
    auto index = component_index();
    if (!index.has_value()) {
        return Result<std::uint32_t>::failure(std::move(index).error());
    }
    const layout::ComponentLayout* const component =
        state_->detached->runtime_schema->component(index.value());
    if (component == nullptr || component->fields.size() > UINT32_MAX) {
        return Result<std::uint32_t>::failure(
            internal_error(state_->diagnostic_path,
                           "detached_component_layout_missing"));
    }
    return Result<std::uint32_t>::success(
        static_cast<std::uint32_t>(component->fields.size()));
} catch (const std::bad_alloc&) {
    return Result<std::uint32_t>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<std::uint32_t>::failure(allocation_error());
}

Result<View> View::field(std::uint32_t index) const try {
    if (state_ == nullptr) {
        return Result<View>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    if (state_->is_backed) {
        auto image = ViewInternals::pin(*state_);
        if (!image.has_value()) {
            return Result<View>::failure(std::move(image).error());
        }
        if (state_->is_sequence ||
            value_cursor_kind(state_->cursor) != TypeKind::component) {
            return Result<View>::failure(type_mismatch(state_->diagnostic_path));
        }
        if (!value_cursor_present(state_->cursor)) {
            return Result<View>::failure(
                unexpected_null(state_->diagnostic_path));
        }
        auto count = image.value().owner->index.component_field_count(
            image.value().bytes, image.value().byte_count, state_->cursor);
        if (!count.has_value()) {
            return Result<View>::failure(std::move(count).error());
        }
        if (index >= count.value()) {
            return Result<View>::failure(view_error(
                FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
                state_->diagnostic_path.append("fields").append(index),
                "Portable payload field index is out of range",
                "field_index"));
        }
        auto child = image.value().owner->index.component_field(
            image.value().bytes, image.value().byte_count, state_->cursor,
            index);
        if (!child.has_value()) {
            return Result<View>::failure(std::move(child).error());
        }
        return ViewInternals::backed_value(
            *state_, child.value(),
            state_->diagnostic_path.append("fields").append(index));
    }
    auto count = field_count();
    if (!count.has_value()) {
        return Result<View>::failure(std::move(count).error());
    }
    if (index >= count.value()) {
        return Result<View>::failure(view_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
            state_->diagnostic_path.append("fields").append(index),
            "Portable payload field index is out of range", "field_index"));
    }
    auto parent = ViewInternals::detached_component_node(*state_);
    if (!parent.has_value()) {
        return Result<View>::failure(std::move(parent).error());
    }
    auto child =
        ViewInternals::detached_child(*state_, *parent.value(), index);
    if (!child.has_value()) {
        return Result<View>::failure(std::move(child).error());
    }
    return ViewInternals::detached_value(
        *state_, child.value(),
        state_->diagnostic_path.append("fields").append(index));
} catch (const std::bad_alloc&) {
    return Result<View>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<View>::failure(allocation_error());
}

Result<std::uint8_t> View::get_bool() const {
    auto bits =
        ViewInternals::checked_scalar_bits(state_, TypeKind::boolean);
    return bits.has_value()
               ? Result<std::uint8_t>::success(
                     static_cast<std::uint8_t>(bits.value()))
               : Result<std::uint8_t>::failure(std::move(bits).error());
}

Result<std::uint8_t> View::get_u8() const {
    auto bits = ViewInternals::checked_scalar_bits(state_, TypeKind::u8);
    return bits.has_value()
               ? Result<std::uint8_t>::success(
                     static_cast<std::uint8_t>(bits.value()))
               : Result<std::uint8_t>::failure(std::move(bits).error());
}

Result<std::uint16_t> View::get_u16() const {
    auto bits = ViewInternals::checked_scalar_bits(state_, TypeKind::u16);
    return bits.has_value()
               ? Result<std::uint16_t>::success(
                     static_cast<std::uint16_t>(bits.value()))
               : Result<std::uint16_t>::failure(std::move(bits).error());
}

Result<std::uint32_t> View::get_u32() const {
    auto bits = ViewInternals::checked_scalar_bits(state_, TypeKind::u32);
    return bits.has_value()
               ? Result<std::uint32_t>::success(
                     static_cast<std::uint32_t>(bits.value()))
               : Result<std::uint32_t>::failure(std::move(bits).error());
}

Result<std::int32_t> View::get_i32() const {
    auto bits = ViewInternals::checked_scalar_bits(state_, TypeKind::i32);
    if (!bits.has_value()) {
        return Result<std::int32_t>::failure(std::move(bits).error());
    }
    const std::uint32_t wire = static_cast<std::uint32_t>(bits.value());
    std::int32_t value = INT32_C(0);
    std::memcpy(&value, &wire, sizeof(value));
    return Result<std::int32_t>::success(value);
}

Result<std::uint64_t> View::get_u8n_f64_bits() const {
    return ViewInternals::checked_scalar_bits(state_, TypeKind::u8n);
}

Result<std::uint64_t> View::get_u16n_f64_bits() const {
    return ViewInternals::checked_scalar_bits(state_, TypeKind::u16n);
}

Result<std::uint32_t> View::get_f32_bits() const {
    auto bits = ViewInternals::checked_scalar_bits(state_, TypeKind::f32);
    return bits.has_value()
               ? Result<std::uint32_t>::success(
                     static_cast<std::uint32_t>(bits.value()))
               : Result<std::uint32_t>::failure(std::move(bits).error());
}

Result<std::uint64_t> View::get_f64_bits() const {
    return ViewInternals::checked_scalar_bits(state_, TypeKind::f64);
}

Result<Access> View::acquire() const try {
    if (state_ == nullptr) {
        return Result<Access>::failure(
            internal_error(JsonPointer{}, "view_state_missing"));
    }
    return state_->is_backed ? ViewInternals::backed_access(*state_)
                             : ViewInternals::detached_access(*state_);
} catch (const std::bad_alloc&) {
    return Result<Access>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<Access>::failure(allocation_error());
}

Result<View> View::materialize() const { return view::materialize(*this); }

}  // namespace fastdb::payload::view
