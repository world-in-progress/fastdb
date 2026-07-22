#pragma once

#include "payload/spec/CompiledSpec.hpp"
#include "payload/spec/RuntimeTopology.hpp"

#include <string>

namespace fastdb::payload::codegen {

std::string render_cpp(const spec::CompiledSpec& compiled,
                       const spec::RuntimeTopology& topology);
std::string render_rust(const spec::CompiledSpec& compiled,
                        const spec::RuntimeTopology& topology);
std::string render_python(const spec::CompiledSpec& compiled,
                          const spec::RuntimeTopology& topology);
std::string render_typescript(const spec::CompiledSpec& compiled,
                              const spec::RuntimeTopology& topology);

}  // namespace fastdb::payload::codegen
