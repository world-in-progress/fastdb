#include "payload/view/Open.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/InputSpan.hpp"
#include "payload/layout/NormalizedInteger.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/view/OpenCommon.hpp"

#include "payload/layout/TextEncoding.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::payload::view {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonPointerBuilder;
using json::JsonValue;
using layout::EntryDescriptor;
using layout::RegionDescriptor;
using layout::RegionKind;
using layout::RuntimeSchema;
using spec::Cardinality;
using spec::TypeKind;
using open_common::RegionFields;
using open_common::WorkCounter;
using open_common::allocation_error;
using open_common::binary_entry_path;
using open_common::binary_header_path;
using open_common::binary_region_path;
using open_common::byte_hex;
using open_common::noncanonical;
using open_common::noncanonical_exact;
using open_common::read_region_fields;
using open_common::read_u32;
using open_common::read_u64;
using open_common::require_canonical_u64;
using open_common::require_zero;
using open_common::resource_error;
using open_common::simple_error;
using open_common::validate_region_fields;

Error invalid_value(const JsonPointer& path, const char* reason) {
    return simple_error(FDB_PAYLOAD_E_INVALID_BINARY_VALUE, path,
                        "Portable payload binary value is invalid", reason);
}

bool record_kind(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::f32:
    case TypeKind::f64:
    case TypeKind::u8n:
    case TypeKind::u16n:
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
    case TypeKind::component:
    case TypeKind::list:
        return true;
    default:
        return false;
    }
}

bool variable_kind(TypeKind kind) noexcept {
    return kind == TypeKind::str || kind == TypeKind::wstr ||
           kind == TypeKind::bytes;
}

bool container_kind(TypeKind kind) noexcept {
    return kind == TypeKind::component || kind == TypeKind::list;
}

const PoolMetadata* find_pool(const std::vector<PoolMetadata>& pools,
                              TypeKind kind) noexcept {
    const RegionKind wanted = kind == TypeKind::str    ? RegionKind::utf8_pool
                              : kind == TypeKind::wstr ? RegionKind::utf16_pool
                                                       : RegionKind::bytes_pool;
    const auto found = std::find_if(
        pools.begin(), pools.end(),
        [wanted](const PoolMetadata& pool) { return pool.kind == wanted; });
    return found == pools.end() ? nullptr : &*found;
}

struct PoolCursors final {
    std::uint64_t utf8{UINT64_C(0)};
    std::uint64_t utf16{UINT64_C(0)};
    std::uint64_t opaque{UINT64_C(0)};

    std::uint64_t& for_kind(TypeKind kind) noexcept {
        if (kind == TypeKind::str) {
            return utf8;
        }
        if (kind == TypeKind::wstr) {
            return utf16;
        }
        return opaque;
    }
};

struct ListRegionState final {
    std::uint32_t owner_runtime_type_id;
    std::uint32_t item_runtime_type_id;
    std::uint32_t validity_region_index;
    std::uint32_t items_region_index;
    RegionDescriptor validity;
    RegionDescriptor items;
    std::uint64_t cursor{UINT64_C(0)};
};

struct ListPartitions final {
    std::vector<ListRegionState> states;
    std::vector<std::uint32_t> indexes;

    ListRegionState* find(std::uint32_t runtime_type_id) noexcept {
        if (runtime_type_id >= indexes.size()) {
            return nullptr;
        }
        const std::uint32_t index = indexes[runtime_type_id];
        return index < states.size() ? &states[index] : nullptr;
    }
};

Result<void> require_access_span(const std::uint8_t* bytes,
                                 std::uint64_t byte_count,
                                 std::uint64_t expected) {
    if (bytes == nullptr || byte_count != expected ||
        !layout::input_span_is_addressable(byte_count)) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_OUT_OF_BOUNDS, JsonPointer{}.append("bytes"),
            "Portable payload observation span does not match the opened image",
            "observation_span_mismatch"));
    }
    return Result<void>::success();
}

Result<ObservedScalar> observe_scalar(const std::uint8_t* bytes,
                                      std::uint64_t byte_count,
                                      const spec::TypeNode& type,
                                      std::uint64_t offset,
                                      bool present,
                                      const JsonPointer& path) {
    if (!present) {
        return Result<ObservedScalar>::success(
            ObservedScalar{false, type.kind, UINT64_C(0)});
    }
    std::uint64_t bits = UINT64_C(0);
    switch (type.kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u8n: {
        auto end = layout::checked_range_end(offset, UINT64_C(1), byte_count,
                                              path);
        if (!end.has_value()) {
            return Result<ObservedScalar>::failure(std::move(end).error());
        }
        bits = bytes[static_cast<std::ptrdiff_t>(offset)];
        break;
    }
    case TypeKind::u16:
    case TypeKind::u16n: {
        auto loaded = layout::load_u16_le(bytes, byte_count, offset, path);
        if (!loaded.has_value()) {
            return Result<ObservedScalar>::failure(std::move(loaded).error());
        }
        bits = loaded.value();
        break;
    }
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::f32: {
        auto loaded = layout::load_u32_le(bytes, byte_count, offset, path);
        if (!loaded.has_value()) {
            return Result<ObservedScalar>::failure(std::move(loaded).error());
        }
        bits = loaded.value();
        break;
    }
    case TypeKind::f64: {
        auto loaded = layout::load_u64_le(bytes, byte_count, offset, path);
        if (!loaded.has_value()) {
            return Result<ObservedScalar>::failure(std::move(loaded).error());
        }
        bits = loaded.value();
        break;
    }
    default:
        return Result<ObservedScalar>::failure(simple_error(
            FDB_PAYLOAD_E_TYPE_MISMATCH, path,
            "Portable payload observation requires a fixed scalar",
            "observation_not_scalar"));
    }
    if (type.kind == TypeKind::u8n || type.kind == TypeKind::u16n) {
        const std::uint32_t maximum_code =
            type.kind == TypeKind::u8n ? UINT32_C(255) : UINT32_C(65535);
        auto decoded = layout::dequantize_normalized(
            static_cast<std::uint32_t>(bits), type.minimum, type.maximum,
            maximum_code, type.kind == TypeKind::u8n ? "u8n" : "u16n",
            path);
        if (!decoded.has_value()) {
            return Result<ObservedScalar>::failure(std::move(decoded).error());
        }
        bits = decoded.value();
    }
    return Result<ObservedScalar>::success(
        ObservedScalar{true, type.kind, bits});
}

bool noncanonical_f32_nan(std::uint32_t bits) noexcept {
    const bool nan = (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) &&
                     (bits & UINT32_C(0x007fffff)) != UINT32_C(0);
    return nan && bits != UINT32_C(0x7fc00000);
}

bool noncanonical_f64_nan(std::uint64_t bits) noexcept {
    const bool nan =
        (bits & UINT64_C(0x7ff0000000000000)) ==
            UINT64_C(0x7ff0000000000000) &&
        (bits & UINT64_C(0x000fffffffffffff)) != UINT64_C(0);
    return nan && bits != UINT64_C(0x7ff8000000000000);
}

struct ValidationSlot final {
    const spec::TypeNode* type;
    std::uint64_t offset;
    std::uint64_t structural_depth;
    bool present;
    JsonPointerBuilder::Mark path_mark;
};

struct ListContinuation final {
    const spec::TypeNode* item_type;
    ListRegionState* state;
    std::uint64_t first_index;
    std::uint64_t item_count;
    std::uint64_t next_index;
    std::uint64_t structural_depth;
    JsonPointerBuilder::Mark path_mark;
};

struct ComponentContinuation final {
    const layout::ComponentLayout* component;
    const spec::Component* source_component;
    std::uint64_t offset;
    std::uint64_t structural_depth;
    std::size_t next_field;
    JsonPointerBuilder::Mark path_mark;
};

using ValidationFrame =
    std::variant<ValidationSlot, ListContinuation, ComponentContinuation>;

