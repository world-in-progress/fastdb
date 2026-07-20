#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace fastdb::payload::view {

struct OpenOptions final {
    bool validate_text_eager;
    std::uint64_t max_total_bytes;
    std::uint64_t max_regions;
    std::uint64_t max_entries;
    std::uint64_t max_components;
    std::uint64_t max_nesting_depth;
    std::uint64_t max_list_elements;
    std::uint64_t max_graph_objects;
    std::uint64_t max_string_bytes;
    std::uint64_t max_validation_work;
};

OpenOptions default_open_options() noexcept;

struct ObservedScalar final {
    bool present;
    spec::TypeKind kind;
    std::uint64_t bits;
};

struct EntrySlotMetadata final {
    std::uint32_t runtime_type_id;
    spec::TypeKind kind;
    std::uint64_t data_offset;
    std::uint64_t value_count;
    std::uint32_t stride;
    bool has_validity;
    std::uint64_t validity_offset;
    std::uint64_t validity_byte_length;
};

struct FieldSlotMetadata final {
    std::uint32_t runtime_type_id;
    spec::TypeKind kind;
    std::uint32_t relative_offset;
    std::uint32_t stride;
    std::uint32_t alignment;
};

struct PoolMetadata final {
    layout::RegionKind kind;
    std::uint32_t region_index;
    std::uint64_t data_offset;
    std::uint64_t byte_length;
    std::uint64_t element_count;
};

struct VariableSlotMetadata final {
    spec::TypeKind kind;
    std::uint64_t descriptor_offset;
    std::uint64_t pool_relative_offset;
    std::uint64_t byte_length;
    bool present;
};

struct ListSlotMetadata final {
    std::uint32_t item_runtime_type_id;
    std::uint64_t item_data_offset;
    std::uint64_t item_count;
    std::uint32_t item_stride;
    bool has_validity;
    std::uint64_t validity_offset;
    std::uint64_t validity_byte_length;
};

struct ObjectPoolMetadata final {
    std::uint32_t component_index;
    std::uint32_t region_index;
    std::uint64_t data_offset;
    std::uint64_t object_count;
    std::uint32_t stride;
    std::uint32_t alignment;
};

struct EntrySequenceCursor final {
    std::uint32_t entry_index;
};

struct InlineValueCursor final {
    std::uint32_t runtime_type_id;
    spec::TypeKind kind;
    std::uint64_t slot_offset;
    bool present;
};

struct IdentityObjectCursor final {
    std::uint32_t component_index;
    std::uint64_t object_id;
    bool present;
};

struct RefCursor final {
    std::uint32_t runtime_type_id;
    std::uint32_t target_component_index;
    std::uint64_t object_id;
    bool present;
};

using ValueCursor = std::variant<InlineValueCursor,
                                 IdentityObjectCursor,
                                 RefCursor>;

inline const InlineValueCursor* inline_value_cursor(
    const ValueCursor& cursor) noexcept {
    return std::get_if<InlineValueCursor>(&cursor);
}

inline spec::TypeKind value_cursor_kind(const ValueCursor& cursor) noexcept {
    if (const auto* inline_cursor = inline_value_cursor(cursor)) {
        return inline_cursor->kind;
    }
    return std::holds_alternative<IdentityObjectCursor>(cursor)
               ? spec::TypeKind::component
               : spec::TypeKind::ref;
}

inline bool value_cursor_present(const ValueCursor& cursor) noexcept {
    return std::visit([](const auto& value) { return value.present; }, cursor);
}

struct GraphIdentity final {
    std::uint32_t component_index;
    std::uint64_t object_id;
};

struct VariableSpanMetadata final {
    spec::TypeKind kind;
    std::uint64_t data_offset;
    std::uint64_t byte_length;
};

