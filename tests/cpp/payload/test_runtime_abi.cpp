#include "TestSupport.hpp"

#include <fastdb_payload.h>

#include "payload/abi/Handles.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace allocation_failure {

std::atomic<std::int64_t> fail_after{INT64_C(-1)};

struct AllocationHeader final {
    void* raw;
};

constexpr std::size_t kDefaultNewAlignment =
    static_cast<std::size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);
static_assert(kDefaultNewAlignment != 0U);
static_assert((kDefaultNewAlignment & (kDefaultNewAlignment - 1U)) == 0U);
static_assert((alignof(AllocationHeader) &
               (alignof(AllocationHeader) - 1U)) == 0U);

void* allocate(std::size_t size,
               std::size_t alignment = kDefaultNewAlignment) {
    const std::int64_t remaining = fail_after.load(std::memory_order_relaxed);
    if (remaining >= INT64_C(0) &&
        fail_after.fetch_sub(INT64_C(1), std::memory_order_relaxed) ==
            INT64_C(0)) {
        throw std::bad_alloc();
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        throw std::bad_alloc();
    }
    alignment = std::max(
        {alignment, alignof(AllocationHeader), kDefaultNewAlignment});
    const std::size_t payload = size == 0U ? 1U : size;
    const std::size_t padding = alignment - 1U;
    constexpr std::size_t maximum =
        std::numeric_limits<std::size_t>::max();
    if (padding > maximum - sizeof(AllocationHeader)) {
        throw std::bad_alloc();
    }
    const std::size_t overhead = sizeof(AllocationHeader) + padding;
    if (payload > maximum - overhead) {
        throw std::bad_alloc();
    }
    const std::size_t allocation_size = payload + overhead;
    void* const raw = std::malloc(allocation_size);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    void* candidate = static_cast<void*>(
        static_cast<std::uint8_t*>(raw) + sizeof(AllocationHeader));
    std::size_t space = allocation_size - sizeof(AllocationHeader);
    void* const aligned = std::align(alignment, payload, candidate, space);
    if (aligned == nullptr) {
        std::free(raw);
        throw std::bad_alloc();
    }
    (static_cast<AllocationHeader*>(aligned) - 1)->raw = raw;
    return aligned;
}

void deallocate(void* value) noexcept {
    if (value != nullptr) {
        std::free((reinterpret_cast<AllocationHeader*>(value) - 1)->raw);
    }
}

struct Reset final {
    ~Reset() { fail_after.store(INT64_C(-1), std::memory_order_relaxed); }
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

namespace fastdb::payload::view {

/* Test-only projection of the Task 8 generation seam through a C handle. */
struct PayloadOwnerTestAccess final {
    static void wait_until_invalidating(const PayloadOwner& owner) {
        std::unique_lock<std::mutex> lock(owner.state_->barrier.mutex);
        owner.state_->barrier.drained.wait(lock, [&owner] {
            return owner.state_->barrier.invalidating;
        });
    }

    static void set_generation(PayloadOwner& owner,
                               std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(owner.state_->barrier.mutex);
        owner.state_->barrier.generation = generation;
    }
};

}  // namespace fastdb::payload::view

namespace {

int verify_allocation_harness_invariants() {
    void* const allocation = ::operator new(1U);
    const bool aligned =
        reinterpret_cast<std::uintptr_t>(allocation) %
            allocation_failure::kDefaultNewAlignment ==
        std::uintptr_t{0};
    ::operator delete(allocation);
    require(aligned);

    bool invalid_alignment_rejected = false;
    try {
        void* const invalid = allocation_failure::allocate(1U, 3U);
        allocation_failure::deallocate(invalid);
    } catch (const std::bad_alloc&) {
        invalid_alignment_rejected = true;
    }
    require(invalid_alignment_rejected);

    bool overflow_rejected = false;
    try {
        void* const overflow = allocation_failure::allocate(
            std::numeric_limits<std::size_t>::max());
        allocation_failure::deallocate(overflow);
    } catch (const std::bad_alloc&) {
        overflow_rejected = true;
    }
    require(overflow_rejected);
    return EXIT_SUCCESS;
}

bool byte_span_contains_address(const std::uint8_t* span,
                                std::size_t span_size,
                                const std::uint8_t* address) noexcept {
    if (span == nullptr || address == nullptr) {
        return false;
    }
    for (std::size_t index = 0U; index < span_size; ++index) {
        if (span + index == address) {
            return true;
        }
    }
    return false;
}

bool byte_spans_share_address(const std::uint8_t* left,
                              std::size_t left_size,
                              const std::uint8_t* right,
                              std::size_t right_size) noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    for (std::size_t left_index = 0U; left_index < left_size; ++left_index) {
        for (std::size_t right_index = 0U; right_index < right_size;
             ++right_index) {
            if (left + left_index == right + right_index) {
                return true;
            }
        }
    }
    return false;
}

class AccessThreadCleanup final {
public:
    AccessThreadCleanup(fdb_payload_v1_access_t*& access,
                        std::thread& thread) noexcept
        : access_(access), thread_(thread) {}

    AccessThreadCleanup(const AccessThreadCleanup&) = delete;
    AccessThreadCleanup& operator=(const AccessThreadCleanup&) = delete;

    ~AccessThreadCleanup() {
        release_and_join();
    }

    void release_and_join() {
        fdb_payload_v1_access_release(access_);
        access_ = nullptr;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    fdb_payload_v1_access_t*& access_;
    std::thread& thread_;
};

constexpr std::string_view kRecordSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"record.v1",
  "entries":[
    {"id":"bool_value","cardinality":"one","type":{"kind":"bool"}},
    {"id":"u8_value","cardinality":"one","type":{"kind":"u8"}},
    {"id":"u16_value","cardinality":"one","type":{"kind":"u16"}},
    {"id":"u32_value","cardinality":"one","type":{"kind":"u32"}},
    {"id":"i32_value","cardinality":"one","type":{"kind":"i32"}},
    {"id":"u8n_value","cardinality":"one","type":{"kind":"u8n","min":-1,"max":1}},
    {"id":"u16n_value","cardinality":"one","type":{"kind":"u16n","min":0,"max":10}},
    {"id":"f32_value","cardinality":"one","type":{"kind":"f32"}},
    {"id":"f64_value","cardinality":"one","type":{"kind":"f64"}},
    {"id":"str_value","cardinality":"one","type":{"kind":"str"}},
    {"id":"wstr_value","cardinality":"one","type":{"kind":"wstr"}},
    {"id":"bytes_value","cardinality":"one","type":{"kind":"bytes"}},
    {"id":"component_value","cardinality":"one","type":{"kind":"component","id":"Pair"}},
    {"id":"list_value","cardinality":"one","type":{"kind":"list","items":{"kind":"u8"}}},
    {"id":"fixed_values","cardinality":"many","type":{"kind":"u16","nullable":true}},
    {"id":"fixed_strided","cardinality":"many","type":{"kind":"u16","nullable":true}}
  ],
  "components":[
    {"id":"Pair","kind":"record","fields":[
      {"id":"left","type":{"kind":"u8"}},
      {"id":"right","type":{"kind":"u8"}}
    ]}
  ]
})";

constexpr std::string_view kObjectGraphSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[{"id":"root","cardinality":"one","type":{"kind":"ref","target":"Node"}}],
  "components":[{"id":"Node","kind":"record","fields":[
    {"id":"name","type":{"kind":"str"}},
    {"id":"next","type":{"kind":"ref","target":"Node","nullable":true}}
  ]}]
})";

constexpr std::string_view kEmptySpansSpec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"record.v1",
  "entries":[
    {"id":"str_value","cardinality":"one","type":{"kind":"str"}},
    {"id":"wstr_value","cardinality":"one","type":{"kind":"wstr"}},
    {"id":"bytes_value","cardinality":"one","type":{"kind":"bytes"}}
  ],
  "components":[]
})";

static_assert(sizeof(fdb_payload_v1_builder_options_t) ==
              FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_open_options_t) ==
              FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_plan_info_t) ==
              FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE);
static_assert(sizeof(fdb_payload_v1_execution_report_t) ==
              FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE);
static_assert(offsetof(fdb_payload_v1_fixed_run_v1_t, data) <
              offsetof(fdb_payload_v1_fixed_run_v1_t, validity));
static_assert(offsetof(fdb_payload_v1_fixed_run_v1_t, validity) <
              offsetof(fdb_payload_v1_fixed_run_v1_t, reserved));
static_assert(offsetof(fdb_payload_v1_backing_v1_t, context) <
              offsetof(fdb_payload_v1_backing_v1_t, reserve));
static_assert(offsetof(fdb_payload_v1_backing_v1_t, reserve) <
              offsetof(fdb_payload_v1_backing_v1_t, release));
static_assert(offsetof(fdb_payload_v1_backing_v1_t, release) <
              offsetof(fdb_payload_v1_backing_v1_t, reserved));

struct ErrorRef final {
    fdb_payload_v1_error_t* value{nullptr};
    ~ErrorRef() { fdb_payload_v1_error_release(value); }
    void clear() {
        fdb_payload_v1_error_release(value);
        value = nullptr;
    }
};

fdb_payload_v1_spec_t* compile_spec(std::string_view source) {
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const auto status = fdb_payload_v1_spec_compile_json(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), nullptr, &spec, &error);
    if (status != UINT32_C(0) || error != nullptr) {
        fdb_payload_v1_error_release(error);
        return nullptr;
    }
    return spec;
}

bool owned_error(fdb_payload_v1_status_t status,
                 fdb_payload_v1_error_t* error) {
    return status != UINT32_C(0) && error != nullptr &&
           fdb_payload_v1_error_code(error) == status;
}

template <typename Handle, typename Operation, typename Release>
int sweep_handle_allocation_boundary(ErrorRef& error,
                                     Operation&& operation,
                                     Release&& release) {
    allocation_failure::Reset reset;
    bool saw_allocation_failure = false;
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(4096);
         ++allocation) {
        Handle* candidate =
            reinterpret_cast<Handle*>(std::uintptr_t{1});
        allocation_failure::fail_after.store(allocation,
                                             std::memory_order_relaxed);
        const auto status = std::forward<Operation>(operation)(
            &candidate, &error.value);
        allocation_failure::fail_after.store(INT64_C(-1),
                                             std::memory_order_relaxed);
        if (status == UINT32_C(0)) {
            require(candidate != nullptr && error.value == nullptr);
            std::forward<Release>(release)(candidate);
            require(saw_allocation_failure);
            return EXIT_SUCCESS;
        }
        require(status == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(candidate == nullptr);
        require(owned_error(status, error.value));
        saw_allocation_failure = true;
        error.clear();
    }
    require(false, "allocation sweep did not reach a successful retry");
    return EXIT_FAILURE;
}

std::string_view error_details(fdb_payload_v1_error_t* error) {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    fdb_payload_v1_error_details_json(error, &data, &size);
    return data == nullptr
               ? std::string_view{}
               : std::string_view{reinterpret_cast<const char*>(data),
                                  static_cast<std::size_t>(size)};
}