Result<void> validate_fixed_slot(
    const RuntimeSchema& runtime, const std::uint8_t* bytes,
    std::uint64_t byte_count, const spec::TypeNode& root_type,
    std::uint64_t root_offset, bool root_present, WorkCounter& work,
    std::uint64_t max_nesting_depth, const std::vector<PoolMetadata>& pools,
    PoolCursors& pool_cursors, ListPartitions& list_partitions,
    std::vector<VariableSlotMetadata>& variables, bool validate_text_eager,
    const JsonPointer& root_path) {
    std::vector<ValidationFrame> pending;
    JsonPointerBuilder path;
    path.assign(root_path);
    const std::uint64_t root_depth =
        root_present && container_kind(root_type.kind) ? UINT64_C(1)
                                                      : UINT64_C(0);
    pending.push_back(ValidationSlot{&root_type, root_offset, root_depth,
                                     root_present, path.mark()});
    while (!pending.empty()) {
        ComponentContinuation* const component_continuation =
            std::get_if<ComponentContinuation>(&pending.back());
        if (component_continuation != nullptr) {
            path.rewind(component_continuation->path_mark);
            if (component_continuation->next_field ==
                component_continuation->component->fields.size()) {
                pending.pop_back();
                continue;
            }
            const std::size_t index =
                component_continuation->next_field++;
            const layout::ComponentFieldLayout& field =
                component_continuation->component->fields[index];
            const spec::Field& source_field =
                component_continuation->source_component->fields[index];
            const std::uint64_t component_offset =
                component_continuation->offset;
            const std::uint64_t structural_depth =
                component_continuation->structural_depth;
            path.append(source_field.id);
            const JsonPointer field_path = path.snapshot();
            auto field_work = work.charge(UINT64_C(1), field_path);
            if (!field_work.has_value()) {
                return field_work;
            }
            bool present = true;
            if (field.validity_bit != UINT32_MAX) {
                auto validity_offset = layout::checked_add_u64(
                    component_offset,
                    field.validity_bit / UINT32_C(8), field_path);
                if (!validity_offset.has_value()) {
                    return Result<void>::failure(
                        std::move(validity_offset).error());
                }
                const std::uint8_t validity =
                    bytes[static_cast<std::ptrdiff_t>(
                        validity_offset.value())];
                present = ((validity >>
                            (field.validity_bit % UINT32_C(8))) &
                           UINT8_C(1)) != UINT8_C(0);
            }
            auto child_offset = layout::checked_add_u64(
                component_offset, field.offset, field_path);
            if (!child_offset.has_value()) {
                return Result<void>::failure(
                    std::move(child_offset).error());
            }
            std::uint64_t child_depth = structural_depth;
            if (present && container_kind(source_field.type.kind)) {
                auto incremented = layout::checked_add_u64(
                    child_depth, UINT64_C(1), field_path);
                if (!incremented.has_value()) {
                    return Result<void>::failure(
                        std::move(incremented).error());
                }
                child_depth = incremented.value();
            }
            pending.push_back(ValidationSlot{
                &source_field.type, child_offset.value(), child_depth,
                present, path.mark()});
            continue;
        }
        ListContinuation* const continuation =
            std::get_if<ListContinuation>(&pending.back());
        if (continuation != nullptr) {
            path.rewind(continuation->path_mark);
            if (continuation->next_index == continuation->item_count) {
                pending.pop_back();
                continue;
            }
            const std::uint64_t local_index = continuation->next_index++;
            ListRegionState* const state = continuation->state;
            const spec::TypeNode* const item_type =
                continuation->item_type;
            const std::uint64_t first_index = continuation->first_index;
            const std::uint64_t structural_depth =
                continuation->structural_depth;
            path.append(local_index);
            const JsonPointer item_path = path.snapshot();
            auto item_work = work.charge(UINT64_C(1), item_path);
            if (!item_work.has_value()) {
                return item_work;
            }
            auto aggregate_index = layout::checked_add_u64(
                first_index, local_index, item_path);
            if (!aggregate_index.has_value()) {
                return Result<void>::failure(
                    std::move(aggregate_index).error());
            }
            auto relative = layout::checked_multiply_u64(
                aggregate_index.value(), state->items.stride, item_path);
            if (!relative.has_value()) {
                return Result<void>::failure(std::move(relative).error());
            }
            auto absolute = layout::checked_add_u64(
                state->items.data_offset, relative.value(), item_path);
            if (!absolute.has_value()) {
                return Result<void>::failure(std::move(absolute).error());
            }
            bool present = true;
            if (state->validity_region_index != UINT32_MAX) {
                auto validity_byte = layout::checked_add_u64(
                    state->validity.data_offset,
                    aggregate_index.value() / UINT64_C(8), item_path);
                if (!validity_byte.has_value()) {
                    return Result<void>::failure(
                        std::move(validity_byte).error());
                }
                present =
                    ((bytes[static_cast<std::ptrdiff_t>(
                          validity_byte.value())] >>
                      (aggregate_index.value() % UINT64_C(8))) &
                     UINT8_C(1)) != UINT8_C(0);
            }
            std::uint64_t child_depth = structural_depth;
            if (present && container_kind(item_type->kind)) {
                auto incremented = layout::checked_add_u64(
                    child_depth, UINT64_C(1), item_path);
                if (!incremented.has_value()) {
                    return Result<void>::failure(
                        std::move(incremented).error());
                }
                child_depth = incremented.value();
            }
            pending.push_back(ValidationSlot{
                item_type, absolute.value(), child_depth, present,
                path.mark()});
            continue;
        }
        ValidationSlot slot =
            std::move(std::get<ValidationSlot>(pending.back()));
        pending.pop_back();
        path.rewind(slot.path_mark);
        const JsonPointer slot_path = path.snapshot();
        if (slot.present && container_kind(slot.type->kind) &&
            slot.structural_depth > max_nesting_depth) {
            return Result<void>::failure(resource_error(
                slot_path, "nesting_depth", slot.structural_depth,
                max_nesting_depth));
        }
        const std::uint32_t type_id = runtime.runtime_id(*slot.type);
        const layout::RuntimeType* runtime_type = runtime.find_type(type_id);
        if (type_id == UINT32_MAX || runtime_type == nullptr) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, slot_path,
                "Portable payload slot type metadata is missing",
                "slot_runtime_type_missing"));
        }
        auto slot_end = layout::checked_range_end(
            slot.offset, runtime_type->slot.stride, byte_count, slot_path);
        if (!slot_end.has_value()) {
            return Result<void>::failure(std::move(slot_end).error());
        }
        if (!slot.present) {
            auto zero = require_zero(
                bytes, byte_count, slot.offset, slot_end.value(), work,
                slot_path, "nonzero_null_storage",
                slot.type->kind == TypeKind::component);
            if (!zero.has_value()) {
                return zero;
            }
            if (variable_kind(slot.type->kind)) {
                variables.push_back(
                    VariableSlotMetadata{slot.type->kind, slot.offset,
                                         UINT64_C(0), UINT64_C(0), false});
            }
            continue;
        }
        if (variable_kind(slot.type->kind)) {
            auto relative_offset =
                read_u64(bytes, byte_count, slot.offset, slot_path);
            auto length_offset =
                layout::checked_add_u64(slot.offset, UINT64_C(8), slot_path);
            if (!length_offset.has_value()) {
                return Result<void>::failure(std::move(length_offset).error());
            }
            auto byte_length =
                read_u64(bytes, byte_count, length_offset.value(), slot_path);
            if (!relative_offset.has_value() || !byte_length.has_value()) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_OUT_OF_BOUNDS, slot_path,
                    "Portable payload variable descriptor is outside the image",
                    "variable_descriptor_out_of_bounds"));
            }
            if (slot.type->kind == TypeKind::wstr &&
                ((relative_offset.value() | byte_length.value()) &
                 UINT64_C(1)) != UINT64_C(0)) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_MISALIGNED, slot_path,
                    "Portable payload UTF-16LE descriptor is misaligned",
                    "utf16_descriptor_misaligned"));
            }
            const PoolMetadata* const pool = find_pool(pools, slot.type->kind);
            if (pool == nullptr) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, slot_path,
                    "Portable payload variable pool metadata is missing",
                    "variable_pool_missing"));
            }
            std::uint64_t& cursor = pool_cursors.for_kind(slot.type->kind);
            auto advanced = layout::checked_partition_advance(
                cursor, relative_offset.value(), byte_length.value(),
                pool->byte_length, slot_path);
            if (!advanced.has_value()) {
                return Result<void>::failure(std::move(advanced).error());
            }
            cursor = advanced.value();
            auto absolute = layout::checked_add_u64(
                pool->data_offset, relative_offset.value(), slot_path);
            if (!absolute.has_value()) {
                return Result<void>::failure(std::move(absolute).error());
            }
            auto bounded = layout::checked_range_end(
                absolute.value(), byte_length.value(), byte_count, slot_path);
            if (!bounded.has_value()) {
                return Result<void>::failure(std::move(bounded).error());
            }
            variables.push_back(VariableSlotMetadata{
                slot.type->kind, slot.offset, relative_offset.value(),
                byte_length.value(), true});
            if (validate_text_eager && slot.type->kind != TypeKind::bytes) {
                const std::uint64_t units =
                    slot.type->kind == TypeKind::str
                        ? byte_length.value()
                        : byte_length.value() / UINT64_C(2);
                auto content_work = work.charge(units, slot_path);
                if (!content_work.has_value()) {
                    return content_work;
                }
                Result<void> valid =
                    slot.type->kind == TypeKind::str
                        ? layout::validate_utf8(
                              std::string_view{
                                  reinterpret_cast<const char*>(bytes) +
                                      static_cast<std::ptrdiff_t>(
                                          absolute.value()),
                                  static_cast<std::size_t>(
                                      byte_length.value())},
                              slot_path)
                        : layout::validate_utf16le(
                              bytes +
                                  static_cast<std::ptrdiff_t>(absolute.value()),
                              byte_length.value(), slot_path);
                if (!valid.has_value()) {
                    return valid;
                }
            }
            continue;
        }
        if (slot.type->kind == TypeKind::list) {
            auto first_index =
                read_u64(bytes, byte_count, slot.offset, slot_path);
            auto count_offset = layout::checked_add_u64(
                slot.offset, UINT64_C(8), slot_path);
            if (!count_offset.has_value()) {
                return Result<void>::failure(
                    std::move(count_offset).error());
            }
            auto item_count = read_u64(bytes, byte_count,
                                       count_offset.value(), slot_path);
            if (!first_index.has_value() || !item_count.has_value()) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_OUT_OF_BOUNDS, slot_path,
                    "Portable payload list descriptor is outside the image",
                    "list_descriptor_out_of_bounds"));
            }
            ListRegionState* const state =
                list_partitions.find(type_id);
            if (state == nullptr || slot.type->items == nullptr ||
                state->item_runtime_type_id !=
                    runtime.runtime_id(*slot.type->items)) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, slot_path,
                    "Portable payload list region metadata is missing",
                    "list_region_metadata_missing"));
            }
            auto first_byte = layout::checked_multiply_u64(
                first_index.value(), state->items.stride, slot_path);
            if (!first_byte.has_value()) {
                return Result<void>::failure(std::move(first_byte).error());
            }
            auto item_bytes = layout::checked_multiply_u64(
                item_count.value(), state->items.stride, slot_path);
            if (!item_bytes.has_value()) {
                return Result<void>::failure(std::move(item_bytes).error());
            }
            auto bounded = layout::checked_range_end(
                first_byte.value(), item_bytes.value(),
                state->items.byte_length, slot_path);
            if (!bounded.has_value()) {
                return Result<void>::failure(std::move(bounded).error());
            }
            auto advanced = layout::checked_partition_advance(
                state->cursor, first_index.value(), item_count.value(),
                state->items.element_count, slot_path);
            if (!advanced.has_value()) {
                return Result<void>::failure(std::move(advanced).error());
            }
            state->cursor = advanced.value();
            if (item_count.value() != UINT64_C(0)) {
                pending.push_back(ListContinuation{
                    slot.type->items.get(), state, first_index.value(),
                    item_count.value(), UINT64_C(0), slot.structural_depth,
                    slot.path_mark});
            }
            continue;
        }
        if (slot.type->kind != TypeKind::component) {
            auto observed = observe_scalar(bytes, byte_count, *slot.type,
                                           slot.offset, true, slot_path);
            if (!observed.has_value()) {
                return Result<void>::failure(std::move(observed).error());
            }
            if (slot.type->kind == TypeKind::boolean &&
                observed.value().bits > UINT64_C(1)) {
                return Result<void>::failure(
                    invalid_value(slot_path, "invalid_boolean_byte"));
            }
            if (slot.type->kind == TypeKind::f32 &&
                noncanonical_f32_nan(
                    static_cast<std::uint32_t>(observed.value().bits))) {
                return Result<void>::failure(
                    noncanonical(slot_path, "noncanonical_f32_nan"));
            }
            if (slot.type->kind == TypeKind::f64 &&
                noncanonical_f64_nan(observed.value().bits)) {
                return Result<void>::failure(
                    noncanonical(slot_path, "noncanonical_f64_nan"));
            }
            continue;
        }

        const layout::ComponentLayout* component = runtime.component(
            slot.type->resolved_component_index);
        if (component == nullptr) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, slot_path,
                "Portable payload component metadata is missing",
                "component_layout_missing"));
        }
        auto validity_end = layout::checked_add_u64(
            slot.offset, component->validity_bytes, slot_path);
        if (!validity_end.has_value()) {
            return Result<void>::failure(std::move(validity_end).error());
        }
        auto charged = work.charge(component->validity_bytes, slot_path);
        if (!charged.has_value()) {
            return charged;
        }
        std::uint32_t nullable_count = UINT32_C(0);
        for (const layout::ComponentFieldLayout& field : component->fields) {
            if (field.validity_bit != UINT32_MAX) {
                ++nullable_count;
            }
        }
        if (component->validity_bytes != UINT32_C(0) &&
            nullable_count % UINT32_C(8) != UINT32_C(0)) {
            auto tail_offset = layout::checked_add_u64(
                slot.offset, component->validity_bytes - UINT32_C(1),
                slot_path);
            if (!tail_offset.has_value()) {
                return Result<void>::failure(
                    std::move(tail_offset).error());
            }
            const std::uint8_t tail = bytes[static_cast<std::ptrdiff_t>(
                tail_offset.value())];
            const std::uint8_t used = static_cast<std::uint8_t>(
                nullable_count % UINT32_C(8));
            const std::uint8_t mask = static_cast<std::uint8_t>(
                UINT8_C(0xff) << used);
            if ((tail & mask) != UINT8_C(0)) {
                return Result<void>::failure(noncanonical(
                    slot_path, "nonzero_component_validity_tail"));
            }
        }

        std::uint64_t cursor = validity_end.value();
        for (const layout::ComponentFieldLayout& field : component->fields) {
            auto field_offset = layout::checked_add_u64(
                slot.offset, field.offset, slot_path);
            if (!field_offset.has_value()) {
                return Result<void>::failure(std::move(field_offset).error());
            }
            auto padding = require_zero(
                bytes, byte_count, cursor, field_offset.value(), work,
                slot_path, "nonzero_component_padding");
            if (!padding.has_value()) {
                return padding;
            }
            auto field_end = layout::checked_add_u64(
                field_offset.value(), field.slot_stride, slot_path);
            if (!field_end.has_value()) {
                return Result<void>::failure(std::move(field_end).error());
            }
            cursor = field_end.value();
        }
        auto tail_padding = require_zero(
            bytes, byte_count, cursor, slot_end.value(), work, slot_path,
            "nonzero_component_padding");
        if (!tail_padding.has_value()) {
            return tail_padding;
        }

        const auto& source_component =
            runtime.spec().resolved().components()[
                slot.type->resolved_component_index];
        if (!component->fields.empty()) {
            pending.push_back(ComponentContinuation{
                component, &source_component, slot.offset,
                slot.structural_depth, 0U, slot.path_mark});
        }
    }
    return Result<void>::success();
}

}  // namespace

