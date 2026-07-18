#include "GoldenCorpus.hpp"

#include "payload/json/Jcs.hpp"
#include "payload/json/JsonDocument.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::test::payload {
namespace {

using fastdb::payload::json::JsonCursor;
using fastdb::payload::json::JsonDocument;
using fastdb::payload::json::jcs_serialize;
[[noreturn]] void malformed(std::string_view reason) {
    throw std::runtime_error("Malformed payload golden corpus: " +
                             std::string(reason));
}

JsonCursor required_member(JsonCursor object, std::string_view wanted) {
    if (!object.is_object()) {
        malformed("expected object");
    }
    auto members = object.members();
    std::string_view name;
    JsonCursor value;
    while (members.next(name, value)) {
        if (name == wanted) {
            return value;
        }
    }
    malformed("missing member " + std::string(wanted));
}

std::string required_string(JsonCursor object, std::string_view name) {
    const JsonCursor value = required_member(object, name);
    if (!value.is_string()) {
        malformed("member is not a string: " + std::string(name));
    }
    return std::string(value.string());
}

std::string one_hex_line(const std::string& path,
                         std::size_t exact_size = 0U) {
    std::string text = load_binary_file(path);
    if (text.empty() || text.back() != '\n' ||
        text.find('\n') != text.size() - 1U) {
        malformed("expectation is not exactly one newline-terminated line: " +
                  path);
    }
    text.pop_back();
    if (text.empty() || text.size() % 2U != 0U ||
        (exact_size != 0U && text.size() != exact_size)) {
        malformed("hex expectation has invalid length: " + path);
    }
    for (const char value : text) {
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            malformed("hex expectation is not lowercase hexadecimal: " +
                      path);
        }
    }
    return text;
}

std::uint32_t required_status(JsonCursor object) {
    const JsonCursor value = required_member(object, "status");
    if (!value.is_number()) {
        malformed("error status is not a number");
    }
    const double number = value.number();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(
                     std::numeric_limits<std::uint32_t>::max()) ||
        std::floor(number) != number) {
        malformed("error status is outside uint32");
    }
    return static_cast<std::uint32_t>(number);
}

GoldenError load_error(const std::string& path) {
    const std::string source = load_binary_file(path);
    auto parsed = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()));
    if (!parsed.has_value()) {
        malformed("error expectation is not strict JSON: " + path);
    }
    const JsonCursor root = parsed.value().root();
    if (!root.is_object() || root.size() != UINT64_C(5)) {
        malformed("error expectation must have five exact members: " + path);
    }
    const std::string details_source = required_string(root, "details_json");
    auto details = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(details_source.data()),
        static_cast<std::uint64_t>(details_source.size()));
    if (!details.has_value()) {
        malformed("error details_json is not strict JSON: " + path);
    }
    const auto canonical = jcs_serialize(details.value().to_json_value());
    const auto* canonical_bytes = std::get_if<std::string>(&canonical);
    if (canonical_bytes == nullptr || *canonical_bytes != details_source) {
        malformed("error details_json is not Core JCS: " + path);
    }
    return GoldenError{required_status(root),
                       required_string(root, "symbol"),
                       required_string(root, "path"),
                       required_string(root, "message"),
                       details_source};
}

GoldenSuccess load_success(const std::string& root, JsonCursor value) {
    if (!value.is_object() || value.size() != UINT64_C(3)) {
        malformed("success tuple must have three exact members");
    }
    return GoldenSuccess{
        one_hex_line(root + "/" +
                     required_string(value, "canonical_hex")),
        one_hex_line(root + "/" + required_string(value, "sha256"), 64U),
        one_hex_line(root + "/" +
                     required_string(value, "manifest_hex")),
    };
}

GoldenCase load_case(const std::string& root, JsonCursor value) {
    if (!value.is_object() || value.size() != UINT64_C(3)) {
        malformed("case must have name, source, and one expectation");
    }
    const std::string name = required_string(value, "name");
    const std::string source_path = required_string(value, "source");

    auto members = value.members();
    std::string_view member_name;
    JsonCursor member_value;
    bool has_success = false;
    bool has_error = false;
    GoldenSuccess success{};
    GoldenError error{};
    while (members.next(member_name, member_value)) {
        if (member_name == "success") {
            has_success = true;
            success = load_success(root, member_value);
        } else if (member_name == "error") {
            has_error = true;
            if (!member_value.is_object() ||
                member_value.size() != UINT64_C(1)) {
                malformed("error tuple must name one expectation");
            }
            error = load_error(
                root + "/" +
                required_string(member_value, "expectation"));
        } else if (member_name != "name" && member_name != "source") {
            malformed("unknown case member");
        }
    }
    if (has_success == has_error) {
        malformed("case must choose exactly one success or error");
    }

    GoldenCase result{name, source_path,
                      load_binary_file(root + "/" + source_path),
                      std::move(success)};
    if (has_error) {
        result.expected = std::move(error);
    }
    return result;
}