int test_initializers_and_constants() {
    fdb_payload_v1_builder_options_t builder{};
    fdb_payload_v1_fixed_run_v1_t run{};
    fdb_payload_v1_open_options_t open{};
    fdb_payload_v1_plan_info_t info{};
    fdb_payload_v1_execution_report_t report{};
    fdb_payload_v1_backing_v1_t backing{};
    fdb_payload_v1_builder_options_init(&builder);
    fdb_payload_v1_fixed_run_init(&run);
    fdb_payload_v1_open_options_init(&open);
    fdb_payload_v1_plan_info_init(&info);
    fdb_payload_v1_execution_report_init(&report);
    fdb_payload_v1_backing_init(&backing);

    require(builder.struct_size == FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE);
    require(builder.flags == UINT32_C(0));
    require(builder.max_value_nodes == UINT64_C(10000000));
    require(builder.max_list_elements == UINT64_C(10000000));
    require(builder.max_text_bytes == (UINT64_C(1) << 30));
    require(builder.max_opaque_bytes == (UINT64_C(1) << 30));
    require(builder.max_nesting_depth == UINT64_C(1024));
    require(builder.max_total_builder_bytes == (UINT64_C(1) << 30));
    require(run.struct_size == sizeof(run));
    require(run.data == nullptr && run.validity == nullptr);
    require(open.struct_size == FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE);
    require(open.flags == FDB_PAYLOAD_OPEN_VALIDATE_TEXT_EAGER);
    require(open.max_total_bytes == (UINT64_C(1) << 30));
    require(open.max_regions == UINT64_C(1000000));
    require(open.max_entries == UINT64_C(65536));
    require(open.max_components == UINT64_C(65536));
    require(open.max_nesting_depth == UINT64_C(1024));
    require(open.max_list_elements == UINT64_C(10000000));
    require(open.max_graph_objects == UINT64_C(10000000));
    require(open.max_string_bytes == (UINT64_C(1) << 30));
    require(open.max_validation_work == UINT64_C(100000000));
    require(info.struct_size == FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE);
    require(report.struct_size == FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE);
    require(backing.struct_size == sizeof(backing));
    require(backing.context == nullptr && backing.reserve == nullptr &&
            backing.write == nullptr && backing.commit == nullptr &&
            backing.rollback == nullptr && backing.retain == nullptr &&
            backing.release == nullptr);

    require(FDB_PAYLOAD_OPERATION_BUILD == (UINT64_C(1) << 2));
    require(FDB_PAYLOAD_OPERATION_OPEN == (UINT64_C(1) << 3));
    require(FDB_PAYLOAD_OPERATION_VIEW == (UINT64_C(1) << 4));
    require(FDB_PAYLOAD_OPERATION_MATERIALIZE == (UINT64_C(1) << 5));
    require(FDB_PAYLOAD_OPERATION_INVALIDATE == (UINT64_C(1) << 6));
    require(FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE == UINT32_C(1));
    require(FDB_PAYLOAD_BUILD_ALLOW_STAGING == UINT32_C(1));
    require(FDB_PAYLOAD_BUILD_REQUIRE_DIRECT == UINT32_C(2));
    require(FDB_PAYLOAD_EXECUTION_DIRECT == UINT32_C(1));
    require(FDB_PAYLOAD_EXECUTION_STAGED == UINT32_C(2));
    require(FDB_PAYLOAD_FALLBACK_NONE == UINT32_C(0));
    require(FDB_PAYLOAD_VIEW_SEQUENCE == UINT32_C(1));
    require(FDB_PAYLOAD_VIEW_BOOL == UINT32_C(2));
    require(FDB_PAYLOAD_VIEW_U8 == UINT32_C(3));
    require(FDB_PAYLOAD_VIEW_U16 == UINT32_C(4));
    require(FDB_PAYLOAD_VIEW_U32 == UINT32_C(5));
    require(FDB_PAYLOAD_VIEW_I32 == UINT32_C(6));
    require(FDB_PAYLOAD_VIEW_U8N == UINT32_C(7));
    require(FDB_PAYLOAD_VIEW_U16N == UINT32_C(8));
    require(FDB_PAYLOAD_VIEW_F32 == UINT32_C(9));
    require(FDB_PAYLOAD_VIEW_F64 == UINT32_C(10));
    require(FDB_PAYLOAD_VIEW_STR == UINT32_C(11));
    require(FDB_PAYLOAD_VIEW_WSTR == UINT32_C(12));
    require(FDB_PAYLOAD_VIEW_BYTES == UINT32_C(13));
    require(FDB_PAYLOAD_VIEW_COMPONENT == UINT32_C(14));
    require(FDB_PAYLOAD_VIEW_LIST == UINT32_C(15));
    require(FDB_PAYLOAD_VIEW_REF == UINT32_C(16));

    fdb_payload_v1_builder_options_init(nullptr);
    fdb_payload_v1_fixed_run_init(nullptr);
    fdb_payload_v1_open_options_init(nullptr);
    fdb_payload_v1_plan_info_init(nullptr);
    fdb_payload_v1_execution_report_init(nullptr);
    fdb_payload_v1_backing_init(nullptr);
    fdb_payload_v1_builder_release(nullptr);
    fdb_payload_v1_plan_retain(nullptr);
    fdb_payload_v1_plan_release(nullptr);
    fdb_payload_v1_payload_retain(nullptr);
    fdb_payload_v1_payload_release(nullptr);
    fdb_payload_v1_view_retain(nullptr);
    fdb_payload_v1_view_release(nullptr);
    fdb_payload_v1_access_release(nullptr);
    return EXIT_SUCCESS;
}

int author_record(fdb_payload_v1_builder_t* builder) {
    ErrorRef error;
    const auto ok = [&error](fdb_payload_v1_status_t status) {
        const bool success = status == UINT32_C(0) && error.value == nullptr;
        error.clear();
        return success;
    };
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(0), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_bool(
        builder, UINT8_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(1), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(8), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(2), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u16(
        builder, UINT16_C(16), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(3), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u32(
        builder, UINT32_C(32), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(4), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_i32(
        builder, INT32_C(-32), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(5), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8n_f64_bits(
        builder, UINT64_C(0x0000000000000000), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(6), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u16n_f64_bits(
        builder, UINT64_C(0x4014000000000000), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(7), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_f32_bits(
        builder, UINT32_C(0x3fc00000), &error.value)));
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(8), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_f64_bits(
        builder, UINT64_C(0x4004000000000000), &error.value)));

    static constexpr std::array<std::uint8_t, 3> text{{'a', 'b', 'c'}};
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(9), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_str(
        builder, text.data(), text.size(), &error.value)));
    static constexpr std::array<std::uint16_t, 2> wide{{
        UINT16_C(0x0041), UINT16_C(0x03a9)}};
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(10), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_wstr(
        builder, wide.data(), wide.size(), &error.value)));
    static constexpr std::array<std::uint8_t, 3> bytes{{
        UINT8_C(0), UINT8_C(1), UINT8_C(255)}};
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(11), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_bytes(
        builder, bytes.data(), bytes.size(), &error.value)));

    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(12), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_component_begin(
        builder, &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(2), &error.value)));

    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(13), UINT64_C(1), &error.value)));
    require(ok(fdb_payload_v1_builder_value_list_begin(
        builder, UINT64_C(2), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(3), &error.value)));
    require(ok(fdb_payload_v1_builder_value_u8(
        builder, UINT8_C(4), &error.value)));

    static constexpr std::array<std::uint16_t, 3> fixed{{
        UINT16_C(10), UINT16_C(20), UINT16_C(30)}};
    static constexpr std::array<std::uint8_t, 1> validity{{UINT8_C(0x05)}};
    fdb_payload_v1_fixed_run_v1_t run{};
    fdb_payload_v1_fixed_run_init(&run);
    run.data = fixed.data();
    run.data_byte_length = sizeof(fixed);
    run.count = fixed.size();
    run.stride_bytes = sizeof(std::uint16_t);
    run.validity = validity.data();
    run.validity_byte_length = validity.size();
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(14), fixed.size(), &error.value)));
    require(ok(fdb_payload_v1_builder_value_fixed_run(
        builder, &run, &error.value)));

    static constexpr std::array<std::uint16_t, 6> strided{{
        UINT16_C(100), UINT16_C(999), UINT16_C(200), UINT16_C(999),
        UINT16_C(300), UINT16_C(999)}};
    static constexpr std::array<std::uint8_t, 2> offset_validity{{
        UINT8_C(0), UINT8_C(0x0a)}};
    require(ok(fdb_payload_v1_builder_entry_begin(
        builder, UINT32_C(15), UINT64_C(3), &error.value)));
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, nullptr, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    fdb_payload_v1_fixed_run_v1_t invalid{};
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.struct_size = UINT32_C(4);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.flags = UINT32_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.reserved[1] = UINT64_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.data_byte_length = sizeof(std::uint16_t);
    invalid.count = UINT64_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    fdb_payload_v1_fixed_run_init(&invalid);
    invalid.data = strided.data();
    invalid.data_byte_length = sizeof(strided);
    invalid.count = UINT64_C(0);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    invalid.count = UINT64_C(3);
    invalid.stride_bytes = UINT64_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    invalid.stride_bytes = UINT64_C(4);
    invalid.validity = nullptr;
    invalid.validity_byte_length = UINT64_C(1);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    invalid.validity = offset_validity.data();
    invalid.validity_byte_length = UINT64_C(0);
    invalid.validity_bit_offset = UINT64_C(9);
    require(fdb_payload_v1_builder_value_fixed_run(
                builder, &invalid, &error.value) ==
            FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS);
    error.clear();

    struct FutureRun final {
        fdb_payload_v1_fixed_run_v1_t known;
        std::array<std::uint8_t, 16> tail;
    } future_run{};
    fdb_payload_v1_fixed_run_init(&future_run.known);
    future_run.known.struct_size = sizeof(future_run);
    future_run.known.data = strided.data();
    future_run.known.data_byte_length = sizeof(strided);
    future_run.known.count = UINT64_C(3);
    future_run.known.stride_bytes = UINT64_C(4);
    future_run.known.validity = offset_validity.data();
    future_run.known.validity_byte_length = offset_validity.size();
    future_run.known.validity_bit_offset = UINT64_C(9);
    future_run.tail.fill(UINT8_C(0xa5));
    require(ok(fdb_payload_v1_builder_value_fixed_run(
        builder, &future_run.known, &error.value)));
    require(std::all_of(future_run.tail.begin(), future_run.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    return EXIT_SUCCESS;
}

struct BackingContext final {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> relocated_bytes;
    std::vector<std::uint32_t> calls;
    std::uint32_t retains{UINT32_C(0)};
    std::uint32_t releases{UINT32_C(0)};
    bool decline_direct{false};
    bool provide_writable{true};
    bool null_owner_token{false};
    bool poison_reserve_outputs_on_failure{false};
    bool poison_commit_outputs_on_failure{false};
    bool relocate_on_commit{false};
    bool tokens_match{true};
    std::uint32_t direct_reserve_status{UINT32_C(0)};
    std::uint32_t staged_reserve_status{UINT32_C(0)};
    std::uint32_t write_status{UINT32_C(0)};
    std::uint32_t commit_status{UINT32_C(0)};
    std::uint32_t rollback_status{UINT32_C(0)};
    std::uint32_t retain_status{UINT32_C(0)};
};

fdb_payload_v1_status_t reserve_backing(
    void* opaque, std::uint32_t mode, std::uint64_t minimum_capacity,
    std::uint32_t, void** out_owner_token, std::uint8_t** out_writable_data,
    std::uint64_t* out_capacity) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.calls.push_back(UINT32_C(10) + mode);
    if (mode == FDB_PAYLOAD_RESERVE_DIRECT && context.decline_direct) {
        if (context.poison_reserve_outputs_on_failure) {
            *out_owner_token = reinterpret_cast<void*>(std::uintptr_t{1});
            *out_writable_data =
                reinterpret_cast<std::uint8_t*>(std::uintptr_t{1});
            *out_capacity = UINT64_MAX;
        }
        return FDB_PAYLOAD_E_DIRECT_UNAVAILABLE;
    }
    const std::uint32_t configured_status =
        mode == FDB_PAYLOAD_RESERVE_DIRECT
            ? context.direct_reserve_status
            : context.staged_reserve_status;
    if (configured_status != UINT32_C(0)) {
        if (context.poison_reserve_outputs_on_failure) {
            *out_owner_token = reinterpret_cast<void*>(std::uintptr_t{1});
            *out_writable_data =
                reinterpret_cast<std::uint8_t*>(std::uintptr_t{1});
            *out_capacity = UINT64_MAX;
        }
        return configured_status;
    }
    context.bytes.assign(static_cast<std::size_t>(minimum_capacity),
                         UINT8_C(0));
    *out_owner_token = context.null_owner_token ? nullptr : opaque;
    *out_writable_data =
        context.provide_writable ? context.bytes.data() : nullptr;
    *out_capacity = minimum_capacity;
    return UINT32_C(0);
}

fdb_payload_v1_status_t write_backing(
    void* opaque, void* owner_token, std::uint64_t offset,
    const std::uint8_t* source,
    std::uint64_t source_size) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    context.calls.push_back(UINT32_C(20));
    if (context.write_status != UINT32_C(0)) {
        return context.write_status;
    }
    if (source_size != UINT64_C(0)) {
        std::memcpy(context.bytes.data() + static_cast<std::size_t>(offset),
                    source, static_cast<std::size_t>(source_size));
    }
    return UINT32_C(0);
}

fdb_payload_v1_status_t commit_backing(
    void* opaque, void* owner_token, std::uint64_t used_size,
    const std::uint8_t** out_readable_data, std::uint64_t* out_readable_size) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    context.calls.push_back(UINT32_C(30));
    if (context.commit_status != UINT32_C(0)) {
        if (context.poison_commit_outputs_on_failure) {
            *out_readable_data =
                reinterpret_cast<const std::uint8_t*>(std::uintptr_t{1});
            *out_readable_size = UINT64_MAX;
        }
        return context.commit_status;
    }
    if (context.relocate_on_commit) {
        context.relocated_bytes = context.bytes;
    }
    *out_readable_data = context.relocate_on_commit
                             ? context.relocated_bytes.data()
                             : context.bytes.data();
    *out_readable_size = used_size;
    return UINT32_C(0);
}

fdb_payload_v1_status_t rollback_backing(void* opaque, void* owner_token) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    context.calls.push_back(UINT32_C(40));
    return context.rollback_status;
}

fdb_payload_v1_status_t retain_backing(void* opaque, void* owner_token) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    ++context.retains;
    return context.retain_status;
}

void release_backing(void* opaque, void* owner_token) {
    auto& context = *static_cast<BackingContext*>(opaque);
    context.tokens_match =
        context.tokens_match &&
        owner_token == (context.null_owner_token ? nullptr : opaque);
    ++context.releases;
}

fdb_payload_v1_backing_v1_t backing_for(BackingContext& context) {
    fdb_payload_v1_backing_v1_t backing{};
    fdb_payload_v1_backing_init(&backing);
    backing.context = &context;
    backing.reserve = reserve_backing;
    backing.write = write_backing;
    backing.commit = commit_backing;
    backing.rollback = rollback_backing;
    backing.retain = retain_backing;
    backing.release = release_backing;
    return backing;
}

struct RuntimeFixture final {
    RuntimeFixture() = default;

    int initialize() {
        spec = compile_spec(kRecordSpec);
        require(spec != nullptr);
        ErrorRef error;
        require(fdb_payload_v1_builder_create(
                    spec, nullptr, &builder, &error.value) == UINT32_C(0));
        require(author_record(builder) == EXIT_SUCCESS);
        require(fdb_payload_v1_builder_freeze(
                    builder, &plan, &error.value) == UINT32_C(0));
        fdb_payload_v1_execution_report_t report{};
        fdb_payload_v1_execution_report_init(&report);
        require(fdb_payload_v1_plan_execute(
                    plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, &payload,
                    &report, &error.value) == UINT32_C(0));
        require(payload != nullptr && error.value == nullptr);
        return EXIT_SUCCESS;
    }

    RuntimeFixture(const RuntimeFixture&) = delete;
    RuntimeFixture& operator=(const RuntimeFixture&) = delete;

    ~RuntimeFixture() {
        fdb_payload_v1_payload_release(payload);
        fdb_payload_v1_plan_release(plan);
        fdb_payload_v1_builder_release(builder);
        fdb_payload_v1_spec_release(spec);
    }

