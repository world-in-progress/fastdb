#include "GoldenCorpus.hpp"
#include "TestSupport.hpp"

#include "payload/backing/Backing.hpp"
#include "payload/build/PayloadBuilder.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Materialize.hpp"
#include "payload/view/PayloadOwner.hpp"
#include "payload/view/View.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#include <utility>
#include <vector>

namespace allocation_failure {

thread_local std::int64_t fail_after = INT64_C(-1);
thread_local std::int64_t block_after = INT64_C(-1);

std::mutex block_mutex;
std::condition_variable block_changed;
bool allocation_blocked = false;
bool allocation_released = false;

struct AllocationHeader final {
    void* raw;
};

constexpr std::size_t default_alignment =
    static_cast<std::size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);

void* allocate(std::size_t size,
               std::size_t alignment = default_alignment) {
    if (fail_after >= INT64_C(0)) {
        if (fail_after == INT64_C(0)) {
            fail_after = INT64_C(-1);
            throw std::bad_alloc();
        }
        --fail_after;
    }
    if (block_after >= INT64_C(0)) {
        if (block_after == INT64_C(0)) {
            block_after = INT64_C(-1);
            std::unique_lock<std::mutex> lock(block_mutex);
            allocation_blocked = true;
            block_changed.notify_all();
            block_changed.wait(lock, [] { return allocation_released; });
        } else {
            --block_after;
        }
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        throw std::bad_alloc();
    }
    alignment = std::max(
        {alignment, alignof(AllocationHeader), default_alignment});
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
    void* const aligned =
        std::align(alignment, payload, candidate, space);
    if (aligned == nullptr) {
        std::free(raw);
        throw std::bad_alloc();
    }
    (static_cast<AllocationHeader*>(aligned) - 1)->raw = raw;
    return aligned;
}

void deallocate(void* value) noexcept {
    if (value != nullptr) {
        std::free((static_cast<AllocationHeader*>(value) - 1)->raw);
    }
}

void prepare_block() {
    std::lock_guard<std::mutex> lock(block_mutex);
    allocation_blocked = false;
    allocation_released = false;
}

bool wait_until_blocked() {
    std::unique_lock<std::mutex> lock(block_mutex);
    return block_changed.wait_for(
        lock, std::chrono::seconds(10), [] { return allocation_blocked; });
}

void release_block() {
    std::lock_guard<std::mutex> lock(block_mutex);
    allocation_released = true;
    block_changed.notify_all();
}

struct Reset final {
    ~Reset() {
        fail_after = INT64_C(-1);
        block_after = INT64_C(-1);
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
    return allocation_failure::allocate(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(
        size, static_cast<std::size_t>(alignment));
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
void operator delete(void* value,
                     std::size_t,
                     std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value,
                       std::size_t,
                       std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}

namespace fastdb::payload::view {

struct GraphMaterializeBarrierFacts final {
    std::uint64_t generation;
    std::uint64_t active_accesses;
    bool invalidating;
    bool invalidated;
};

struct PayloadOwnerTestAccess final {
    static GraphMaterializeBarrierFacts barrier_facts(
        const PayloadOwner& owner) {
        std::lock_guard<std::mutex> lock(owner.state_->barrier.mutex);
        return GraphMaterializeBarrierFacts{
            owner.state_->barrier.generation,
            owner.state_->barrier.active_accesses,
            owner.state_->barrier.invalidating,
            owner.state_->barrier.invalidated};
    }

    static bool wait_until_invalidating(const PayloadOwner& owner) {
        std::unique_lock<std::mutex> lock(owner.state_->barrier.mutex);
        return owner.state_->barrier.drained.wait_for(
            lock, std::chrono::seconds(10), [&owner] {
                return owner.state_->barrier.invalidating;
            });
    }
};

struct ViewTestAccess final {
    static std::uint64_t object_count(const View& view,
                                      std::uint32_t component) noexcept {
        if (view.state_ == nullptr || view.state_->is_backed ||
            view.state_->detached == nullptr ||
            component >= view.state_->detached->object_pools.size()) {
            return UINT64_C(0);
        }
        return static_cast<std::uint64_t>(
            view.state_->detached->object_pools[component].size());
    }

    static const void* detached_identity(const View& view) noexcept {
        return view.state_ == nullptr ? nullptr
                                     : view.state_->detached.get();
    }

    static bool detached_is_source_independent(const View& view) noexcept {
        return view.state_ != nullptr && !view.state_->is_backed &&
               view.state_->owner == nullptr &&
               view.state_->detached != nullptr;
    }
};

}  // namespace fastdb::payload::view

namespace {

using fastdb::payload::error::Result;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::spec::CompiledSpec;
using fastdb::payload::view::PayloadOwner;
using fastdb::payload::view::View;
using fastdb::payload::view::ViewKind;
using fastdb::test::payload::BinaryGoldenCase;

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

std::uint8_t hex_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    return static_cast<std::uint8_t>(value - 'a' + 10);
}

std::vector<std::uint8_t> decode_hex(std::string_view hexadecimal) {
    std::vector<std::uint8_t> bytes(hexadecimal.size() / 2U, UINT8_C(0));
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            (hex_nibble(hexadecimal[index * 2U]) << UINT8_C(4)) |
            hex_nibble(hexadecimal[index * 2U + 1U]));
    }
    return bytes;
}

const BinaryGoldenCase& find_case(
    const std::vector<BinaryGoldenCase>& cases,
    std::string_view name) {
    const auto found = std::find_if(
        cases.begin(), cases.end(), [name](const BinaryGoldenCase& item) {
            return item.name == name;
        });
    if (found == cases.end()) {
        throw std::runtime_error("missing graph materialize golden case");
    }
    return *found;
}

PayloadOwner open_copy(const BinaryGoldenCase& item) {
    std::vector<std::uint8_t> bytes = decode_hex(item.success.binary_hex);
    CompiledSpec compiled =
        take(CompiledSpec::compile(item.success.source), "compile graph");
    return take(PayloadOwner::open_copy(
                    std::move(compiled), bytes.data(), bytes.size()),
                "open graph copy");
}

std::uint32_t component_index(const CompiledSpec& compiled,
                              std::string_view id) {
    const auto index = compiled.component_index(id);
    if (!index.has_value()) {
        throw std::runtime_error("missing graph materialize component");
    }
    return *index;
}

std::uint32_t component_index(const BinaryGoldenCase& item,
                              std::string_view id) {
    CompiledSpec compiled =
        take(CompiledSpec::compile(item.success.source), "compile component");
    return component_index(compiled, id);
}

void require_error_code(const fastdb::payload::error::Error& error,
                        std::uint32_t code) {
    if (error.code() != code) {
        throw std::runtime_error("unexpected graph materialize error code");
    }
}

struct ExternalBacking final {
    std::vector<std::uint8_t> storage;
    std::atomic<std::uint32_t> retain_count{UINT32_C(0)};
    std::atomic<std::uint32_t> release_count{UINT32_C(0)};

