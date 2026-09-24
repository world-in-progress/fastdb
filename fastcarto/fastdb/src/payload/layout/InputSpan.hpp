#pragma once

#include "payload/spec/Model.hpp"

#include <cstdint>

namespace fastdb::payload::layout {

std::uint64_t max_addressable_input_span_bytes() noexcept;
bool input_span_is_addressable(std::uint64_t byte_length) noexcept;

std::uint64_t load_native_fixed_scalar_bits(
    spec::TypeKind kind, const std::uint8_t* source) noexcept;

}  // namespace fastdb::payload::layout
