#include "payload/layout/InputSpan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fastdb::payload::layout {
namespace {

template <typename T>
T load_object(const std::uint8_t* source) noexcept {
    T value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

}  // namespace

std::uint64_t max_addressable_input_span_bytes() noexcept {
    std::uint64_t maximum = UINT64_MAX;
    if constexpr (sizeof(std::size_t) <= sizeof(std::uint64_t)) {
        maximum = std::min(
            maximum,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()));
    }
    if constexpr (sizeof(std::ptrdiff_t) <= sizeof(std::uint64_t)) {
        maximum = std::min(
            maximum,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::ptrdiff_t>::max()));
    }
    return maximum;
}

bool input_span_is_addressable(std::uint64_t byte_length) noexcept {
    return byte_length <= max_addressable_input_span_bytes();
}

std::uint64_t load_native_fixed_scalar_bits(
    spec::TypeKind kind, const std::uint8_t* source) noexcept {
    switch (kind) {
    case spec::TypeKind::boolean:
    case spec::TypeKind::u8:
        return load_object<std::uint8_t>(source);
    case spec::TypeKind::u16:
        return load_object<std::uint16_t>(source);
    case spec::TypeKind::u32:
    case spec::TypeKind::f32:
        return load_object<std::uint32_t>(source);
    case spec::TypeKind::i32: {
        const std::int32_t value = load_object<std::int32_t>(source);
        std::uint32_t representation = UINT32_C(0);
        std::memcpy(&representation, &value, sizeof(representation));
        return representation;
    }
    case spec::TypeKind::u8n:
    case spec::TypeKind::u16n:
    case spec::TypeKind::f64:
        return load_object<std::uint64_t>(source);
    case spec::TypeKind::str:
    case spec::TypeKind::wstr:
    case spec::TypeKind::bytes:
    case spec::TypeKind::component:
    case spec::TypeKind::list:
    case spec::TypeKind::ref:
        return UINT64_C(0);
    }
    return UINT64_C(0);
}

}  // namespace fastdb::payload::layout
