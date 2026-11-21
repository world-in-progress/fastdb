#include "payload/build/ValueArena.hpp"

#include <type_traits>

namespace fastdb::payload::build {

static_assert(sizeof(ValueNode) == 64U,
              "portable logical value nodes have an exact 64-byte charge");
static_assert(std::has_unique_object_representations_v<ValueNode>,
              "portable logical value nodes must not contain padding bytes");

}  // namespace fastdb::payload::build
