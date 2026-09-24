#include "payload/build/ValueArena.hpp"

namespace fastdb::payload::build {

static_assert(sizeof(ValueNode) == 64U,
              "portable logical value nodes have an exact 64-byte charge");

}  // namespace fastdb::payload::build
