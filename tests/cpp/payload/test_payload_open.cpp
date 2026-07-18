#include "TestSupport.hpp"
#include "GoldenCorpus.hpp"

#if __has_include("payload/view/PayloadOwner.hpp")
#include "payload/backing/Backing.hpp"
#include "payload/build/BuildPlan.hpp"
#include "payload/build/PayloadBuilder.hpp"
#include "payload/error/Result.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Open.hpp"
#include "payload/view/PayloadOwner.hpp"
#define FASTDB_TASK7_HAS_PAYLOAD_OWNER 1
#else
#define FASTDB_TASK7_HAS_PAYLOAD_OWNER 0
#endif

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocation_failure {

std::atomic<std::int64_t> fail_after{INT64_C(-1)};
std::atomic<std::size_t> fail_exact_size{0U};
std::atomic<bool> postcommit_only{false};
std::atomic<bool> postcommit_phase{false};

struct AllocationHeader final {
    void* raw;
};

void* allocate(std::size_t size,
               std::size_t alignment = alignof(std::max_align_t)) {
    const bool armed = !postcommit_only.load(std::memory_order_relaxed) ||
                       postcommit_phase.load(std::memory_order_relaxed);
    if (armed && fail_exact_size.load(std::memory_order_relaxed) == size &&
        size != 0U) {
        throw std::bad_alloc();
    }
    const std::int64_t remaining = fail_after.load(std::memory_order_relaxed);
    if (armed && remaining >= INT64_C(0) &&
        fail_after.fetch_sub(INT64_C(1), std::memory_order_relaxed) ==
            INT64_C(0)) {
        throw std::bad_alloc();
    }
    alignment = std::max(alignment, alignof(AllocationHeader));
    const std::size_t payload = size == 0U ? 1U : size;
    if (payload > std::numeric_limits<std::size_t>::max() -
                      sizeof(AllocationHeader) - (alignment - 1U)) {
        throw std::bad_alloc();
    }
    void* const raw =
        std::malloc(payload + sizeof(AllocationHeader) + alignment - 1U);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    const std::uintptr_t begin =
        reinterpret_cast<std::uintptr_t>(raw) + sizeof(AllocationHeader);
    const std::uintptr_t aligned =
        (begin + alignment - 1U) & ~(alignment - 1U);
    (reinterpret_cast<AllocationHeader*>(aligned) - 1)->raw = raw;
    return reinterpret_cast<void*>(aligned);
}

void deallocate(void* value) noexcept {
    if (value != nullptr) {
        std::free((reinterpret_cast<AllocationHeader*>(value) - 1)->raw);
    }
}

struct InjectionReset final {
    ~InjectionReset() {
        fail_after.store(INT64_C(-1), std::memory_order_relaxed);
        fail_exact_size.store(0U, std::memory_order_relaxed);
        postcommit_only.store(false, std::memory_order_relaxed);
        postcommit_phase.store(false, std::memory_order_relaxed);
    }
};

}  // namespace allocation_failure

void* operator new(std::size_t size) {
    return allocation_failure::allocate(size);
}
void* operator new[](std::size_t size) {
    return allocation_failure::allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(size,
        static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(size,
        static_cast<std::size_t>(alignment));
}
void operator delete(void* value) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::size_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::size_t, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::size_t,
                       std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}

#if FASTDB_TASK7_HAS_PAYLOAD_OWNER
namespace fastdb::payload::view {

struct PayloadOwnerTestAccess final {
    static std::vector<std::uint8_t> copy_bytes(const PayloadOwner& owner) {
        const std::uint8_t* const begin =
            owner.state_->backing.readable_data();
        const std::uint64_t size = owner.state_->backing.readable_size();
        return std::vector<std::uint8_t>(
            begin, begin + static_cast<std::ptrdiff_t>(size));
    }

    static const void* state_identity(const PayloadOwner& owner) noexcept {
        return owner.state_.get();
    }
};

}  // namespace fastdb::payload::view

namespace fastdb::payload::build {

struct BuildPlanTestAccess final {
    static view::OpenOptions publication_options(
        const BuildPlan& plan,
        const PlanInfo& synthetic_info) {
        return plan.publication_options(synthetic_info);
    }
};

}  // namespace fastdb::payload::build
#endif

