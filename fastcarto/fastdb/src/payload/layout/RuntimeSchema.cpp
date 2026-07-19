#include "payload/layout/RuntimeSchema.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/CheckedMath.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fastdb::payload::layout {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;
using spec::Component;
using spec::Profile;
using spec::TypeKind;
using spec::TypeNode;

Error runtime_error(const JsonPointer& path,
                    std::uint32_t code,
                    const char* message,
                    const char* reason) {
    return Error::from_details(
        code, path, message,
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

SlotLayout primitive_slot(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u8n:
        return {UINT32_C(1), UINT32_C(1)};
    case TypeKind::u16:
    case TypeKind::u16n:
        return {UINT32_C(2), UINT32_C(2)};
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::f32:
        return {UINT32_C(4), UINT32_C(4)};
    case TypeKind::f64:
        return {UINT32_C(8), UINT32_C(8)};
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
    case TypeKind::list:
        return {UINT32_C(16), UINT32_C(8)};
    case TypeKind::component:
    case TypeKind::ref:
        return {UINT32_C(0), UINT32_C(0)};
    }
    return {UINT32_C(0), UINT32_C(0)};
}

}  // namespace

Result<void> RuntimeSchema::require_record_runtime(
    const spec::CompiledSpec& compiled) {
    if (compiled.profile() == Profile::record_v1) {
        return Result<void>::success();
    }
    return Result<void>::failure(Error::from_details(
        FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE, JsonPointer{},
        "Payload runtime is unavailable for this profile",
        JsonValue::object({
            JsonValue::Member{"profile", JsonValue{"object_graph.v1"}},
            JsonValue::Member{"reason",
                              JsonValue{"runtime_slice_not_implemented"}},
        })));
}

Result<RuntimeSchema> RuntimeSchema::compile(
    const spec::CompiledSpec& compiled) {
    auto available = require_record_runtime(compiled);
    if (!available.has_value()) {
        return Result<RuntimeSchema>::failure(
            std::move(available).error());
    }
    RuntimeSchema schema(compiled);
    auto assigned = schema.assign_all_types();
    if (!assigned.has_value()) {
        return Result<RuntimeSchema>::failure(std::move(assigned).error());
    }
    auto reachable = schema.analyze_reachability();
    if (!reachable.has_value()) {
        return Result<RuntimeSchema>::failure(std::move(reachable).error());
    }
    auto components = schema.compile_component_layouts();
    if (!components.has_value()) {
        return Result<RuntimeSchema>::failure(std::move(components).error());
    }
    auto inventory = schema.finalize_reachable_inventory();
    if (!inventory.has_value()) {
        return Result<RuntimeSchema>::failure(std::move(inventory).error());
    }
    return Result<RuntimeSchema>::success(std::move(schema));
}

std::uint32_t RuntimeSchema::runtime_id(const TypeNode& type) const noexcept {
    const auto found = type_ids_.find(&type);
    return found == type_ids_.end() ? UINT32_MAX : found->second;
}

Result<void> RuntimeSchema::assign_all_types() {
    auto assign_chain = [this](const TypeNode& root) -> Result<void> {
        const TypeNode* current = &root;
        while (current != nullptr) {
            if (types_.size() >= static_cast<std::size_t>(UINT32_MAX)) {
                return Result<void>::failure(Error::from_details(
                    FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT,
                    JsonPointer{}.append("runtime").append("types"),
                    "Runtime type count exceeds the reserved wire sentinel",
                    JsonValue::object({
                        JsonValue::Member{
                            "actual",
                            JsonValue{std::to_string(
                                static_cast<std::uint64_t>(UINT32_MAX) +
                                UINT64_C(1))}},
                        JsonValue::Member{
                            "limit", JsonValue{std::to_string(UINT32_MAX)}},
                        JsonValue::Member{
                            "resource", JsonValue{"runtime_types"}},
                    })));
            }
            const auto id = static_cast<std::uint32_t>(types_.size());
            types_.push_back(
                RuntimeType{id, current, primitive_slot(current->kind), false});
            type_ids_.emplace(current, id);
            if (current->kind != TypeKind::list) {
                break;
            }
            current = current->items.get();
        }
        return Result<void>::success();
    };

    for (const spec::Entry& entry : spec_.resolved().entries()) {
        auto assigned = assign_chain(entry.type);
        if (!assigned.has_value()) {
            return assigned;
        }
    }
    for (const Component& component : spec_.resolved().components()) {
        for (const spec::Field& field : component.fields) {
            auto assigned = assign_chain(field.type);
            if (!assigned.has_value()) {
                return assigned;
            }
        }
    }
    reachable_components_.assign(spec_.resolved().components().size(),
                                 UINT8_C(0));
    return Result<void>::success();
}

