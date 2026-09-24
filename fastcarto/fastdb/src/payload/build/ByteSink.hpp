#pragma once

#include "payload/error/Result.hpp"

#include <cstdint>

namespace fastdb::payload::build {

class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual error::Result<void> write(std::uint64_t offset,
                                      const std::uint8_t* data,
                                      std::uint64_t size) = 0;
};

}  // namespace fastdb::payload::build
