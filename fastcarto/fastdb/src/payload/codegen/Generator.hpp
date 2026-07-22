#pragma once

#include "payload/codegen/Artifact.hpp"
#include "payload/spec/CompiledSpec.hpp"

namespace fastdb::payload::codegen {

error::Result<ArtifactSet> generate(
    const spec::CompiledSpec& compiled,
    Target target,
    GenerationLimits limits = {});

}  // namespace fastdb::payload::codegen
