#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/spec/Model.hpp"

namespace fastdb::payload::spec {

error::Result<ResolvedSpec> resolve_source(SourceSpec source);

json::JsonValue normalized_source_json(const ResolvedSpec& source);

}  // namespace fastdb::payload::spec