class PayloadIndex final {
public:
    std::uint64_t total_length() const noexcept { return total_length_; }
    std::uint32_t entry_count() const noexcept {
        return static_cast<std::uint32_t>(entries_.size());
    }
    std::uint64_t validation_work() const noexcept {
        return validation_work_;
    }
    std::uint64_t root_value_count() const noexcept {
        return root_value_count_;
    }
    std::uint64_t graph_object_count() const noexcept {
        return graph_object_count_;
    }
    std::uint32_t region_count() const noexcept { return region_count_; }
    const std::vector<VariableSlotMetadata>& variable_slots() const noexcept {
        return variable_slots_;
    }
    std::optional<PoolMetadata> pool_metadata(
        layout::RegionKind kind) const noexcept;
    bool text_validated_eagerly() const noexcept {
        return text_validated_eagerly_;
    }
    std::uint64_t retained_max_string_bytes() const noexcept {
        return retained_max_string_bytes_;
    }
    std::uint64_t retained_max_validation_work() const noexcept {
        return retained_max_validation_work_;
    }
    const std::shared_ptr<const layout::RuntimeSchema>& runtime_schema()
        const noexcept {
        return runtime_schema_;
    }
    std::optional<ListSlotMetadata> list_slot(
        std::uint32_t owner_runtime_type_id) const noexcept;
    std::optional<ObjectPoolMetadata> object_pool_metadata(
        std::uint32_t component_index) const noexcept;
    error::Result<IdentityObjectCursor> ref_target(RefCursor cursor) const;
    error::Result<GraphIdentity> graph_identity(ValueCursor cursor) const;

    error::Result<EntrySequenceCursor> entry_sequence(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        std::uint32_t entry_index) const;
    error::Result<std::uint64_t> sequence_length(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        EntrySequenceCursor sequence) const;
    error::Result<ValueCursor> entry_value(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        EntrySequenceCursor sequence,
        std::uint64_t value_index) const;
    error::Result<std::uint32_t> component_index(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        ValueCursor cursor) const;
    error::Result<std::uint32_t> component_field_count(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        ValueCursor cursor) const;
    error::Result<ValueCursor> component_field(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        ValueCursor cursor,
        std::uint32_t field_index) const;
    error::Result<std::uint64_t> list_length(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        ValueCursor cursor) const;
    error::Result<ValueCursor> list_item(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        ValueCursor cursor,
        std::uint64_t item_index) const;
    error::Result<ObservedScalar> scalar_observation(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        ValueCursor cursor) const;
    error::Result<VariableSpanMetadata> variable_span(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        ValueCursor cursor) const;
    error::Result<VariableSpanMetadata> text_span(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        ValueCursor cursor,
        const json::JsonPointer& diagnostic_path) const;

    error::Result<EntrySlotMetadata> entry_slot(
        std::uint32_t entry_index) const;
    error::Result<FieldSlotMetadata> field_slot(
        std::uint32_t entry_index,
        const std::uint32_t* field_indexes,
        std::uint64_t field_depth) const;
    /*
     * Private Task 3 seam: observation callers must pass the same still-live,
     * immutable byte image accepted by open_record. Core checks the supplied
     * span shape, but cannot pin or prove backing identity until PayloadOwner.
     */
    error::Result<ObservedScalar> scalar(const std::uint8_t* bytes,
                                         std::uint64_t byte_count,
                                         std::uint32_t entry_index,
                                         std::uint64_t value_index) const;
    error::Result<ObservedScalar> field_scalar(
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        std::uint32_t entry_index,
        std::uint64_t value_index,
        const std::uint32_t* field_indexes,
        std::uint64_t field_depth) const;

private:
    friend error::Result<PayloadIndex> open_record(
        const spec::CompiledSpec&,
        const std::uint8_t*,
        std::uint64_t,
        OpenOptions);
    friend error::Result<PayloadIndex> open_graph(
        const spec::CompiledSpec&,
        const std::uint8_t*,
        std::uint64_t,
        OpenOptions);

    std::vector<EntrySlotMetadata> entries_;
    std::vector<PoolMetadata> pools_;
    std::vector<VariableSlotMetadata> variable_slots_;
    std::vector<std::optional<ListSlotMetadata>> list_slots_;
    std::vector<std::optional<ObjectPoolMetadata>> object_pools_;
    std::shared_ptr<const layout::RuntimeSchema> runtime_schema_;
    std::uint64_t total_length_{UINT64_C(0)};
    std::uint64_t validation_work_{UINT64_C(0)};
    std::uint64_t root_value_count_{UINT64_C(0)};
    std::uint64_t graph_object_count_{UINT64_C(0)};
    std::uint64_t retained_max_string_bytes_{UINT64_C(0)};
    std::uint64_t retained_max_validation_work_{UINT64_C(0)};
    std::uint32_t region_count_{UINT32_C(0)};
    bool text_validated_eagerly_{false};
};

error::Result<PayloadIndex> open_record(
    const spec::CompiledSpec& spec,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    OpenOptions limits = default_open_options());

}  // namespace fastdb::payload::view