Result<void> RuntimeSchema::analyze_reachability() {
    std::vector<const TypeNode*> pending;
    pending.reserve(spec_.resolved().entries().size());
    for (const spec::Entry& entry : spec_.resolved().entries()) {
        pending.push_back(&entry.type);
    }
    while (!pending.empty()) {
        const TypeNode* const type = pending.back();
        pending.pop_back();
        const std::uint32_t id = runtime_id(*type);
        if (id == UINT32_MAX) {
            return Result<void>::failure(runtime_error(
                JsonPointer{}.append("runtime").append("types"),
                FDB_PAYLOAD_E_INTERNAL,
                "Runtime reachability found an unassigned source type",
                "unassigned_runtime_type"));
        }
        if (types_[id].reachable) {
            continue;
        }
        types_[id].reachable = true;
        if (type->kind == TypeKind::list) {
            if (type->items == nullptr) {
                return Result<void>::failure(runtime_error(
                    JsonPointer{}.append("runtime").append("types"),
                    FDB_PAYLOAD_E_INTERNAL,
                    "Runtime list has no resolved item source type",
                    "missing_list_item_type"));
            }
            pending.push_back(type->items.get());
        } else if (type->kind == TypeKind::component) {
            const std::uint32_t component_index =
                type->resolved_component_index;
            if (component_index >= reachable_components_.size()) {
                return Result<void>::failure(runtime_error(
                    JsonPointer{}.append("runtime").append("components"),
                    FDB_PAYLOAD_E_INTERNAL,
                    "Runtime component target is outside the resolved model",
                    "invalid_component_target"));
            }
            if (reachable_components_[component_index] == UINT8_C(0)) {
                reachable_components_[component_index] = UINT8_C(1);
                const Component& component =
                    spec_.resolved().components()[component_index];
                for (auto field = component.fields.rbegin();
                     field != component.fields.rend(); ++field) {
                    pending.push_back(&field->type);
                }
            }
        } else if (type->kind == TypeKind::ref) {
            return Result<void>::failure(runtime_error(
                JsonPointer{}.append("runtime").append("types"),
                FDB_PAYLOAD_E_INTERNAL,
                "Record runtime reached a forbidden reference type",
                "record_ref_invariant"));
        }
    }
    return Result<void>::success();
}

