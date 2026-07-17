#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonDocument.hpp"
#include "payload/spec/Manifest.hpp"
#include "payload/spec/Model.hpp"
#include "payload/spec/Parse.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace fastdb::payload::spec {

struct CompileLimits final {
    json::JsonParseLimits json;
    SourceParseLimits source;
};

class CompiledSpec final {
public:
    static error::Result<CompiledSpec> compile(
        std::string_view source,
        CompileLimits limits = {});

    CompiledSpec(const CompiledSpec&) = default;
    CompiledSpec& operator=(const CompiledSpec&) = default;
    CompiledSpec(CompiledSpec&&) noexcept = default;
    CompiledSpec& operator=(CompiledSpec&&) noexcept = default;
    ~CompiledSpec() = default;

    std::string_view canonical_bytes() const noexcept;
    const std::array<std::uint8_t, 32>& digest() const noexcept;
    std::string_view manifest_bytes() const noexcept;
    Profile profile() const noexcept;
    const SemanticFacts& facts() const noexcept;
    const ManifestCapabilities& capabilities() const noexcept;
    const ResolvedSpec& resolved() const noexcept;

    std::optional<std::uint32_t> entry_index(
        std::string_view id) const noexcept;
    std::optional<std::uint32_t> component_index(
        std::string_view id) const noexcept;
    std::optional<std::uint32_t> component_field_index(
        std::uint32_t component_index,
        std::string_view id) const noexcept;

private:
    struct State;

    explicit CompiledSpec(std::shared_ptr<const State> state) noexcept;

    std::shared_ptr<const State> state_;
};

}  // namespace fastdb::payload::spec
