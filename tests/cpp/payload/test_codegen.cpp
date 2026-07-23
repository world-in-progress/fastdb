#include "TestSupport.hpp"

#include "payload/codegen/Artifact.hpp"
#include "payload/codegen/Generator.hpp"
#include "payload/codegen/Identifier.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/spec/RuntimeTopology.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace allocation_guard {

thread_local bool enabled = false;
thread_local std::size_t allocation_index = 0U;
thread_local std::size_t failure_index =
    std::numeric_limits<std::size_t>::max();

void* allocate(std::size_t size) {
    if (enabled) {
        if (allocation_index == failure_index) {
            enabled = false;
            throw std::bad_alloc();
        }
        ++allocation_index;
    }
    void* const allocation = std::malloc(size == 0U ? 1U : size);
    if (allocation == nullptr) {
        throw std::bad_alloc();
    }
    return allocation;
}

class FailAt final {
public:
    explicit FailAt(std::size_t index) {
        allocation_index = 0U;
        failure_index = index;
        enabled = true;
    }

    FailAt(const FailAt&) = delete;
    FailAt& operator=(const FailAt&) = delete;

    ~FailAt() { enabled = false; }
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

using fastdb::payload::codegen::ArtifactDraft;
using fastdb::payload::codegen::ArtifactKind;
using fastdb::payload::codegen::ArtifactSet;
using fastdb::payload::codegen::GenerationLimits;
using fastdb::payload::codegen::Target;
using fastdb::payload::codegen::generate;
using fastdb::payload::codegen::generator_core_abi_version;
using fastdb::payload::codegen::generator_version;
using fastdb::payload::codegen::make_artifact_set;
using fastdb::payload::codegen::project_identifier;
using fastdb::payload::codegen::project_type_identifier;
using fastdb::payload::codegen::target_name;
using fastdb::payload::codegen::validate_target;
using fastdb::payload::identity::sha256;
using fastdb::payload::spec::CompiledSpec;

constexpr std::string_view kEmptyRecordSource =
    R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]})";

struct TargetExpectation final {
    Target target;
    std::string_view name;
    std::string_view suffix;
    std::string_view golden;
    std::string_view runtime_marker;
    std::string_view identifier_prefix;
    std::string_view graph_identity_marker;
    std::string_view ref_target_marker;
    std::string_view entry_graph_identity_marker;
    std::string_view entry_ref_target_marker;
    std::string_view component_guard_marker;
};

constexpr std::array<TargetExpectation, 4> kTargets{{
    {Target::cpp, "cpp", ".hpp", "empty-record.expected.hpp",
     "<fastdb_payload.hpp>", "fdb_cpp_id_", "graph_identity",
     "ref_target", "at_graph_identity", "at_ref_target",
     "component_index() !="},
    {Target::rust, "rust", ".rs", "empty-record.expected.rs", "fastdb::",
     "fdb_rust_id_", "graph_identity", "ref_target",
     "at_graph_identity", "at_ref_target", "component_index()? !="},
    {Target::python, "python", ".py", "empty-record.expected.py",
     "fastdb4py.payload", "fdb_python_id_", "graph_identity",
     "ref_target", "at_graph_identity", "at_ref_target",
     "component_index() !="},
    {Target::typescript, "typescript", ".ts", "empty-record.expected.ts",
     "fastdb4ts/payload", "fdb_ts_id_", "graphIdentity", "refTarget",
     "atGraphIdentity", "atRefTarget", "componentIndex() !=="},
}};

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

CompiledSpec compile(std::string_view source) {
    auto result = CompiledSpec::compile(source);
    if (!result.has_value()) {
        std::abort();
    }
    return std::move(result).value();
}

int set_codegen_environment(const char* home,
                            const char* temporary,
                            const char* timezone,
                            const char* locale) {
#if defined(_WIN32)
    require(_putenv_s("HOME", home) == 0);
    require(_putenv_s("TMP", temporary) == 0);
    require(_putenv_s("TZ", timezone) == 0);
    require(_putenv_s("LC_ALL", locale) == 0);
#else
    require(setenv("HOME", home, 1) == 0);
    require(setenv("TMPDIR", temporary, 1) == 0);
    require(setenv("TZ", timezone, 1) == 0);
    require(setenv("LC_ALL", locale, 1) == 0);
#endif
    return EXIT_SUCCESS;
}

