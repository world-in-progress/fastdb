#include "payload/view/Open.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/InputSpan.hpp"
#include "payload/layout/NormalizedInteger.hpp"
#include "payload/layout/RuntimeSchema.hpp"

#include "payload/layout/TextEncoding.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
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

Error simple_error(std::uint32_t code,
                   const JsonPointer& path,
                   const char* message,
                   const char* reason) {
    return Error::from_details(
        code, path, message,
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

Error noncanonical(const JsonPointer& path, const char* reason) {
    return simple_error(FDB_PAYLOAD_E_NON_CANONICAL_BINARY, path,
                        "Portable payload binary is not canonical", reason);
}

Error invalid_value(const JsonPointer& path, const char* reason) {
    return simple_error(FDB_PAYLOAD_E_INVALID_BINARY_VALUE, path,
                        "Portable payload binary value is invalid", reason);
}

Error resource_error(const JsonPointer& path,
                     const char* resource,
                     std::uint64_t actual,
                     std::uint64_t limit) {
    return Error::from_details(
        FDB_PAYLOAD_E_RESOURCE_LIMIT, path,
        "Portable payload open resource limit exceeded",
        JsonValue::object({
            JsonValue::Member{"actual", JsonValue{std::to_string(actual)}},
            JsonValue::Member{"limit", JsonValue{std::to_string(limit)}},
            JsonValue::Member{"resource", JsonValue{resource}},
        }));
}

Error allocation_error() {
    return simple_error(FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
                        "Portable payload open allocation failed",
                        "allocation_failed");
}

class WorkCounter final {
public:
    explicit WorkCounter(std::uint64_t limit) : limit_(limit) {}

    Result<void> charge(std::uint64_t units, const JsonPointer& path) {
        std::uint64_t next = value_;
        auto accumulated = layout::checked_accumulate_u64(next, units, path);
        if (!accumulated.has_value()) {
            return accumulated;
        }
        if (next > limit_) {
            return Result<void>::failure(resource_error(
                path, "validation_work", next, limit_));
        }
        value_ = next;
        return Result<void>::success();
    }

    std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t limit_;
    std::uint64_t value_{UINT64_C(0)};
};

Result<void> require_zero(const std::uint8_t* bytes,
                          std::uint64_t byte_count,
                          std::uint64_t begin,
                          std::uint64_t end,
                          WorkCounter& work,
                          const JsonPointer& path,
                          const char* reason,
                          bool charge_units = true) {
    if (end < begin) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_LENGTH_OVERFLOW, path,
            "Portable payload zero range is reversed",
            "reversed_zero_range"));
    }
    auto span = layout::checked_range_end(begin, end - begin, byte_count, path);
    if (!span.has_value()) {
        return Result<void>::failure(std::move(span).error());
    }
    if (charge_units) {
        auto charged = work.charge(end - begin, path);
        if (!charged.has_value()) {
            return charged;
        }
    }
    for (std::uint64_t offset = begin; offset < end; ++offset) {
        if (bytes[static_cast<std::ptrdiff_t>(offset)] != UINT8_C(0)) {
            return Result<void>::failure(noncanonical(path, reason));
        }
    }
    return Result<void>::success();
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

Result<std::uint32_t> read_u32(const std::uint8_t* bytes,
                               std::uint64_t byte_count,
                               std::uint64_t offset,
                               const JsonPointer& path) {
    return layout::load_u32_le(bytes, byte_count, offset, path);
}

Result<std::uint64_t> read_u64(const std::uint8_t* bytes,
                               std::uint64_t byte_count,
                               std::uint64_t offset,
                               const JsonPointer& path) {
    return layout::load_u64_le(bytes, byte_count, offset, path);
}

struct RegionFields final {
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint32_t owner;
    std::uint32_t runtime_type_id;
    std::uint64_t data_offset;
    std::uint64_t byte_length;
    std::uint64_t element_count;
    std::uint32_t stride;
    std::uint32_t alignment;
    std::uint64_t reserved;
};

