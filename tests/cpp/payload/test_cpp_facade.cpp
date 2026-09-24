#include "TestSupport.hpp"

#include <fastdb_payload.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(sizeof(fdb_payload_v1_compile_options_t) ==
                  FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_capabilities_t) ==
                  FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_codegen_options_t) ==
              FDB_PAYLOAD_V1_CODEGEN_OPTIONS_V1_SIZE);
static_assert(std::is_nothrow_move_constructible_v<
              fastdb::payload::v1::Blob>);
static_assert(std::is_nothrow_move_assignable_v<fastdb::payload::v1::Blob>);
static_assert(std::is_nothrow_move_constructible_v<
              fastdb::payload::v1::CompiledSpec>);
static_assert(std::is_nothrow_move_assignable_v<
              fastdb::payload::v1::CompiledSpec>);
static_assert(
    std::is_nothrow_move_constructible_v<fastdb::payload::v1::ArtifactSet>);
static_assert(
    std::is_nothrow_move_assignable_v<fastdb::payload::v1::ArtifactSet>);
static_assert(std::is_copy_constructible_v<fastdb::payload::v1::PayloadError>);

namespace {

using fastdb::payload::v1::Artifact;
using fastdb::payload::v1::ArtifactKind;
using fastdb::payload::v1::ArtifactSet;
using fastdb::payload::v1::Blob;
using fastdb::payload::v1::Capabilities;
using fastdb::payload::v1::CodegenOptions;
using fastdb::payload::v1::CodegenTarget;
using fastdb::payload::v1::CompiledSpec;
using fastdb::payload::v1::CompileOptions;
using fastdb::payload::v1::PayloadError;
using fastdb::payload::v1::Profile;

constexpr std::string_view kEmptySpec =
    R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]})";

constexpr std::string_view kSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"record.v1",
  "entries":[
    {"id":"single","cardinality":"one","type":{"kind":"component","id":"AllTypes"}},
    {"id":"series","cardinality":"many","type":{"kind":"list","nullable":true,"items":{"kind":"u8","nullable":true}}}
  ],
  "components":[
    {"id":"Leaf","kind":"record","fields":[]},
    {"id":"AllTypes","kind":"record","fields":[
      {"id":"value","type":{"kind":"u8n","min":-1,"max":1}},
      {"id":"label","type":{"kind":"str"}},
      {"id":"leaf","type":{"kind":"component","id":"Leaf"}}
    ]}
  ]
})";

struct CSpecOwner final {
    ~CSpecOwner() { fdb_payload_v1_spec_release(value); }
    fdb_payload_v1_spec_t* value{nullptr};
};

std::string take_blob_bytes(fdb_payload_v1_blob_t* blob) {
    const std::uint8_t* const data = fdb_payload_v1_blob_data(blob);
    const std::uint64_t size = fdb_payload_v1_blob_size(blob);
    std::string result;
    if (data != nullptr) {
        result.assign(reinterpret_cast<const char*>(data),
                      static_cast<std::size_t>(size));
    }
    fdb_payload_v1_blob_release(blob);
    return result;
}

