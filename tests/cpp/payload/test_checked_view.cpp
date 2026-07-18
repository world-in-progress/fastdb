#include "TestSupport.hpp"

#include "payload/build/PayloadBuilder.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if __has_include("payload/view/AccessBarrier.hpp") && \
    __has_include("payload/view/Materialize.hpp") && \
    __has_include("payload/view/View.hpp")
#define FASTDB_TASK8_HAS_CHECKED_VIEW UINT32_C(1)
#include "payload/view/AccessBarrier.hpp"
#include "payload/view/Materialize.hpp"
#include "payload/view/View.hpp"
#else
#define FASTDB_TASK8_HAS_CHECKED_VIEW UINT32_C(0)
#endif

namespace allocation_failure {

std::atomic<std::int64_t> fail_after{INT64_C(-1)};

struct AllocationHeader final {
    void* raw;
};

void* allocate(std::size_t size,
               std::size_t alignment = alignof(std::max_align_t)) {
    const std::int64_t remaining = fail_after.load(std::memory_order_relaxed);
    if (remaining >= INT64_C(0) &&
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

#if FASTDB_TASK8_HAS_CHECKED_VIEW
namespace fastdb::payload::view {

struct BarrierFacts final {
    std::uint64_t generation;
    std::uint64_t active_accesses;
    std::uint64_t waiting_invalidators;
    bool invalidating;
    bool invalidated;
    bool has_backing;
};

/* Test-only friend seam for the accepted barrier/optional-backing facts. */
struct PayloadOwnerTestAccess final {
    static BarrierFacts barrier_facts(const PayloadOwner& owner);
    static void wait_until_invalidating(const PayloadOwner& owner);
    static bool wait_until_invalidator_waiting(const PayloadOwner& owner);
    static void set_generation(PayloadOwner& owner,
                               std::uint64_t generation);
    static bool barrier_mutex_available(const PayloadOwner& owner);
    static std::uint64_t validation_work(const PayloadOwner& owner);
};

struct ViewTestAccess final {
    static bool all_direct_children_contiguous(const View& view) noexcept {
        if (view.state_ == nullptr || view.state_->is_backed ||
            view.state_->detached == nullptr) {
            return false;
        }
        const auto& nodes = view.state_->detached->arena.nodes();
        for (const build::ValueNode& node : nodes) {
            const bool container =
                node.tag == build::ValueTag::sequence ||
                node.tag == build::ValueTag::component ||
                node.tag == build::ValueTag::list;
            if (!container) {
                continue;
            }
            if (node.child_count == UINT64_C(0)) {
                if (node.first_child != build::invalid_node_index) {
                    return false;
                }
                continue;
            }
            if (node.first_child >= nodes.size() ||
                node.child_count >
                    static_cast<std::uint64_t>(nodes.size()) -
                        node.first_child) {
                return false;
            }
            for (std::uint64_t index = UINT64_C(0);
                 index < node.child_count; ++index) {
                const build::NodeIndex child = node.first_child + index;
                const build::NodeIndex expected_next =
                    index + UINT64_C(1) < node.child_count
                        ? child + UINT64_C(1)
                        : build::invalid_node_index;
                if (nodes[static_cast<std::size_t>(child)].next_sibling !=
                    expected_next) {
                    return false;
                }
            }
        }
        return true;
    }

    static ByteSpan detached_arena_bytes(const View& view) noexcept {
        if (view.state_ == nullptr || view.state_->is_backed ||
            view.state_->detached == nullptr) {
            return ByteSpan{nullptr, UINT64_C(0)};
        }
        const auto& bytes = view.state_->detached->arena.byte_storage();
        return ByteSpan{
            bytes.empty() ? nullptr : bytes.data(),
            static_cast<std::uint64_t>(bytes.size())};
    }
};

BarrierFacts PayloadOwnerTestAccess::barrier_facts(
    const PayloadOwner& owner) {
    std::lock_guard<std::mutex> lock(owner.state_->barrier.mutex);
    return BarrierFacts{
        owner.state_->barrier.generation,
        owner.state_->barrier.active_accesses,
        owner.state_->barrier.waiting_invalidators,
        owner.state_->barrier.invalidating,
        owner.state_->barrier.invalidated,
        owner.state_->backing.has_value(),
    };
}

void PayloadOwnerTestAccess::wait_until_invalidating(
    const PayloadOwner& owner) {
    std::unique_lock<std::mutex> lock(owner.state_->barrier.mutex);
    owner.state_->barrier.drained.wait(lock, [&owner] {
        return owner.state_->barrier.invalidating;
    });
}

bool PayloadOwnerTestAccess::wait_until_invalidator_waiting(
    const PayloadOwner& owner) {
    std::unique_lock<std::mutex> lock(owner.state_->barrier.mutex);
    return owner.state_->barrier.drained.wait_for(
        lock, std::chrono::seconds(10), [&owner] {
            return owner.state_->barrier.waiting_invalidators >
                   UINT64_C(0);
        });
}

void PayloadOwnerTestAccess::set_generation(PayloadOwner& owner,
                                            std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(owner.state_->barrier.mutex);
    owner.state_->barrier.generation = generation;
}

bool PayloadOwnerTestAccess::barrier_mutex_available(
    const PayloadOwner& owner) {
    if (!owner.state_->barrier.mutex.try_lock()) {
        return false;
    }
    owner.state_->barrier.mutex.unlock();
    return true;
}

std::uint64_t PayloadOwnerTestAccess::validation_work(
    const PayloadOwner& owner) {
    return owner.state_->index.validation_work();
}

}  // namespace fastdb::payload::view
#endif

namespace {

enum class MatrixArea : std::uint8_t {
    navigation,
    scalar,
    access,
    materialize,
    ownership,
    invalidation,
    concurrency,
    generation,
};

struct MatrixCase final {
    MatrixArea area;
    std::string_view name;
};

constexpr std::array<MatrixCase, 41> task8_matrix{{
    {MatrixArea::navigation, "entry views are sequences for one and many"},
    {MatrixArea::navigation, "empty many has length zero"},
    {MatrixArea::navigation, "component identity and field order are exact"},
    {MatrixArea::navigation, "nested list items use the validated index"},
    {MatrixArea::navigation, "index failures are deterministic"},
    {MatrixArea::navigation, "null and wrong-kind precedence is stable"},
    {MatrixArea::scalar, "Boolean and unsigned getters preserve exact values"},
    {MatrixArea::scalar, "signed integer getter preserves exact bits"},
    {MatrixArea::scalar, "f32 and f64 getters preserve canonical bits"},
    {MatrixArea::scalar, "normalized getters reuse exact dequantization"},
    {MatrixArea::scalar, "null scalar is distinct from zero"},
    {MatrixArea::access, "payload access pins the complete immutable image"},
    {MatrixArea::access, "UTF-8 and opaque spans borrow under one pin"},
    {MatrixArea::access, "wide access owns aligned decoded code units"},
    {MatrixArea::access, "wide access never aliases wire or arena bytes"},
    {MatrixArea::access, "wrong access-kind calls fail"},
    {MatrixArea::access, "lazy text validates each selected span"},
    {MatrixArea::access, "lazy string and work limits are per access"},
    {MatrixArea::materialize, "every scalar kind materializes exactly"},
    {MatrixArea::materialize, "component and list subtrees materialize"},
    {MatrixArea::materialize, "null and empty distinctions survive"},
    {MatrixArea::materialize, "detached views survive source invalidation"},
    {MatrixArea::materialize, "detached views can materialize again"},
    {MatrixArea::materialize, "deep materialize retains linear path state"},
    {MatrixArea::materialize, "allocation failure publishes no partial view"},
    {MatrixArea::ownership, "views retain shared owner state"},
    {MatrixArea::ownership, "owner copies retain one backing reference"},
    {MatrixArea::ownership, "move and self-assignment preserve ownership"},
    {MatrixArea::invalidation, "active access blocks invalidation"},
    {MatrixArea::invalidation, "invalidating rejects every new operation"},
    {MatrixArea::invalidation, "release occurs outside the barrier mutex"},
    {MatrixArea::invalidation, "backing can be reused immediately on return"},
    {MatrixArea::invalidation, "stale views never touch reused bytes"},
    {MatrixArea::invalidation, "repeat invalidation releases only once"},
    {MatrixArea::concurrency, "one hundred thousand short readers are safe"},
    {MatrixArea::concurrency, "two invalidators share one completion"},
    {MatrixArea::concurrency, "reader and invalidation races are bounded"},
    {MatrixArea::concurrency, "distinct-context callback reentry is allowed"},
    {MatrixArea::generation, "valid generation mismatch is stale"},
    {MatrixArea::generation, "generation exhaustion never wraps"},
    {MatrixArea::generation, "active pin accounting never underflows"},
}};

int verify_matrix_inventory() {
    std::array<bool, 8> covered{};
    for (const MatrixCase& item : task8_matrix) {
        require(!item.name.empty());
        covered[static_cast<std::size_t>(item.area)] = true;
    }
    for (const bool present : covered) {
        require(present);
    }
    return EXIT_SUCCESS;
}

#if FASTDB_TASK8_HAS_CHECKED_VIEW

using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::spec::CompiledSpec;
using fastdb::payload::view::Access;
using fastdb::payload::view::PayloadOwner;
using fastdb::payload::view::View;
using fastdb::payload::view::ViewKind;

constexpr std::uint32_t require_direct = UINT32_C(2);

static_assert(static_cast<std::uint32_t>(ViewKind::sequence) == UINT32_C(1));
static_assert(static_cast<std::uint32_t>(ViewKind::boolean) == UINT32_C(2));
static_assert(static_cast<std::uint32_t>(ViewKind::u8) == UINT32_C(3));
static_assert(static_cast<std::uint32_t>(ViewKind::u16) == UINT32_C(4));
static_assert(static_cast<std::uint32_t>(ViewKind::u32) == UINT32_C(5));
static_assert(static_cast<std::uint32_t>(ViewKind::i32) == UINT32_C(6));
static_assert(static_cast<std::uint32_t>(ViewKind::u8n) == UINT32_C(7));
static_assert(static_cast<std::uint32_t>(ViewKind::u16n) == UINT32_C(8));
static_assert(static_cast<std::uint32_t>(ViewKind::f32) == UINT32_C(9));
static_assert(static_cast<std::uint32_t>(ViewKind::f64) == UINT32_C(10));
static_assert(static_cast<std::uint32_t>(ViewKind::str) == UINT32_C(11));
static_assert(static_cast<std::uint32_t>(ViewKind::wstr) == UINT32_C(12));
static_assert(static_cast<std::uint32_t>(ViewKind::bytes) == UINT32_C(13));
static_assert(static_cast<std::uint32_t>(ViewKind::component) == UINT32_C(14));
static_assert(static_cast<std::uint32_t>(ViewKind::list) == UINT32_C(15));
static_assert(static_cast<std::uint32_t>(ViewKind::ref) == UINT32_C(16));
static_assert(std::is_copy_constructible_v<View>);
static_assert(std::is_copy_assignable_v<View>);
static_assert(std::is_nothrow_move_constructible_v<View>);
static_assert(!std::is_copy_constructible_v<Access>);
static_assert(!std::is_copy_assignable_v<Access>);
static_assert(std::is_nothrow_move_constructible_v<Access>);

constexpr std::string_view all_algebra_spec = R"json({
  "schema":"fastdb.payload.v1",
  "profile":"record.v1",
  "entries":[
    {"id":"bools","cardinality":"many","type":{"kind":"bool","nullable":true}},
    {"id":"u8","cardinality":"one","type":{"kind":"u8"}},
    {"id":"u16s","cardinality":"many","type":{"kind":"u16","nullable":true}},
    {"id":"u32","cardinality":"one","type":{"kind":"u32"}},
    {"id":"i32","cardinality":"one","type":{"kind":"i32"}},
    {"id":"u8n","cardinality":"one","type":{"kind":"u8n","min":0,"max":1}},
    {"id":"u16n","cardinality":"one","type":{"kind":"u16n","min":-1,"max":1}},
    {"id":"f32","cardinality":"one","type":{"kind":"f32"}},
    {"id":"f64","cardinality":"one","type":{"kind":"f64"}},
    {"id":"strs","cardinality":"many","type":{"kind":"str","nullable":true}},
    {"id":"wstrs","cardinality":"many","type":{"kind":"wstr","nullable":true}},
    {"id":"byte_values","cardinality":"many","type":{"kind":"bytes","nullable":true}},
    {"id":"record","cardinality":"one","type":{"kind":"component","id":"Root"}},
    {"id":"empty_many","cardinality":"many","type":{"kind":"u8"}},
    {"id":"nested_lists","cardinality":"many","type":{"kind":"list","nullable":true,"items":{"kind":"list","nullable":true,"items":{"kind":"u8","nullable":true}}}}
  ],
  "components":[
    {"id":"Root","kind":"record","fields":[
      {"id":"leaf","type":{"kind":"component","id":"Leaf"}},
      {"id":"values","type":{"kind":"list","items":{"kind":"u16","nullable":true}}},
      {"id":"empty_values","type":{"kind":"list","items":{"kind":"u8"}}},
      {"id":"null_values","type":{"kind":"list","nullable":true,"items":{"kind":"u8"}}},
      {"id":"empty_str","type":{"kind":"str"}},
      {"id":"null_bytes","type":{"kind":"bytes","nullable":true}},
      {"id":"leaves","type":{"kind":"list","items":{"kind":"component","id":"Leaf","nullable":true}}}
    ]},
    {"id":"Leaf","kind":"record","fields":[
      {"id":"id","type":{"kind":"u32"}},
      {"id":"label","type":{"kind":"str","nullable":true}}
    ]}
  ]
})json";

template <typename T>
T take(Result<T>&& result, std::string_view operation) {
    if (!result.has_value()) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 std::string(result.error().details_json()));
    }
    return std::move(result).value();
}