    fastdb::payload::backing::Callbacks callbacks() noexcept {
        return fastdb::payload::backing::Callbacks{
            this, nullptr, nullptr, nullptr, nullptr,
            &retain_thunk, &release_thunk};
    }

private:
    static std::uint32_t retain_thunk(void* context, void*) {
        static_cast<ExternalBacking*>(context)->retain_count.fetch_add(
            UINT32_C(1), std::memory_order_relaxed);
        return UINT32_C(0);
    }

    static void release_thunk(void* context, void*) {
        auto& self = *static_cast<ExternalBacking*>(context);
        self.release_count.fetch_add(UINT32_C(1),
                                     std::memory_order_relaxed);
        std::fill(self.storage.begin(), self.storage.end(), UINT8_C(0xa5));
    }
};

PayloadOwner open_external(const BinaryGoldenCase& item,
                           ExternalBacking& backing) {
    backing.storage = decode_hex(item.success.binary_hex);
    auto retained = take(
        fastdb::payload::backing::RetainedBacking::acquire(
            backing.callbacks(), &backing, backing.storage.data(),
            backing.storage.size()),
        "retain external graph backing");
    CompiledSpec compiled = take(
        CompiledSpec::compile(item.success.source), "compile external graph");
    return take(PayloadOwner::open_external(
                    std::move(compiled), backing.storage.data(),
                    backing.storage.size(), std::move(retained)),
                "open external graph");
}

constexpr std::string_view closure_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"roots","cardinality":"many","type":{"kind":"component","id":"Node"}},
    {"id":"focus_ref","cardinality":"one","type":{"kind":"ref","target":"Node"}},
    {"id":"holder","cardinality":"one","type":{"kind":"component","id":"Holder"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"value","type":{"kind":"u32"}},
      {"id":"next","type":{"kind":"ref","target":"Node","nullable":true}},
      {"id":"asset","type":{"kind":"ref","target":"Asset"}}
    ]},
    {"id":"Asset","kind":"record","fields":[
      {"id":"code","type":{"kind":"u16"}},
      {"id":"owner","type":{"kind":"ref","target":"Node"}}
    ]},
    {"id":"Envelope","kind":"record","fields":[
      {"id":"start","type":{"kind":"ref","target":"Node"}},
      {"id":"assets","type":{"kind":"list","items":{"kind":"ref","target":"Asset"}}}
    ]},
    {"id":"Holder","kind":"record","fields":[
      {"id":"envelope","type":{"kind":"component","id":"Envelope"}}
    ]}
  ]
})";

struct ClosureFixture final {
    PayloadOwner owner;
    std::uint32_t node;
    std::uint32_t asset;
    std::uint32_t envelope;
};

