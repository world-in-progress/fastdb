#pragma once

#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/view/AccessBarrier.hpp"
#include "payload/view/Open.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace fastdb::payload::view {

struct PayloadOwnerState;
struct MaterializeMetrics;
struct GraphMaterializeInternals;
struct GraphViewInternals;
struct ViewInternals;
struct ViewTestAccess;
class View;

error::Result<View> materialize_with_metrics(
    const View& view,
    MaterializeMetrics* metrics);
error::Result<View> materialize_graph_with_metrics(
    const View& view,
    MaterializeMetrics* metrics);

enum class ViewKind : std::uint32_t {
    sequence = UINT32_C(1),
    boolean = UINT32_C(2),
    u8 = UINT32_C(3),
    u16 = UINT32_C(4),
    u32 = UINT32_C(5),
    i32 = UINT32_C(6),
    u8n = UINT32_C(7),
    u16n = UINT32_C(8),
    f32 = UINT32_C(9),
    f64 = UINT32_C(10),
    str = UINT32_C(11),
    wstr = UINT32_C(12),
    bytes = UINT32_C(13),
    component = UINT32_C(14),
    list = UINT32_C(15),
    ref = UINT32_C(16),
};

struct ByteSpan final {
    const std::uint8_t* data;
    std::uint64_t size;
};

struct WideSpan final {
    const std::uint16_t* data;
    std::uint64_t size;
};

enum class AccessKind : std::uint8_t {
    payload,
    str,
    wstr,
    bytes,
};

struct DetachedViewState final {
    DetachedViewState(build::ValueArena arena_value,
                      std::shared_ptr<const layout::RuntimeSchema> runtime,
                      build::NodeIndex root_value) noexcept
        : DetachedViewState(std::move(arena_value), {}, std::move(runtime),
                            root_value) {}

    DetachedViewState(
        build::ValueArena arena_value,
        std::vector<std::vector<build::NodeIndex>> object_pools_value,
        std::shared_ptr<const layout::RuntimeSchema> runtime,
        build::NodeIndex root_value) noexcept
        : arena(std::move(arena_value)),
          object_pools(std::move(object_pools_value)),
          runtime_schema(std::move(runtime)),
          root(root_value) {}

    build::NodeIndex object_record_node(
        std::uint32_t component_index,
        std::uint64_t object_id) const noexcept {
        if (component_index >= object_pools.size() ||
            object_id >= object_pools[component_index].size()) {
            return build::invalid_node_index;
        }
        const build::NodeIndex node =
            object_pools[component_index][static_cast<std::size_t>(object_id)];
        if (node >= arena.nodes().size()) {
            return build::invalid_node_index;
        }
        const build::ValueNode& record =
            arena.nodes()[static_cast<std::size_t>(node)];
        return record.tag == build::ValueTag::object_record &&
                       record.object_component_index == component_index &&
                       record.object_id == object_id
                   ? node
                   : build::invalid_node_index;
    }

    const build::ValueNode* object_record(
        std::uint32_t component_index,
        std::uint64_t object_id) const noexcept {
        const build::NodeIndex node =
            object_record_node(component_index, object_id);
        return node == build::invalid_node_index
                   ? nullptr
                   : &arena.nodes()[static_cast<std::size_t>(node)];
    }

    build::ValueArena arena;
    std::vector<std::vector<build::NodeIndex>> object_pools;
    std::shared_ptr<const layout::RuntimeSchema> runtime_schema;
    build::NodeIndex root;
};

class Access final {
public:
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;
    Access(Access&&) noexcept;
    Access& operator=(Access&&) noexcept;
    ~Access();

    error::Result<ByteSpan> payload_bytes() const;
    error::Result<ByteSpan> str() const;
    error::Result<WideSpan> wstr() const;
    error::Result<ByteSpan> bytes() const;

private:
    struct State;
    explicit Access(std::unique_ptr<State> state) noexcept;

    friend class PayloadOwner;
    friend class View;
    friend struct ViewInternals;

    std::unique_ptr<State> state_;
};

class View final {
public:
    View(const View&) noexcept = default;
    View& operator=(const View&) noexcept = default;
    View(View&&) noexcept = default;
    View& operator=(View&&) noexcept = default;
    ~View() = default;

    error::Result<ViewKind> kind() const;
    error::Result<bool> is_null() const;
    error::Result<std::uint64_t> length() const;
    error::Result<View> at(std::uint64_t index) const;
    error::Result<std::uint32_t> component_index() const;
    error::Result<std::uint32_t> field_count() const;
    error::Result<View> field(std::uint32_t index) const;
    error::Result<View> ref_target() const;
    error::Result<GraphIdentity> graph_identity() const;
    error::Result<std::uint8_t> get_bool() const;
    error::Result<std::uint8_t> get_u8() const;
    error::Result<std::uint16_t> get_u16() const;
    error::Result<std::uint32_t> get_u32() const;
    error::Result<std::int32_t> get_i32() const;
    error::Result<std::uint64_t> get_u8n_f64_bits() const;
    error::Result<std::uint64_t> get_u16n_f64_bits() const;
    error::Result<std::uint32_t> get_f32_bits() const;
    error::Result<std::uint64_t> get_f64_bits() const;
    error::Result<Access> acquire() const;
    error::Result<View> materialize() const;
    error::Result<void>
    require_spec_sha256(const std::array<std::uint8_t, 32>& expected) const;

private:
    struct State;
    explicit View(std::shared_ptr<const State> state) noexcept;

    friend class PayloadOwner;
    friend struct GraphMaterializeInternals;
    friend struct GraphViewInternals;
    friend struct ViewInternals;
    friend struct ViewTestAccess;
    friend error::Result<View> materialize(const View& view);
    friend error::Result<View> materialize_with_metrics(
        const View& view,
        MaterializeMetrics* metrics);
    friend error::Result<View> materialize_graph_with_metrics(
        const View& view,
        MaterializeMetrics* metrics);

    std::shared_ptr<const State> state_;
};

struct Access::State final {
    std::shared_ptr<PayloadOwnerState> owner;
    std::shared_ptr<const DetachedViewState> detached;
    std::optional<AccessPin> pin;
    AccessKind kind{AccessKind::payload};
    const std::uint8_t* bytes{nullptr};
    std::uint64_t byte_count{UINT64_C(0)};
    std::vector<std::uint16_t> wide_units;
};

struct View::State final {
    bool is_backed{false};
    bool is_sequence{false};
    std::shared_ptr<PayloadOwnerState> owner;
    std::uint64_t generation{UINT64_C(0)};
    EntrySequenceCursor sequence{UINT32_C(0)};
    ValueCursor cursor{InlineValueCursor{
        UINT32_C(0), spec::TypeKind::boolean, UINT64_C(0), false}};
    std::shared_ptr<const DetachedViewState> detached;
    build::NodeIndex node{build::invalid_node_index};
    json::JsonPointer diagnostic_path;
};

}  // namespace fastdb::payload::view
