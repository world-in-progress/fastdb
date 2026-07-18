#pragma once

#include "payload/error/Result.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"

#include <cstdint>
#include <optional>
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

class PayloadIndex final {
public:
    std::uint64_t total_length() const noexcept { return total_length_; }
    std::uint32_t entry_count() const noexcept {
        return static_cast<std::uint32_t>(entries_.size());
    }
    std::uint64_t validation_work() const noexcept {
        return validation_work_;
    }
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

    std::vector<EntrySlotMetadata> entries_;
    std::vector<PoolMetadata> pools_;
    std::vector<VariableSlotMetadata> variable_slots_;
    std::optional<layout::RuntimeSchema> runtime_schema_;
    std::uint64_t total_length_{UINT64_C(0)};
    std::uint64_t validation_work_{UINT64_C(0)};
    std::uint64_t retained_max_string_bytes_{UINT64_C(0)};
    std::uint64_t retained_max_validation_work_{UINT64_C(0)};
    bool text_validated_eagerly_{false};
};

error::Result<PayloadIndex> open_record(
    const spec::CompiledSpec& spec,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    OpenOptions limits = default_open_options());

}  // namespace fastdb::payload::view
