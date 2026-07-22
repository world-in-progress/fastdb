#pragma once

#include "payload/codegen/Artifact.hpp"
#include "payload/spec/CompiledSpec.hpp"

#include <fastdb_payload.h>

#include <cstdint>
#include <string_view>

namespace fastdb::payload::codegen {

inline constexpr std::string_view generator_version =
    "fastdb.payload.codegen.v1";
inline constexpr std::uint32_t generator_core_abi_version =
    FDB_PAYLOAD_V1_ABI_VERSION;

error::Result<ArtifactSet> generate(
    const spec::CompiledSpec& compiled,
    Target target,
    GenerationLimits limits = {});

}  // namespace fastdb::payload::codegen
