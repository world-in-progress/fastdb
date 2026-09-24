#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/spec/Model.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace fastdb::payload::spec {

struct ManifestCapabilities final {
    std::uint64_t operation_flags;
    std::uint64_t codegen_target_flags;
    std::uint32_t direct_build_status;
    std::string direct_build_reason;
};

struct ManifestArtifact final {
    std::string canonical_bytes;
    ManifestCapabilities capabilities;
};

error::Result<ManifestArtifact> build_manifest(
    const ResolvedSpec& resolved,
    const std::array<std::uint8_t, 32>& payload_digest);

bool manifest_value_conforms(const json::JsonValue& manifest);

}  // namespace fastdb::payload::spec
