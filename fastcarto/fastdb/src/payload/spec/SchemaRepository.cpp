#include "payload/spec/SchemaRepository.hpp"

#include "payload/identity/Sha256.hpp"
#include "payload/json/Jcs.hpp"
#include "payload/json/JsonDocument.hpp"

#include "payload/spec/EmbeddedSchemas.inc"

#include <fastdb_payload.h>

#include <string>
#include <utility>
#include <variant>

namespace fastdb::payload::spec {
namespace {

error::Result<SchemaArtifact> schema_artifact(const std::uint8_t* source,
                                              std::uint64_t source_size,
                                              const char* failure_message) {
    auto parsed = json::JsonDocument::parse(source, source_size);
    if (!parsed.has_value()) {
        return error::Result<SchemaArtifact>::failure(
            std::move(parsed).error());
    }

    auto serialized = json::jcs_serialize(parsed.value().to_json_value());
    if (auto* failure = std::get_if<json::JcsFailure>(&serialized)) {
        return error::Result<SchemaArtifact>::failure(
            error::Error::from_jcs_failure(
                *failure, json::JsonPointer{},
                failure_message,
                json::JsonValue::object({})));
    }

    std::string canonical = std::get<std::string>(std::move(serialized));
    const auto digest = identity::sha256(
        reinterpret_cast<const std::uint8_t*>(canonical.data()),
        static_cast<std::uint64_t>(canonical.size()));
    return error::Result<SchemaArtifact>::success(
        SchemaArtifact{std::move(canonical), digest});
}

}  // namespace

error::Result<SchemaArtifact> SchemaRepository::payload_source_schema() {
    return schema_artifact(kFastdbPayloadV1Schema,
                           kFastdbPayloadV1SchemaLength,
                           "Embedded payload source schema cannot be canonicalized");
}

error::Result<SchemaArtifact> SchemaRepository::payload_manifest_schema() {
    return schema_artifact(
        kFastdbPayloadManifestV1Schema,
        kFastdbPayloadManifestV1SchemaLength,
        "Embedded payload manifest schema cannot be canonicalized");
}

}  // namespace fastdb::payload::spec
