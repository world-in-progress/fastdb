#pragma once

#include "payload/error/Result.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Open.hpp"

#include <cstdint>

namespace fastdb::payload::view {

struct GraphOpenFacts final {
    std::uint64_t total_length;
    std::uint64_t root_value_count;
    std::uint64_t graph_object_count;
    std::uint64_t validation_work;
    std::uint32_t region_count;
    std::uint32_t entry_count;
};

error::Result<GraphOpenFacts> open_graph(
    const spec::CompiledSpec& spec,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    OpenOptions limits = default_open_options());

}  // namespace fastdb::payload::view
