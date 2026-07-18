#pragma once

#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/layout/RecordLayout.hpp"

#include <cstdint>

namespace fastdb::payload::build {

class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual error::Result<void> write(std::uint64_t offset,
                                      const std::uint8_t* data,
                                      std::uint64_t size) = 0;
};

error::Result<void> encode_record(const layout::RecordLayout& layout,
                                  const LogicalPayload& values,
                                  ByteSink& sink);

}  // namespace fastdb::payload::build
