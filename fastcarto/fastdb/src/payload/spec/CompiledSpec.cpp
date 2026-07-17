#include "payload/spec/CompiledSpec.hpp"

#include "payload/identity/Sha256.hpp"
#include "payload/json/Jcs.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/spec/Resolve.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::payload::spec {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;

struct IdIndex final {
    std::string id;
    std::uint32_t index;
};

using Lookup = std::vector<IdIndex>;

bool id_index_less(const IdIndex& left, const IdIndex& right) noexcept {
    return left.id < right.id;
}

Lookup entry_lookup(const ResolvedSpec& resolved) {
    Lookup lookup;
    lookup.reserve(resolved.entries().size());
    for (const Entry& entry : resolved.entries()) {
        lookup.push_back(IdIndex{entry.id, entry.index});
    }
    std::sort(lookup.begin(), lookup.end(), id_index_less);
    return lookup;
}

Lookup component_lookup(const ResolvedSpec& resolved) {
    Lookup lookup;
    lookup.reserve(resolved.components().size());
    for (const Component& component : resolved.components()) {
        lookup.push_back(IdIndex{component.id, component.index});
    }
    std::sort(lookup.begin(), lookup.end(), id_index_less);
    return lookup;
}

std::vector<Lookup> field_lookups(const ResolvedSpec& resolved) {
    std::vector<Lookup> lookups;
    lookups.reserve(resolved.components().size());
    for (const Component& component : resolved.components()) {
        Lookup fields;
        fields.reserve(component.fields.size());
        for (const Field& field : component.fields) {
            fields.push_back(IdIndex{field.id, field.index});
        }
        std::sort(fields.begin(), fields.end(), id_index_less);
        lookups.push_back(std::move(fields));
    }
    return lookups;
}

std::optional<std::uint32_t> lookup_index(const Lookup& lookup,
                                          std::string_view id) noexcept {
    const auto found = std::lower_bound(
        lookup.begin(), lookup.end(), id,
        [](const IdIndex& candidate, std::string_view wanted) {
            return candidate.id < wanted;
        });
    if (found == lookup.end() || found->id != id) {
        return std::nullopt;
    }
    return found->index;
}

Error canonicalization_failure(json::JcsFailure failure) {
    return Error::from_jcs_failure(
        failure, JsonPointer{},
        "Normalized payload specification cannot be canonicalized",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"payload_jcs_failure"}}}));
}

}  // namespace

struct CompiledSpec::State final {
    State(std::string canonical_value,
          std::array<std::uint8_t, 32> digest_value,
          ResolvedSpec resolved_value,
          ManifestArtifact manifest_value)
        : canonical(std::move(canonical_value)),
          digest(digest_value),
          resolved(std::move(resolved_value)),
          manifest(std::move(manifest_value.canonical_bytes)),
          capabilities(std::move(manifest_value.capabilities)),
          entries(entry_lookup(resolved)),
          components(component_lookup(resolved)),
          fields(field_lookups(resolved)) {}

    const std::string canonical;
    const std::array<std::uint8_t, 32> digest;
    const ResolvedSpec resolved;
    const std::string manifest;
    const ManifestCapabilities capabilities;
    const Lookup entries;
    const Lookup components;
    const std::vector<Lookup> fields;
};

Result<CompiledSpec> CompiledSpec::compile(std::string_view source,
                                           CompileLimits limits) {
    auto document = json::JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), limits.json);
    if (!document.has_value()) {
        return Result<CompiledSpec>::failure(std::move(document).error());
    }

    auto parsed = parse_and_normalize_source(document.value(), limits.source);
    if (!parsed.has_value()) {
        return Result<CompiledSpec>::failure(std::move(parsed).error());
    }

    auto resolved = resolve_source(std::move(parsed).value());
    if (!resolved.has_value()) {
        return Result<CompiledSpec>::failure(std::move(resolved).error());
    }

    auto serialized = json::jcs_serialize(
        normalized_source_json(resolved.value()));
    if (auto* failure = std::get_if<json::JcsFailure>(&serialized)) {
        return Result<CompiledSpec>::failure(
            canonicalization_failure(*failure));
    }
    std::string canonical = std::get<std::string>(std::move(serialized));
    const auto digest = identity::sha256(
        reinterpret_cast<const std::uint8_t*>(canonical.data()),
        static_cast<std::uint64_t>(canonical.size()));

    auto manifest = build_manifest(resolved.value(), digest);
    if (!manifest.has_value()) {
        return Result<CompiledSpec>::failure(std::move(manifest).error());
    }

    std::shared_ptr<const State> state = std::make_shared<const State>(
        std::move(canonical), digest, std::move(resolved).value(),
        std::move(manifest).value());
    return Result<CompiledSpec>::success(CompiledSpec(std::move(state)));
}

CompiledSpec::CompiledSpec(std::shared_ptr<const State> state) noexcept
    : state_(std::move(state)) {}

std::string_view CompiledSpec::canonical_bytes() const noexcept {
    return state_->canonical;
}

const std::array<std::uint8_t, 32>& CompiledSpec::digest() const noexcept {
    return state_->digest;
}

std::string_view CompiledSpec::manifest_bytes() const noexcept {
    return state_->manifest;
}

Profile CompiledSpec::profile() const noexcept {
    return state_->resolved.profile();
}

const SemanticFacts& CompiledSpec::facts() const noexcept {
    return state_->resolved.facts();
}

const ManifestCapabilities& CompiledSpec::capabilities() const noexcept {
    return state_->capabilities;
}

const ResolvedSpec& CompiledSpec::resolved() const noexcept {
    return state_->resolved;
}

std::optional<std::uint32_t> CompiledSpec::entry_index(
    std::string_view id) const noexcept {
    return lookup_index(state_->entries, id);
}

std::optional<std::uint32_t> CompiledSpec::component_index(
    std::string_view id) const noexcept {
    return lookup_index(state_->components, id);
}

std::optional<std::uint32_t> CompiledSpec::component_field_index(
    std::uint32_t component_index,
    std::string_view id) const noexcept {
    const std::size_t position = static_cast<std::size_t>(component_index);
    if (position >= state_->fields.size()) {
        return std::nullopt;
    }
    return lookup_index(state_->fields[position], id);
}

}  // namespace fastdb::payload::spec