OpenOptions default_open_options() noexcept {
    return OpenOptions{true,                 UINT64_C(1) << 30,
                      UINT64_C(1000000),
                      UINT64_C(65536),     UINT64_C(65536),
                      UINT64_C(1024),      UINT64_C(10000000),
                      UINT64_C(10000000),  UINT64_C(1) << 30,
                      UINT64_C(100000000)};
}

std::optional<PoolMetadata>
PayloadIndex::pool_metadata(layout::RegionKind kind) const noexcept {
    const auto found = std::find_if(
        pools_.begin(), pools_.end(),
        [kind](const PoolMetadata& pool) { return pool.kind == kind; });
    return found == pools_.end() ? std::nullopt
                                 : std::optional<PoolMetadata>{*found};
}

std::optional<ListSlotMetadata> PayloadIndex::list_slot(
    std::uint32_t owner_runtime_type_id) const noexcept {
    if (owner_runtime_type_id >= list_slots_.size()) {
        return std::nullopt;
    }
    return list_slots_[owner_runtime_type_id];
}

namespace {

JsonPointer cursor_path(const ValueCursor& cursor) {
    return JsonPointer{}.append("values").append(cursor.slot_offset);
}

Result<const layout::RuntimeType*> checked_cursor_type(
    const PayloadIndex& index,
    const ValueCursor& cursor,
    TypeKind expected) {
    if (cursor.kind != expected) {
        return Result<const layout::RuntimeType*>::failure(simple_error(
            FDB_PAYLOAD_E_TYPE_MISMATCH, cursor_path(cursor),
            "Portable payload view kind does not match the operation",
            "view_kind_mismatch"));
    }
    if (!cursor.present) {
        return Result<const layout::RuntimeType*>::failure(simple_error(
            FDB_PAYLOAD_E_UNEXPECTED_NULL, cursor_path(cursor),
            "Portable payload view is null", "unexpected_null"));
    }
    if (index.runtime_schema() == nullptr) {
        return Result<const layout::RuntimeType*>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload runtime metadata is missing",
            "runtime_schema_missing"));
    }
    const layout::RuntimeType* const type =
        index.runtime_schema()->find_type(cursor.runtime_type_id);
    if (type == nullptr || type->source == nullptr ||
        type->source->kind != cursor.kind) {
        return Result<const layout::RuntimeType*>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload cursor type metadata is inconsistent",
            "cursor_runtime_type_mismatch"));
    }
    return Result<const layout::RuntimeType*>::success(type);
}

struct CheckedListDescriptor final {
    ListSlotMetadata metadata;
    std::uint64_t first_index;
    std::uint64_t item_count;
};

Result<CheckedListDescriptor> checked_list_descriptor(
    const PayloadIndex& index,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor) {
    auto type = checked_cursor_type(index, cursor, TypeKind::list);
    if (!type.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(type).error());
    }
    const auto metadata = index.list_slot(cursor.runtime_type_id);
    if (!metadata.has_value() || type.value()->source->items == nullptr ||
        metadata->item_runtime_type_id !=
            index.runtime_schema()->runtime_id(*type.value()->source->items)) {
        return Result<CheckedListDescriptor>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload list cursor metadata is missing",
            "list_cursor_metadata_missing"));
    }
    auto first = layout::load_u64_le(bytes, byte_count, cursor.slot_offset,
                                     cursor_path(cursor));
    auto count_offset = layout::checked_add_u64(
        cursor.slot_offset, UINT64_C(8), cursor_path(cursor));
    if (!first.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(first).error());
    }
    if (!count_offset.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(count_offset).error());
    }
    auto count = layout::load_u64_le(bytes, byte_count, count_offset.value(),
                                     cursor_path(cursor));
    if (!count.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(count).error());
    }
    auto end_index = layout::checked_range_end(
        first.value(), count.value(), metadata->item_count,
        cursor_path(cursor));
    if (!end_index.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(end_index).error());
    }
    auto relative = layout::checked_multiply_u64(
        first.value(), metadata->item_stride, cursor_path(cursor));
    auto byte_length = layout::checked_multiply_u64(
        count.value(), metadata->item_stride, cursor_path(cursor));
    if (!relative.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(relative).error());
    }
    if (!byte_length.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(byte_length).error());
    }
    auto data_offset = layout::checked_add_u64(
        metadata->item_data_offset, relative.value(), cursor_path(cursor));
    if (!data_offset.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(data_offset).error());
    }
    auto bounded = layout::checked_range_end(
        data_offset.value(), byte_length.value(), byte_count,
        cursor_path(cursor));
    if (!bounded.has_value()) {
        return Result<CheckedListDescriptor>::failure(
            std::move(bounded).error());
    }
    return Result<CheckedListDescriptor>::success(CheckedListDescriptor{
        *metadata, first.value(), count.value()});
}

}  // namespace

Result<EntrySequenceCursor> PayloadIndex::entry_sequence(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint32_t entry_index) const {
    auto span = require_access_span(bytes, byte_count, total_length_);
    if (!span.has_value()) {
        return Result<EntrySequenceCursor>::failure(std::move(span).error());
    }
    if (entry_index >= entries_.size()) {
        return Result<EntrySequenceCursor>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
            JsonPointer{}.append("entries").append(entry_index),
            "Portable payload entry index is out of range", "entry_index"));
    }
    return Result<EntrySequenceCursor>::success(
        EntrySequenceCursor{entry_index});
}

Result<std::uint64_t> PayloadIndex::sequence_length(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    EntrySequenceCursor sequence) const {
    auto verified = entry_sequence(bytes, byte_count, sequence.entry_index);
    if (!verified.has_value()) {
        return Result<std::uint64_t>::failure(std::move(verified).error());
    }
    return Result<std::uint64_t>::success(
        entries_[sequence.entry_index].value_count);
}

Result<ValueCursor> PayloadIndex::entry_value(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    EntrySequenceCursor sequence,
    std::uint64_t value_index) const {
    auto verified = entry_sequence(bytes, byte_count, sequence.entry_index);
    if (!verified.has_value()) {
        return Result<ValueCursor>::failure(std::move(verified).error());
    }
    const EntrySlotMetadata& entry = entries_[sequence.entry_index];
    const JsonPointer path =
        JsonPointer{}.append("entries").append(sequence.entry_index).append(
            value_index);
    if (value_index >= entry.value_count) {
        return Result<ValueCursor>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE, path,
            "Portable payload value index is out of range", "value_index"));
    }
    bool present = true;
    if (entry.has_validity) {
        auto validity_offset = layout::checked_add_u64(
            entry.validity_offset, value_index / UINT64_C(8), path);
        if (!validity_offset.has_value()) {
            return Result<ValueCursor>::failure(
                std::move(validity_offset).error());
        }
        auto bounded = layout::checked_range_end(
            validity_offset.value(), UINT64_C(1), byte_count, path);
        if (!bounded.has_value()) {
            return Result<ValueCursor>::failure(std::move(bounded).error());
        }
        present = ((bytes[static_cast<std::ptrdiff_t>(
                         validity_offset.value())] >>
                    (value_index % UINT64_C(8))) &
                   UINT8_C(1)) != UINT8_C(0);
    }
    auto relative = layout::checked_multiply_u64(
        value_index, entry.stride, path);
    if (!relative.has_value()) {
        return Result<ValueCursor>::failure(std::move(relative).error());
    }
    auto offset = layout::checked_add_u64(entry.data_offset, relative.value(),
                                          path);
    if (!offset.has_value()) {
        return Result<ValueCursor>::failure(std::move(offset).error());
    }
    auto bounded = layout::checked_range_end(offset.value(), entry.stride,
                                             byte_count, path);
    if (!bounded.has_value()) {
        return Result<ValueCursor>::failure(std::move(bounded).error());
    }
    return Result<ValueCursor>::success(ValueCursor{
        entry.runtime_type_id, entry.kind, offset.value(), present});
}

Result<std::uint32_t> PayloadIndex::component_index(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor) const {
    auto span = require_access_span(bytes, byte_count, total_length_);
    if (!span.has_value()) {
        return Result<std::uint32_t>::failure(std::move(span).error());
    }
    auto type = checked_cursor_type(*this, cursor, TypeKind::component);
    if (!type.has_value()) {
        return Result<std::uint32_t>::failure(std::move(type).error());
    }
    return Result<std::uint32_t>::success(
        type.value()->source->resolved_component_index);
}

Result<std::uint32_t> PayloadIndex::component_field_count(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor) const {
    auto component_index_result = component_index(bytes, byte_count, cursor);
    if (!component_index_result.has_value()) {
        return Result<std::uint32_t>::failure(
            std::move(component_index_result).error());
    }
    const layout::ComponentLayout* const component =
        runtime_schema_->component(component_index_result.value());
    if (component == nullptr || component->fields.size() > UINT32_MAX) {
        return Result<std::uint32_t>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload component field metadata is missing",
            "component_layout_missing"));
    }
    return Result<std::uint32_t>::success(
        static_cast<std::uint32_t>(component->fields.size()));
}

Result<ValueCursor> PayloadIndex::component_field(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor,
    std::uint32_t field_index) const {
    auto component_index_result = component_index(bytes, byte_count, cursor);
    if (!component_index_result.has_value()) {
        return Result<ValueCursor>::failure(
            std::move(component_index_result).error());
    }
    const layout::ComponentLayout* const component =
        runtime_schema_->component(component_index_result.value());
    if (component == nullptr) {
        return Result<ValueCursor>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload component field metadata is missing",
            "component_layout_missing"));
    }
    if (field_index >= component->fields.size()) {
        return Result<ValueCursor>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
            cursor_path(cursor).append("fields").append(field_index),
            "Portable payload field index is out of range", "field_index"));
    }
    const layout::ComponentFieldLayout& field =
        component->fields[field_index];
    const layout::RuntimeType* const field_type =
        runtime_schema_->find_type(field.runtime_type_id);
    if (field_type == nullptr || field_type->source == nullptr) {
        return Result<ValueCursor>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload component field type is missing",
            "field_runtime_type_missing"));
    }
    const JsonPointer path =
        cursor_path(cursor).append("fields").append(field_index);
    bool present = true;
    if (field.validity_bit != UINT32_MAX) {
        auto validity_offset = layout::checked_add_u64(
            cursor.slot_offset, field.validity_bit / UINT32_C(8), path);
        if (!validity_offset.has_value()) {
            return Result<ValueCursor>::failure(
                std::move(validity_offset).error());
        }
        auto bounded = layout::checked_range_end(
            validity_offset.value(), UINT64_C(1), byte_count, path);
        if (!bounded.has_value()) {
            return Result<ValueCursor>::failure(std::move(bounded).error());
        }
        present = ((bytes[static_cast<std::ptrdiff_t>(
                         validity_offset.value())] >>
                    (field.validity_bit % UINT32_C(8))) &
                   UINT8_C(1)) != UINT8_C(0);
    }
    auto offset = layout::checked_add_u64(cursor.slot_offset, field.offset,
                                          path);
    if (!offset.has_value()) {
        return Result<ValueCursor>::failure(std::move(offset).error());
    }
    auto bounded = layout::checked_range_end(
        offset.value(), field.slot_stride, byte_count, path);
    if (!bounded.has_value()) {
        return Result<ValueCursor>::failure(std::move(bounded).error());
    }
    return Result<ValueCursor>::success(ValueCursor{
        field.runtime_type_id, field_type->source->kind, offset.value(),
        present});
}

