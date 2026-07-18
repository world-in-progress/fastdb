#include "payload/layout/NormalizedInteger.hpp"

#include "payload/json/JsonValue.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace fastdb::payload::layout {
namespace {

constexpr std::size_t maximum_limb_count = 256U;

struct ArithmeticLimit final {};

class BigUInt final {
public:
    BigUInt() = default;
    explicit BigUInt(std::uint64_t value) {
        if (value != UINT64_C(0)) {
            limbs_.push_back(static_cast<std::uint32_t>(value));
            const std::uint32_t high = static_cast<std::uint32_t>(value >> 32U);
            if (high != UINT32_C(0)) {
                limbs_.push_back(high);
            }
        }
    }

    bool empty() const noexcept { return limbs_.empty(); }

    std::size_t bit_length() const noexcept {
        if (limbs_.empty()) {
            return 0U;
        }
        const std::uint32_t high = limbs_.back();
        std::size_t high_bits = 0U;
        for (std::uint32_t value = high; value != UINT32_C(0); value >>= 1U) {
            ++high_bits;
        }
        return (limbs_.size() - 1U) * 32U + high_bits;
    }

    bool bit(std::size_t index) const noexcept {
        const std::size_t limb = index / 32U;
        return limb < limbs_.size() &&
               ((limbs_[limb] >> (index % 32U)) & UINT32_C(1)) !=
                   UINT32_C(0);
    }

    void set_bit(std::size_t index) {
        const std::size_t required = index / 32U + 1U;
        require_capacity(required);
        if (limbs_.size() < required) {
            limbs_.resize(required, UINT32_C(0));
        }
        limbs_[index / 32U] |= UINT32_C(1) << (index % 32U);
    }

    int compare(const BigUInt& other) const noexcept {
        if (limbs_.size() != other.limbs_.size()) {
            return limbs_.size() < other.limbs_.size() ? -1 : 1;
        }
        for (std::size_t index = limbs_.size(); index > 0U; --index) {
            if (limbs_[index - 1U] != other.limbs_[index - 1U]) {
                return limbs_[index - 1U] < other.limbs_[index - 1U] ? -1
                                                                     : 1;
            }
        }
        return 0;
    }

    void shift_left(std::size_t bits) {
        if (empty() || bits == 0U) {
            return;
        }
        const std::size_t whole = bits / 32U;
        const std::size_t partial = bits % 32U;
        const std::size_t extra = partial == 0U ? 0U : 1U;
        require_capacity(limbs_.size() + whole + extra);
        std::vector<std::uint32_t> shifted(limbs_.size() + whole + extra,
                                           UINT32_C(0));
        std::uint64_t carry = UINT64_C(0);
        for (std::size_t index = 0U; index < limbs_.size(); ++index) {
            const std::uint64_t value =
                (static_cast<std::uint64_t>(limbs_[index]) << partial) |
                carry;
            shifted[index + whole] = static_cast<std::uint32_t>(value);
            carry = value >> 32U;
        }
        if (partial != 0U) {
            shifted[limbs_.size() + whole] =
                static_cast<std::uint32_t>(carry);
        }
        limbs_ = std::move(shifted);
        normalize();
    }