    fdb_payload_v1_spec_t* spec{nullptr};
    fdb_payload_v1_builder_t* builder{nullptr};
    fdb_payload_v1_plan_t* plan{nullptr};
    fdb_payload_v1_payload_t* payload{nullptr};
};

int test_handle_allocation_boundaries_and_owner_projection() {
    RuntimeFixture fixture;
    require(fixture.initialize() == EXIT_SUCCESS);
    ErrorRef error;

    require(sweep_handle_allocation_boundary<fdb_payload_v1_access_t>(
                error,
                [&fixture](fdb_payload_v1_access_t** out,
                           fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_payload_acquire(
                        fixture.payload, out, out_error);
                },
                fdb_payload_v1_access_release) == EXIT_SUCCESS);
    require(sweep_handle_allocation_boundary<fdb_payload_v1_view_t>(
                error,
                [&fixture](fdb_payload_v1_view_t** out,
                           fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_payload_entry_view(
                        fixture.payload, UINT32_C(0), out, out_error);
                },
                fdb_payload_v1_view_release) == EXIT_SUCCESS);

    fdb_payload_v1_view_t* bool_sequence = nullptr;
    require(fdb_payload_v1_payload_entry_view(
                fixture.payload, UINT32_C(0), &bool_sequence,
                &error.value) == UINT32_C(0));
    require(sweep_handle_allocation_boundary<fdb_payload_v1_view_t>(
                error,
                [bool_sequence](fdb_payload_v1_view_t** out,
                                fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_view_at(
                        bool_sequence, UINT64_C(0), out, out_error);
                },
                fdb_payload_v1_view_release) == EXIT_SUCCESS);

    fdb_payload_v1_view_t* component_sequence = nullptr;
    fdb_payload_v1_view_t* component = nullptr;
    require(fdb_payload_v1_payload_entry_view(
                fixture.payload, UINT32_C(12), &component_sequence,
                &error.value) == UINT32_C(0));
    require(fdb_payload_v1_view_at(
                component_sequence, UINT64_C(0), &component,
                &error.value) == UINT32_C(0));
    require(sweep_handle_allocation_boundary<fdb_payload_v1_view_t>(
                error,
                [component](fdb_payload_v1_view_t** out,
                            fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_view_field(
                        component, UINT32_C(0), out, out_error);
                },
                fdb_payload_v1_view_release) == EXIT_SUCCESS);

    fdb_payload_v1_view_t* text_sequence = nullptr;
    fdb_payload_v1_view_t* text = nullptr;
    require(fdb_payload_v1_payload_entry_view(
                fixture.payload, UINT32_C(9), &text_sequence,
                &error.value) == UINT32_C(0));
    require(fdb_payload_v1_view_at(
                text_sequence, UINT64_C(0), &text,
                &error.value) == UINT32_C(0));
    require(sweep_handle_allocation_boundary<fdb_payload_v1_access_t>(
                error,
                [text](fdb_payload_v1_access_t** out,
                       fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_view_acquire(
                        text, out, out_error);
                },
                fdb_payload_v1_access_release) == EXIT_SUCCESS);
    require(sweep_handle_allocation_boundary<fdb_payload_v1_view_t>(
                error,
                [component](fdb_payload_v1_view_t** out,
                            fdb_payload_v1_error_t** out_error) {
                    return fdb_payload_v1_view_materialize(
                        component, out, out_error);
                },
                fdb_payload_v1_view_release) == EXIT_SUCCESS);

    fdb_payload_v1_view_release(text);
    fdb_payload_v1_view_release(text_sequence);
    fdb_payload_v1_view_release(component);
    fdb_payload_v1_view_release(component_sequence);
    fdb_payload_v1_view_release(bool_sequence);

    /* Failed Access construction must unwind both Core and ABI owner pins. */
    require(fdb_payload_v1_payload_invalidate(
                fixture.payload, &error.value) == UINT32_C(0));
    require(error.value == nullptr);
    return EXIT_SUCCESS;
}

int test_stale_generation_c_projection() {
    RuntimeFixture fixture;
    require(fixture.initialize() == EXIT_SUCCESS);
    ErrorRef error;
    fdb_payload_v1_view_t* sequence = nullptr;
    fdb_payload_v1_view_t* scalar = nullptr;
    require(fdb_payload_v1_payload_entry_view(
                fixture.payload, UINT32_C(3), &sequence,
                &error.value) == UINT32_C(0));
    require(fdb_payload_v1_view_at(
                sequence, UINT64_C(0), &scalar,
                &error.value) == UINT32_C(0));

    auto& owner = const_cast<fastdb::payload::view::PayloadOwner&>(
        fixture.payload->value);
    fastdb::payload::view::PayloadOwnerTestAccess::set_generation(
        owner, UINT64_C(2));
    std::uint32_t value = UINT32_MAX;
    const auto status = fdb_payload_v1_view_get_u32(
        scalar, &value, &error.value);
    require(status == FDB_PAYLOAD_E_STALE_GENERATION);
    require(value == UINT32_C(0));
    require(owned_error(status, error.value));

    fdb_payload_v1_view_release(scalar);
    fdb_payload_v1_view_release(sequence);
    return EXIT_SUCCESS;
}

int test_view_and_access_keep_owner_alive() {
    RuntimeFixture fixture;
    require(fixture.initialize() == EXIT_SUCCESS);
    ErrorRef error;
    fdb_payload_v1_view_t* independent_view = nullptr;
    fdb_payload_v1_view_t* text_sequence = nullptr;
    fdb_payload_v1_view_t* text = nullptr;
    fdb_payload_v1_access_t* access = nullptr;
    require(fdb_payload_v1_payload_entry_view(
                fixture.payload, UINT32_C(0), &independent_view,
                &error.value) == UINT32_C(0));
    require(fdb_payload_v1_payload_entry_view(
                fixture.payload, UINT32_C(9), &text_sequence,
                &error.value) == UINT32_C(0));
    require(fdb_payload_v1_view_at(
                text_sequence, UINT64_C(0), &text,
                &error.value) == UINT32_C(0));
    require(fdb_payload_v1_view_acquire(
                text, &access, &error.value) == UINT32_C(0));

    fdb_payload_v1_view_release(text);
    fdb_payload_v1_view_release(text_sequence);
    fdb_payload_v1_payload_release(fixture.payload);
    fixture.payload = nullptr;

    std::uint32_t kind = UINT32_C(0);
    std::uint64_t length = UINT64_C(0);
    require(fdb_payload_v1_view_kind(
                independent_view, &kind, &error.value) == UINT32_C(0));
    require(kind == FDB_PAYLOAD_VIEW_SEQUENCE);
    require(fdb_payload_v1_view_length(
                independent_view, &length, &error.value) == UINT32_C(0));
    require(length == UINT64_C(1));
    fdb_payload_v1_view_release(independent_view);

    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    require(fdb_payload_v1_access_str(
                access, &data, &size, &error.value) == UINT32_C(0));
    require(size == UINT64_C(3));
    require(std::memcmp(data, "abc", 3U) == 0);
    fdb_payload_v1_access_release(access);
    return EXIT_SUCCESS;
}

