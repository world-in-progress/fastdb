#pragma once

#include "payload/error/Result.hpp"
#include "payload/spec/Model.hpp"

#include <cstdint>
#include <vector>

namespace fastdb::payload::spec {

enum class ValueContext : std::uint8_t {
    entry_value,
    component_field,
};

enum class StorageRole : std::uint8_t {
    ordinary_value,
    list_descriptor,
    object_root_id,
    inline_component,
    reference_id,
};

struct RuntimeTypeTopology final {
    const TypeNode* source;
    ValueContext context;
    StorageRole storage_role;
    bool reachable;
};

struct RuntimeTopology final {
    std::vector<RuntimeTypeTopology> types;
    std::vector<std::uint8_t> reachable_components;
    std::vector<std::uint8_t> identity_components;
    std::uint32_t reachable_type_count{UINT32_C(0)};
    std::uint32_t reachable_component_count{UINT32_C(0)};
    std::uint32_t reachable_list_type_count{UINT32_C(0)};
    bool has_utf8{false};
    bool has_utf16le{false};
    bool has_bytes{false};
    bool has_list_items{false};
    bool has_objects{false};
    bool has_references{false};
    bool has_roots{false};
};

error::Result<RuntimeTopology> derive_runtime_topology(
    const ResolvedSpec& resolved);

}  // namespace fastdb::payload::spec