int test_initializers_and_complete_queries() {
    CompileOptions options;
    require(options.value.struct_size ==
            FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE);
    options.value.max_source_bytes = UINT64_C(0);

    Capabilities initialized;
    require(initialized.value.struct_size ==
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
    require(initialized.value.profile == UINT32_C(0));

    CSpecOwner c_spec;
    fdb_payload_v1_error_t* c_error = nullptr;
    require(fdb_payload_v1_spec_compile_json(
                reinterpret_cast<const std::uint8_t*>(kSpec.data()),
                static_cast<std::uint64_t>(kSpec.size()), &options.value,
                &c_spec.value, &c_error) == UINT32_C(0));
    require(c_spec.value != nullptr && c_error == nullptr);

    CompiledSpec spec = CompiledSpec::compile(kSpec, options);

    fdb_payload_v1_blob_t* c_blob = nullptr;
    require(fdb_payload_v1_spec_canonical_json(
                c_spec.value, &c_blob, &c_error) == UINT32_C(0));
    const std::string c_canonical = take_blob_bytes(c_blob);
    const Blob canonical = spec.canonical_json();
    require(canonical.as_string_view() == c_canonical);
    require(canonical.bytes().size == c_canonical.size());
    require(canonical.bytes().data != nullptr);

    c_blob = nullptr;
    require(fdb_payload_v1_spec_manifest_json(
                c_spec.value, &c_blob, &c_error) == UINT32_C(0));
    const std::string c_manifest = take_blob_bytes(c_blob);
    require(spec.manifest_json().as_string_view() == c_manifest);

    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> c_digest{};
    require(fdb_payload_v1_spec_sha256(
                c_spec.value, c_digest.data(), &c_error) == UINT32_C(0));
    require(spec.sha256() == c_digest);

    fdb_payload_v1_profile_t c_profile = UINT32_C(0);
    require(fdb_payload_v1_spec_profile(
                c_spec.value, &c_profile, &c_error) == UINT32_C(0));
    require(static_cast<std::uint32_t>(spec.profile()) == c_profile);
    require(spec.profile() == Profile::record_v1);

    fdb_payload_v1_capabilities_t c_capabilities{};
    fdb_payload_v1_capabilities_init(&c_capabilities);
    require(fdb_payload_v1_spec_capabilities(
                c_spec.value, &c_capabilities, &c_error) == UINT32_C(0));
    const auto capabilities = spec.capabilities();
    require(capabilities.value.struct_size == c_capabilities.struct_size);
    require(capabilities.value.profile == c_capabilities.profile);
    require(capabilities.value.semantic_flags ==
            c_capabilities.semantic_flags);
    require(capabilities.value.operation_flags ==
            c_capabilities.operation_flags);
    require(capabilities.value.codegen_target_flags ==
            c_capabilities.codegen_target_flags);
    require(capabilities.value.direct_build_status ==
            c_capabilities.direct_build_status);
    require(capabilities.value.reserved32 == c_capabilities.reserved32);
    for (std::size_t index = 0; index < 4U; ++index) {
        require(capabilities.value.reserved64[index] ==
                c_capabilities.reserved64[index]);
    }

    std::uint32_t c_entry_count = UINT32_C(0);
    require(fdb_payload_v1_spec_entry_count(
                c_spec.value, &c_entry_count, &c_error) == UINT32_C(0));
    require(spec.entry_count() == c_entry_count);
    for (std::uint32_t entry = 0; entry < c_entry_count; ++entry) {
        c_blob = nullptr;
        require(fdb_payload_v1_spec_entry_id(
                    c_spec.value, entry, &c_blob, &c_error) == UINT32_C(0));
        const std::string c_id = take_blob_bytes(c_blob);
        require(spec.entry_id(entry).as_string_view() == c_id);
        std::uint32_t c_index = UINT32_MAX;
        require(fdb_payload_v1_spec_entry_index(
                    c_spec.value,
                    reinterpret_cast<const std::uint8_t*>(c_id.data()),
                    static_cast<std::uint64_t>(c_id.size()), &c_index,
                    &c_error) == UINT32_C(0));
        require(spec.entry_index(c_id) == c_index);
    }

    std::uint32_t c_component_count = UINT32_C(0);
    require(fdb_payload_v1_spec_component_count(
                c_spec.value, &c_component_count, &c_error) == UINT32_C(0));
    require(spec.component_count() == c_component_count);
    for (std::uint32_t component = 0; component < c_component_count;
         ++component) {
        c_blob = nullptr;
        require(fdb_payload_v1_spec_component_id(
                    c_spec.value, component, &c_blob,
                    &c_error) == UINT32_C(0));
        const std::string c_id = take_blob_bytes(c_blob);
        require(spec.component_id(component).as_string_view() == c_id);
        std::uint32_t c_index = UINT32_MAX;
        require(fdb_payload_v1_spec_component_index(
                    c_spec.value,
                    reinterpret_cast<const std::uint8_t*>(c_id.data()),
                    static_cast<std::uint64_t>(c_id.size()), &c_index,
                    &c_error) == UINT32_C(0));
        require(spec.component_index(c_id) == c_index);

        std::uint32_t c_field_count = UINT32_C(0);
        require(fdb_payload_v1_spec_component_field_count(
                    c_spec.value, component, &c_field_count,
                    &c_error) == UINT32_C(0));
        require(spec.component_field_count(component) == c_field_count);
        for (std::uint32_t field = 0; field < c_field_count; ++field) {
            c_blob = nullptr;
            require(fdb_payload_v1_spec_component_field_id(
                        c_spec.value, component, field, &c_blob,
                        &c_error) == UINT32_C(0));
            const std::string c_field_id = take_blob_bytes(c_blob);
            require(spec.component_field_id(component, field)
                        .as_string_view() == c_field_id);
            c_index = UINT32_MAX;
            require(fdb_payload_v1_spec_component_field_index(
                        c_spec.value, component,
                        reinterpret_cast<const std::uint8_t*>(
                            c_field_id.data()),
                        static_cast<std::uint64_t>(c_field_id.size()),
                        &c_index, &c_error) == UINT32_C(0));
            require(spec.component_field_index(component, c_field_id) ==
                    c_index);
        }
    }

    c_blob = nullptr;
    require(fdb_payload_v1_source_schema_json(&c_blob, &c_error) ==
            UINT32_C(0));
    const std::string c_schema = take_blob_bytes(c_blob);
    const Blob schema = fastdb::payload::v1::source_schema();
    require(schema.as_string_view() == c_schema);

    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> c_schema_digest{};
    require(fdb_payload_v1_source_schema_sha256(
                c_schema_digest.data(), &c_error) == UINT32_C(0));
    require(fastdb::payload::v1::source_schema_sha256() == c_schema_digest);
    require(c_error == nullptr);
    return EXIT_SUCCESS;
}

int test_blob_and_spec_copy_move_self_assignment_lifetimes() {
    const Blob detached = []() {
        const CompiledSpec temporary = CompiledSpec::compile(kSpec);
        return temporary.canonical_json();
    }();
    require(!detached.as_string_view().empty());

    CompiledSpec source = CompiledSpec::compile(kSpec);
    CompiledSpec copy(source);
    require(copy.entry_count() == UINT32_C(2));

    CompiledSpec assigned = CompiledSpec::compile(kEmptySpec);
    assigned = source;
    require(assigned.component_count() == UINT32_C(2));
    CompiledSpec* const assigned_alias = &assigned;
    assigned = *assigned_alias;
    require(assigned.entry_id(UINT32_C(0)).as_string_view() == "single");

    CompiledSpec moved(std::move(copy));
    require(moved.entry_index("series") == UINT32_C(1));
    CompiledSpec move_assigned = CompiledSpec::compile(kEmptySpec);
    move_assigned = std::move(moved);
    require(move_assigned.component_index("Leaf") == UINT32_C(1));
    CompiledSpec* const move_assigned_alias = &move_assigned;
    move_assigned = std::move(*move_assigned_alias);
    require(move_assigned.entry_count() == UINT32_C(2));

    Blob original = source.canonical_json();
    const auto bytes = original.bytes();
    require(bytes.data != nullptr && bytes.size != UINT64_C(0));
    const std::string expected(original.as_string_view());
    Blob blob_copy(original);
    require(blob_copy.as_string_view() == expected);
    Blob blob_assigned = source.manifest_json();
    blob_assigned = original;
    require(blob_assigned.as_string_view() == expected);
    Blob* const blob_assigned_alias = &blob_assigned;
    blob_assigned = *blob_assigned_alias;
    require(blob_assigned.as_string_view() == expected);
    Blob blob_moved(std::move(blob_copy));
    require(blob_moved.as_string_view() == expected);
    Blob blob_move_assigned = source.manifest_json();
    blob_move_assigned = std::move(blob_moved);
    require(blob_move_assigned.as_string_view() == expected);
    Blob* const blob_move_assigned_alias = &blob_move_assigned;
    blob_move_assigned = std::move(*blob_move_assigned_alias);
    require(blob_move_assigned.as_string_view() == expected);
    return EXIT_SUCCESS;
}

int test_payload_error_copies_every_owned_field() {
    std::optional<PayloadError> copied;
    try {
        static_cast<void>(CompiledSpec::compile(
            R"({"schema":1,"schema":2})"));
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_DUPLICATE_KEY);
        require(error.symbol() == "DUPLICATE_KEY");
        require(error.path() == "/schema");
        require(std::string_view(error.what()) ==
                "JSON object member name is duplicated");
        require(error.details_json() ==
                R"({"first_member_index":0,"key":"schema","second_member_index":1})");
        copied.emplace(error);
    }
    require(copied.has_value());
    require(copied->code() == FDB_PAYLOAD_E_DUPLICATE_KEY);
    require(copied->symbol() == "DUPLICATE_KEY");
    require(copied->path() == "/schema");
    require(std::string_view(copied->what()) ==
            "JSON object member name is duplicated");
    require(copied->details_json() ==
            R"({"first_member_index":0,"key":"schema","second_member_index":1})");

    const CompiledSpec spec = CompiledSpec::compile(kSpec);
    try {
        static_cast<void>(spec.entry_index(std::string_view{"single\0x", 8}));
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_INVALID_ARGUMENT);
        require(error.symbol() == "INVALID_ARGUMENT");
    }
    try {
        static_cast<void>(spec.entry_index("Single"));
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_NOT_FOUND);
        require(error.symbol() == "NOT_FOUND");
    }
    try {
        static_cast<void>(spec.entry_id(UINT32_C(2)));
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    }
    return EXIT_SUCCESS;
}

