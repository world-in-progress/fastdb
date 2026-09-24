#pragma once

#include "payload/spec/CompiledSpec.hpp"

#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace fastdb::payload::build {

using NodeIndex = std::uint64_t;
inline constexpr NodeIndex invalid_node_index =
    std::numeric_limits<NodeIndex>::max();
using ObjectHandle = std::uint64_t;
inline constexpr ObjectHandle invalid_object_handle = UINT64_C(0);

struct ObjectCoordinate final {
    std::uint32_t component_index;
    std::uint64_t object_id;
};

enum class ValueTag : std::uint8_t {
    null_value,
    boolean,
    u8,
    u16,
    u32,
    i32,
    u8n,
    u16n,
    f32,
    f64,
    str,
    wstr,
    bytes,
    component,
    list,
    sequence,
    object_record,
    object_root,
    reference,
};

struct ValueNode final {
    std::uint32_t runtime_type_id;
    ValueTag tag;
    std::uint8_t reserved8[3];
    std::uint64_t scalar_bits_or_offset;
    std::uint64_t byte_length;
    NodeIndex first_child;
    NodeIndex next_sibling;
    std::uint64_t child_count;
    std::uint32_t object_component_index{UINT32_MAX};
    std::uint32_t reserved32{UINT32_C(0)};
    std::uint64_t object_id{UINT64_MAX};
};

class ValueArena final {
public:
    const std::vector<ValueNode>& nodes() const noexcept { return nodes_; }
    const std::vector<std::uint8_t>& byte_storage() const noexcept {
        return byte_storage_;
    }

    /* Core-internal mutable storage; only the builder publishes it. */
    std::vector<ValueNode> nodes_;
    std::vector<std::uint8_t> byte_storage_;
};

class LogicalPayload final {
public:
    LogicalPayload(const LogicalPayload&) = delete;
    LogicalPayload& operator=(const LogicalPayload&) = delete;
    LogicalPayload(LogicalPayload&&) noexcept = default;
    LogicalPayload& operator=(LogicalPayload&&) noexcept = default;
    ~LogicalPayload() = default;

    const spec::CompiledSpec& spec() const noexcept { return spec_; }
    const std::vector<ValueNode>& nodes() const noexcept {
        return arena_.nodes_;
    }
    const std::vector<NodeIndex>& entry_roots() const noexcept {
        return entry_roots_;
    }
    const std::vector<std::vector<NodeIndex>>& object_pools() const noexcept {
        return object_pools_;
    }
    std::uint64_t graph_object_count() const noexcept {
        return graph_object_count_;
    }
    std::string_view byte_storage() const noexcept {
        if (arena_.byte_storage_.empty()) {
            return {};
        }
        return std::string_view{
            reinterpret_cast<const char*>(arena_.byte_storage_.data()),
            arena_.byte_storage_.size()};
    }

private:
    friend class PayloadBuilder;

    LogicalPayload(spec::CompiledSpec spec,
                   ValueArena arena,
                   std::vector<NodeIndex> entry_roots,
                   std::vector<std::vector<NodeIndex>> object_pools,
                   std::uint64_t graph_object_count) noexcept
        : spec_(std::move(spec)),
          arena_(std::move(arena)),
          entry_roots_(std::move(entry_roots)),
          object_pools_(std::move(object_pools)),
          graph_object_count_(graph_object_count) {}

    spec::CompiledSpec spec_;
    ValueArena arena_;
    std::vector<NodeIndex> entry_roots_;
    std::vector<std::vector<NodeIndex>> object_pools_;
    std::uint64_t graph_object_count_{UINT64_C(0)};
};

}  // namespace fastdb::payload::build