BinaryGoldenCase load_binary_case(const std::string& root, JsonCursor value) {
    if (!value.is_object() || value.size() != UINT64_C(2)) {
        malformed("binary case must have name and success");
    }
    const std::string name = required_string(value, "name");
    const JsonCursor success = required_member(value, "success");
    if (!success.is_object() || success.size() != UINT64_C(4)) {
        malformed("binary success must have four exact members");
    }
    const std::string source_path = required_string(success, "source");
    const std::string binary_path = required_string(success, "binary_hex");
    const std::string sha256_path = required_string(success, "sha256");
    BinaryGoldenSuccess expected{
        source_path,
        load_binary_file(root + "/" + source_path),
        required_string(success, "scenario"),
        binary_path,
        one_hex_line(root + "/" + binary_path),
        sha256_path,
        one_hex_line(root + "/" + sha256_path, 64U),
    };
    if (expected.source.empty() || expected.scenario.empty()) {
        malformed("binary source and scenario must be non-empty");
    }
    return BinaryGoldenCase{name, std::move(expected)};
}

}  // namespace

std::string load_binary_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open payload golden file: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::vector<GoldenCase> load_spec_golden_corpus(const std::string& root) {
    const std::string index_source = load_binary_file(root + "/index.json");
    auto parsed = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(index_source.data()),
        static_cast<std::uint64_t>(index_source.size()));
    if (!parsed.has_value()) {
        malformed("index is not strict JSON");
    }
    const JsonCursor document = parsed.value().root();
    if (!document.is_object() || document.size() != UINT64_C(2) ||
        required_string(document, "schema") !=
            "fastdb.payload.golden.spec-index.v1") {
        malformed("index root contract");
    }
    const JsonCursor cases_value = required_member(document, "cases");
    if (!cases_value.is_array()) {
        malformed("index cases is not an array");
    }

    std::vector<GoldenCase> cases;
    cases.reserve(static_cast<std::size_t>(cases_value.size()));
    std::set<std::string> names;
    std::set<std::string> sources;
    auto elements = cases_value.elements();
    JsonCursor element;
    while (elements.next(element)) {
        GoldenCase item = load_case(root, element);
        if (!names.insert(item.name).second ||
            !sources.insert(item.source_relative_path).second) {
            malformed("case name or source is listed more than once");
        }
        if (item.source.empty()) {
            malformed("source fixture is empty");
        }
        cases.push_back(std::move(item));
    }
    return cases;
}

std::vector<BinaryGoldenCase> load_binary_golden_corpus(
    const std::string& root) {
    const std::string index_source = load_binary_file(root + "/index.json");
    auto parsed = JsonDocument::parse(
        reinterpret_cast<const std::uint8_t*>(index_source.data()),
        static_cast<std::uint64_t>(index_source.size()));
    if (!parsed.has_value()) {
        malformed("binary index is not strict JSON");
    }
    const JsonCursor document = parsed.value().root();
    if (!document.is_object() || document.size() != UINT64_C(2) ||
        required_string(document, "schema") !=
            "fastdb.payload.golden.binary-index.v1") {
        malformed("binary index root contract");
    }
    const JsonCursor cases_value = required_member(document, "cases");
    if (!cases_value.is_array()) {
        malformed("binary index cases is not an array");
    }

    std::vector<BinaryGoldenCase> cases;
    cases.reserve(static_cast<std::size_t>(cases_value.size()));
    std::set<std::string> names;
    std::set<std::string> sources;
    std::set<std::string> scenarios;
    std::set<std::string> binary_paths;
    std::set<std::string> sha256_paths;
    auto elements = cases_value.elements();
    JsonCursor element;
    while (elements.next(element)) {
        BinaryGoldenCase item = load_binary_case(root, element);
        if (!names.insert(item.name).second ||
            !sources.insert(item.success.source_relative_path).second ||
            !scenarios.insert(item.success.scenario).second ||
            !binary_paths.insert(item.success.binary_relative_path).second ||
            !sha256_paths.insert(item.success.sha256_relative_path).second) {
            malformed("binary case fields are listed more than once");
        }
        cases.push_back(std::move(item));
    }
    return cases;
}

}  // namespace fastdb::test::payload