ClosureFixture build_closure_fixture() {
    CompiledSpec compiled =
        take(CompiledSpec::compile(closure_spec), "compile closure graph");
    const std::uint32_t node = component_index(compiled, "Node");
    const std::uint32_t asset = component_index(compiled, "Asset");
    const std::uint32_t envelope = component_index(compiled, "Envelope");
    const std::uint32_t holder = component_index(compiled, "Holder");
    PayloadBuilder builder = take(
        PayloadBuilder::create(std::move(compiled)), "create closure graph");

    const auto node0 = take(builder.declare_object(node), "declare node 0");
    const auto asset0 =
        take(builder.declare_object(asset), "declare asset 0");
    const auto node1 = take(builder.declare_object(node), "declare node 1");
    const auto asset1 =
        take(builder.declare_object(asset), "declare asset 1");
    const auto node2 = take(builder.declare_object(node), "declare node 2");
    const auto holder0 =
        take(builder.declare_object(holder), "declare holder 0");

    take(builder.begin_object_fill(node0), "fill node 0");
    take(builder.push_u32(UINT32_C(10)), "node 0 value");
    take(builder.push_null(), "node 0 next");
    take(builder.push_ref(asset0), "node 0 asset");

    take(builder.begin_object_fill(node1), "fill node 1");
    take(builder.push_u32(UINT32_C(20)), "node 1 value");
    take(builder.push_ref(node2), "node 1 next");
    take(builder.push_ref(asset1), "node 1 asset");

    take(builder.begin_object_fill(node2), "fill node 2");
    take(builder.push_u32(UINT32_C(30)), "node 2 value");
    take(builder.push_ref(node1), "node 2 next");
    take(builder.push_ref(asset1), "node 2 asset");

    take(builder.begin_object_fill(asset0), "fill asset 0");
    take(builder.push_u16(UINT16_C(100)), "asset 0 code");
    take(builder.push_ref(node0), "asset 0 owner");

    take(builder.begin_object_fill(asset1), "fill asset 1");
    take(builder.push_u16(UINT16_C(200)), "asset 1 code");
    take(builder.push_ref(node2), "asset 1 owner");

    take(builder.begin_object_fill(holder0), "fill holder 0");
    take(builder.begin_component(), "inline envelope");
    take(builder.push_ref(node1), "envelope start");
    take(builder.begin_list(UINT64_C(2)), "envelope assets");
    take(builder.push_ref(asset1), "envelope first asset");
    take(builder.push_ref(asset1), "envelope shared asset");

    take(builder.begin_entry(UINT32_C(0), UINT64_C(2)), "roots entry");
    take(builder.push_object(node0), "root node 0");
    take(builder.push_object(node1), "root node 1");
    take(builder.begin_entry(UINT32_C(1), UINT64_C(1)), "ref entry");
    take(builder.push_ref(node2), "focus ref");
    take(builder.begin_entry(UINT32_C(2), UINT64_C(1)), "holder entry");
    take(builder.push_object(holder0), "holder root");

    auto plan = take(builder.freeze_plan(), "freeze closure graph");
    PayloadOwner owner = take(
        plan.execute(FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, nullptr),
        "execute closure graph");
    return ClosureFixture{
        std::move(owner), node, asset, envelope};
}

