#include "TestSupport.hpp"

#include "payload/identity/Sha256.hpp"
#include "payload/json/Jcs.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

using fastdb::payload::identity::sha256;
using fastdb::payload::identity::sha256_lower_hex;
using fastdb::payload::json::JcsFailure;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::json::JsonValue;
using fastdb::payload::json::jcs_serialize;

int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return 10 + value - 'a';
    }
    if (value >= 'A' && value <= 'F') {
        return 10 + value - 'A';
    }
    return -1;
}

std::string load_canonical_fixture(const char* filename) {
    const std::string path =
        std::string(FASTDB_PAYLOAD_JCS_FIXTURE_DIR) + '/' + filename;
    std::ifstream input(path);
    std::string decoded;
    int high_nibble = -1;
    char current = '\0';
    while (input.get(current)) {
        if (std::isspace(static_cast<unsigned char>(current)) != 0) {
            continue;
        }
        const int nibble = hex_digit(current);
        if (nibble < 0) {
            return {};
        }
        if (high_nibble < 0) {
            high_nibble = nibble;
        } else {
            decoded.push_back(
                static_cast<char>((high_nibble << 4) | nibble));
            high_nibble = -1;
        }
    }
    if (!input.eof() || high_nibble >= 0) {
        return {};
    }
    return decoded;
}

bool serializes_to(const JsonValue& value, const std::string& expected) {
    const auto result = jcs_serialize(value);
    const auto* serialized = std::get_if<std::string>(&result);
    return serialized != nullptr && *serialized == expected;
}

bool fails_with(const JsonValue& value, JcsFailure expected) {
    const auto result = jcs_serialize(value);
    const auto* failure = std::get_if<JcsFailure>(&result);
    return failure != nullptr && *failure == expected;
}

template <typename Difference = std::ptrdiff_t>
bool rejects_sha256_iterator_overflow() {
    if constexpr (sizeof(Difference) <= sizeof(std::uint64_t)) {
        const std::uint8_t byte = 0;
        const std::uint64_t too_large =
            static_cast<std::uint64_t>(std::numeric_limits<Difference>::max()) +
            UINT64_C(1);
        try {
            static_cast<void>(sha256(&byte, too_large));
        } catch (const std::length_error&) {
            return true;
        }
        return false;
    }
    return true;
}

