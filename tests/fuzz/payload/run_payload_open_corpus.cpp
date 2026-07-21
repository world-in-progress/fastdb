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
#ifndef FASTDB_PAYLOAD_FUZZ_GRAPH_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_GRAPH_SPEC_PATH must name the graph value spec"
#endif
#ifndef FASTDB_PAYLOAD_FUZZ_GRAPH_CYCLE_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_GRAPH_CYCLE_SPEC_PATH must name the graph cycle spec"
#endif
#ifndef FASTDB_PAYLOAD_FUZZ_GRAPH_NULL_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_GRAPH_NULL_SPEC_PATH must name the graph null spec"
#endif
#ifndef FASTDB_PAYLOAD_FUZZ_GRAPH_DISCONNECTED_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_GRAPH_DISCONNECTED_SPEC_PATH must name the disconnected graph spec"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);

namespace {

enum class SpecProfile : std::uint8_t {
    record,
    graph_values,
    graph_cycle,
    graph_null,
    graph_disconnected,
};

struct ExpectedResult final {
    std::string_view name;
    fdb_payload_v1_status_t status;
    std::string_view path;
    SpecProfile spec_profile;
};

constexpr std::array<ExpectedResult, 16> kExpectedResults{{
    {"valid-empty.bin", UINT32_C(0), "", SpecProfile::record},
    {"valid-fixed.bin", UINT32_C(0), "", SpecProfile::record},
    {"valid-text.bin", UINT32_C(0), "", SpecProfile::record},
    {"valid-list.bin", UINT32_C(0), "", SpecProfile::record},
    {"malformed-magic.bin", FDB_PAYLOAD_E_INVALID_MAGIC,
     "/binary/header/magic", SpecProfile::record},
    {"malformed-length.bin", FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
     "/binary/header/total_length", SpecProfile::record},
    {"malformed-offset.bin", FDB_PAYLOAD_E_MISALIGNED,
     "/binary/regions/0/data_offset", SpecProfile::record},
    {"malformed-validity.bin", FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
     "/binary/regions/1/data", SpecProfile::record},
    {"malformed-text.bin", FDB_PAYLOAD_E_INVALID_TEXT_ENCODING,
     "/entries/records/0/scalars/j_str", SpecProfile::record},
    {"malformed-list.bin", FDB_PAYLOAD_E_OUT_OF_BOUNDS,
     "/entries/records/0/bools", SpecProfile::record},
    {"valid-graph-cycle.bin", UINT32_C(0), "", SpecProfile::graph_cycle},
    {"valid-graph-variable.bin", UINT32_C(0), "",
     SpecProfile::graph_values},
    {"valid-graph-null.bin", UINT32_C(0), "", SpecProfile::graph_null},
    {"malformed-graph-object-region.bin",
     FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/binary/regions/1/stride",
     SpecProfile::graph_disconnected},
    {"malformed-graph-reference.bin", FDB_PAYLOAD_E_INVALID_REFERENCE,
     "/objects/0/0/c_shared", SpecProfile::graph_cycle},
    {"malformed-graph-unreachable.bin",
     FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/objects/Node/1",
     SpecProfile::graph_disconnected},
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

fdb_payload_v1_spec_t* compile_spec(const char* path) {
    const std::string source = read_file(path);
    if (source.empty()) {
        return nullptr;
    }
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const auto status = fdb_payload_v1_spec_compile_json(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), nullptr, &spec, &error);
    const bool succeeded = status == UINT32_C(0) && spec != nullptr &&
                           error == nullptr;
    fdb_payload_v1_error_release(error);
    if (!succeeded) {
        fdb_payload_v1_spec_release(spec);
        return nullptr;
    }
    return spec;
}

struct Specs final {
    Specs()
        : record(compile_spec(FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC_PATH)),
          graph_values(compile_spec(FASTDB_PAYLOAD_FUZZ_GRAPH_SPEC_PATH)),
          graph_cycle(
              compile_spec(FASTDB_PAYLOAD_FUZZ_GRAPH_CYCLE_SPEC_PATH)),
          graph_null(compile_spec(FASTDB_PAYLOAD_FUZZ_GRAPH_NULL_SPEC_PATH)),
          graph_disconnected(compile_spec(
              FASTDB_PAYLOAD_FUZZ_GRAPH_DISCONNECTED_SPEC_PATH)) {}

    ~Specs() {
        fdb_payload_v1_spec_release(graph_disconnected);
        fdb_payload_v1_spec_release(graph_null);
        fdb_payload_v1_spec_release(graph_cycle);
        fdb_payload_v1_spec_release(graph_values);
        fdb_payload_v1_spec_release(record);
    }

    Specs(const Specs&) = delete;
    Specs& operator=(const Specs&) = delete;

    bool complete() const noexcept {
        return record != nullptr && graph_values != nullptr &&
               graph_cycle != nullptr && graph_null != nullptr &&
               graph_disconnected != nullptr;
    }

    const fdb_payload_v1_spec_t* select(SpecProfile profile) const noexcept {
        switch (profile) {
        case SpecProfile::record:
            return record;
        case SpecProfile::graph_values:
            return graph_values;
        case SpecProfile::graph_cycle:
            return graph_cycle;
        case SpecProfile::graph_null:
            return graph_null;
        case SpecProfile::graph_disconnected:
            return graph_disconnected;
        }
        return nullptr;
    }

    fdb_payload_v1_spec_t* record;
    fdb_payload_v1_spec_t* graph_values;
    fdb_payload_v1_spec_t* graph_cycle;
    fdb_payload_v1_spec_t* graph_null;
    fdb_payload_v1_spec_t* graph_disconnected;
};

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
    const Specs compiled_specs;
    if (!compiled_specs.complete()) {
        return EXIT_FAILURE;
    }

    std::array<bool, kExpectedResults.size()> seen{};
    for (int index = 1; index < argc; ++index) {
        std::ifstream input(argv[index], std::ios::binary);
        if (!input) {
            return EXIT_FAILURE;
        }
        const std::string bytes{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
        const auto name = basename(argv[index]);
        const ExpectedResult* expected = find_expected(name);
        if (expected == nullptr) {
            return EXIT_FAILURE;
        }
        const auto expected_index = static_cast<std::size_t>(
            expected - kExpectedResults.data());
        if (seen[expected_index] ||
            !verify_expected_open(*expected,
                                  compiled_specs.select(
                                      expected->spec_profile),
                                  data, bytes.size())) {
            return EXIT_FAILURE;
        }
        seen[expected_index] = true;
        if (LLVMFuzzerTestOneInput(data, bytes.size()) != 0) {
            return EXIT_FAILURE;
        }
    }
    for (const bool was_seen : seen) {
        if (!was_seen) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