namespace {

enum class MatrixArea : std::uint8_t {
    api_cut,
    copy_open,
    external_open,
    plan_publication,
    malformed_open,
    resource_limits,
    allocation_cleanup,
};

struct MatrixCase final {
    MatrixArea area;
    std::string_view name;
};

constexpr std::array<MatrixCase, 34> task7_matrix{{
    {MatrixArea::api_cut, "OpenOptions is the only private open-options name"},
    {MatrixArea::api_cut, "BuildPlan execute returns PayloadOwner"},
    {MatrixArea::api_cut, "PendingPayload callable seam is removed"},
    {MatrixArea::api_cut, "owner has no unpinned byte accessor"},
    {MatrixArea::copy_open, "copy preflights pointer length and total limit"},
    {MatrixArea::copy_open, "copy validates the owned image"},
    {MatrixArea::copy_open, "copy survives source mutation and release"},
    {MatrixArea::external_open, "retain occurs before validation"},
    {MatrixArea::external_open, "retain allocation failure has no release"},
    {MatrixArea::external_open, "unknown retain status is backing contract"},
    {MatrixArea::external_open, "retained span mismatch releases once"},
    {MatrixArea::external_open, "validation failure releases once"},
    {MatrixArea::external_open, "unaligned base has identical meaning"},
    {MatrixArea::external_open, "null owner token remains legal"},
    {MatrixArea::external_open, "owner copies share one retained reference"},
    {MatrixArea::plan_publication, "direct execution adopts without retain"},
    {MatrixArea::plan_publication, "staged execution reports exact history"},
    {MatrixArea::plan_publication, "opened owner has no fabricated report"},
    {MatrixArea::plan_publication, "plan and source lifetime are independent"},
    {MatrixArea::plan_publication, "commit corruption is backing contract"},
    {MatrixArea::plan_publication, "wrong commit span releases never rolls back"},
    {MatrixArea::malformed_open, "physical paths are rooted at binary"},
    {MatrixArea::malformed_open, "header and directory fail at first path"},
    {MatrixArea::malformed_open, "entry and region descriptors fail at first path"},
    {MatrixArea::malformed_open, "validity null padding and scalar bytes are canonical"},
    {MatrixArea::malformed_open, "list and pool partitions consume exactly"},
    {MatrixArea::malformed_open, "eager text validates before publication"},
    {MatrixArea::malformed_open, "lazy text defers only content validation"},
    {MatrixArea::resource_limits, "all nine safe defaults are frozen"},
    {MatrixArea::resource_limits, "total regions entries components reject early"},
    {MatrixArea::resource_limits, "depth lists strings and work reject before read"},
    {MatrixArea::resource_limits, "validation work exact boundary succeeds"},
    {MatrixArea::allocation_cleanup, "reader bad alloc and length error are contained"},
    {MatrixArea::allocation_cleanup, "first State allocation releases after commit"},
}};

int verify_matrix_inventory() {
    std::array<bool, 7> covered{};
    for (const MatrixCase& item : task7_matrix) {
        require(!item.name.empty());
        covered[static_cast<std::size_t>(item.area)] = true;
    }
    require(std::all_of(covered.begin(), covered.end(),
                        [](bool value) { return value; }));
    return EXIT_SUCCESS;
}

int verify_golden_inventory() {
    const auto corpus =
        fastdb::test::payload::load_binary_open_golden_corpus(
            FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    require(corpus.size() == 13U);
    std::size_t success_count = 0U;
    std::size_t invalid_count = 0U;
    for (const auto& item : corpus) {
        require(!item.name.empty());
        if (std::holds_alternative<
                fastdb::test::payload::BinaryGoldenSuccess>(item.expected)) {
            ++success_count;
            continue;
        }
        ++invalid_count;
        const auto& invalid =
            std::get<fastdb::test::payload::BinaryGoldenInvalid>(
                item.expected);
        require(!invalid.source.empty());
        require(!invalid.binary_hex.empty());
        require(!invalid.expectation_relative_path.empty());
        require(invalid.expected.path.rfind("/binary/", 0U) == 0U);
        require(invalid.expected.details_json.front() == '{');
        require(invalid.expected.details_json.back() == '}');
    }
    require(success_count == 7U);
    require(invalid_count == 6U);
    require(corpus[7].name == "invalid-header-magic");
    require(corpus[8].name == "invalid-header-reserved");
    require(corpus[9].name == "invalid-directory-offset");
    require(corpus[10].name == "invalid-resource-total");
    require(corpus[11].name == "invalid-region-flags");
    require(corpus[12].name == "invalid-entry-index");
    return EXIT_SUCCESS;
}

#if FASTDB_TASK7_HAS_PAYLOAD_OWNER

using fastdb::payload::backing::Callbacks;
using fastdb::payload::backing::RetainedBacking;
using fastdb::payload::build::BuildPlan;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::spec::CompiledSpec;
using fastdb::payload::view::OpenOptions;
using fastdb::payload::view::PayloadOwner;

constexpr std::uint32_t allow_staging = UINT32_C(1);
constexpr std::uint32_t require_direct = UINT32_C(2);
constexpr std::uint32_t direct_mode = UINT32_C(1);
constexpr std::uint32_t staged_mode = UINT32_C(2);
constexpr std::uint32_t success_status = UINT32_C(0);
constexpr std::uint32_t unknown_status = UINT32_C(0x6a17f00d);

void fixture_require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error("Malformed Task 7 test fixture: " +
                                 std::string(reason));
    }
}

using ExecuteResult = decltype(std::declval<const BuildPlan&>().execute(
    require_direct, static_cast<const Callbacks*>(nullptr)));
static_assert(std::is_same_v<ExecuteResult, Result<PayloadOwner>>,
              "Task 7 removes PendingPayload and publishes PayloadOwner");