double double_from_bits(std::uint64_t bits) {
    double value = 0.0;
    static_assert(sizeof(value) == sizeof(bits), "binary64 is required");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

JsonValue rfc8785_values() {
    JsonValue::Array numbers{
        JsonValue{333333333.33333329},
        JsonValue{1e30},
        JsonValue{4.50},
        JsonValue{2e-3},
        JsonValue{1e-27},
    };

    std::string text = "\xE2\x82\xAC$";
    text.push_back('\x0F');
    text += "\nA'B\"\\\\\"/";

    JsonValue::Array literals{
        JsonValue{nullptr},
        JsonValue{true},
        JsonValue{false},
    };

    return JsonValue::object({
        JsonValue::Member{"numbers", JsonValue::array(std::move(numbers))},
        JsonValue::Member{"string", JsonValue{std::move(text)}},
        JsonValue::Member{"literals", JsonValue::array(std::move(literals))},
    });
}

JsonValue rfc8785_numbers() {
    JsonValue::Array values{
        JsonValue{double_from_bits(UINT64_C(0x0000000000000000))},
        JsonValue{double_from_bits(UINT64_C(0x8000000000000000))},
        JsonValue{1e30},
        JsonValue{1e-7},
        JsonValue{double_from_bits(UINT64_C(0x0000000000000001))},
        JsonValue{double_from_bits(UINT64_C(0x8000000000000001))},
        JsonValue{double_from_bits(UINT64_C(0x7fefffffffffffff))},
        JsonValue{double_from_bits(UINT64_C(0xffefffffffffffff))},
        JsonValue{double_from_bits(UINT64_C(0x4340000000000000))},
        JsonValue{double_from_bits(UINT64_C(0xc340000000000000))},
        JsonValue{double_from_bits(UINT64_C(0x4430000000000000))},
        JsonValue{double_from_bits(UINT64_C(0x44b52d02c7e14af5))},
        JsonValue{double_from_bits(UINT64_C(0x44b52d02c7e14af6))},
        JsonValue{double_from_bits(UINT64_C(0x44b52d02c7e14af7))},
        JsonValue{double_from_bits(UINT64_C(0x444b1ae4d6e2ef4e))},
        JsonValue{double_from_bits(UINT64_C(0x444b1ae4d6e2ef4f))},
        JsonValue{double_from_bits(UINT64_C(0x444b1ae4d6e2ef50))},
        JsonValue{double_from_bits(UINT64_C(0x3eb0c6f7a0b5ed8c))},
        JsonValue{double_from_bits(UINT64_C(0x3eb0c6f7a0b5ed8d))},
        JsonValue{double_from_bits(UINT64_C(0x41b3de4355555553))},
        JsonValue{double_from_bits(UINT64_C(0x41b3de4355555554))},
        JsonValue{double_from_bits(UINT64_C(0x41b3de4355555555))},
        JsonValue{double_from_bits(UINT64_C(0x41b3de4355555556))},
        JsonValue{double_from_bits(UINT64_C(0x41b3de4355555557))},
        JsonValue{double_from_bits(UINT64_C(0xbecbf647612f3696))},
        JsonValue{double_from_bits(UINT64_C(0x43143ff3c1cb0959))},
    };
    return JsonValue::array(std::move(values));
}

int test_json_pointer() {
    const JsonPointer root;
    require(root.value().empty());
    require(root.append("a/b").value() == "/a~1b");
    require(root.append("m~n").value() == "/m~0n");
    require(root.append("items").append(UINT64_C(12)).value() ==
            "/items/12");
    return EXIT_SUCCESS;
}

int test_literals_strings_and_arrays() {
    require(serializes_to(JsonValue{nullptr}, "null"));
    require(serializes_to(JsonValue{true}, "true"));
    require(serializes_to(JsonValue{false}, "false"));

    std::string escaped;
    escaped.push_back('\b');
    escaped.push_back('\t');
    escaped.push_back('\n');
    escaped.push_back('\f');
    escaped.push_back('\r');
    escaped.push_back('\x01');
    escaped += "\"\\/";
    require(serializes_to(JsonValue{escaped},
                          "\"\\b\\t\\n\\f\\r\\u0001\\\"\\\\/\""));
    require(serializes_to(JsonValue{"</script>"}, "\"</script>\""));

    std::string controls;
    controls.push_back('\x00');
    controls.push_back('\x01');
    controls.push_back('\x1F');
    require(serializes_to(JsonValue{controls},
                          "\"\\u0000\\u0001\\u001f\""));

    const std::string precomposed = "\xC3\xA9";
    const std::string decomposed = "e\xCC\x81";
    const JsonValue text_order = JsonValue::array({
        JsonValue{precomposed},
        JsonValue{decomposed},
    });
    require(serializes_to(text_order,
                          std::string("[\"") + precomposed + "\",\"" +
                              decomposed + "\"]"));

    require(serializes_to(
        JsonValue::array({JsonValue{3.0}, JsonValue{1.0}, JsonValue{2.0}}),
        "[3,1,2]"));
    return EXIT_SUCCESS;
}

int test_utf16_object_order_and_duplicates() {
    const std::string euro = "\xE2\x82\xAC";
    const std::string hebrew_dalet_with_dagesh = "\xEF\xAC\xB3";
    const std::string grinning_face = "\xF0\x9F\x98\x80";
    const std::string control_u0080 = "\xC2\x80";
    const std::string o_with_diaeresis = "\xC3\xB6";

    const JsonValue official_sort_vector = JsonValue::object({
        JsonValue::Member{euro, JsonValue{"Euro Sign"}},
        JsonValue::Member{"\r", JsonValue{"Carriage Return"}},
        JsonValue::Member{hebrew_dalet_with_dagesh,
                          JsonValue{"Hebrew Letter Dalet With Dagesh"}},
        JsonValue::Member{"1", JsonValue{"One"}},
        JsonValue::Member{grinning_face,
                          JsonValue{"Emoji: Grinning Face"}},
        JsonValue::Member{control_u0080, JsonValue{"Control"}},
        JsonValue::Member{o_with_diaeresis,
                          JsonValue{"Latin Small Letter O With Diaeresis"}},
    });

    const std::string expected =
        std::string("{\"\\r\":\"Carriage Return\",\"1\":\"One\",\"") +
        control_u0080 + "\":\"Control\",\"" + o_with_diaeresis +
        "\":\"Latin Small Letter O With Diaeresis\",\"" + euro +
        "\":\"Euro Sign\",\"" + grinning_face +
        "\":\"Emoji: Grinning Face\",\"" + hebrew_dalet_with_dagesh +
        "\":\"Hebrew Letter Dalet With Dagesh\"}";
    require(serializes_to(official_sort_vector, expected));

    require(serializes_to(
        JsonValue::object({
            JsonValue::Member{"ab", JsonValue{3.0}},
            JsonValue::Member{"aa", JsonValue{2.0}},
            JsonValue::Member{"a", JsonValue{1.0}},
            JsonValue::Member{"", JsonValue{0.0}},
        }),
        "{\"\":0,\"a\":1,\"aa\":2,\"ab\":3}"));

    require(fails_with(
        JsonValue::object({
            JsonValue::Member{"x", JsonValue{1.0}},
            JsonValue::Member{"other", JsonValue{2.0}},
            JsonValue::Member{"x", JsonValue{3.0}},
        }),
        JcsFailure::duplicate_member));
    return EXIT_SUCCESS;
}

int test_strict_utf8() {
    const std::array<std::string, 5> invalid_values{
        std::string("\x80", 1),
        std::string("\xC0\xAF", 2),
        std::string("\xE2\x82", 2),
        std::string("\xED\xA0\x80", 3),
        std::string("\xF4\x90\x80\x80", 4),
    };
    for (const auto& invalid : invalid_values) {
        require(fails_with(JsonValue{invalid}, JcsFailure::invalid_utf8));
    }

    require(fails_with(
        JsonValue::object({
            JsonValue::Member{std::string("\xED\xA0\x80", 3),
                              JsonValue{nullptr}},
        }),
        JcsFailure::invalid_utf8));
    return EXIT_SUCCESS;
}

int test_rfc_fixtures_and_number_failures() {
    require(serializes_to(
        rfc8785_values(),
        load_canonical_fixture("rfc8785-values.canonical.hex")));
    require(serializes_to(
        rfc8785_numbers(),
        load_canonical_fixture("rfc8785-numbers.canonical.hex")));

    require(fails_with(JsonValue{std::numeric_limits<double>::quiet_NaN()},
                       JcsFailure::non_finite_number));
    require(fails_with(JsonValue{std::numeric_limits<double>::infinity()},
                       JcsFailure::non_finite_number));
    require(fails_with(JsonValue{-std::numeric_limits<double>::infinity()},
                       JcsFailure::non_finite_number));
    return EXIT_SUCCESS;
}

int test_sha256() {
    const auto empty_digest = sha256(nullptr, UINT64_C(0));
    require(sha256_lower_hex(empty_digest) ==
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855");

    const std::string abc = "abc";
    const auto abc_digest = sha256(
        reinterpret_cast<const std::uint8_t*>(abc.data()),
        static_cast<std::uint64_t>(abc.size()));
    require(sha256_lower_hex(abc_digest) ==
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad");

    bool rejected_null = false;
    try {
        static_cast<void>(sha256(nullptr, UINT64_C(1)));
    } catch (const std::invalid_argument&) {
        rejected_null = true;
    }
    require(rejected_null);
    require(rejects_sha256_iterator_overflow());
    return EXIT_SUCCESS;
}

int test_typed_null_string_rejected() {
    const char* typed_null = nullptr;
    bool rejected = false;
    try {
        const JsonValue invalid{typed_null};
        static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected);
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    require(test_json_pointer() == EXIT_SUCCESS);
    require(test_literals_strings_and_arrays() == EXIT_SUCCESS);
    require(test_utf16_object_order_and_duplicates() == EXIT_SUCCESS);
    require(test_strict_utf8() == EXIT_SUCCESS);
    require(test_rfc_fixtures_and_number_failures() == EXIT_SUCCESS);
    require(test_sha256() == EXIT_SUCCESS);
    require(test_typed_null_string_rejected() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