Result<std::uint64_t> PayloadIndex::list_length(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor) const {
    auto span = require_access_span(bytes, byte_count, total_length_);
    if (!span.has_value()) {
        return Result<std::uint64_t>::failure(std::move(span).error());
    }
    auto descriptor = checked_list_descriptor(*this, bytes, byte_count,
                                              cursor);
    if (!descriptor.has_value()) {
        return Result<std::uint64_t>::failure(
            std::move(descriptor).error());
    }
    return Result<std::uint64_t>::success(descriptor.value().item_count);
}

Result<ValueCursor> PayloadIndex::list_item(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor,
    std::uint64_t item_index) const {
    auto span = require_access_span(bytes, byte_count, total_length_);
    if (!span.has_value()) {
        return Result<ValueCursor>::failure(std::move(span).error());
    }
    auto descriptor = checked_list_descriptor(*this, bytes, byte_count,
                                              cursor);
    if (!descriptor.has_value()) {
        return Result<ValueCursor>::failure(
            std::move(descriptor).error());
    }
    const JsonPointer path = cursor_path(cursor).append(item_index);
    if (item_index >= descriptor.value().item_count) {
        return Result<ValueCursor>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE, path,
            "Portable payload list index is out of range", "list_index"));
    }
    auto aggregate_index = layout::checked_add_u64(
        descriptor.value().first_index, item_index, path);
    if (!aggregate_index.has_value()) {
        return Result<ValueCursor>::failure(
            std::move(aggregate_index).error());
    }
    bool present = true;
    if (descriptor.value().metadata.has_validity) {
        auto validity_offset = layout::checked_add_u64(
            descriptor.value().metadata.validity_offset,
            aggregate_index.value() / UINT64_C(8), path);
        if (!validity_offset.has_value()) {
            return Result<ValueCursor>::failure(
                std::move(validity_offset).error());
        }
        auto bounded = layout::checked_range_end(
            validity_offset.value(), UINT64_C(1), byte_count, path);
        if (!bounded.has_value()) {
            return Result<ValueCursor>::failure(std::move(bounded).error());
        }
        present = ((bytes[static_cast<std::ptrdiff_t>(
                         validity_offset.value())] >>
                    (aggregate_index.value() % UINT64_C(8))) &
                   UINT8_C(1)) != UINT8_C(0);
    }
    auto relative = layout::checked_multiply_u64(
        aggregate_index.value(), descriptor.value().metadata.item_stride,
        path);
    if (!relative.has_value()) {
        return Result<ValueCursor>::failure(std::move(relative).error());
    }
    auto offset = layout::checked_add_u64(
        descriptor.value().metadata.item_data_offset, relative.value(), path);
    if (!offset.has_value()) {
        return Result<ValueCursor>::failure(std::move(offset).error());
    }
    auto bounded = layout::checked_range_end(
        offset.value(), descriptor.value().metadata.item_stride, byte_count,
        path);
    if (!bounded.has_value()) {
        return Result<ValueCursor>::failure(std::move(bounded).error());
    }
    const layout::RuntimeType* const item_type = runtime_schema_->find_type(
        descriptor.value().metadata.item_runtime_type_id);
    if (item_type == nullptr || item_type->source == nullptr) {
        return Result<ValueCursor>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, path,
            "Portable payload list item type is missing",
            "list_item_runtime_type_missing"));
    }
    return Result<ValueCursor>::success(ValueCursor{
        descriptor.value().metadata.item_runtime_type_id,
        item_type->source->kind, offset.value(), present});
}

Result<ObservedScalar> PayloadIndex::scalar_observation(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor) const {
    auto span = require_access_span(bytes, byte_count, total_length_);
    if (!span.has_value()) {
        return Result<ObservedScalar>::failure(std::move(span).error());
    }
    if (runtime_schema_ == nullptr) {
        return Result<ObservedScalar>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload runtime metadata is missing",
            "runtime_schema_missing"));
    }
    const layout::RuntimeType* const type =
        runtime_schema_->find_type(cursor.runtime_type_id);
    if (type == nullptr || type->source == nullptr ||
        type->source->kind != cursor.kind) {
        return Result<ObservedScalar>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload scalar cursor metadata is inconsistent",
            "cursor_runtime_type_mismatch"));
    }
    return observe_scalar(bytes, byte_count, *type->source,
                          cursor.slot_offset, cursor.present,
                          cursor_path(cursor));
}

Result<VariableSpanMetadata> PayloadIndex::variable_span(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor) const {
    auto span = require_access_span(bytes, byte_count, total_length_);
    if (!span.has_value()) {
        return Result<VariableSpanMetadata>::failure(std::move(span).error());
    }
    if (!variable_kind(cursor.kind)) {
        return Result<VariableSpanMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_TYPE_MISMATCH, cursor_path(cursor),
            "Portable payload span acquisition requires text or bytes",
            "view_kind_mismatch"));
    }
    if (!cursor.present) {
        return Result<VariableSpanMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_UNEXPECTED_NULL, cursor_path(cursor),
            "Portable payload view is null", "unexpected_null"));
    }
    if (runtime_schema_ == nullptr) {
        return Result<VariableSpanMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload runtime metadata is missing",
            "runtime_schema_missing"));
    }
    const layout::RuntimeType* const type =
        runtime_schema_->find_type(cursor.runtime_type_id);
    if (type == nullptr || type->source == nullptr ||
        type->source->kind != cursor.kind) {
        return Result<VariableSpanMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload span cursor metadata is inconsistent",
            "cursor_runtime_type_mismatch"));
    }
    auto relative = layout::load_u64_le(bytes, byte_count,
                                        cursor.slot_offset,
                                        cursor_path(cursor));
    auto length_offset = layout::checked_add_u64(
        cursor.slot_offset, UINT64_C(8), cursor_path(cursor));
    if (!relative.has_value()) {
        return Result<VariableSpanMetadata>::failure(
            std::move(relative).error());
    }
    if (!length_offset.has_value()) {
        return Result<VariableSpanMetadata>::failure(
            std::move(length_offset).error());
    }
    auto length = layout::load_u64_le(bytes, byte_count,
                                      length_offset.value(),
                                      cursor_path(cursor));
    if (!length.has_value()) {
        return Result<VariableSpanMetadata>::failure(
            std::move(length).error());
    }
    if (cursor.kind == TypeKind::wstr &&
        ((relative.value() | length.value()) & UINT64_C(1)) != UINT64_C(0)) {
        return Result<VariableSpanMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_MISALIGNED, cursor_path(cursor),
            "Portable payload UTF-16LE descriptor is misaligned",
            "utf16_descriptor_misaligned"));
    }
    const PoolMetadata* const pool = find_pool(pools_, cursor.kind);
    if (pool == nullptr) {
        return Result<VariableSpanMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, cursor_path(cursor),
            "Portable payload variable pool metadata is missing",
            "variable_pool_missing"));
    }
    auto relative_end = layout::checked_range_end(
        relative.value(), length.value(), pool->byte_length,
        cursor_path(cursor));
    if (!relative_end.has_value()) {
        return Result<VariableSpanMetadata>::failure(
            std::move(relative_end).error());
    }
    auto absolute = layout::checked_add_u64(
        pool->data_offset, relative.value(), cursor_path(cursor));
    if (!absolute.has_value()) {
        return Result<VariableSpanMetadata>::failure(
            std::move(absolute).error());
    }
    auto bounded = layout::checked_range_end(
        absolute.value(), length.value(), byte_count, cursor_path(cursor));
    if (!bounded.has_value()) {
        return Result<VariableSpanMetadata>::failure(
            std::move(bounded).error());
    }
    return Result<VariableSpanMetadata>::success(VariableSpanMetadata{
        cursor.kind, absolute.value(), length.value()});
}

Result<VariableSpanMetadata> PayloadIndex::text_span(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    ValueCursor cursor,
    const JsonPointer& diagnostic_path) const {
    if (cursor.kind != TypeKind::str && cursor.kind != TypeKind::wstr) {
        return Result<VariableSpanMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_TYPE_MISMATCH, diagnostic_path,
            "Portable payload text acquisition requires a text value",
            "view_kind_mismatch"));
    }
    auto span = variable_span(bytes, byte_count, cursor);
    if (!span.has_value()) {
        return span;
    }
    if (span.value().byte_length > retained_max_string_bytes_) {
        return Result<VariableSpanMetadata>::failure(resource_error(
            diagnostic_path, "string_bytes", span.value().byte_length,
            retained_max_string_bytes_));
    }
    if (!text_validated_eagerly_) {
        const std::uint64_t units =
            cursor.kind == TypeKind::str
                ? span.value().byte_length
                : span.value().byte_length / UINT64_C(2);
        auto total_work = layout::checked_add_u64(
            validation_work_, units, diagnostic_path);
        if (!total_work.has_value()) {
            return Result<VariableSpanMetadata>::failure(resource_error(
                diagnostic_path, "validation_work", UINT64_MAX,
                retained_max_validation_work_));
        }
        if (total_work.value() > retained_max_validation_work_) {
            return Result<VariableSpanMetadata>::failure(resource_error(
                diagnostic_path, "validation_work", total_work.value(),
                retained_max_validation_work_));
        }
        Result<void> valid = cursor.kind == TypeKind::str
            ? layout::validate_utf8(
                  std::string_view{
                      span.value().byte_length == UINT64_C(0)
                          ? ""
                          : reinterpret_cast<const char*>(bytes) +
                                static_cast<std::ptrdiff_t>(
                                    span.value().data_offset),
                      static_cast<std::size_t>(span.value().byte_length)},
                  diagnostic_path)
            : layout::validate_utf16le(
                  bytes + static_cast<std::ptrdiff_t>(
                              span.value().data_offset),
                  span.value().byte_length, diagnostic_path);
        if (!valid.has_value()) {
            return Result<VariableSpanMetadata>::failure(
                std::move(valid).error());
        }
    }
    return span;
}

Result<EntrySlotMetadata> PayloadIndex::entry_slot(
    std::uint32_t entry_index) const {
    if (entry_index >= entries_.size()) {
        return Result<EntrySlotMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
            JsonPointer{}.append("entries").append(entry_index),
            "Portable payload entry index is out of range", "entry_index"));
    }
    return Result<EntrySlotMetadata>::success(entries_[entry_index]);
}

