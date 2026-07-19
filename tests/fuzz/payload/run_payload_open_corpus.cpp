#include <fastdb_payload.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

#ifndef FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC_PATH must name the matching spec"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);

namespace {

struct ExpectedResult final {
    std::string_view name;
    fdb_payload_v1_status_t status;
    std::string_view path;
};

constexpr std::array<ExpectedResult, 10> kExpectedResults{{
    {"valid-empty.bin", UINT32_C(0), ""},
    {"valid-fixed.bin", UINT32_C(0), ""},
    {"valid-text.bin", UINT32_C(0), ""},
    {"valid-list.bin", UINT32_C(0), ""},
    {"malformed-magic.bin", FDB_PAYLOAD_E_INVALID_MAGIC,
     "/binary/header/magic"},
    {"malformed-length.bin", FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
     "/binary/header/total_length"},
    {"malformed-offset.bin", FDB_PAYLOAD_E_MISALIGNED,
     "/binary/regions/0/data_offset"},
    {"malformed-validity.bin", FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
     "/binary/regions/1/data"},
    {"malformed-text.bin", FDB_PAYLOAD_E_INVALID_TEXT_ENCODING,
     "/entries/records/0/scalars/j_str"},
    {"malformed-list.bin", FDB_PAYLOAD_E_OUT_OF_BOUNDS,
     "/entries/records/0/bools"},
}};

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return input ? std::string(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>())
                 : std::string{};
}

std::string_view basename(std::string_view path) {
    const auto separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path
                                                : path.substr(separator + 1U);
}

std::string error_path(const fdb_payload_v1_error_t* error) {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    fdb_payload_v1_error_path(error, &data, &size);
    if ((data == nullptr && size != UINT64_C(0)) ||
        size > static_cast<std::uint64_t>(
                   std::numeric_limits<std::size_t>::max())) {
        return "<invalid error path>";
    }
    if (size == UINT64_C(0)) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data),
                       static_cast<std::size_t>(size));
}

const ExpectedResult* find_expected(std::string_view name) {
    for (const auto& expected : kExpectedResults) {
        if (expected.name == name) {
            return &expected;
        }
    }
    return nullptr;
}

bool verify_expected_open(const ExpectedResult& expected,
                          const fdb_payload_v1_spec_t* spec,
                          const std::uint8_t* data,
                          std::size_t size) {
    fdb_payload_v1_open_options_t options{};
    fdb_payload_v1_open_options_init(&options);
    fdb_payload_v1_payload_t* payload = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const auto status = fdb_payload_v1_payload_open_copy(
        spec, data, static_cast<std::uint64_t>(size), &options, &payload,
        &error);
    const std::string actual_path = error_path(error);
    const std::uint32_t actual_code = fdb_payload_v1_error_code(error);
    const bool success_shape =
        status == UINT32_C(0) ? payload != nullptr && error == nullptr
                              : payload == nullptr && error != nullptr;
    const bool matches = success_shape && status == expected.status &&
                         actual_code == expected.status &&
                         actual_path == expected.path;
    if (!matches) {
        std::cerr << expected.name << ": expected status=" << expected.status
                  << " path=" << expected.path << ", got status=" << status
                  << " code=" << actual_code << " path=" << actual_path
                  << '\n';
    }
    fdb_payload_v1_error_release(error);
    fdb_payload_v1_payload_release(payload);
    return matches;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != static_cast<int>(kExpectedResults.size()) + 1) {
        return EXIT_FAILURE;
    }
    const std::string spec_source =
        read_file(FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC_PATH);
    if (spec_source.empty()) {
        return EXIT_FAILURE;
    }
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* compile_error = nullptr;
    const auto compile_status = fdb_payload_v1_spec_compile_json(
        reinterpret_cast<const std::uint8_t*>(spec_source.data()),
        static_cast<std::uint64_t>(spec_source.size()), nullptr, &spec,
        &compile_error);
    if (compile_status != UINT32_C(0) || spec == nullptr ||
        compile_error != nullptr) {
        fdb_payload_v1_error_release(compile_error);
        fdb_payload_v1_spec_release(spec);
        return EXIT_FAILURE;
    }

    std::array<bool, kExpectedResults.size()> seen{};
    for (int index = 1; index < argc; ++index) {
        std::ifstream input(argv[index], std::ios::binary);
        if (!input) {
            fdb_payload_v1_spec_release(spec);
            return EXIT_FAILURE;
        }
        const std::string bytes{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
        const auto name = basename(argv[index]);
        const ExpectedResult* expected = find_expected(name);
        if (expected == nullptr) {
            fdb_payload_v1_spec_release(spec);
            return EXIT_FAILURE;
        }
        const auto expected_index = static_cast<std::size_t>(
            expected - kExpectedResults.data());
        if (seen[expected_index] ||
            !verify_expected_open(*expected, spec, data, bytes.size())) {
            fdb_payload_v1_spec_release(spec);
            return EXIT_FAILURE;
        }
        seen[expected_index] = true;
        if (LLVMFuzzerTestOneInput(data, bytes.size()) != 0) {
            fdb_payload_v1_spec_release(spec);
            return EXIT_FAILURE;
        }
    }
    fdb_payload_v1_spec_release(spec);
    for (const bool was_seen : seen) {
        if (!was_seen) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
