#pragma once

#include "payload/backing/Backing.hpp"

namespace fastdb::payload::backing {

const Callbacks& heap_callbacks() noexcept;

}  // namespace fastdb::payload::backing