Result<FieldSlotMetadata> PayloadIndex::field_slot(
    std::uint32_t entry_index,
    const std::uint32_t* field_indexes,
    std::uint64_t field_depth) const {
    if (runtime_schema_ == nullptr || entry_index >= entries_.size()) {
        return Result<FieldSlotMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
            JsonPointer{}.append("entries").append(entry_index),
            "Portable payload entry index is out of range", "entry_index"));
    }
    if (field_indexes == nullptr || field_depth == UINT64_C(0) ||
        field_depth > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Result<FieldSlotMetadata>::failure(simple_error(
            FDB_PAYLOAD_E_INVALID_ARGUMENT,
            JsonPointer{}.append("fields"),
            "Portable payload field path is invalid", "invalid_field_path"));
    }
    const layout::RuntimeType* current =
        runtime_schema_->find_type(entries_[entry_index].runtime_type_id);
    std::uint64_t relative_offset = UINT64_C(0);
    for (std::uint64_t depth = UINT64_C(0); depth < field_depth; ++depth) {
        if (current == nullptr || current->source->kind != TypeKind::component) {
            return Result<FieldSlotMetadata>::failure(simple_error(
                FDB_PAYLOAD_E_TYPE_MISMATCH,
                JsonPointer{}.append("fields").append(depth),
                "Portable payload field path crosses a non-component",
                "field_path_not_component"));
        }
        const layout::ComponentLayout* component = runtime_schema_->component(
            current->source->resolved_component_index);
        if (component == nullptr) {
            return Result<FieldSlotMetadata>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL,
                JsonPointer{}.append("fields").append(depth),
                "Portable payload component metadata is missing",
                "component_layout_missing"));
        }
        const std::uint32_t field_index =
            field_indexes[static_cast<std::size_t>(depth)];
        if (field_index >= component->fields.size()) {
            return Result<FieldSlotMetadata>::failure(simple_error(
                FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
                JsonPointer{}.append("fields").append(depth),
                "Portable payload field index is out of range",
                "field_index"));
        }
        const layout::ComponentFieldLayout& field =
            component->fields[field_index];
        auto added = layout::checked_add_u64(
            relative_offset, field.offset,
            JsonPointer{}.append("fields").append(depth));
        if (!added.has_value()) {
            return Result<FieldSlotMetadata>::failure(
                std::move(added).error());
        }
        relative_offset = added.value();
        current = runtime_schema_->find_type(field.runtime_type_id);
        if (current == nullptr) {
            return Result<FieldSlotMetadata>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL,
                JsonPointer{}.append("fields").append(depth),
                "Portable payload field type metadata is missing",
                "field_runtime_type_missing"));
        }
        if (depth + UINT64_C(1) == field_depth) {
            if (relative_offset > UINT32_MAX) {
                return Result<FieldSlotMetadata>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL,
                    JsonPointer{}.append("fields"),
                    "Portable payload field offset exceeds component stride",
                    "field_offset_out_of_range"));
            }
            return Result<FieldSlotMetadata>::success(FieldSlotMetadata{
                field.runtime_type_id, current->source->kind,
                static_cast<std::uint32_t>(relative_offset),
                field.slot_stride, field.alignment});
        }
    }
    return Result<FieldSlotMetadata>::failure(simple_error(
        FDB_PAYLOAD_E_INVALID_ARGUMENT, JsonPointer{}.append("fields"),
        "Portable payload field path is empty", "invalid_field_path"));
}

Result<ObservedScalar> PayloadIndex::scalar(const std::uint8_t* bytes,
                                            std::uint64_t byte_count,
                                            std::uint32_t entry_index,
                                            std::uint64_t value_index) const {
    auto sequence = entry_sequence(bytes, byte_count, entry_index);
    if (!sequence.has_value()) {
        return Result<ObservedScalar>::failure(
            std::move(sequence).error());
    }
    auto cursor = entry_value(bytes, byte_count, sequence.value(),
                              value_index);
    if (!cursor.has_value()) {
        return Result<ObservedScalar>::failure(std::move(cursor).error());
    }
    return scalar_observation(bytes, byte_count, cursor.value());
}

Result<ObservedScalar> PayloadIndex::field_scalar(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint32_t entry_index,
    std::uint64_t value_index,
    const std::uint32_t* field_indexes,
    std::uint64_t field_depth) const {
    auto target = field_slot(entry_index, field_indexes, field_depth);
    if (!target.has_value()) {
        return Result<ObservedScalar>::failure(std::move(target).error());
    }
    auto sequence = entry_sequence(bytes, byte_count, entry_index);
    if (!sequence.has_value()) {
        return Result<ObservedScalar>::failure(
            std::move(sequence).error());
    }
    auto current = entry_value(bytes, byte_count, sequence.value(),
                               value_index);
    if (!current.has_value()) {
        return Result<ObservedScalar>::failure(std::move(current).error());
    }
    for (std::uint64_t depth = UINT64_C(0); depth < field_depth; ++depth) {
        if (!current.value().present) {
            return Result<ObservedScalar>::success(ObservedScalar{
                false, target.value().kind, UINT64_C(0)});
        }
        auto child = component_field(
            bytes, byte_count, current.value(),
            field_indexes[static_cast<std::size_t>(depth)]);
        if (!child.has_value()) {
            return Result<ObservedScalar>::failure(
                std::move(child).error());
        }
        current = std::move(child);
    }
    return scalar_observation(bytes, byte_count, current.value());
}