static_assert(std::is_copy_constructible_v<PayloadOwner>);
static_assert(std::is_copy_assignable_v<PayloadOwner>);
static_assert(std::is_move_constructible_v<PayloadOwner>);
static_assert(!std::is_copy_constructible_v<RetainedBacking>);
static_assert(std::is_move_constructible_v<RetainedBacking>);

template <typename T, typename = void>
struct HasReadableData : std::false_type {};

template <typename T>
struct HasReadableData<
    T, std::void_t<decltype(std::declval<const T&>().readable_data())>>
    : std::true_type {};

static_assert(!HasReadableData<PayloadOwner>::value,
              "Task 7 must not add an unpinned byte accessor");

std::string load_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    fixture_require(static_cast<bool>(input), "cannot open file");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::uint8_t hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    std::abort();
}

std::vector<std::uint8_t> decode_hex(std::string text) {
    fixture_require(text.size() % 2U == 0U, "odd hexadecimal length");
    std::vector<std::uint8_t> bytes(text.size() / 2U);
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            (hex_digit(text[index * 2U]) << 4U) |
            hex_digit(text[index * 2U + 1U]));
    }
    return bytes;
}

std::vector<std::uint8_t> load_hex(const std::string& relative) {
    std::string text = load_file(
        std::string(FASTDB_PAYLOAD_BINARY_FIXTURE_DIR) + "/" + relative);
    fixture_require(!text.empty(), "empty hexadecimal file");
    fixture_require(text.back() == '\n', "hexadecimal file lacks newline");
    text.pop_back();
    return decode_hex(std::move(text));
}

Result<CompiledSpec> compile_fixture(const std::string& relative) {
    const std::string source = load_file(
        std::string(FASTDB_PAYLOAD_BINARY_FIXTURE_DIR) + "/" + relative);
    return CompiledSpec::compile(source);
}

struct FakeBacking final {
    explicit FakeBacking(std::uint64_t capacity)
        : storage(static_cast<std::size_t>(capacity) + 8U, UINT8_C(0)) {}

    Callbacks callbacks() noexcept {
        return Callbacks{this, &reserve_callback, &write_callback,
                         &commit_callback, &rollback_callback,
                         &retain_callback, &release_callback};
    }

    void record(char event) noexcept {
        if (event_count < events.size()) {
            events[event_count++] = event;
        }
    }

    static std::uint32_t reserve_callback(
        void* context, std::uint32_t mode, std::uint64_t minimum_capacity,
        std::uint32_t, void** token, std::uint8_t** writable,
        std::uint64_t* capacity) {
        auto& self = *static_cast<FakeBacking*>(context);
        self.record('V');
        ++self.reserve_count;
        if (self.decline_first_direct && mode == direct_mode &&
            self.reserve_count == UINT32_C(1)) {
            return FDB_PAYLOAD_E_DIRECT_UNAVAILABLE;
        }
        *token = self.null_token ? nullptr : &self.token;
        *writable = self.storage.data() + self.base_offset;
        *capacity = static_cast<std::uint64_t>(self.storage.size()) -
                    self.base_offset;
        return minimum_capacity <= *capacity
                   ? success_status
                   : FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }

    static std::uint32_t write_callback(void* context, void*,
                                        std::uint64_t offset,
                                        const std::uint8_t* source,
                                        std::uint64_t size) {
        auto& self = *static_cast<FakeBacking*>(context);
        self.record('W');
        ++self.write_count;
        const std::uint64_t storage_size = self.storage.size();
        if (self.base_offset > storage_size ||
            offset > storage_size - self.base_offset ||
            size > storage_size - self.base_offset - offset ||
            (source == nullptr && size != UINT64_C(0))) {
            return FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        if (size != UINT64_C(0)) {
            std::copy(source, source + static_cast<std::ptrdiff_t>(size),
                      self.storage.data() + self.base_offset + offset);
        }
        return success_status;
    }

    static std::uint32_t commit_callback(
        void* context, void*, std::uint64_t used,
        const std::uint8_t** readable, std::uint64_t* readable_size) {
        auto& self = *static_cast<FakeBacking*>(context);
        self.record('C');
        ++self.commit_count;
        if (self.corrupt_magic && used != UINT64_C(0)) {
            self.storage[self.base_offset] ^= UINT8_C(0xff);
        }
        *readable = self.storage.data() + self.base_offset +
                    (self.wrong_readable_pointer ? 1U : 0U);
        *readable_size = self.short_commit && used != UINT64_C(0)
                             ? used - UINT64_C(1)
                             : used;
        allocation_failure::postcommit_phase.store(
            true, std::memory_order_relaxed);
        return success_status;
    }

    static std::uint32_t rollback_callback(void* context, void*) {
        auto& self = *static_cast<FakeBacking*>(context);
        self.record('B');
        ++self.rollback_count;
        return success_status;
    }

    static std::uint32_t retain_callback(void* context, void*) {
        auto& self = *static_cast<FakeBacking*>(context);
        self.record('T');
        ++self.retain_count;
        return self.retain_status;
    }

    static void release_callback(void* context, void*) {
        auto& self = *static_cast<FakeBacking*>(context);
        self.record('L');
        ++self.release_count;
    }

    std::vector<std::uint8_t> storage;
    std::uint8_t token{UINT8_C(0)};
    std::uint64_t base_offset{UINT64_C(0)};
    std::uint32_t retain_status{success_status};
    std::uint32_t reserve_count{UINT32_C(0)};
    std::uint32_t write_count{UINT32_C(0)};
    std::uint32_t commit_count{UINT32_C(0)};
    std::uint32_t rollback_count{UINT32_C(0)};
    std::uint32_t retain_count{UINT32_C(0)};
    std::uint32_t release_count{UINT32_C(0)};
    std::array<char, 16> events{};
    std::size_t event_count{0U};
    bool decline_first_direct{false};
    bool corrupt_magic{false};
    bool short_commit{false};
    bool wrong_readable_pointer{false};
    bool null_token{false};
};

Result<BuildPlan> make_empty_plan() {
    constexpr std::string_view source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]})";
    auto compiled = CompiledSpec::compile(source);
    if (!compiled.has_value()) {
        return Result<BuildPlan>::failure(std::move(compiled).error());
    }
    auto builder = PayloadBuilder::create(std::move(compiled).value());
    if (!builder.has_value()) {
        return Result<BuildPlan>::failure(std::move(builder).error());
    }
    return builder.value().freeze_plan();
}

