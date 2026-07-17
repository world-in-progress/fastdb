#include "TestSupport.hpp"

#include "payload/identity/Sha256.hpp"
#include "payload/json/Jcs.hpp"
#include "payload/json/JsonDocument.hpp"
#include "payload/spec/SchemaRepository.hpp"

#include <fastdb_payload.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <variant>

namespace allocation_guard {

thread_local bool enabled = false;
thread_local std::size_t remaining = 0;

void* allocate(std::size_t size) {
    if (enabled) {
        if (size > remaining) {
            throw std::bad_alloc();
        }
        remaining -= size;
    }
    void* const allocation = std::malloc(size);
    if (allocation == nullptr) {
        throw std::bad_alloc();
    }
    return allocation;
}

class Budget final {
public:
    explicit Budget(std::size_t bytes) {
        remaining = bytes;
        enabled = true;
    }

    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;

    ~Budget() { enabled = false; }
};

}  // namespace allocation_guard

void* operator new(std::size_t size) {
    return allocation_guard::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_guard::allocate(size);
}

void operator delete(void* allocation) noexcept { std::free(allocation); }

void operator delete[](void* allocation) noexcept { std::free(allocation); }

void operator delete(void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

namespace {

using fastdb::payload::error::Error;
using fastdb::payload::error::Result;
using fastdb::payload::identity::sha256_lower_hex;
using fastdb::payload::json::JsonCursor;
using fastdb::payload::json::JsonDocument;
using fastdb::payload::json::JsonParseLimits;
using fastdb::payload::json::JsonValue;
using fastdb::payload::json::jcs_serialize;
using fastdb::payload::spec::SchemaRepository;

Result<JsonDocument> parse(std::string_view source,
                           JsonParseLimits limits = {}) {
    return JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), limits);
}

bool has_error(const Result<JsonDocument>& result,
               std::uint32_t code,
               std::string_view path,
               std::string_view message,
               std::string_view details) {
    if (result.has_value()) {
        return false;
    }
    const Error& error = result.error();
    return error.code() == code && error.path() == path &&
           error.message() == message && error.details_json() == details;
}

std::string load_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return 10 + value - 'a';
    }
    return -1;
}

