#pragma once

#include <cstdint>
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
};

struct BinaryGoldenCase final {
    std::string name;
    BinaryGoldenSuccess success;
};

std::vector<GoldenCase> load_spec_golden_corpus(const std::string& root);

std::vector<BinaryGoldenCase> load_binary_golden_corpus(
    const std::string& root);

std::string load_binary_file(const std::string& path);

}  // namespace fastdb::test::payload