Result<void> RuntimeSchema::compile_component_layouts() {
    const auto& source_components = spec_.resolved().components();
    component_layout_indexes_.assign(source_components.size(), UINT32_MAX);
    std::vector<std::uint32_t> dependency_count(source_components.size(),
                                                UINT32_C(0));
    std::vector<std::vector<std::uint32_t>> dependents(
        source_components.size());
    for (std::uint32_t component_index = UINT32_C(0);
         component_index < source_components.size(); ++component_index) {
        if (!component_reachable(component_index)) {
            continue;
        }
        const Component& component = source_components[component_index];
        for (const spec::Field& field : component.fields) {
            if (field.type.kind == TypeKind::ref) {
                return Result<void>::failure(runtime_error(
                    JsonPointer{}.append("runtime").append("components"),
                    FDB_PAYLOAD_E_INTERNAL,
                    "Record component layout reached a reference type",
                    "record_ref_invariant"));
            }
            if (field.type.kind != TypeKind::component) {
                continue;
            }
            const std::uint32_t target = field.type.resolved_component_index;
            if (target >= source_components.size() ||
                !component_reachable(target) ||
                dependency_count[component_index] == UINT32_MAX) {
                return Result<void>::failure(runtime_error(
                    JsonPointer{}.append("runtime").append("components"),
                    FDB_PAYLOAD_E_INTERNAL,
                    "Runtime component dependency is outside the resolved model",
                    "invalid_component_dependency"));
            }
            ++dependency_count[component_index];
            dependents[target].push_back(component_index);
        }
    }

    std::vector<std::uint32_t> ready;
    ready.reserve(source_components.size());
    std::size_t reachable_count = 0U;
    for (std::uint32_t component_index = UINT32_C(0);
         component_index < dependency_count.size(); ++component_index) {
        if (!component_reachable(component_index)) {
            continue;
        }
        ++reachable_count;
        if (dependency_count[component_index] == UINT32_C(0)) {
            ready.push_back(component_index);
        }
    }

    std::size_t next_ready = 0U;
    while (next_ready < ready.size()) {
        const std::uint32_t component_index = ready[next_ready++];
        const Component& component = source_components[component_index];
        std::uint64_t nullable_count = UINT64_C(0);
        for (const spec::Field& field : component.fields) {
            if (field.type.nullable) {
                auto accumulated = checked_accumulate_u64(
                    nullable_count, UINT64_C(1),
                    JsonPointer{}.append("components").append(component.id));
                if (!accumulated.has_value()) {
                    return accumulated;
                }
            }
        }
        auto validity_rounded = checked_add_u64(
            nullable_count, UINT64_C(7),
            JsonPointer{}.append("components").append(component.id));
        if (!validity_rounded.has_value()) {
            return Result<void>::failure(std::move(validity_rounded).error());
        }
        auto validity_bytes = checked_narrow_u32(
            validity_rounded.value() / UINT64_C(8),
            JsonPointer{}.append("components").append(component.id));
        if (!validity_bytes.has_value()) {
            return Result<void>::failure(std::move(validity_bytes).error());
        }

        ComponentLayout result{component_index, UINT32_C(0), UINT32_C(1),
                               validity_bytes.value(), {}};
        result.fields.reserve(component.fields.size());
        std::uint64_t cursor = validity_bytes.value();
        std::uint32_t maximum_alignment = UINT32_C(1);
        std::uint32_t validity_bit = UINT32_C(0);
        for (const spec::Field& field : component.fields) {
            SlotLayout slot = primitive_slot(field.type.kind);
            if (field.type.kind == TypeKind::component) {
                const ComponentLayout* const nested =
                    this->component(field.type.resolved_component_index);
                if (nested == nullptr) {
                    return Result<void>::failure(runtime_error(
                        JsonPointer{}
                            .append("components")
                            .append(component.id)
                            .append(field.id),
                        FDB_PAYLOAD_E_INTERNAL,
                        "Runtime component dependency layout is missing",
                        "component_dependency_layout_missing"));
                }
                slot = {nested->stride, nested->alignment};
            }
            const JsonPointer field_path = JsonPointer{}
                                                     .append("components")
                                                     .append(component.id)
                                                     .append(field.id);
            auto aligned =
                checked_align_up_u64(cursor, slot.alignment, field_path);
            if (!aligned.has_value()) {
                return Result<void>::failure(std::move(aligned).error());
            }
            auto offset = checked_narrow_u32(aligned.value(), field_path);
            if (!offset.has_value()) {
                return Result<void>::failure(std::move(offset).error());
            }
            const std::uint32_t bit =
                field.type.nullable ? validity_bit++ : UINT32_MAX;
            const std::uint32_t field_type_id = runtime_id(field.type);
            if (field_type_id == UINT32_MAX ||
                find_type(field_type_id) == nullptr) {
                return Result<void>::failure(runtime_error(
                    field_path, FDB_PAYLOAD_E_INTERNAL,
                    "Runtime component field type is unassigned",
                    "unassigned_runtime_type"));
            }
            result.fields.push_back(ComponentFieldLayout{
                field_type_id, offset.value(), slot.stride,
                slot.alignment, bit});
            auto end =
                checked_add_u64(aligned.value(), slot.stride, field_path);
            if (!end.has_value()) {
                return Result<void>::failure(std::move(end).error());
            }
            cursor = end.value();
            maximum_alignment = std::max(maximum_alignment, slot.alignment);
        }
        if (component.fields.empty()) {
            result.stride = UINT32_C(1);
            result.alignment = UINT32_C(1);
        } else {
            auto stride = checked_align_up_u64(
                cursor, maximum_alignment,
                JsonPointer{}.append("components").append(component.id));
            if (!stride.has_value()) {
                return Result<void>::failure(std::move(stride).error());
            }
            auto narrowed = checked_narrow_u32(
                stride.value(),
                JsonPointer{}.append("components").append(component.id));
            if (!narrowed.has_value()) {
                return Result<void>::failure(std::move(narrowed).error());
            }
            result.stride = narrowed.value();
            result.alignment = maximum_alignment;
        }
        auto layout_index = checked_narrow_u32(
            components_.size(),
            JsonPointer{}.append("runtime").append("components"));
        if (!layout_index.has_value()) {
            return Result<void>::failure(std::move(layout_index).error());
        }
        component_layout_indexes_[component_index] = layout_index.value();
        components_.push_back(std::move(result));
        for (const std::uint32_t dependent : dependents[component_index]) {
            if (dependency_count[dependent] == UINT32_C(0)) {
                return Result<void>::failure(runtime_error(
                    JsonPointer{}.append("runtime").append("components"),
                    FDB_PAYLOAD_E_INTERNAL,
                    "Runtime component dependency accounting underflowed",
                    "component_dependency_underflow"));
            }
            --dependency_count[dependent];
            if (dependency_count[dependent] == UINT32_C(0)) {
                ready.push_back(dependent);
            }
        }
    }
    if (components_.size() != reachable_count) {
        return Result<void>::failure(runtime_error(
            JsonPointer{}.append("runtime").append("components"),
            FDB_PAYLOAD_E_INTERNAL,
            "Runtime component layout contains an internal by-value cycle",
            "component_layout_cycle"));
    }

    for (RuntimeType& type : types_) {
        if (type.reachable && type.source->kind == TypeKind::component) {
            const ComponentLayout* const component_layout =
                component(type.source->resolved_component_index);
            if (component_layout == nullptr) {
                return Result<void>::failure(runtime_error(
                    JsonPointer{}.append("runtime").append("components"),
                    FDB_PAYLOAD_E_INTERNAL,
                    "Reachable runtime component layout is missing",
                    "reachable_component_layout_missing"));
            }
            type.slot = {component_layout->stride,
                         component_layout->alignment};
        }
    }
    return Result<void>::success();
}