PayloadOwner make_owner_with_local_plan() {
    auto local_plan = make_empty_plan();
    fixture_require(local_plan.has_value(), "local plan creation failed");
    auto local_owner = local_plan.value().execute(require_direct, nullptr);
    fixture_require(local_owner.has_value(), "local plan execution failed");
    return std::move(local_owner).value();
}

int verify_clean_api_cut() {
    const OpenOptions defaults = fastdb::payload::view::default_open_options();
    require(defaults.validate_text_eager);
    require(defaults.max_total_bytes == (UINT64_C(1) << 30));
    require(defaults.max_regions == UINT64_C(1000000));
    require(defaults.max_entries == UINT64_C(65536));
    require(defaults.max_components == UINT64_C(65536));
    require(defaults.max_nesting_depth == UINT64_C(1024));
    require(defaults.max_list_elements == UINT64_C(10000000));
    require(defaults.max_graph_objects == UINT64_C(10000000));
    require(defaults.max_string_bytes == (UINT64_C(1) << 30));
    require(defaults.max_validation_work == UINT64_C(100000000));

    const std::string open_header = load_file(
        std::string(FASTDB_PAYLOAD_CORE_SOURCE_DIR) + "/view/Open.hpp");
    const std::string plan_header = load_file(
        std::string(FASTDB_PAYLOAD_CORE_SOURCE_DIR) + "/build/BuildPlan.hpp");
    const std::string open_source = load_file(
        std::string(FASTDB_PAYLOAD_CORE_SOURCE_DIR) + "/view/Open.cpp");
    require(open_header.find("OpenLimits") == std::string::npos);
    require(open_header.find("default_open_limits") == std::string::npos);
    require(plan_header.find("PendingPayload") == std::string::npos);
    require(open_source.find("catch (const std::length_error&)") !=
            std::string::npos);
    require(open_source.find("catch (...)") == std::string::npos);
    return EXIT_SUCCESS;
}

int test_copy_open_and_physical_diagnostics() {
    auto compiled = compile_fixture("spec/empty.source.json");
    require(compiled.has_value());
    const auto expected_digest = compiled.value().digest();
    std::vector<std::uint8_t> bytes = load_hex("valid/empty.bin.hex");
    const std::vector<std::uint8_t> expected_bytes = bytes;
    auto opened = PayloadOwner::open_copy(
        std::move(compiled).value(), bytes.data(), bytes.size());
    require(opened.has_value(), opened.has_value()
                                    ? std::string_view{}
                                    : opened.error().details_json());
    require(opened.value().digest() == expected_digest);
    require(opened.value().profile() ==
            fastdb::payload::spec::Profile::record_v1);
    require(!opened.value().execution_report().has_value());
    std::fill(bytes.begin(), bytes.end(), UINT8_C(0xff));
    require(opened.value().digest() == expected_digest);
    require(fastdb::payload::view::PayloadOwnerTestAccess::copy_bytes(
                opened.value()) == expected_bytes);
    PayloadOwner shared = opened.value();
    require(shared.digest() == expected_digest);
    require(fastdb::payload::view::PayloadOwnerTestAccess::state_identity(
                shared) ==
            fastdb::payload::view::PayloadOwnerTestAccess::state_identity(
                opened.value()));

    OpenOptions preflight = fastdb::payload::view::default_open_options();
    preflight.max_total_bytes = UINT64_C(128);
    auto oversized_spec = compile_fixture("spec/empty.source.json");
    require(oversized_spec.has_value());
    allocation_failure::InjectionReset reset;
    allocation_failure::fail_exact_size.store(129U,
                                              std::memory_order_relaxed);
    auto oversized = PayloadOwner::open_copy(
        std::move(oversized_spec).value(),
        reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX), UINT64_C(129),
        preflight);
    allocation_failure::fail_exact_size.store(0U,
                                              std::memory_order_relaxed);
    require(!oversized.has_value());
    require(oversized.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(oversized.error().path() == "/binary/header");
    require(oversized.error().details_json() ==
            "{\"actual\":\"129\",\"limit\":\"128\",\"resource\":\"total_bytes\"}");

    auto null_spec = compile_fixture("spec/empty.source.json");
    require(null_spec.has_value());
    auto null_source = PayloadOwner::open_copy(
        std::move(null_spec).value(), nullptr, expected_bytes.size());
    require(!null_source.has_value());
    require(null_source.error().code() == FDB_PAYLOAD_E_OUT_OF_BOUNDS);
    require(null_source.error().path() == "/binary/header");

    return EXIT_SUCCESS;
}

