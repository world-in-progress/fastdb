#include "payload/codegen/Artifact.hpp"

#include "payload/identity/Sha256.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/TextEncoding.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastdb::payload::codegen {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

bool known_target(Target target) noexcept {
    switch (target) {
    case Target::cpp:
    case Target::rust:
    case Target::python:
    case Target::typescript:
        return true;
    }
    return false;
}

bool known_kind(ArtifactKind kind) noexcept {
    return kind == ArtifactKind::source;
}

Error target_error() {
    return Error::from_details(
        FDB_PAYLOAD_E_UNSUPPORTED_TARGET,
        JsonPointer{}.append("codegen").append("target"),
        "Portable payload codegen target is unsupported",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"unknown_target"}}}));
}

Error path_error(std::uint64_t index, const char* reason) {
    return Error::from_details(
        FDB_PAYLOAD_E_INVALID_ARTIFACT_PATH,
        JsonPointer{}
            .append("codegen")
            .append("artifacts")
            .append(index)
            .append("relative_path"),
        "Generated artifact path is invalid",
        JsonValue::object(
            {JsonValue::Member{"reason", JsonValue{reason}}}));
}

Error kind_error(std::uint64_t index) {
    return Error::from_details(
        FDB_PAYLOAD_E_GENERATOR_FAILED,
        JsonPointer{}
            .append("codegen")
            .append("artifacts")
            .append(index)
            .append("kind"),
        "Generated artifact kind is unsupported",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"unknown_artifact_kind"}}}));
}

Error limit_error(const char* field,
                  const char* reason,
                  std::uint64_t actual,
                  std::uint64_t limit) {
    return Error::from_details(
        FDB_PAYLOAD_E_GENERATOR_FAILED,
        JsonPointer{}.append("codegen").append("limits").append(field),
        "Portable payload codegen output limit was exceeded",
        JsonValue::object({
            JsonValue::Member{"actual",
                              JsonValue{std::to_string(actual)}},
            JsonValue::Member{"limit",
                              JsonValue{std::to_string(limit)}},
            JsonValue::Member{"reason", JsonValue{reason}},
        }));
}

bool unsigned_utf8_less(std::string_view left, std::string_view right) {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(),
        [](char left_byte, char right_byte) {
            return static_cast<unsigned char>(left_byte) <
                   static_cast<unsigned char>(right_byte);
        });
}

bool ascii_letter(unsigned char byte) noexcept {
    return (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z')) ||
           (byte >= static_cast<unsigned char>('a') &&
            byte <= static_cast<unsigned char>('z'));
}

const char* invalid_path_reason(std::string_view path) {
    if (path.empty()) {
        return "empty_path";
    }
    if (path.front() == '/') {
        return "absolute_path";
    }
    if (path.find('\\') != std::string_view::npos) {
        return "backslash";
    }
    if (path.find('\0') != std::string_view::npos) {
        return "nul_byte";
    }
    if (path.size() >= 2U &&
        ascii_letter(static_cast<unsigned char>(path[0])) &&
        path[1] == ':') {
        return "drive_prefix";
    }
    const auto utf8 = layout::validate_utf8(
        path, JsonPointer{}.append("codegen").append("artifact_path"));
    if (!utf8.has_value()) {
        return "invalid_utf8";
    }

    std::size_t begin = 0U;
    while (begin <= path.size()) {
        const std::size_t slash = path.find('/', begin);
        const std::size_t end =
            slash == std::string_view::npos ? path.size() : slash;
        const std::string_view segment = path.substr(begin, end - begin);
        if (segment.empty()) {
            return "empty_segment";
        }
        if (segment == ".") {
            return "dot_segment";
        }
        if (segment == "..") {
            return "parent_segment";
        }
        if (slash == std::string_view::npos) {
            break;
        }
        begin = slash + 1U;
    }
    return nullptr;
}

std::uint64_t checked_size(std::size_t size) {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    return static_cast<std::uint64_t>(size);
}

}  // namespace

Artifact::Artifact(std::string relative_path,
                   ArtifactKind kind,
                   std::string bytes,
                   std::array<std::uint8_t, 32> digest)
    : relative_path_(std::move(relative_path)),
      kind_(kind),
      bytes_(std::move(bytes)),
      sha256_(digest) {}