Result<void> RuntimeSchema::finalize_reachable_inventory() {
    for (RuntimeType& type : types_) {
        if (!type.reachable) {
            continue;
        }
        switch (type.source->kind) {
        case TypeKind::list: {
            const std::uint32_t item_id = runtime_id(*type.source->items);
            const RuntimeType* const item = find_type(item_id);
            if (item_id == UINT32_MAX || item == nullptr) {
                return Result<void>::failure(runtime_error(
                    JsonPointer{}.append("runtime").append("lists"),
                    FDB_PAYLOAD_E_INTERNAL,
                    "Runtime list item type is unassigned",
                    "unassigned_runtime_type"));
            }
            list_nodes_.push_back(ListNodeLayout{
                type.runtime_type_id, item_id, item->slot.stride,
                item->slot.alignment, item->source->nullable});
            break;
        }
        case TypeKind::str:
            has_utf8_pool_ = true;
            break;
        case TypeKind::wstr:
            has_utf16_pool_ = true;
            break;
        case TypeKind::bytes:
            has_bytes_pool_ = true;
            break;
        default:
            break;
        }
    }
    std::sort(list_nodes_.begin(), list_nodes_.end(),
              [](const ListNodeLayout& left, const ListNodeLayout& right) {
                  return left.owner_runtime_type_id <
                         right.owner_runtime_type_id;
              });
    return Result<void>::success();
}

