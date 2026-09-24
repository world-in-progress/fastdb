#pragma once

#include "payload/build/ValueArena.hpp"

#include <cstdint>
#include <vector>

namespace fastdb::payload::layout {

struct DescriptorFact final {
    build::NodeIndex node_index;
    std::uint32_t runtime_type_id;
    std::uint64_t first;
    std::uint64_t count;
};

struct ListAggregate final {
    std::uint32_t owner_runtime_type_id;
    std::uint32_t item_runtime_type_id;
    std::uint32_t validity_region_index;
    std::uint32_t items_region_index;
    std::vector<build::NodeIndex> item_nodes;
};

}  // namespace fastdb::payload::layout
