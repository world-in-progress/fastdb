#include "TestSupport.hpp"

#include <fastdb_payload.h>

#include "payload/abi/Handles.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define FASTDB_PAYLOAD_TEST_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FASTDB_PAYLOAD_TEST_HAS_ASAN 1
#endif
#endif

#ifndef FASTDB_PAYLOAD_TEST_HAS_ASAN
#define FASTDB_PAYLOAD_TEST_HAS_ASAN 0
#endif

#if FASTDB_PAYLOAD_TEST_HAS_ASAN
#include <sanitizer/asan_interface.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

static_assert(sizeof(fdb_payload_v1_compile_options_t) ==
                  FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_capabilities_t) ==
                  FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
static_assert(std::is_same_v<fdb_payload_v1_status_t, std::uint32_t>);

namespace {

std::atomic<std::int64_t> fail_after{-1};
std::atomic<bool> fail_forever{false};
std::atomic<std::uint64_t> live_allocations{UINT64_C(0)};

constexpr std::size_t kAllocationRegistryCapacity = 1U << 16U;
static_assert((kAllocationRegistryCapacity &
               (kAllocationRegistryCapacity - 1U)) == 0U);

// Sanitizer runtimes may route a deallocation through the replacement delete
// even when the matching allocation bypassed the replacement new. Track the
// pointers observed by this injector so only its own allocations affect the
// balance assertions. The fixed table and spin lock cannot allocate.
std::array<void*, kAllocationRegistryCapacity> allocation_registry{};
std::atomic_flag allocation_registry_lock = ATOMIC_FLAG_INIT;

void lock_allocation_registry() noexcept {
    while (allocation_registry_lock.test_and_set(std::memory_order_acquire)) {
    }
}

void unlock_allocation_registry() noexcept {
    allocation_registry_lock.clear(std::memory_order_release);
}

bool register_allocation_unlocked(void* value) noexcept {
    const std::size_t mask = kAllocationRegistryCapacity - 1U;
    const std::size_t start =
        (reinterpret_cast<std::uintptr_t>(value) >> 4U) & mask;
    for (std::size_t offset = 0; offset < kAllocationRegistryCapacity;
         ++offset) {
        const std::size_t index = (start + offset) & mask;
        if (allocation_registry[index] == nullptr) {
            allocation_registry[index] = value;
            return true;
        }
    }
    return false;
}

bool register_allocation(void* value) noexcept {
    lock_allocation_registry();
    const bool registered = register_allocation_unlocked(value);
    unlock_allocation_registry();
    return registered;
}

bool unregister_allocation(void* value) noexcept {
    const std::size_t mask = kAllocationRegistryCapacity - 1U;
    const std::size_t start =
        (reinterpret_cast<std::uintptr_t>(value) >> 4U) & mask;
    lock_allocation_registry();
    for (std::size_t offset = 0; offset < kAllocationRegistryCapacity;
         ++offset) {
        const std::size_t index = (start + offset) & mask;
        void* const entry = allocation_registry[index];
        if (entry == nullptr) {
            unlock_allocation_registry();
            return false;
        }
        if (entry == value) {
            allocation_registry[index] = nullptr;
            std::size_t following = (index + 1U) & mask;
            while (allocation_registry[following] != nullptr) {
                void* const displaced = allocation_registry[following];
                allocation_registry[following] = nullptr;
                if (!register_allocation_unlocked(displaced)) {
                    std::abort();
                }
                following = (following + 1U) & mask;
            }
            unlock_allocation_registry();
            return true;
        }
    }
    unlock_allocation_registry();
    return false;
}

bool should_fail_allocation() noexcept {
    if (fail_forever.load(std::memory_order_relaxed)) {
        return true;
    }
    std::int64_t current = fail_after.load(std::memory_order_relaxed);
    while (current >= 0) {
        if (current == 0) {
            if (fail_after.compare_exchange_weak(
                    current, -1, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
            continue;
        }
        if (fail_after.compare_exchange_weak(
                current, current - 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return false;
        }
    }
    return false;
}

void* allocate_for_test(std::size_t size) {
    if (should_fail_allocation()) {
        throw std::bad_alloc{};
    }
    void* const result = std::malloc(size == 0U ? 1U : size);
    if (result == nullptr) {
        throw std::bad_alloc{};
    }
    if (!register_allocation(result)) {
        std::free(result);
        throw std::bad_alloc{};
    }
    live_allocations.fetch_add(UINT64_C(1), std::memory_order_relaxed);
    return result;
}

void deallocate_for_test(void* value) noexcept {
    if (value != nullptr) {
        if (unregister_allocation(value)) {
            live_allocations.fetch_sub(UINT64_C(1),
                                       std::memory_order_relaxed);
        }
        std::free(value);
    }
}

void disable_failures() noexcept {
    fail_forever.store(false, std::memory_order_relaxed);
    fail_after.store(-1, std::memory_order_relaxed);
}

constexpr std::string_view kEmptySpec =
    R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]})";

constexpr std::string_view kRichSpec = R"({
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

constexpr std::string_view kDuplicateKey =
    R"({"schema":1,"schema":2})";

std::string_view blob_view(const fdb_payload_v1_blob_t* blob) {
    const std::uint8_t* const data = fdb_payload_v1_blob_data(blob);
    const std::uint64_t size = fdb_payload_v1_blob_size(blob);
    if (data == nullptr) {
        return {};
    }
    return {reinterpret_cast<const char*>(data),
            static_cast<std::size_t>(size)};
}

using ErrorGetter = void (*)(const fdb_payload_v1_error_t*,
                             const std::uint8_t**,
                             std::uint64_t*);

std::string_view error_view(const fdb_payload_v1_error_t* error,
                            ErrorGetter getter) {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    getter(error, &data, &size);
    if (data == nullptr) {
        return {};
    }
    return {reinterpret_cast<const char*>(data),
            static_cast<std::size_t>(size)};
}

bool error_is(const fdb_payload_v1_error_t* error,
              std::uint32_t code,
              std::string_view symbol,
              std::string_view path,
              std::string_view message,
              std::string_view details) {
    return error != nullptr && fdb_payload_v1_error_code(error) == code &&
           error_view(error, fdb_payload_v1_error_symbol) == symbol &&
           error_view(error, fdb_payload_v1_error_path) == path &&
           error_view(error, fdb_payload_v1_error_message) == message &&
           error_view(error, fdb_payload_v1_error_details_json) == details;
}

std::array<std::uint8_t, 32> hex_digest(std::string_view hex) {
    std::array<std::uint8_t, 32> result{};
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        return static_cast<std::uint8_t>(value - 'a' + 10);
    };
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(
            (nibble(hex[index * 2U]) << 4U) |
            nibble(hex[index * 2U + 1U]));
    }
    return result;
}

fdb_payload_v1_status_t compile(std::string_view source,
                                const fdb_payload_v1_compile_options_t* options,
                                fdb_payload_v1_spec_t** out_spec,
                                fdb_payload_v1_error_t** out_error) {
    return fdb_payload_v1_spec_compile_json(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), options, out_spec,
        out_error);
}

