#include "payload/codegen/Generator.hpp"

#include "payload/codegen/Identifier.hpp"
#include "payload/codegen/Renderer.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/spec/RuntimeTopology.hpp"

#include <fastdb_payload.h>

#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace fastdb::payload::codegen {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

Error generator_failure(const char* reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_GENERATOR_FAILED,
        JsonPointer{}.append("codegen"),
        "Portable payload source generation failed",
        JsonValue::object(
            {JsonValue::Member{"reason", JsonValue{reason}}}));
}

std::string suffix(Target target) {
    switch (target) {
    case Target::cpp:
        return ".hpp";
    case Target::rust:
        return ".rs";
    case Target::python:
        return ".py";
    case Target::typescript:
        return ".ts";
    }
    return {};
}

std::string render(const spec::CompiledSpec& compiled,
                   const spec::RuntimeTopology& topology,
                   Target target) {
    switch (target) {
    case Target::cpp:
        return render_cpp(compiled, topology);
    case Target::rust:
        return render_rust(compiled, topology);
    case Target::python:
        return render_python(compiled, topology);
    case Target::typescript:
        return render_typescript(compiled, topology);
    }
    return {};
}

}  // namespace

Result<ArtifactSet> generate(const spec::CompiledSpec& compiled,
                             Target target,
                             GenerationLimits limits) {
    try {
        auto valid_target = validate_target(target);
        if (!valid_target.has_value()) {
            return Result<ArtifactSet>::failure(
                std::move(valid_target).error());
        }

        auto topology =
            spec::derive_runtime_topology(compiled.resolved());
        if (!topology.has_value()) {
            return Result<ArtifactSet>::failure(
                generator_failure("runtime_topology_failed"));
        }
        if (topology.value().identity_components.size() !=
            compiled.resolved().components().size()) {
            return Result<ArtifactSet>::failure(
                generator_failure("runtime_topology_failed"));
        }
        const std::string digest =
            identity::sha256_lower_hex(compiled.digest());
        std::string relative_path = "fastdb_payload_";
        relative_path += digest;
        relative_path += suffix(target);

        std::vector<ArtifactDraft> drafts;
        drafts.push_back(ArtifactDraft{
            std::move(relative_path), ArtifactKind::source,
            render(compiled, topology.value(), target)});
        return make_artifact_set(target, std::move(drafts), limits);
    } catch (const std::bad_alloc&) {
        return Result<ArtifactSet>::failure(
            generator_failure("allocation_failed"));
    } catch (const std::exception&) {
        return Result<ArtifactSet>::failure(
            generator_failure("renderer_exception"));
    } catch (...) {
        return Result<ArtifactSet>::failure(
            generator_failure("unknown_exception"));
    }
}

}  // namespace fastdb::payload::codegen
