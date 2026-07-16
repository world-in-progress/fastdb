#pragma once

#include "payload/json/JsonValue.hpp"

#include <string>
#include <variant>

namespace fastdb::payload::json {

enum class JcsFailure {
    non_finite_number,
    invalid_utf8,
    duplicate_member,
};

std::variant<std::string, JcsFailure> jcs_serialize(const JsonValue& value);

}  // namespace fastdb::payload::json