    void shift_left_one() {
        if (empty()) {
            return;
        }
        require_capacity(limbs_.size() + 1U);
        std::uint64_t carry = UINT64_C(0);
        for (std::uint32_t& limb : limbs_) {
            const std::uint64_t value =
                (static_cast<std::uint64_t>(limb) << 1U) | carry;
            limb = static_cast<std::uint32_t>(value);
            carry = value >> 32U;
        }
        if (carry != UINT64_C(0)) {
            limbs_.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void add_one() {
        std::uint64_t carry = UINT64_C(1);
        for (std::uint32_t& limb : limbs_) {
            const std::uint64_t value =
                static_cast<std::uint64_t>(limb) + carry;
            limb = static_cast<std::uint32_t>(value);
            carry = value >> 32U;
            if (carry == UINT64_C(0)) {
                return;
            }
        }
        if (carry != UINT64_C(0)) {
            require_capacity(limbs_.size() + 1U);
            limbs_.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void add(const BigUInt& other) {
        require_capacity(std::max(limbs_.size(), other.limbs_.size()) + 1U);
        if (limbs_.size() < other.limbs_.size()) {
            limbs_.resize(other.limbs_.size(), UINT32_C(0));
        }
        std::uint64_t carry = UINT64_C(0);
        for (std::size_t index = 0U; index < limbs_.size(); ++index) {
            const std::uint64_t right =
                index < other.limbs_.size() ? other.limbs_[index]
                                            : UINT32_C(0);
            const std::uint64_t value =
                static_cast<std::uint64_t>(limbs_[index]) + right + carry;
            limbs_[index] = static_cast<std::uint32_t>(value);
            carry = value >> 32U;
        }
        if (carry != UINT64_C(0)) {
            limbs_.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void subtract(const BigUInt& other) {
        std::uint64_t borrow = UINT64_C(0);
        for (std::size_t index = 0U; index < limbs_.size(); ++index) {
            const std::uint64_t right =
                (index < other.limbs_.size() ? other.limbs_[index]
                                             : UINT32_C(0)) +
                borrow;
            const std::uint64_t left = limbs_[index];
            limbs_[index] = static_cast<std::uint32_t>(left - right);
            borrow = left < right ? UINT64_C(1) : UINT64_C(0);
        }
        normalize();
    }

    void multiply_small(std::uint32_t multiplier) {
        if (empty() || multiplier == UINT32_C(1)) {
            return;
        }
        if (multiplier == UINT32_C(0)) {
            limbs_.clear();
            return;
        }
        require_capacity(limbs_.size() + 1U);
        std::uint64_t carry = UINT64_C(0);
        for (std::uint32_t& limb : limbs_) {
            const std::uint64_t value =
                static_cast<std::uint64_t>(limb) * multiplier + carry;
            limb = static_cast<std::uint32_t>(value);
            carry = value >> 32U;
        }
        if (carry != UINT64_C(0)) {
            limbs_.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    std::uint64_t to_u64() const {
        if (limbs_.size() > 2U) {
            throw ArithmeticLimit{};
        }
        std::uint64_t value = limbs_.empty() ? UINT64_C(0) : limbs_[0];
        if (limbs_.size() == 2U) {
            value |= static_cast<std::uint64_t>(limbs_[1]) << 32U;
        }
        return value;
    }

private:
    static void require_capacity(std::size_t count) {
        if (count > maximum_limb_count) {
            throw ArithmeticLimit{};
        }
    }

    void normalize() noexcept {
        while (!limbs_.empty() && limbs_.back() == UINT32_C(0)) {
            limbs_.pop_back();
        }
    }

    std::vector<std::uint32_t> limbs_;
};

struct Dyadic final {
    bool negative;
    BigUInt magnitude;
    int exponent;
};

struct SignedMagnitude final {
    bool negative;
    BigUInt magnitude;
};

std::uint64_t double_bits(double value) noexcept {
    std::uint64_t bits = UINT64_C(0);
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

Dyadic decompose(std::uint64_t bits) {
    const bool negative = (bits >> 63U) != UINT64_C(0);
    const std::uint32_t exponent_bits =
        static_cast<std::uint32_t>((bits >> 52U) & UINT64_C(0x7ff));
    const std::uint64_t fraction = bits & UINT64_C(0x000fffffffffffff);
    if (exponent_bits == UINT32_C(0)) {
        return Dyadic{fraction == UINT64_C(0) ? false : negative,
                      BigUInt(fraction), -1074};
    }
    return Dyadic{negative,
                  BigUInt(fraction | UINT64_C(0x0010000000000000)),
                  static_cast<int>(exponent_bits) - 1023 - 52};
}

BigUInt shifted(BigUInt value, int bits) {
    if (bits < 0) {
        throw ArithmeticLimit{};
    }
    value.shift_left(static_cast<std::size_t>(bits));
    return value;
}

Dyadic nonnegative_difference(const Dyadic& high, const Dyadic& low) {
    const int exponent = std::min(high.exponent, low.exponent);
    BigUInt high_value = shifted(high.magnitude, high.exponent - exponent);
    BigUInt low_value = shifted(low.magnitude, low.exponent - exponent);
    if (!high.negative && low.negative) {
        high_value.add(low_value);
        return Dyadic{false, std::move(high_value), exponent};
    }
    if (high.negative && low.negative) {
        low_value.subtract(high_value);
        return Dyadic{false, std::move(low_value), exponent};
    }
    high_value.subtract(low_value);
    return Dyadic{false, std::move(high_value), exponent};
}

std::pair<BigUInt, BigUInt> divide(BigUInt numerator,
                                   const BigUInt& denominator) {
    BigUInt quotient;
    BigUInt remainder;
    for (std::size_t bit = numerator.bit_length(); bit > 0U; --bit) {
        remainder.shift_left_one();
        if (numerator.bit(bit - 1U)) {
            remainder.add_one();
        }
        if (remainder.compare(denominator) >= 0) {
            remainder.subtract(denominator);
            quotient.set_bit(bit - 1U);
        }
    }
    return {std::move(quotient), std::move(remainder)};
}

BigUInt round_divide(BigUInt numerator, const BigUInt& denominator) {
    auto divided = divide(std::move(numerator), denominator);
    BigUInt twice_remainder = divided.second;
    twice_remainder.shift_left_one();
    const int comparison = twice_remainder.compare(denominator);
    if (comparison > 0 ||
        (comparison == 0 && divided.first.bit(0U))) {
        divided.first.add_one();
    }
    return std::move(divided.first);
}

int compare_to_power(const BigUInt& numerator,
                     int exponent,
                     const BigUInt& denominator,
                     int power) {
    if (exponent >= power) {
        return shifted(numerator, exponent - power).compare(denominator);
    }
    return numerator.compare(shifted(denominator, power - exponent));
}

std::uint64_t round_rational_to_binary64(bool negative,
                                         const BigUInt& numerator,
                                         int exponent,
                                         std::uint32_t denominator_value) {
    if (numerator.empty()) {
        return UINT64_C(0);
    }
    const BigUInt denominator(denominator_value);
    int power = static_cast<int>(numerator.bit_length()) - 1 + exponent -
                (static_cast<int>(denominator.bit_length()) - 1);
    while (compare_to_power(numerator, exponent, denominator, power) < 0) {
        --power;
    }
    while (compare_to_power(numerator, exponent, denominator, power + 1) >=
           0) {
        ++power;
    }

    std::uint64_t bits = UINT64_C(0);
    if (power >= -1022) {
        const int scale = exponent + 52 - power;
        BigUInt scaled_numerator = numerator;
        BigUInt scaled_denominator = denominator;
        if (scale >= 0) {
            scaled_numerator.shift_left(static_cast<std::size_t>(scale));
        } else {
            scaled_denominator.shift_left(
                static_cast<std::size_t>(-scale));
        }
        std::uint64_t significand =
            round_divide(std::move(scaled_numerator), scaled_denominator)
                .to_u64();
        if (significand == (UINT64_C(1) << 53U)) {
            significand = UINT64_C(1) << 52U;
            ++power;
        }
        const std::uint64_t exponent_field =
            static_cast<std::uint64_t>(power + 1023);
        bits = (exponent_field << 52U) |
               (significand - (UINT64_C(1) << 52U));
    } else {
        const int scale = exponent + 1074;
        BigUInt scaled_numerator = numerator;
        BigUInt scaled_denominator = denominator;
        if (scale >= 0) {
            scaled_numerator.shift_left(static_cast<std::size_t>(scale));
        } else {
            scaled_denominator.shift_left(
                static_cast<std::size_t>(-scale));
        }
        const std::uint64_t significand =
            round_divide(std::move(scaled_numerator), scaled_denominator)
                .to_u64();
        bits = significand == (UINT64_C(1) << 52U)
                   ? UINT64_C(0x0010000000000000)
                   : significand;
    }
    return bits | (negative ? UINT64_C(0x8000000000000000) : UINT64_C(0));
}

SignedMagnitude add_signed(SignedMagnitude left,
                           bool right_negative,
                           BigUInt right) {
    if (left.magnitude.empty()) {
        return SignedMagnitude{right.empty() ? false : right_negative,
                               std::move(right)};
    }
    if (right.empty()) {
        return left;
    }
    if (left.negative == right_negative) {
        left.magnitude.add(right);
        return left;
    }
    const int comparison = left.magnitude.compare(right);
    if (comparison >= 0) {
        left.magnitude.subtract(right);
        if (left.magnitude.empty()) {
            left.negative = false;
        }
        return left;
    }
    right.subtract(left.magnitude);
    return SignedMagnitude{right_negative, std::move(right)};
}

bool finite(std::uint64_t bits) noexcept {
    return (bits & UINT64_C(0x7ff0000000000000)) !=
           UINT64_C(0x7ff0000000000000);
}

std::uint64_t ordered_key(std::uint64_t bits) noexcept {
    if ((bits & UINT64_C(0x7fffffffffffffff)) == UINT64_C(0)) {
        bits = UINT64_C(0);
    }
    if ((bits & UINT64_C(0x8000000000000000)) != UINT64_C(0)) {
        return ~bits;
    }
    return bits | UINT64_C(0x8000000000000000);
}

error::Error range_error(std::uint64_t bits,
                         std::string_view kind,
                         const json::JsonPointer& path) {
    const char* const reason = finite(bits) ? "outside_declared_range"
                                            : "non_finite";
    return error::Error::from_details(
        FDB_PAYLOAD_E_OUT_OF_RANGE, path,
        "Normalized payload value is outside its finite declared range",
        json::JsonValue::object({
            json::JsonValue::Member{"kind",
                                    json::JsonValue{std::string(kind)}},
            json::JsonValue::Member{"reason", json::JsonValue{reason}},
        }));
}

error::Error arithmetic_limit_error(const json::JsonPointer& path) {
    return error::Error::from_details(
        FDB_PAYLOAD_E_RESOURCE_LIMIT, path,
        "Normalized payload arithmetic exceeded its bounded limb storage",
        json::JsonValue::object({json::JsonValue::Member{
            "reason", json::JsonValue{"normalized_limb_limit"}}}));
}

error::Error allocation_error() {
    return error::Error::from_details(
        FDB_PAYLOAD_E_ALLOCATION_FAILED, json::JsonPointer{},
        "Normalized payload arithmetic allocation failed",
        json::JsonValue::object({json::JsonValue::Member{
            "reason", json::JsonValue{"allocation_failed"}}}));
}

}  // namespace

error::Result<void> validate_normalized_input(
    std::uint64_t binary64_bits,
    double minimum,
    double maximum,
    std::string_view kind,
    const json::JsonPointer& path) {
    if (!finite(binary64_bits)) {
        return error::Result<void>::failure(
            range_error(binary64_bits, kind, path));
    }
    const std::uint64_t value_key = ordered_key(binary64_bits);
    if (value_key < ordered_key(double_bits(minimum)) ||
        value_key > ordered_key(double_bits(maximum))) {
        return error::Result<void>::failure(
            range_error(binary64_bits, kind, path));
    }
    return error::Result<void>::success();
}

error::Result<std::uint32_t> quantize_normalized(
    std::uint64_t binary64_bits,
    double minimum,
    double maximum,
    std::uint32_t maximum_code,
    std::string_view kind,
    const json::JsonPointer& path) {
    auto valid = validate_normalized_input(binary64_bits, minimum, maximum,
                                           kind, path);
    if (!valid.has_value()) {
        return error::Result<std::uint32_t>::failure(
            std::move(valid).error());
    }
    try {
        const Dyadic value = decompose(binary64_bits);
        const Dyadic lower = decompose(double_bits(minimum));
        const Dyadic upper = decompose(double_bits(maximum));
        Dyadic numerator = nonnegative_difference(value, lower);
        Dyadic denominator = nonnegative_difference(upper, lower);
        numerator.magnitude.multiply_small(maximum_code);
        if (numerator.exponent > denominator.exponent) {
            numerator.magnitude.shift_left(static_cast<std::size_t>(
                numerator.exponent - denominator.exponent));
        } else if (denominator.exponent > numerator.exponent) {
            denominator.magnitude.shift_left(static_cast<std::size_t>(
                denominator.exponent - numerator.exponent));
        }
        const std::uint64_t code =
            round_divide(std::move(numerator.magnitude),
                         denominator.magnitude)
                .to_u64();
        if (code > maximum_code) {
            return error::Result<std::uint32_t>::failure(
                arithmetic_limit_error(path));
        }
        return error::Result<std::uint32_t>::success(
            static_cast<std::uint32_t>(code));
    } catch (const std::bad_alloc&) {
        return error::Result<std::uint32_t>::failure(allocation_error());
    } catch (const ArithmeticLimit&) {
        return error::Result<std::uint32_t>::failure(
            arithmetic_limit_error(path));
    }
}

error::Result<std::uint64_t> dequantize_normalized(
    std::uint32_t code,
    double minimum,
    double maximum,
    std::uint32_t maximum_code,
    std::string_view,
    const json::JsonPointer& path) {
    if (maximum_code == UINT32_C(0) || code > maximum_code) {
        return error::Result<std::uint64_t>::failure(
            arithmetic_limit_error(path));
    }
    try {
        const Dyadic lower = decompose(double_bits(minimum));
        const Dyadic upper = decompose(double_bits(maximum));
        Dyadic difference = nonnegative_difference(upper, lower);
        const int exponent = std::min(lower.exponent, difference.exponent);
        BigUInt lower_term = shifted(lower.magnitude,
                                     lower.exponent - exponent);
        lower_term.multiply_small(maximum_code);
        BigUInt difference_term = shifted(
            difference.magnitude, difference.exponent - exponent);
        difference_term.multiply_small(code);
        SignedMagnitude numerator{lower.negative, std::move(lower_term)};
        numerator = add_signed(std::move(numerator), false,
                               std::move(difference_term));
        return error::Result<std::uint64_t>::success(
            round_rational_to_binary64(numerator.negative,
                                       numerator.magnitude, exponent,
                                       maximum_code));
    } catch (const std::bad_alloc&) {
        return error::Result<std::uint64_t>::failure(allocation_error());
    } catch (const ArithmeticLimit&) {
        return error::Result<std::uint64_t>::failure(
            arithmetic_limit_error(path));
    }
}

}  // namespace fastdb::payload::layout
