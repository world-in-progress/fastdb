#pragma once

#include "payload/build/ByteSink.hpp"
#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/layout/RecordLayout.hpp"

#include <cstdint>

namespace fastdb::payload::build {

error::Result<void> encode_record(const layout::RecordLayout& layout,
                                  const LogicalPayload& values,
                                  ByteSink& sink);

}  // namespace fastdb::payload::build