Result<RegionFields> read_region_fields(const std::uint8_t* bytes,
                                        std::uint64_t byte_count,
                                        std::uint32_t region_index,
                                        const JsonPointer& path) {
    const std::uint64_t base =
        layout::header_size + static_cast<std::uint64_t>(region_index) *
                                  layout::region_descriptor_size;
    auto kind = read_u32(bytes, byte_count, base + layout::region_kind_offset,
                         path);
    auto flags = read_u32(bytes, byte_count,
                          base + layout::region_flags_offset, path);
    auto owner = read_u32(bytes, byte_count,
                          base + layout::region_owner_index_offset, path);
    auto type_id = read_u32(bytes, byte_count,
                            base + layout::region_runtime_type_id_offset, path);
    auto data_offset = read_u64(bytes, byte_count,
                                base + layout::region_data_offset_offset, path);
    auto byte_length = read_u64(bytes, byte_count,
                                base + layout::region_byte_length_offset, path);
    auto element_count = read_u64(
        bytes, byte_count, base + layout::region_element_count_offset, path);
    auto stride = read_u32(bytes, byte_count,
                           base + layout::region_stride_offset, path);
    auto alignment = read_u32(bytes, byte_count,
                              base + layout::region_alignment_offset, path);
    auto reserved = read_u64(bytes, byte_count,
                             base + layout::region_reserved_offset, path);
    if (!kind.has_value() || !flags.has_value() || !owner.has_value() ||
        !type_id.has_value() || !data_offset.has_value() ||
        !byte_length.has_value() || !element_count.has_value() ||
        !stride.has_value() || !alignment.has_value() ||
        !reserved.has_value()) {
        return Result<RegionFields>::failure(simple_error(
            FDB_PAYLOAD_E_OUT_OF_BOUNDS, path,
            "Portable payload region descriptor is outside the image",
            "region_descriptor_out_of_bounds"));
    }
    return Result<RegionFields>::success(RegionFields{
        kind.value(), flags.value(), owner.value(), type_id.value(),
        data_offset.value(), byte_length.value(), element_count.value(),
        stride.value(), alignment.value(), reserved.value()});
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

OpenLimits default_open_limits() noexcept {
    return OpenLimits{UINT64_C(1) << 30,   UINT64_C(1000000),
                      UINT64_C(65536),     UINT64_C(65536),
                      UINT64_C(1024),      UINT64_C(10000000),
                      UINT64_C(10000000),  UINT64_C(1) << 30,
                      UINT64_C(100000000), true};
}

std::optional<PoolMetadata>
PayloadIndex::pool_metadata(layout::RegionKind kind) const noexcept {
    const auto found = std::find_if(
        pools_.begin(), pools_.end(),
        [kind](const PoolMetadata& pool) { return pool.kind == kind; });
    return found == pools_.end() ? std::nullopt
                                 : std::optional<PoolMetadata>{*found};
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
    if (runtime_schema_ == std::nullopt || entry_index >= entries_.size()) {
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
    auto span = require_access_span(bytes, byte_count, total_length_);
    if (!span.has_value()) {
        return Result<ObservedScalar>::failure(std::move(span).error());
    }
    auto metadata = entry_slot(entry_index);
    if (!metadata.has_value()) {
        return Result<ObservedScalar>::failure(std::move(metadata).error());
    }
    if (value_index >= metadata.value().value_count) {
        return Result<ObservedScalar>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
            JsonPointer{}.append("entries").append(entry_index).append(
                value_index),
            "Portable payload value index is out of range", "value_index"));
    }
    const layout::RuntimeType* type =
        runtime_schema_->find_type(metadata.value().runtime_type_id);
    if (type == nullptr) {
        return Result<ObservedScalar>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("entries"),
            "Portable payload entry type metadata is missing",
            "entry_runtime_type_missing"));
    }
    bool present = true;
    if (metadata.value().has_validity) {
        auto validity_offset = layout::checked_add_u64(
            metadata.value().validity_offset, value_index / UINT64_C(8),
            JsonPointer{}.append("entries").append(entry_index));
        if (!validity_offset.has_value()) {
            return Result<ObservedScalar>::failure(
                std::move(validity_offset).error());
        }
        present = ((bytes[static_cast<std::ptrdiff_t>(
                        validity_offset.value())] >>
                    (value_index % UINT64_C(8))) &
                   UINT8_C(1)) != UINT8_C(0);
    }
    auto relative = layout::checked_multiply_u64(
        value_index, metadata.value().stride,
        JsonPointer{}.append("entries").append(entry_index));
    if (!relative.has_value()) {
        return Result<ObservedScalar>::failure(std::move(relative).error());
    }
    auto offset = layout::checked_add_u64(
        metadata.value().data_offset, relative.value(),
        JsonPointer{}.append("entries").append(entry_index));
    if (!offset.has_value()) {
        return Result<ObservedScalar>::failure(std::move(offset).error());
    }
    return observe_scalar(bytes, byte_count, *type->source, offset.value(),
                          present,
                          JsonPointer{}.append("entries").append(entry_index)
                              .append(value_index));
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
    auto span = require_access_span(bytes, byte_count, total_length_);
    if (!span.has_value()) {
        return Result<ObservedScalar>::failure(std::move(span).error());
    }
    const EntrySlotMetadata& entry = entries_[entry_index];
    if (value_index >= entry.value_count) {
        return Result<ObservedScalar>::failure(simple_error(
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE,
            JsonPointer{}.append("entries").append(entry_index).append(
                value_index),
            "Portable payload value index is out of range", "value_index"));
    }
    bool present = true;
    if (entry.has_validity) {
        auto validity_offset = layout::checked_add_u64(
            entry.validity_offset, value_index / UINT64_C(8),
            JsonPointer{}.append("entries").append(entry_index));
        if (!validity_offset.has_value()) {
            return Result<ObservedScalar>::failure(
                std::move(validity_offset).error());
        }
        present = ((bytes[static_cast<std::ptrdiff_t>(
                        validity_offset.value())] >>
                    (value_index % UINT64_C(8))) &
                   UINT8_C(1)) != UINT8_C(0);
    }
    const layout::RuntimeType* current =
        runtime_schema_->find_type(entry.runtime_type_id);
    auto row_relative = layout::checked_multiply_u64(
        value_index, entry.stride,
        JsonPointer{}.append("entries").append(entry_index));
    if (!row_relative.has_value()) {
        return Result<ObservedScalar>::failure(
            std::move(row_relative).error());
    }
    auto row_offset = layout::checked_add_u64(
        entry.data_offset, row_relative.value(),
        JsonPointer{}.append("entries").append(entry_index));
    if (!row_offset.has_value()) {
        return Result<ObservedScalar>::failure(std::move(row_offset).error());
    }
    std::uint64_t base = row_offset.value();
    for (std::uint64_t depth = UINT64_C(0); depth < field_depth; ++depth) {
        if (!present) {
            return Result<ObservedScalar>::success(ObservedScalar{
                false, target.value().kind, UINT64_C(0)});
        }
        if (current == nullptr || current->source->kind != TypeKind::component) {
            return Result<ObservedScalar>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL,
                JsonPointer{}.append("fields").append(depth),
                "Portable payload field path metadata is inconsistent",
                "field_path_runtime_mismatch"));
        }
        const layout::ComponentLayout* component = runtime_schema_->component(
            current->source->resolved_component_index);
        if (component == nullptr) {
            return Result<ObservedScalar>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL,
                JsonPointer{}.append("fields").append(depth),
                "Portable payload component metadata is missing",
                "component_layout_missing"));
        }
        const std::uint32_t field_index =
            field_indexes[static_cast<std::size_t>(depth)];
        const layout::ComponentFieldLayout& field =
            component->fields[field_index];
        const auto& source_component =
            runtime_schema_->spec().resolved().components()[
                current->source->resolved_component_index];
        const spec::TypeNode& field_type =
            source_component.fields[field_index].type;
        if (field.validity_bit != UINT32_MAX) {
            auto validity_offset = layout::checked_add_u64(
                base, field.validity_bit / UINT32_C(8),
                JsonPointer{}.append("fields").append(depth));
            if (!validity_offset.has_value()) {
                return Result<ObservedScalar>::failure(
                    std::move(validity_offset).error());
            }
            present = ((bytes[static_cast<std::ptrdiff_t>(
                            validity_offset.value())] >>
                        (field.validity_bit % UINT32_C(8))) &
                       UINT8_C(1)) != UINT8_C(0);
        }
        auto field_offset = layout::checked_add_u64(
            base, field.offset,
            JsonPointer{}.append("fields").append(depth));
        if (!field_offset.has_value()) {
            return Result<ObservedScalar>::failure(
                std::move(field_offset).error());
        }
        base = field_offset.value();
        current = runtime_schema_->find_type(field.runtime_type_id);
        if (depth + UINT64_C(1) == field_depth) {
            return observe_scalar(
                bytes, byte_count, field_type, base, present,
                JsonPointer{}.append("entries").append(entry_index).append(
                    value_index));
        }
    }
    return Result<ObservedScalar>::failure(simple_error(
        FDB_PAYLOAD_E_INVALID_ARGUMENT, JsonPointer{}.append("fields"),
        "Portable payload field path is empty", "invalid_field_path"));
}