int test_union_closure_dense_remap_and_root_kinds() {
    ClosureFixture fixture = build_closure_fixture();
    View roots = take(fixture.owner.entry_view(UINT32_C(0)), "closure roots");
    require(take(roots.kind(), "closure roots kind") == ViewKind::sequence);

    View all = take(roots.materialize(), "materialize union closure");
    require(take(all.kind(), "union root kind") == ViewKind::sequence);
    require(take(all.length(), "union root length") == UINT64_C(2));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                all, fixture.node) == UINT64_C(3));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                all, fixture.asset) == UINT64_C(2));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                all, fixture.envelope) == UINT64_C(0));
    require(take(take(all.at(UINT64_C(0)), "union node 0")
                     .graph_identity(),
                 "union node 0 identity")
                .object_id == UINT64_C(0));
    require(take(take(all.at(UINT64_C(1)), "union node 1")
                     .graph_identity(),
                 "union node 1 identity")
                .object_id == UINT64_C(1));

    View source_second = take(roots.at(UINT64_C(1)), "selected node 1");
    View selected = take(source_second.materialize(),
                         "materialize selected closure");
    require(take(selected.kind(), "selected root kind") ==
            ViewKind::component);
    require(take(selected.graph_identity(), "selected identity")
                .object_id == UINT64_C(0));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                selected, fixture.node) == UINT64_C(2));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                selected, fixture.asset) == UINT64_C(1));
    require(take(take(selected.field(UINT32_C(0)), "selected value")
                     .get_u32(),
                 "selected value bits") == UINT32_C(20));

    View selected_next = take(selected.field(UINT32_C(1)), "selected next");
    require(take(selected_next.kind(), "selected next kind") ==
            ViewKind::ref);
    require(take(selected_next.graph_identity(), "selected next identity")
                .object_id == UINT64_C(1));
    View selected_node2 =
        take(selected_next.ref_target(), "selected node 2 target");
    require(take(take(selected_node2.field(UINT32_C(0)), "node 2 value")
                     .get_u32(),
                 "node 2 bits") == UINT32_C(30));
    require(take(take(selected_node2.field(UINT32_C(1)), "node 2 next")
                     .graph_identity(),
                 "node 2 next identity")
                .object_id == UINT64_C(0));

    const auto first_asset = take(
        take(selected.field(UINT32_C(2)), "node 1 asset").graph_identity(),
        "node 1 asset identity");
    const auto second_asset = take(
        take(selected_node2.field(UINT32_C(2)), "node 2 asset")
            .graph_identity(),
        "node 2 asset identity");
    require(first_asset.component_index == fixture.asset);
    require(first_asset.object_id == UINT64_C(0));
    require(second_asset.component_index == first_asset.component_index);
    require(second_asset.object_id == first_asset.object_id);
    View asset_target = take(
        take(selected.field(UINT32_C(2)), "shared asset ref").ref_target(),
        "shared asset target");
    require(take(take(asset_target.field(UINT32_C(0)), "asset code")
                     .get_u16(),
                 "asset code bits") == UINT16_C(200));
    require(take(take(asset_target.field(UINT32_C(1)), "asset owner")
                     .graph_identity(),
                 "asset owner identity")
                .object_id == UINT64_C(1));

    View source_ref = take(
        take(fixture.owner.entry_view(UINT32_C(1)), "focus entry")
            .at(UINT64_C(0)),
        "focus source ref");
    View detached_ref = take(source_ref.materialize(), "materialize ref root");
    require(take(detached_ref.kind(), "detached ref kind") == ViewKind::ref);
    require(take(detached_ref.graph_identity(), "detached ref identity")
                .object_id == UINT64_C(1));
    require(take(take(take(detached_ref.ref_target(), "detached ref target")
                          .field(UINT32_C(0)),
                      "detached ref value")
                     .get_u32(),
                 "detached ref value bits") == UINT32_C(30));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached_ref, fixture.node) == UINT64_C(2));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached_ref, fixture.asset) == UINT64_C(1));

    View source_holder = take(
        take(fixture.owner.entry_view(UINT32_C(2)), "holder source entry")
            .at(UINT64_C(0)),
        "source holder");
    View source_envelope =
        take(source_holder.field(UINT32_C(0)), "source envelope");
    View detached_envelope = take(
        source_envelope.materialize(), "materialize inline envelope");
    require(take(detached_envelope.kind(), "envelope root kind") ==
            ViewKind::component);
    require(take(detached_envelope.component_index(), "envelope component") ==
            fixture.envelope);
    auto inline_identity = detached_envelope.graph_identity();
    require(!inline_identity.has_value());
    require_error_code(inline_identity.error(), FDB_PAYLOAD_E_TYPE_MISMATCH);
    View start = take(detached_envelope.field(UINT32_C(0)), "envelope start");
    require(take(start.graph_identity(), "envelope start identity")
                .object_id == UINT64_C(0));
    View assets = take(detached_envelope.field(UINT32_C(1)), "asset list");
    require(take(assets.length(), "asset list length") == UINT64_C(2));
    const auto asset_a = take(
        take(assets.at(UINT64_C(0)), "first asset ref").graph_identity(),
        "first asset ref identity");
    const auto asset_b = take(
        take(assets.at(UINT64_C(1)), "second asset ref").graph_identity(),
        "second asset ref identity");
    require(asset_a.component_index == fixture.asset);
    require(asset_a.object_id == UINT64_C(0));
    require(asset_b.component_index == asset_a.component_index);
    require(asset_b.object_id == asset_a.object_id);

    View detached_asset_list = take(
        take(source_envelope.field(UINT32_C(1)), "source asset list")
            .materialize(),
        "materialize list root");
    require(take(detached_asset_list.kind(), "detached list root kind") ==
            ViewKind::list);
    require(take(detached_asset_list.length(), "detached list root length") ==
            UINT64_C(2));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached_asset_list, fixture.node) == UINT64_C(2));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached_asset_list, fixture.asset) == UINT64_C(1));
    require(take(take(detached_asset_list.at(UINT64_C(0)),
                      "detached list first ref")
                     .graph_identity(),
                 "detached list first identity")
                .object_id == UINT64_C(0));

    std::optional<View> once;
    once.emplace(take(source_second.materialize(), "first detached graph"));
    View twice = take(once->materialize(), "second detached graph");
    require(fastdb::payload::view::ViewTestAccess::detached_identity(*once) !=
            fastdb::payload::view::ViewTestAccess::detached_identity(twice));
    require(fastdb::payload::view::ViewTestAccess::detached_is_source_independent(
                twice));
    once.reset();

    take(fixture.owner.invalidate(), "invalidate closure source");
    require(take(take(twice.field(UINT32_C(0)), "twice value")
                     .get_u32(),
                 "twice value bits") == UINT32_C(20));
    require(take(take(take(twice.field(UINT32_C(1)), "twice next")
                          .ref_target(),
                      "twice next target")
                     .graph_identity(),
                 "twice target identity")
                .object_id == UINT64_C(1));
    return EXIT_SUCCESS;
}

