#pragma once

#include "payload/build/ByteSink.hpp"
#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/layout/GraphLayout.hpp"

namespace fastdb::payload::build {

error::Result<void> encode_graph(const layout::GraphLayout& layout,
                                 const LogicalPayload& values,
                                 ByteSink& sink);

}  // namespace fastdb::payload::build