std::size_t count_substring(std::string_view source,
                            std::string_view needle) {
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while ((offset = source.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

bool contains_forbidden_content(std::string_view source) {
    constexpr std::array<std::string_view, 17> forbidden{{
        "fastdb.schema.v1",
        "columnar.v1",
        "call-db",
        "call_db",
        "CRM",
        "crm",
        "route",
        "relay",
        "transport",
        "lease",
        "policy",
        "Toodle",
        "toodle",
        "GIS",
        "gis",
        "FDBP",
        "binary_header",
    }};
    return std::any_of(forbidden.begin(), forbidden.end(),
                       [source](std::string_view value) {
                           return source.find(value) != std::string_view::npos;
                       });
}

bool has_source_id(const fastdb::payload::spec::ResolvedSpec& resolved,
                   std::string_view id) {
    for (const auto& entry : resolved.entries()) {
        if (entry.id == id) {
            return true;
        }
    }
    for (const auto& component : resolved.components()) {
        if (component.id == id) {
            return true;
        }
        for (const auto& field : component.fields) {
            if (field.id == id) {
                return true;
            }
        }
    }
    return false;
}

int test_identifier_projection_is_total_and_collision_free() {
    require(generator_version == "fastdb.payload.codegen.v1");
    require(generator_core_abi_version == FDB_PAYLOAD_V1_ABI_VERSION);
    require(static_cast<std::uint8_t>(Target::cpp) == UINT8_C(1));
    require(static_cast<std::uint8_t>(Target::rust) == UINT8_C(2));
    require(static_cast<std::uint8_t>(Target::python) == UINT8_C(4));
    require(static_cast<std::uint8_t>(Target::typescript) == UINT8_C(8));

    std::string all_bytes;
    std::string all_hex;
    constexpr char kHex[] = "0123456789abcdef";
    all_bytes.reserve(256U);
    all_hex.reserve(512U);
    for (std::uint32_t value = 0U; value <= UINT32_C(255); ++value) {
        const auto byte = static_cast<unsigned char>(value);
        all_bytes.push_back(static_cast<char>(byte));
        all_hex.push_back(kHex[byte >> 4U]);
        all_hex.push_back(kHex[byte & 0x0fU]);
    }

    for (const auto& expectation : kTargets) {
        const std::string projected =
            project_identifier(expectation.target, all_bytes);
        require(projected ==
                std::string(expectation.identifier_prefix) + all_hex);
        require(project_type_identifier(expectation.target, all_bytes)
                    .find(projected) != std::string::npos);
        require(project_identifier(expectation.target, "class") !=
                project_identifier(expectation.target,
                                   "fdb_cpp_id_636c617373"));
        require(target_name(expectation.target) == expectation.name);
    }
    return EXIT_SUCCESS;
}

int test_artifact_set_validates_paths_order_hashes_and_limits() {
    auto unsupported =
        validate_target(static_cast<Target>(UINT8_C(99)));
    require(!unsupported.has_value());
    require(unsupported.error().code() ==
            FDB_PAYLOAD_E_UNSUPPORTED_TARGET);
    require(unsupported.error().path() == "/codegen/target");
    require(unsupported.error().details_json() ==
            R"({"reason":"unknown_target"})");

    auto multiple = validate_target(static_cast<Target>(UINT8_C(3)));
    require(!multiple.has_value());
    require(multiple.error().code() == FDB_PAYLOAD_E_UNSUPPORTED_TARGET);
    require(multiple.error().path() == "/codegen/target");

    auto empty = validate_target(static_cast<Target>(UINT8_C(0)));
    require(!empty.has_value());
    require(empty.error().code() == FDB_PAYLOAD_E_UNSUPPORTED_TARGET);

    std::vector<ArtifactDraft> drafts;
    drafts.push_back(
        ArtifactDraft{"z/second.hpp", ArtifactKind::source, "second"});
    drafts.push_back(
        ArtifactDraft{"a/first.hpp", ArtifactKind::source, "first"});

    auto made =
        make_artifact_set(Target::cpp, std::move(drafts), GenerationLimits{});
    require(made.has_value());
    const auto& artifacts = made.value().artifacts();
    require(artifacts.size() == 2U);
    require(artifacts[0].relative_path() == "a/first.hpp");
    require(artifacts[1].relative_path() == "z/second.hpp");
    require(artifacts[0].kind() == ArtifactKind::source);
    require(artifacts[0].bytes() == "first");
    require(artifacts[0].sha256() ==
            sha256(reinterpret_cast<const std::uint8_t*>("first"),
                   UINT64_C(5)));

    std::vector<ArtifactDraft> utf8_relative;
    utf8_relative.push_back(ArtifactDraft{
        std::string{"\xc3\xa9:relative.hpp"}, ArtifactKind::source, "x"});
    auto utf8_path = make_artifact_set(
        Target::cpp, std::move(utf8_relative), GenerationLimits{});
    require(utf8_path.has_value());
    require(utf8_path.value().artifacts().front().relative_path() ==
            std::string_view{"\xc3\xa9:relative.hpp"});

    constexpr std::array<std::string_view, 11> invalid_paths{{
        "",
        "/absolute.hpp",
        "./dot.hpp",
        "../parent.hpp",
        "a/../parent.hpp",
        "a/./dot.hpp",
        "a//empty.hpp",
        "a\\windows.hpp",
        "C:/drive.hpp",
        std::string_view{"nul\0suffix", 10U},
        std::string_view{"bad\xc0\x80.hpp", 9U},
    }};
    for (const std::string_view path : invalid_paths) {
        std::vector<ArtifactDraft> invalid;
        invalid.push_back(
            ArtifactDraft{std::string(path), ArtifactKind::source, "x"});
        auto rejected = make_artifact_set(Target::cpp, std::move(invalid),
                                          GenerationLimits{});
        require(!rejected.has_value());
        require(rejected.error().code() ==
                FDB_PAYLOAD_E_INVALID_ARTIFACT_PATH);
        require(rejected.error().path() ==
                "/codegen/artifacts/0/relative_path");
    }

    std::vector<ArtifactDraft> duplicates;
    duplicates.push_back(ArtifactDraft{"same.hpp", ArtifactKind::source, "a"});
    duplicates.push_back(ArtifactDraft{"same.hpp", ArtifactKind::source, "b"});
    auto duplicate = make_artifact_set(Target::cpp, std::move(duplicates),
                                       GenerationLimits{});
    require(!duplicate.has_value());
    require(duplicate.error().code() ==
            FDB_PAYLOAD_E_INVALID_ARTIFACT_PATH);
    require(duplicate.error().details_json() ==
            R"({"reason":"duplicate_artifact_path"})");

    std::vector<ArtifactDraft> unknown_kind;
    unknown_kind.push_back(ArtifactDraft{
        "unknown.hpp", static_cast<ArtifactKind>(UINT32_C(99)), "x"});
    auto unknown = make_artifact_set(Target::cpp, std::move(unknown_kind),
                                     GenerationLimits{});
    require(!unknown.has_value());
    require(unknown.error().code() == FDB_PAYLOAD_E_GENERATOR_FAILED);
    require(unknown.error().path() == "/codegen/artifacts/0/kind");
    require(unknown.error().details_json() ==
            R"({"reason":"unknown_artifact_kind"})");

    GenerationLimits count_limit;
    count_limit.max_artifacts = UINT64_C(1);
    std::vector<ArtifactDraft> too_many;
    too_many.push_back(ArtifactDraft{"a.hpp", ArtifactKind::source, "a"});
    too_many.push_back(ArtifactDraft{"b.hpp", ArtifactKind::source, "b"});
    auto count_rejected = make_artifact_set(
        Target::cpp, std::move(too_many), count_limit);
    require(!count_rejected.has_value());
    require(count_rejected.error().code() == FDB_PAYLOAD_E_GENERATOR_FAILED);
    require(count_rejected.error().path() ==
            "/codegen/limits/max_artifacts");
    require(count_rejected.error().details_json() ==
            R"({"actual":"2","limit":"1","reason":"artifact_count_exceeded"})");

    GenerationLimits byte_limit;
    byte_limit.max_total_bytes = UINT64_C(1);
    std::vector<ArtifactDraft> too_large;
    too_large.push_back(ArtifactDraft{"a.hpp", ArtifactKind::source, "xx"});
    auto bytes_rejected = make_artifact_set(
        Target::cpp, std::move(too_large), byte_limit);
    require(!bytes_rejected.has_value());
    require(bytes_rejected.error().code() == FDB_PAYLOAD_E_GENERATOR_FAILED);
    require(bytes_rejected.error().path() ==
            "/codegen/limits/max_total_bytes");
    require(bytes_rejected.error().details_json() ==
            R"({"actual":"2","limit":"1","reason":"total_bytes_exceeded"})");
    return EXIT_SUCCESS;
}

int test_exact_empty_artifacts_and_repeated_determinism() {
    const CompiledSpec spec = compile(kEmptyRecordSource);
    const std::string digest =
        fastdb::payload::identity::sha256_lower_hex(spec.digest());
    std::string expected_receipt =
        "{\n"
        "  \"schema\": \"fastdb.payload.codegen-golden.v1\",\n"
        "  \"payload_sha256\": \"" +
        digest +
        "\",\n"
        "  \"artifacts\": [\n";

    for (std::size_t target_index = 0U;
         target_index < kTargets.size(); ++target_index) {
        const auto& expectation = kTargets[target_index];
        auto first = generate(spec, expectation.target);
        auto second = generate(spec, expectation.target);
        require(first.has_value());
        require(second.has_value());
        require(first.value().target() == expectation.target);
        require(first.value().artifacts().size() == 1U);
        require(second.value().artifacts().size() == 1U);

        const auto& artifact = first.value().artifacts().front();
        const auto& repeated = second.value().artifacts().front();
        require(artifact.relative_path() ==
                "fastdb_payload_" + digest +
                    std::string(expectation.suffix));
        require(artifact.kind() == ArtifactKind::source);
        require(artifact.relative_path() == repeated.relative_path());
        require(artifact.bytes() == repeated.bytes());
        require(artifact.sha256() == repeated.sha256());
        require(artifact.sha256() ==
                sha256(reinterpret_cast<const std::uint8_t*>(
                           artifact.bytes().data()),
                       static_cast<std::uint64_t>(artifact.bytes().size())));

        const std::string golden =
            read_file(std::string(FASTDB_PAYLOAD_CODEGEN_FIXTURE_DIR) + "/" +
                      std::string(expectation.golden));
        require(!golden.empty());
        require(artifact.bytes() == golden);

        expected_receipt +=
            "    {\n"
            "      \"target\": \"";
        expected_receipt += expectation.name;
        expected_receipt +=
            "\",\n"
            "      \"relative_path\": \"";
        expected_receipt += artifact.relative_path();
        expected_receipt +=
            "\",\n"
            "      \"kind\": \"source\",\n"
            "      \"golden\": \"";
        expected_receipt += expectation.golden;
        expected_receipt +=
            "\",\n"
            "      \"sha256\": \"";
        expected_receipt +=
            fastdb::payload::identity::sha256_lower_hex(
                artifact.sha256());
        expected_receipt += "\"\n    }";
        expected_receipt +=
            target_index + 1U == kTargets.size() ? "\n" : ",\n";
    }
    expected_receipt += "  ]\n}\n";
    require(read_file(std::string(FASTDB_PAYLOAD_CODEGEN_FIXTURE_DIR) +
                      "/index.json") == expected_receipt);
    return EXIT_SUCCESS;
}

int test_rich_specs_emit_only_official_runtime_ergonomics() {
    const std::array<std::string, 4> sources{{
        read_file(std::string(FASTDB_PAYLOAD_SPEC_FIXTURE_DIR) +
                  "/valid/record-all-types.source.json"),
        read_file(std::string(FASTDB_PAYLOAD_BINARY_FIXTURE_DIR) +
                  "/spec/nested-lists.source.json"),
        read_file(std::string(FASTDB_PAYLOAD_BINARY_FIXTURE_DIR) +
                  "/spec/graph-shared-cycle.source.json"),
        R"({"schema":"fastdb.payload.v1","profile":"object_graph.v1","entries":[{"id":"class","cardinality":"one","type":{"kind":"component","id":"type"}},{"id":"fdb_cpp_id_636c617373","cardinality":"many","type":{"kind":"ref","target":"type","nullable":true}}],"components":[{"id":"type","kind":"record","fields":[{"id":"match","type":{"kind":"u32"}},{"id":"interface","type":{"kind":"ref","target":"type","nullable":true}},{"id":"fdb_ts_id_696e74657266616365","type":{"kind":"list","nullable":true,"items":{"kind":"ref","target":"type"}}}]}]})",
    }};
    constexpr std::array<std::string_view, 8> observed_ids{{
        "single",
        "series",
        "AllTypes",
        "boolean_value",
        "class",
        "fdb_cpp_id_636c617373",
        "type",
        "interface",
    }};

    for (const std::string& source : sources) {
        require(!source.empty());
        const CompiledSpec spec = compile(source);
        auto derived = fastdb::payload::spec::derive_runtime_topology(
            spec.resolved());
        require(derived.has_value());
        const bool has_identity = std::any_of(
            derived.value().identity_components.begin(),
            derived.value().identity_components.end(),
            [](std::uint8_t value) { return value != UINT8_C(0); });
        const bool has_direct_ref_entry = std::any_of(
            spec.resolved().entries().begin(),
            spec.resolved().entries().end(), [](const auto& entry) {
                return entry.type.kind ==
                       fastdb::payload::spec::TypeKind::ref;
            });
        const bool has_component_root = std::any_of(
            spec.resolved().entries().begin(),
            spec.resolved().entries().end(), [](const auto& entry) {
                return entry.type.kind ==
                       fastdb::payload::spec::TypeKind::component;
            });
        for (const auto& expectation : kTargets) {
            require(set_codegen_environment(
                        "/codegen-host-a/home", "/codegen-host-a/tmp",
                        "UTC+11", "codegen-locale-a") == EXIT_SUCCESS);
            auto generated = generate(spec, expectation.target);
            require(set_codegen_environment(
                        "/codegen-host-b/home", "/codegen-host-b/tmp",
                        "UTC-09", "codegen-locale-b") == EXIT_SUCCESS);
            auto repeated = generate(spec, expectation.target);
            require(generated.has_value());
            require(repeated.has_value());
            require(generated.value().artifacts().size() == 1U);
            require(generated.value().artifacts().front().bytes() ==
                    repeated.value().artifacts().front().bytes());

            const std::string_view bytes =
                generated.value().artifacts().front().bytes();
            const std::size_t provenance =
                bytes.find("generated-by: fastdb.payload.codegen.v1");
            require(provenance == 2U || provenance == 3U);
            require(bytes.find("payload-sha256:") != std::string_view::npos);
            require(bytes.find("core-abi-version: 1") !=
                    std::string_view::npos);
            require(bytes.find(
                        "generator-version: fastdb.payload.codegen.v1") !=
                    std::string_view::npos);
            require(bytes.find("target: " +
                                   std::string(expectation.name)) !=
                    std::string_view::npos);
            require(bytes.find("/codegen-host-a/") ==
                    std::string_view::npos);
            require(bytes.find("/codegen-host-b/") ==
                    std::string_view::npos);
            require(bytes.find("codegen-locale-") ==
                    std::string_view::npos);
            require(bytes.find("UTC+") == std::string_view::npos);
            require(bytes.find("UTC-") == std::string_view::npos);
            require(bytes.find(expectation.runtime_marker) !=
                    std::string_view::npos);
            if (expectation.target == Target::cpp) {
                require(bytes.find(
                            "fastdb::payload::v1::CompiledSpec") !=
                        std::string_view::npos);
                require(bytes.find(
                            "fastdb::payload::CompiledSpec") ==
                        std::string_view::npos);
            }
            if (has_identity) {
                require(bytes.find(expectation.graph_identity_marker) !=
                        std::string_view::npos);
                require(bytes.find("_builder_declare") !=
                        std::string_view::npos);
            } else {
                require(bytes.find(expectation.graph_identity_marker) ==
                        std::string_view::npos);
                require(bytes.find("_builder_declare") ==
                        std::string_view::npos);
            }
            if (expectation.target == Target::typescript &&
                !has_identity) {
                require(bytes.find("  GraphIdentity,\n") ==
                        std::string_view::npos);
                require(bytes.find("  ObjectHandle,\n") ==
                        std::string_view::npos);
            }
            if (expectation.target == Target::typescript) {
                require(bytes.find(
                            "export const CANONICAL_SOURCE = new TextEncoder()") ==
                        std::string_view::npos);
                require(bytes.find(
                            "export function canonicalSource(): Uint8Array") !=
                        std::string_view::npos);
                require(bytes.find(
                            "CompiledSpec.compile(canonicalSource())") !=
                        std::string_view::npos);
            }
            if (spec.facts().has_references) {
                require(bytes.find(expectation.ref_target_marker) !=
                        std::string_view::npos);
            } else {
                require(bytes.find(expectation.ref_target_marker) ==
                        std::string_view::npos);
            }
            if (has_direct_ref_entry) {
                require(bytes.find(expectation.entry_ref_target_marker) !=
                        std::string_view::npos);
            } else {
                require(bytes.find(expectation.entry_ref_target_marker) ==
                        std::string_view::npos);
            }
            if (spec.profile() ==
                    fastdb::payload::spec::Profile::object_graph_v1 &&
                has_component_root) {
                require(bytes.find(
                            expectation.entry_graph_identity_marker) !=
                        std::string_view::npos);
            } else {
                require(bytes.find(
                            expectation.entry_graph_identity_marker) ==
                        std::string_view::npos);
            }
            require(bytes.find("materialize") != std::string_view::npos);
            if (!spec.resolved().components().empty()) {
                require(bytes.find(expectation.component_guard_marker) !=
                        std::string_view::npos);
                if (expectation.target == Target::python) {
                    require(bytes.find(
                                "return cls(view.clone(), _fastdb_component_view_token)") !=
                            std::string_view::npos);
                }
                if (expectation.target == Target::typescript) {
                    require(bytes.find(
                                "new " +
                                project_type_identifier(
                                    Target::typescript,
                                    spec.resolved().components().front().id) +
                                "(view.clone(), fastdbComponentViewToken)") !=
                            std::string_view::npos);
                }
            }
            if (!spec.resolved().entries().empty()) {
                const auto& first_entry = spec.resolved().entries().front();
                const std::string entry_symbol =
                    project_identifier(expectation.target, first_entry.id);
                if (expectation.target == Target::cpp) {
                    const std::string entry_type =
                        "FdbCppEntry_" + entry_symbol + "_Sequence";
                    require(bytes.find("friend " + entry_type + " " +
                                       entry_symbol + "_from_payload(") !=
                            std::string_view::npos);
                    require(bytes.find("private:\n    friend " + entry_type) !=
                            std::string_view::npos);
                    require(bytes.find("public:\n    explicit " + entry_type) ==
                            std::string_view::npos);
                    require(bytes.find("    explicit " + entry_type +
                                       "(fastdb::payload::v1::View view)") !=
                            std::string_view::npos);
                    require(bytes.find("return " + entry_type +
                                       "(payload.entry_view(") !=
                            std::string_view::npos);
                }
                if (expectation.target == Target::rust) {
                    require(bytes.find(
                                "pub fn new(view: fastdb::View) -> Self") ==
                            std::string_view::npos);
                    require(bytes.find(
                                "fn new(view: fastdb::View) -> Self") !=
                            std::string_view::npos);
                }
                if (expectation.target == Target::python) {
                    require(bytes.find("_fastdb_entry_view_token") !=
                            std::string_view::npos);
                    require(bytes.find(
                                "def __init__(self, view: View, token: object) -> None:") !=
                            std::string_view::npos);
                    require(bytes.find(
                                "if token is not _fastdb_entry_view_token:") !=
                            std::string_view::npos);
                    require(bytes.find(
                                "FastDB generated entry views require owned construction") !=
                            std::string_view::npos);
                }
                if (expectation.target == Target::typescript) {
                    require(bytes.find("fastdbEntryViewToken") !=
                            std::string_view::npos);
                    require(bytes.find(
                                "constructor(private readonly view: View, token: symbol) {") !=
                            std::string_view::npos);
                    require(bytes.find(
                                "if (token !== fastdbEntryViewToken) {") !=
                            std::string_view::npos);
                    require(bytes.find(
                                "FastDB generated entry views require owned construction") !=
                            std::string_view::npos);
                }
            }
            require(bytes.find("entry") != std::string_view::npos);
            require(bytes.find("builder") != std::string_view::npos ||
                    bytes.find("Builder") != std::string_view::npos);
            require(!contains_forbidden_content(bytes));

            for (const std::string_view id : observed_ids) {
                if (!has_source_id(spec.resolved(), id)) {
                    continue;
                }
                const std::string projected =
                    project_identifier(expectation.target, id);
                const std::size_t occurrences =
                    count_substring(bytes, projected);
                require(occurrences >= 2U,
                        "target=" + std::string(expectation.name) +
                            " id=" + std::string(id) +
                            " occurrences=" +
                            std::to_string(occurrences));
                require(bytes.find(id) != std::string_view::npos);
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_generation_rejects_unknown_target_and_literal_limits() {
    const CompiledSpec spec = compile(kEmptyRecordSource);

    auto unknown = generate(spec, static_cast<Target>(UINT8_C(99)));
    require(!unknown.has_value());
    require(unknown.error().code() == FDB_PAYLOAD_E_UNSUPPORTED_TARGET);
    require(unknown.error().path() == "/codegen/target");
    require(unknown.error().details_json() ==
            R"({"reason":"unknown_target"})");

    GenerationLimits no_artifacts;
    no_artifacts.max_artifacts = UINT64_C(0);
    auto count_rejected = generate(spec, Target::cpp, no_artifacts);
    require(!count_rejected.has_value());
    require(count_rejected.error().details_json() ==
            R"({"actual":"1","limit":"0","reason":"artifact_count_exceeded"})");

    auto baseline = generate(spec, Target::cpp);
    require(baseline.has_value());
    const std::uint64_t exact_size = static_cast<std::uint64_t>(
        baseline.value().artifacts().front().bytes().size());

    GenerationLimits exact;
    exact.max_total_bytes = exact_size;
    require(generate(spec, Target::cpp, exact).has_value());

    GenerationLimits one_short;
    one_short.max_total_bytes = exact_size - UINT64_C(1);
    auto byte_rejected = generate(spec, Target::cpp, one_short);
    require(!byte_rejected.has_value());
    require(byte_rejected.error().code() == FDB_PAYLOAD_E_GENERATOR_FAILED);
    require(byte_rejected.error().path() ==
            "/codegen/limits/max_total_bytes");
    return EXIT_SUCCESS;
}

int test_allocation_failures_publish_no_partial_artifact_set() {
    const CompiledSpec spec = compile(kEmptyRecordSource);

    bool invalid_target_allocation_escaped = false;
    std::optional<fastdb::payload::error::Result<ArtifactSet>>
        invalid_target;
    try {
        allocation_guard::FailAt fail_at(0U);
        invalid_target.emplace(
            generate(spec, static_cast<Target>(UINT8_C(99))));
    } catch (const std::bad_alloc&) {
        invalid_target_allocation_escaped = true;
    }
    require(!invalid_target_allocation_escaped);
    require(invalid_target.has_value());
    require(!invalid_target->has_value());
    require(invalid_target->error().code() ==
            FDB_PAYLOAD_E_GENERATOR_FAILED);
    require(invalid_target->error().path() == "/codegen");
    require(invalid_target->error().details_json() ==
            R"({"reason":"allocation_failed"})");

    GenerationLimits zero_bytes;
    zero_bytes.max_total_bytes = UINT64_C(0);
    bool observed_limit_allocation_failure = false;
    bool observed_checked_limit = false;
    for (std::size_t failure = 0U; failure < 4096U; ++failure) {
        bool allocation_escaped = false;
        std::optional<fastdb::payload::error::Result<ArtifactSet>> limited;
        try {
            allocation_guard::FailAt fail_at(failure);
            limited.emplace(generate(spec, Target::cpp, zero_bytes));
        } catch (const std::bad_alloc&) {
            allocation_escaped = true;
        }
        require(!allocation_escaped);
        require(limited.has_value());
        require(!limited->has_value());
        require(limited->error().code() == FDB_PAYLOAD_E_GENERATOR_FAILED);
        if (limited->error().details_json() ==
            R"({"reason":"allocation_failed"})") {
            observed_limit_allocation_failure = true;
        } else {
            require(limited->error().path() ==
                    "/codegen/limits/max_total_bytes");
            require(limited->error().details_json().find(
                        R"("limit":"0")") != std::string_view::npos);
            require(limited->error().details_json().find(
                        R"("reason":"total_bytes_exceeded")") !=
                    std::string_view::npos);
            observed_checked_limit = true;
        }
        if (observed_limit_allocation_failure && observed_checked_limit) {
            break;
        }
    }
    require(observed_limit_allocation_failure);
    require(observed_checked_limit);

    bool observed_failure = false;
    bool observed_success = false;
    for (std::size_t failure = 0U; failure < 4096U; ++failure) {
        std::optional<fastdb::payload::error::Result<ArtifactSet>> generated;
        {
            allocation_guard::FailAt fail_at(failure);
            generated.emplace(generate(spec, Target::cpp));
        }
        require(generated.has_value());
        if (generated->has_value()) {
            observed_success = true;
            require(generated->value().artifacts().size() == 1U);
            break;
        }
        observed_failure = true;
        require(generated->error().code() ==
                FDB_PAYLOAD_E_GENERATOR_FAILED);
        require(generated->error().path() == "/codegen");
        require(generated->error().details_json() ==
                R"({"reason":"allocation_failed"})");
    }
    require(observed_failure);
    require(observed_success);
    return EXIT_SUCCESS;
}

int emit_empty_goldens(const std::string& directory) {
    const CompiledSpec spec = compile(kEmptyRecordSource);
    std::filesystem::create_directories(directory);
    for (const auto& expectation : kTargets) {
        auto generated = generate(spec, expectation.target);
        if (!generated.has_value() ||
            generated.value().artifacts().size() != 1U) {
            return EXIT_FAILURE;
        }
        std::ofstream output(
            std::filesystem::path(directory) /
                std::string(expectation.golden),
            std::ios::binary | std::ios::trunc);
        if (!output) {
            return EXIT_FAILURE;
        }
        const std::string_view bytes =
            generated.value().artifacts().front().bytes();
        output.write(bytes.data(),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

int emit_spec_artifacts(const std::string& source_path,
                        const std::string& directory) {
    const std::string source = read_file(source_path);
    if (source.empty()) {
        return EXIT_FAILURE;
    }
    const CompiledSpec spec = compile(source);
    std::filesystem::create_directories(directory);
    for (const auto& expectation : kTargets) {
        auto generated = generate(spec, expectation.target);
        if (!generated.has_value() ||
            generated.value().artifacts().size() != 1U) {
            return EXIT_FAILURE;
        }
        const auto& artifact = generated.value().artifacts().front();
        std::ofstream output(
            std::filesystem::path(directory) /
                std::string(artifact.relative_path()),
            std::ios::binary | std::ios::trunc);
        if (!output) {
            return EXIT_FAILURE;
        }
        output.write(artifact.bytes().data(),
                     static_cast<std::streamsize>(
                         artifact.bytes().size()));
        if (!output) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--emit-empty-goldens") {
        return emit_empty_goldens(argv[2]);
    }
    if (argc == 4 && std::string_view(argv[1]) == "--emit-spec") {
        return emit_spec_artifacts(argv[2], argv[3]);
    }
    if (test_identifier_projection_is_total_and_collision_free() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_artifact_set_validates_paths_order_hashes_and_limits() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_exact_empty_artifacts_and_repeated_determinism() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_rich_specs_emit_only_official_runtime_ergonomics() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_generation_rejects_unknown_target_and_literal_limits() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_allocation_failures_publish_no_partial_artifact_set() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