int test_artifact_codegen_raii_and_limits() {
    const CompiledSpec spec = CompiledSpec::compile(kSpec);
    ArtifactSet generated = spec.generate(CodegenTarget::cpp);
    require(generated.size() == UINT64_C(1));

    ArtifactSet copied(generated);
    require(copied.size() == UINT64_C(1));
    ArtifactSet assigned = spec.generate(CodegenTarget::rust);
    assigned = generated;
    require(assigned.size() == UINT64_C(1));
    ArtifactSet moved(std::move(copied));
    require(moved.size() == UINT64_C(1));

    const Artifact artifact = generated.at(UINT64_C(0));
    require(artifact.kind() == ArtifactKind::source);
    require(!artifact.relative_path().empty());
    require(artifact.relative_path().size() > 4U);
    require(artifact.relative_path().substr(artifact.relative_path().size() -
                                            4U) == ".hpp");
    require(artifact.bytes().data != nullptr);
    require(artifact.bytes().size != 0U);
    require(std::any_of(artifact.sha256().begin(), artifact.sha256().end(),
                        [](std::uint8_t byte) { return byte != UINT8_C(0); }));

    const Artifact detached = [&spec]() {
        ArtifactSet temporary = spec.generate(CodegenTarget::python);
        return temporary.at(UINT64_C(0));
    }();
    require(!detached.relative_path().empty());
    require(detached.bytes().size != 0U);

    // The ordinary Core Wasm receipt is deliberately single-threaded. Native
    // and pthread-enabled builds retain the shared ArtifactSet stress proof.
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    {
        std::atomic<std::uint32_t> concurrent_failures{UINT32_C(0)};
        std::vector<std::thread> workers;
        for (std::uint32_t worker = UINT32_C(0); worker < UINT32_C(8);
             ++worker) {
            workers.emplace_back([local = generated, &concurrent_failures]() {
                try {
                    for (std::uint32_t query = UINT32_C(0);
                         query < UINT32_C(32); ++query) {
                        const Artifact observed = local.at(UINT64_C(0));
                        if (local.size() != UINT64_C(1) ||
                            observed.kind() != ArtifactKind::source ||
                            observed.relative_path().empty() ||
                            observed.bytes().size == 0U) {
                            concurrent_failures.fetch_add(
                                UINT32_C(1), std::memory_order_relaxed);
                        }
                    }
                } catch (...) {
                    concurrent_failures.fetch_add(UINT32_C(1),
                                                  std::memory_order_relaxed);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        require(concurrent_failures.load(std::memory_order_relaxed) ==
                UINT32_C(0));
    }
#endif

    try {
        static_cast<void>(generated.at(UINT64_C(1)));
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    }

    CodegenOptions no_artifacts;
    no_artifacts.value.max_artifacts = UINT64_C(0);
    try {
        static_cast<void>(
            spec.generate(CodegenTarget::typescript, no_artifacts));
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_GENERATOR_FAILED);
        require(error.path() == "/codegen/limits/max_artifacts");
    }

    CodegenOptions no_bytes;
    no_bytes.value.max_total_bytes = UINT64_C(0);
    try {
        static_cast<void>(spec.generate(CodegenTarget::rust, no_bytes));
        require(false);
    } catch (const PayloadError& error) {
        require(error.code() == FDB_PAYLOAD_E_GENERATOR_FAILED);
        require(error.path() == "/codegen/limits/max_total_bytes");
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    require(test_initializers_and_complete_queries() == EXIT_SUCCESS);
    require(test_blob_and_spec_copy_move_self_assignment_lifetimes() ==
            EXIT_SUCCESS);
    require(test_payload_error_copies_every_owned_field() == EXIT_SUCCESS);
    require(test_artifact_codegen_raii_and_limits() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