Result<PayloadIndex> open_record(const spec::CompiledSpec& compiled,
                                 const std::uint8_t* bytes,
                                 std::uint64_t byte_count,
                                 OpenOptions limits) {
    try {
        auto available = RuntimeSchema::require_record_runtime(compiled);
        if (!available.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(available).error());
        }
        const JsonPointer header_path = binary_header_path();
        if (byte_count > limits.max_total_bytes) {
            return Result<PayloadIndex>::failure(resource_error(
                header_path, "total_bytes", byte_count,
                limits.max_total_bytes));
        }
        if (bytes == nullptr || !layout::input_span_is_addressable(byte_count)) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_OUT_OF_BOUNDS, header_path,
                "Portable payload byte span is not addressable",
                "binary_span_out_of_bounds"));
        }
        if (byte_count < layout::header_size) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_OUT_OF_BOUNDS, header_path,
                "Portable payload is shorter than the fixed header",
                "header_out_of_bounds"));
        }
        WorkCounter work(limits.max_validation_work);
        auto charged = work.charge(UINT64_C(1), header_path);
        if (!charged.has_value()) {
            return Result<PayloadIndex>::failure(std::move(charged).error());
        }
        if (!std::equal(layout::binary_magic.begin(),
                        layout::binary_magic.end(), bytes)) {
            return Result<PayloadIndex>::failure(Error::from_details(
                FDB_PAYLOAD_E_INVALID_MAGIC, header_path.append("magic"),
                "Portable payload magic is invalid",
                JsonValue::object({
                    JsonValue::Member{
                        "actual",
                        JsonValue{byte_hex(bytes,
                                           layout::binary_magic.size())}},
                    JsonValue::Member{
                        "expected",
                        JsonValue{byte_hex(layout::binary_magic.data(),
                                           layout::binary_magic.size())}},
                    JsonValue::Member{"reason", JsonValue{"invalid_magic"}},
                })));
        }
        auto major = layout::load_u16_le(
            bytes, byte_count, layout::header_major_offset,
            header_path.append("major"));
        auto minor = layout::load_u16_le(
            bytes, byte_count, layout::header_minor_offset,
            header_path.append("minor"));
        if (!major.has_value()) {
            return Result<PayloadIndex>::failure(std::move(major).error());
        }
        if (!minor.has_value()) {
            return Result<PayloadIndex>::failure(std::move(minor).error());
        }
        if (major.value() != layout::binary_major) {
            return Result<PayloadIndex>::failure(Error::from_details(
                FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION,
                header_path.append("major"),
                "Portable payload binary version is unsupported",
                JsonValue::object({
                    JsonValue::Member{
                        "actual", JsonValue{std::to_string(major.value())}},
                    JsonValue::Member{
                        "expected",
                        JsonValue{std::to_string(layout::binary_major)}},
                    JsonValue::Member{"reason",
                                      JsonValue{"unsupported_binary_major"}},
                })));
        }
        if (minor.value() != layout::binary_minor) {
            return Result<PayloadIndex>::failure(Error::from_details(
                FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION,
                header_path.append("minor"),
                "Portable payload binary version is unsupported",
                JsonValue::object({
                    JsonValue::Member{
                        "actual", JsonValue{std::to_string(minor.value())}},
                    JsonValue::Member{
                        "expected",
                        JsonValue{std::to_string(layout::binary_minor)}},
                    JsonValue::Member{"reason",
                                      JsonValue{"unsupported_binary_minor"}},
                })));
        }

        auto header_size_value = read_u32(
            bytes, byte_count, layout::header_size_offset,
            header_path.append("size"));
        auto profile = read_u32(bytes, byte_count,
                                layout::header_profile_offset,
                                header_path.append("profile"));
        auto flags = read_u32(bytes, byte_count, layout::header_flags_offset,
                              header_path.append("flags"));
        auto total = read_u64(bytes, byte_count,
                              layout::header_total_length_offset,
                              header_path.append("total_length"));
        if (!header_size_value.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(header_size_value).error());
        }
        if (!profile.has_value()) {
            return Result<PayloadIndex>::failure(std::move(profile).error());
        }
        if (!flags.has_value()) {
            return Result<PayloadIndex>::failure(std::move(flags).error());
        }
        if (!total.has_value()) {
            return Result<PayloadIndex>::failure(std::move(total).error());
        }
        if (header_size_value.value() != layout::header_size) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("size"), "header_size",
                std::to_string(header_size_value.value()),
                std::to_string(layout::header_size)));
        }
        if (profile.value() != FDB_PAYLOAD_PROFILE_RECORD_V1) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("profile"), "profile",
                std::to_string(profile.value()),
                std::to_string(FDB_PAYLOAD_PROFILE_RECORD_V1)));
        }
        if (flags.value() != UINT32_C(0)) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("flags"), "header_flags",
                std::to_string(flags.value()), "0"));
        }
        if (total.value() > byte_count) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_OUT_OF_BOUNDS,
                header_path.append("total_length"),
                "Declared portable payload length exceeds supplied bytes",
                "declared_total_out_of_bounds"));
        }
        if (total.value() != byte_count) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("total_length"), "trailing_binary_bytes",
                std::to_string(total.value()), std::to_string(byte_count)));
        }
        if (!std::equal(compiled.digest().begin(), compiled.digest().end(),
                        bytes + static_cast<std::ptrdiff_t>(
                                    layout::header_spec_digest_offset))) {
            return Result<PayloadIndex>::failure(Error::from_details(
                FDB_PAYLOAD_E_DIGEST_MISMATCH,
                header_path.append("spec_sha256"),
                "Portable payload spec digest does not match",
                JsonValue::object({
                    JsonValue::Member{
                        "actual",
                        JsonValue{byte_hex(
                            bytes + static_cast<std::ptrdiff_t>(
                                        layout::header_spec_digest_offset),
                            compiled.digest().size())}},
                    JsonValue::Member{
                        "expected",
                        JsonValue{byte_hex(compiled.digest().data(),
                                           compiled.digest().size())}},
                    JsonValue::Member{"reason",
                                      JsonValue{"spec_digest_mismatch"}},
                })));
        }
        auto header_reserved = require_zero(
            bytes, byte_count, layout::header_reserved_offset,
            layout::header_size, work, header_path.append("reserved"),
            "nonzero_header_reserved", false, true);
        if (!header_reserved.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(header_reserved).error());
        }

        const std::uint64_t expected_entries = static_cast<std::uint64_t>(
            compiled.resolved().entries().size());
        std::uint64_t expected_regions = expected_entries;
        for (const spec::Entry& entry : compiled.resolved().entries()) {
            if (!entry.type.nullable) {
                continue;
            }
            auto counted = layout::checked_accumulate_u64(
                expected_regions, UINT64_C(1),
                JsonPointer{}.append("regions"));
            if (!counted.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(counted).error());
            }
        }
        const std::uint64_t expected_components =
            static_cast<std::uint64_t>(
                compiled.resolved().components().size());
        std::uint64_t known_minimum_work = UINT64_C(1);
        auto counted_work = layout::checked_accumulate_u64(
            known_minimum_work, expected_entries,
            JsonPointer{}.append("validation_work"));
        if (!counted_work.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(counted_work).error());
        }
        counted_work = layout::checked_accumulate_u64(
            known_minimum_work, expected_regions,
            JsonPointer{}.append("validation_work"));
        if (!counted_work.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(counted_work).error());
        }
        if (expected_entries > limits.max_entries) {
            return Result<PayloadIndex>::failure(resource_error(
                header_path.append("entry_count"), "entries", expected_entries,
                limits.max_entries));
        }
        if (expected_regions > limits.max_regions) {
            return Result<PayloadIndex>::failure(resource_error(
                header_path.append("region_count"), "regions", expected_regions,
                limits.max_regions));
        }
        if (expected_components > limits.max_components) {
            return Result<PayloadIndex>::failure(resource_error(
                header_path, "components",
                expected_components, limits.max_components));
        }
        if (known_minimum_work > limits.max_validation_work) {
            return Result<PayloadIndex>::failure(resource_error(
                header_path, "validation_work",
                known_minimum_work, limits.max_validation_work));
        }

        auto runtime = RuntimeSchema::compile(compiled);
        if (!runtime.has_value()) {
            return Result<PayloadIndex>::failure(std::move(runtime).error());
        }
        const auto& runtime_entries =
            runtime.value().spec().resolved().entries();
        if (runtime_entries.size() != expected_entries) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("entries"),
                "Runtime entry inventory differs from the compiled spec",
                "runtime_entry_inventory_mismatch"));
        }
        for (const spec::Entry& entry : compiled.resolved().entries()) {
            if (!record_kind(entry.type.kind)) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL,
                    JsonPointer{}.append("entries").append(entry.id),
                    "Portable record open reached a forbidden reference type",
                    "record_ref_invariant"));
            }
        }
        std::uint64_t list_region_count = UINT64_C(0);
        for (const layout::ListNodeLayout& list :
             runtime.value().list_nodes()) {
            const std::uint64_t count =
                list.item_nullable ? UINT64_C(2) : UINT64_C(1);
            counted_work = layout::checked_accumulate_u64(
                list_region_count, count,
                JsonPointer{}.append("runtime").append("lists"));
            if (!counted_work.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(counted_work).error());
            }
        }
        const std::uint64_t pool_count =
            (runtime.value().has_utf8_pool() ? UINT64_C(1) : UINT64_C(0)) +
            (runtime.value().has_utf16_pool() ? UINT64_C(1) : UINT64_C(0)) +
            (runtime.value().has_bytes_pool() ? UINT64_C(1) : UINT64_C(0));
        counted_work = layout::checked_accumulate_u64(
            expected_regions, list_region_count,
            JsonPointer{}.append("regions"));
        if (!counted_work.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(counted_work).error());
        }
        counted_work = layout::checked_accumulate_u64(
            expected_regions, pool_count, JsonPointer{}.append("regions"));
        if (!counted_work.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(counted_work).error());
        }
        known_minimum_work = UINT64_C(1);
        counted_work = layout::checked_accumulate_u64(
            known_minimum_work, expected_entries,
            JsonPointer{}.append("validation_work"));
        if (!counted_work.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(counted_work).error());
        }
        counted_work = layout::checked_accumulate_u64(
            known_minimum_work, expected_regions,
            JsonPointer{}.append("validation_work"));
        if (!counted_work.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(counted_work).error());
        }
        if (expected_regions > limits.max_regions) {
            return Result<PayloadIndex>::failure(
                resource_error(header_path.append("region_count"), "regions",
                               expected_regions, limits.max_regions));
        }
        if (known_minimum_work > limits.max_validation_work) {
            return Result<PayloadIndex>::failure(resource_error(
                header_path, "validation_work",
                known_minimum_work, limits.max_validation_work));
        }

        auto region_directory_offset = read_u64(
            bytes, byte_count, layout::header_region_directory_offset,
            header_path.append("region_directory_offset"));
        auto region_count = read_u32(
            bytes, byte_count, layout::header_region_count_offset,
            header_path.append("region_count"));
        auto region_size = read_u32(
            bytes, byte_count, layout::header_region_descriptor_size_offset,
            header_path.append("region_descriptor_size"));
        auto entry_directory_offset = read_u64(
            bytes, byte_count, layout::header_entry_directory_offset,
            header_path.append("entry_directory_offset"));
        auto entry_count = read_u32(
            bytes, byte_count, layout::header_entry_count_offset,
            header_path.append("entry_count"));
        auto entry_size = read_u32(
            bytes, byte_count, layout::header_entry_descriptor_size_offset,
            header_path.append("entry_descriptor_size"));
        auto root_value_count = read_u64(
            bytes, byte_count, layout::header_root_value_count_offset,
            header_path.append("root_value_count"));
        if (!region_directory_offset.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(region_directory_offset).error());
        }
        if (!region_count.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(region_count).error());
        }
        if (!region_size.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(region_size).error());
        }
        if (!entry_directory_offset.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(entry_directory_offset).error());
        }
        if (!entry_count.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(entry_count).error());
        }
        if (!entry_size.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(entry_size).error());
        }
        if (!root_value_count.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(root_value_count).error());
        }
        if (region_directory_offset.value() != layout::header_size) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("region_directory_offset"),
                "region_directory_offset",
                std::to_string(region_directory_offset.value()),
                std::to_string(layout::header_size)));
        }
        if (region_count.value() != expected_regions) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("region_count"), "region_count",
                std::to_string(region_count.value()),
                std::to_string(expected_regions)));
        }
        if (region_size.value() != layout::region_descriptor_size) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("region_descriptor_size"),
                "region_descriptor_size",
                std::to_string(region_size.value()),
                std::to_string(layout::region_descriptor_size)));
        }
        if (entry_count.value() != expected_entries) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("entry_count"), "entry_count",
                std::to_string(entry_count.value()),
                std::to_string(expected_entries)));
        }
        if (entry_size.value() != layout::entry_descriptor_size) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("entry_descriptor_size"),
                "entry_descriptor_size",
                std::to_string(entry_size.value()),
                std::to_string(layout::entry_descriptor_size)));
        }
        auto region_bytes = layout::checked_multiply_u64(
            region_count.value(), layout::region_descriptor_size,
            header_path.append("region_count"));
        if (!region_bytes.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(region_bytes).error());
        }
        auto canonical_entry_offset = layout::checked_add_u64(
            layout::header_size, region_bytes.value(),
            header_path.append("entry_directory_offset"));
        if (!canonical_entry_offset.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(canonical_entry_offset).error());
        }
        if (entry_directory_offset.value() != canonical_entry_offset.value()) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("entry_directory_offset"),
                "entry_directory_not_contiguous",
                std::to_string(entry_directory_offset.value()),
                std::to_string(canonical_entry_offset.value())));
        }
        auto entry_bytes = layout::checked_multiply_u64(
            entry_count.value(), layout::entry_descriptor_size,
            header_path.append("entry_count"));
        if (!entry_bytes.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(entry_bytes).error());
        }
        auto entry_end = layout::checked_range_end(
            entry_directory_offset.value(), entry_bytes.value(), byte_count,
            header_path.append("entry_directory_offset"));
        if (!entry_end.has_value()) {
            return Result<PayloadIndex>::failure(std::move(entry_end).error());
        }

        PayloadIndex output;
        output.entries_.reserve(static_cast<std::size_t>(expected_entries));
        std::vector<EntryDescriptor> entries;
        entries.reserve(static_cast<std::size_t>(expected_entries));
        std::uint64_t summed_roots = UINT64_C(0);
        std::uint32_t next_region = UINT32_C(0);
        for (std::uint32_t index = UINT32_C(0);
             index < entry_count.value(); ++index) {
            const spec::Entry& source = runtime_entries[index];
            const JsonPointer path = binary_entry_path(index);
            charged = work.charge(UINT64_C(1), path);
            if (!charged.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(charged).error());
            }
            const std::uint64_t base =
                entry_directory_offset.value() +
                static_cast<std::uint64_t>(index) *
                    layout::entry_descriptor_size;
            auto actual_index = read_u32(
                bytes, byte_count, base + layout::entry_index_offset,
                path.append("index"));
            if (!actual_index.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(actual_index).error());
            }
            auto type_id = read_u32(
                bytes, byte_count,
                base + layout::entry_runtime_type_id_offset,
                path.append("runtime_type_id"));
            if (!type_id.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(type_id).error());
            }
            auto cardinality = read_u32(
                bytes, byte_count, base + layout::entry_cardinality_offset,
                path.append("cardinality"));
            if (!cardinality.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(cardinality).error());
            }
            auto entry_flags = read_u32(
                bytes, byte_count, base + layout::entry_flags_offset,
                path.append("flags"));
            if (!entry_flags.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(entry_flags).error());
            }
            auto value_count = read_u64(
                bytes, byte_count, base + layout::entry_value_count_offset,
                path.append("value_count"));
            if (!value_count.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(value_count).error());
            }
            auto values_region = read_u32(
                bytes, byte_count,
                base + layout::entry_values_region_index_offset,
                path.append("values_region_index"));
            if (!values_region.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(values_region).error());
            }
            auto validity_region = read_u32(
                bytes, byte_count,
                base + layout::entry_validity_region_index_offset,
                path.append("validity_region_index"));
            if (!validity_region.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(validity_region).error());
            }
            auto reserved = read_u64(
                bytes, byte_count, base + layout::entry_reserved_offset,
                path.append("reserved"));
            if (!reserved.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(reserved).error());
            }
            const std::uint32_t expected_type =
                runtime.value().runtime_id(source.type);
            if (expected_type == UINT32_MAX ||
                runtime.value().find_type(expected_type) == nullptr) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, path,
                    "Runtime entry type is unassigned",
                    "unassigned_runtime_type"));
            }
            const std::uint32_t expected_cardinality =
                source.cardinality == Cardinality::one ? UINT32_C(1)
                                                       : UINT32_C(2);
            const std::uint32_t expected_flags =
                source.type.nullable ? UINT32_C(1) : UINT32_C(0);
            const std::uint32_t expected_validity =
                source.type.nullable ? next_region++ : UINT32_MAX;
            const std::uint32_t expected_values = next_region++;
            if (actual_index.value() != index) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("index"), "entry_index",
                    std::to_string(actual_index.value()),
                    std::to_string(index)));
            }
            if (type_id.value() != expected_type) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("runtime_type_id"), "entry_runtime_type_id",
                    std::to_string(type_id.value()),
                    std::to_string(expected_type)));
            }
            if (cardinality.value() != expected_cardinality) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("cardinality"), "entry_cardinality",
                    std::to_string(cardinality.value()),
                    std::to_string(expected_cardinality)));
            }
            if (entry_flags.value() != expected_flags) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("flags"), "entry_flags",
                    std::to_string(entry_flags.value()),
                    std::to_string(expected_flags)));
            }
            if (values_region.value() != expected_values) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("values_region_index"),
                    "entry_values_region_index",
                    std::to_string(values_region.value()),
                    std::to_string(expected_values)));
            }
            if (validity_region.value() != expected_validity) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("validity_region_index"),
                    "entry_validity_region_index",
                    std::to_string(validity_region.value()),
                    std::to_string(expected_validity)));
            }
            if (reserved.value() != UINT64_C(0)) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("reserved"), "entry_reserved",
                    std::to_string(reserved.value()), "0"));
            }
            if (source.cardinality == Cardinality::one &&
                value_count.value() != UINT64_C(1)) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("value_count"), "entry_one_value_count",
                    std::to_string(value_count.value()), "1"));
            }
            auto sum = layout::checked_add_u64(summed_roots,
                                               value_count.value(), path);
            if (!sum.has_value()) {
                return Result<PayloadIndex>::failure(std::move(sum).error());
            }
            summed_roots = sum.value();
            entries.push_back(EntryDescriptor{
                index, expected_type, expected_cardinality, expected_flags,
                value_count.value(), expected_values, expected_validity});
        }
        if (summed_roots != root_value_count.value()) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("root_value_count"),
                "root_value_count_mismatch",
                std::to_string(root_value_count.value()),
                std::to_string(summed_roots)));
        }

        auto data_start = layout::checked_align_up_u64(
            entry_end.value(), UINT32_C(8),
            header_path.append("entry_directory_offset"));
        if (!data_start.has_value()) {
            return Result<PayloadIndex>::failure(std::move(data_start).error());
        }
        auto directory_padding = require_zero(
            bytes, byte_count, entry_end.value(), data_start.value(), work,
            header_path.append("entry_directory_offset"),
            "nonzero_directory_padding", true, true);
        if (!directory_padding.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(directory_padding).error());
        }

        std::vector<RegionDescriptor> regions;
        regions.reserve(static_cast<std::size_t>(expected_regions));
        std::uint64_t cursor = data_start.value();
        std::uint32_t region_index = UINT32_C(0);
        PoolCursors pool_cursors;
        for (const EntryDescriptor& entry : entries) {
            const spec::Entry& source =
                runtime_entries[entry.entry_index];
            const layout::RuntimeType* const runtime_type =
                runtime.value().find_type(entry.runtime_type_id);
            if (entry.runtime_type_id == UINT32_MAX ||
                runtime_type == nullptr) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL,
                    JsonPointer{}.append("entries").append(source.id),
                    "Runtime entry type is outside the inventory",
                    "runtime_type_id_out_of_range"));
            }
            const layout::SlotLayout slot = runtime_type->slot;
            const std::uint32_t count = source.type.nullable ? UINT32_C(2)
                                                            : UINT32_C(1);
            for (std::uint32_t local = UINT32_C(0); local < count; ++local) {
                const bool validity = source.type.nullable && local == 0U;
                const JsonPointer path =
                    binary_region_path(region_index);
                charged = work.charge(UINT64_C(1), path);
                if (!charged.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(charged).error());
                }
                auto fields = read_region_fields(
                    bytes, byte_count, region_index, path);
                if (!fields.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(fields).error());
                }
                const RegionKind expected_kind =
                    validity ? RegionKind::entry_validity
                             : RegionKind::entry_values;
                const std::uint32_t expected_stride =
                    validity ? UINT32_C(0) : slot.stride;
                const std::uint32_t expected_alignment =
                    validity ? UINT32_C(1) : slot.alignment;
                Result<std::uint64_t> expected_length =
                    layout::checked_multiply_u64(entry.value_count,
                                                 slot.stride,
                                                 binary_entry_path(
                                                     entry.entry_index)
                                                     .append("value_count"));
                if (validity) {
                    auto rounded = layout::checked_add_u64(
                        entry.value_count, UINT64_C(7),
                        binary_entry_path(entry.entry_index)
                            .append("value_count"));
                    if (!rounded.has_value()) {
                        return Result<PayloadIndex>::failure(
                            std::move(rounded).error());
                    }
                    expected_length = Result<std::uint64_t>::success(
                        rounded.value() / UINT64_C(8));
                }
                if (!expected_length.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(expected_length).error());
                }
                if (!validity) {
                    charged = work.charge(entry.value_count, path);
                    if (!charged.has_value()) {
                        return Result<PayloadIndex>::failure(
                            std::move(charged).error());
                    }
                }
                auto aligned = layout::checked_align_up_u64(
                    cursor, expected_alignment, path.append("data_offset"));
                if (!aligned.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(aligned).error());
                }
                if (fields.value().data_offset % expected_alignment !=
                    UINT64_C(0)) {
                    return Result<PayloadIndex>::failure(simple_error(
                        FDB_PAYLOAD_E_MISALIGNED,
                        path.append("data_offset"),
                        "Portable payload region offset is misaligned",
                        "region_offset_misaligned"));
                }
                const RegionFields expected{
                    static_cast<std::uint32_t>(expected_kind), UINT32_C(0),
                    entry.entry_index, entry.runtime_type_id, aligned.value(),
                    expected_length.value(), entry.value_count,
                    expected_stride, expected_alignment, UINT64_C(0)};
                auto descriptor = validate_region_fields(fields.value(),
                                                         expected, path);
                if (!descriptor.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(descriptor).error());
                }
                auto padding = require_zero(
                    bytes, byte_count, cursor, aligned.value(), work,
                    path.append("data_offset"),
                    "nonzero_inter_region_padding", true, true);
                if (!padding.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(padding).error());
                }
                auto end = layout::checked_range_end(
                    fields.value().data_offset,
                    fields.value().byte_length, byte_count,
                    path.append("byte_length"));
                if (!end.has_value()) {
                    return Result<PayloadIndex>::failure(std::move(end).error());
                }
                cursor = end.value();
                regions.push_back(RegionDescriptor{
                    expected_kind, UINT32_C(0), entry.entry_index,
                    entry.runtime_type_id, fields.value().data_offset,
                    fields.value().byte_length, entry.value_count,
                    expected_stride,
                    expected_alignment});
                ++region_index;
            }
        }

        ListPartitions list_partitions;
        list_partitions.indexes.assign(runtime.value().type_count(),
                                       UINT32_MAX);
        output.list_slots_.assign(runtime.value().type_count(), std::nullopt);
        list_partitions.states.reserve(
            runtime.value().list_nodes().size());
        std::uint64_t total_list_elements = UINT64_C(0);
        for (const layout::ListNodeLayout& list :
             runtime.value().list_nodes()) {
            RegionDescriptor validity_descriptor{
                RegionKind::list_validity, UINT32_C(0),
                list.owner_runtime_type_id, list.item_runtime_type_id,
                UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT32_C(0),
                UINT32_C(1)};
            std::uint32_t validity_region_index = UINT32_MAX;
            if (list.item_nullable) {
                const JsonPointer path =
                    binary_region_path(region_index);
                charged = work.charge(UINT64_C(1), path);
                if (!charged.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(charged).error());
                }
                auto fields = read_region_fields(
                    bytes, byte_count, region_index, path);
                if (!fields.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(fields).error());
                }
                auto rounded = layout::checked_add_u64(
                    fields.value().element_count, UINT64_C(7), path);
                if (!rounded.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(rounded).error());
                }
                auto aligned = layout::checked_align_up_u64(
                    cursor, UINT32_C(1), path);
                if (!aligned.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(aligned).error());
                }
                const RegionFields expected{
                    static_cast<std::uint32_t>(RegionKind::list_validity),
                    UINT32_C(0), list.owner_runtime_type_id,
                    list.item_runtime_type_id, aligned.value(),
                    rounded.value() / UINT64_C(8),
                    fields.value().element_count, UINT32_C(0), UINT32_C(1),
                    UINT64_C(0)};
                auto descriptor = validate_region_fields(fields.value(),
                                                         expected, path);
                if (!descriptor.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(descriptor).error());
                }
                auto padding = require_zero(
                    bytes, byte_count, cursor, aligned.value(), work,
                    path.append("data_offset"),
                    "nonzero_inter_region_padding", true, true);
                if (!padding.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(padding).error());
                }
                auto end = layout::checked_range_end(
                    fields.value().data_offset,
                    fields.value().byte_length, byte_count, path);
                if (!end.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(end).error());
                }
                validity_region_index = region_index;
                validity_descriptor = RegionDescriptor{
                    RegionKind::list_validity, UINT32_C(0),
                    list.owner_runtime_type_id, list.item_runtime_type_id,
                    fields.value().data_offset,
                    fields.value().byte_length,
                    fields.value().element_count, UINT32_C(0), UINT32_C(1)};
                regions.push_back(validity_descriptor);
                cursor = end.value();
                ++region_index;
            }

            const JsonPointer path =
                binary_region_path(region_index);
            charged = work.charge(UINT64_C(1), path);
            if (!charged.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(charged).error());
            }
            auto fields = read_region_fields(
                bytes, byte_count, region_index, path);
            if (!fields.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(fields).error());
            }
            auto expected_length = layout::checked_multiply_u64(
                fields.value().element_count, list.item_stride, path);
            if (!expected_length.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(expected_length).error());
            }
            auto aligned = layout::checked_align_up_u64(
                cursor, list.item_alignment, path);
            if (!aligned.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(aligned).error());
            }
            if (fields.value().data_offset % list.item_alignment !=
                UINT64_C(0)) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_MISALIGNED, path,
                    "Portable payload list item region offset is misaligned",
                    "region_offset_misaligned"));
            }
            const RegionFields expected{
                static_cast<std::uint32_t>(RegionKind::list_items),
                UINT32_C(0), list.owner_runtime_type_id,
                list.item_runtime_type_id, aligned.value(),
                expected_length.value(), fields.value().element_count,
                list.item_stride, list.item_alignment, UINT64_C(0)};
            auto descriptor = validate_region_fields(fields.value(), expected,
                                                     path);
            if (!descriptor.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(descriptor).error());
            }
            if (list.item_nullable &&
                validity_descriptor.element_count !=
                    fields.value().element_count) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("element_count"),
                    "list_validity_element_count",
                    std::to_string(fields.value().element_count),
                    std::to_string(validity_descriptor.element_count)));
            }
            auto padding = require_zero(
                bytes, byte_count, cursor, aligned.value(), work,
                path.append("data_offset"),
                "nonzero_inter_region_padding", true, true);
            if (!padding.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(padding).error());
            }
            auto end = layout::checked_range_end(
                fields.value().data_offset, fields.value().byte_length,
                byte_count, path.append("byte_length"));
            if (!end.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(end).error());
            }
            auto accumulated = layout::checked_accumulate_u64(
                total_list_elements, fields.value().element_count,
                path.append("element_count"));
            if (!accumulated.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(accumulated).error());
            }
            if (total_list_elements > limits.max_list_elements) {
                return Result<PayloadIndex>::failure(resource_error(
                    path.append("element_count"),
                    "list_elements", total_list_elements,
                    limits.max_list_elements));
            }
            const RegionDescriptor items_descriptor{
                RegionKind::list_items, UINT32_C(0),
                list.owner_runtime_type_id, list.item_runtime_type_id,
                fields.value().data_offset, fields.value().byte_length,
                fields.value().element_count, list.item_stride,
                list.item_alignment};
            if (list.owner_runtime_type_id >=
                    list_partitions.indexes.size() ||
                list_partitions.indexes[list.owner_runtime_type_id] !=
                    UINT32_MAX) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL,
                    JsonPointer{}.append("runtime").append("lists"),
                    "Portable payload list runtime inventory is invalid",
                    "list_runtime_inventory_mismatch"));
            }
            list_partitions.indexes[list.owner_runtime_type_id] =
                static_cast<std::uint32_t>(list_partitions.states.size());
            list_partitions.states.push_back(ListRegionState{
                list.owner_runtime_type_id, list.item_runtime_type_id,
                validity_region_index, region_index, validity_descriptor,
                items_descriptor, UINT64_C(0)});
            output.list_slots_[list.owner_runtime_type_id] =
                ListSlotMetadata{
                    list.item_runtime_type_id,
                    items_descriptor.data_offset,
                    items_descriptor.element_count,
                    items_descriptor.stride,
                    validity_region_index != UINT32_MAX,
                    validity_region_index == UINT32_MAX
                        ? UINT64_C(0)
                        : validity_descriptor.data_offset,
                    validity_region_index == UINT32_MAX
                        ? UINT64_C(0)
                        : validity_descriptor.byte_length};
            regions.push_back(items_descriptor);
            cursor = end.value();
            ++region_index;
        }

        const std::array<RegionKind, 3> pool_order{{RegionKind::utf8_pool,
                                                    RegionKind::utf16_pool,
                                                    RegionKind::bytes_pool}};
        std::uint64_t text_pool_bytes = UINT64_C(0);
        for (const RegionKind expected_kind : pool_order) {
            const bool required = expected_kind == RegionKind::utf8_pool
                                      ? runtime.value().has_utf8_pool()
                                  : expected_kind == RegionKind::utf16_pool
                                      ? runtime.value().has_utf16_pool()
                                      : runtime.value().has_bytes_pool();
            if (!required) {
                continue;
            }
            const JsonPointer path =
                binary_region_path(region_index);
            charged = work.charge(UINT64_C(1), path);
            if (!charged.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(charged).error());
            }
            auto fields = read_region_fields(
                bytes, byte_count, region_index, path);
            if (!fields.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(fields).error());
            }
            const std::uint32_t expected_alignment =
                expected_kind == RegionKind::utf16_pool ? UINT32_C(2)
                                                        : UINT32_C(1);
            auto aligned =
                layout::checked_align_up_u64(
                    cursor, expected_alignment, path.append("data_offset"));
            if (!aligned.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(aligned).error());
            }
            if (fields.value().data_offset % expected_alignment !=
                UINT64_C(0)) {
                return Result<PayloadIndex>::failure(
                    simple_error(FDB_PAYLOAD_E_MISALIGNED,
                                 path.append("data_offset"),
                                 "Portable payload pool offset is misaligned",
                                 "region_offset_misaligned"));
            }
            if (expected_kind == RegionKind::utf16_pool &&
                fields.value().byte_length % UINT64_C(2) != UINT64_C(0)) {
                return Result<PayloadIndex>::failure(
                    noncanonical_exact(path.append("byte_length"),
                                       "odd_utf16_pool_length",
                                       std::to_string(
                                           fields.value().byte_length),
                                       "even"));
            }
            const std::uint64_t expected_elements =
                expected_kind == RegionKind::utf16_pool
                    ? fields.value().byte_length / UINT64_C(2)
                    : fields.value().byte_length;
            const RegionFields expected{
                static_cast<std::uint32_t>(expected_kind), UINT32_C(0),
                UINT32_MAX, UINT32_MAX, aligned.value(),
                fields.value().byte_length, expected_elements, UINT32_C(0),
                expected_alignment, UINT64_C(0)};
            auto descriptor = validate_region_fields(fields.value(), expected,
                                                     path);
            if (!descriptor.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(descriptor).error());
            }
            auto padding = require_zero(
                bytes, byte_count, cursor, aligned.value(), work,
                path.append("data_offset"), "nonzero_inter_region_padding",
                true, true);
            if (!padding.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(padding).error());
            }
            auto end = layout::checked_range_end(
                fields.value().data_offset, fields.value().byte_length,
                byte_count, path.append("byte_length"));
            if (!end.has_value()) {
                return Result<PayloadIndex>::failure(std::move(end).error());
            }
            if (expected_kind != RegionKind::bytes_pool) {
                auto accumulated = layout::checked_accumulate_u64(
                    text_pool_bytes, fields.value().byte_length,
                    path.append("byte_length"));
                if (!accumulated.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(accumulated).error());
                }
                if (text_pool_bytes > limits.max_string_bytes) {
                    return Result<PayloadIndex>::failure(resource_error(
                        path.append("byte_length"),
                        "string_bytes", text_pool_bytes,
                        limits.max_string_bytes));
                }
            }
            regions.push_back(RegionDescriptor{
                expected_kind, UINT32_C(0), UINT32_MAX, UINT32_MAX,
                fields.value().data_offset, fields.value().byte_length,
                fields.value().element_count, UINT32_C(0),
                expected_alignment});
            output.pools_.push_back(
                PoolMetadata{expected_kind, region_index,
                             fields.value().data_offset,
                             fields.value().byte_length,
                             fields.value().element_count});
            cursor = end.value();
            ++region_index;
        }
        if (region_index != region_count.value()) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("region_count"),
                "region_inventory_not_consumed",
                std::to_string(region_count.value()),
                std::to_string(region_index)));
        }

        auto canonical_total = layout::checked_align_up_u64(
            cursor, UINT32_C(8), header_path.append("total_length"));
        if (!canonical_total.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(canonical_total).error());
        }
        if (canonical_total.value() != byte_count) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                header_path.append("total_length"),
                "canonical_total_length_mismatch",
                std::to_string(byte_count),
                std::to_string(canonical_total.value())));
        }
        auto final_padding = require_zero(
            bytes, byte_count, cursor, canonical_total.value(), work,
            header_path.append("total_length"), "nonzero_final_padding",
            true, true);
        if (!final_padding.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(final_padding).error());
        }

        for (const ListRegionState& list : list_partitions.states) {
            if (list.validity_region_index == UINT32_MAX) {
                continue;
            }
            const JsonPointer path = binary_region_path(list.validity_region_index);
            charged = work.charge(list.validity.byte_length, path);
            if (!charged.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(charged).error());
            }
            if (list.validity.element_count % UINT64_C(8) != UINT64_C(0) &&
                list.validity.element_count != UINT64_C(0)) {
                auto tail_offset = layout::checked_add_u64(
                    list.validity.data_offset,
                    list.validity.byte_length - UINT64_C(1), path);
                if (!tail_offset.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(tail_offset).error());
                }
                const std::uint8_t tail = bytes[static_cast<std::ptrdiff_t>(
                    tail_offset.value())];
                const std::uint8_t used = static_cast<std::uint8_t>(
                    list.validity.element_count % UINT64_C(8));
                const std::uint8_t mask = static_cast<std::uint8_t>(
                    UINT8_C(0xff) << used);
                if ((tail & mask) != UINT8_C(0)) {
                    return Result<PayloadIndex>::failure(noncanonical_exact(
                        path.append("data"), "nonzero_validity_tail",
                        std::to_string(static_cast<std::uint32_t>(tail & mask)),
                        "0"));
                }
            }
        }

        for (const EntryDescriptor& entry : entries) {
            const RegionDescriptor& values_region =
                regions[entry.values_region_index];
            const RegionDescriptor* validity_region =
                entry.validity_region_index == UINT32_MAX
                    ? nullptr
                    : &regions[entry.validity_region_index];
            const layout::RuntimeType* type =
                runtime.value().find_type(entry.runtime_type_id);
            if (type == nullptr) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL,
                    JsonPointer{}.append("entries").append(entry.entry_index),
                    "Portable payload entry type metadata is missing",
                    "entry_runtime_type_missing"));
            }
            output.entries_.push_back(EntrySlotMetadata{
                entry.runtime_type_id, type->source->kind,
                values_region.data_offset, entry.value_count,
                values_region.stride, validity_region != nullptr,
                validity_region == nullptr ? UINT64_C(0)
                                           : validity_region->data_offset,
                validity_region == nullptr ? UINT64_C(0)
                                           : validity_region->byte_length});
        }

        for (const EntryDescriptor& entry : entries) {
            const spec::Entry& source =
                runtime_entries[entry.entry_index];
            const RegionDescriptor& value_region =
                regions[entry.values_region_index];
            const RegionDescriptor* validity_region =
                entry.validity_region_index == UINT32_MAX
                    ? nullptr
                    : &regions[entry.validity_region_index];
            if (validity_region != nullptr) {
                charged = work.charge(
                    validity_region->byte_length,
                    binary_region_path(entry.validity_region_index));
                if (!charged.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(charged).error());
                }
                if (entry.value_count % UINT64_C(8) != UINT64_C(0) &&
                    entry.value_count != UINT64_C(0)) {
                    auto tail_offset = layout::checked_add_u64(
                        validity_region->data_offset,
                        validity_region->byte_length - UINT64_C(1),
                        binary_region_path(entry.validity_region_index));
                    if (!tail_offset.has_value()) {
                        return Result<PayloadIndex>::failure(
                            std::move(tail_offset).error());
                    }
                    const std::uint8_t tail = bytes[static_cast<std::ptrdiff_t>(
                        tail_offset.value())];
                    const std::uint8_t used = static_cast<std::uint8_t>(
                        entry.value_count % UINT64_C(8));
                    const std::uint8_t mask = static_cast<std::uint8_t>(
                        UINT8_C(0xff) << used);
                    if ((tail & mask) != UINT8_C(0)) {
                        return Result<PayloadIndex>::failure(
                            noncanonical_exact(
                                binary_region_path(
                                    entry.validity_region_index)
                                    .append("data"),
                                "nonzero_validity_tail",
                                std::to_string(static_cast<std::uint32_t>(
                                    tail & mask)),
                                "0"));
                    }
                }
            }
            for (std::uint64_t index = UINT64_C(0);
                 index < entry.value_count; ++index) {
                const JsonPointer entry_path =
                    JsonPointer{}.append("entries").append(source.id);
                const JsonPointer path =
                    source.cardinality == Cardinality::one
                        ? entry_path
                        : entry_path.append(index);
                bool present = true;
                if (validity_region != nullptr) {
                    auto validity_offset = layout::checked_add_u64(
                        validity_region->data_offset,
                        index / UINT64_C(8), path);
                    if (!validity_offset.has_value()) {
                        return Result<PayloadIndex>::failure(
                            std::move(validity_offset).error());
                    }
                    const std::uint8_t validity = bytes[
                        static_cast<std::ptrdiff_t>(
                            validity_offset.value())];
                    present =
                        ((validity >> (index % UINT64_C(8))) & UINT8_C(1)) !=
                        UINT8_C(0);
                }
                auto relative_offset = layout::checked_multiply_u64(
                    index, value_region.stride, path);
                if (!relative_offset.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(relative_offset).error());
                }
                auto absolute_offset = layout::checked_add_u64(
                    value_region.data_offset, relative_offset.value(), path);
                if (!absolute_offset.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(absolute_offset).error());
                }
                const std::uint64_t offset = absolute_offset.value();
                auto valid = validate_fixed_slot(
                    runtime.value(), bytes, byte_count, source.type, offset,
                    present, work, limits.max_nesting_depth, output.pools_,
                    pool_cursors, list_partitions, output.variable_slots_,
                    limits.validate_text_eager, path);
                if (!valid.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(valid).error());
                }
            }
        }

        for (const ListRegionState& list : list_partitions.states) {
            auto consumed = layout::require_partition_consumed(
                list.cursor, list.items.element_count,
                binary_region_path(list.items_region_index)
                    .append("element_count"));
            if (!consumed.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(consumed).error());
            }
        }

        for (const PoolMetadata& pool : output.pools_) {
            const TypeKind kind =
                pool.kind == RegionKind::utf8_pool    ? TypeKind::str
                : pool.kind == RegionKind::utf16_pool ? TypeKind::wstr
                                                      : TypeKind::bytes;
            auto consumed = layout::require_partition_consumed(
                pool_cursors.for_kind(kind), pool.byte_length,
                binary_region_path(pool.region_index).append("byte_length"));
            if (!consumed.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(consumed).error());
            }
        }

        output.runtime_schema_ = std::make_shared<const RuntimeSchema>(
            std::move(runtime).value());
        output.total_length_ = byte_count;
        output.validation_work_ = work.value();
        output.retained_max_string_bytes_ = limits.max_string_bytes;
        output.retained_max_validation_work_ = limits.max_validation_work;
        output.text_validated_eagerly_ = limits.validate_text_eager;
        return Result<PayloadIndex>::success(std::move(output));
    } catch (const std::bad_alloc&) {
        return Result<PayloadIndex>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<PayloadIndex>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::view
