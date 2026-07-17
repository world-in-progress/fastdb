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
                   std::vector<NodeIndex> entry_roots) noexcept
        : spec_(std::move(spec)),
          arena_(std::move(arena)),
          entry_roots_(std::move(entry_roots)) {}

    spec::CompiledSpec spec_;
    ValueArena arena_;
    std::vector<NodeIndex> entry_roots_;
};

}  // namespace fastdb::payload::build