int test_selected_object_materializes_independent_closure(
    const std::vector<BinaryGoldenCase>& cases) {
    PayloadOwner owner = open_copy(find_case(cases,
                                             "graph-disconnected-roots"));
    View roots = take(owner.entry_view(UINT32_C(0)), "roots entry");
    View source_second = take(roots.at(UINT64_C(1)), "source second root");
    require(take(source_second.graph_identity(), "source identity")
                .object_id == UINT64_C(1));

    View detached = take(source_second.materialize(),
                         "materialize selected graph object");
    require(take(detached.kind(), "detached kind") == ViewKind::component);
    require(take(detached.graph_identity(), "detached identity").object_id ==
            UINT64_C(0));
    require(take(take(detached.field(UINT32_C(0)), "detached value")
                     .get_u32(),
                 "detached value bits") == UINT32_C(22));
    require(owner.invalidate().has_value());
    require(take(take(detached.field(UINT32_C(0)), "surviving value")
                     .get_u32(),
                 "surviving value bits") == UINT32_C(22));
    return EXIT_SUCCESS;
}

int test_all_values_survive_source_release(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-all-values");
    const std::uint32_t node = component_index(item, "Node");
    const std::uint32_t asset = component_index(item, "Asset");
    const std::uint32_t inline_component = component_index(item, "Inline");
    PayloadOwner owner = open_copy(item);
    View source = take(
        take(owner.entry_view(UINT32_C(0)), "all-values entry")
            .at(UINT64_C(0)),
        "all-values source root");
    View detached = take(source.materialize(), "materialize all values");
    View scalar_source = take(
        take(owner.entry_view(UINT32_C(3)), "scalar root entry")
            .at(UINT64_C(1)),
        "scalar source root");
    View detached_scalar =
        take(scalar_source.materialize(), "materialize scalar root");
    take(owner.invalidate(), "invalidate all-values source");

    require(take(detached_scalar.kind(), "detached scalar kind") ==
            ViewKind::u8n);
    require(take(detached_scalar.get_u8n_f64_bits(),
                 "detached scalar bits") ==
            UINT64_C(0x3fe0101010101010));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached_scalar, node) == UINT64_C(0));

    require(fastdb::payload::view::ViewTestAccess::detached_is_source_independent(
                detached));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached, node) == UINT64_C(1));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached, asset) == UINT64_C(1));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached, inline_component) == UINT64_C(0));
    require(take(detached.graph_identity(), "all-values identity")
                .object_id == UINT64_C(0));
    require(take(take(detached.field(UINT32_C(0)), "bool field").get_bool(),
                 "bool value") == UINT8_C(1));
    require(take(take(detached.field(UINT32_C(1)), "u8 field").get_u8(),
                 "u8 value") == UINT8_C(0x12));
    require(take(take(detached.field(UINT32_C(2)), "u16 field").get_u16(),
                 "u16 value") == UINT16_C(0x3456));
    require(take(take(detached.field(UINT32_C(3)), "u32 field").get_u32(),
                 "u32 value") == UINT32_C(0x789abcde));
    require(take(take(detached.field(UINT32_C(4)), "i32 field").get_i32(),
                 "i32 value") == INT32_C(-1234567));
    require(take(take(detached.field(UINT32_C(5)), "u8n field")
                     .get_u8n_f64_bits(),
                 "u8n bits") == UINT64_C(0x3fe0101010101010));
    require(take(take(detached.field(UINT32_C(6)), "u16n field")
                     .get_u16n_f64_bits(),
                 "u16n bits") == UINT64_C(0x3ef0001000100010));
    require(take(take(detached.field(UINT32_C(7)), "f32 field")
                     .get_f32_bits(),
                 "f32 bits") == UINT32_C(0x7fc00000));
    require(take(take(detached.field(UINT32_C(8)), "f64 field")
                     .get_f64_bits(),
                 "f64 bits") == UINT64_C(0x7ff8000000000000));

    auto text = take(
        take(detached.field(UINT32_C(9)), "text field").acquire(),
        "detached text access");
    const auto text_span = take(text.str(), "detached text span");
    require(text_span.size == UINT64_C(4));
    require(std::memcmp(text_span.data, "same", 4U) == 0);
    auto wide = take(
        take(detached.field(UINT32_C(10)), "wide field").acquire(),
        "detached wide access");
    const auto wide_span = take(wide.wstr(), "detached wide span");
    require(wide_span.size == UINT64_C(3));
    require(wide_span.data[0] == UINT16_C(0x0041));
    require(wide_span.data[1] == UINT16_C(0xd83d));
    require(wide_span.data[2] == UINT16_C(0xde00));
    auto opaque = take(
        take(detached.field(UINT32_C(11)), "bytes field").acquire(),
        "detached bytes access");
    const auto opaque_span = take(opaque.bytes(), "detached bytes span");
    const std::array<std::uint8_t, 3> expected_opaque{{
        UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x7e)}};
    require(opaque_span.size == expected_opaque.size());
    require(std::memcmp(opaque_span.data, expected_opaque.data(),
                        expected_opaque.size()) == 0);

    View inline_value =
        take(detached.field(UINT32_C(12)), "inline component field");
    require(take(inline_value.component_index(), "inline component index") ==
            inline_component);
    require(take(take(inline_value.field(UINT32_C(0)), "inline null")
                     .is_null(),
                 "inline null state"));
    require(take(take(inline_value.field(UINT32_C(1)), "inline code")
                     .get_u16(),
                 "inline code bits") == UINT16_C(0xbeef));

    View values = take(detached.field(UINT32_C(13)), "detached value list");
    require(take(values.length(), "detached value list length") ==
            UINT64_C(3));
    require(take(take(values.at(UINT64_C(0)), "negative zero")
                     .get_f32_bits(),
                 "negative-zero bits") == UINT32_C(0x80000000));
    require(take(take(values.at(UINT64_C(1)), "null list value").is_null(),
                 "null list state"));
    require(take(take(values.at(UINT64_C(2)), "canonical nan")
                     .get_f32_bits(),
                 "canonical nan bits") == UINT32_C(0x7fc00000));

    View self_ref = take(detached.field(UINT32_C(14)), "detached self ref");
    require(take(self_ref.kind(), "detached self kind") == ViewKind::ref);
    require(take(self_ref.graph_identity(), "detached self identity")
                .object_id == UINT64_C(0));
    require(take(take(self_ref.ref_target(), "detached self target")
                     .graph_identity(),
                 "detached self target identity")
                .object_id == UINT64_C(0));
    View asset_ref = take(detached.field(UINT32_C(15)), "detached asset ref");
    const auto asset_identity =
        take(asset_ref.graph_identity(), "detached asset identity");
    require(asset_identity.component_index == asset);
    require(asset_identity.object_id == UINT64_C(0));
    View asset_target = take(asset_ref.ref_target(), "detached asset target");
    require(take(take(asset_target.field(UINT32_C(0)), "asset name").acquire(),
                 "asset name access")
                .str()
                .value()
                .size == UINT64_C(4));
    const auto owner_identity = take(
        take(asset_target.field(UINT32_C(1)), "asset owner").graph_identity(),
        "asset owner identity");
    require(owner_identity.component_index == node);
    require(owner_identity.object_id == UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_null_empty_and_null_identity_roots(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-null-empty");
    const std::uint32_t node = component_index(item, "Node");
    PayloadOwner owner = open_copy(item);
    View roots = take(owner.entry_view(UINT32_C(0)), "null-empty roots");
    View detached = take(roots.materialize(), "materialize null-empty roots");
    require(take(detached.length(), "detached null-empty length") ==
            UINT64_C(2));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                detached, node) == UINT64_C(1));
    View object = take(detached.at(UINT64_C(0)), "detached nullable object");
    require(take(take(object.field(UINT32_C(0)), "detached null text")
                     .is_null(),
                 "detached null text state"));
    auto wide = take(take(object.field(UINT32_C(1)), "detached empty wide")
                         .acquire(),
                     "detached empty wide access");
    require(take(wide.wstr(), "detached empty wide span").size ==
            UINT64_C(0));
    auto bytes = take(take(object.field(UINT32_C(2)), "detached empty bytes")
                          .acquire(),
                      "detached empty bytes access");
    require(take(bytes.bytes(), "detached empty bytes span").size ==
            UINT64_C(0));
    require(take(take(object.field(UINT32_C(3)), "detached empty list")
                     .length(),
                 "detached empty list length") == UINT64_C(0));
    require(take(take(object.field(UINT32_C(4)), "detached null component")
                     .is_null(),
                 "detached null component state"));
    View null_ref = take(object.field(UINT32_C(5)), "detached null ref");
    require(take(null_ref.kind(), "detached null ref kind") == ViewKind::ref);
    require(take(null_ref.is_null(), "detached null ref state"));
    require(!null_ref.ref_target().has_value());

    View null_root = take(detached.at(UINT64_C(1)), "detached null root");
    require(take(null_root.kind(), "detached null root kind") ==
            ViewKind::component);
    require(take(null_root.is_null(), "detached null root state"));
    auto null_identity = null_root.graph_identity();
    require(!null_identity.has_value());
    require_error_code(null_identity.error(), FDB_PAYLOAD_E_UNEXPECTED_NULL);

    View source_null = take(roots.at(UINT64_C(1)), "source null root");
    View isolated_null =
        take(source_null.materialize(), "materialize isolated null root");
    require(take(isolated_null.kind(), "isolated null root kind") ==
            ViewKind::component);
    require(take(isolated_null.is_null(), "isolated null root state"));
    require(fastdb::payload::view::ViewTestAccess::object_count(
                isolated_null, node) == UINT64_C(0));
    take(owner.invalidate(), "invalidate null-empty source");
    require(take(isolated_null.is_null(), "surviving isolated null"));
    return EXIT_SUCCESS;
}

