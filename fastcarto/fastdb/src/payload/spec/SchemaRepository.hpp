#pragma once

#include "payload/error/Result.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace fastdb::payload::spec {

struct SchemaArtifact final {
    std::string canonical_bytes;
    std::array<std::uint8_t, 32> sha256;
};

class SchemaRepository final {
public:
    static error::Result<SchemaArtifact> payload_source_schema();
    static error::Result<SchemaArtifact> payload_manifest_schema();
};

}  // namespace fastdb::payload::spec