int test_complete_record_runtime() {
    fdb_payload_v1_spec_t* spec = compile_spec(kRecordSpec);
    require(spec != nullptr);
    ErrorRef error;
    fdb_payload_v1_builder_options_t options{};
    fdb_payload_v1_builder_options_init(&options);
    fdb_payload_v1_builder_t* builder = nullptr;
    require(fdb_payload_v1_builder_create(
                spec, &options, &builder, &error.value) == UINT32_C(0));
    require(builder != nullptr && error.value == nullptr);
    fdb_payload_v1_plan_t* incomplete_plan =
        reinterpret_cast<fdb_payload_v1_plan_t*>(std::uintptr_t{1});
    const auto incomplete_status = fdb_payload_v1_builder_freeze(
        builder, &incomplete_plan, &error.value);
    require(incomplete_status == FDB_PAYLOAD_E_MISSING_ENTRY,
            std::to_string(incomplete_status));
    require(incomplete_plan == nullptr);
    require(owned_error(incomplete_status, error.value));
    error.clear();
    require(author_record(builder) == EXIT_SUCCESS);

    fdb_payload_v1_plan_t* plan = nullptr;
    require(fdb_payload_v1_builder_freeze(
                builder, &plan, &error.value) == UINT32_C(0));
    require(plan != nullptr && error.value == nullptr);

    fdb_payload_v1_error_t* state_error = nullptr;
    const auto state_status =
        fdb_payload_v1_builder_value_null(builder, &state_error);
    require(owned_error(state_status, state_error));
    require(fdb_payload_v1_error_code(state_error) ==
            FDB_PAYLOAD_E_BUILDER_STATE);
    fdb_payload_v1_error_release(state_error);
    fdb_payload_v1_builder_release(builder);

    fdb_payload_v1_plan_info_t info{};
    fdb_payload_v1_plan_info_init(&info);
    require(fdb_payload_v1_plan_info(plan, &info, &error.value) ==
            UINT32_C(0));
    require(info.total_bytes > UINT64_C(0));
    require(info.region_count > UINT64_C(0));
    require(info.logical_value_count > UINT64_C(0));
    require(info.direct_build_status == FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE);

    struct FutureInfo final {
        fdb_payload_v1_plan_info_t known;
        std::array<std::uint8_t, 16> tail;
    } future_info{};
    fdb_payload_v1_plan_info_init(&future_info.known);
    future_info.known.struct_size = sizeof(future_info);
    future_info.tail.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_plan_info(
                plan, &future_info.known, &error.value) == UINT32_C(0));
    require(future_info.known.total_bytes == info.total_bytes);
    require(std::all_of(future_info.tail.begin(), future_info.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_plan_info_t short_info{};
    fdb_payload_v1_plan_info_init(&short_info);
    short_info.struct_size = UINT32_C(4);
    short_info.total_bytes = UINT64_MAX;
    require(fdb_payload_v1_plan_info(
                plan, &short_info, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(short_info.struct_size == UINT32_C(4));
    error.clear();
    fdb_payload_v1_plan_info_t invalid_info{};
    fdb_payload_v1_plan_info_init(&invalid_info);
    invalid_info.flags = UINT32_C(1);
    require(fdb_payload_v1_plan_info(
                plan, &invalid_info, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_info.flags == UINT32_C(0));
    error.clear();
    fdb_payload_v1_plan_info_init(&invalid_info);
    invalid_info.reserved[3] = UINT64_C(1);
    require(fdb_payload_v1_plan_info(
                plan, &invalid_info, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_info.reserved[3] == UINT64_C(0));
    error.clear();

    fdb_payload_v1_execution_report_t cleared_report{};
    fdb_payload_v1_execution_report_init(&cleared_report);
    cleared_report.mode = UINT32_MAX;
    cleared_report.used_bytes = UINT64_MAX;
    fdb_payload_v1_error_t* clearing_error = nullptr;
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, nullptr,
                &cleared_report, &clearing_error) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_report.mode == UINT32_C(0));
    require(cleared_report.used_bytes == UINT64_C(0));
    require(owned_error(FDB_PAYLOAD_E_INVALID_ARGUMENT, clearing_error));
    fdb_payload_v1_error_release(clearing_error);

    std::atomic<bool> consistent{true};
    std::vector<std::thread> workers;
    for (std::uint32_t worker = 0; worker < UINT32_C(4); ++worker) {
        workers.emplace_back([plan, info, &consistent]() {
            for (std::uint32_t iteration = 0; iteration < UINT32_C(50);
                 ++iteration) {
                fdb_payload_v1_plan_retain(plan);
                fdb_payload_v1_plan_info_t local{};
                fdb_payload_v1_plan_info_init(&local);
                fdb_payload_v1_error_t* local_error = nullptr;
                if (fdb_payload_v1_plan_info(plan, &local, &local_error) !=
                        UINT32_C(0) ||
                    local_error != nullptr ||
                    local.total_bytes != info.total_bytes ||
                    local.region_count != info.region_count) {
                    consistent.store(false, std::memory_order_relaxed);
                }
                fdb_payload_v1_error_release(local_error);
                fdb_payload_v1_plan_release(plan);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(consistent.load(std::memory_order_relaxed));

    fdb_payload_v1_execution_report_t report{};
    fdb_payload_v1_execution_report_init(&report);
    fdb_payload_v1_payload_t* payload = nullptr;
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, &payload,
                &report, &error.value) == UINT32_C(0));
    require(payload != nullptr && error.value == nullptr);
    require(report.mode == FDB_PAYLOAD_EXECUTION_DIRECT);
    require(report.fallback_reason == FDB_PAYLOAD_FALLBACK_NONE);
    require(report.used_bytes == info.total_bytes);

    struct FutureExecuteReport final {
        fdb_payload_v1_execution_report_t known;
        std::array<std::uint8_t, 16> tail;
    } future_execute_report{};
    fdb_payload_v1_execution_report_init(&future_execute_report.known);
    future_execute_report.known.struct_size = sizeof(future_execute_report);
    future_execute_report.tail.fill(UINT8_C(0xa5));
    fdb_payload_v1_payload_t* future_execute_payload = nullptr;
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr,
                &future_execute_payload, &future_execute_report.known,
                &error.value) == UINT32_C(0));
    require(future_execute_payload != nullptr);
    require(future_execute_report.known.used_bytes == info.total_bytes);
    require(std::all_of(
        future_execute_report.tail.begin(), future_execute_report.tail.end(),
        [](std::uint8_t value) { return value == UINT8_C(0xa5); }));
    fdb_payload_v1_payload_release(future_execute_payload);

    fdb_payload_v1_execution_report_t queried_report{};
    fdb_payload_v1_execution_report_init(&queried_report);
    require(fdb_payload_v1_payload_execution_report(
                payload, &queried_report, &error.value) == UINT32_C(0));
    require(queried_report.used_bytes == report.used_bytes);
    require(queried_report.mode == report.mode);
    struct FutureReport final {
        fdb_payload_v1_execution_report_t known;
        std::array<std::uint8_t, 16> tail;
    } future_report{};
    fdb_payload_v1_execution_report_init(&future_report.known);
    future_report.known.struct_size = sizeof(future_report);
    future_report.tail.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_payload_execution_report(
                payload, &future_report.known, &error.value) == UINT32_C(0));
    require(future_report.known.used_bytes == report.used_bytes);
    require(std::all_of(future_report.tail.begin(), future_report.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_execution_report_t invalid_report{};
    fdb_payload_v1_execution_report_init(&invalid_report);
    invalid_report.struct_size = UINT32_C(4);
    require(fdb_payload_v1_payload_execution_report(
                payload, &invalid_report, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_report.struct_size == UINT32_C(4));
    error.clear();
    fdb_payload_v1_execution_report_init(&invalid_report);
    invalid_report.reserved32 = UINT32_C(1);
    invalid_report.mode = UINT32_MAX;
    require(fdb_payload_v1_payload_execution_report(
                payload, &invalid_report, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_report.mode == UINT32_C(0));
    require(invalid_report.reserved32 == UINT32_C(0));
    error.clear();
    fdb_payload_v1_execution_report_init(&invalid_report);
    invalid_report.reserved64[1] = UINT64_C(1);
    invalid_report.used_bytes = UINT64_MAX;
    require(fdb_payload_v1_payload_execution_report(
                payload, &invalid_report, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(invalid_report.used_bytes == UINT64_C(0));
    require(invalid_report.reserved64[1] == UINT64_C(0));
    error.clear();

    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
    require(fdb_payload_v1_payload_sha256(
                payload, digest.data(), &error.value) == UINT32_C(0));
    const auto payload_digest = digest;
    fdb_payload_v1_profile_t profile = UINT32_C(0);
    require(fdb_payload_v1_payload_profile(
                payload, &profile, &error.value) == UINT32_C(0));
    require(profile == FDB_PAYLOAD_PROFILE_RECORD_V1);

    fdb_payload_v1_blob_t* binary = nullptr;
    require(fdb_payload_v1_payload_binary_blob(
                payload, &binary, &error.value) == UINT32_C(0));
    require(binary != nullptr);
    require(fdb_payload_v1_blob_size(binary) == info.total_bytes);

    fdb_payload_v1_open_options_t rejected_open_options{};
    fdb_payload_v1_open_options_init(&rejected_open_options);
    rejected_open_options.struct_size = UINT32_C(4);
    fdb_payload_v1_payload_t* rejected_opened =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    auto rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), &rejected_open_options,
        &rejected_opened, &error.value);
    require(rejected_open_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_opened == nullptr);
    require(owned_error(rejected_open_status, error.value));
    error.clear();
    fdb_payload_v1_open_options_init(&rejected_open_options);
    rejected_open_options.flags = UINT32_C(2);
    rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), &rejected_open_options,
        &rejected_opened, &error.value);
    require(rejected_open_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_opened == nullptr);
    error.clear();
    fdb_payload_v1_open_options_init(&rejected_open_options);
    rejected_open_options.reserved[0] = UINT64_C(1);
    rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), &rejected_open_options,
        &rejected_opened, &error.value);
    require(rejected_open_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_opened == nullptr);
    error.clear();

    rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, nullptr, UINT64_C(1), nullptr, &rejected_opened, &error.value);
    require(rejected_open_status == FDB_PAYLOAD_E_OUT_OF_BOUNDS);
    require(rejected_opened == nullptr);
    error.clear();
    rejected_open_status = fdb_payload_v1_payload_open_copy(
        spec, nullptr, UINT64_C(0), nullptr, &rejected_opened, &error.value);
    require(rejected_open_status != UINT32_C(0));
    require(rejected_open_status != FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(rejected_opened == nullptr);
    require(owned_error(rejected_open_status, error.value));
    error.clear();

    fdb_payload_v1_spec_t* graph_spec = compile_spec(kObjectGraphSpec);
    require(graph_spec != nullptr);
    fdb_payload_v1_payload_t* graph_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    auto graph_open_status = fdb_payload_v1_payload_open_copy(
        graph_spec, fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_size(binary), nullptr, &graph_payload,
        &error.value);
    require(graph_open_status == FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE,
            std::to_string(graph_open_status));
    require(graph_payload == nullptr);
    require(owned_error(graph_open_status, error.value));
    error.clear();

    BackingContext graph_external_context;
    graph_external_context.bytes.assign(
        fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_data(binary) + fdb_payload_v1_blob_size(binary));
    auto graph_external_backing = backing_for(graph_external_context);
    graph_payload = reinterpret_cast<fdb_payload_v1_payload_t*>(
        std::uintptr_t{1});
    graph_open_status = fdb_payload_v1_payload_open_external(
        graph_spec, graph_external_context.bytes.data(),
        graph_external_context.bytes.size(), &graph_external_backing,
        &graph_external_context, nullptr, &graph_payload, &error.value);
    require(graph_open_status == FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE,
            std::to_string(graph_open_status));
    require(graph_payload == nullptr);
    require(owned_error(graph_open_status, error.value));
    require(graph_external_context.retains == UINT32_C(1));
    require(graph_external_context.releases == UINT32_C(1));
    error.clear();
    fdb_payload_v1_spec_release(graph_spec);

    fdb_payload_v1_payload_t* opened = nullptr;
    fdb_payload_v1_open_options_t open{};
    fdb_payload_v1_open_options_init(&open);
    require(fdb_payload_v1_payload_open_copy(
                spec, fdb_payload_v1_blob_data(binary),
                fdb_payload_v1_blob_size(binary), &open, &opened,
                &error.value) == UINT32_C(0));
    require(opened != nullptr);

    struct FutureOpen final {
        fdb_payload_v1_open_options_t known;
        std::array<std::uint8_t, 16> tail;
    } future_open{};
    fdb_payload_v1_open_options_init(&future_open.known);
    future_open.known.struct_size = sizeof(future_open);
    future_open.known.flags = UINT32_C(0);
    future_open.tail.fill(UINT8_C(0xa5));
    fdb_payload_v1_payload_t* lazy_opened = nullptr;
    require(fdb_payload_v1_payload_open_copy(
                spec, fdb_payload_v1_blob_data(binary),
                fdb_payload_v1_blob_size(binary), &future_open.known,
                &lazy_opened, &error.value) == UINT32_C(0));
    require(lazy_opened != nullptr);
    require(std::all_of(future_open.tail.begin(), future_open.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_payload_release(lazy_opened);

    fdb_payload_v1_execution_report_init(&queried_report);
    queried_report.mode = UINT32_MAX;
    queried_report.used_bytes = UINT64_MAX;
    const auto missing_report = fdb_payload_v1_payload_execution_report(
        opened, &queried_report, &error.value);
    require(owned_error(missing_report, error.value));
    require(missing_report == FDB_PAYLOAD_E_PLAN_STATE);
    require(queried_report.mode == UINT32_C(0));
    require(queried_report.used_bytes == UINT64_C(0));
    require(error_details(error.value) ==
            R"({"reason":"opened_payload_has_no_execution_report"})");
    error.clear();

    BackingContext external_context;
    external_context.bytes.assign(fdb_payload_v1_blob_data(binary),
                                  fdb_payload_v1_blob_data(binary) +
                                      fdb_payload_v1_blob_size(binary));
    auto external_backing = backing_for(external_context);
    fdb_payload_v1_payload_t* external = nullptr;
    require(fdb_payload_v1_payload_open_external(
                spec, external_context.bytes.data(),
                external_context.bytes.size(), &external_backing,
                &external_context, nullptr, &external,
                &error.value) == UINT32_C(0));
    require(external != nullptr && external_context.retains == UINT32_C(1));
    fdb_payload_v1_payload_retain(external);
    fdb_payload_v1_payload_release(external);
    require(external_context.releases == UINT32_C(0));
    require(fdb_payload_v1_payload_invalidate(external, &error.value) ==
            UINT32_C(0));
    require(external_context.releases == UINT32_C(1));
    require(fdb_payload_v1_payload_invalidate(external, &error.value) ==
            UINT32_C(0));
    require(external_context.releases == UINT32_C(1));
    fdb_payload_v1_payload_release(external);

    BackingContext external_only_context;
    external_only_context.bytes.assign(fdb_payload_v1_blob_data(binary),
                                       fdb_payload_v1_blob_data(binary) +
                                           fdb_payload_v1_blob_size(binary));
    fdb_payload_v1_backing_v1_t external_only_backing{};
    fdb_payload_v1_backing_init(&external_only_backing);
    external_only_backing.struct_size = static_cast<std::uint32_t>(
        offsetof(fdb_payload_v1_backing_v1_t, release) +
        sizeof(fdb_payload_v1_backing_release_fn));
    external_only_backing.context = &external_only_context;
    external_only_backing.retain = retain_backing;
    external_only_backing.release = release_backing;
    fdb_payload_v1_payload_t* external_only = nullptr;
    require(fdb_payload_v1_payload_open_external(
                spec, external_only_context.bytes.data(),
                external_only_context.bytes.size(), &external_only_backing,
                &external_only_context, nullptr, &external_only,
                &error.value) == UINT32_C(0));
    require(external_only != nullptr);
    fdb_payload_v1_payload_release(external_only);
    require(external_only_context.retains == UINT32_C(1));
    require(external_only_context.releases == UINT32_C(1));

    BackingContext invalid_external_context;
    invalid_external_context.bytes.assign(
        fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_data(binary) + fdb_payload_v1_blob_size(binary));
    invalid_external_context.bytes.front() ^= UINT8_C(0xff);
    auto invalid_external_backing = external_only_backing;
    invalid_external_backing.context = &invalid_external_context;
    external_only = reinterpret_cast<fdb_payload_v1_payload_t*>(
        std::uintptr_t{1});
    const auto invalid_external_status =
        fdb_payload_v1_payload_open_external(
            spec, invalid_external_context.bytes.data(),
            invalid_external_context.bytes.size(), &invalid_external_backing,
            &invalid_external_context, nullptr, &external_only,
            &error.value);
    require(invalid_external_status != UINT32_C(0));
    require(external_only == nullptr);
    require(owned_error(invalid_external_status, error.value));
    require(invalid_external_context.retains == UINT32_C(1));
    require(invalid_external_context.releases == UINT32_C(1));
    error.clear();

    require(fdb_payload_v1_payload_invalidate(payload, &error.value) ==
            UINT32_C(0));
    digest.fill(UINT8_C(0));
    require(fdb_payload_v1_payload_sha256(
                payload, digest.data(), &error.value) == UINT32_C(0));
    require(digest == payload_digest);
    profile = UINT32_C(0);
    require(fdb_payload_v1_payload_profile(
                payload, &profile, &error.value) == UINT32_C(0));
    require(profile == FDB_PAYLOAD_PROFILE_RECORD_V1);
    fdb_payload_v1_execution_report_init(&queried_report);
    require(fdb_payload_v1_payload_execution_report(
                payload, &queried_report, &error.value) == UINT32_C(0));
    require(queried_report.used_bytes == report.used_bytes);
    fdb_payload_v1_blob_t* invalidated_blob =
        reinterpret_cast<fdb_payload_v1_blob_t*>(std::uintptr_t{1});
    const auto invalidated_blob_status = fdb_payload_v1_payload_binary_blob(
        payload, &invalidated_blob, &error.value);
    require(invalidated_blob_status == FDB_PAYLOAD_E_VIEW_INVALIDATED);
    require(invalidated_blob == nullptr);
    require(owned_error(invalidated_blob_status, error.value));
    error.clear();
    require(fdb_payload_v1_blob_size(binary) == info.total_bytes);
    fdb_payload_v1_payload_release(payload);
    fdb_payload_v1_payload_release(opened);

    BackingContext direct_context;
    auto direct_backing = backing_for(direct_context);
    fdb_payload_v1_payload_t* direct_payload = nullptr;
    fdb_payload_v1_execution_report_t direct_report{};
    fdb_payload_v1_execution_report_init(&direct_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &direct_backing,
                &direct_payload, &direct_report, &error.value) ==
            UINT32_C(0));
    require(direct_payload != nullptr);
    require(direct_report.mode == FDB_PAYLOAD_EXECUTION_DIRECT);
    require(direct_context.calls ==
            std::vector<std::uint32_t>({UINT32_C(11), UINT32_C(30)}));
    require(direct_context.releases == UINT32_C(0));
    require(fdb_payload_v1_payload_invalidate(
                direct_payload, &error.value) == UINT32_C(0));
    require(direct_context.releases == UINT32_C(1));
    fdb_payload_v1_payload_release(direct_payload);

    BackingContext future_backing_context;
    struct FutureBacking final {
        fdb_payload_v1_backing_v1_t known;
        std::array<std::uint8_t, 16> tail;
    } future_backing{};
    future_backing.known = backing_for(future_backing_context);
    future_backing.known.struct_size = sizeof(future_backing);
    future_backing.tail.fill(UINT8_C(0xa5));
    fdb_payload_v1_payload_t* future_backed_payload = nullptr;
    fdb_payload_v1_execution_report_t future_backing_report{};
    fdb_payload_v1_execution_report_init(&future_backing_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT,
                &future_backing.known, &future_backed_payload,
                &future_backing_report, &error.value) == UINT32_C(0));
    require(future_backed_payload != nullptr);
    require(std::all_of(future_backing.tail.begin(),
                        future_backing.tail.end(), [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_payload_release(future_backed_payload);
    require(future_backing_context.releases == UINT32_C(1));

    BackingContext staged_context;
    staged_context.decline_direct = true;
    auto staged_backing = backing_for(staged_context);
    fdb_payload_v1_payload_t* staged_payload = nullptr;
    fdb_payload_v1_execution_report_t staged_report{};
    fdb_payload_v1_execution_report_init(&staged_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_ALLOW_STAGING, &staged_backing,
                &staged_payload, &staged_report, &error.value) ==
            UINT32_C(0));
    require(staged_payload != nullptr);
    require(staged_report.mode == FDB_PAYLOAD_EXECUTION_STAGED);
    require(staged_report.fallback_reason ==
            FDB_PAYLOAD_FALLBACK_BACKING_DECLINED_DIRECT);
    require(staged_context.calls == std::vector<std::uint32_t>(
                {UINT32_C(11), UINT32_C(12), UINT32_C(30)}));
    fdb_payload_v1_payload_release(staged_payload);
    require(staged_context.releases == UINT32_C(1));

    BackingContext declined_context;
    declined_context.decline_direct = true;
    auto declined_backing = backing_for(declined_context);
    fdb_payload_v1_payload_t* declined_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    fdb_payload_v1_execution_report_t declined_report{};
    fdb_payload_v1_execution_report_init(&declined_report);
    declined_report.mode = UINT32_MAX;
    const auto declined_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &declined_backing,
        &declined_payload, &declined_report, &error.value);
    require(declined_status == FDB_PAYLOAD_E_DIRECT_UNAVAILABLE);
    require(declined_payload == nullptr);
    require(declined_report.mode == UINT32_C(0));
    require(owned_error(declined_status, error.value));
    error.clear();
    require(declined_context.calls ==
            std::vector<std::uint32_t>({UINT32_C(11)}));
    require(declined_context.releases == UINT32_C(0));

    BackingContext range_context;
    range_context.provide_writable = false;
    auto range_backing = backing_for(range_context);
    range_backing.struct_size =
        static_cast<std::uint32_t>(
            offsetof(fdb_payload_v1_backing_v1_t, reserved));
    std::fill(std::begin(range_backing.reserved),
              std::end(range_backing.reserved), UINT64_MAX);
    fdb_payload_v1_payload_t* range_payload = nullptr;
    fdb_payload_v1_execution_report_t range_report{};
    fdb_payload_v1_execution_report_init(&range_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &range_backing,
                &range_payload, &range_report, &error.value) == UINT32_C(0));
    require(range_payload != nullptr);
    require(range_context.calls.front() == UINT32_C(11));
    require(range_context.calls.back() == UINT32_C(30));
    require(std::find(range_context.calls.begin(), range_context.calls.end(),
                      UINT32_C(20)) != range_context.calls.end());
    fdb_payload_v1_payload_release(range_payload);
    require(range_context.releases == UINT32_C(1));

    enum class CallbackFailureStep : std::uint8_t {
        direct_reserve,
        staged_reserve,
        write,
        commit,
        rollback,
    };
    struct CallbackFailureCase final {
        const char* name;
        CallbackFailureStep step;
        std::uint32_t injected_status;
        std::uint32_t expected_status;
    };
    static constexpr std::array<CallbackFailureCase, 13> failure_cases{{
        {"direct reserve allocation", CallbackFailureStep::direct_reserve,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"direct reserve unknown", CallbackFailureStep::direct_reserve,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"staged reserve allocation", CallbackFailureStep::staged_reserve,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"staged reserve direct unavailable",
         CallbackFailureStep::staged_reserve,
         FDB_PAYLOAD_E_DIRECT_UNAVAILABLE, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"staged reserve unknown", CallbackFailureStep::staged_reserve,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"write allocation", CallbackFailureStep::write,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"write unknown", CallbackFailureStep::write,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"commit allocation", CallbackFailureStep::commit,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"commit failed", CallbackFailureStep::commit,
         FDB_PAYLOAD_E_COMMIT_FAILED, FDB_PAYLOAD_E_COMMIT_FAILED},
        {"commit unknown", CallbackFailureStep::commit,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
        {"rollback allocation", CallbackFailureStep::rollback,
         FDB_PAYLOAD_E_ALLOCATION_FAILED, FDB_PAYLOAD_E_ROLLBACK_FAILED},
        {"rollback failed", CallbackFailureStep::rollback,
         FDB_PAYLOAD_E_ROLLBACK_FAILED, FDB_PAYLOAD_E_ROLLBACK_FAILED},
        {"rollback unknown", CallbackFailureStep::rollback,
         FDB_PAYLOAD_E_INTERNAL, FDB_PAYLOAD_E_BACKING_CONTRACT},
    }};
    for (const CallbackFailureCase& test_case : failure_cases) {
        BackingContext context;
        context.poison_reserve_outputs_on_failure = true;
        context.poison_commit_outputs_on_failure = true;
        std::uint32_t policy = FDB_PAYLOAD_BUILD_REQUIRE_DIRECT;
        std::vector<std::uint32_t> expected_calls;
        switch (test_case.step) {
        case CallbackFailureStep::direct_reserve:
            context.direct_reserve_status = test_case.injected_status;
            expected_calls = {UINT32_C(11)};
            break;
        case CallbackFailureStep::staged_reserve:
            context.decline_direct = true;
            context.staged_reserve_status = test_case.injected_status;
            policy = FDB_PAYLOAD_BUILD_ALLOW_STAGING;
            expected_calls = {UINT32_C(11), UINT32_C(12)};
            break;
        case CallbackFailureStep::write:
            context.provide_writable = false;
            context.write_status = test_case.injected_status;
            expected_calls = {UINT32_C(11), UINT32_C(20), UINT32_C(40)};
            break;
        case CallbackFailureStep::commit:
            context.commit_status = test_case.injected_status;
            expected_calls = {UINT32_C(11), UINT32_C(30), UINT32_C(40)};
            break;
        case CallbackFailureStep::rollback:
            context.provide_writable = false;
            context.write_status = FDB_PAYLOAD_E_ALLOCATION_FAILED;
            context.rollback_status = test_case.injected_status;
            expected_calls = {UINT32_C(11), UINT32_C(20), UINT32_C(40)};
            break;
        }
        auto callback_table = backing_for(context);
        fdb_payload_v1_payload_t* failed_payload =
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
        fdb_payload_v1_execution_report_t failed_report{};
        fdb_payload_v1_execution_report_init(&failed_report);
        failed_report.mode = UINT32_MAX;
        const auto failure_status = fdb_payload_v1_plan_execute(
            plan, policy, &callback_table, &failed_payload, &failed_report,
            &error.value);
        require(failure_status == test_case.expected_status, test_case.name);
        require(owned_error(failure_status, error.value), test_case.name);
        require(failed_payload == nullptr, test_case.name);
        require(failed_report.mode == UINT32_C(0), test_case.name);
        require(context.calls == expected_calls, test_case.name);
        require(context.releases == UINT32_C(0), test_case.name);
        require(context.tokens_match, test_case.name);
        error.clear();
    }

    struct RetainFailureCase final {
        const char* name;
        std::uint32_t injected_status;
        std::uint32_t expected_status;
    };
    static constexpr std::array<RetainFailureCase, 2> retain_failures{{
        {"retain allocation", FDB_PAYLOAD_E_ALLOCATION_FAILED,
         FDB_PAYLOAD_E_ALLOCATION_FAILED},
        {"retain unknown", FDB_PAYLOAD_E_INTERNAL,
         FDB_PAYLOAD_E_BACKING_CONTRACT},
    }};
    for (const RetainFailureCase& test_case : retain_failures) {
        BackingContext context;
        context.bytes.assign(fdb_payload_v1_blob_data(binary),
                             fdb_payload_v1_blob_data(binary) +
                                 fdb_payload_v1_blob_size(binary));
        context.retain_status = test_case.injected_status;
        auto callback_table = backing_for(context);
        fdb_payload_v1_payload_t* retained_payload =
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
        const auto failure_status = fdb_payload_v1_payload_open_external(
            spec, context.bytes.data(), context.bytes.size(), &callback_table,
            &context, nullptr, &retained_payload, &error.value);
        require(failure_status == test_case.expected_status, test_case.name);
        require(owned_error(failure_status, error.value), test_case.name);
        require(retained_payload == nullptr, test_case.name);
        require(context.retains == UINT32_C(1), test_case.name);
        require(context.releases == UINT32_C(0), test_case.name);
        require(context.tokens_match, test_case.name);
        error.clear();
    }

    fdb_payload_v1_payload_t* failed_payload = nullptr;
    fdb_payload_v1_execution_report_t failed_report{};
    auto failure_status = UINT32_C(0);

    enum class RequiredBuildCallback : std::uint8_t {
        reserve,
        commit,
        rollback,
        release,
    };
    static constexpr std::array<RequiredBuildCallback, 4>
        required_build_callbacks{{
            RequiredBuildCallback::reserve,
            RequiredBuildCallback::commit,
            RequiredBuildCallback::rollback,
            RequiredBuildCallback::release,
        }};
    for (const RequiredBuildCallback missing : required_build_callbacks) {
        BackingContext context;
        auto callback_table = backing_for(context);
        switch (missing) {
        case RequiredBuildCallback::reserve:
            callback_table.reserve = nullptr;
            break;
        case RequiredBuildCallback::commit:
            callback_table.commit = nullptr;
            break;
        case RequiredBuildCallback::rollback:
            callback_table.rollback = nullptr;
            break;
        case RequiredBuildCallback::release:
            callback_table.release = nullptr;
            break;
        }
        failed_payload = reinterpret_cast<fdb_payload_v1_payload_t*>(
            std::uintptr_t{1});
        fdb_payload_v1_execution_report_init(&failed_report);
        failure_status = fdb_payload_v1_plan_execute(
            plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &callback_table,
            &failed_payload, &failed_report, &error.value);
        require(failure_status == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(owned_error(failure_status, error.value));
        require(failed_payload == nullptr);
        require(context.calls.empty());
        require(context.retains == UINT32_C(0));
        require(context.releases == UINT32_C(0));
        error.clear();
    }

    BackingContext missing_write_context;
    missing_write_context.provide_writable = false;
    auto missing_write_table = backing_for(missing_write_context);
    missing_write_table.write = nullptr;
    failed_payload = reinterpret_cast<fdb_payload_v1_payload_t*>(
        std::uintptr_t{1});
    fdb_payload_v1_execution_report_init(&failed_report);
    failure_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &missing_write_table,
        &failed_payload, &failed_report, &error.value);
    require(failure_status == FDB_PAYLOAD_E_BACKING_CONTRACT);
    require(failed_payload == nullptr);
    require(missing_write_context.calls ==
            std::vector<std::uint32_t>({UINT32_C(11), UINT32_C(40)}));
    require(missing_write_context.releases == UINT32_C(0));
    require(missing_write_context.tokens_match);
    error.clear();

    for (const bool missing_retain : {true, false}) {
        BackingContext context;
        context.bytes.assign(fdb_payload_v1_blob_data(binary),
                             fdb_payload_v1_blob_data(binary) +
                                 fdb_payload_v1_blob_size(binary));
        auto callback_table = backing_for(context);
        if (missing_retain) {
            callback_table.retain = nullptr;
        } else {
            callback_table.release = nullptr;
        }
        failed_payload = reinterpret_cast<fdb_payload_v1_payload_t*>(
            std::uintptr_t{1});
        failure_status = fdb_payload_v1_payload_open_external(
            spec, context.bytes.data(), context.bytes.size(), &callback_table,
            &context, nullptr, &failed_payload, &error.value);
        require(failure_status == FDB_PAYLOAD_E_BACKING_CONTRACT);
        require(owned_error(failure_status, error.value));
        require(failed_payload == nullptr);
        require(context.calls.empty());
        require(context.retains == UINT32_C(0));
        require(context.releases == UINT32_C(0));
        error.clear();
    }

    BackingContext null_build_token_context;
    null_build_token_context.null_owner_token = true;
    auto null_build_token_table = backing_for(null_build_token_context);
    fdb_payload_v1_payload_t* null_token_payload = nullptr;
    fdb_payload_v1_execution_report_t null_token_report{};
    fdb_payload_v1_execution_report_init(&null_token_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT,
                &null_build_token_table, &null_token_payload,
                &null_token_report, &error.value) == UINT32_C(0));
    require(null_token_payload != nullptr);
    require(null_build_token_context.tokens_match);
    fdb_payload_v1_payload_release(null_token_payload);
    require(null_build_token_context.releases == UINT32_C(1));

    BackingContext null_external_token_context;
    null_external_token_context.null_owner_token = true;
    null_external_token_context.bytes.assign(
        fdb_payload_v1_blob_data(binary),
        fdb_payload_v1_blob_data(binary) + fdb_payload_v1_blob_size(binary));
    auto null_external_token_table = backing_for(null_external_token_context);
    null_token_payload = nullptr;
    require(fdb_payload_v1_payload_open_external(
                spec, null_external_token_context.bytes.data(),
                null_external_token_context.bytes.size(),
                &null_external_token_table, nullptr, nullptr,
                &null_token_payload, &error.value) == UINT32_C(0));
    require(null_token_payload != nullptr);
    require(null_external_token_context.retains == UINT32_C(1));
    require(null_external_token_context.tokens_match);
    fdb_payload_v1_payload_release(null_token_payload);
    require(null_external_token_context.releases == UINT32_C(1));

    BackingContext relocated_context;
    relocated_context.relocate_on_commit = true;
    auto relocated_table = backing_for(relocated_context);
    fdb_payload_v1_payload_t* relocated_payload = nullptr;
    fdb_payload_v1_execution_report_t relocated_report{};
    fdb_payload_v1_execution_report_init(&relocated_report);
    require(fdb_payload_v1_plan_execute(
                plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &relocated_table,
                &relocated_payload, &relocated_report,
                &error.value) == UINT32_C(0));
    require(relocated_payload != nullptr);
    require(relocated_context.bytes.data() !=
            relocated_context.relocated_bytes.data());
    fdb_payload_v1_blob_t* relocated_blob = nullptr;
    require(fdb_payload_v1_payload_binary_blob(
                relocated_payload, &relocated_blob,
                &error.value) == UINT32_C(0));
    require(relocated_blob != nullptr);
    require(fdb_payload_v1_blob_size(relocated_blob) ==
            fdb_payload_v1_blob_size(binary));
    require(std::equal(
        fdb_payload_v1_blob_data(relocated_blob),
        fdb_payload_v1_blob_data(relocated_blob) +
            fdb_payload_v1_blob_size(relocated_blob),
        fdb_payload_v1_blob_data(binary)));
    fdb_payload_v1_blob_release(relocated_blob);
    fdb_payload_v1_payload_release(relocated_payload);
    require(relocated_context.releases == UINT32_C(1));
    require(relocated_context.tokens_match);

    BackingContext rejected_table_context;
    auto rejected_table = backing_for(rejected_table_context);
    rejected_table.flags = UINT32_C(1);
    fdb_payload_v1_execution_report_init(&failed_report);
    failure_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &rejected_table,
        &failed_payload, &failed_report, &error.value);
    require(failure_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_table_context.calls.empty());
    error.clear();
    rejected_table = backing_for(rejected_table_context);
    rejected_table.reserved[0] = UINT64_C(1);
    fdb_payload_v1_execution_report_init(&failed_report);
    failure_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &rejected_table,
        &failed_payload, &failed_report, &error.value);
    require(failure_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(rejected_table_context.calls.empty());
    error.clear();
    rejected_table = backing_for(rejected_table_context);
    rejected_table.struct_size = static_cast<std::uint32_t>(
        offsetof(fdb_payload_v1_backing_v1_t, reserved) +
        sizeof(rejected_table.reserved[0]));
    rejected_table.reserved[0] = UINT64_C(1);
    fdb_payload_v1_execution_report_init(&failed_report);
    failure_status = fdb_payload_v1_plan_execute(
        plan, FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, &rejected_table,
        &failed_payload, &failed_report, &error.value);
    require(failure_status == FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(failed_payload == nullptr);
    require(rejected_table_context.calls.empty());
    error.clear();

    fdb_payload_v1_blob_release(binary);
    fdb_payload_v1_plan_release(plan);
    fdb_payload_v1_spec_release(spec);
    return EXIT_SUCCESS;
}

int test_checked_view_access_materialize_and_barrier_abi() {
    RuntimeFixture fixture;
    require(fixture.initialize() == EXIT_SUCCESS);
    ErrorRef error;

    fdb_payload_v1_access_t* cleared_access =
        reinterpret_cast<fdb_payload_v1_access_t*>(std::uintptr_t{1});
    auto clearing_status = fdb_payload_v1_payload_acquire(
        nullptr, &cleared_access, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_access == nullptr && owned_error(clearing_status,
                                                     error.value));
    error.clear();
    fdb_payload_v1_view_t* cleared_view =
        reinterpret_cast<fdb_payload_v1_view_t*>(std::uintptr_t{1});
    clearing_status = fdb_payload_v1_payload_entry_view(
        nullptr, UINT32_C(0), &cleared_view, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_view == nullptr);
    error.clear();
    std::uint32_t cleared_u32 = UINT32_MAX;
    clearing_status =
        fdb_payload_v1_view_kind(nullptr, &cleared_u32, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_u32 == UINT32_C(0));
    error.clear();
    const std::uint8_t* cleared_bytes =
        reinterpret_cast<const std::uint8_t*>(std::uintptr_t{1});
    std::uint64_t cleared_size = UINT64_MAX;
    clearing_status = fdb_payload_v1_access_payload_bytes(
        nullptr, &cleared_bytes, &cleared_size, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_bytes == nullptr && cleared_size == UINT64_C(0));
    error.clear();

    fdb_payload_v1_capabilities_t capabilities{};
    fdb_payload_v1_capabilities_init(&capabilities);
    require(fdb_payload_v1_spec_capabilities(
                fixture.spec, &capabilities, &error.value) == UINT32_C(0));
    require(capabilities.operation_flags ==
            (FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY |
             FDB_PAYLOAD_OPERATION_BUILD | FDB_PAYLOAD_OPERATION_OPEN |
             FDB_PAYLOAD_OPERATION_VIEW |
             FDB_PAYLOAD_OPERATION_MATERIALIZE |
             FDB_PAYLOAD_OPERATION_INVALIDATE));

    fdb_payload_v1_access_t* payload_access = nullptr;
    require(fdb_payload_v1_payload_acquire(
                fixture.payload, &payload_access, &error.value) ==
            UINT32_C(0));
    require(payload_access != nullptr && error.value == nullptr);
    const std::uint8_t* payload_bytes = nullptr;
    std::uint64_t payload_size = UINT64_C(0);
    require(fdb_payload_v1_access_payload_bytes(
                payload_access, &payload_bytes, &payload_size,
                &error.value) == UINT32_C(0));
    require(payload_bytes != nullptr && payload_size > UINT64_C(0));
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        require(payload_size <= static_cast<std::uint64_t>(
                                    std::numeric_limits<std::size_t>::max()));
    }
    const std::size_t payload_native_size =
        static_cast<std::size_t>(payload_size);
    const std::uint8_t* wrong_bytes =
        reinterpret_cast<const std::uint8_t*>(std::uintptr_t{1});
    std::uint64_t wrong_size = UINT64_MAX;
    const auto wrong_payload_access = fdb_payload_v1_access_str(
        payload_access, &wrong_bytes, &wrong_size, &error.value);
    require(wrong_payload_access == FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(wrong_bytes == nullptr && wrong_size == UINT64_C(0));
    require(owned_error(wrong_payload_access, error.value));
    error.clear();

    const auto entry = [&error](const fdb_payload_v1_payload_t* payload,
                                std::uint32_t index)
        -> fdb_payload_v1_view_t* {
        fdb_payload_v1_view_t* result = nullptr;
        if (fdb_payload_v1_payload_entry_view(
                payload, index, &result, &error.value) != UINT32_C(0) ||
            result == nullptr || error.value != nullptr) {
            return nullptr;
        }
        return result;
    };
    const auto at = [&error](const fdb_payload_v1_view_t* view,
                             std::uint64_t index)
        -> fdb_payload_v1_view_t* {
        fdb_payload_v1_view_t* result = nullptr;
        if (fdb_payload_v1_view_at(
                view, index, &result, &error.value) != UINT32_C(0) ||
            result == nullptr || error.value != nullptr) {
            return nullptr;
        }
        return result;
    };
    const auto entry_at = [&entry, &at](const fdb_payload_v1_payload_t* payload,
                                        std::uint32_t entry_index,
                                        std::uint64_t value_index) {
        fdb_payload_v1_view_t* sequence = entry(payload, entry_index);
        fdb_payload_v1_view_t* result = at(sequence, value_index);
        fdb_payload_v1_view_release(sequence);
        return result;
    };

    fdb_payload_v1_view_t* bool_sequence = entry(fixture.payload, UINT32_C(0));
    std::uint32_t kind = UINT32_C(0);
    std::uint8_t is_null = UINT8_C(1);
    std::uint64_t length = UINT64_C(0);
    require(fdb_payload_v1_view_kind(
                bool_sequence, &kind, &error.value) == UINT32_C(0));
    require(kind == FDB_PAYLOAD_VIEW_SEQUENCE);
    require(fdb_payload_v1_view_is_null(
                bool_sequence, &is_null, &error.value) == UINT32_C(0));
    require(is_null == UINT8_C(0));
    require(fdb_payload_v1_view_length(
                bool_sequence, &length, &error.value) == UINT32_C(0));
    require(length == UINT64_C(1));
    fdb_payload_v1_view_t* scalar = at(bool_sequence, UINT64_C(0));
    require(fdb_payload_v1_view_kind(
                scalar, &kind, &error.value) == UINT32_C(0));
    require(kind == FDB_PAYLOAD_VIEW_BOOL);
    std::uint8_t bool_value = UINT8_C(0);
    require(fdb_payload_v1_view_get_bool(
                scalar, &bool_value, &error.value) == UINT32_C(0));
    require(bool_value == UINT8_C(1));
    std::uint8_t u8_value = UINT8_MAX;
    auto status = fdb_payload_v1_view_get_u8(
        scalar, &u8_value, &error.value);
    require(status == FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(u8_value == UINT8_C(0));
    require(owned_error(status, error.value));
    error.clear();
    fdb_payload_v1_view_release(scalar);
    fdb_payload_v1_view_release(bool_sequence);

    scalar = entry_at(fixture.payload, UINT32_C(1), UINT64_C(0));
    require(fdb_payload_v1_view_get_u8(
                scalar, &u8_value, &error.value) == UINT32_C(0));
    require(u8_value == UINT8_C(8));
    fdb_payload_v1_view_release(scalar);

    scalar = entry_at(fixture.payload, UINT32_C(2), UINT64_C(0));
    std::uint16_t u16_value = UINT16_C(0);
    require(fdb_payload_v1_view_get_u16(
                scalar, &u16_value, &error.value) == UINT32_C(0));
    require(u16_value == UINT16_C(16));
    fdb_payload_v1_view_release(scalar);

    scalar = entry_at(fixture.payload, UINT32_C(3), UINT64_C(0));
    std::uint32_t u32_value = UINT32_C(0);
    require(fdb_payload_v1_view_get_u32(
                scalar, &u32_value, &error.value) == UINT32_C(0));
    require(u32_value == UINT32_C(32));
    fdb_payload_v1_view_release(scalar);

    scalar = entry_at(fixture.payload, UINT32_C(4), UINT64_C(0));
    std::int32_t i32_value = INT32_C(0);
    require(fdb_payload_v1_view_get_i32(
                scalar, &i32_value, &error.value) == UINT32_C(0));
    require(i32_value == INT32_C(-32));
    fdb_payload_v1_view_release(scalar);

    scalar = entry_at(fixture.payload, UINT32_C(5), UINT64_C(0));
    std::uint64_t f64_bits = UINT64_MAX;
    require(fdb_payload_v1_view_get_u8n_f64_bits(
                scalar, &f64_bits, &error.value) == UINT32_C(0));
    require(f64_bits != UINT64_MAX);
    fdb_payload_v1_view_release(scalar);

    scalar = entry_at(fixture.payload, UINT32_C(6), UINT64_C(0));
    f64_bits = UINT64_MAX;
    require(fdb_payload_v1_view_get_u16n_f64_bits(
                scalar, &f64_bits, &error.value) == UINT32_C(0));
    require(f64_bits != UINT64_MAX);
    fdb_payload_v1_view_release(scalar);

    scalar = entry_at(fixture.payload, UINT32_C(7), UINT64_C(0));
    std::uint32_t f32_bits = UINT32_C(0);
    require(fdb_payload_v1_view_get_f32_bits(
                scalar, &f32_bits, &error.value) == UINT32_C(0));
    require(f32_bits == UINT32_C(0x3fc00000));
    fdb_payload_v1_view_release(scalar);

    scalar = entry_at(fixture.payload, UINT32_C(8), UINT64_C(0));
    f64_bits = UINT64_C(0);
    require(fdb_payload_v1_view_get_f64_bits(
                scalar, &f64_bits, &error.value) == UINT32_C(0));
    require(f64_bits == UINT64_C(0x4004000000000000));
    fdb_payload_v1_view_release(scalar);

    fdb_payload_v1_view_t* text =
        entry_at(fixture.payload, UINT32_C(9), UINT64_C(0));
    fdb_payload_v1_access_t* text_access = nullptr;
    require(fdb_payload_v1_view_acquire(
                text, &text_access, &error.value) == UINT32_C(0));
    const std::uint8_t* text_data = nullptr;
    std::uint64_t text_size = UINT64_C(0);
    require(fdb_payload_v1_access_str(
                text_access, &text_data, &text_size, &error.value) ==
            UINT32_C(0));
    require(text_size == UINT64_C(3));
    require(std::memcmp(text_data, "abc", 3U) == 0);
    require(byte_span_contains_address(
        payload_bytes, payload_native_size, text_data));
    wrong_bytes = reinterpret_cast<const std::uint8_t*>(std::uintptr_t{1});
    wrong_size = UINT64_MAX;
    status = fdb_payload_v1_access_bytes(
        text_access, &wrong_bytes, &wrong_size, &error.value);
    require(status == FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(wrong_bytes == nullptr && wrong_size == UINT64_C(0));
    error.clear();

    fdb_payload_v1_view_t* wide =
        entry_at(fixture.payload, UINT32_C(10), UINT64_C(0));
    fdb_payload_v1_access_t* wide_access = nullptr;
    require(fdb_payload_v1_view_acquire(
                wide, &wide_access, &error.value) == UINT32_C(0));
    const std::uint16_t* wide_data = nullptr;
    std::uint64_t wide_size = UINT64_C(0);
    require(fdb_payload_v1_access_wstr(
                wide_access, &wide_data, &wide_size, &error.value) ==
            UINT32_C(0));
    require(wide_size == UINT64_C(2));
    require(wide_data[0] == UINT16_C(0x0041));
    require(wide_data[1] == UINT16_C(0x03a9));
    require(reinterpret_cast<std::uintptr_t>(wide_data) %
                alignof(std::uint16_t) == std::uintptr_t{0});
    const auto* wide_as_bytes =
        reinterpret_cast<const std::uint8_t*>(wide_data);
    require(!byte_spans_share_address(
        wide_as_bytes, static_cast<std::size_t>(wide_size) *
                           sizeof(std::uint16_t),
        payload_bytes, payload_native_size));

    fdb_payload_v1_view_t* opaque =
        entry_at(fixture.payload, UINT32_C(11), UINT64_C(0));
    fdb_payload_v1_access_t* opaque_access = nullptr;
    require(fdb_payload_v1_view_acquire(
                opaque, &opaque_access, &error.value) == UINT32_C(0));
    const std::uint8_t* opaque_data = nullptr;
    std::uint64_t opaque_size = UINT64_C(0);
    require(fdb_payload_v1_access_bytes(
                opaque_access, &opaque_data, &opaque_size, &error.value) ==
            UINT32_C(0));
    require(opaque_size == UINT64_C(3));
    require(opaque_data[0] == UINT8_C(0) &&
            opaque_data[1] == UINT8_C(1) &&
            opaque_data[2] == UINT8_C(255));
    require(byte_span_contains_address(
        payload_bytes, payload_native_size, opaque_data));

    fdb_payload_v1_view_t* component =
        entry_at(fixture.payload, UINT32_C(12), UINT64_C(0));
    require(fdb_payload_v1_view_kind(
                component, &kind, &error.value) == UINT32_C(0));
    require(kind == FDB_PAYLOAD_VIEW_COMPONENT);
    std::uint32_t component_index = UINT32_MAX;
    std::uint32_t field_count = UINT32_C(0);
    require(fdb_payload_v1_view_component_index(
                component, &component_index, &error.value) == UINT32_C(0));
    require(component_index == UINT32_C(0));
    require(fdb_payload_v1_view_field_count(
                component, &field_count, &error.value) == UINT32_C(0));
    require(field_count == UINT32_C(2));
    fdb_payload_v1_view_t* field = nullptr;
    require(fdb_payload_v1_view_field(
                component, UINT32_C(1), &field, &error.value) == UINT32_C(0));
    require(fdb_payload_v1_view_get_u8(
                field, &u8_value, &error.value) == UINT32_C(0));
    require(u8_value == UINT8_C(2));
    fdb_payload_v1_view_release(field);

    fdb_payload_v1_view_t* list =
        entry_at(fixture.payload, UINT32_C(13), UINT64_C(0));
    require(fdb_payload_v1_view_kind(
                list, &kind, &error.value) == UINT32_C(0));
    require(kind == FDB_PAYLOAD_VIEW_LIST);
    require(fdb_payload_v1_view_length(
                list, &length, &error.value) == UINT32_C(0));
    require(length == UINT64_C(2));
    scalar = at(list, UINT64_C(1));
    require(fdb_payload_v1_view_get_u8(
                scalar, &u8_value, &error.value) == UINT32_C(0));
    require(u8_value == UINT8_C(4));
    fdb_payload_v1_view_release(scalar);

    fdb_payload_v1_view_t* nullable_sequence =
        entry(fixture.payload, UINT32_C(14));
    fdb_payload_v1_view_t* null_value = at(nullable_sequence, UINT64_C(1));
    require(fdb_payload_v1_view_is_null(
                null_value, &is_null, &error.value) == UINT32_C(0));
    require(is_null == UINT8_C(1));
    u16_value = UINT16_MAX;
    status = fdb_payload_v1_view_get_u16(
        null_value, &u16_value, &error.value);
    require(status == FDB_PAYLOAD_E_UNEXPECTED_NULL);
    require(u16_value == UINT16_C(0));
    require(owned_error(status, error.value));
    error.clear();

    fdb_payload_v1_view_t* no_child =
        reinterpret_cast<fdb_payload_v1_view_t*>(std::uintptr_t{1});
    status = fdb_payload_v1_view_at(
        nullable_sequence, UINT64_C(3), &no_child, &error.value);
    require(status == FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    require(no_child == nullptr && owned_error(status, error.value));
    error.clear();
    no_child = reinterpret_cast<fdb_payload_v1_view_t*>(std::uintptr_t{1});
    status = fdb_payload_v1_view_field(
        component, UINT32_C(2), &no_child, &error.value);
    require(status == FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    require(no_child == nullptr && owned_error(status, error.value));
    error.clear();
    length = UINT64_MAX;
    status = fdb_payload_v1_view_length(
        component, &length, &error.value);
    require(status == FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(length == UINT64_C(0));
    error.clear();

    fdb_payload_v1_view_t* detached = nullptr;
    require(fdb_payload_v1_view_materialize(
                component, &detached, &error.value) == UINT32_C(0));
    require(detached != nullptr && error.value == nullptr);
    fdb_payload_v1_view_t* detached_again = nullptr;
    require(fdb_payload_v1_view_materialize(
                detached, &detached_again, &error.value) == UINT32_C(0));
    require(detached_again != nullptr);

    std::atomic<bool> thread_queries_ok{true};
    std::vector<std::thread> workers;
    workers.reserve(32U);
    for (std::uint32_t worker = UINT32_C(0); worker < UINT32_C(32); ++worker) {
        workers.emplace_back([&fixture, component, &thread_queries_ok]() {
            for (std::uint32_t iteration = UINT32_C(0);
                 iteration < UINT32_C(100); ++iteration) {
                fdb_payload_v1_payload_retain(fixture.payload);
                fdb_payload_v1_view_retain(component);
                fdb_payload_v1_error_t* local_error = nullptr;
                std::uint32_t local_kind = UINT32_C(0);
                std::uint32_t local_fields = UINT32_C(0);
                if (fdb_payload_v1_view_kind(
                        component, &local_kind, &local_error) != UINT32_C(0) ||
                    local_error != nullptr ||
                    local_kind != FDB_PAYLOAD_VIEW_COMPONENT ||
                    fdb_payload_v1_view_field_count(
                        component, &local_fields, &local_error) !=
                        UINT32_C(0) ||
                    local_error != nullptr || local_fields != UINT32_C(2)) {
                    thread_queries_ok.store(false, std::memory_order_relaxed);
                }
                fdb_payload_v1_error_release(local_error);
                fdb_payload_v1_view_release(component);
                fdb_payload_v1_payload_release(fixture.payload);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(thread_queries_ok.load(std::memory_order_relaxed));

    fdb_payload_v1_access_release(text_access);
    text_access = nullptr;
    fdb_payload_v1_access_release(wide_access);
    wide_access = nullptr;
    fdb_payload_v1_access_release(opaque_access);
    opaque_access = nullptr;
    fdb_payload_v1_access_release(payload_access);
    payload_access = nullptr;

    BackingContext barrier_context;
    fdb_payload_v1_blob_t* binary = nullptr;
    require(fdb_payload_v1_payload_binary_blob(
                fixture.payload, &binary, &error.value) == UINT32_C(0));
    barrier_context.bytes.assign(fdb_payload_v1_blob_data(binary),
                                 fdb_payload_v1_blob_data(binary) +
                                     fdb_payload_v1_blob_size(binary));
    auto barrier_backing = backing_for(barrier_context);
    fdb_payload_v1_payload_t* barrier_payload = nullptr;
    require(fdb_payload_v1_payload_open_external(
                fixture.spec, barrier_context.bytes.data(),
                barrier_context.bytes.size(), &barrier_backing,
                &barrier_context, nullptr, &barrier_payload,
                &error.value) == UINT32_C(0));
    require(barrier_context.retains == UINT32_C(1));
    fdb_payload_v1_access_t* barrier_access = nullptr;
    require(fdb_payload_v1_payload_acquire(
                barrier_payload, &barrier_access, &error.value) ==
            UINT32_C(0));
    const std::uint8_t* external_data = nullptr;
    std::uint64_t external_size = UINT64_C(0);
    require(fdb_payload_v1_access_payload_bytes(
                barrier_access, &external_data, &external_size,
                &error.value) == UINT32_C(0));
    require(external_data == barrier_context.bytes.data());
    require(external_size == barrier_context.bytes.size());

    std::atomic<bool> invalidation_finished{false};
    std::uint32_t invalidation_status = UINT32_MAX;
    std::thread invalidator([&]() {
        fdb_payload_v1_error_t* local_error = nullptr;
        invalidation_status = fdb_payload_v1_payload_invalidate(
            barrier_payload, &local_error);
        if (local_error != nullptr) {
            thread_queries_ok.store(false, std::memory_order_relaxed);
        }
        fdb_payload_v1_error_release(local_error);
        invalidation_finished.store(true, std::memory_order_release);
    });
    AccessThreadCleanup invalidation_cleanup{barrier_access, invalidator};

    fastdb::payload::view::PayloadOwnerTestAccess::wait_until_invalidating(
        barrier_payload->value);
    fdb_payload_v1_view_t* rejected = nullptr;
    fdb_payload_v1_error_t* rejected_error = nullptr;
    const std::uint32_t rejected_status =
        fdb_payload_v1_payload_entry_view(
            barrier_payload, UINT32_C(0), &rejected, &rejected_error);
    const bool rejected_output_was_null = rejected == nullptr;
    const bool invalidation_was_blocked =
        !invalidation_finished.load(std::memory_order_acquire);
    const std::uint32_t releases_while_blocked = barrier_context.releases;
    fdb_payload_v1_view_release(rejected);
    fdb_payload_v1_error_release(rejected_error);
    invalidation_cleanup.release_and_join();

    require(rejected_status == FDB_PAYLOAD_E_VIEW_INVALIDATED);
    require(rejected_output_was_null);
    require(invalidation_was_blocked);
    require(releases_while_blocked == UINT32_C(0));
    require(invalidation_status == UINT32_C(0));
    require(invalidation_finished.load(std::memory_order_acquire));
    require(barrier_context.releases == UINT32_C(1));

    kind = UINT32_MAX;
    status = fdb_payload_v1_view_kind(component, &kind, &error.value);
    require(status == UINT32_C(0));
    require(kind == FDB_PAYLOAD_VIEW_COMPONENT);
    require(fdb_payload_v1_payload_invalidate(
                fixture.payload, &error.value) == UINT32_C(0));
    kind = UINT32_MAX;
    status = fdb_payload_v1_view_kind(component, &kind, &error.value);
    require(status == FDB_PAYLOAD_E_VIEW_INVALIDATED);
    require(kind == UINT32_C(0));
    require(owned_error(status, error.value));
    error.clear();
    require(fdb_payload_v1_view_kind(
                detached, &kind, &error.value) == UINT32_C(0));
    require(kind == FDB_PAYLOAD_VIEW_COMPONENT);
    require(fdb_payload_v1_view_field(
                detached, UINT32_C(1), &field, &error.value) == UINT32_C(0));
    require(fdb_payload_v1_view_get_u8(
                field, &u8_value, &error.value) == UINT32_C(0));
    require(u8_value == UINT8_C(2));
    fdb_payload_v1_view_release(field);

    fdb_payload_v1_blob_release(binary);
    fdb_payload_v1_payload_release(barrier_payload);
    fdb_payload_v1_view_release(detached_again);
    fdb_payload_v1_view_release(detached);
    fdb_payload_v1_view_release(null_value);
    fdb_payload_v1_view_release(nullable_sequence);
    fdb_payload_v1_view_release(list);
    fdb_payload_v1_view_release(component);
    fdb_payload_v1_access_release(opaque_access);
    fdb_payload_v1_view_release(opaque);
    fdb_payload_v1_access_release(wide_access);
    fdb_payload_v1_view_release(wide);
    fdb_payload_v1_access_release(text_access);
    fdb_payload_v1_view_release(text);
    return EXIT_SUCCESS;
}

int test_null_error_sink_rule() {
    fdb_payload_v1_spec_t* spec = compile_spec(kRecordSpec);
    require(spec != nullptr);
#define REQUIRE_NULL_SINK_REJECTED(...)                                      \
    require((__VA_ARGS__) == FDB_PAYLOAD_E_INVALID_ARGUMENT)

    fdb_payload_v1_builder_t* builder =
        reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_create(spec, nullptr, &builder, nullptr));
    require(builder ==
            reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1}));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_entry_begin(
        nullptr, UINT32_C(0), UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_null(nullptr, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_bool(nullptr, UINT8_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_u8(nullptr, UINT8_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_u16(nullptr, UINT16_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_u32(nullptr, UINT32_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_i32(nullptr, INT32_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_u8n_f64_bits(
        nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_u16n_f64_bits(
        nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_f32_bits(
        nullptr, UINT32_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_f64_bits(
        nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_str(
        nullptr, nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_wstr(
        nullptr, nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_bytes(
        nullptr, nullptr, UINT64_C(0), nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_fixed_run(
        nullptr, nullptr, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_value_component_begin(nullptr, nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_builder_value_list_begin(
        nullptr, UINT64_C(0), nullptr));
    fdb_payload_v1_plan_t* plan =
        reinterpret_cast<fdb_payload_v1_plan_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_builder_freeze(nullptr, &plan, nullptr));
    require(plan ==
            reinterpret_cast<fdb_payload_v1_plan_t*>(std::uintptr_t{1}));

    fdb_payload_v1_plan_info_t info{};
    fdb_payload_v1_plan_info_init(&info);
    info.total_bytes = UINT64_MAX;
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_plan_info(nullptr, &info, nullptr));
    require(info.total_bytes == UINT64_MAX);
    fdb_payload_v1_payload_t* payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    fdb_payload_v1_execution_report_t report{};
    fdb_payload_v1_execution_report_init(&report);
    report.mode = UINT32_MAX;
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_plan_execute(
        nullptr, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, &payload, &report,
        nullptr));
    require(payload ==
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1}));
    require(report.mode == UINT32_MAX);

    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_payload_open_copy(
        spec, nullptr, UINT64_C(0), nullptr, &payload, nullptr));
    require(payload ==
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1}));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_payload_open_external(
        spec, nullptr, UINT64_C(0), nullptr, nullptr, nullptr, &payload,
        nullptr));
    require(payload ==
            reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1}));
    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
    digest.fill(UINT8_C(0xa5));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_sha256(nullptr, digest.data(), nullptr));
    require(std::all_of(digest.begin(), digest.end(), [](std::uint8_t value) {
        return value == UINT8_C(0xa5);
    }));
    fdb_payload_v1_profile_t profile = UINT32_MAX;
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_profile(nullptr, &profile, nullptr));
    require(profile == UINT32_MAX);
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_payload_execution_report(
        nullptr, &report, nullptr));
    require(report.mode == UINT32_MAX);
    fdb_payload_v1_blob_t* blob =
        reinterpret_cast<fdb_payload_v1_blob_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_binary_blob(nullptr, &blob, nullptr));
    require(blob ==
            reinterpret_cast<fdb_payload_v1_blob_t*>(std::uintptr_t{1}));
    fdb_payload_v1_access_t* access =
        reinterpret_cast<fdb_payload_v1_access_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_acquire(nullptr, &access, nullptr));
    require(access ==
            reinterpret_cast<fdb_payload_v1_access_t*>(std::uintptr_t{1}));
    fdb_payload_v1_view_t* view =
        reinterpret_cast<fdb_payload_v1_view_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_payload_entry_view(
        nullptr, UINT32_C(0), &view, nullptr));
    require(view ==
            reinterpret_cast<fdb_payload_v1_view_t*>(std::uintptr_t{1}));
    std::uint32_t view_u32 = UINT32_MAX;
    std::uint64_t view_u64 = UINT64_MAX;
    std::uint16_t view_u16 = UINT16_MAX;
    std::int32_t view_i32 = INT32_MAX;
    std::uint8_t view_u8 = UINT8_MAX;
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_kind(nullptr, &view_u32, nullptr));
    require(view_u32 == UINT32_MAX);
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_is_null(nullptr, &view_u8, nullptr));
    require(view_u8 == UINT8_MAX);
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_length(nullptr, &view_u64, nullptr));
    require(view_u64 == UINT64_MAX);
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_at(nullptr, UINT64_C(0), &view, nullptr));
    require(view ==
            reinterpret_cast<fdb_payload_v1_view_t*>(std::uintptr_t{1}));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_component_index(nullptr, &view_u32, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_field_count(nullptr, &view_u32, nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_view_field(
        nullptr, UINT32_C(0), &view, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_get_bool(nullptr, &view_u8, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_get_u8(nullptr, &view_u8, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_get_u16(nullptr, &view_u16, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_get_u32(nullptr, &view_u32, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_get_i32(nullptr, &view_i32, nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_view_get_u8n_f64_bits(
        nullptr, &view_u64, nullptr));
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_view_get_u16n_f64_bits(
        nullptr, &view_u64, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_get_f32_bits(nullptr, &view_u32, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_get_f64_bits(nullptr, &view_u64, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_acquire(nullptr, &access, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_view_materialize(nullptr, &view, nullptr));
    const std::uint8_t* byte_span =
        reinterpret_cast<const std::uint8_t*>(std::uintptr_t{1});
    const std::uint16_t* wide_span =
        reinterpret_cast<const std::uint16_t*>(std::uintptr_t{1});
    REQUIRE_NULL_SINK_REJECTED(fdb_payload_v1_access_payload_bytes(
        nullptr, &byte_span, &view_u64, nullptr));
    require(byte_span ==
            reinterpret_cast<const std::uint8_t*>(std::uintptr_t{1}));
    require(view_u64 == UINT64_MAX);
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_access_str(nullptr, &byte_span, &view_u64, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_access_wstr(nullptr, &wide_span, &view_u64, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_access_bytes(nullptr, &byte_span, &view_u64, nullptr));
    REQUIRE_NULL_SINK_REJECTED(
        fdb_payload_v1_payload_invalidate(nullptr, nullptr));

    fdb_payload_v1_spec_release(spec);
#undef REQUIRE_NULL_SINK_REJECTED
    return EXIT_SUCCESS;
}

int test_errors_prefixes_callbacks_and_graph_unavailable() {
    fdb_payload_v1_spec_t* graph = compile_spec(kObjectGraphSpec);
    require(graph != nullptr);
    ErrorRef error;
    fdb_payload_v1_builder_t* builder =
        reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1});
    const auto graph_status = fdb_payload_v1_builder_create(
        graph, nullptr, &builder, &error.value);
    require(graph_status == FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE);
    require(builder == nullptr);
    require(owned_error(graph_status, error.value));
    error.clear();
    fdb_payload_v1_spec_release(graph);

    static constexpr std::string_view builder_priority_details =
        R"({"argument":"builder","reason":"null_handle"})";
    auto priority_status = fdb_payload_v1_builder_value_str(
        nullptr, nullptr, UINT64_C(1), &error.value);
    require(priority_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(error_details(error.value) == builder_priority_details,
            std::string(error_details(error.value)));
    error.clear();
    priority_status = fdb_payload_v1_builder_value_wstr(
        nullptr, nullptr, UINT64_C(1), &error.value);
    require(priority_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(error_details(error.value) == builder_priority_details);
    error.clear();
    priority_status = fdb_payload_v1_builder_value_bytes(
        nullptr, nullptr, UINT64_C(1), &error.value);
    require(priority_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(error_details(error.value) == builder_priority_details);
    error.clear();

    fdb_payload_v1_spec_t* record = compile_spec(kRecordSpec);
    require(record != nullptr);
    builder = reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1});
    require(fdb_payload_v1_builder_create(record, nullptr, &builder, nullptr) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(builder ==
            reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1}));

    fdb_payload_v1_builder_options_t short_options{};
    fdb_payload_v1_builder_options_init(&short_options);
    short_options.struct_size = UINT32_C(4);
    builder = reinterpret_cast<fdb_payload_v1_builder_t*>(std::uintptr_t{1});
    require(fdb_payload_v1_builder_create(
                record, &short_options, &builder, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    require(builder == nullptr);
    error.clear();

    struct FutureOptions final {
        fdb_payload_v1_builder_options_t known;
        std::array<std::uint8_t, 16> tail;
    } future{};
    fdb_payload_v1_builder_options_init(&future.known);
    future.known.struct_size = sizeof(future);
    future.tail.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_builder_create(
                record, &future.known, &builder, &error.value) == UINT32_C(0));
    require(std::all_of(future.tail.begin(), future.tail.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0xa5);
                        }));
    fdb_payload_v1_builder_release(builder);

    fdb_payload_v1_builder_options_init(&future.known);
    future.known.flags = UINT32_C(1);
    require(fdb_payload_v1_builder_create(
                record, &future.known, &builder, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();
    fdb_payload_v1_builder_options_init(&future.known);
    future.known.reserved[2] = UINT64_C(1);
    require(fdb_payload_v1_builder_create(
                record, &future.known, &builder, &error.value) ==
            FDB_PAYLOAD_E_UNSUPPORTED_ABI);
    error.clear();

    require(fdb_payload_v1_builder_create(
                record, nullptr, &builder, &error.value) == UINT32_C(0));
    fdb_payload_v1_error_t* owned = nullptr;
    require(fdb_payload_v1_builder_entry_begin(
                builder, UINT32_C(1), UINT64_C(1), &owned) == UINT32_C(0));
    const auto mismatch =
        fdb_payload_v1_builder_value_bool(builder, UINT8_C(1), &owned);
    require(mismatch == FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(owned_error(mismatch, owned));
    fdb_payload_v1_error_release(owned);

    require(fdb_payload_v1_builder_value_str(
                builder, nullptr, UINT64_C(1), &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    fdb_payload_v1_builder_release(builder);

    fdb_payload_v1_spec_t* empty_spans = compile_spec(kEmptySpansSpec);
    require(empty_spans != nullptr);
    builder = nullptr;
    require(fdb_payload_v1_builder_create(
                empty_spans, nullptr, &builder, &error.value) == UINT32_C(0));
    require(fdb_payload_v1_builder_entry_begin(
                builder, UINT32_C(0), UINT64_C(1), &error.value) ==
            UINT32_C(0));
    require(fdb_payload_v1_builder_value_str(
                builder, nullptr, UINT64_C(1), &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    require(fdb_payload_v1_builder_value_str(
                builder, nullptr, UINT64_C(0), &error.value) == UINT32_C(0));
    require(fdb_payload_v1_builder_entry_begin(
                builder, UINT32_C(1), UINT64_C(1), &error.value) ==
            UINT32_C(0));
    require(fdb_payload_v1_builder_value_wstr(
                builder, nullptr, UINT64_C(1), &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    require(fdb_payload_v1_builder_value_wstr(
                builder, nullptr, UINT64_C(0), &error.value) == UINT32_C(0));
    require(fdb_payload_v1_builder_entry_begin(
                builder, UINT32_C(2), UINT64_C(1), &error.value) ==
            UINT32_C(0));
    require(fdb_payload_v1_builder_value_bytes(
                builder, nullptr, UINT64_C(1), &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    error.clear();
    require(fdb_payload_v1_builder_value_bytes(
                builder, nullptr, UINT64_C(0), &error.value) == UINT32_C(0));
    fdb_payload_v1_plan_t* empty_plan = nullptr;
    require(fdb_payload_v1_builder_freeze(
                builder, &empty_plan, &error.value) == UINT32_C(0));
    require(empty_plan != nullptr);
    fdb_payload_v1_builder_release(builder);
    fdb_payload_v1_plan_release(empty_plan);
    fdb_payload_v1_spec_release(empty_spans);

    fdb_payload_v1_plan_t* cleared_plan =
        reinterpret_cast<fdb_payload_v1_plan_t*>(std::uintptr_t{1});
    auto clearing_status = fdb_payload_v1_builder_freeze(
        nullptr, &cleared_plan, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_plan == nullptr);
    require(owned_error(clearing_status, error.value));
    error.clear();

    fdb_payload_v1_plan_info_t cleared_info{};
    fdb_payload_v1_plan_info_init(&cleared_info);
    cleared_info.total_bytes = UINT64_MAX;
    clearing_status =
        fdb_payload_v1_plan_info(nullptr, &cleared_info, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_info.total_bytes == UINT64_C(0));
    error.clear();

    fdb_payload_v1_payload_t* cleared_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    fdb_payload_v1_execution_report_t cleared_report{};
    fdb_payload_v1_execution_report_init(&cleared_report);
    cleared_report.mode = UINT32_MAX;
    clearing_status = fdb_payload_v1_plan_execute(
        nullptr, FDB_PAYLOAD_BUILD_ALLOW_STAGING, nullptr, &cleared_payload,
        &cleared_report, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_payload == nullptr);
    require(cleared_report.mode == UINT32_C(0));
    error.clear();

    cleared_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    clearing_status = fdb_payload_v1_payload_open_copy(
        nullptr, nullptr, UINT64_C(0), nullptr, &cleared_payload,
        &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_payload == nullptr);
    error.clear();
    cleared_payload =
        reinterpret_cast<fdb_payload_v1_payload_t*>(std::uintptr_t{1});
    clearing_status = fdb_payload_v1_payload_open_external(
        nullptr, nullptr, UINT64_C(0), nullptr, nullptr, nullptr,
        &cleared_payload, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_payload == nullptr);
    error.clear();

    std::array<std::uint8_t, 32> digest;
    digest.fill(UINT8_C(0xa5));
    require(fdb_payload_v1_payload_sha256(nullptr, digest.data(),
                                          &error.value) ==
            FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(std::all_of(digest.begin(), digest.end(),
                        [](std::uint8_t value) {
                            return value == UINT8_C(0);
                        }));
    error.clear();

    fdb_payload_v1_profile_t cleared_profile = UINT32_MAX;
    clearing_status = fdb_payload_v1_payload_profile(
        nullptr, &cleared_profile, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_profile == UINT32_C(0));
    error.clear();
    fdb_payload_v1_execution_report_init(&cleared_report);
    cleared_report.mode = UINT32_MAX;
    clearing_status = fdb_payload_v1_payload_execution_report(
        nullptr, &cleared_report, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_report.mode == UINT32_C(0));
    error.clear();
    fdb_payload_v1_blob_t* cleared_blob =
        reinterpret_cast<fdb_payload_v1_blob_t*>(std::uintptr_t{1});
    clearing_status = fdb_payload_v1_payload_binary_blob(
        nullptr, &cleared_blob, &error.value);
    require(clearing_status == FDB_PAYLOAD_E_INVALID_ARGUMENT);
    require(cleared_blob == nullptr);
    error.clear();
    fdb_payload_v1_spec_release(record);
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string_view{argv[1]} ==
            "--single-thread-injected-failure") {
        require(verify_allocation_harness_invariants() == EXIT_SUCCESS);
        require(test_handle_allocation_boundaries_and_owner_projection() ==
                EXIT_SUCCESS);
        return EXIT_SUCCESS;
    }

    require(verify_allocation_harness_invariants() == EXIT_SUCCESS);
    require(test_initializers_and_constants() == EXIT_SUCCESS);
    require(test_complete_record_runtime() == EXIT_SUCCESS);
    require(test_handle_allocation_boundaries_and_owner_projection() ==
            EXIT_SUCCESS);
    require(test_stale_generation_c_projection() == EXIT_SUCCESS);
    require(test_view_and_access_keep_owner_alive() == EXIT_SUCCESS);
    require(test_checked_view_access_materialize_and_barrier_abi() ==
            EXIT_SUCCESS);
    require(test_null_error_sink_rule() == EXIT_SUCCESS);
    require(test_errors_prefixes_callbacks_and_graph_unavailable() ==
            EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