int test_materialization_holds_one_full_source_pin(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-all-values");
    ExternalBacking backing;
    PayloadOwner owner = open_external(item, backing);
    View root = take(
        take(owner.entry_view(UINT32_C(0)), "pin entry").at(UINT64_C(0)),
        "pin root");
    require(backing.retain_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));

    allocation_failure::prepare_block();
    std::optional<View> detached;
    std::atomic<std::uint32_t> materialize_status{UINT32_MAX};
    std::thread materializer([&] {
        allocation_failure::Reset reset;
        allocation_failure::block_after = INT64_C(0);
        auto result = root.materialize();
        allocation_failure::block_after = INT64_C(-1);
        if (result.has_value()) {
            detached.emplace(std::move(result).value());
            materialize_status.store(UINT32_C(0), std::memory_order_release);
        } else {
            materialize_status.store(result.error().code(),
                                     std::memory_order_release);
        }
    });
    const bool blocked = allocation_failure::wait_until_blocked();
    if (!blocked) {
        allocation_failure::release_block();
        materializer.join();
        throw std::runtime_error(
            "graph materialization did not reach allocation barrier");
    }
    const auto pinned =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(!pinned.invalidating);
    require(!pinned.invalidated);
    require(pinned.active_accesses == UINT64_C(1));

    std::atomic<std::uint32_t> invalidate_status{UINT32_MAX};
    std::thread invalidator([&] {
        auto result = owner.invalidate();
        invalidate_status.store(result.has_value() ? UINT32_C(0)
                                                   : result.error().code(),
                                std::memory_order_release);
    });
    const bool invalidating =
        fastdb::payload::view::PayloadOwnerTestAccess::
            wait_until_invalidating(owner);
    if (!invalidating) {
        allocation_failure::release_block();
        materializer.join();
        invalidator.join();
        throw std::runtime_error(
            "source invalidation did not wait for graph materialization");
    }
    const auto draining =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(draining.invalidating);
    require(!draining.invalidated);
    require(draining.active_accesses == UINT64_C(1));
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(0));
    auto rejected = root.kind();
    require(!rejected.has_value());
    require_error_code(rejected.error(), FDB_PAYLOAD_E_VIEW_INVALIDATED);

    allocation_failure::release_block();
    materializer.join();
    invalidator.join();
    require(materialize_status.load(std::memory_order_acquire) ==
            UINT32_C(0));
    require(invalidate_status.load(std::memory_order_acquire) ==
            UINT32_C(0));
    require(detached.has_value());
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    require(std::all_of(backing.storage.begin(), backing.storage.end(),
                        [](std::uint8_t byte) {
                            return byte == UINT8_C(0xa5);
                        }));
    const auto drained =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(!drained.invalidating);
    require(drained.invalidated);
    require(drained.active_accesses == UINT64_C(0));
    require(fastdb::payload::view::ViewTestAccess::detached_is_source_independent(
                *detached));
    require(take(take(detached->field(UINT32_C(3)), "post-release u32")
                     .get_u32(),
                 "post-release u32 bits") == UINT32_C(0x789abcde));
    return EXIT_SUCCESS;
}

