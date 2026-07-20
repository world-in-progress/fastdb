#include "payload/spec/RuntimeTopology.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"

#include <fastdb_payload.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastdb::payload::spec {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

struct PendingType final {
    const TypeNode* source;
    ValueContext context;
};

Error topology_error(const char* message, const char* reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_INTERNAL,
        JsonPointer{}.append("runtime").append("topology"), message,
        JsonValue::object({
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

Error type_limit_error() {
    return Error::from_details(
        FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT,
        JsonPointer{}.append("runtime").append("types"),
        "Runtime type count exceeds the reserved wire sentinel",
        JsonValue::object({
            JsonValue::Member{
                "actual",
                JsonValue{std::to_string(
                    static_cast<std::uint64_t>(UINT32_MAX) + UINT64_C(1))}},
            JsonValue::Member{"limit",
                              JsonValue{std::to_string(UINT32_MAX)}},
            JsonValue::Member{"resource", JsonValue{"runtime_types"}},
        }));
}

StorageRole storage_role(Profile profile,
                         TypeKind kind,
                         ValueContext context) noexcept {
    switch (kind) {
    case TypeKind::list:
        return StorageRole::list_descriptor;
    case TypeKind::component:
        if (profile == Profile::object_graph_v1 &&
            context == ValueContext::entry_value) {
            return StorageRole::object_root_id;
        }
        return StorageRole::inline_component;
    case TypeKind::ref:
        return StorageRole::reference_id;
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::u8n:
    case TypeKind::u16n:
    case TypeKind::f32:
    case TypeKind::f64:
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
        return StorageRole::ordinary_value;
    }
    return StorageRole::ordinary_value;
}

}  // namespace

Result<RuntimeTopology> derive_runtime_topology(
    const ResolvedSpec& resolved) {
    RuntimeTopology topology;
    const std::size_t component_count = resolved.components().size();
    topology.reachable_components.assign(component_count, UINT8_C(0));
    topology.identity_components.assign(component_count, UINT8_C(0));

    const auto assign_chain = [&](const TypeNode& root,
                                  ValueContext context) -> Result<void> {
        const TypeNode* current = &root;
        while (current != nullptr) {
            if (topology.types.size() >=
                static_cast<std::size_t>(UINT32_MAX)) {
                return Result<void>::failure(type_limit_error());
            }
            topology.types.push_back(RuntimeTypeTopology{
                current, context,
                storage_role(resolved.profile(), current->kind, context),
                false});
            if (current->kind != TypeKind::list) {
                break;
            }
            if (current->items == nullptr) {
                return Result<void>::failure(topology_error(
                    "Runtime list has no resolved item source type",
                    "missing_list_item_type"));
            }
            current = current->items.get();
        }
        return Result<void>::success();
    };

    for (const Entry& entry : resolved.entries()) {
        auto assigned = assign_chain(entry.type, ValueContext::entry_value);
        if (!assigned.has_value()) {
            return Result<RuntimeTopology>::failure(
                std::move(assigned).error());
        }
    }
    for (const Component& component : resolved.components()) {
        for (const Field& field : component.fields) {
            auto assigned =
                assign_chain(field.type, ValueContext::component_field);
            if (!assigned.has_value()) {
                return Result<RuntimeTopology>::failure(
                    std::move(assigned).error());
            }
        }
    }

    std::unordered_map<const TypeNode*, std::uint32_t> type_ids;
    type_ids.reserve(topology.types.size());
    for (std::size_t index = 0U; index < topology.types.size(); ++index) {
        const auto inserted = type_ids.emplace(
            topology.types[index].source,
            static_cast<std::uint32_t>(index));
        if (!inserted.second) {
            return Result<RuntimeTopology>::failure(topology_error(
                "Runtime source type occurrence was assigned twice",
                "duplicate_source_type"));
        }
    }

    std::vector<PendingType> pending;
    pending.reserve(resolved.entries().size());
    for (auto entry = resolved.entries().rbegin();
         entry != resolved.entries().rend(); ++entry) {
        pending.push_back(PendingType{&entry->type,
                                      ValueContext::entry_value});
    }

    const auto mark_identity = [&](std::uint32_t component_index)
        -> Result<void> {
        const std::size_t index =
            static_cast<std::size_t>(component_index);
        if (index >= component_count) {
            return Result<void>::failure(topology_error(
                "Runtime identity target is outside the resolved model",
                "invalid_identity_target"));
        }
        topology.identity_components[index] = UINT8_C(1);
        topology.has_objects = true;
        return Result<void>::success();
    };

    const auto enqueue_component = [&](std::uint32_t component_index)
        -> Result<void> {
        const std::size_t index =
            static_cast<std::size_t>(component_index);
        if (index >= component_count) {
            return Result<void>::failure(topology_error(
                "Runtime component target is outside the resolved model",
                "invalid_component_target"));
        }
        if (topology.reachable_components[index] != UINT8_C(0)) {
            return Result<void>::success();
        }
        topology.reachable_components[index] = UINT8_C(1);
        ++topology.reachable_component_count;
        const auto& fields = resolved.components()[index].fields;
        for (auto field = fields.rbegin(); field != fields.rend(); ++field) {
            pending.push_back(PendingType{&field->type,
                                          ValueContext::component_field});
        }
        return Result<void>::success();
    };

    while (!pending.empty()) {
        const PendingType work = pending.back();
        pending.pop_back();
        const auto found = type_ids.find(work.source);
        if (found == type_ids.end()) {
            return Result<RuntimeTopology>::failure(topology_error(
                "Runtime reachability found an unassigned source type",
                "unassigned_runtime_type"));
        }
        RuntimeTypeTopology& type = topology.types[found->second];
        if (type.context != work.context) {
            return Result<RuntimeTopology>::failure(topology_error(
                "Runtime source type context is inconsistent",
                "runtime_context_mismatch"));
        }
        if (type.reachable) {
            continue;
        }
        type.reachable = true;
        ++topology.reachable_type_count;

        switch (type.source->kind) {
        case TypeKind::str:
            topology.has_utf8 = true;
            break;
        case TypeKind::wstr:
            topology.has_utf16le = true;
            break;
        case TypeKind::bytes:
            topology.has_bytes = true;
            break;
        case TypeKind::list:
            topology.has_list_items = true;
            ++topology.reachable_list_type_count;
            if (type.source->items == nullptr) {
                return Result<RuntimeTopology>::failure(topology_error(
                    "Runtime list has no resolved item source type",
                    "missing_list_item_type"));
            }
            pending.push_back(
                PendingType{type.source->items.get(), work.context});
            break;
        case TypeKind::component: {
            if (resolved.profile() == Profile::object_graph_v1 &&
                work.context == ValueContext::entry_value) {
                auto identity = mark_identity(
                    type.source->resolved_component_index);
                if (!identity.has_value()) {
                    return Result<RuntimeTopology>::failure(
                        std::move(identity).error());
                }
                topology.has_roots = true;
            }
            auto component = enqueue_component(
                type.source->resolved_component_index);
            if (!component.has_value()) {
                return Result<RuntimeTopology>::failure(
                    std::move(component).error());
            }
            break;
        }
        case TypeKind::ref: {
            topology.has_references = true;
            if (resolved.profile() == Profile::object_graph_v1 &&
                work.context == ValueContext::entry_value) {
                topology.has_roots = true;
            }
            auto identity =
                mark_identity(type.source->resolved_component_index);
            if (!identity.has_value()) {
                return Result<RuntimeTopology>::failure(
                    std::move(identity).error());
            }
            auto component = enqueue_component(
                type.source->resolved_component_index);
            if (!component.has_value()) {
                return Result<RuntimeTopology>::failure(
                    std::move(component).error());
            }
            break;
        }
        case TypeKind::boolean:
        case TypeKind::u8:
        case TypeKind::u16:
        case TypeKind::u32:
        case TypeKind::i32:
        case TypeKind::u8n:
        case TypeKind::u16n:
        case TypeKind::f32:
        case TypeKind::f64:
            break;
        }
    }

    return Result<RuntimeTopology>::success(std::move(topology));
}

}  // namespace fastdb::payload::spec
