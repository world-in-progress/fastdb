#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fastdb::test::payload {

struct GoldenSuccess final {
    std::string canonical_hex;
    std::string sha256;
    std::string manifest_hex;
};

struct GoldenError final {
    std::uint32_t status;
    std::string symbol;
    std::string path;
    std::string message;
    std::string details_json;
};

struct GoldenCase final {
    std::string name;
    std::string source_relative_path;
    std::string source;
    std::variant<GoldenSuccess, GoldenError> expected;
};

struct BinaryGoldenSuccess final {
    std::string source_relative_path;
    std::string source;
    std::string scenario;
    std::string binary_relative_path;
    std::string binary_hex;
    std::string sha256_relative_path;
    std::string sha256;
    std::string layout_relative_path;
    std::string layout_receipt;
};

struct BinaryGoldenCase final {
    std::string name;
    BinaryGoldenSuccess success;
};

struct BinaryOpenOptions final {
    std::optional<bool> validate_text_eager;
    std::optional<std::uint64_t> max_total_bytes;
    std::optional<std::uint64_t> max_regions;
    std::optional<std::uint64_t> max_entries;
    std::optional<std::uint64_t> max_components;
    std::optional<std::uint64_t> max_nesting_depth;
    std::optional<std::uint64_t> max_list_elements;
    std::optional<std::uint64_t> max_graph_objects;
    std::optional<std::uint64_t> max_string_bytes;
    std::optional<std::uint64_t> max_validation_work;
};

struct BinaryGoldenInvalid final {
    std::string source_relative_path;
    std::string source;
    std::string binary_relative_path;
    std::string binary_hex;
    BinaryOpenOptions options;
    std::string expectation_relative_path;
    GoldenError expected;
};

struct BinaryOpenGoldenCase final {
    std::string name;
    std::variant<BinaryGoldenSuccess, BinaryGoldenInvalid> expected;
};

std::vector<GoldenCase> load_spec_golden_corpus(const std::string& root);

std::vector<BinaryGoldenCase> load_binary_golden_corpus(
    const std::string& root);

std::vector<BinaryGoldenCase> load_graph_binary_golden_corpus(
    const std::string& root);

std::vector<BinaryOpenGoldenCase> load_binary_open_golden_corpus(
    const std::string& root);

std::vector<BinaryOpenGoldenCase> load_graph_binary_open_golden_corpus(
    const std::string& root);

std::string load_binary_file(const std::string& path);

}  // namespace fastdb::test::payload
