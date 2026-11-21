#pragma once

#include "payload/error/Result.hpp"
#include "payload/spec/RuntimeTopology.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fastdb::payload::spec {
class CompiledSpec;
}

namespace fastdb::payload::layout {

class RecordLayout;
struct RuntimeSchemaTestAccess;

struct SlotLayout final {
    std::uint32_t stride;
    std::uint32_t alignment;
};

struct RuntimeType final {
    std::uint32_t runtime_type_id;
    const spec::TypeNode* source;
    SlotLayout slot;
    spec::StorageRole storage_role;
    bool reachable;
};

struct ComponentFieldLayout final {
    std::uint32_t runtime_type_id;
    std::uint32_t offset;
    std::uint32_t slot_stride;
    std::uint32_t alignment;
    std::uint32_t validity_bit;
};

struct ComponentLayout final {
    std::uint32_t component_index;
    std::uint32_t stride;
    std::uint32_t alignment;
    std::uint32_t validity_bytes;
    std::vector<ComponentFieldLayout> fields;
};

struct ListNodeLayout final {
    std::uint32_t owner_runtime_type_id;
    std::uint32_t item_runtime_type_id;
    std::uint32_t item_stride;
    std::uint32_t item_alignment;
    bool item_nullable;
};

class RuntimeSchema final {
public:
    static error::Result<void> require_record_runtime(
        const spec::CompiledSpec& spec);
    static error::Result<std::shared_ptr<const RuntimeSchema>> compile(
        const spec::CompiledSpec& spec);

    std::uint32_t type_count() const noexcept {
        return static_cast<std::uint32_t>(types_.size());
    }
    const RuntimeType* find_type(
        std::uint32_t runtime_type_id) const noexcept {
        return runtime_type_id < types_.size() ? &types_[runtime_type_id]
                                               : nullptr;
    }
    std::uint32_t runtime_id(const spec::TypeNode& type) const noexcept;
    std::optional<spec::StorageRole> storage_role(
        std::uint32_t runtime_type_id) const noexcept {
        const RuntimeType* const type = find_type(runtime_type_id);
        if (type == nullptr) {
            return std::nullopt;
        }
        return type->storage_role;
    }
    bool component_reachable(std::uint32_t component_index) const noexcept {
        return component_index < reachable_components_.size() &&
               reachable_components_[component_index] != UINT8_C(0);
    }
    const ComponentLayout* component(
        std::uint32_t component_index) const noexcept {
        if (component_index >= component_layout_indexes_.size()) {
            return nullptr;
        }
        const std::uint32_t layout_index =
            component_layout_indexes_[component_index];
        return layout_index < components_.size() ? &components_[layout_index]
                                                 : nullptr;
    }
    const std::vector<ComponentLayout>& components() const noexcept {
        return components_;
    }
    bool component_identity_bearing(
        std::uint32_t component_index) const noexcept {
        return component_index < identity_component_flags_.size() &&
               identity_component_flags_[component_index] != UINT8_C(0);
    }
    const std::vector<std::uint32_t>& identity_components() const noexcept {
        return identity_components_;
    }
    const std::vector<ListNodeLayout>& list_nodes() const noexcept {
        return list_nodes_;
    }
    bool has_utf8_pool() const noexcept { return has_utf8_pool_; }
    bool has_utf16_pool() const noexcept { return has_utf16_pool_; }
    bool has_bytes_pool() const noexcept { return has_bytes_pool_; }
    const spec::ResolvedSpec& resolved() const noexcept { return *resolved_; }
    const std::array<std::uint8_t, 32>& digest() const noexcept {
        return digest_;
    }

private:
    friend class spec::CompiledSpec;
    friend class RecordLayout;
    friend struct RuntimeSchemaTestAccess;

    using TypeIds =
        std::unordered_map<const spec::TypeNode*, std::uint32_t>;

    RuntimeSchema(std::shared_ptr<const spec::ResolvedSpec> resolved,
                  const std::array<std::uint8_t, 32>& digest)
        : resolved_(std::move(resolved)), digest_(digest) {}

    static error::Result<std::shared_ptr<const RuntimeSchema>> build(
        std::shared_ptr<const spec::ResolvedSpec> resolved,
        const std::array<std::uint8_t, 32>& digest);

    error::Result<void> assign_topology(spec::RuntimeTopology topology);
    error::Result<void> compile_component_layouts();
    error::Result<void> finalize_reachable_inventory();
    error::Result<void> validate_list_metadata() const;

    // Type pointers refer into this shared model. Keeping it separate from
    // the compiled-spec cache avoids an ownership cycle.
    std::shared_ptr<const spec::ResolvedSpec> resolved_;
    std::array<std::uint8_t, 32> digest_;
    std::vector<RuntimeType> types_;
    TypeIds type_ids_;
    std::vector<ComponentLayout> components_;
    std::vector<std::uint32_t> component_layout_indexes_;
    std::vector<std::uint8_t> reachable_components_;
    std::vector<std::uint8_t> identity_component_flags_;
    std::vector<std::uint32_t> identity_components_;
    std::vector<ListNodeLayout> list_nodes_;
    bool has_utf8_pool_{false};
    bool has_utf16_pool_{false};
    bool has_bytes_pool_{false};
};

}  // namespace fastdb::payload::layout