Result<void> RuntimeSchema::validate_list_metadata() const {
    const JsonPointer path = JsonPointer{}.append("runtime").append("lists");
    std::vector<std::uint32_t> metadata_indexes(types_.size(), UINT32_MAX);
    std::size_t reachable_list_count = 0U;
    for (const RuntimeType& type : types_) {
        if (type.reachable && type.source != nullptr &&
            type.source->kind == TypeKind::list) {
            ++reachable_list_count;
        }
    }
    if (list_nodes_.size() != reachable_list_count) {
        return Result<void>::failure(runtime_error(
            path, FDB_PAYLOAD_E_INTERNAL,
            "Reachable list runtime metadata is incomplete",
            "missing_list_runtime_metadata"));
    }
    std::uint32_t previous_owner = UINT32_C(0);
    bool have_previous = false;
    for (std::uint32_t index = UINT32_C(0); index < list_nodes_.size();
         ++index) {
        const ListNodeLayout& node = list_nodes_[index];
        if (node.owner_runtime_type_id >= types_.size() ||
            node.item_runtime_type_id >= types_.size()) {
            return Result<void>::failure(runtime_error(
                path, FDB_PAYLOAD_E_INTERNAL,
                "List runtime metadata references a missing type",
                "missing_list_runtime_metadata"));
        }
        const RuntimeType& owner = types_[node.owner_runtime_type_id];
        if (!owner.reachable || owner.source == nullptr ||
            owner.source->kind != TypeKind::list) {
            return Result<void>::failure(runtime_error(
                path, FDB_PAYLOAD_E_INTERNAL,
                "List runtime metadata owner is not a reachable list",
                "list_runtime_metadata_mismatch"));
        }
        if ((have_previous &&
             node.owner_runtime_type_id <= previous_owner) ||
            metadata_indexes[node.owner_runtime_type_id] != UINT32_MAX) {
            return Result<void>::failure(runtime_error(
                path, FDB_PAYLOAD_E_INTERNAL,
                "List runtime metadata is not uniquely sorted",
                "list_runtime_metadata_order"));
        }
        metadata_indexes[node.owner_runtime_type_id] = index;
        previous_owner = node.owner_runtime_type_id;
        have_previous = true;
    }
    for (const RuntimeType& type : types_) {
        if (type.reachable && type.source != nullptr &&
            type.source->kind == TypeKind::list &&
            metadata_indexes[type.runtime_type_id] == UINT32_MAX) {
            return Result<void>::failure(runtime_error(
                path, FDB_PAYLOAD_E_INTERNAL,
                "Reachable list runtime metadata is missing",
                "missing_list_runtime_metadata"));
        }
    }

    std::vector<std::uint8_t> state(types_.size(), UINT8_C(0));
    std::vector<std::uint32_t> chain;
    for (const ListNodeLayout& root : list_nodes_) {
        std::uint32_t current = root.owner_runtime_type_id;
        chain.clear();
        while (current < metadata_indexes.size() &&
               metadata_indexes[current] != UINT32_MAX) {
            if (state[current] == UINT8_C(1)) {
                return Result<void>::failure(runtime_error(
                    path, FDB_PAYLOAD_E_INTERNAL,
                    "List runtime metadata contains a cycle",
                    "cyclic_list_runtime_metadata"));
            }
            if (state[current] == UINT8_C(2)) {
                break;
            }
            state[current] = UINT8_C(1);
            chain.push_back(current);
            const ListNodeLayout& node =
                list_nodes_[metadata_indexes[current]];
            current = node.item_runtime_type_id;
        }
        for (const std::uint32_t runtime_type_id : chain) {
            state[runtime_type_id] = UINT8_C(2);
        }
    }

    for (const ListNodeLayout& node : list_nodes_) {
        const RuntimeType& owner = types_[node.owner_runtime_type_id];
        const std::uint32_t expected_item = runtime_id(*owner.source->items);
        const RuntimeType& item = types_[node.item_runtime_type_id];
        if (expected_item == UINT32_MAX ||
            node.item_runtime_type_id != expected_item || !item.reachable ||
            node.item_stride != item.slot.stride ||
            node.item_alignment != item.slot.alignment ||
            node.item_nullable != item.source->nullable) {
            return Result<void>::failure(runtime_error(
                path, FDB_PAYLOAD_E_INTERNAL,
                "List runtime metadata does not match its source type",
                "list_runtime_metadata_mismatch"));
        }
    }
    return Result<void>::success();
}

}  // namespace fastdb::payload::layout
