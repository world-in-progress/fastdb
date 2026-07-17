#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonDocument.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/spec/Model.hpp"

#include <cstdint>

namespace fastdb::payload::spec {

struct SourceParseLimits final {
    std::uint32_t max_entries{UINT32_C(65536)};
    std::uint32_t max_components{UINT32_C(65536)};
    std::uint32_t max_fields_per_component{UINT32_C(65536)};
    std::uint64_t max_total_fields{UINT64_C(1000000)};
};

error::Result<SourceSpec> parse_and_normalize_source(
    const json::JsonDocument& document,
    SourceParseLimits limits = {});

json::JsonValue normalized_source_json(const SourceSpec& source);

}  // namespace fastdb::payload::spec
