#include "payload/build/ValueArena.hpp"

namespace fastdb::payload::build {

static_assert(sizeof(ValueNode) <= 64U,
              "logical accounting reserves 64 bytes per value node");

}  // namespace fastdb::payload::build