bool decode_digest_pin(std::string_view text,
                       std::array<std::uint8_t, 32>& output) {
    if (text.size() != 65U || text.back() != '\n') {
        return false;
    }
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = hex_digit(text[index * 2U]);
        const int low = hex_digit(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

int test_strict_syntax_and_numbers() {
    require(has_error(parse(""), FDB_PAYLOAD_E_INVALID_JSON, "",
                      "Invalid JSON source",
                      R"({"reason":"empty_content"})"));
    require(has_error(parse(R"({"x":)"), FDB_PAYLOAD_E_INVALID_JSON, "",
                      "Invalid JSON source",
                      R"({"reason":"unexpected_end"})"));
    require(has_error(parse("null true"), FDB_PAYLOAD_E_INVALID_JSON, "",
                      "Invalid JSON source",
                      R"({"reason":"unexpected_content"})"));

    std::string invalid_utf8 = "{\"x\":\"";
    invalid_utf8.push_back(static_cast<char>(0x80));
    invalid_utf8 += "\"}";
    require(has_error(parse(invalid_utf8), FDB_PAYLOAD_E_INVALID_JSON, "",
                      "Invalid JSON source",
                      R"({"reason":"invalid_string"})"));
    require(has_error(parse(R"({"x":"\x"})"),
                      FDB_PAYLOAD_E_INVALID_JSON, "", "Invalid JSON source",
                      R"({"reason":"invalid_string"})"));
    require(has_error(parse(R"({"x":"\uD800"})"),
                      FDB_PAYLOAD_E_INVALID_JSON, "", "Invalid JSON source",
                      R"({"reason":"invalid_string"})"));
    require(has_error(parse("1e9999"), FDB_PAYLOAD_E_INVALID_NUMBER, "",
                      "JSON number is outside finite binary64 range",
                      R"({"reason":"number_out_of_range"})"));
    for (const std::string_view symbolic : {
             std::string_view{"NaN"},
             std::string_view{"Infinity"},
             std::string_view{"-Infinity"},
             std::string_view{R"({"value":NaN})"},
         }) {
        require(has_error(parse(symbolic), FDB_PAYLOAD_E_INVALID_NUMBER, "",
                          "JSON number is outside finite binary64 range",
                          R"({"reason":"symbolic_non_finite"})"));
    }
    return EXIT_SUCCESS;
}

int test_duplicate_paths_and_details() {
    require(has_error(
        parse(R"({"schema":1,"schema":2})"), FDB_PAYLOAD_E_DUPLICATE_KEY,
        "/schema", "JSON object member name is duplicated",
        R"({"first_member_index":0,"key":"schema","second_member_index":1})"));
    require(has_error(
        parse(R"({"outer":{"a/b":1,"a/b":2}})"),
        FDB_PAYLOAD_E_DUPLICATE_KEY, "/outer/a~1b",
        "JSON object member name is duplicated",
        R"({"first_member_index":0,"key":"a/b","second_member_index":1})"));
    require(has_error(
        parse(R"({"entries":[{"id":"first","id":"second"}]})"),
        FDB_PAYLOAD_E_DUPLICATE_KEY, "/entries/0/id",
        "JSON object member name is duplicated",
        R"({"first_member_index":0,"key":"id","second_member_index":1})"));
    return EXIT_SUCCESS;
}

int test_resource_limits() {
    JsonParseLimits source_limits;
    source_limits.max_source_bytes = UINT64_C(3);
    require(has_error(
        parse("null", source_limits), FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "",
        "JSON source exceeds configured limit",
        R"({"actual":"4","kind":"source_bytes","limit":"3"})"));

    JsonParseLimits value_limits;
    value_limits.max_json_values = UINT64_C(3);
    require(has_error(
        parse("[null,true,false]", value_limits),
        FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "/2",
        "JSON value count exceeds configured limit",
        R"({"actual":"4","kind":"json_values","limit":"3"})"));

    JsonParseLimits depth_limits;
    depth_limits.max_nesting_depth = UINT32_C(1);
    require(has_error(
        parse("[[]]", depth_limits), FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "/0",
        "JSON nesting depth exceeds configured limit",
        R"({"actual":"2","kind":"nesting_depth","limit":"1"})"));

    const std::uint8_t source_byte = static_cast<std::uint8_t>('0');
    JsonParseLimits exact_limits;
    exact_limits.max_source_bytes = UINT64_C(9007199254740992);
    const auto exact = JsonDocument::parse(
        &source_byte, UINT64_C(9007199254740993), exact_limits);
    require(has_error(
        exact, FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "",
        "JSON source exceeds configured limit",
        R"({"actual":"9007199254740993","kind":"source_bytes","limit":"9007199254740992"})"));
    return EXIT_SUCCESS;
}

int test_value_limit_audit_has_bounded_allocation() {
    constexpr std::size_t value_count = 20000U;
    std::string source;
    source.reserve(value_count * 5U + 1U);
    source.push_back('[');
    for (std::size_t index = 0; index < value_count; ++index) {
        if (index != 0U) {
            source.push_back(',');
        }
        source += "null";
    }
    source.push_back(']');

    JsonParseLimits limits;
    limits.max_json_values = UINT64_C(3);
    bool threw = false;
    bool returned_exact_limit = false;
    try {
        allocation_guard::Budget budget(64U * 1024U);
        const auto result = parse(source, limits);
        returned_exact_limit = has_error(
            result, FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT, "/2",
            "JSON value count exceeds configured limit",
            R"({"actual":"4","kind":"json_values","limit":"3"})");
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    require(!threw);
    require(returned_exact_limit);
    return EXIT_SUCCESS;
}

int test_duplicate_safe_cursors_and_conversion() {
    auto parsed = parse(R"({"z":1,"a":[true,null,"x"],"n":-0.0})");
    require(parsed.has_value());
    const JsonCursor root = parsed.value().root();
    require(root.is_object());
    require(root.size() == UINT64_C(3));
    auto members = root.members();
    std::string_view member_name;
    JsonCursor member_value;
    require(members.next(member_name, member_value));
    require(member_name == "z");
    require(member_value.is_number());
    require(member_value.number() == 1.0);
    require(members.next(member_name, member_value));
    require(member_name == "a");
    const JsonCursor array = member_value;
    require(array.is_array());
    require(array.size() == UINT64_C(3));
    auto elements = array.elements();
    require(elements.next(member_value));
    require(member_value.is_boolean());
    require(member_value.boolean());
    require(elements.next(member_value));
    require(member_value.is_null());
    require(elements.next(member_value));
    require(member_value.string() == "x");
    require(!elements.next(member_value));
    require(members.next(member_name, member_value));
    require(member_name == "n");
    require(member_value.number() == 0.0);
    require(!members.next(member_name, member_value));

    const JsonValue value = parsed.value().to_json_value();
    const auto canonical = jcs_serialize(value);
    const auto* bytes = std::get_if<std::string>(&canonical);
    require(bytes != nullptr);
    require(*bytes == R"({"a":[true,null,"x"],"n":0,"z":1})");
    return EXIT_SUCCESS;
}

int test_embedded_source_schema_and_digest_pin() {
    const std::string schema_path =
        std::string(FASTDB_PAYLOAD_SCHEMA_DIR) +
        "/fastdb.payload.v1.schema.json";
    const std::string source = load_file(schema_path);
    require(!source.empty());

    auto parsed = parse(source);
    require(parsed.has_value());
    const JsonCursor root = parsed.value().root();
    require(root.is_object());

    bool found_draft = false;
    bool found_id = false;
    bool found_closed_root = false;
    auto members = root.members();
    std::string_view key;
    JsonCursor value;
    while (members.next(key, value)) {
        if (key == "$schema") {
            require(value.string() ==
                    "https://json-schema.org/draft/2020-12/schema");
            found_draft = true;
        } else if (key == "$id") {
            require(value.string() ==
                    "urn:fastdb:schema:fastdb.payload.v1");
            found_id = true;
        } else if (key == "additionalProperties") {
            require(value.is_boolean());
            require(!value.boolean());
            found_closed_root = true;
        }
    }
    require(found_draft);
    require(found_id);
    require(found_closed_root);

    auto artifact = SchemaRepository::payload_source_schema();
    require(artifact.has_value());
    require(!artifact.value().canonical_bytes.empty());
    require(artifact.value().sha256.size() == FDB_PAYLOAD_V1_SHA256_SIZE);

    const std::string pin = load_file(
        std::string(FASTDB_PAYLOAD_SCHEMA_DIR) +
        "/fastdb.payload.v1.schema.sha256");
    std::array<std::uint8_t, 32> expected{};
    require(decode_digest_pin(pin, expected));
    require(expected == artifact.value().sha256);
    require(pin.substr(0U, 64U) ==
            sha256_lower_hex(artifact.value().sha256));
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string_view selected = argv[1];
        if (selected == "syntax") {
            return test_strict_syntax_and_numbers();
        }
        if (selected == "limits") {
            return test_resource_limits();
        }
        if (selected == "allocation") {
            return test_value_limit_audit_has_bounded_allocation();
        }
        return EXIT_FAILURE;
    }
    require(test_strict_syntax_and_numbers() == EXIT_SUCCESS);
    require(test_duplicate_paths_and_details() == EXIT_SUCCESS);
    require(test_resource_limits() == EXIT_SUCCESS);
    require(test_value_limit_audit_has_bounded_allocation() == EXIT_SUCCESS);
    require(test_duplicate_safe_cursors_and_conversion() == EXIT_SUCCESS);
    require(test_embedded_source_schema_and_digest_pin() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
