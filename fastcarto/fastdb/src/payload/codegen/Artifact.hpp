#pragma once

#include "payload/error/Result.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastdb::payload::codegen {

enum class Target : std::uint8_t {
    cpp = UINT8_C(1),
    rust = UINT8_C(2),
    python = UINT8_C(3),
    typescript = UINT8_C(4),
};

enum class ArtifactKind : std::uint32_t {
    source = UINT32_C(1),
};

struct GenerationLimits final {
    std::uint64_t max_artifacts{UINT64_C(16)};
    std::uint64_t max_total_bytes{UINT64_C(16) * UINT64_C(1024) *
                                  UINT64_C(1024)};
};

struct ArtifactDraft final {
    std::string relative_path;
    ArtifactKind kind;
    std::string bytes;
};

class Artifact final {
public:
    std::string_view relative_path() const noexcept;
    ArtifactKind kind() const noexcept;
    std::string_view bytes() const noexcept;
    const std::array<std::uint8_t, 32>& sha256() const noexcept;

private:
    friend error::Result<class ArtifactSet> make_artifact_set(
        Target target,
        std::vector<ArtifactDraft> drafts,
        GenerationLimits limits);

    Artifact(std::string relative_path,
             ArtifactKind kind,
             std::string bytes,
             std::array<std::uint8_t, 32> digest);

    std::string relative_path_;
    ArtifactKind kind_;
    std::string bytes_;
    std::array<std::uint8_t, 32> sha256_;
};

class ArtifactSet final {
public:
    Target target() const noexcept;
    const std::vector<Artifact>& artifacts() const noexcept;

private:
    friend error::Result<ArtifactSet> make_artifact_set(
        Target target,
        std::vector<ArtifactDraft> drafts,
        GenerationLimits limits);

    ArtifactSet(Target target, std::vector<Artifact> artifacts);

    Target target_;
    std::vector<Artifact> artifacts_;
};

error::Result<void> validate_target(Target target);

error::Result<ArtifactSet> make_artifact_set(
    Target target,
    std::vector<ArtifactDraft> drafts,
    GenerationLimits limits = {});

}  // namespace fastdb::payload::codegen
