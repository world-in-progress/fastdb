#pragma once

#include "payload/error/Result.hpp"
#include "payload/spec/CompiledSpec.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fastdb::payload::layout {

struct SlotLayout final {
    std::uint32_t stride;
    std::uint32_t alignment;
};

struct RuntimeType final {
    std::uint32_t runtime_type_id;
    const spec::TypeNode* source;
    SlotLayout slot;
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
    static error::Result<RuntimeSchema> compile(
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
    const std::vector<ListNodeLayout>& list_nodes() const noexcept {
        return list_nodes_;
    }
    bool has_utf8_pool() const noexcept { return has_utf8_pool_; }
    bool has_utf16_pool() const noexcept { return has_utf16_pool_; }
    bool has_bytes_pool() const noexcept { return has_bytes_pool_; }
    const spec::CompiledSpec& spec() const noexcept { return spec_; }

private:
    explicit RuntimeSchema(spec::CompiledSpec spec)
        : spec_(std::move(spec)) {}

    error::Result<void> assign_all_types();
    error::Result<void> analyze_reachability();
    error::Result<void> compile_component_layouts();
    error::Result<void> finalize_reachable_inventory();

    spec::CompiledSpec spec_;
    std::vector<RuntimeType> types_;
    std::unordered_map<const spec::TypeNode*, std::uint32_t> type_ids_;
    std::vector<ComponentLayout> components_;
    std::vector<std::uint32_t> component_layout_indexes_;
    std::vector<std::uint8_t> reachable_components_;
    std::vector<ListNodeLayout> list_nodes_;
    bool has_utf8_pool_{false};
    bool has_utf16_pool_{false};
    bool has_bytes_pool_{false};
};

}  // namespace fastdb::payload::layout