Result<PayloadIndex> open_record(const spec::CompiledSpec& compiled,
                                 const std::uint8_t* bytes,
                                 std::uint64_t byte_count,
                                 OpenLimits limits) {
    try {
        const JsonPointer header_path = JsonPointer{}.append("header");
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
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_INVALID_MAGIC,
                header_path.append("magic"),
                "Portable payload magic is invalid", "invalid_magic"));
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
        if (major.value() != layout::binary_major ||
            minor.value() != layout::binary_minor) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION,
                header_path.append("version"),
                "Portable payload binary version is unsupported",
                "unsupported_binary_version"));
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
        if (!header_size_value.has_value() || !profile.has_value() ||
            !flags.has_value() || !total.has_value()) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_OUT_OF_BOUNDS, header_path,
                "Portable payload header field is outside the image",
                "header_field_out_of_bounds"));
        }
        if (header_size_value.value() != layout::header_size ||
            profile.value() != FDB_PAYLOAD_PROFILE_RECORD_V1 ||
            flags.value() != UINT32_C(0)) {
            return Result<PayloadIndex>::failure(
                noncanonical(header_path, "header_contract"));
        }
        if (total.value() > byte_count) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_OUT_OF_BOUNDS,
                header_path.append("total_length"),
                "Declared portable payload length exceeds supplied bytes",
                "declared_total_out_of_bounds"));
        }
        if (total.value() != byte_count) {
            return Result<PayloadIndex>::failure(noncanonical(
                header_path.append("total_length"), "trailing_binary_bytes"));
        }
        if (!std::equal(compiled.digest().begin(), compiled.digest().end(),
                        bytes + static_cast<std::ptrdiff_t>(
                                    layout::header_spec_digest_offset))) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_DIGEST_MISMATCH,
                header_path.append("spec_sha256"),
                "Portable payload spec digest does not match",
                "spec_digest_mismatch"));
        }
        auto header_reserved = require_zero(
            bytes, byte_count, layout::header_reserved_offset,
            layout::header_size, work, header_path.append("reserved"),
            "nonzero_header_reserved", false);
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
                JsonPointer{}.append("entries"), "entries", expected_entries,
                limits.max_entries));
        }
        if (expected_regions > limits.max_regions) {
            return Result<PayloadIndex>::failure(resource_error(
                JsonPointer{}.append("regions"), "regions", expected_regions,
                limits.max_regions));
        }
        if (expected_components > limits.max_components) {
            return Result<PayloadIndex>::failure(resource_error(
                JsonPointer{}.append("components"), "components",
                expected_components, limits.max_components));
        }
        if (known_minimum_work > limits.max_validation_work) {
            return Result<PayloadIndex>::failure(resource_error(
                JsonPointer{}.append("validation_work"), "validation_work",
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
                resource_error(JsonPointer{}.append("regions"), "regions",
                               expected_regions, limits.max_regions));
        }
        if (known_minimum_work > limits.max_validation_work) {
            return Result<PayloadIndex>::failure(resource_error(
                JsonPointer{}.append("validation_work"), "validation_work",
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
        if (!region_directory_offset.has_value() || !region_count.has_value() ||
            !region_size.has_value() || !entry_directory_offset.has_value() ||
            !entry_count.has_value() || !entry_size.has_value() ||
            !root_value_count.has_value()) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_OUT_OF_BOUNDS, header_path,
                "Portable payload directory header is outside the image",
                "directory_header_out_of_bounds"));
        }
        if (region_directory_offset.value() != layout::header_size ||
            region_count.value() != expected_regions ||
            region_size.value() != layout::region_descriptor_size ||
            entry_count.value() != expected_entries ||
            entry_size.value() != layout::entry_descriptor_size) {
            return Result<PayloadIndex>::failure(
                noncanonical(header_path, "directory_header_contract"));
        }
        auto region_bytes = layout::checked_multiply_u64(
            region_count.value(), layout::region_descriptor_size,
            JsonPointer{}.append("regions"));
        if (!region_bytes.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(region_bytes).error());
        }
        auto canonical_entry_offset = layout::checked_add_u64(
            layout::header_size, region_bytes.value(),
            JsonPointer{}.append("entries"));
        if (!canonical_entry_offset.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(canonical_entry_offset).error());
        }
        if (entry_directory_offset.value() != canonical_entry_offset.value()) {
            return Result<PayloadIndex>::failure(noncanonical(
                header_path.append("entry_directory_offset"),
                "entry_directory_not_contiguous"));
        }
        auto entry_bytes = layout::checked_multiply_u64(
            entry_count.value(), layout::entry_descriptor_size,
            JsonPointer{}.append("entries"));
        if (!entry_bytes.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(entry_bytes).error());
        }
        auto entry_end = layout::checked_range_end(
            entry_directory_offset.value(), entry_bytes.value(), byte_count,
            JsonPointer{}.append("entries"));
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
            const JsonPointer path =
                JsonPointer{}.append("entries").append(source.id);
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
                bytes, byte_count, base + layout::entry_index_offset, path);
            auto type_id = read_u32(
                bytes, byte_count,
                base + layout::entry_runtime_type_id_offset, path);
            auto cardinality = read_u32(
                bytes, byte_count, base + layout::entry_cardinality_offset,
                path);
            auto entry_flags = read_u32(
                bytes, byte_count, base + layout::entry_flags_offset, path);
            auto value_count = read_u64(
                bytes, byte_count, base + layout::entry_value_count_offset,
                path);
            auto values_region = read_u32(
                bytes, byte_count,
                base + layout::entry_values_region_index_offset, path);
            auto validity_region = read_u32(
                bytes, byte_count,
                base + layout::entry_validity_region_index_offset, path);
            auto reserved = read_u64(
                bytes, byte_count, base + layout::entry_reserved_offset, path);
            if (!actual_index.has_value() || !type_id.has_value() ||
                !cardinality.has_value() || !entry_flags.has_value() ||
                !value_count.has_value() || !values_region.has_value() ||
                !validity_region.has_value() || !reserved.has_value()) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_OUT_OF_BOUNDS, path,
                    "Portable payload entry descriptor is outside the image",
                    "entry_descriptor_out_of_bounds"));
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
            if (actual_index.value() != index ||
                type_id.value() != expected_type ||
                cardinality.value() != expected_cardinality ||
                entry_flags.value() != expected_flags ||
                values_region.value() != expected_values ||
                validity_region.value() != expected_validity ||
                reserved.value() != UINT64_C(0) ||
                (source.cardinality == Cardinality::one &&
                 value_count.value() != UINT64_C(1))) {
                return Result<PayloadIndex>::failure(
                    noncanonical(path, "entry_descriptor_contract"));
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
            return Result<PayloadIndex>::failure(noncanonical(
                header_path.append("root_value_count"),
                "root_value_count_mismatch"));
        }

        auto data_start = layout::checked_align_up_u64(
            entry_end.value(), UINT32_C(8), JsonPointer{}.append("regions"));
        if (!data_start.has_value()) {
            return Result<PayloadIndex>::failure(std::move(data_start).error());
        }
        auto directory_padding = require_zero(
            bytes, byte_count, entry_end.value(), data_start.value(), work,
            JsonPointer{}.append("padding"), "nonzero_directory_padding");
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
                    JsonPointer{}.append("regions").append(region_index);
                charged = work.charge(UINT64_C(1), path);
                if (!charged.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(charged).error());
                }
                const std::uint64_t base =
                    layout::header_size +
                    static_cast<std::uint64_t>(region_index) *
                        layout::region_descriptor_size;
                auto kind = read_u32(bytes, byte_count,
                                     base + layout::region_kind_offset, path);
                auto region_flags = read_u32(
                    bytes, byte_count, base + layout::region_flags_offset,
                    path);
                auto owner = read_u32(
                    bytes, byte_count, base + layout::region_owner_index_offset,
                    path);
                auto type_id = read_u32(
                    bytes, byte_count,
                    base + layout::region_runtime_type_id_offset, path);
                auto data_offset = read_u64(
                    bytes, byte_count, base + layout::region_data_offset_offset,
                    path);
                auto byte_length = read_u64(
                    bytes, byte_count,
                    base + layout::region_byte_length_offset, path);
                auto element_count = read_u64(
                    bytes, byte_count,
                    base + layout::region_element_count_offset, path);
                auto stride = read_u32(
                    bytes, byte_count, base + layout::region_stride_offset,
                    path);
                auto alignment = read_u32(
                    bytes, byte_count, base + layout::region_alignment_offset,
                    path);
                auto reserved = read_u64(
                    bytes, byte_count, base + layout::region_reserved_offset,
                    path);
                if (!kind.has_value() || !region_flags.has_value() ||
                    !owner.has_value() || !type_id.has_value() ||
                    !data_offset.has_value() || !byte_length.has_value() ||
                    !element_count.has_value() || !stride.has_value() ||
                    !alignment.has_value() || !reserved.has_value()) {
                    return Result<PayloadIndex>::failure(simple_error(
                        FDB_PAYLOAD_E_OUT_OF_BOUNDS, path,
                        "Portable payload region descriptor is outside the image",
                        "region_descriptor_out_of_bounds"));
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
                                                 slot.stride, path);
                if (validity) {
                    auto rounded = layout::checked_add_u64(
                        entry.value_count, UINT64_C(7), path);
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
                    cursor, expected_alignment, path);
                if (!aligned.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(aligned).error());
                }
                if (data_offset.value() % expected_alignment != UINT64_C(0)) {
                    return Result<PayloadIndex>::failure(simple_error(
                        FDB_PAYLOAD_E_MISALIGNED, path,
                        "Portable payload region offset is misaligned",
                        "region_offset_misaligned"));
                }
                if (kind.value() !=
                        static_cast<std::uint32_t>(expected_kind) ||
                    region_flags.value() != UINT32_C(0) ||
                    owner.value() != entry.entry_index ||
                    type_id.value() != entry.runtime_type_id ||
                    data_offset.value() != aligned.value() ||
                    byte_length.value() != expected_length.value() ||
                    element_count.value() != entry.value_count ||
                    stride.value() != expected_stride ||
                    alignment.value() != expected_alignment ||
                    reserved.value() != UINT64_C(0)) {
                    return Result<PayloadIndex>::failure(
                        noncanonical(path, "region_descriptor_contract"));
                }
                auto padding = require_zero(
                    bytes, byte_count, cursor, aligned.value(), work,
                    JsonPointer{}.append("padding"),
                    "nonzero_inter_region_padding");
                if (!padding.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(padding).error());
                }
                auto end = layout::checked_range_end(
                    data_offset.value(), byte_length.value(), byte_count, path);
                if (!end.has_value()) {
                    return Result<PayloadIndex>::failure(std::move(end).error());
                }
                cursor = end.value();
                regions.push_back(RegionDescriptor{
                    expected_kind, UINT32_C(0), entry.entry_index,
                    entry.runtime_type_id, data_offset.value(),
                    byte_length.value(), entry.value_count, expected_stride,
                    expected_alignment});
                ++region_index;
            }
        }

        ListPartitions list_partitions;
        list_partitions.indexes.assign(runtime.value().type_count(),
                                       UINT32_MAX);
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
                    JsonPointer{}.append("regions").append(region_index);
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
                if (fields.value().kind !=
                        static_cast<std::uint32_t>(
                            RegionKind::list_validity) ||
                    fields.value().flags != UINT32_C(0) ||
                    fields.value().owner != list.owner_runtime_type_id ||
                    fields.value().runtime_type_id !=
                        list.item_runtime_type_id ||
                    fields.value().data_offset != aligned.value() ||
                    fields.value().byte_length !=
                        rounded.value() / UINT64_C(8) ||
                    fields.value().stride != UINT32_C(0) ||
                    fields.value().alignment != UINT32_C(1) ||
                    fields.value().reserved != UINT64_C(0)) {
                    return Result<PayloadIndex>::failure(noncanonical(
                        path, "region_descriptor_contract"));
                }
                auto padding = require_zero(
                    bytes, byte_count, cursor, aligned.value(), work,
                    JsonPointer{}.append("padding"),
                    "nonzero_inter_region_padding");
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
                JsonPointer{}.append("regions").append(region_index);
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
            if (fields.value().kind !=
                    static_cast<std::uint32_t>(RegionKind::list_items) ||
                fields.value().flags != UINT32_C(0) ||
                fields.value().owner != list.owner_runtime_type_id ||
                fields.value().runtime_type_id !=
                    list.item_runtime_type_id ||
                fields.value().data_offset != aligned.value() ||
                fields.value().byte_length != expected_length.value() ||
                fields.value().stride != list.item_stride ||
                fields.value().alignment != list.item_alignment ||
                fields.value().reserved != UINT64_C(0) ||
                (list.item_nullable &&
                 validity_descriptor.element_count !=
                     fields.value().element_count)) {
                return Result<PayloadIndex>::failure(
                    noncanonical(path, "region_descriptor_contract"));
            }
            auto padding = require_zero(
                bytes, byte_count, cursor, aligned.value(), work,
                JsonPointer{}.append("padding"),
                "nonzero_inter_region_padding");
            if (!padding.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(padding).error());
            }
            auto end = layout::checked_range_end(
                fields.value().data_offset, fields.value().byte_length,
                byte_count, path);
            if (!end.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(end).error());
            }
            auto accumulated = layout::checked_accumulate_u64(
                total_list_elements, fields.value().element_count,
                JsonPointer{}.append("lists").append(
                    list.owner_runtime_type_id));
            if (!accumulated.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(accumulated).error());
            }
            if (total_list_elements > limits.max_list_elements) {
                return Result<PayloadIndex>::failure(resource_error(
                    JsonPointer{}.append("lists").append(
                        list.owner_runtime_type_id),
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
                JsonPointer{}.append("regions").append(region_index);
            charged = work.charge(UINT64_C(1), path);
            if (!charged.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(charged).error());
            }
            const std::uint64_t base =
                layout::header_size + static_cast<std::uint64_t>(region_index) *
                                          layout::region_descriptor_size;
            auto kind = read_u32(bytes, byte_count,
                                 base + layout::region_kind_offset, path);
            auto region_flags = read_u32(
                bytes, byte_count, base + layout::region_flags_offset, path);
            auto owner =
                read_u32(bytes, byte_count,
                         base + layout::region_owner_index_offset, path);
            auto type_id =
                read_u32(bytes, byte_count,
                         base + layout::region_runtime_type_id_offset, path);
            auto data_offset =
                read_u64(bytes, byte_count,
                         base + layout::region_data_offset_offset, path);
            auto byte_length =
                read_u64(bytes, byte_count,
                         base + layout::region_byte_length_offset, path);
            auto element_count =
                read_u64(bytes, byte_count,
                         base + layout::region_element_count_offset, path);
            auto stride = read_u32(bytes, byte_count,
                                   base + layout::region_stride_offset, path);
            auto alignment =
                read_u32(bytes, byte_count,
                         base + layout::region_alignment_offset, path);
            auto reserved = read_u64(
                bytes, byte_count, base + layout::region_reserved_offset, path);
            if (!kind.has_value() || !region_flags.has_value() ||
                !owner.has_value() || !type_id.has_value() ||
                !data_offset.has_value() || !byte_length.has_value() ||
                !element_count.has_value() || !stride.has_value() ||
                !alignment.has_value() || !reserved.has_value()) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_OUT_OF_BOUNDS, path,
                    "Portable payload pool descriptor is outside the image",
                    "region_descriptor_out_of_bounds"));
            }
            const std::uint32_t expected_alignment =
                expected_kind == RegionKind::utf16_pool ? UINT32_C(2)
                                                        : UINT32_C(1);
            auto aligned =
                layout::checked_align_up_u64(cursor, expected_alignment, path);
            if (!aligned.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(aligned).error());
            }
            if (data_offset.value() % expected_alignment != UINT64_C(0)) {
                return Result<PayloadIndex>::failure(
                    simple_error(FDB_PAYLOAD_E_MISALIGNED, path,
                                 "Portable payload pool offset is misaligned",
                                 "region_offset_misaligned"));
            }
            if (kind.value() != static_cast<std::uint32_t>(expected_kind) ||
                region_flags.value() != UINT32_C(0) ||
                owner.value() != UINT32_MAX || type_id.value() != UINT32_MAX ||
                data_offset.value() != aligned.value() ||
                stride.value() != UINT32_C(0) ||
                alignment.value() != expected_alignment ||
                reserved.value() != UINT64_C(0)) {
                return Result<PayloadIndex>::failure(
                    noncanonical(path, "region_descriptor_contract"));
            }
            auto padding =
                require_zero(bytes, byte_count, cursor, aligned.value(), work,
                             JsonPointer{}.append("padding"),
                             "nonzero_inter_region_padding");
            if (!padding.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(padding).error());
            }
            auto end = layout::checked_range_end(
                data_offset.value(), byte_length.value(), byte_count, path);
            if (!end.has_value()) {
                return Result<PayloadIndex>::failure(std::move(end).error());
            }
            if (expected_kind == RegionKind::utf16_pool &&
                byte_length.value() % UINT64_C(2) != UINT64_C(0)) {
                return Result<PayloadIndex>::failure(
                    noncanonical(path, "odd_utf16_pool_length"));
            }
            const std::uint64_t expected_elements =
                expected_kind == RegionKind::utf16_pool
                    ? byte_length.value() / UINT64_C(2)
                    : byte_length.value();
            if (element_count.value() != expected_elements) {
                return Result<PayloadIndex>::failure(
                    noncanonical(path, "region_descriptor_contract"));
            }
            if (expected_kind != RegionKind::bytes_pool) {
                auto accumulated = layout::checked_accumulate_u64(
                    text_pool_bytes, byte_length.value(),
                    JsonPointer{}.append("pools").append("text"));
                if (!accumulated.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(accumulated).error());
                }
                if (text_pool_bytes > limits.max_string_bytes) {
                    return Result<PayloadIndex>::failure(resource_error(
                        JsonPointer{}.append("pools").append("text"),
                        "string_bytes", text_pool_bytes,
                        limits.max_string_bytes));
                }
            }
            regions.push_back(RegionDescriptor{
                expected_kind, UINT32_C(0), UINT32_MAX, UINT32_MAX,
                data_offset.value(), byte_length.value(), element_count.value(),
                UINT32_C(0), expected_alignment});
            output.pools_.push_back(
                PoolMetadata{expected_kind, data_offset.value(),
                             byte_length.value(), element_count.value()});
            cursor = end.value();
            ++region_index;
        }
        if (region_index != region_count.value()) {
            return Result<PayloadIndex>::failure(
                noncanonical(JsonPointer{}.append("regions"),
                             "region_inventory_not_consumed"));
        }

        auto canonical_total = layout::checked_align_up_u64(
            cursor, UINT32_C(8), JsonPointer{}.append("total_length"));
        if (!canonical_total.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(canonical_total).error());
        }
        if (canonical_total.value() != byte_count) {
            return Result<PayloadIndex>::failure(noncanonical(
                JsonPointer{}.append("total_length"),
                "canonical_total_length_mismatch"));
        }
        auto final_padding = require_zero(
            bytes, byte_count, cursor, canonical_total.value(), work,
            JsonPointer{}.append("padding"), "nonzero_final_padding");
        if (!final_padding.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(final_padding).error());
        }

        for (const ListRegionState& list : list_partitions.states) {
            if (list.validity_region_index == UINT32_MAX) {
                continue;
            }
            const JsonPointer path = JsonPointer{}
                                         .append("regions")
                                         .append(list.validity_region_index);
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
                    return Result<PayloadIndex>::failure(noncanonical(
                        path, "nonzero_validity_tail"));
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
                    JsonPointer{}.append("regions").append(
                        entry.validity_region_index));
                if (!charged.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(charged).error());
                }
                if (entry.value_count % UINT64_C(8) != UINT64_C(0) &&
                    entry.value_count != UINT64_C(0)) {
                    auto tail_offset = layout::checked_add_u64(
                        validity_region->data_offset,
                        validity_region->byte_length - UINT64_C(1),
                        JsonPointer{}.append("regions").append(
                            entry.validity_region_index));
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
                        return Result<PayloadIndex>::failure(noncanonical(
                            JsonPointer{}.append("regions").append(
                                entry.validity_region_index),
                            "nonzero_validity_tail"));
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
                JsonPointer{}.append("lists").append(
                    list.owner_runtime_type_id));
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
            const char* name = pool.kind == RegionKind::utf8_pool    ? "utf8"
                               : pool.kind == RegionKind::utf16_pool ? "utf16le"
                                                                     : "bytes";
            auto consumed = layout::require_partition_consumed(
                pool_cursors.for_kind(kind), pool.byte_length,
                JsonPointer{}.append("pools").append(name));
            if (!consumed.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(consumed).error());
            }
        }

        output.runtime_schema_.emplace(std::move(runtime).value());
        output.total_length_ = byte_count;
        output.validation_work_ = work.value();
        output.retained_max_string_bytes_ = limits.max_string_bytes;
        output.retained_max_validation_work_ = limits.max_validation_work;
        output.text_validated_eagerly_ = limits.validate_text_eager;
        return Result<PayloadIndex>::success(std::move(output));
    } catch (const std::bad_alloc&) {
        return Result<PayloadIndex>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::view