std::string_view Artifact::relative_path() const noexcept {
    return relative_path_;
}

ArtifactKind Artifact::kind() const noexcept { return kind_; }

std::string_view Artifact::bytes() const noexcept { return bytes_; }

const std::array<std::uint8_t, 32>& Artifact::sha256() const noexcept {
    return sha256_;
}

ArtifactSet::ArtifactSet(Target target, std::vector<Artifact> artifacts)
    : target_(target), artifacts_(std::move(artifacts)) {}

Target ArtifactSet::target() const noexcept { return target_; }

const std::vector<Artifact>& ArtifactSet::artifacts() const noexcept {
    return artifacts_;
}

Result<ArtifactSet> make_artifact_set(Target target,
                                      std::vector<ArtifactDraft> drafts,
                                      GenerationLimits limits) {
    auto valid_target = validate_target(target);
    if (!valid_target.has_value()) {
        return Result<ArtifactSet>::failure(
            std::move(valid_target).error());
    }

    const std::uint64_t artifact_count = checked_size(drafts.size());
    auto valid_count = validate_artifact_count(artifact_count, limits);
    if (!valid_count.has_value()) {
        return Result<ArtifactSet>::failure(std::move(valid_count).error());
    }

    std::uint64_t total_bytes = UINT64_C(0);
    for (std::size_t index = 0U; index < drafts.size(); ++index) {
        const ArtifactDraft& draft = drafts[index];
        if (const char* reason = invalid_path_reason(draft.relative_path);
            reason != nullptr) {
            return Result<ArtifactSet>::failure(
                path_error(checked_size(index), reason));
        }
        if (!known_kind(draft.kind)) {
            return Result<ArtifactSet>::failure(
                kind_error(checked_size(index)));
        }

        const std::uint64_t byte_count = checked_size(draft.bytes.size());
        if (byte_count > std::numeric_limits<std::uint64_t>::max() -
                             total_bytes) {
            return Result<ArtifactSet>::failure(limit_error(
                "max_total_bytes", "total_bytes_overflow",
                std::numeric_limits<std::uint64_t>::max(),
                limits.max_total_bytes));
        }
        total_bytes += byte_count;
        auto valid_bytes = validate_total_bytes(total_bytes, limits);
        if (!valid_bytes.has_value()) {
            return Result<ArtifactSet>::failure(std::move(valid_bytes).error());
        }
    }

    std::sort(drafts.begin(), drafts.end(),
              [](const ArtifactDraft& left, const ArtifactDraft& right) {
                  return unsigned_utf8_less(left.relative_path,
                                            right.relative_path);
              });
    for (std::size_t index = 1U; index < drafts.size(); ++index) {
        if (drafts[index - 1U].relative_path ==
            drafts[index].relative_path) {
            return Result<ArtifactSet>::failure(
                path_error(checked_size(index), "duplicate_artifact_path"));
        }
    }

    std::vector<Artifact> artifacts;
    artifacts.reserve(drafts.size());
    for (ArtifactDraft& draft : drafts) {
        const auto digest = identity::sha256(
            reinterpret_cast<const std::uint8_t*>(draft.bytes.data()),
            checked_size(draft.bytes.size()));
        artifacts.push_back(Artifact(std::move(draft.relative_path),
                                     draft.kind, std::move(draft.bytes),
                                     digest));
    }
    return Result<ArtifactSet>::success(
        ArtifactSet(target, std::move(artifacts)));
}

Result<void> validate_target(Target target) {
    if (!known_target(target)) {
        return Result<void>::failure(target_error());
    }
    return Result<void>::success();
}

Result<void> validate_artifact_count(std::uint64_t artifact_count,
                                     GenerationLimits limits) {
    if (artifact_count > limits.max_artifacts) {
        return Result<void>::failure(
            limit_error("max_artifacts", "artifact_count_exceeded",
                        artifact_count, limits.max_artifacts));
    }
    return Result<void>::success();
}

Result<void> validate_total_bytes(std::uint64_t total_bytes,
                                  GenerationLimits limits) {
    if (total_bytes > limits.max_total_bytes) {
        return Result<void>::failure(
            limit_error("max_total_bytes", "total_bytes_exceeded", total_bytes,
                        limits.max_total_bytes));
    }
    return Result<void>::success();
}

}  // namespace fastdb::payload::codegen