int test_compile_and_every_query_round_trip() {
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    require(compile(kRichSpec, nullptr, &spec, &error) == UINT32_C(0));
    require(spec != nullptr);
    require(error == nullptr);

    fdb_payload_v1_blob_t* canonical = nullptr;
    fdb_payload_v1_blob_t* manifest = nullptr;
    require(fdb_payload_v1_spec_canonical_json(spec, &canonical, &error) ==
            UINT32_C(0));
    require(fdb_payload_v1_spec_manifest_json(spec, &manifest, &error) ==
            UINT32_C(0));
    require(!blob_view(canonical).empty());
    require(!blob_view(manifest).empty());
    require(blob_view(manifest).find("\"operations\":[\"compile\",\"query\"]") !=
            std::string_view::npos);

    std::array<std::uint8_t, 32> digest{};
    require(fdb_payload_v1_spec_sha256(spec, digest.data(), &error) ==
            UINT32_C(0));
    require(digest == hex_digest(
                          "031fe82b5b26811157e5f0f4079832962bb16fdd3fc1d"
                          "4fe22aecc9a1e7a9151"));

    fdb_payload_v1_profile_t profile = UINT32_C(0);
    require(fdb_payload_v1_spec_profile(spec, &profile, &error) ==
            UINT32_C(0));
    require(profile == FDB_PAYLOAD_PROFILE_RECORD_V1);

    fdb_payload_v1_capabilities_t capabilities{};
    fdb_payload_v1_capabilities_init(&capabilities);
    require(fdb_payload_v1_spec_capabilities(spec, &capabilities, &error) ==
            UINT32_C(0));
    require(capabilities.struct_size ==
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
    require(capabilities.profile == FDB_PAYLOAD_PROFILE_RECORD_V1);
    require(capabilities.semantic_flags ==
            (FDB_PAYLOAD_SEMANTIC_HAS_NULLABLE |
             FDB_PAYLOAD_SEMANTIC_HAS_LISTS |
             FDB_PAYLOAD_SEMANTIC_HAS_VARIABLE_WIDTH |
             FDB_PAYLOAD_SEMANTIC_HAS_NORMALIZED_INTEGERS));
    require(capabilities.operation_flags ==
            (FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY));
    require(capabilities.codegen_target_flags == UINT64_C(0));
    require(capabilities.direct_build_status ==
            FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED);

    std::uint32_t count = UINT32_C(0);
    require(fdb_payload_v1_spec_entry_count(spec, &count, &error) ==
            UINT32_C(0));
    require(count == UINT32_C(2));
    constexpr std::string_view entry_ids[] = {"single", "series"};
    for (std::uint32_t index = 0; index < count; ++index) {
        fdb_payload_v1_blob_t* id = nullptr;
        require(fdb_payload_v1_spec_entry_id(spec, index, &id, &error) ==
                UINT32_C(0));
        require(blob_view(id) == entry_ids[index]);
        std::uint32_t round_trip = UINT32_MAX;
        require(fdb_payload_v1_spec_entry_index(
                    spec, fdb_payload_v1_blob_data(id),
                    fdb_payload_v1_blob_size(id), &round_trip, &error) ==
                UINT32_C(0));
        require(round_trip == index);
        fdb_payload_v1_blob_release(id);
    }

    require(fdb_payload_v1_spec_component_count(spec, &count, &error) ==
            UINT32_C(0));
    require(count == UINT32_C(2));
    constexpr std::string_view component_ids[] = {"AllTypes", "Leaf"};
    for (std::uint32_t component = 0; component < count; ++component) {
        fdb_payload_v1_blob_t* id = nullptr;
        require(fdb_payload_v1_spec_component_id(spec, component, &id,
                                                 &error) == UINT32_C(0));
        require(blob_view(id) == component_ids[component]);
        std::uint32_t round_trip = UINT32_MAX;
        require(fdb_payload_v1_spec_component_index(
                    spec, fdb_payload_v1_blob_data(id),
                    fdb_payload_v1_blob_size(id), &round_trip, &error) ==
                UINT32_C(0));
        require(round_trip == component);
        fdb_payload_v1_blob_release(id);
    }

    require(fdb_payload_v1_spec_component_field_count(
                spec, UINT32_C(0), &count, &error) == UINT32_C(0));
    require(count == UINT32_C(3));
    constexpr std::string_view field_ids[] = {"value", "label", "leaf"};
    for (std::uint32_t field = 0; field < count; ++field) {
        fdb_payload_v1_blob_t* id = nullptr;
        require(fdb_payload_v1_spec_component_field_id(
                    spec, UINT32_C(0), field, &id, &error) == UINT32_C(0));
        require(blob_view(id) == field_ids[field]);
        std::uint32_t round_trip = UINT32_MAX;
        require(fdb_payload_v1_spec_component_field_index(
                    spec, UINT32_C(0), fdb_payload_v1_blob_data(id),
                    fdb_payload_v1_blob_size(id), &round_trip, &error) ==
                UINT32_C(0));
        require(round_trip == field);
        fdb_payload_v1_blob_release(id);
    }

    fdb_payload_v1_blob_t* schema = nullptr;
    require(fdb_payload_v1_source_schema_json(&schema, &error) == UINT32_C(0));
    require(blob_view(schema).find("urn:fastdb:schema:fastdb.payload.v1") !=
            std::string_view::npos);
    std::array<std::uint8_t, 32> schema_digest{};
    require(fdb_payload_v1_source_schema_sha256(schema_digest.data(),
                                                &error) == UINT32_C(0));
    require(schema_digest == hex_digest(
                                 "527209d9820ab21260f56a23befa7a5188feb55e"
                                 "9729ea63930425e6226739a1"));

    fdb_payload_v1_blob_release(schema);
    fdb_payload_v1_blob_release(canonical);
    fdb_payload_v1_blob_release(manifest);
    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

int test_nullability_output_clearing_and_exact_errors() {
    fdb_payload_v1_spec_t* spec = reinterpret_cast<fdb_payload_v1_spec_t*>(1);
    fdb_payload_v1_error_t* error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
    fdb_payload_v1_status_t status = fdb_payload_v1_spec_compile_json(
        nullptr, UINT64_C(1), nullptr, &spec, &error);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(spec == nullptr);
    require(error_is(error, FDB_PAYLOAD_E_INVALID_ARGUMENT,
                     "INVALID_ARGUMENT", "", "Invalid ABI argument",
                     R"({"argument":"source","reason":"null_data"})"));
    fdb_payload_v1_error_release(error);

    spec = reinterpret_cast<fdb_payload_v1_spec_t*>(1);
    error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
    status = fdb_payload_v1_spec_compile_json(nullptr, UINT64_C(0), nullptr,
                                              &spec, &error);
    require(status == FDB_PAYLOAD_E_INVALID_JSON);
    require(spec == nullptr);
    require(error != nullptr && fdb_payload_v1_error_code(error) == status);
    fdb_payload_v1_error_release(error);

    spec = reinterpret_cast<fdb_payload_v1_spec_t*>(1);
    status = compile(kEmptySpec, nullptr, &spec, nullptr);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(spec == reinterpret_cast<fdb_payload_v1_spec_t*>(1));

    error = nullptr;
    status = compile(kEmptySpec, nullptr, nullptr, &error);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(error != nullptr && fdb_payload_v1_error_code(error) == status);
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_blob_t* blob = reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
    status = fdb_payload_v1_spec_canonical_json(nullptr, &blob, &error);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(blob == nullptr);
    require(error_is(error, FDB_PAYLOAD_E_INVALID_ARGUMENT,
                     "INVALID_ARGUMENT", "", "Invalid ABI argument",
                     R"({"argument":"spec","reason":"null_handle"})"));
    fdb_payload_v1_error_release(error);

    std::array<std::uint8_t, 32> digest{};
    digest.fill(UINT8_C(0xa5));
    error = nullptr;
    status = fdb_payload_v1_spec_sha256(nullptr, digest.data(), &error);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(std::all_of(digest.begin(), digest.end(),
                        [](std::uint8_t value) { return value == 0U; }));
    fdb_payload_v1_error_release(error);

    std::uint32_t scalar = UINT32_MAX;
    error = nullptr;
    status = fdb_payload_v1_spec_entry_count(nullptr, &scalar, &error);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(scalar == UINT32_C(0));
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_capabilities_t capabilities{};
    fdb_payload_v1_capabilities_init(&capabilities);
    capabilities.profile = UINT32_MAX;
    capabilities.semantic_flags = UINT64_MAX;
    capabilities.operation_flags = UINT64_MAX;
    capabilities.codegen_target_flags = UINT64_MAX;
    error = nullptr;
    status = fdb_payload_v1_spec_capabilities(
        nullptr, &capabilities, &error);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(capabilities.struct_size ==
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
    require(capabilities.profile == UINT32_C(0));
    require(capabilities.semantic_flags == UINT64_C(0));
    require(capabilities.operation_flags == UINT64_C(0));
    require(capabilities.codegen_target_flags == UINT64_C(0));
    require(capabilities.direct_build_status ==
            FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED);
    require(capabilities.reserved32 == UINT32_C(0));
    require(std::all_of(std::begin(capabilities.reserved64),
                        std::end(capabilities.reserved64),
                        [](std::uint64_t value) {
                            return value == UINT64_C(0);
                        }));
    require(capabilities.semantic_flags == UINT64_C(0));
    require(capabilities.operation_flags == UINT64_C(0));
    require(capabilities.codegen_target_flags == UINT64_C(0));
    fdb_payload_v1_error_release(error);

    error = nullptr;
    status = fdb_payload_v1_source_schema_json(nullptr, &error);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(error != nullptr && fdb_payload_v1_error_code(error) == status);
    fdb_payload_v1_error_release(error);

    digest.fill(UINT8_C(0xa5));
    status = fdb_payload_v1_source_schema_sha256(digest.data(), nullptr);
    require(status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(std::all_of(digest.begin(), digest.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    return EXIT_SUCCESS;
}

int test_null_error_sink_preserves_every_value_output() {
    fdb_payload_v1_spec_t* compile_output =
        reinterpret_cast<fdb_payload_v1_spec_t*>(1);
    require(compile(kEmptySpec, nullptr, &compile_output, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(compile_output == reinterpret_cast<fdb_payload_v1_spec_t*>(1));

    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    require(compile(kRichSpec, nullptr, &spec, &error) == UINT32_C(0));
    require(spec != nullptr && error == nullptr);

    fdb_payload_v1_blob_t* blob =
        reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    require(fdb_payload_v1_spec_canonical_json(spec, &blob, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(blob == reinterpret_cast<fdb_payload_v1_blob_t*>(1));

    blob = reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    require(fdb_payload_v1_spec_manifest_json(spec, &blob, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(blob == reinterpret_cast<fdb_payload_v1_blob_t*>(1));

    std::array<std::uint8_t, 32> digest{};
    digest.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_spec_sha256(spec, digest.data(), nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(std::all_of(digest.begin(), digest.end(), [](std::uint8_t value) {
        return value == UINT8_C(0xa5);
    }));

    fdb_payload_v1_profile_t profile = UINT32_MAX;
    require(fdb_payload_v1_spec_profile(spec, &profile, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(profile == UINT32_MAX);

    fdb_payload_v1_capabilities_t capabilities{};
    fdb_payload_v1_capabilities_init(&capabilities);
    capabilities.profile = UINT32_MAX;
    capabilities.semantic_flags = UINT64_MAX;
    capabilities.operation_flags = UINT64_MAX;
    capabilities.codegen_target_flags = UINT64_MAX;
    capabilities.direct_build_status = UINT32_MAX;
    capabilities.reserved32 = UINT32_MAX;
    std::fill(std::begin(capabilities.reserved64),
              std::end(capabilities.reserved64), UINT64_MAX);
    const fdb_payload_v1_capabilities_t capabilities_sentinel = capabilities;
    require(fdb_payload_v1_spec_capabilities(spec, &capabilities, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(std::memcmp(&capabilities, &capabilities_sentinel,
                        sizeof(capabilities)) == 0);

    std::uint32_t scalar = UINT32_MAX;
    require(fdb_payload_v1_spec_entry_count(spec, &scalar, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(scalar == UINT32_MAX);

    blob = reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    require(fdb_payload_v1_spec_entry_id(spec, UINT32_C(0), &blob, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(blob == reinterpret_cast<fdb_payload_v1_blob_t*>(1));

    constexpr std::uint8_t entry_id[] = {'s', 'i', 'n', 'g', 'l', 'e'};
    scalar = UINT32_MAX;
    require(fdb_payload_v1_spec_entry_index(
                spec, entry_id, sizeof(entry_id), &scalar, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(scalar == UINT32_MAX);

    scalar = UINT32_MAX;
    require(fdb_payload_v1_spec_component_count(spec, &scalar, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(scalar == UINT32_MAX);

    blob = reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    require(fdb_payload_v1_spec_component_id(
                spec, UINT32_C(0), &blob, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(blob == reinterpret_cast<fdb_payload_v1_blob_t*>(1));

    constexpr std::uint8_t component_id[] = {'A', 'l', 'l', 'T', 'y',
                                             'p', 'e', 's'};
    scalar = UINT32_MAX;
    require(fdb_payload_v1_spec_component_index(
                spec, component_id, sizeof(component_id), &scalar, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(scalar == UINT32_MAX);

    scalar = UINT32_MAX;
    require(fdb_payload_v1_spec_component_field_count(
                spec, UINT32_C(0), &scalar, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(scalar == UINT32_MAX);

    blob = reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    require(fdb_payload_v1_spec_component_field_id(
                spec, UINT32_C(0), UINT32_C(0), &blob, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(blob == reinterpret_cast<fdb_payload_v1_blob_t*>(1));

    constexpr std::uint8_t field_id[] = {'v', 'a', 'l', 'u', 'e'};
    scalar = UINT32_MAX;
    require(fdb_payload_v1_spec_component_field_index(
                spec, UINT32_C(0), field_id, sizeof(field_id), &scalar,
                nullptr) == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(scalar == UINT32_MAX);

    blob = reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    require(fdb_payload_v1_source_schema_json(&blob, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(blob == reinterpret_cast<fdb_payload_v1_blob_t*>(1));

    digest.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_source_schema_sha256(digest.data(), nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(std::all_of(digest.begin(), digest.end(), [](std::uint8_t value) {
        return value == UINT8_C(0xa5);
    }));

    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

int test_every_required_output_and_null_handle_path_clears() {
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    require(compile(kRichSpec, nullptr, &spec, &error) == UINT32_C(0));

    using BlobSpecQuery = fdb_payload_v1_status_t (*)(
        const fdb_payload_v1_spec_t*, fdb_payload_v1_blob_t**,
        fdb_payload_v1_error_t**);
    constexpr BlobSpecQuery blob_queries[] = {
        fdb_payload_v1_spec_canonical_json,
        fdb_payload_v1_spec_manifest_json,
    };
    for (BlobSpecQuery query : blob_queries) {
        fdb_payload_v1_blob_t* blob =
            reinterpret_cast<fdb_payload_v1_blob_t*>(1);
        error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
        require(query(nullptr, &blob, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        require(blob == nullptr);
        require(error != nullptr && fdb_payload_v1_error_code(error) ==
                                        FDB_PAYLOAD_E_INVALID_ARGUMENT);
        fdb_payload_v1_error_release(error);
        error = nullptr;
        require(query(spec, nullptr, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        require(error != nullptr && fdb_payload_v1_error_code(error) ==
                                        FDB_PAYLOAD_E_INVALID_ARGUMENT);
        fdb_payload_v1_error_release(error);
    }

    using CountSpecQuery = fdb_payload_v1_status_t (*)(
        const fdb_payload_v1_spec_t*, std::uint32_t*,
        fdb_payload_v1_error_t**);
    constexpr CountSpecQuery count_queries[] = {
        fdb_payload_v1_spec_entry_count,
        fdb_payload_v1_spec_component_count,
    };
    for (CountSpecQuery query : count_queries) {
        std::uint32_t count = UINT32_MAX;
        error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
        require(query(nullptr, &count, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        require(count == UINT32_C(0));
        fdb_payload_v1_error_release(error);
        error = nullptr;
        require(query(spec, nullptr, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        fdb_payload_v1_error_release(error);
    }

    using IdSpecQuery = fdb_payload_v1_status_t (*)(
        const fdb_payload_v1_spec_t*, std::uint32_t,
        fdb_payload_v1_blob_t**, fdb_payload_v1_error_t**);
    constexpr IdSpecQuery id_queries[] = {
        fdb_payload_v1_spec_entry_id,
        fdb_payload_v1_spec_component_id,
    };
    for (IdSpecQuery query : id_queries) {
        fdb_payload_v1_blob_t* id =
            reinterpret_cast<fdb_payload_v1_blob_t*>(1);
        error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
        require(query(nullptr, UINT32_C(0), &id, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        require(id == nullptr);
        fdb_payload_v1_error_release(error);
        error = nullptr;
        require(query(spec, UINT32_C(0), nullptr, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        fdb_payload_v1_error_release(error);
    }

    using IndexSpecQuery = fdb_payload_v1_status_t (*)(
        const fdb_payload_v1_spec_t*, const std::uint8_t*, std::uint64_t,
        std::uint32_t*, fdb_payload_v1_error_t**);
    constexpr IndexSpecQuery index_queries[] = {
        fdb_payload_v1_spec_entry_index,
        fdb_payload_v1_spec_component_index,
    };
    constexpr std::uint8_t valid_id[] = {'A'};
    for (IndexSpecQuery query : index_queries) {
        std::uint32_t index = UINT32_MAX;
        error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
        require(query(nullptr, valid_id, sizeof(valid_id), &index, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        require(index == UINT32_C(0));
        fdb_payload_v1_error_release(error);
        error = nullptr;
        require(query(spec, valid_id, sizeof(valid_id), nullptr, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        fdb_payload_v1_error_release(error);
    }

    std::array<std::uint8_t, 32> digest{};
    digest.fill(UINT8_C(0xa5));
    error = nullptr;
    require(fdb_payload_v1_spec_sha256(nullptr, digest.data(), &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(std::all_of(digest.begin(), digest.end(),
                        [](std::uint8_t value) { return value == 0U; }));
    fdb_payload_v1_error_release(error);
    error = nullptr;
    require(fdb_payload_v1_spec_sha256(spec, nullptr, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_profile_t profile = UINT32_MAX;
    error = nullptr;
    require(fdb_payload_v1_spec_profile(nullptr, &profile, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(profile == UINT32_C(0));
    fdb_payload_v1_error_release(error);
    error = nullptr;
    require(fdb_payload_v1_spec_profile(spec, nullptr, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_capabilities_t capabilities{};
    fdb_payload_v1_capabilities_init(&capabilities);
    error = nullptr;
    require(fdb_payload_v1_spec_capabilities(nullptr, &capabilities, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(capabilities.struct_size ==
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
    require(capabilities.profile == UINT32_C(0));
    fdb_payload_v1_error_release(error);
    error = nullptr;
    require(fdb_payload_v1_spec_capabilities(spec, nullptr, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);

    std::uint32_t count = UINT32_MAX;
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_count(
                nullptr, UINT32_C(0), &count, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(count == UINT32_C(0));
    fdb_payload_v1_error_release(error);
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_count(
                spec, UINT32_C(0), nullptr, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_blob_t* field_id =
        reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_id(
                nullptr, UINT32_C(0), UINT32_C(0), &field_id, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(field_id == nullptr);
    fdb_payload_v1_error_release(error);
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_id(
                spec, UINT32_C(0), UINT32_C(0), nullptr, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);

    std::uint32_t field_index = UINT32_MAX;
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_index(
                nullptr, UINT32_C(0), valid_id, sizeof(valid_id), &field_index,
                &error) == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(field_index == UINT32_C(0));
    fdb_payload_v1_error_release(error);
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_index(
                spec, UINT32_C(0), valid_id, sizeof(valid_id), nullptr,
                &error) == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);

    error = nullptr;
    require(fdb_payload_v1_source_schema_json(nullptr, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);
    error = nullptr;
    require(fdb_payload_v1_source_schema_sha256(nullptr, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);

    error = nullptr;
    require(compile(kEmptySpec, nullptr, nullptr, &error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

int test_options_prefix_tail_reserved_and_limits() {
    fdb_payload_v1_compile_options_t options{};
    fdb_payload_v1_compile_options_init(&options);
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;

    options.struct_size = UINT32_C(4);
    std::array<std::uint8_t, sizeof(options) - sizeof(std::uint32_t)> canary{};
    std::memcpy(canary.data(), reinterpret_cast<const std::uint8_t*>(&options) +
                                   sizeof(std::uint32_t),
                canary.size());
    require(compile(kEmptySpec, &options, &spec, &error) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(spec == nullptr);
    require(error_is(
        error, FDB_PAYLOAD_E_UNSUPPORTED_ABI, "UNSUPPORTED_ABI", "",
        "Unsupported ABI struct contract",
        R"({"argument":"options","minimum_struct_size":80,"reason":"struct_size_too_small","struct_size":4})"));
    require(std::memcmp(canary.data(),
                        reinterpret_cast<const std::uint8_t*>(&options) +
                            sizeof(std::uint32_t),
                        canary.size()) == 0);
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_compile_options_init(&options);
    options.flags = UINT32_C(1);
    error = nullptr;
    require(compile(kEmptySpec, &options, &spec, &error) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(error_is(
        error, FDB_PAYLOAD_E_UNSUPPORTED_ABI, "UNSUPPORTED_ABI", "",
        "Unsupported ABI struct contract",
        R"({"argument":"options","field":"flags","reason":"nonzero_v1_field"})"));
    fdb_payload_v1_error_release(error);

    for (std::size_t reserved = 0; reserved < 4U; ++reserved) {
        fdb_payload_v1_compile_options_init(&options);
        options.reserved[reserved] = UINT64_C(1);
        error = nullptr;
        require(compile(kEmptySpec, &options, &spec, &error) ==
                FDB_PAYLOAD_E_UNSUPPORTED_ABI);
        require(error != nullptr &&
                fdb_payload_v1_error_code(error) ==
                    FDB_PAYLOAD_E_UNSUPPORTED_ABI);
        fdb_payload_v1_error_release(error);
    }

    fdb_payload_v1_compile_options_init(&options);
    options.max_source_bytes = UINT64_C(3);
    error = nullptr;
    require(compile(kEmptySpec, &options, &spec, &error) ==
            FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT);
    require(spec == nullptr);
    require(error != nullptr && fdb_payload_v1_error_code(error) ==
                                    FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT);
    fdb_payload_v1_error_release(error);

    const auto rejected_by_limit = [&](std::string_view source) {
        spec = reinterpret_cast<fdb_payload_v1_spec_t*>(1);
        error = nullptr;
        const fdb_payload_v1_status_t status =
            compile(source, &options, &spec, &error);
        const bool rejected =
            status == FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT && spec == nullptr &&
            error != nullptr && fdb_payload_v1_error_code(error) == status;
        fdb_payload_v1_error_release(error);
        return rejected;
    };

    fdb_payload_v1_compile_options_init(&options);
    options.max_json_values = UINT64_C(1);
    require(rejected_by_limit(kEmptySpec));
    fdb_payload_v1_compile_options_init(&options);
    options.max_nesting_depth = UINT32_C(1);
    require(rejected_by_limit(kEmptySpec));
    fdb_payload_v1_compile_options_init(&options);
    options.max_entries = UINT32_C(1);
    require(rejected_by_limit(kRichSpec));
    fdb_payload_v1_compile_options_init(&options);
    options.max_components = UINT32_C(1);
    require(rejected_by_limit(kRichSpec));
    fdb_payload_v1_compile_options_init(&options);
    options.max_fields_per_component = UINT32_C(2);
    require(rejected_by_limit(kRichSpec));
    fdb_payload_v1_compile_options_init(&options);
    options.max_total_fields = UINT64_C(2);
    require(rejected_by_limit(kRichSpec));

    fdb_payload_v1_compile_options_init(&options);
    options.max_source_bytes = UINT64_C(0);
    options.max_json_values = UINT64_C(0);
    options.max_nesting_depth = UINT32_C(0);
    options.max_entries = UINT32_C(0);
    options.max_components = UINT32_C(0);
    options.max_fields_per_component = UINT32_C(0);
    options.max_total_fields = UINT64_C(0);
    error = nullptr;
    require(compile(kEmptySpec, &options, &spec, &error) == UINT32_C(0));
    require(spec != nullptr && error == nullptr);
    fdb_payload_v1_spec_release(spec);

    struct FutureOptions final {
        fdb_payload_v1_compile_options_t known;
        std::array<std::uint8_t, 32> future;
    } future{};
    fdb_payload_v1_compile_options_init(&future.known);
    future.known.struct_size = static_cast<std::uint32_t>(sizeof(future));
    future.future.fill(UINT8_C(0xa5));
    const auto expected_tail = future.future;
    error = nullptr;
    spec = nullptr;
    require(compile(kEmptySpec, &future.known, &spec, &error) == UINT32_C(0));
    require(future.future == expected_tail);
    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

#if defined(__unix__) || defined(__APPLE__)
int test_guarded_short_prefixes_do_not_read_the_tail() {
    static_assert(alignof(fdb_payload_v1_compile_options_t) ==
                  alignof(fdb_payload_v1_capabilities_t));
    const long queried_page_size = ::sysconf(_SC_PAGESIZE);
    require(queried_page_size > 0);
    const std::size_t page_size = static_cast<std::size_t>(queried_page_size);
    constexpr std::size_t struct_alignment =
        alignof(fdb_payload_v1_compile_options_t);
    require(page_size % struct_alignment == 0U);
    void* const pages =
        ::mmap(nullptr, page_size * 2U, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANON, -1, 0);
    require(pages != MAP_FAILED);
    require(::mprotect(static_cast<char*>(pages) + page_size, page_size,
                       PROT_NONE) == 0);

    auto* const prefix = reinterpret_cast<std::uint32_t*>(
        static_cast<char*>(pages) + page_size - struct_alignment);
    require(reinterpret_cast<std::uintptr_t>(prefix) % struct_alignment ==
            0U);
    constexpr std::uint32_t canary = UINT32_C(0xa5a55a5a);
    prefix[0] = UINT32_C(4);
    prefix[1] = canary;
#if FASTDB_PAYLOAD_TEST_HAS_ASAN
    __asan_poison_memory_region(prefix + 1, sizeof(std::uint32_t));
#endif
    const auto* const options =
        reinterpret_cast<const fdb_payload_v1_compile_options_t*>(prefix);
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const fdb_payload_v1_status_t options_status =
        compile(kEmptySpec, options, &spec, &error);
#if FASTDB_PAYLOAD_TEST_HAS_ASAN
    __asan_unpoison_memory_region(prefix + 1, sizeof(std::uint32_t));
#endif
    require(options_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(spec == nullptr);
    require(error_is(
        error, FDB_PAYLOAD_E_UNSUPPORTED_ABI, "UNSUPPORTED_ABI", "",
        "Unsupported ABI struct contract",
        R"({"argument":"options","minimum_struct_size":80,"reason":"struct_size_too_small","struct_size":4})"));
    require(prefix[0] == UINT32_C(4));
    require(prefix[1] == canary);
    fdb_payload_v1_error_release(error);

#if FASTDB_PAYLOAD_TEST_HAS_ASAN
    __asan_poison_memory_region(prefix + 1, sizeof(std::uint32_t));
#endif
    auto* const capabilities =
        reinterpret_cast<fdb_payload_v1_capabilities_t*>(prefix);
    error = nullptr;
    const fdb_payload_v1_status_t capabilities_status =
        fdb_payload_v1_spec_capabilities(nullptr, capabilities, &error);
#if FASTDB_PAYLOAD_TEST_HAS_ASAN
    __asan_unpoison_memory_region(prefix + 1, sizeof(std::uint32_t));
#endif
    require(capabilities_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(error_is(
        error, FDB_PAYLOAD_E_UNSUPPORTED_ABI, "UNSUPPORTED_ABI", "",
        "Unsupported ABI struct contract",
        R"({"argument":"capabilities","minimum_struct_size":72,"reason":"struct_size_too_small","struct_size":4})"));
    require(prefix[0] == UINT32_C(4));
    require(prefix[1] == canary);
    fdb_payload_v1_error_release(error);

    require(::mprotect(static_cast<char*>(pages) + page_size, page_size,
                       PROT_READ | PROT_WRITE) == 0);
    require(::munmap(pages, page_size * 2U) == 0);
    return EXIT_SUCCESS;
}
#else
int test_guarded_short_prefixes_do_not_read_the_tail() { return EXIT_SUCCESS; }
#endif

int test_capability_prefix_tail_and_output_publication() {
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    require(compile(kRichSpec, nullptr, &spec, &error) == UINT32_C(0));

    fdb_payload_v1_capabilities_t capabilities{};
    std::memset(&capabilities, 0xa5, sizeof(capabilities));
    capabilities.struct_size = UINT32_C(4);
    const auto before = capabilities;
    require(fdb_payload_v1_spec_capabilities(spec, &capabilities, &error) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(capabilities.struct_size == UINT32_C(4));
    require(std::memcmp(reinterpret_cast<const std::uint8_t*>(&capabilities) +
                            sizeof(std::uint32_t),
                        reinterpret_cast<const std::uint8_t*>(&before) +
                            sizeof(std::uint32_t),
                        sizeof(capabilities) - sizeof(std::uint32_t)) == 0);
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_capabilities_init(&capabilities);
    capabilities.reserved32 = UINT32_C(1);
    error = nullptr;
    require(fdb_payload_v1_spec_capabilities(spec, &capabilities, &error) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(capabilities.struct_size ==
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
    require(capabilities.profile == UINT32_C(0));
    require(capabilities.semantic_flags == UINT64_C(0));
    require(capabilities.operation_flags == UINT64_C(0));
    require(capabilities.codegen_target_flags == UINT64_C(0));
    require(capabilities.direct_build_status ==
            FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED);
    require(capabilities.reserved32 == UINT32_C(0));
    require(std::all_of(std::begin(capabilities.reserved64),
                        std::end(capabilities.reserved64),
                        [](std::uint64_t value) {
                            return value == UINT64_C(0);
                        }));
    require(error != nullptr && fdb_payload_v1_error_code(error) ==
                                    FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    fdb_payload_v1_error_release(error);

    for (std::size_t reserved = 0; reserved < 4U; ++reserved) {
        fdb_payload_v1_capabilities_init(&capabilities);
        capabilities.reserved64[reserved] = UINT64_C(1);
        error = nullptr;
        require(fdb_payload_v1_spec_capabilities(spec, &capabilities,
                                                 &error) ==
                FDB_PAYLOAD_E_UNSUPPORTED_ABI);
        require(capabilities.profile == UINT32_C(0));
        require(capabilities.semantic_flags == UINT64_C(0));
        require(capabilities.operation_flags == UINT64_C(0));
        require(capabilities.codegen_target_flags == UINT64_C(0));
        require(capabilities.reserved32 == UINT32_C(0));
        require(std::all_of(std::begin(capabilities.reserved64),
                            std::end(capabilities.reserved64),
                            [](std::uint64_t value) {
                                return value == UINT64_C(0);
                            }));
        fdb_payload_v1_error_release(error);
    }

    struct FutureCapabilities final {
        fdb_payload_v1_capabilities_t known;
        std::array<std::uint8_t, 32> future;
    } future{};
    fdb_payload_v1_capabilities_init(&future.known);
    future.known.struct_size = static_cast<std::uint32_t>(sizeof(future));
    future.future.fill(UINT8_C(0x5a));
    const auto expected_tail = future.future;
    error = nullptr;
    require(fdb_payload_v1_spec_capabilities(spec, &future.known, &error) ==
            UINT32_C(0));
    require(future.known.struct_size == sizeof(future));
    require(future.known.profile == FDB_PAYLOAD_PROFILE_RECORD_V1);
    require(future.future == expected_tail);

    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

int test_id_validation_not_found_and_range_errors() {
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    require(compile(kRichSpec, nullptr, &spec, &error) == UINT32_C(0));

    constexpr std::uint8_t embedded_nul[] = {'s', 'i', 'n', 'g', 'l', 'e', 0};
    constexpr std::uint8_t non_ascii[] = {UINT8_C(0xc3), UINT8_C(0xa9)};
    constexpr std::uint8_t bad_first[] = {'7', 'x'};
    constexpr std::uint8_t bad_body[] = {'a', '-'};
    struct InvalidId final {
        const std::uint8_t* data;
        std::uint64_t size;
    } invalid_ids[] = {
        {nullptr, UINT64_C(1)},
        {nullptr, UINT64_C(0)},
        {reinterpret_cast<const std::uint8_t*>(""), UINT64_C(0)},
        {embedded_nul, sizeof(embedded_nul)},
        {non_ascii, sizeof(non_ascii)},
        {bad_first, sizeof(bad_first)},
        {bad_body, sizeof(bad_body)},
    };
    for (const InvalidId& invalid : invalid_ids) {
        std::uint32_t index = UINT32_MAX;
        error = nullptr;
        require(fdb_payload_v1_spec_entry_index(
                    spec, invalid.data, invalid.size, &index, &error) ==
                FDB_PAYLOAD_E_INVALID_ARGUMENT);
        require(index == UINT32_C(0));
        require(error != nullptr && fdb_payload_v1_error_code(error) ==
                                        FDB_PAYLOAD_E_INVALID_ARGUMENT);
        fdb_payload_v1_error_release(error);
    }

    constexpr std::string_view absent = "Single";
    std::uint32_t index = UINT32_MAX;
    error = nullptr;
    require(fdb_payload_v1_spec_entry_index(
                spec, reinterpret_cast<const std::uint8_t*>(absent.data()),
                absent.size(), &index, &error) == FDB_PAYLOAD_E_NOT_FOUND);
    require(index == UINT32_C(0));
    require(error_is(
        error, FDB_PAYLOAD_E_NOT_FOUND, "NOT_FOUND", "",
        "Payload ID was not found",
        R"({"id":"Single","kind":"entry","reason":"not_found"})"));
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_blob_t* id = reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    error = nullptr;
    require(fdb_payload_v1_spec_entry_id(spec, UINT32_C(2), &id, &error) ==
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    require(id == nullptr);
    require(error_is(
        error, FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE, "INDEX_OUT_OF_RANGE", "",
        "Payload index is out of range",
        R"({"collection":"entries","count":2,"index":2,"reason":"index_out_of_range"})"));
    fdb_payload_v1_error_release(error);

    std::uint32_t count = UINT32_MAX;
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_count(
                spec, UINT32_C(2), &count, &error) ==
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    require(count == UINT32_C(0));
    fdb_payload_v1_error_release(error);

    id = reinterpret_cast<fdb_payload_v1_blob_t*>(1);
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_id(
                spec, UINT32_C(0), UINT32_C(3), &id, &error) ==
            FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    require(id == nullptr);
    fdb_payload_v1_error_release(error);

    constexpr std::string_view absent_field = "missing";
    index = UINT32_MAX;
    error = nullptr;
    require(fdb_payload_v1_spec_component_field_index(
                spec, UINT32_C(0),
                reinterpret_cast<const std::uint8_t*>(absent_field.data()),
                absent_field.size(), &index, &error) ==
            FDB_PAYLOAD_E_NOT_FOUND);
    require(index == UINT32_C(0));
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

int test_independent_handle_lifetimes_and_saturation() {
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    require(compile(kRichSpec, nullptr, &spec, &error) == UINT32_C(0));

    fdb_payload_v1_spec_retain(spec);
    fdb_payload_v1_spec_release(spec);
    std::uint32_t count = UINT32_C(0);
    require(fdb_payload_v1_spec_entry_count(spec, &count, &error) ==
            UINT32_C(0));
    require(count == UINT32_C(2));

    fdb_payload_v1_blob_t* canonical = nullptr;
    require(fdb_payload_v1_spec_canonical_json(spec, &canonical, &error) ==
            UINT32_C(0));
    fdb_payload_v1_blob_retain(canonical);
    fdb_payload_v1_blob_release(canonical);
    const std::string canonical_copy(blob_view(canonical));

    std::array<std::uint8_t, 32> digest{};
    require(fdb_payload_v1_spec_sha256(spec, digest.data(), &error) ==
            UINT32_C(0));
    fdb_payload_v1_spec_release(spec);
    require(blob_view(canonical) == canonical_copy);
    const std::array<std::uint8_t, 32> zero_digest{};
    require(digest != zero_digest);
    fdb_payload_v1_blob_release(canonical);

    spec = nullptr;
    error = nullptr;
    require(compile(kDuplicateKey, nullptr, &spec, &error) ==
            FDB_PAYLOAD_E_DUPLICATE_KEY);
    require(spec == nullptr);
    fdb_payload_v1_error_retain(error);
    fdb_payload_v1_error_release(error);
    require(fdb_payload_v1_error_code(error) == FDB_PAYLOAD_E_DUPLICATE_KEY);
    require(error_view(error, fdb_payload_v1_error_path) == "/schema");
    fdb_payload_v1_error_release(error);

    require(compile(kEmptySpec, nullptr, &spec, &error) == UINT32_C(0));
    constexpr std::uint64_t immortal =
        std::numeric_limits<std::uint64_t>::max();
    spec->references.store(immortal - UINT64_C(1), std::memory_order_relaxed);
    fdb_payload_v1_spec_retain(spec);
    require(spec->references.load(std::memory_order_relaxed) == immortal);
    fdb_payload_v1_spec_release(spec);
    require(spec->references.load(std::memory_order_relaxed) == immortal);
    spec->references.store(UINT64_C(1), std::memory_order_relaxed);
    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

int test_allocation_failure_publication_and_retry() {
    fdb_payload_v1_spec_t* warm = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    require(compile(kEmptySpec, nullptr, &warm, &error) == UINT32_C(0));
    fdb_payload_v1_spec_release(warm);
    const std::uint64_t baseline_live =
        live_allocations.load(std::memory_order_relaxed);

    bool reached_success = false;
    std::uint32_t injected_failures = UINT32_C(0);
    for (std::int64_t allocation = 0; allocation < 512; ++allocation) {
        fdb_payload_v1_spec_t* spec =
            reinterpret_cast<fdb_payload_v1_spec_t*>(1);
        error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
        fail_after.store(allocation, std::memory_order_relaxed);
        const fdb_payload_v1_status_t status =
            compile(kEmptySpec, nullptr, &spec, &error);
        disable_failures();
        if (status == UINT32_C(0)) {
            require(spec != nullptr);
            require(error == nullptr);
            fdb_payload_v1_spec_release(spec);
            reached_success = true;
            break;
        }
        ++injected_failures;
        require(spec == nullptr);
        require(error != nullptr);
        require(fdb_payload_v1_error_code(error) == status);
        fdb_payload_v1_error_release(error);
    }
    require(reached_success);
    require(injected_failures > UINT32_C(10));

    fdb_payload_v1_spec_t* spec = nullptr;
    error = nullptr;
    require(compile(kRichSpec, nullptr, &spec, &error) == UINT32_C(0));

    const auto sweep_blob_query = [&](auto&& query) -> int {
        bool query_reached_success = false;
        std::uint32_t query_failures = UINT32_C(0);
        for (std::int64_t allocation = 0; allocation < 2048;
             ++allocation) {
            const std::uint64_t attempt_live =
                live_allocations.load(std::memory_order_relaxed);
            fdb_payload_v1_blob_t* blob =
                reinterpret_cast<fdb_payload_v1_blob_t*>(1);
            error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
            fail_after.store(allocation, std::memory_order_relaxed);
            const fdb_payload_v1_status_t status = query(&blob, &error);
            disable_failures();
            if (status == UINT32_C(0)) {
                require(blob != nullptr);
                require(error == nullptr);
                fdb_payload_v1_blob_release(blob);
                require(live_allocations.load(std::memory_order_relaxed) ==
                        attempt_live);
                query_reached_success = true;
                break;
            }
            ++query_failures;
            require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
            require(blob == nullptr);
            require(error != nullptr &&
                    fdb_payload_v1_error_code(error) == status);
            fdb_payload_v1_error_release(error);
            require(live_allocations.load(std::memory_order_relaxed) ==
                    attempt_live);
        }
        require(query_reached_success);
        require(query_failures >= UINT32_C(2));
        return EXIT_SUCCESS;
    };

    require(sweep_blob_query(
                [&](fdb_payload_v1_blob_t** out_blob,
                    fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_spec_canonical_json(
                        spec, out_blob, out_error);
                }) == EXIT_SUCCESS);
    require(sweep_blob_query(
                [&](fdb_payload_v1_blob_t** out_blob,
                    fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_spec_manifest_json(
                        spec, out_blob, out_error);
                }) == EXIT_SUCCESS);
    require(sweep_blob_query(
                [&](fdb_payload_v1_blob_t** out_blob,
                    fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_spec_entry_id(
                        spec, UINT32_C(0), out_blob, out_error);
                }) == EXIT_SUCCESS);
    require(sweep_blob_query(
                [&](fdb_payload_v1_blob_t** out_blob,
                    fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_spec_component_id(
                        spec, UINT32_C(0), out_blob, out_error);
                }) == EXIT_SUCCESS);
    require(sweep_blob_query(
                [&](fdb_payload_v1_blob_t** out_blob,
                    fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_spec_component_field_id(
                        spec, UINT32_C(0), UINT32_C(0), out_blob, out_error);
                }) == EXIT_SUCCESS);
    require(sweep_blob_query(
                [](fdb_payload_v1_blob_t** out_blob,
                   fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_source_schema_json(out_blob,
                                                             out_error);
                }) == EXIT_SUCCESS);

    bool diagnostic_reached_success = false;
    std::uint32_t diagnostic_failures = UINT32_C(0);
    for (std::int64_t allocation = 0; allocation < 512; ++allocation) {
        const std::uint64_t attempt_live =
            live_allocations.load(std::memory_order_relaxed);
        std::uint32_t count = UINT32_MAX;
        error = reinterpret_cast<fdb_payload_v1_error_t*>(1);
        fail_after.store(allocation, std::memory_order_relaxed);
        const fdb_payload_v1_status_t status =
            fdb_payload_v1_spec_entry_count(nullptr, &count, &error);
        disable_failures();
        require(count == UINT32_C(0));
        require(error != nullptr && fdb_payload_v1_error_code(error) == status);
        if (status == FDB_PAYLOAD_E_INVALID_ARGUMENT) {
            require(error_is(error, FDB_PAYLOAD_E_INVALID_ARGUMENT,
                             "INVALID_ARGUMENT", "", "Invalid ABI argument",
                             R"({"argument":"spec","reason":"null_handle"})"));
            require(error->references.load(std::memory_order_relaxed) ==
                    UINT64_C(1));
            require(error->destroy != nullptr);
            fdb_payload_v1_error_release(error);
            require(live_allocations.load(std::memory_order_relaxed) ==
                    attempt_live);
            diagnostic_reached_success = true;
            break;
        }
        ++diagnostic_failures;
        require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(error->references.load(std::memory_order_relaxed) ==
                std::numeric_limits<std::uint64_t>::max());
        require(error->destroy == nullptr);
        fdb_payload_v1_error_release(error);
        require(live_allocations.load(std::memory_order_relaxed) ==
                attempt_live);
    }
    require(diagnostic_reached_success);
    require(diagnostic_failures >= UINT32_C(2));

    std::array<std::uint8_t, 32> digest{};
    digest.fill(UINT8_C(0xa5));
    fail_forever.store(true, std::memory_order_relaxed);
    fdb_payload_v1_status_t status =
        fdb_payload_v1_source_schema_sha256(digest.data(), &error);
    disable_failures();
    if (status == UINT32_C(0)) {
        require(error == nullptr);
        require(digest == hex_digest(
                              "527209d9820ab21260f56a23befa7a5188feb55e"
                              "9729ea63930425e6226739a1"));
    } else {
        require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(std::all_of(digest.begin(), digest.end(),
                            [](std::uint8_t value) { return value == 0U; }));
        require(error != nullptr && fdb_payload_v1_error_code(error) == status);
        fdb_payload_v1_error_release(error);
    }

    std::uint32_t count = UINT32_MAX;
    fail_forever.store(true, std::memory_order_relaxed);
    status = fdb_payload_v1_spec_entry_count(nullptr, &count, &error);
    disable_failures();
    require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(count == UINT32_C(0));
    require(error != nullptr && fdb_payload_v1_error_code(error) == status);
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_profile_t profile = UINT32_MAX;
    fail_forever.store(true, std::memory_order_relaxed);
    status = fdb_payload_v1_spec_profile(nullptr, &profile, &error);
    disable_failures();
    require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(profile == UINT32_C(0));
    fdb_payload_v1_error_release(error);

    fdb_payload_v1_capabilities_t capabilities{};
    fdb_payload_v1_capabilities_init(&capabilities);
    fail_forever.store(true, std::memory_order_relaxed);
    status = fdb_payload_v1_spec_capabilities(
        nullptr, &capabilities, &error);
    disable_failures();
    require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
    require(capabilities.struct_size ==
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE);
    require(capabilities.profile == UINT32_C(0));
    fdb_payload_v1_error_release(error);

    digest.fill(UINT8_C(0));
    error = nullptr;
    fail_forever.store(true, std::memory_order_relaxed);
    status = fdb_payload_v1_spec_sha256(spec, digest.data(), &error);
    disable_failures();
    require(status == UINT32_C(0));
    require(error == nullptr);
    const std::array<std::uint8_t, 32> zero_digest{};
    require(digest != zero_digest);

    fdb_payload_v1_capabilities_init(&capabilities);
    fail_forever.store(true, std::memory_order_relaxed);
    status = fdb_payload_v1_spec_capabilities(spec, &capabilities, &error);
    disable_failures();
    require(status == UINT32_C(0));
    require(error == nullptr);
    require(capabilities.semantic_flags == UINT64_C(27));

    count = UINT32_C(0);
    error = nullptr;
    require(fdb_payload_v1_spec_entry_count(spec, &count, &error) ==
            UINT32_C(0));
    require(count == UINT32_C(2));
    fdb_payload_v1_spec_release(spec);
    require(live_allocations.load(std::memory_order_relaxed) == baseline_live);
    return EXIT_SUCCESS;
}

int test_sixteen_thread_query_consistency() {
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    require(compile(kRichSpec, nullptr, &spec, &error) == UINT32_C(0));

    fdb_payload_v1_blob_t* canonical_blob = nullptr;
    fdb_payload_v1_blob_t* manifest_blob = nullptr;
    require(fdb_payload_v1_spec_canonical_json(spec, &canonical_blob, &error) ==
            UINT32_C(0));
    require(fdb_payload_v1_spec_manifest_json(spec, &manifest_blob, &error) ==
            UINT32_C(0));
    const std::string canonical(blob_view(canonical_blob));
    const std::string manifest(blob_view(manifest_blob));
    fdb_payload_v1_blob_release(canonical_blob);
    fdb_payload_v1_blob_release(manifest_blob);
    std::array<std::uint8_t, 32> digest{};
    require(fdb_payload_v1_spec_sha256(spec, digest.data(), &error) ==
            UINT32_C(0));

    std::atomic<bool> consistent{true};
    std::vector<std::thread> workers;
    workers.reserve(16U);
    for (std::uint32_t worker = 0; worker < UINT32_C(16); ++worker) {
        fdb_payload_v1_spec_retain(spec);
        workers.emplace_back([spec, &canonical, &manifest, digest, &consistent]() {
            for (std::uint32_t iteration = 0; iteration < UINT32_C(100);
                 ++iteration) {
                fdb_payload_v1_error_t* local_error = nullptr;
                fdb_payload_v1_blob_t* first = nullptr;
                fdb_payload_v1_blob_t* second = nullptr;
                std::array<std::uint8_t, 32> observed{};
                fdb_payload_v1_profile_t profile = UINT32_C(0);
                fdb_payload_v1_capabilities_t capabilities{};
                fdb_payload_v1_capabilities_init(&capabilities);
                std::uint32_t count = UINT32_C(0);
                bool ok = true;
                if (fdb_payload_v1_spec_canonical_json(
                        spec, &first, &local_error) != UINT32_C(0) ||
                    blob_view(first) != canonical) {
                    ok = false;
                }
                if (ok &&
                    (fdb_payload_v1_spec_manifest_json(
                        spec, &second, &local_error) != UINT32_C(0) ||
                     blob_view(second) != manifest)) {
                    ok = false;
                }
                if (ok &&
                    (fdb_payload_v1_spec_sha256(
                        spec, observed.data(), &local_error) != UINT32_C(0) ||
                     observed != digest)) {
                    ok = false;
                }
                if (ok &&
                    (fdb_payload_v1_spec_profile(
                        spec, &profile, &local_error) != UINT32_C(0) ||
                     profile != FDB_PAYLOAD_PROFILE_RECORD_V1)) {
                    ok = false;
                }
                if (ok &&
                    (fdb_payload_v1_spec_capabilities(
                        spec, &capabilities, &local_error) != UINT32_C(0) ||
                     capabilities.struct_size !=
                         FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE ||
                     capabilities.profile != FDB_PAYLOAD_PROFILE_RECORD_V1 ||
                     capabilities.semantic_flags != UINT64_C(27) ||
                     capabilities.operation_flags != UINT64_C(3) ||
                     capabilities.codegen_target_flags != UINT64_C(0) ||
                     capabilities.direct_build_status !=
                         FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED)) {
                    ok = false;
                }
                if (ok &&
                    (fdb_payload_v1_spec_entry_count(
                        spec, &count, &local_error) != UINT32_C(0) ||
                     count != UINT32_C(2))) {
                    ok = false;
                }

                constexpr std::string_view expected_entry_ids[] = {
                    "single", "series"};
                for (std::uint32_t entry = UINT32_C(0);
                     ok && entry < UINT32_C(2); ++entry) {
                    fdb_payload_v1_blob_t* entry_id = nullptr;
                    if (fdb_payload_v1_spec_entry_id(
                            spec, entry, &entry_id, &local_error) !=
                            UINT32_C(0) ||
                        blob_view(entry_id) != expected_entry_ids[entry]) {
                        ok = false;
                    } else {
                        std::uint32_t index = UINT32_MAX;
                        if (fdb_payload_v1_spec_entry_index(
                                spec, fdb_payload_v1_blob_data(entry_id),
                                fdb_payload_v1_blob_size(entry_id), &index,
                                &local_error) != UINT32_C(0) ||
                            index != entry) {
                            ok = false;
                        }
                    }
                    fdb_payload_v1_blob_release(entry_id);
                }

                if (ok &&
                    (fdb_payload_v1_spec_component_count(
                        spec, &count, &local_error) != UINT32_C(0) ||
                     count != UINT32_C(2))) {
                    ok = false;
                }

                constexpr std::string_view expected_component_ids[] = {
                    "AllTypes", "Leaf"};
                constexpr std::uint32_t expected_field_counts[] = {
                    UINT32_C(3), UINT32_C(0)};
                constexpr std::string_view expected_all_types_fields[] = {
                    "value", "label", "leaf"};
                for (std::uint32_t component = UINT32_C(0);
                     ok && component < UINT32_C(2); ++component) {
                    fdb_payload_v1_blob_t* component_id = nullptr;
                    if (fdb_payload_v1_spec_component_id(
                            spec, component, &component_id, &local_error) !=
                            UINT32_C(0) ||
                        blob_view(component_id) !=
                            expected_component_ids[component]) {
                        ok = false;
                    } else {
                        std::uint32_t index = UINT32_MAX;
                        if (fdb_payload_v1_spec_component_index(
                                spec, fdb_payload_v1_blob_data(component_id),
                                fdb_payload_v1_blob_size(component_id), &index,
                                &local_error) != UINT32_C(0) ||
                            index != component) {
                            ok = false;
                        }
                    }
                    fdb_payload_v1_blob_release(component_id);

                    std::uint32_t field_count = UINT32_MAX;
                    if (ok &&
                        (fdb_payload_v1_spec_component_field_count(
                             spec, component, &field_count, &local_error) !=
                             UINT32_C(0) ||
                         field_count != expected_field_counts[component])) {
                        ok = false;
                    }
                    for (std::uint32_t field = UINT32_C(0);
                         ok && field < field_count; ++field) {
                        fdb_payload_v1_blob_t* field_id = nullptr;
                        if (fdb_payload_v1_spec_component_field_id(
                                spec, component, field, &field_id,
                                &local_error) != UINT32_C(0) ||
                            blob_view(field_id) !=
                                expected_all_types_fields[field]) {
                            ok = false;
                        } else {
                            std::uint32_t index = UINT32_MAX;
                            if (fdb_payload_v1_spec_component_field_index(
                                    spec, component,
                                    fdb_payload_v1_blob_data(field_id),
                                    fdb_payload_v1_blob_size(field_id), &index,
                                    &local_error) != UINT32_C(0) ||
                                index != field) {
                                ok = false;
                            }
                        }
                        fdb_payload_v1_blob_release(field_id);
                    }
                }

                if (local_error != nullptr) {
                    ok = false;
                }
                if (!ok) {
                    consistent.store(false, std::memory_order_relaxed);
                }
                fdb_payload_v1_blob_release(first);
                fdb_payload_v1_blob_release(second);
                fdb_payload_v1_error_release(local_error);
            }
            fdb_payload_v1_spec_release(spec);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(consistent.load(std::memory_order_relaxed));
    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

}  // namespace

void* operator new(std::size_t size) { return allocate_for_test(size); }
void* operator new[](std::size_t size) { return allocate_for_test(size); }
void operator delete(void* value) noexcept { deallocate_for_test(value); }
void operator delete[](void* value) noexcept { deallocate_for_test(value); }
void operator delete(void* value, std::size_t) noexcept {
    deallocate_for_test(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    deallocate_for_test(value);
}

int main() {
    require(test_compile_and_every_query_round_trip() == EXIT_SUCCESS);
    require(test_nullability_output_clearing_and_exact_errors() == EXIT_SUCCESS);
    require(test_null_error_sink_preserves_every_value_output() ==
            EXIT_SUCCESS);
    require(test_every_required_output_and_null_handle_path_clears() ==
            EXIT_SUCCESS);
    require(test_options_prefix_tail_reserved_and_limits() == EXIT_SUCCESS);
    require(test_guarded_short_prefixes_do_not_read_the_tail() == EXIT_SUCCESS);
    require(test_capability_prefix_tail_and_output_publication() == EXIT_SUCCESS);
    require(test_id_validation_not_found_and_range_errors() == EXIT_SUCCESS);
    require(test_independent_handle_lifetimes_and_saturation() == EXIT_SUCCESS);
    require(test_allocation_failure_publication_and_retry() == EXIT_SUCCESS);
    require(test_sixteen_thread_query_consistency() == EXIT_SUCCESS);
    disable_failures();
    return EXIT_SUCCESS;
}