int test_allocation_failure_is_transactional_and_balanced(
    const std::vector<BinaryGoldenCase>& cases) {
    PayloadOwner owner = open_copy(find_case(cases, "graph-all-values"));
    View source = take(
        take(owner.entry_view(UINT32_C(0)), "allocation entry")
            .at(UINT64_C(0)),
        "allocation source root");
    std::uint64_t observed_failures = UINT64_C(0);
    bool reached_success = false;
    for (std::int64_t allocation = INT64_C(0);
         allocation < INT64_C(4096); ++allocation) {
        allocation_failure::fail_after = allocation;
        auto result = source.materialize();
        allocation_failure::fail_after = INT64_C(-1);
        const auto facts =
            fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(
                owner);
        require(facts.active_accesses == UINT64_C(0));
        require(!facts.invalidating);
        require(!facts.invalidated);
        if (result.has_value()) {
            reached_success = true;
            require(take(take(result.value().field(UINT32_C(3)),
                              "allocation success value")
                             .get_u32(),
                         "allocation success bits") ==
                    UINT32_C(0x789abcde));
            break;
        }
        require_error_code(result.error(), FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++observed_failures;
        require(take(source.kind(), "allocation retry source kind") ==
                ViewKind::component);
        require(take(take(source.field(UINT32_C(3)),
                          "allocation retry source value")
                         .get_u32(),
                     "allocation retry source bits") ==
                UINT32_C(0x789abcde));
    }
    allocation_failure::fail_after = INT64_C(-1);
    require(reached_success);
    require(observed_failures >= UINT64_C(12));
    return EXIT_SUCCESS;
}

constexpr std::string_view deep_graph_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"roots","cardinality":"many","type":{"kind":"component","id":"Node"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"value","type":{"kind":"u32"}},
      {"id":"next","type":{"kind":"ref","target":"Node"}}
    ]}
  ]
})";

