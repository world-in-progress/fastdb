#pragma once

#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/spec/Model.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fastdb::payload::build {

struct GraphAuthoringTestAccess;

class GraphAuthoring final {
public:
    error::Result<void> initialize(std::size_t component_count);

    std::uint64_t object_count() const noexcept {
        return graph_object_count_;
    }
    std::uint64_t component_object_count(
        std::uint32_t component_index) const noexcept;

    error::Result<void> prepare_declaration(
        std::uint32_t component_index,
        const json::JsonPointer& path);
    error::Result<ObjectHandle> commit_declaration(
        std::uint32_t component_index,
        NodeIndex object_node,
        const json::JsonPointer& path);
    error::Result<ObjectCoordinate> resolve(
        ObjectHandle object,
        const json::JsonPointer& path) const;

    bool filled(ObjectCoordinate object) const noexcept;
    NodeIndex object_node(ObjectCoordinate object) const noexcept;
    void mark_filled(ObjectCoordinate object) noexcept;
    error::Result<void> validate_filled(
        const spec::ResolvedSpec& resolved) const;
    error::Result<void> validate_reachable(
        const spec::ResolvedSpec& resolved,
        const ValueArena& arena,
        const std::vector<NodeIndex>& entry_roots) const;

    std::vector<std::vector<NodeIndex>> take_object_pools() noexcept;
    void restore_object_pools(
        std::vector<std::vector<NodeIndex>> object_pools) noexcept;
    void commit_frozen_state() noexcept;

private:
    error::Result<ObjectHandle> allocate_handle(
        const json::JsonPointer& path);
    bool use_test_sequence(ObjectHandle next) noexcept;

    std::vector<std::vector<NodeIndex>> object_pools_;
    std::vector<std::vector<std::uint8_t>> filled_;
    std::unordered_map<ObjectHandle, ObjectCoordinate> handles_;
    std::uint64_t graph_object_count_{UINT64_C(0)};
    ObjectHandle test_next_handle_{invalid_object_handle};
    bool use_test_handle_sequence_{false};

    friend struct GraphAuthoringTestAccess;
};

struct GraphAuthoringTestAccess final {
    static bool use_object_handle_sequence(GraphAuthoring& graph,
                                           ObjectHandle next) noexcept;
};

}  // namespace fastdb::payload::build