void apply_options(
    OpenOptions& target,
    const fastdb::test::payload::BinaryOpenOptions& source) {
    if (source.validate_text_eager) {
        target.validate_text_eager = *source.validate_text_eager;
    }
#define FASTDB_APPLY_OPEN_LIMIT(name) \
    if (source.name) {                  \
        target.name = *source.name;     \
    }
    FASTDB_APPLY_OPEN_LIMIT(max_total_bytes)
    FASTDB_APPLY_OPEN_LIMIT(max_regions)
    FASTDB_APPLY_OPEN_LIMIT(max_entries)
    FASTDB_APPLY_OPEN_LIMIT(max_components)
    FASTDB_APPLY_OPEN_LIMIT(max_nesting_depth)
    FASTDB_APPLY_OPEN_LIMIT(max_list_elements)
    FASTDB_APPLY_OPEN_LIMIT(max_graph_objects)
    FASTDB_APPLY_OPEN_LIMIT(max_string_bytes)
    FASTDB_APPLY_OPEN_LIMIT(max_validation_work)
#undef FASTDB_APPLY_OPEN_LIMIT
}

int test_invalid_golden_corpus() {
    const auto corpus =
        fastdb::test::payload::load_binary_open_golden_corpus(
            FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    std::size_t checked = 0U;
    for (const auto& item : corpus) {
        const auto* invalid =
            std::get_if<fastdb::test::payload::BinaryGoldenInvalid>(
                &item.expected);
        if (invalid == nullptr) {
            continue;
        }
        auto compiled = CompiledSpec::compile(invalid->source);
        require(compiled.has_value());
        auto bytes = decode_hex(invalid->binary_hex);
        OpenOptions options = fastdb::payload::view::default_open_options();
        apply_options(options, invalid->options);
        auto result = PayloadOwner::open_copy(
            std::move(compiled).value(), bytes.data(), bytes.size(), options);
        require(!result.has_value());
        require(result.error().code() == invalid->expected.status);
        require(result.error().symbol() == invalid->expected.symbol);
        require(result.error().path() == invalid->expected.path);
        require(result.error().message() == invalid->expected.message);
        require(result.error().details_json() ==
                invalid->expected.details_json);
        ++checked;
    }
    require(checked == 6U);
    return EXIT_SUCCESS;
}

int test_external_open_ownership() {
    auto bytes = load_hex("valid/empty.bin.hex");
    auto compiled = compile_fixture("spec/empty.source.json");
    require(compiled.has_value());
    for (const bool remove_retain : {false, true}) {
        FakeBacking missing(bytes.size());
        Callbacks missing_callbacks = missing.callbacks();
        if (remove_retain) {
            missing_callbacks.retain = nullptr;
        } else {
            missing_callbacks.release = nullptr;
        }
        auto rejected = RetainedBacking::acquire(
            missing_callbacks, &missing.token, missing.storage.data(),
            bytes.size());
        require(!rejected.has_value());
        require(rejected.error().code() == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(rejected.error().path() == "/backing");
        require(rejected.error().details_json() ==
                "{\"reason\":\"missing_retain_or_release\"}");
        require(missing.retain_count == UINT32_C(0));
        require(missing.release_count == UINT32_C(0));
    }
    FakeBacking backing(bytes.size());
    std::copy(bytes.begin(), bytes.end(), backing.storage.begin());
    Callbacks callbacks = backing.callbacks();
    auto retained = RetainedBacking::acquire(
        callbacks, &backing.token, backing.storage.data(), bytes.size());
    require(retained.has_value());
    require(backing.retain_count == UINT32_C(1));
    {
        auto opened = PayloadOwner::open_external(
            std::move(compiled).value(), backing.storage.data(), bytes.size(),
            std::move(retained).value());
        require(opened.has_value());
        require(!opened.value().execution_report().has_value());
        require(backing.release_count == UINT32_C(0));
        PayloadOwner first = opened.value();
        PayloadOwner second = first;
        require(fastdb::payload::view::PayloadOwnerTestAccess::state_identity(
                    first) ==
                fastdb::payload::view::PayloadOwnerTestAccess::state_identity(
                    second));
        require(backing.retain_count == UINT32_C(1));
    }
    require(backing.release_count == UINT32_C(1));

    auto mismatch_spec = compile_fixture("spec/empty.source.json");
    require(mismatch_spec.has_value());
    FakeBacking mismatch(bytes.size() + UINT64_C(1));
    std::copy(bytes.begin(), bytes.end(), mismatch.storage.begin());
    auto mismatch_retained = RetainedBacking::acquire(
        mismatch.callbacks(), &mismatch.token, mismatch.storage.data(),
        bytes.size());
    require(mismatch_retained.has_value());
    auto mismatch_result = PayloadOwner::open_external(
        std::move(mismatch_spec).value(), mismatch.storage.data() + 1,
        bytes.size(), std::move(mismatch_retained).value());
    require(!mismatch_result.has_value());
    require(mismatch_result.error().code() == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(mismatch_result.error().path() == "/backing");
    require(mismatch_result.error().details_json() ==
            "{\"reason\":\"retained_span_mismatch\"}");
    require(mismatch.release_count == UINT32_C(1));

    for (const std::uint32_t status :
         {FDB_PAYLOAD_E_ALLOCATION_FAILED, unknown_status}) {
        FakeBacking failed(bytes.size());
        failed.retain_status = status;
        auto failed_retain = RetainedBacking::acquire(
            failed.callbacks(), &failed.token, failed.storage.data(),
            bytes.size());
        require(!failed_retain.has_value());
        require(failed_retain.error().code() ==
                (status == FDB_PAYLOAD_E_ALLOCATION_FAILED
                     ? FDB_PAYLOAD_E_ALLOCATION_FAILED
                     : FDB_PAYLOAD_E_BACKING_CONTRACT));
        require(failed.retain_count == UINT32_C(1));
        require(failed.release_count == UINT32_C(0));
        require(failed_retain.error().path() == "/backing");
        if (status == unknown_status) {
            require(failed_retain.error().details_json() ==
                    "{\"callback\":\"retain\",\"callback_status\":1779953677}");
        }
    }

    auto malformed_spec = compile_fixture("spec/empty.source.json");
    require(malformed_spec.has_value());
    FakeBacking malformed(bytes.size());
    std::copy(bytes.begin(), bytes.end(), malformed.storage.begin());
    malformed.storage[0] ^= UINT8_C(0xff);
    auto malformed_retained = RetainedBacking::acquire(
        malformed.callbacks(), &malformed.token, malformed.storage.data(),
        bytes.size());
    require(malformed_retained.has_value());
    auto malformed_result = PayloadOwner::open_external(
        std::move(malformed_spec).value(), malformed.storage.data(),
        bytes.size(), std::move(malformed_retained).value());
    require(!malformed_result.has_value());
    require(malformed_result.error().code() == FDB_PAYLOAD_E_INVALID_MAGIC);
    require(malformed_result.error().path() == "/binary/header/magic");
    require(malformed.retain_count == UINT32_C(1));
    require(malformed.release_count == UINT32_C(1));
    require(malformed.event_count == 2U);
    require(malformed.events[0] == 'T');
    require(malformed.events[1] == 'L');

    auto null_spec = compile_fixture("spec/empty.source.json");
    require(null_spec.has_value());
    FakeBacking null_owner(bytes.size());
    null_owner.null_token = true;
    std::copy(bytes.begin(), bytes.end(), null_owner.storage.begin());
    auto null_retained = RetainedBacking::acquire(
        null_owner.callbacks(), nullptr, null_owner.storage.data(),
        bytes.size());
    require(null_retained.has_value());
    {
        auto null_result = PayloadOwner::open_external(
            std::move(null_spec).value(), null_owner.storage.data(),
            bytes.size(), std::move(null_retained).value());
        require(null_result.has_value());
    }
    require(null_owner.retain_count == UINT32_C(1));
    require(null_owner.release_count == UINT32_C(1));

    auto unaligned_spec = compile_fixture("spec/empty.source.json");
    require(unaligned_spec.has_value());
    FakeBacking unaligned(bytes.size() + UINT64_C(1));
    unaligned.base_offset = UINT64_C(1);
    std::copy(bytes.begin(), bytes.end(), unaligned.storage.begin() + 1);
    const std::uint8_t* const unaligned_base = unaligned.storage.data() + 1;
    auto unaligned_retained = RetainedBacking::acquire(
        unaligned.callbacks(), &unaligned.token, unaligned_base, bytes.size());
    require(unaligned_retained.has_value());
    auto unaligned_open = PayloadOwner::open_external(
        std::move(unaligned_spec).value(), unaligned_base, bytes.size(),
        std::move(unaligned_retained).value());
    require(unaligned_open.has_value());
    return EXIT_SUCCESS;
}

int test_plan_publication_and_cleanup() {
    auto planned = make_empty_plan();
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;
    auto heap = planned.value().execute(require_direct, nullptr);
    require(heap.has_value());
    require(heap.value().execution_report().has_value());
    require(heap.value().execution_report()->mode == direct_mode);
    require(heap.value().execution_report()->fallback_reason == UINT32_C(0));
    require(heap.value().execution_report()->used_bytes == total);
    require(fastdb::payload::view::PayloadOwnerTestAccess::copy_bytes(
                heap.value()) == load_hex("valid/empty.bin.hex"));

    PayloadOwner independent = make_owner_with_local_plan();
    require(fastdb::payload::view::PayloadOwnerTestAccess::copy_bytes(
                independent) == load_hex("valid/empty.bin.hex"));

    FakeBacking direct(total);
    {
        auto callbacks = direct.callbacks();
        auto result = planned.value().execute(require_direct, &callbacks);
        require(result.has_value());
        require(result.value().execution_report()->mode == direct_mode);
        require(direct.retain_count == UINT32_C(0));
        require(direct.rollback_count == UINT32_C(0));
        require(direct.release_count == UINT32_C(0));
    }
    require(direct.release_count == UINT32_C(1));

    FakeBacking staged(total);
    staged.decline_first_direct = true;
    {
        auto callbacks = staged.callbacks();
        auto result = planned.value().execute(allow_staging, &callbacks);
        require(result.has_value());
        require(result.value().execution_report()->mode == staged_mode);
        require(result.value().execution_report()->fallback_reason ==
                UINT32_C(2));
        require(result.value().execution_report()->staging_bytes == total);
    }
    require(staged.retain_count == UINT32_C(0));
    require(staged.rollback_count == UINT32_C(0));
    require(staged.release_count == UINT32_C(1));

    FakeBacking corrupted(total);
    corrupted.corrupt_magic = true;
    auto corrupt_callbacks = corrupted.callbacks();
    auto corrupt_result =
        planned.value().execute(require_direct, &corrupt_callbacks);
    require(!corrupt_result.has_value());
    require(corrupt_result.error().code() ==
            FDB_PAYLOAD_E_BACKING_CONTRACT);
    require(corrupt_result.error().path() == "/backing");
    require(corrupt_result.error().details_json() ==
            "{\"original_code\":3001,\"original_details_json\":\"{\\\"actual\\\":\\\"b944425041593100\\\",\\\"expected\\\":\\\"4644425041593100\\\",\\\"reason\\\":\\\"invalid_magic\\\"}\",\"original_path\":\"/binary/header/magic\",\"original_symbol\":\"INVALID_MAGIC\",\"reason\":\"committed_image_validation_failed\"}");
    require(corrupted.commit_count == UINT32_C(1));
    require(corrupted.rollback_count == UINT32_C(0));
    require(corrupted.release_count == UINT32_C(1));

    FakeBacking short_span(total);
    short_span.short_commit = true;
    auto short_callbacks = short_span.callbacks();
    auto short_result = planned.value().execute(require_direct,
                                                &short_callbacks);
    require(!short_result.has_value());
    require(short_result.error().code() == FDB_PAYLOAD_E_BACKING_CONTRACT);
    require(short_span.commit_count == UINT32_C(1));
    require(short_span.rollback_count == UINT32_C(0));
    require(short_span.release_count == UINT32_C(1));

    FakeBacking wrong_pointer(total);
    wrong_pointer.wrong_readable_pointer = true;
    auto wrong_callbacks = wrong_pointer.callbacks();
    auto wrong_result = planned.value().execute(require_direct,
                                                &wrong_callbacks);
    require(!wrong_result.has_value());
    require(wrong_result.error().code() == FDB_PAYLOAD_E_BACKING_CONTRACT);
    require(wrong_pointer.commit_count == UINT32_C(1));
    require(wrong_pointer.rollback_count == UINT32_C(0));
    require(wrong_pointer.release_count == UINT32_C(1));
    return EXIT_SUCCESS;
}

int test_plan_publication_options_exceed_safe_defaults() {
    auto planned = make_empty_plan();
    require(planned.has_value());
    fastdb::payload::build::PlanInfo synthetic = planned.value().info();
    const OpenOptions defaults = fastdb::payload::view::default_open_options();
    synthetic.total_bytes = defaults.max_total_bytes + UINT64_C(1);
    synthetic.region_count = defaults.max_regions + UINT64_C(1);
    synthetic.logical_value_count = defaults.max_nesting_depth + UINT64_C(1);
    synthetic.list_element_count =
        defaults.max_list_elements + UINT64_C(1);
    synthetic.text_bytes = defaults.max_string_bytes + UINT64_C(1);
    synthetic.validation_work =
        defaults.max_validation_work + UINT64_C(1);
    const OpenOptions derived =
        fastdb::payload::build::BuildPlanTestAccess::publication_options(
            planned.value(), synthetic);
    require(derived.validate_text_eager);
    require(derived.max_total_bytes == synthetic.total_bytes);
    require(derived.max_regions == synthetic.region_count);
    require(derived.max_nesting_depth == synthetic.logical_value_count);
    require(derived.max_list_elements == synthetic.list_element_count);
    require(derived.max_string_bytes == synthetic.text_bytes);
    require(derived.max_validation_work == synthetic.validation_work);
    require(derived.max_entries >= defaults.max_entries);
    require(derived.max_components >= defaults.max_components);
    require(derived.max_graph_objects == defaults.max_graph_objects);
    return EXIT_SUCCESS;
}

int run_postcommit_allocation_sweep() {
    auto planned = make_empty_plan();
    require(planned.has_value());
    const std::uint64_t total = planned.value().info().total_bytes;
    bool observed_postcommit_failure = false;
    bool observed_success = false;
    std::uint64_t postcommit_failures = UINT64_C(0);
    std::int64_t last_failure_allocation = INT64_C(-1);
    std::int64_t success_allocation = INT64_C(-1);
    for (std::int64_t allocation = INT64_C(0);
         allocation < INT64_C(2048); ++allocation) {
        FakeBacking backing(total);
        auto callbacks = backing.callbacks();
        allocation_failure::InjectionReset reset;
        allocation_failure::postcommit_only.store(true,
                                                  std::memory_order_relaxed);
        allocation_failure::postcommit_phase.store(false,
                                                   std::memory_order_relaxed);
        allocation_failure::fail_after.store(
            allocation, std::memory_order_relaxed);
        std::optional<Result<PayloadOwner>> result;
        bool exception_escaped = false;
        try {
            result.emplace(
                planned.value().execute(require_direct, &callbacks));
        } catch (const std::bad_alloc&) {
            exception_escaped = true;
        } catch (const std::length_error&) {
            exception_escaped = true;
        }
        allocation_failure::fail_after.store(
            INT64_C(-1), std::memory_order_relaxed);
        allocation_failure::postcommit_only.store(false,
                                                  std::memory_order_relaxed);
        allocation_failure::postcommit_phase.store(false,
                                                   std::memory_order_relaxed);
        require(!exception_escaped);
        require(result.has_value());
        if (result->has_value()) {
            observed_success = true;
            success_allocation = allocation;
            break;
        }
        require(result->error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(backing.commit_count == UINT32_C(1));
        require(backing.rollback_count == UINT32_C(0));
        require(backing.release_count == UINT32_C(1));
        observed_postcommit_failure = true;
        ++postcommit_failures;
        last_failure_allocation = allocation;
    }
    allocation_failure::fail_after.store(INT64_C(-1),
                                         std::memory_order_relaxed);
    require(observed_postcommit_failure);
    require(observed_success);
    require(postcommit_failures >= UINT64_C(2));
    require(last_failure_allocation + INT64_C(1) == success_allocation);
    return EXIT_SUCCESS;
}

int test_postcommit_allocation_cleanup() {
    require(run_postcommit_allocation_sweep() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int test_open_resource_and_text_boundaries() {
    const auto empty = load_hex("valid/empty.bin.hex");
    OpenOptions options = fastdb::payload::view::default_open_options();
    options.max_validation_work = UINT64_C(0);
    auto too_little_spec = compile_fixture("spec/empty.source.json");
    require(too_little_spec.has_value());
    auto too_little = PayloadOwner::open_copy(
        std::move(too_little_spec).value(), empty.data(), empty.size(),
        options);
    require(!too_little.has_value());
    require(too_little.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(too_little.error().path() == "/binary/header");
    options.max_validation_work = UINT64_C(1);
    auto exact_spec = compile_fixture("spec/empty.source.json");
    require(exact_spec.has_value());
    auto exact = PayloadOwner::open_copy(
        std::move(exact_spec).value(), empty.data(), empty.size(), options);
    require(exact.has_value());

    options = fastdb::payload::view::default_open_options();
    options.max_total_bytes = empty.size() - UINT64_C(1);
    auto limited_spec = compile_fixture("spec/empty.source.json");
    require(limited_spec.has_value());
    auto limited = PayloadOwner::open_copy(
        std::move(limited_spec).value(), empty.data(), empty.size(), options);
    require(!limited.has_value());
    require(limited.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(limited.error().path() == "/binary/header");

    auto text = load_hex("valid/text-bytes.bin.hex");
    auto text_spec = compile_fixture("spec/text-bytes.source.json");
    require(text_spec.has_value());
    options = fastdb::payload::view::default_open_options();
    options.max_string_bytes = UINT64_C(0);
    auto string_limited = PayloadOwner::open_copy(
        std::move(text_spec).value(), text.data(), text.size(), options);
    require(!string_limited.has_value());
    require(string_limited.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(string_limited.error().path() ==
            "/binary/regions/7/byte_length");
    return EXIT_SUCCESS;
}

int run_task7_matrix() {
    if (verify_clean_api_cut() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_copy_open_and_physical_diagnostics() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_invalid_golden_corpus() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_external_open_ownership() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_plan_publication_and_cleanup() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_plan_publication_options_exceed_safe_defaults() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_postcommit_allocation_cleanup() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_open_resource_and_text_boundaries();
}

#endif

}  // namespace

int main() {
    if (verify_matrix_inventory() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (verify_golden_inventory() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
#if FASTDB_TASK7_HAS_PAYLOAD_OWNER
    return run_task7_matrix();
#else
    std::cerr
        << "P2 Task 7 RED: payload/view/PayloadOwner.hpp is absent; "
           "hardened copy/external open and immutable owner publication are "
           "not implemented\n";
    return EXIT_FAILURE;
#endif
}