struct DeepGraphFixture final {
    PayloadOwner owner;
    std::uint32_t node;
};

DeepGraphFixture build_deep_graph(std::size_t object_count) {
    CompiledSpec compiled =
        take(CompiledSpec::compile(deep_graph_spec), "compile deep graph");
    const std::uint32_t node = component_index(compiled, "Node");
    PayloadBuilder builder = take(
        PayloadBuilder::create(std::move(compiled)), "create deep graph");
    std::vector<fastdb::payload::build::ObjectHandle> objects;
    objects.reserve(object_count);
    for (std::size_t index = 0U; index < object_count; ++index) {
        objects.push_back(
            take(builder.declare_object(node), "declare deep object"));
    }
    for (std::size_t index = 0U; index < object_count; ++index) {
        take(builder.begin_object_fill(objects[index]), "fill deep object");
        take(builder.push_u32(static_cast<std::uint32_t>(index)),
             "deep object value");
        take(builder.push_ref(objects[(index + 1U) % object_count]),
             "deep object next");
    }
    take(builder.begin_entry(UINT32_C(0),
                             static_cast<std::uint64_t>(object_count)),
         "deep roots entry");
    for (const auto object : objects) {
        take(builder.push_object(object), "deep root object");
    }
    auto plan = take(builder.freeze_plan(), "freeze deep graph");
    return DeepGraphFixture{
        take(plan.execute(FDB_PAYLOAD_BUILD_REQUIRE_DIRECT, nullptr),
             "execute deep graph"),
        node};
}

int test_deep_cycle_and_wide_root_are_iterative() {
    constexpr std::size_t object_count = 12000U;
    DeepGraphFixture fixture = build_deep_graph(object_count);
    View roots = take(fixture.owner.entry_view(UINT32_C(0)), "deep roots");

    View deep = take(take(roots.at(UINT64_C(0)), "deep first root")
                         .materialize(),
                     "materialize deep cycle");
    require(fastdb::payload::view::ViewTestAccess::object_count(
                deep, fixture.node) == object_count);
    require(take(deep.graph_identity(), "deep first identity").object_id ==
            UINT64_C(0));
    View first_next = take(deep.field(UINT32_C(1)), "deep first next");
    require(take(first_next.graph_identity(), "deep next identity")
                .object_id == UINT64_C(1));

    View wide = take(roots.materialize(), "materialize wide root sequence");
    require(take(wide.length(), "wide root length") == object_count);
    require(fastdb::payload::view::ViewTestAccess::object_count(
                wide, fixture.node) == object_count);
    View last = take(wide.at(object_count - 1U), "wide last root");
    require(take(last.graph_identity(), "wide last identity").object_id ==
            object_count - 1U);
    require(take(take(last.field(UINT32_C(0)), "wide last value").get_u32(),
                 "wide last value bits") == object_count - 1U);
    require(take(take(last.field(UINT32_C(1)), "wide cycle close")
                     .graph_identity(),
                 "wide cycle close identity")
                .object_id == UINT64_C(0));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    try {
        const auto cases =
            fastdb::test::payload::load_graph_binary_golden_corpus(
                FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
        require(cases.size() == 7U);
        if (test_selected_object_materializes_independent_closure(cases) !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_union_closure_dense_remap_and_root_kinds() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_all_values_survive_source_release(cases) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_null_empty_and_null_identity_roots(cases) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_materialization_holds_one_full_source_pin(cases) !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_allocation_failure_is_transactional_and_balanced(cases) !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        return test_deep_cycle_and_wide_root_are_iterative();
    } catch (const std::exception& error) {
        std::cerr << "P3 Task 7 graph-materialize fixture failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
