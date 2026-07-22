#pragma once

#include "payload/codegen/Artifact.hpp"

#include <string>
#include <string_view>

namespace fastdb::payload::codegen {

std::string_view target_name(Target target) noexcept;
std::string project_identifier(Target target, std::string_view source_id);
std::string project_type_identifier(Target target,
                                    std::string_view source_id);

}  // namespace fastdb::payload::codegen