void take(Result<void>&& result, std::string_view operation) {
    if (!result.has_value()) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 std::string(result.error().details_json()));
    }
}

PayloadOwner build_all_algebra_owner() {
    auto compiled = take(CompiledSpec::compile(all_algebra_spec), "compile");
    auto builder = take(PayloadBuilder::create(std::move(compiled)), "create");

    const std::array<std::uint16_t, 2> wide_bmp_nul{{UINT16_C(0x4e2d),
                                                      UINT16_C(0)}};
    const std::array<std::uint16_t, 2> wide_pair{{UINT16_C(0xd83d),
                                                  UINT16_C(0xde03)}};
    const std::array<std::uint8_t, 3> opaque{{UINT8_C(0), UINT8_C(0xff),
                                              UINT8_C(0x80)}};

#define FASTDB_CHECKED_VIEW_BUILD(expression) \
    take((expression), #expression)
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(0), UINT64_C(3)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_bool(UINT8_C(0)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_bool(UINT8_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(1), UINT64_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u8(UINT8_C(0xab)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(2), UINT64_C(3)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u16(UINT16_C(0x1234)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u16(UINT16_MAX));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(3), UINT64_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u32(UINT32_C(0x12345678)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(4), UINT64_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_i32(INT32_C(-2)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(5), UINT64_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(
        builder.push_u8n_bits(UINT64_C(0x3ff0000000000000)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(6), UINT64_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(
        builder.push_u16n_bits(UINT64_C(0xbff0000000000000)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(7), UINT64_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_f32_bits(UINT32_C(0x3f800000)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(8), UINT64_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(
        builder.push_f64_bits(UINT64_C(0x8000000000000000)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(9), UINT64_C(4)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_str(""));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_str(std::string_view{"A\0B", 3U}));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_str("\xe4\xb8\xad"));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(10), UINT64_C(4)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_wstr(nullptr, UINT64_C(0)));
    FASTDB_CHECKED_VIEW_BUILD(
        builder.push_wstr(wide_bmp_nul.data(), wide_bmp_nul.size()));
    FASTDB_CHECKED_VIEW_BUILD(
        builder.push_wstr(wide_pair.data(), wide_pair.size()));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(11), UINT64_C(4)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_bytes(nullptr, UINT64_C(0)));
    FASTDB_CHECKED_VIEW_BUILD(
        builder.push_bytes(opaque.data(), opaque.size()));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_bytes(
        reinterpret_cast<const std::uint8_t*>("A\0"), UINT64_C(2)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(12), UINT64_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_component());
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_component());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u32(UINT32_C(7)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_str("leaf"));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_list(UINT64_C(3)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u16(UINT16_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u16(UINT16_C(2)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_list(UINT64_C(0)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_str(""));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_list(UINT64_C(3)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_component());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u32(UINT32_C(8)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_component());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u32(UINT32_C(9)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_str("nine"));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(13), UINT64_C(0)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_entry(UINT32_C(14), UINT64_C(3)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_list(UINT64_C(3)));
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_list(UINT64_C(3)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u8(UINT8_C(1)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.push_u8(UINT8_C(2)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_list(UINT64_C(0)));
    FASTDB_CHECKED_VIEW_BUILD(builder.push_null());
    FASTDB_CHECKED_VIEW_BUILD(builder.begin_list(UINT64_C(0)));
#undef FASTDB_CHECKED_VIEW_BUILD

    auto plan = take(builder.freeze_plan(), "freeze_plan");
    return take(plan.execute(require_direct, nullptr), "execute");
}

std::string deep_list_spec(std::uint32_t depth) {
    std::string source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"deep","cardinality":"one","type":)";
    for (std::uint32_t index = UINT32_C(0); index < depth; ++index) {
        source += R"({"kind":"list","items":)";
    }
    source += R"({"kind":"u8"})";
    source.append(static_cast<std::size_t>(depth), '}');
    source += R"(}],"components":[]})";
    return source;
}

PayloadOwner build_deep_list_owner(std::uint32_t depth) {
    const std::string source = deep_list_spec(depth);
    fastdb::payload::spec::CompileLimits compile_limits{};
    compile_limits.json.max_source_bytes =
        UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024);
    compile_limits.json.max_json_values = UINT64_C(100000);
    compile_limits.json.max_nesting_depth = depth + UINT32_C(16);
    auto compiled = take(
        CompiledSpec::compile(source, compile_limits), "compile deep list");
    auto builder_limits = fastdb::payload::build::default_builder_limits();
    builder_limits.max_nesting_depth = depth;
    auto builder = take(
        PayloadBuilder::create(std::move(compiled), builder_limits),
        "create deep list");
    take(builder.begin_entry(UINT32_C(0), UINT64_C(1)),
         "begin deep entry");
    for (std::uint32_t index = UINT32_C(0); index < depth; ++index) {
        take(builder.begin_list(UINT64_C(1)), "begin deep list");
    }
    take(builder.push_u8(UINT8_C(42)), "push deep leaf");
    auto plan = take(builder.freeze_plan(), "freeze deep plan");
    return take(plan.execute(require_direct, nullptr), "execute deep plan");
}

std::vector<std::uint8_t> build_all_algebra_image() {
    PayloadOwner owner = build_all_algebra_owner();
    Access access = take(owner.acquire(), "owner.acquire");
    const auto span = take(access.payload_bytes(), "payload_bytes");
    return std::vector<std::uint8_t>(
        span.data, span.data + static_cast<std::ptrdiff_t>(span.size));
}

class TestGate final {
public:
    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        condition_.notify_all();
    }

    bool wait(std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_{false};
};

struct FakeExternalBacking final {
    std::vector<std::uint8_t> storage;
    std::atomic<std::uint32_t> retain_count{UINT32_C(0)};
    std::atomic<std::uint32_t> release_count{UINT32_C(0)};
    std::atomic<bool> release_saw_unlocked_barrier{false};
    std::atomic<bool> distinct_reentry_succeeded{false};
    PayloadOwner* observed_owner{nullptr};
    PayloadOwner* distinct_reentry_owner{nullptr};
    bool block_release{false};

    std::mutex release_mutex;
    std::condition_variable release_condition;
    bool release_entered{false};
    bool allow_release{false};

    fastdb::payload::backing::Callbacks callbacks() noexcept {
        return fastdb::payload::backing::Callbacks{
            this, nullptr, nullptr, nullptr, nullptr, &retain_thunk,
            &release_thunk};
    }

    bool wait_for_release() {
        std::unique_lock<std::mutex> lock(release_mutex);
        return release_condition.wait_for(
            lock, std::chrono::seconds(10),
            [this] { return release_entered; });
    }

    void unblock_release() {
        std::lock_guard<std::mutex> lock(release_mutex);
        allow_release = true;
        release_condition.notify_all();
    }

private:
    static std::uint32_t retain_thunk(void* context, void*) {
        auto& self = *static_cast<FakeExternalBacking*>(context);
        self.retain_count.fetch_add(UINT32_C(1), std::memory_order_relaxed);
        return UINT32_C(0);
    }

    static void release_thunk(void* context, void*) {
        auto& self = *static_cast<FakeExternalBacking*>(context);
        self.release_count.fetch_add(UINT32_C(1), std::memory_order_relaxed);
        if (self.observed_owner != nullptr) {
            self.release_saw_unlocked_barrier.store(
                fastdb::payload::view::PayloadOwnerTestAccess::
                    barrier_mutex_available(*self.observed_owner),
                std::memory_order_relaxed);
        }
        if (self.distinct_reentry_owner != nullptr) {
            auto reentry = self.distinct_reentry_owner->acquire();
            self.distinct_reentry_succeeded.store(
                reentry.has_value() &&
                    reentry.value().payload_bytes().has_value(),
                std::memory_order_relaxed);
        }
        std::fill(self.storage.begin(), self.storage.end(), UINT8_C(0xa5));
        std::unique_lock<std::mutex> lock(self.release_mutex);
        self.release_entered = true;
        self.release_condition.notify_all();
        if (self.block_release) {
            self.release_condition.wait(lock,
                                        [&self] { return self.allow_release; });
        }
    }
};

PayloadOwner open_external_all_algebra(
    FakeExternalBacking& backing,
    fastdb::payload::view::OpenOptions options =
        fastdb::payload::view::default_open_options()) {
    backing.storage = build_all_algebra_image();
    auto retained = take(fastdb::payload::backing::RetainedBacking::acquire(
                             backing.callbacks(), &backing,
                             backing.storage.data(), backing.storage.size()),
                         "retain external backing");
    auto compiled = take(CompiledSpec::compile(all_algebra_spec), "compile");
    return take(PayloadOwner::open_external(
                    std::move(compiled), backing.storage.data(),
                    backing.storage.size(), std::move(retained), options),
                "open_external");
}

int require_error_code(std::uint32_t expected,
                       const fastdb::payload::error::Error& error) {
    require(error.code() == expected);
    return EXIT_SUCCESS;
}

int test_navigation_scalars_and_errors() {
    PayloadOwner owner = build_all_algebra_owner();

    auto bools = owner.entry_view(UINT32_C(0));
    require(bools.has_value());
    require(bools.value().kind().value() == ViewKind::sequence);
    require(!bools.value().is_null().value());
    require(bools.value().length().value() == UINT64_C(3));
    require(bools.value().at(UINT64_C(0)).value().get_bool().value() ==
            UINT8_C(0));
    const View null_bool = bools.value().at(UINT64_C(1)).value();
    require(null_bool.kind().value() == ViewKind::boolean);
    require(null_bool.is_null().value());
    auto null_bool_value = null_bool.get_bool();
    require(!null_bool_value.has_value());
    require(require_error_code(FDB_PAYLOAD_E_UNEXPECTED_NULL,
                               null_bool_value.error()) == EXIT_SUCCESS);
    auto null_wrong_kind = null_bool.get_u8();
    require(!null_wrong_kind.has_value());
    require(require_error_code(FDB_PAYLOAD_E_TYPE_MISMATCH,
                               null_wrong_kind.error()) == EXIT_SUCCESS);
    require(bools.value().at(UINT64_C(2)).value().get_bool().value() ==
            UINT8_C(1));

    require(owner.entry_view(UINT32_C(1)).value().at(UINT64_C(0)).value()
                .get_u8().value() == UINT8_C(0xab));
    const View u16s = owner.entry_view(UINT32_C(2)).value();
    require(u16s.at(UINT64_C(0)).value().get_u16().value() ==
            UINT16_C(0x1234));
    require(u16s.at(UINT64_C(2)).value().get_u16().value() == UINT16_MAX);
    require(owner.entry_view(UINT32_C(3)).value().at(UINT64_C(0)).value()
                .get_u32().value() == UINT32_C(0x12345678));
    require(owner.entry_view(UINT32_C(4)).value().at(UINT64_C(0)).value()
                .get_i32().value() == INT32_C(-2));
    require(owner.entry_view(UINT32_C(5)).value().at(UINT64_C(0)).value()
                .get_u8n_f64_bits().value() ==
            UINT64_C(0x3ff0000000000000));
    require(owner.entry_view(UINT32_C(6)).value().at(UINT64_C(0)).value()
                .get_u16n_f64_bits().value() ==
            UINT64_C(0xbff0000000000000));
    require(owner.entry_view(UINT32_C(7)).value().at(UINT64_C(0)).value()
                .get_f32_bits().value() == UINT32_C(0x3f800000));
    require(owner.entry_view(UINT32_C(8)).value().at(UINT64_C(0)).value()
                .get_f64_bits().value() ==
            UINT64_C(0x8000000000000000));

    const View record = owner.entry_view(UINT32_C(12)).value()
                            .at(UINT64_C(0)).value();
    require(record.kind().value() == ViewKind::component);
    require(record.component_index().value() == UINT32_C(1));
    require(record.field_count().value() == UINT32_C(7));
    const View leaf = record.field(UINT32_C(0)).value();
    require(leaf.kind().value() == ViewKind::component);
    require(leaf.component_index().value() == UINT32_C(0));
    require(leaf.field(UINT32_C(0)).value().get_u32().value() == UINT32_C(7));
    const View values = record.field(UINT32_C(1)).value();
    require(values.kind().value() == ViewKind::list);
    require(values.length().value() == UINT64_C(3));
    require(values.at(UINT64_C(0)).value().get_u16().value() == UINT16_C(1));
    require(values.at(UINT64_C(1)).value().is_null().value());
    require(values.at(UINT64_C(2)).value().get_u16().value() == UINT16_C(2));
    require(record.field(UINT32_C(2)).value().length().value() == UINT64_C(0));
    require(record.field(UINT32_C(3)).value().is_null().value());
    const View leaves = record.field(UINT32_C(6)).value();
    require(leaves.length().value() == UINT64_C(3));
    require(leaves.at(UINT64_C(0)).value().is_null().value());
    require(leaves.at(UINT64_C(1)).value().field(UINT32_C(0)).value()
                .get_u32().value() == UINT32_C(8));
    require(leaves.at(UINT64_C(2)).value().field(UINT32_C(1)).value()
                .kind().value() == ViewKind::str);

    require(owner.entry_view(UINT32_C(13)).value().length().value() ==
            UINT64_C(0));
    auto missing_entry = owner.entry_view(UINT32_C(15));
    require(!missing_entry.has_value());
    require(missing_entry.error().code() == FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    auto missing_value = bools.value().at(UINT64_C(3));
    require(!missing_value.has_value());
    require(missing_value.error().code() == FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    auto missing_field = record.field(UINT32_C(7));
    require(!missing_field.has_value());
    require(missing_field.error().code() == FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE);
    auto scalar_length = owner.entry_view(UINT32_C(1)).value()
                             .at(UINT64_C(0)).value().length();
    require(!scalar_length.has_value());
    require(scalar_length.error().code() == FDB_PAYLOAD_E_TYPE_MISMATCH);
    auto list_field = values.field(UINT32_C(0));
    require(!list_field.has_value());
    require(list_field.error().code() == FDB_PAYLOAD_E_TYPE_MISMATCH);
    return EXIT_SUCCESS;
}

int test_access_and_materialize() {
    PayloadOwner owner = build_all_algebra_owner();

    auto payload_access = owner.acquire();
    require(payload_access.has_value());
    const auto payload_span = payload_access.value().payload_bytes();
    require(payload_span.has_value());
    require(payload_span.value().data != nullptr);
    require(payload_span.value().size > UINT64_C(0));
    auto wrong_payload_kind = payload_access.value().str();
    require(!wrong_payload_kind.has_value());
    require(wrong_payload_kind.error().code() == FDB_PAYLOAD_E_TYPE_MISMATCH);

    const View embedded = owner.entry_view(UINT32_C(9)).value()
                              .at(UINT64_C(2)).value();
    auto text_access = embedded.acquire();
    require(text_access.has_value());
    const auto text = text_access.value().str();
    require(text.has_value());
    require(text.value().size == UINT64_C(3));
    require(std::memcmp(text.value().data, "A\0B", 3U) == 0);
    require(!text_access.value().bytes().has_value());

    const View wide_value = owner.entry_view(UINT32_C(10)).value()
                                .at(UINT64_C(2)).value();
    auto wide_access = wide_value.acquire();
    require(wide_access.has_value());
    const auto wide = wide_access.value().wstr();
    require(wide.has_value());
    require(wide.value().size == UINT64_C(2));
    require(wide.value().data[0] == UINT16_C(0x4e2d));
    require(wide.value().data[1] == UINT16_C(0));
    require(reinterpret_cast<std::uintptr_t>(wide.value().data) %
                alignof(std::uint16_t) == std::uintptr_t{0});
    const auto* const payload_begin = payload_span.value().data;
    const auto* const payload_end =
        payload_begin + static_cast<std::ptrdiff_t>(payload_span.value().size);
    const auto* const wide_as_bytes =
        reinterpret_cast<const std::uint8_t*>(wide.value().data);
    require(wide_as_bytes < payload_begin || wide_as_bytes >= payload_end);

    const View opaque_value = owner.entry_view(UINT32_C(11)).value()
                                  .at(UINT64_C(2)).value();
    auto opaque_access = opaque_value.acquire();
    require(opaque_access.has_value());
    const auto opaque = opaque_access.value().bytes();
    require(opaque.has_value());
    require(opaque.value().size == UINT64_C(3));
    require(opaque.value().data[0] == UINT8_C(0));
    require(opaque.value().data[1] == UINT8_C(0xff));
    require(opaque.value().data[2] == UINT8_C(0x80));
    require(!opaque_access.value().str().has_value());
    Access moved_opaque = std::move(opaque_access).value();
    auto moved_from_access = opaque_access.value().bytes();
    require(!moved_from_access.has_value());
    require(moved_from_access.error().code() == FDB_PAYLOAD_E_TYPE_MISMATCH);
    require(moved_opaque.bytes().value().size == UINT64_C(3));

    const View record = owner.entry_view(UINT32_C(12)).value()
                            .at(UINT64_C(0)).value();
    auto detached_result = record.materialize();
    require(detached_result.has_value());
    View detached = std::move(detached_result).value();
    auto repeated_result = detached.materialize();
    require(repeated_result.has_value());
    View repeated = std::move(repeated_result).value();
    require(repeated.component_index().value() == UINT32_C(1));
    require(repeated.field(UINT32_C(0)).value().field(UINT32_C(0)).value()
                .get_u32().value() == UINT32_C(7));
    require(repeated.field(UINT32_C(1)).value().length().value() ==
            UINT64_C(3));
    require(repeated.field(UINT32_C(2)).value().length().value() ==
            UINT64_C(0));
    require(repeated.field(UINT32_C(3)).value().is_null().value());
    require(repeated.field(UINT32_C(4)).value().acquire().value().str().value()
                .size == UINT64_C(0));
    require(repeated.field(UINT32_C(5)).value().is_null().value());

    auto detached_text = embedded.materialize();
    require(detached_text.has_value());
    auto detached_text_access = detached_text.value().acquire();
    require(detached_text_access.has_value());
    require(detached_text_access.value().str().value().size == UINT64_C(3));
    require(std::memcmp(detached_text_access.value().str().value().data,
                        "A\0B", 3U) == 0);
    return EXIT_SUCCESS;
}

int test_materialize_every_entry_and_value_kind() {
    PayloadOwner owner = build_all_algebra_owner();
    constexpr std::array<std::uint64_t, 15> entry_lengths{{
        UINT64_C(3), UINT64_C(1), UINT64_C(3), UINT64_C(1), UINT64_C(1),
        UINT64_C(1), UINT64_C(1), UINT64_C(1), UINT64_C(1), UINT64_C(4),
        UINT64_C(4), UINT64_C(4), UINT64_C(1), UINT64_C(0), UINT64_C(3),
    }};
    for (std::uint32_t entry = UINT32_C(0);
         entry < entry_lengths.size(); ++entry) {
        View sequence = owner.entry_view(entry).value();
        View detached_sequence = sequence.materialize().value();
        require(detached_sequence.kind().value() == ViewKind::sequence);
        require(!detached_sequence.is_null().value());
        require(detached_sequence.length().value() == entry_lengths[entry]);
    }

    View boolean = owner.entry_view(UINT32_C(0)).value()
                       .at(UINT64_C(2)).value().materialize().value();
    require(boolean.kind().value() == ViewKind::boolean);
    require(boolean.get_bool().value() == UINT8_C(1));
    View u8 = owner.entry_view(UINT32_C(1)).value()
                  .at(UINT64_C(0)).value().materialize().value();
    require(u8.kind().value() == ViewKind::u8);
    require(u8.get_u8().value() == UINT8_C(0xab));
    View u16 = owner.entry_view(UINT32_C(2)).value()
                   .at(UINT64_C(0)).value().materialize().value();
    require(u16.kind().value() == ViewKind::u16);
    require(u16.get_u16().value() == UINT16_C(0x1234));
    View u32 = owner.entry_view(UINT32_C(3)).value()
                   .at(UINT64_C(0)).value().materialize().value();
    require(u32.kind().value() == ViewKind::u32);
    require(u32.get_u32().value() == UINT32_C(0x12345678));
    View i32 = owner.entry_view(UINT32_C(4)).value()
                   .at(UINT64_C(0)).value().materialize().value();
    require(i32.kind().value() == ViewKind::i32);
    require(i32.get_i32().value() == INT32_C(-2));
    View u8n = owner.entry_view(UINT32_C(5)).value()
                   .at(UINT64_C(0)).value().materialize().value();
    require(u8n.kind().value() == ViewKind::u8n);
    require(u8n.get_u8n_f64_bits().value() ==
            UINT64_C(0x3ff0000000000000));
    View u16n = owner.entry_view(UINT32_C(6)).value()
                    .at(UINT64_C(0)).value().materialize().value();
    require(u16n.kind().value() == ViewKind::u16n);
    require(u16n.get_u16n_f64_bits().value() ==
            UINT64_C(0xbff0000000000000));
    View f32 = owner.entry_view(UINT32_C(7)).value()
                   .at(UINT64_C(0)).value().materialize().value();
    require(f32.kind().value() == ViewKind::f32);
    require(f32.get_f32_bits().value() == UINT32_C(0x3f800000));
    View f64 = owner.entry_view(UINT32_C(8)).value()
                   .at(UINT64_C(0)).value().materialize().value();
    require(f64.kind().value() == ViewKind::f64);
    require(f64.get_f64_bits().value() == UINT64_C(0x8000000000000000));

    View null_scalar = owner.entry_view(UINT32_C(2)).value()
                           .at(UINT64_C(1)).value().materialize().value();
    require(null_scalar.kind().value() == ViewKind::u16);
    require(null_scalar.is_null().value());
    auto null_getter = null_scalar.get_u16();
    require(!null_getter.has_value());
    require(null_getter.error().code() == FDB_PAYLOAD_E_UNEXPECTED_NULL);

    View string_value = owner.entry_view(UINT32_C(9)).value()
                            .at(UINT64_C(2)).value().materialize().value();
    require(string_value.kind().value() == ViewKind::str);
    auto string_access = string_value.acquire();
    require(string_access.has_value());
    require(string_access.value().str().value().size == UINT64_C(3));
    require(std::memcmp(string_access.value().str().value().data,
                        "A\0B", 3U) == 0);
    View wide_value = owner.entry_view(UINT32_C(10)).value()
                          .at(UINT64_C(3)).value().materialize().value();
    require(wide_value.kind().value() == ViewKind::wstr);
    auto wide_access = wide_value.acquire();
    require(wide_access.has_value());
    require(wide_access.value().wstr().value().size == UINT64_C(2));
    require(wide_access.value().wstr().value().data[0] == UINT16_C(0xd83d));
    require(wide_access.value().wstr().value().data[1] == UINT16_C(0xde03));
    const auto detached_wide_bytes =
        fastdb::payload::view::ViewTestAccess::detached_arena_bytes(
            wide_value);
    require(detached_wide_bytes.size == UINT64_C(4));
    require(detached_wide_bytes.data != nullptr);
    const std::uintptr_t detached_wide_begin =
        reinterpret_cast<std::uintptr_t>(detached_wide_bytes.data);
    require(detached_wide_bytes.size <=
            std::numeric_limits<std::uintptr_t>::max() -
                detached_wide_begin);
    const std::uintptr_t detached_wide_end =
        detached_wide_begin +
        static_cast<std::uintptr_t>(detached_wide_bytes.size);
    const std::uintptr_t decoded_wide_begin = reinterpret_cast<std::uintptr_t>(
        wide_access.value().wstr().value().data);
    constexpr std::uintptr_t decoded_wide_size =
        static_cast<std::uintptr_t>(2U * sizeof(std::uint16_t));
    require(decoded_wide_begin <=
            std::numeric_limits<std::uintptr_t>::max() - decoded_wide_size);
    const std::uintptr_t decoded_wide_end =
        decoded_wide_begin + decoded_wide_size;
    require(decoded_wide_end <= detached_wide_begin ||
            detached_wide_end <= decoded_wide_begin);
    View byte_value = owner.entry_view(UINT32_C(11)).value()
                          .at(UINT64_C(2)).value().materialize().value();
    require(byte_value.kind().value() == ViewKind::bytes);
    auto byte_access = byte_value.acquire();
    require(byte_access.has_value());
    require(byte_access.value().bytes().value().size == UINT64_C(3));
    require(byte_access.value().bytes().value().data[1] == UINT8_C(0xff));

    View empty_str = owner.entry_view(UINT32_C(9)).value()
                         .at(UINT64_C(1)).value().materialize().value();
    View empty_wstr = owner.entry_view(UINT32_C(10)).value()
                          .at(UINT64_C(1)).value().materialize().value();
    View empty_bytes = owner.entry_view(UINT32_C(11)).value()
                           .at(UINT64_C(1)).value().materialize().value();
    require(empty_str.acquire().value().str().value().size == UINT64_C(0));
    require(empty_wstr.acquire().value().wstr().value().size == UINT64_C(0));
    require(empty_bytes.acquire().value().bytes().value().size == UINT64_C(0));
    View null_str = owner.entry_view(UINT32_C(9)).value()
                        .at(UINT64_C(0)).value().materialize().value();
    View null_wstr = owner.entry_view(UINT32_C(10)).value()
                         .at(UINT64_C(0)).value().materialize().value();
    View null_bytes = owner.entry_view(UINT32_C(11)).value()
                          .at(UINT64_C(0)).value().materialize().value();
    require(null_str.is_null().value());
    require(null_wstr.is_null().value());
    require(null_bytes.is_null().value());
    require(null_str.acquire().error().code() == FDB_PAYLOAD_E_UNEXPECTED_NULL);
    require(null_wstr.acquire().error().code() == FDB_PAYLOAD_E_UNEXPECTED_NULL);
    require(null_bytes.acquire().error().code() == FDB_PAYLOAD_E_UNEXPECTED_NULL);

    View record = owner.entry_view(UINT32_C(12)).value()
                      .at(UINT64_C(0)).value();
    View detached_list = record.field(UINT32_C(1)).value().materialize().value();
    require(detached_list.kind().value() == ViewKind::list);
    require(detached_list.length().value() == UINT64_C(3));
    require(detached_list.at(UINT64_C(0)).value().get_u16().value() ==
            UINT16_C(1));
    require(detached_list.at(UINT64_C(1)).value().is_null().value());
    require(detached_list.at(UINT64_C(2)).value().get_u16().value() ==
            UINT16_C(2));
    return EXIT_SUCCESS;
}

int test_recursive_list_materialization_layout() {
    PayloadOwner owner = build_all_algebra_owner();
    View backed_sequence = owner.entry_view(UINT32_C(14)).value();
    require(backed_sequence.length().value() == UINT64_C(3));
    View backed_outer = backed_sequence.at(UINT64_C(0)).value();
    require(backed_outer.length().value() == UINT64_C(3));
    View backed_inner = backed_outer.at(UINT64_C(0)).value();
    require(backed_inner.length().value() == UINT64_C(3));
    require(backed_inner.at(UINT64_C(0)).value().get_u8().value() ==
            UINT8_C(1));
    require(backed_inner.at(UINT64_C(1)).value().is_null().value());
    require(backed_inner.at(UINT64_C(2)).value().get_u8().value() ==
            UINT8_C(2));
    require(backed_outer.at(UINT64_C(1)).value().is_null().value());
    require(backed_outer.at(UINT64_C(2)).value().length().value() ==
            UINT64_C(0));
    require(backed_sequence.at(UINT64_C(1)).value().is_null().value());
    require(backed_sequence.at(UINT64_C(2)).value().length().value() ==
            UINT64_C(0));

    View detached_sequence = backed_sequence.materialize().value();
    require(fastdb::payload::view::ViewTestAccess::
                all_direct_children_contiguous(detached_sequence));
    View detached_outer = detached_sequence.at(UINT64_C(0)).value();
    View detached_inner = detached_outer.at(UINT64_C(0)).value();
    require(detached_inner.length().value() == UINT64_C(3));
    require(detached_inner.at(UINT64_C(0)).value().get_u8().value() ==
            UINT8_C(1));
    require(detached_inner.at(UINT64_C(1)).value().is_null().value());
    require(detached_inner.at(UINT64_C(2)).value().get_u8().value() ==
            UINT8_C(2));
    require(detached_outer.at(UINT64_C(1)).value().is_null().value());
    require(detached_outer.at(UINT64_C(2)).value().length().value() ==
            UINT64_C(0));
    require(detached_sequence.at(UINT64_C(1)).value().is_null().value());
    require(detached_sequence.at(UINT64_C(2)).value().length().value() ==
            UINT64_C(0));

    View repeated = detached_sequence.materialize().value();
    require(fastdb::payload::view::ViewTestAccess::
                all_direct_children_contiguous(repeated));
    require(repeated.at(UINT64_C(0)).value()
                .at(UINT64_C(0)).value()
                .at(UINT64_C(2)).value().get_u8().value() == UINT8_C(2));
    require(repeated.at(UINT64_C(0)).value()
                .at(UINT64_C(1)).value().is_null().value());
    require(repeated.at(UINT64_C(2)).value().length().value() == UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_deep_materialize_retains_linear_path_state() {
    constexpr std::uint32_t depth = UINT32_C(256);
    constexpr std::uint64_t maximum_linear_path_state =
        UINT64_C(16) * (static_cast<std::uint64_t>(depth) + UINT64_C(2)) +
        UINT64_C(64);
    PayloadOwner owner = build_deep_list_owner(depth);
    View backed_sequence = owner.entry_view(UINT32_C(0)).value();

    fastdb::payload::view::MaterializeMetrics backed_metrics;
    auto detached_result =
        fastdb::payload::view::materialize_with_metrics(
            backed_sequence, &backed_metrics);
    require(detached_result.has_value());
    const std::string backed_message =
        "peak retained path bytes=" +
        std::to_string(backed_metrics.peak_retained_diagnostic_path_bytes) +
        ", linear bound=" + std::to_string(maximum_linear_path_state);
    require(backed_metrics.peak_retained_diagnostic_path_bytes <=
                maximum_linear_path_state,
            backed_message);
    View detached_sequence = std::move(detached_result).value();

    View cursor = detached_sequence.at(UINT64_C(0)).value();
    for (std::uint32_t index = UINT32_C(0); index < depth; ++index) {
        require(cursor.kind().value() == ViewKind::list);
        require(cursor.length().value() == UINT64_C(1));
        cursor = cursor.at(UINT64_C(0)).value();
    }
    require(cursor.get_u8().value() == UINT8_C(42));

    fastdb::payload::view::MaterializeMetrics detached_metrics;
    auto repeated_result =
        fastdb::payload::view::materialize_with_metrics(
            detached_sequence, &detached_metrics);
    require(repeated_result.has_value());
    const std::string detached_message =
        "rematerialize peak retained path bytes=" +
        std::to_string(detached_metrics.peak_retained_diagnostic_path_bytes) +
        ", linear bound=" + std::to_string(maximum_linear_path_state);
    require(detached_metrics.peak_retained_diagnostic_path_bytes <=
                maximum_linear_path_state,
            detached_message);
    View repeated = std::move(repeated_result).value();
    require(repeated.at(UINT64_C(0)).value()
                .at(UINT64_C(0)).value().kind().value() == ViewKind::list);
    return EXIT_SUCCESS;
}

int test_owner_view_lifetime() {
    FakeExternalBacking backing;
    std::optional<View> retained_view;
    {
        PayloadOwner owner = open_external_all_algebra(backing);
        PayloadOwner copy = owner;
        copy = copy;
        copy = std::move(copy);
        retained_view.emplace(
            owner.entry_view(UINT32_C(3)).value().at(UINT64_C(0)).value());
        owner = copy;
        require(backing.retain_count.load(std::memory_order_relaxed) ==
                UINT32_C(1));
        require(backing.release_count.load(std::memory_order_relaxed) ==
                UINT32_C(0));
    }
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(0));
    require(retained_view->get_u32().value() == UINT32_C(0x12345678));
    View copied_view = *retained_view;
    copied_view = copied_view;
    copied_view = std::move(copied_view);
    retained_view.reset();
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(0));
    require(copied_view.get_u32().value() == UINT32_C(0x12345678));
    copied_view = View{std::move(copied_view)};
    require(copied_view.get_u32().value() == UINT32_C(0x12345678));
    View move_source = copied_view;
    View moved_view = std::move(move_source);
    auto moved_from_view = move_source.get_u32();
    require(!moved_from_view.has_value());
    require(moved_from_view.error().code() == FDB_PAYLOAD_E_INTERNAL);
    require(moved_view.get_u32().value() == UINT32_C(0x12345678));
    return EXIT_SUCCESS;
}

int test_decisive_invalidation_barrier() {
    PayloadOwner distinct_owner = build_all_algebra_owner();
    FakeExternalBacking backing;
    backing.block_release = true;
    PayloadOwner owner = open_external_all_algebra(backing);
    backing.observed_owner = &owner;
    backing.distinct_reentry_owner = &distinct_owner;
    const auto digest = owner.digest();

    View backed = owner.entry_view(UINT32_C(12)).value()
                      .at(UINT64_C(0)).value();
    View detached = backed.materialize().value();
    std::optional<Access> long_access;
    long_access.emplace(
        backed.field(UINT32_C(0)).value().field(UINT32_C(1)).value()
            .acquire().value());
    require(long_access->str().value().size == UINT64_C(4));

    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    std::atomic<std::uint32_t> first_status{UINT32_MAX};
    std::atomic<std::uint32_t> second_status{UINT32_MAX};
    TestGate second_started;
    std::thread first([&] {
        auto result = owner.invalidate();
        first_status.store(result.has_value() ? UINT32_C(0)
                                              : result.error().code(),
                           std::memory_order_relaxed);
        first_done.store(true, std::memory_order_release);
    });

    fastdb::payload::view::PayloadOwnerTestAccess::wait_until_invalidating(
        owner);
    const auto active_facts =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(active_facts.invalidating);
    require(!active_facts.invalidated);
    require(active_facts.active_accesses == UINT64_C(1));
    require(!first_done.load(std::memory_order_acquire));

    auto rejected_owner_access = owner.acquire();
    require(!rejected_owner_access.has_value());
    require(rejected_owner_access.error().code() ==
            FDB_PAYLOAD_E_VIEW_INVALIDATED);
    auto rejected_entry = owner.entry_view(UINT32_C(0));
    require(!rejected_entry.has_value());
    require(rejected_entry.error().code() == FDB_PAYLOAD_E_VIEW_INVALIDATED);
    auto rejected_kind = backed.kind();
    require(!rejected_kind.has_value());
    require(rejected_kind.error().code() == FDB_PAYLOAD_E_VIEW_INVALIDATED);
    auto rejected_materialize = backed.materialize();
    require(!rejected_materialize.has_value());
    require(rejected_materialize.error().code() ==
            FDB_PAYLOAD_E_VIEW_INVALIDATED);

    long_access.reset();
    require(backing.wait_for_release());
    require(!first_done.load(std::memory_order_acquire));
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    require(backing.release_saw_unlocked_barrier.load(
        std::memory_order_relaxed));
    require(backing.distinct_reentry_succeeded.load(
        std::memory_order_relaxed));
    require(std::all_of(backing.storage.begin(), backing.storage.end(),
                        [](std::uint8_t byte) {
                            return byte == UINT8_C(0xa5);
                        }));
    std::thread second([&] {
        second_started.open();
        auto result = owner.invalidate();
        second_status.store(result.has_value() ? UINT32_C(0)
                                               : result.error().code(),
                            std::memory_order_relaxed);
        second_done.store(true, std::memory_order_release);
    });
    require(second_started.wait());
    require(fastdb::payload::view::PayloadOwnerTestAccess::
                wait_until_invalidator_waiting(owner));
    const auto waiting_facts =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(waiting_facts.waiting_invalidators == UINT64_C(1));
    require(!second_done.load(std::memory_order_acquire));
    backing.unblock_release();
    first.join();
    second.join();
    require(first_status.load(std::memory_order_relaxed) == UINT32_C(0));
    require(second_status.load(std::memory_order_relaxed) == UINT32_C(0));
    require(owner.invalidate().has_value());
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    require(owner.digest() == digest);

    const auto final_facts =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(final_facts.invalidated);
    require(!final_facts.invalidating);
    require(final_facts.active_accesses == UINT64_C(0));
    require(final_facts.waiting_invalidators == UINT64_C(0));
    require(!final_facts.has_backing);
    require(!backed.field_count().has_value());
    require(backed.field_count().error().code() ==
            FDB_PAYLOAD_E_VIEW_INVALIDATED);
    require(detached.field(UINT32_C(0)).value().field(UINT32_C(0)).value()
                .get_u32().value() == UINT32_C(7));
    require(detached.field(UINT32_C(1)).value().length().value() ==
            UINT64_C(3));
    return EXIT_SUCCESS;
}

int test_one_hundred_thousand_short_readers() {
    PayloadOwner owner = build_all_algebra_owner();
    const View scalar = owner.entry_view(UINT32_C(3)).value()
                            .at(UINT64_C(0)).value();
    constexpr std::uint32_t thread_count = UINT32_C(8);
    constexpr std::uint32_t reads_per_thread = UINT32_C(12500);
    static_assert(thread_count * reads_per_thread == UINT32_C(100000));
    TestGate start;
    std::atomic<std::uint32_t> ready{UINT32_C(0)};
    std::atomic<std::uint32_t> failures{UINT32_C(0)};
    std::vector<std::thread> readers;
    readers.reserve(thread_count);
    for (std::uint32_t thread_index = UINT32_C(0);
         thread_index < thread_count; ++thread_index) {
        readers.emplace_back([&] {
            ready.fetch_add(UINT32_C(1), std::memory_order_release);
            if (!start.wait()) {
                failures.fetch_add(UINT32_C(1), std::memory_order_relaxed);
                return;
            }
            for (std::uint32_t index = UINT32_C(0);
                 index < reads_per_thread; ++index) {
                auto value = scalar.get_u32();
                if (!value.has_value() ||
                    value.value() != UINT32_C(0x12345678)) {
                    failures.fetch_add(UINT32_C(1),
                                       std::memory_order_relaxed);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start.open();
    for (auto& reader : readers) {
        reader.join();
    }
    require(failures.load(std::memory_order_relaxed) == UINT32_C(0));
    const auto facts =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(facts.active_accesses == UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_reader_invalidation_race() {
    FakeExternalBacking backing;
    PayloadOwner owner = open_external_all_algebra(backing);
    const View scalar = owner.entry_view(UINT32_C(3)).value()
                            .at(UINT64_C(0)).value();
    constexpr std::uint32_t thread_count = UINT32_C(4);
    constexpr std::uint32_t reads_per_thread = UINT32_C(5000);
    constexpr std::uint32_t total_reads = thread_count * reads_per_thread;
    TestGate start;
    std::atomic<std::uint32_t> ready{UINT32_C(0)};
    std::atomic<std::uint32_t> successful{UINT32_C(0)};
    std::atomic<std::uint32_t> invalidated{UINT32_C(0)};
    std::atomic<std::uint32_t> unexpected{UINT32_C(0)};
    std::vector<std::thread> readers;
    readers.reserve(thread_count);
    for (std::uint32_t thread_index = UINT32_C(0);
         thread_index < thread_count; ++thread_index) {
        readers.emplace_back([&] {
            ready.fetch_add(UINT32_C(1), std::memory_order_release);
            if (!start.wait()) {
                unexpected.fetch_add(UINT32_C(1),
                                     std::memory_order_relaxed);
                return;
            }
            for (std::uint32_t index = UINT32_C(0);
                 index < reads_per_thread; ++index) {
                auto result = scalar.get_u32();
                if (result.has_value() &&
                    result.value() == UINT32_C(0x12345678)) {
                    successful.fetch_add(UINT32_C(1),
                                         std::memory_order_relaxed);
                } else if (!result.has_value() &&
                           result.error().code() ==
                               FDB_PAYLOAD_E_VIEW_INVALIDATED) {
                    invalidated.fetch_add(UINT32_C(1),
                                          std::memory_order_relaxed);
                } else {
                    unexpected.fetch_add(UINT32_C(1),
                                         std::memory_order_relaxed);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    std::atomic<std::uint32_t> invalidation_status{UINT32_MAX};
    std::thread invalidator([&] {
        if (!start.wait()) {
            invalidation_status.store(FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE,
                                      std::memory_order_relaxed);
            return;
        }
        auto result = owner.invalidate();
        invalidation_status.store(result.has_value() ? UINT32_C(0)
                                                     : result.error().code(),
                                  std::memory_order_relaxed);
    });
    start.open();
    for (auto& reader : readers) {
        reader.join();
    }
    invalidator.join();
    require(invalidation_status.load(std::memory_order_relaxed) ==
            UINT32_C(0));
    require(unexpected.load(std::memory_order_relaxed) == UINT32_C(0));
    require(successful.load(std::memory_order_relaxed) +
                invalidated.load(std::memory_order_relaxed) ==
            total_reads);
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    return EXIT_SUCCESS;
}

int test_generation_exhaustion_and_pin_accounting() {
    PayloadOwner owner = build_all_algebra_owner();
    View stale = owner.entry_view(UINT32_C(3)).value()
                     .at(UINT64_C(0)).value();
    auto initial =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(initial.generation == UINT64_C(1));
    require(initial.active_accesses == UINT64_C(0));

    fastdb::payload::view::PayloadOwnerTestAccess::set_generation(
        owner, UINT64_C(2));
    auto stale_result = stale.get_u32();
    require(!stale_result.has_value());
    require(stale_result.error().code() == FDB_PAYLOAD_E_STALE_GENERATION);
    View current = owner.entry_view(UINT32_C(3)).value()
                       .at(UINT64_C(0)).value();
    std::optional<Access> pin;
    pin.emplace(owner.acquire().value());
    auto pinned =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(pinned.active_accesses == UINT64_C(1));
    pin.reset();
    auto drained =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(drained.active_accesses == UINT64_C(0));
    require(current.get_u32().value() == UINT32_C(0x12345678));

    fastdb::payload::view::PayloadOwnerTestAccess::set_generation(
        owner, UINT64_MAX);
    View exhausted = owner.entry_view(UINT32_C(3)).value()
                         .at(UINT64_C(0)).value();
    require(exhausted.get_u32().value() == UINT32_C(0x12345678));
    require(owner.invalidate().has_value());
    auto final =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(final.generation == UINT64_MAX);
    require(final.invalidated);
    require(final.active_accesses == UINT64_C(0));
    require(!final.has_backing);
    auto exhausted_result = exhausted.get_u32();
    require(!exhausted_result.has_value());
    require(exhausted_result.error().code() ==
            FDB_PAYLOAD_E_VIEW_INVALIDATED);
    require(owner.invalidate().has_value());
    require(fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner)
                .generation == UINT64_MAX);
    return EXIT_SUCCESS;
}

int test_materialize_allocation_failure_is_transactional() {
    allocation_failure::Reset reset;
    PayloadOwner owner = build_all_algebra_owner();
    const View source = owner.entry_view(UINT32_C(12)).value()
                            .at(UINT64_C(0)).value();
    bool saw_allocation_failure = false;
    bool saw_success = false;
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(4096);
         ++allocation) {
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        auto result = source.materialize();
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (result.has_value()) {
            saw_success = true;
            require(result.value().field(UINT32_C(0)).value()
                        .field(UINT32_C(0)).value().get_u32().value() ==
                    UINT32_C(7));
            break;
        }
        saw_allocation_failure = true;
        require(result.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(source.field(UINT32_C(1)).value().length().value() ==
                UINT64_C(3));
        require(source.field(UINT32_C(0)).value().field(UINT32_C(0)).value()
                    .get_u32().value() == UINT32_C(7));
    }
    require(saw_allocation_failure);
    require(saw_success);
    return EXIT_SUCCESS;
}

Result<PayloadOwner> open_copy_all_algebra(
    const std::vector<std::uint8_t>& image,
    fastdb::payload::view::OpenOptions options) {
    auto compiled = CompiledSpec::compile(all_algebra_spec);
    if (!compiled.has_value()) {
        return Result<PayloadOwner>::failure(std::move(compiled).error());
    }
    return PayloadOwner::open_copy(std::move(compiled).value(), image.data(),
                                   image.size(), options);
}

int test_lazy_text_validation_retry_and_concurrency() {
    PayloadOwner valid = build_all_algebra_owner();
    Access payload = valid.acquire().value();
    View selected = valid.entry_view(UINT32_C(9)).value()
                        .at(UINT64_C(3)).value();
    Access selected_access = selected.acquire().value();
    const auto payload_span = payload.payload_bytes().value();
    const auto selected_span = selected_access.str().value();
    require(selected_span.size == UINT64_C(3));
    require(selected_span.data >= payload_span.data);
    require(selected_span.data + selected_span.size <=
            payload_span.data + payload_span.size);
    const std::uint64_t selected_offset = static_cast<std::uint64_t>(
        selected_span.data - payload_span.data);
    std::vector<std::uint8_t> corrupted(
        payload_span.data,
        payload_span.data + static_cast<std::ptrdiff_t>(payload_span.size));
    corrupted[static_cast<std::size_t>(selected_offset)] = UINT8_C(0xff);

    auto eager_options = fastdb::payload::view::default_open_options();
    auto eager = open_copy_all_algebra(corrupted, eager_options);
    require(!eager.has_value());
    require(eager.error().code() == FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);

    auto lazy_options = fastdb::payload::view::default_open_options();
    lazy_options.validate_text_eager = false;
    auto lazy_result = open_copy_all_algebra(corrupted, lazy_options);
    require(lazy_result.has_value());
    PayloadOwner lazy = std::move(lazy_result).value();
    View invalid = lazy.entry_view(UINT32_C(9)).value()
                       .at(UINT64_C(3)).value();
    auto first = invalid.acquire();
    require(!first.has_value());
    require(first.error().code() == FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);
    const std::string expected_path(first.error().path());
    const std::string expected_details(first.error().details_json());
    auto retry = invalid.acquire();
    require(!retry.has_value());
    require(retry.error().code() == FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);
    require(retry.error().path() == expected_path);
    require(retry.error().details_json() == expected_details);

    constexpr std::uint32_t thread_count = UINT32_C(8);
    constexpr std::uint32_t attempts_per_thread = UINT32_C(100);
    TestGate start;
    std::atomic<std::uint32_t> ready{UINT32_C(0)};
    std::atomic<std::uint32_t> failures{UINT32_C(0)};
    std::vector<std::thread> readers;
    readers.reserve(thread_count);
    for (std::uint32_t index = UINT32_C(0); index < thread_count; ++index) {
        readers.emplace_back([&] {
            ready.fetch_add(UINT32_C(1), std::memory_order_release);
            if (!start.wait()) {
                failures.fetch_add(UINT32_C(1),
                                   std::memory_order_relaxed);
                return;
            }
            for (std::uint32_t attempt = UINT32_C(0);
                 attempt < attempts_per_thread; ++attempt) {
                auto result = invalid.acquire();
                if (result.has_value() ||
                    result.error().code() !=
                        FDB_PAYLOAD_E_INVALID_TEXT_ENCODING ||
                    result.error().path() != expected_path ||
                    result.error().details_json() != expected_details) {
                    failures.fetch_add(UINT32_C(1),
                                       std::memory_order_relaxed);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start.open();
    for (auto& reader : readers) {
        reader.join();
    }
    require(failures.load(std::memory_order_relaxed) == UINT32_C(0));

    View empty = lazy.entry_view(UINT32_C(9)).value()
                     .at(UINT64_C(1)).value();
    require(empty.acquire().value().str().value().size == UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_lazy_wstr_validation_retry_and_concurrency() {
    PayloadOwner valid = build_all_algebra_owner();
    Access payload = valid.acquire().value();
    const auto payload_span = payload.payload_bytes().value();
    std::vector<std::uint8_t> corrupted(
        payload_span.data,
        payload_span.data + static_cast<std::ptrdiff_t>(payload_span.size));
    const std::array<std::uint8_t, 4> unique_pair{{
        UINT8_C(0x3d), UINT8_C(0xd8), UINT8_C(0x03), UINT8_C(0xde)}};
    auto occurrence = std::search(corrupted.begin(), corrupted.end(),
                                  unique_pair.begin(), unique_pair.end());
    require(occurrence != corrupted.end());
    require(std::search(occurrence + 1, corrupted.end(), unique_pair.begin(),
                        unique_pair.end()) == corrupted.end());
    occurrence[2] = UINT8_C(0x41);
    occurrence[3] = UINT8_C(0x00);

    auto eager_options = fastdb::payload::view::default_open_options();
    auto eager = open_copy_all_algebra(corrupted, eager_options);
    require(!eager.has_value());
    require(eager.error().code() == FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);

    auto lazy_options = fastdb::payload::view::default_open_options();
    lazy_options.validate_text_eager = false;
    auto lazy_result = open_copy_all_algebra(corrupted, lazy_options);
    require(lazy_result.has_value());
    PayloadOwner lazy = std::move(lazy_result).value();
    View invalid = lazy.entry_view(UINT32_C(10)).value()
                       .at(UINT64_C(3)).value();
    auto first = invalid.acquire();
    require(!first.has_value());
    require(first.error().code() == FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);
    const std::string expected_path(first.error().path());
    const std::string expected_details(first.error().details_json());
    auto retry = invalid.acquire();
    require(!retry.has_value());
    require(retry.error().code() == FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);
    require(retry.error().path() == expected_path);
    require(retry.error().details_json() == expected_details);

    constexpr std::uint32_t thread_count = UINT32_C(8);
    constexpr std::uint32_t attempts_per_thread = UINT32_C(100);
    TestGate start;
    std::atomic<std::uint32_t> ready{UINT32_C(0)};
    std::atomic<std::uint32_t> failures{UINT32_C(0)};
    std::vector<std::thread> readers;
    readers.reserve(thread_count);
    for (std::uint32_t index = UINT32_C(0); index < thread_count; ++index) {
        readers.emplace_back([&] {
            ready.fetch_add(UINT32_C(1), std::memory_order_release);
            if (!start.wait()) {
                failures.fetch_add(UINT32_C(1),
                                   std::memory_order_relaxed);
                return;
            }
            for (std::uint32_t attempt = UINT32_C(0);
                 attempt < attempts_per_thread; ++attempt) {
                auto result = invalid.acquire();
                if (result.has_value() ||
                    result.error().code() !=
                        FDB_PAYLOAD_E_INVALID_TEXT_ENCODING ||
                    result.error().path() != expected_path ||
                    result.error().details_json() != expected_details) {
                    failures.fetch_add(UINT32_C(1),
                                       std::memory_order_relaxed);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start.open();
    for (auto& reader : readers) {
        reader.join();
    }
    require(failures.load(std::memory_order_relaxed) == UINT32_C(0));
    return EXIT_SUCCESS;
}

int test_lazy_selected_span_work_boundary() {
    const std::vector<std::uint8_t> image = build_all_algebra_image();
    auto baseline_options = fastdb::payload::view::default_open_options();
    baseline_options.validate_text_eager = false;
    auto baseline_result = open_copy_all_algebra(image, baseline_options);
    require(baseline_result.has_value());
    PayloadOwner baseline = std::move(baseline_result).value();
    const std::uint64_t structural_work =
        fastdb::payload::view::PayloadOwnerTestAccess::validation_work(
            baseline);
    require(structural_work <= UINT64_MAX - UINT64_C(3));

    auto short_options = baseline_options;
    short_options.max_validation_work = structural_work + UINT64_C(2);
    auto short_result = open_copy_all_algebra(image, short_options);
    require(short_result.has_value());
    PayloadOwner short_owner = std::move(short_result).value();
    View selected = short_owner.entry_view(UINT32_C(9)).value()
                        .at(UINT64_C(3)).value();
    auto short_access = selected.acquire();
    require(!short_access.has_value());
    require(short_access.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    auto short_retry = selected.acquire();
    require(!short_retry.has_value());
    require(short_retry.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(short_retry.error().path() == short_access.error().path());
    require(short_retry.error().details_json() ==
            short_access.error().details_json());
    require(short_owner.entry_view(UINT32_C(9)).value()
                .at(UINT64_C(1)).value().acquire().value().str().value().size ==
            UINT64_C(0));

    auto exact_options = baseline_options;
    exact_options.max_validation_work = structural_work + UINT64_C(3);
    auto exact_result = open_copy_all_algebra(image, exact_options);
    require(exact_result.has_value());
    PayloadOwner exact_owner = std::move(exact_result).value();
    auto exact_access = exact_owner.entry_view(UINT32_C(9)).value()
                            .at(UINT64_C(3)).value().acquire();
    require(exact_access.has_value());
    require(exact_access.value().str().value().size == UINT64_C(3));
    require(std::memcmp(exact_access.value().str().value().data,
                        "\xe4\xb8\xad", 3U) == 0);
    return EXIT_SUCCESS;
}

int run_checked_view_behavior_suite() {
    try {
        if (test_navigation_scalars_and_errors() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_access_and_materialize() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_materialize_every_entry_and_value_kind() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_recursive_list_materialization_layout() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_deep_materialize_retains_linear_path_state() !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_owner_view_lifetime() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_decisive_invalidation_barrier() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_one_hundred_thousand_short_readers() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_reader_invalidation_race() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_generation_exhaustion_and_pin_accounting() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_materialize_allocation_failure_is_transactional() !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_lazy_text_validation_retry_and_concurrency() !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_lazy_wstr_validation_retry_and_concurrency() !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_lazy_selected_span_work_boundary() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
    } catch (const std::exception& error) {
        std::cerr << "P2 Task 8 behavior fixture failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#endif

}  // namespace

int main() {
    if (verify_matrix_inventory() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
#if FASTDB_TASK8_HAS_CHECKED_VIEW
    return run_checked_view_behavior_suite();
#else
    std::cerr
        << "P2 Task 8 RED: payload/view/AccessBarrier.hpp, View.hpp, and "
           "Materialize.hpp are absent; checked views, scoped access, "
           "detached materialization, and invalidation are not implemented\n";
    return EXIT_FAILURE;
#endif
}
