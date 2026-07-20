#pragma once

#include "payload/error/Result.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Open.hpp"

#include <cstdint>

namespace fastdb::payload::view {

error::Result<PayloadIndex> open_graph(
    const spec::CompiledSpec& spec,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    OpenOptions limits = default_open_options());

}  // namespace fastdb::payload::view
