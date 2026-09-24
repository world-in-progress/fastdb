#include "GoldenCorpus.hpp"
#include "TestSupport.hpp"

#include "payload/backing/Backing.hpp"
#include "payload/build/PayloadBuilder.hpp"
#include "payload/error/Result.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/PayloadOwner.hpp"
#include "payload/view/View.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
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

struct Reset final {
    ~Reset() { fail_after = INT64_C(-1); }
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

struct GraphBarrierFacts final {
    std::uint64_t generation;
    std::uint64_t active_accesses;
    bool invalidating;
    bool invalidated;
};

struct PayloadOwnerTestAccess final {
    static GraphBarrierFacts barrier_facts(const PayloadOwner& owner) {
        std::lock_guard<std::mutex> lock(owner.state_->barrier.mutex);
        return GraphBarrierFacts{
            owner.state_->barrier.generation,
            owner.state_->barrier.active_accesses,
            owner.state_->barrier.invalidating,
            owner.state_->barrier.invalidated,
        };
    }

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

using fastdb::payload::error::Result;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::spec::CompiledSpec;
using fastdb::payload::view::Access;
using fastdb::payload::view::GraphIdentity;
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
        throw std::runtime_error("missing graph golden case");
    }
    return *found;
}

std::uint32_t component_index(const BinaryGoldenCase& item,
                              std::string_view id) {
    CompiledSpec compiled = take(
        CompiledSpec::compile(item.success.source), "compile component index");
    const auto index = compiled.component_index(id);
    if (!index.has_value()) {
        throw std::runtime_error("missing graph component");
    }
    return *index;
}

PayloadOwner open_copy(const BinaryGoldenCase& item) {
    std::vector<std::uint8_t> bytes = decode_hex(item.success.binary_hex);
    CompiledSpec compiled =
        take(CompiledSpec::compile(item.success.source), "compile graph");
    return take(PayloadOwner::open_copy(
                    std::move(compiled), bytes.data(), bytes.size()),
                "open graph copy");
}

void require_error(const fastdb::payload::error::Error& error,
                   std::uint32_t code,
                   std::string_view path,
                   std::string_view reason) {
    std::string details{"{\"reason\":\""};
    details.append(reason);
    details += "\"}";
    if (error.code() != code || error.path() != path ||
        error.details_json() != details) {
        throw std::runtime_error("unexpected graph view error contract");
    }
}

int test_all_value_navigation(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-all-values");
    const std::uint32_t node_index = component_index(item, "Node");
    const std::uint32_t inline_index = component_index(item, "Inline");
    const std::uint32_t asset_index = component_index(item, "Asset");
    PayloadOwner owner = open_copy(item);

    View root_sequence = take(owner.entry_view(UINT32_C(0)), "root entry");
    require(take(root_sequence.kind(), "root entry kind") ==
            ViewKind::sequence);
    require(take(root_sequence.length(), "root entry length") ==
            UINT64_C(1));
    auto sequence_identity = root_sequence.graph_identity();
    require(!sequence_identity.has_value());
    require_error(sequence_identity.error(), FDB_PAYLOAD_E_TYPE_MISMATCH,
                  "/entries/0", "view_kind_mismatch");

    View root = take(root_sequence.at(UINT64_C(0)), "root object");
    require(take(root.kind(), "root kind") == ViewKind::component);
    require(take(root.component_index(), "root component") == node_index);
    require(take(root.field_count(), "root fields") == UINT32_C(16));
    const GraphIdentity root_identity =
        take(root.graph_identity(), "root identity");
    require(root_identity.component_index == node_index);
    require(root_identity.object_id == UINT64_C(0));
    auto root_dereference = root.ref_target();
    require(!root_dereference.has_value());
    require_error(root_dereference.error(), FDB_PAYLOAD_E_TYPE_MISMATCH,
                  "/entries/0/0", "view_kind_mismatch");

    require(take(take(root.field(UINT32_C(0)), "bool field").get_bool(),
                 "bool value") == UINT8_C(1));
    require(take(take(root.field(UINT32_C(1)), "u8 field").get_u8(),
                 "u8 value") == UINT8_C(0x12));
    require(take(take(root.field(UINT32_C(2)), "u16 field").get_u16(),
                 "u16 value") == UINT16_C(0x3456));
    require(take(take(root.field(UINT32_C(3)), "u32 field").get_u32(),
                 "u32 value") == UINT32_C(0x789abcde));
    require(take(take(root.field(UINT32_C(4)), "i32 field").get_i32(),
                 "i32 value") == INT32_C(-1234567));
    require(take(take(root.field(UINT32_C(5)), "u8n field")
                     .get_u8n_f64_bits(),
                 "u8n bits") == UINT64_C(0x3fe0101010101010));
    require(take(take(root.field(UINT32_C(6)), "u16n field")
                     .get_u16n_f64_bits(),
                 "u16n bits") == UINT64_C(0x3ef0001000100010));
    require(take(take(root.field(UINT32_C(7)), "f32 field").get_f32_bits(),
                 "f32 bits") == UINT32_C(0x7fc00000));
    require(take(take(root.field(UINT32_C(8)), "f64 field").get_f64_bits(),
                 "f64 bits") == UINT64_C(0x7ff8000000000000));

    Access text = take(take(root.field(UINT32_C(9)), "str field").acquire(),
                       "str access");
    const auto text_span = take(text.str(), "str span");
    require(text_span.size == UINT64_C(4));
    require(std::memcmp(text_span.data, "same", 4U) == 0);
    Access wide =
        take(take(root.field(UINT32_C(10)), "wstr field").acquire(),
             "wstr access");
    const auto wide_span = take(wide.wstr(), "wstr span");
    require(wide_span.size == UINT64_C(3));
    require(wide_span.data[0] == UINT16_C(0x0041));
    require(wide_span.data[1] == UINT16_C(0xd83d));
    require(wide_span.data[2] == UINT16_C(0xde00));
    Access opaque =
        take(take(root.field(UINT32_C(11)), "bytes field").acquire(),
             "bytes access");
    const auto opaque_span = take(opaque.bytes(), "bytes span");
    const std::array<std::uint8_t, 3> expected_opaque{{
        UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x7e)}};
    require(opaque_span.size == expected_opaque.size());
    require(std::memcmp(opaque_span.data, expected_opaque.data(),
                        expected_opaque.size()) == 0);

    View inline_component =
        take(root.field(UINT32_C(12)), "inline component");
    require(take(inline_component.kind(), "inline kind") ==
            ViewKind::component);
    require(take(inline_component.component_index(), "inline index") ==
            inline_index);
    require(take(inline_component.field_count(), "inline fields") ==
            UINT32_C(2));
    auto inline_identity = inline_component.graph_identity();
    require(!inline_identity.has_value());
    require_error(inline_identity.error(), FDB_PAYLOAD_E_TYPE_MISMATCH,
                  "/entries/0/0/fields/12", "view_kind_mismatch");
    View inline_null =
        take(inline_component.field(UINT32_C(0)), "inline null");
    require(take(inline_null.is_null(), "inline null state"));
    require(take(take(inline_component.field(UINT32_C(1)), "inline code")
                     .get_u16(),
                 "inline code value") == UINT16_C(0xbeef));

    View values = take(root.field(UINT32_C(13)), "value list");
    require(take(values.kind(), "value list kind") == ViewKind::list);
    require(take(values.length(), "value list length") == UINT64_C(3));
    require(take(take(values.at(UINT64_C(0)), "first f32").get_f32_bits(),
                 "first f32 bits") == UINT32_C(0x80000000));
    require(take(take(values.at(UINT64_C(1)), "null f32").is_null(),
                 "null f32 state"));
    require(take(take(values.at(UINT64_C(2)), "last f32").get_f32_bits(),
                 "last f32 bits") == UINT32_C(0x7fc00000));

    View self_ref = take(root.field(UINT32_C(14)), "self ref");
    require(take(self_ref.kind(), "self ref kind") == ViewKind::ref);
    auto implicit = self_ref.field(UINT32_C(0));
    require(!implicit.has_value());
    require_error(implicit.error(), FDB_PAYLOAD_E_TYPE_MISMATCH,
                  "/entries/0/0/fields/14", "view_kind_mismatch");
    const GraphIdentity self_identity =
        take(self_ref.graph_identity(), "self identity");
    require(self_identity.component_index == node_index);
    require(self_identity.object_id == UINT64_C(0));
    View self_target = take(self_ref.ref_target(), "self target");
    require(take(self_target.graph_identity(), "self target identity")
                .object_id == UINT64_C(0));
    require(take(take(self_target.field(UINT32_C(3)), "self u32")
                     .get_u32(),
                 "self u32 value") == UINT32_C(0x789abcde));

    View asset_ref = take(root.field(UINT32_C(15)), "asset ref");
    const GraphIdentity asset_identity =
        take(asset_ref.graph_identity(), "asset identity");
    require(asset_identity.component_index == asset_index);
    require(asset_identity.object_id == UINT64_C(0));
    View asset = take(asset_ref.ref_target(), "asset target");
    require(take(asset.component_index(), "asset component") == asset_index);
    require(take(asset.field_count(), "asset fields") == UINT32_C(2));
    require(take(take(asset.field(UINT32_C(1)), "asset owner")
                     .graph_identity(),
                 "asset owner identity")
                .component_index == node_index);

    View nullable_refs = take(owner.entry_view(UINT32_C(2)), "refs entry");
    require(take(nullable_refs.length(), "refs length") == UINT64_C(2));
    View null_ref = take(nullable_refs.at(UINT64_C(1)), "null ref");
    require(take(null_ref.kind(), "null ref kind") == ViewKind::ref);
    require(take(null_ref.is_null(), "null ref state"));
    auto null_target = null_ref.ref_target();
    require(!null_target.has_value());
    require_error(null_target.error(), FDB_PAYLOAD_E_UNEXPECTED_NULL,
                  "/entries/2/1", "unexpected_null");
    auto null_identity = null_ref.graph_identity();
    require(!null_identity.has_value());
    require_error(null_identity.error(), FDB_PAYLOAD_E_UNEXPECTED_NULL,
                  "/entries/2/1", "unexpected_null");

    View u8n_values = take(owner.entry_view(UINT32_C(3)), "u8n entry");
    require(take(take(u8n_values.at(UINT64_C(0)), "u8n zero")
                     .get_u8n_f64_bits(),
                 "u8n zero bits") == UINT64_C(0));
    require(take(take(u8n_values.at(UINT64_C(2)), "u8n one")
                     .get_u8n_f64_bits(),
                 "u8n one bits") == UINT64_C(0x3ff0000000000000));
    View u16n_values = take(owner.entry_view(UINT32_C(4)), "u16n entry");
    require(take(take(u16n_values.at(UINT64_C(0)), "u16n minus one")
                     .get_u16n_f64_bits(),
                 "u16n minus one bits") == UINT64_C(0xbff0000000000000));
    require(take(take(u16n_values.at(UINT64_C(2)), "u16n one")
                     .get_u16n_f64_bits(),
                 "u16n one bits") == UINT64_C(0x3ff0000000000000));

    View detached = take(root.materialize(), "materialize graph root");
    require(take(detached.graph_identity(), "detached root identity")
                .object_id == UINT64_C(0));
    require(take(take(detached.field(UINT32_C(3)), "detached u32")
                     .get_u32(),
                 "detached u32 value") == UINT32_C(0x789abcde));
    return EXIT_SUCCESS;
}

int test_cycles_sharing_and_payload_scope(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-shared-cycle");
    const std::uint32_t node_index = component_index(item, "Node");
    const std::uint32_t pair_index = component_index(item, "Pair");
    PayloadOwner first_owner = open_copy(item);
    PayloadOwner second_owner = open_copy(item);

    View first = take(take(first_owner.entry_view(UINT32_C(0)), "first entry")
                          .at(UINT64_C(0)),
                      "first object");
    View second =
        take(take(first_owner.entry_view(UINT32_C(1)), "second entry")
                 .at(UINT64_C(0)),
             "second object");
    require(take(first.graph_identity(), "first identity").object_id ==
            UINT64_C(0));
    require(take(second.graph_identity(), "second identity").object_id ==
            UINT64_C(1));
    require(take(first.component_index(), "first component") == node_index);
    require(take(take(first.field(UINT32_C(0)), "first value").get_u32(),
                 "first value bits") == UINT32_C(100));
    require(take(take(second.field(UINT32_C(0)), "second value").get_u32(),
                 "second value bits") == UINT32_C(200));

    View first_self = take(first.field(UINT32_C(1)), "first self");
    View first_shared = take(first.field(UINT32_C(2)), "first shared");
    View second_self = take(second.field(UINT32_C(1)), "second self");
    View second_shared = take(second.field(UINT32_C(2)), "second shared");
    require(take(first_self.graph_identity(), "first self identity")
                .object_id == UINT64_C(0));
    require(take(first_shared.graph_identity(), "first shared identity")
                .object_id == UINT64_C(1));
    require(take(second_self.graph_identity(), "second self identity")
                .object_id == UINT64_C(1));
    require(take(second_shared.graph_identity(), "second shared identity")
                .object_id == UINT64_C(1));
    require(take(take(first_shared.ref_target(), "shared target")
                     .field(UINT32_C(0)),
                 "shared target value")
                .get_u32()
                .value() == UINT32_C(200));

    View pair = take(first.field(UINT32_C(3)), "inline pair");
    require(take(pair.component_index(), "pair component") == pair_index);
    require(take(take(pair.field(UINT32_C(0)), "pair flag").get_bool(),
                 "pair flag value") == UINT8_C(1));
    require(take(take(pair.field(UINT32_C(1)), "pair code").get_u16(),
                 "pair code value") == UINT16_C(0x1234));
    auto pair_identity = pair.graph_identity();
    require(!pair_identity.has_value());
    require(pair_identity.error().code() == FDB_PAYLOAD_E_TYPE_MISMATCH);

    View other_first =
        take(take(second_owner.entry_view(UINT32_C(0)), "other entry")
                 .at(UINT64_C(0)),
             "other first object");
    const GraphIdentity local =
        take(first.graph_identity(), "local identity");
    const GraphIdentity other =
        take(other_first.graph_identity(), "other identity");
    require(local.component_index == other.component_index);
    require(local.object_id == other.object_id);
    require(first_owner.invalidate().has_value());
    require(!first.graph_identity().has_value());
    require(take(other_first.graph_identity(), "independent identity")
                .object_id == UINT64_C(0));
    require(take(take(other_first.field(UINT32_C(0)), "independent value")
                     .get_u32(),
                 "independent value bits") == UINT32_C(100));
    return EXIT_SUCCESS;
}

int test_nested_null_and_empty_values(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& nested = find_case(cases, "graph-nested-lists");
    PayloadOwner nested_owner = open_copy(nested);
    View root = take(take(nested_owner.entry_view(UINT32_C(0)), "nested root")
                         .at(UINT64_C(0)),
                     "nested object");
    View matrix = take(root.field(UINT32_C(0)), "object matrix");
    require(take(matrix.length(), "matrix length") == UINT64_C(3));
    View first_row = take(matrix.at(UINT64_C(0)), "first row");
    require(take(first_row.length(), "first row length") == UINT64_C(2));
    require(take(take(first_row.at(UINT64_C(0)), "matrix one").get_u16(),
                 "matrix one value") == UINT16_C(1));
    require(take(take(first_row.at(UINT64_C(1)), "matrix null").is_null(),
                 "matrix null state"));
    View null_row = take(matrix.at(UINT64_C(1)), "null row");
    require(take(null_row.is_null(), "null row state"));
    auto null_length = null_row.length();
    require(!null_length.has_value());
    require(null_length.error().code() == FDB_PAYLOAD_E_UNEXPECTED_NULL);
    require(take(take(matrix.at(UINT64_C(2)), "empty row").length(),
                 "empty row length") == UINT64_C(0));

    View box = take(root.field(UINT32_C(1)), "inline box");
    View boxed_values = take(box.field(UINT32_C(0)), "boxed values");
    require(take(boxed_values.length(), "boxed outer length") ==
            UINT64_C(2));
    require(take(take(take(boxed_values.at(UINT64_C(0)), "boxed row")
                          .at(UINT64_C(0)),
                      "boxed value")
                     .get_u8(),
                 "boxed value bits") == UINT8_C(7));

    View matrices =
        take(nested_owner.entry_view(UINT32_C(1)), "matrices entry");
    require(take(matrices.length(), "matrices entry length") ==
            UINT64_C(3));
    require(take(take(matrices.at(UINT64_C(2)), "empty matrix")
                     .length(),
                 "empty matrix length") == UINT64_C(0));

    const BinaryGoldenCase& null_empty =
        find_case(cases, "graph-null-empty");
    PayloadOwner null_owner = open_copy(null_empty);
    View roots = take(null_owner.entry_view(UINT32_C(0)), "nullable roots");
    require(take(roots.length(), "nullable roots length") == UINT64_C(2));
    View object = take(roots.at(UINT64_C(0)), "nullable object");
    require(take(take(object.field(UINT32_C(0)), "null text").is_null(),
                 "null text state"));
    Access empty_wide =
        take(take(object.field(UINT32_C(1)), "empty wide").acquire(),
             "empty wide access");
    require(take(empty_wide.wstr(), "empty wide span").size == UINT64_C(0));
    Access empty_bytes =
        take(take(object.field(UINT32_C(2)), "empty bytes").acquire(),
             "empty bytes access");
    require(take(empty_bytes.bytes(), "empty bytes span").size ==
            UINT64_C(0));
    require(take(take(object.field(UINT32_C(3)), "empty items").length(),
                 "empty items length") == UINT64_C(0));
    require(take(take(object.field(UINT32_C(4)), "null component").is_null(),
                 "null component state"));
    View null_next = take(object.field(UINT32_C(5)), "null next");
    require(take(null_next.is_null(), "null next state"));
    require(!null_next.ref_target().has_value());
    View null_root = take(roots.at(UINT64_C(1)), "null identity root");
    require(take(null_root.kind(), "null root kind") == ViewKind::component);
    require(take(null_root.is_null(), "null root state"));
    auto null_root_identity = null_root.graph_identity();
    require(!null_root_identity.has_value());
    require_error(null_root_identity.error(), FDB_PAYLOAD_E_UNEXPECTED_NULL,
                  "/entries/0/1", "unexpected_null");
    return EXIT_SUCCESS;
}

constexpr std::string_view entry_list_spec = R"({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"roots","cardinality":"one","type":{"kind":"list","items":{"kind":"list","items":{"kind":"component","id":"Node","nullable":true}}}},
    {"id":"refs","cardinality":"one","type":{"kind":"list","items":{"kind":"list","items":{"kind":"ref","target":"Node","nullable":true}}}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"value","type":{"kind":"u32"}},
      {"id":"next","type":{"kind":"ref","target":"Node","nullable":true}}
    ]}
  ]
})";

PayloadOwner build_entry_list_owner() {
    CompiledSpec compiled =
        take(CompiledSpec::compile(entry_list_spec), "compile entry lists");
    const auto node = compiled.component_index("Node");
    if (!node.has_value()) {
        throw std::runtime_error("missing entry-list Node component");
    }
    PayloadBuilder builder = take(
        PayloadBuilder::create(std::move(compiled)), "create entry lists");
    const auto first = take(builder.declare_object(*node), "declare first");
    const auto second =
        take(builder.declare_object(*node), "declare second");

    take(builder.begin_object_fill(first), "fill first");
    take(builder.push_u32(UINT32_C(11)), "first value");
    take(builder.push_ref(second), "first next");
    take(builder.begin_object_fill(second), "fill second");
    take(builder.push_u32(UINT32_C(22)), "second value");
    take(builder.push_ref(first), "second next");

    take(builder.begin_entry(UINT32_C(0), UINT64_C(1)), "roots entry");
    take(builder.begin_list(UINT64_C(2)), "roots outer");
    take(builder.begin_list(UINT64_C(2)), "roots first inner");
    take(builder.push_object(first), "roots first");
    take(builder.push_null(), "roots null");
    take(builder.begin_list(UINT64_C(1)), "roots second inner");
    take(builder.push_object(second), "roots second");

    take(builder.begin_entry(UINT32_C(1), UINT64_C(1)), "refs entry");
    take(builder.begin_list(UINT64_C(2)), "refs outer");
    take(builder.begin_list(UINT64_C(2)), "refs first inner");
    take(builder.push_ref(second), "refs second");
    take(builder.push_null(), "refs null");
    take(builder.begin_list(UINT64_C(1)), "refs second inner");
    take(builder.push_ref(first), "refs first");

    auto plan = take(builder.freeze_plan(), "freeze entry lists");
    return take(plan.execute(UINT32_C(2), nullptr), "execute entry lists");
}

int test_nested_root_and_ref_lists() {
    PayloadOwner owner = build_entry_list_owner();
    View roots = take(take(owner.entry_view(UINT32_C(0)), "root-list entry")
                          .at(UINT64_C(0)),
                      "root-list outer");
    require(take(roots.kind(), "root-list outer kind") == ViewKind::list);
    require(take(roots.length(), "root-list outer length") == UINT64_C(2));
    View first_inner = take(roots.at(UINT64_C(0)), "root-list first inner");
    View first = take(first_inner.at(UINT64_C(0)), "root-list first object");
    require(take(first.kind(), "root-list first kind") ==
            ViewKind::component);
    require(take(first.graph_identity(), "root-list first identity")
                .object_id == UINT64_C(0));
    require(take(take(first.field(UINT32_C(0)), "root-list first value")
                     .get_u32(),
                 "root-list first bits") == UINT32_C(11));
    View null_root =
        take(first_inner.at(UINT64_C(1)), "root-list null object");
    require(take(null_root.kind(), "root-list null kind") ==
            ViewKind::component);
    require(take(null_root.is_null(), "root-list null state"));
    require(!null_root.graph_identity().has_value());
    View second_inner =
        take(roots.at(UINT64_C(1)), "root-list second inner");
    View second = take(second_inner.at(UINT64_C(0)), "root-list second");
    require(take(second.graph_identity(), "root-list second identity")
                .object_id == UINT64_C(1));
    View first_next =
        take(first.field(UINT32_C(1)), "root-list first next");
    require(take(first_next.graph_identity(), "first next identity")
                .object_id == UINT64_C(1));
    View second_next =
        take(take(first_next.ref_target(), "first next target")
                 .field(UINT32_C(1)),
             "second next");
    require(take(second_next.graph_identity(), "second next identity")
                .object_id == UINT64_C(0));

    View refs = take(take(owner.entry_view(UINT32_C(1)), "ref-list entry")
                         .at(UINT64_C(0)),
                     "ref-list outer");
    View ref_inner = take(refs.at(UINT64_C(0)), "ref-list first inner");
    View second_ref = take(ref_inner.at(UINT64_C(0)), "ref-list second ref");
    require(take(second_ref.kind(), "ref-list ref kind") == ViewKind::ref);
    require(take(second_ref.graph_identity(), "ref-list ref identity")
                .object_id == UINT64_C(1));
    require(take(take(second_ref.ref_target(), "ref-list target")
                     .field(UINT32_C(0)),
                 "ref-list target value")
                .get_u32()
                .value() == UINT32_C(22));
    View null_ref = take(ref_inner.at(UINT64_C(1)), "ref-list null ref");
    require(take(null_ref.is_null(), "ref-list null state"));
    require(!null_ref.ref_target().has_value());
    View first_ref =
        take(take(refs.at(UINT64_C(1)), "ref-list second inner")
                 .at(UINT64_C(0)),
             "ref-list first ref");
    require(take(first_ref.graph_identity(), "ref-list first identity")
                .object_id == UINT64_C(0));
    return EXIT_SUCCESS;
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
        "retain graph backing");
    CompiledSpec compiled =
        take(CompiledSpec::compile(item.success.source), "compile external");
    return take(PayloadOwner::open_external(
                    std::move(compiled), backing.storage.data(),
                    backing.storage.size(), std::move(retained)),
                "open external graph");
}

int test_child_retains_owner(const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-single-root");
    ExternalBacking backing;
    std::optional<View> child;
    {
        PayloadOwner owner = open_external(item, backing);
        child.emplace(
            take(take(owner.entry_view(UINT32_C(0)), "retained entry")
                     .at(UINT64_C(0)),
                 "retained child"));
    }
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(0));
    require(take(child->graph_identity(), "retained child identity")
                .object_id == UINT64_C(0));
    require(take(take(child->field(UINT32_C(0)), "retained child field")
                     .get_u16(),
                 "retained child value") == UINT16_C(0x1234));
    child.reset();
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    return EXIT_SUCCESS;
}

int test_lifetime_invalidation_and_concurrency(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-shared-cycle");
    ExternalBacking backing;
    PayloadOwner owner = open_external(item, backing);
    require(backing.retain_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    View root = take(take(owner.entry_view(UINT32_C(0)), "lifetime entry")
                         .at(UINT64_C(0)),
                     "lifetime root");
    View reference = take(root.field(UINT32_C(2)), "lifetime ref");

    constexpr std::uint32_t thread_count = UINT32_C(32);
    constexpr std::uint32_t iterations = UINT32_C(1000);
    std::atomic<std::uint32_t> failures{UINT32_C(0)};
    std::vector<std::thread> readers;
    readers.reserve(thread_count);
    for (std::uint32_t thread = UINT32_C(0); thread < thread_count;
         ++thread) {
        readers.emplace_back([root, reference, &failures] {
            for (std::uint32_t iteration = UINT32_C(0);
                 iteration < iterations; ++iteration) {
                auto target = reference.ref_target();
                auto identity = root.graph_identity();
                if (!target.has_value() || !identity.has_value() ||
                    identity.value().object_id != UINT64_C(0) ||
                    !target.value().field(UINT32_C(0)).has_value()) {
                    failures.fetch_add(UINT32_C(1),
                                       std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    require(failures.load(std::memory_order_relaxed) == UINT32_C(0));
    require(owner.invalidate().has_value());
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    return EXIT_SUCCESS;
}

int test_generation_and_allocation_failures(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-shared-cycle");
    PayloadOwner owner = open_copy(item);
    View root = take(take(owner.entry_view(UINT32_C(0)), "generation entry")
                         .at(UINT64_C(0)),
                     "generation root");
    View reference = take(root.field(UINT32_C(2)), "generation ref");
    fastdb::payload::view::PayloadOwnerTestAccess::set_generation(
        owner, UINT64_C(2));
    auto stale_root = root.graph_identity();
    require(!stale_root.has_value());
    require_error(stale_root.error(), FDB_PAYLOAD_E_STALE_GENERATION,
                  "/view", "stale_generation");
    auto stale_ref = reference.ref_target();
    require(!stale_ref.has_value());
    require_error(stale_ref.error(), FDB_PAYLOAD_E_STALE_GENERATION,
                  "/view", "stale_generation");

    View current =
        take(take(owner.entry_view(UINT32_C(0)), "current entry")
                 .at(UINT64_C(0)),
             "current root");
    View current_ref = take(current.field(UINT32_C(2)), "current ref");
    allocation_failure::Reset reset;
    bool saw_failure = false;
    bool saw_success = false;
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(256);
         ++allocation) {
        allocation_failure::fail_after = allocation;
        auto target = current_ref.ref_target();
        allocation_failure::fail_after = INT64_C(-1);
        if (target.has_value()) {
            saw_success = true;
            require(take(target.value().graph_identity(),
                         "allocated target identity")
                        .object_id == UINT64_C(1));
            break;
        }
        saw_failure = true;
        require(target.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(take(current_ref.graph_identity(), "retry identity")
                    .object_id == UINT64_C(1));
    }
    require(saw_failure);
    require(saw_success);

    bool saw_field_failure = false;
    bool saw_field_success = false;
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(256);
         ++allocation) {
        allocation_failure::fail_after = allocation;
        auto field = current.field(UINT32_C(0));
        allocation_failure::fail_after = INT64_C(-1);
        if (field.has_value()) {
            saw_field_success = true;
            require(take(field.value().get_u32(), "allocated graph field") ==
                    UINT32_C(100));
            break;
        }
        saw_field_failure = true;
        require(field.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(take(current.graph_identity(), "field retry identity")
                    .object_id == UINT64_C(0));
    }
    require(saw_field_failure);
    require(saw_field_success);

    const BinaryGoldenCase& values = find_case(cases, "graph-all-values");
    PayloadOwner values_owner = open_copy(values);
    View values_root =
        take(take(values_owner.entry_view(UINT32_C(0)), "access entry")
                 .at(UINT64_C(0)),
             "access root");
    View text = take(values_root.field(UINT32_C(9)), "access text");
    bool saw_access_failure = false;
    bool saw_access_success = false;
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(256);
         ++allocation) {
        allocation_failure::fail_after = allocation;
        auto access = text.acquire();
        allocation_failure::fail_after = INT64_C(-1);
        if (access.has_value()) {
            saw_access_success = true;
            require(take(access.value().str(), "allocated graph access")
                        .size == UINT64_C(4));
            break;
        }
        saw_access_failure = true;
        require(access.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        require(take(text.kind(), "access retry kind") == ViewKind::str);
    }
    require(saw_access_failure);
    require(saw_access_success);
    return EXIT_SUCCESS;
}

int test_external_invalidation_drain(
    const std::vector<BinaryGoldenCase>& cases) {
    const BinaryGoldenCase& item = find_case(cases, "graph-all-values");
    ExternalBacking backing;
    PayloadOwner owner = open_external(item, backing);
    View root = take(take(owner.entry_view(UINT32_C(0)), "drain entry")
                         .at(UINT64_C(0)),
                     "drain root");
    View reference = take(root.field(UINT32_C(14)), "drain ref");
    std::optional<Access> active;
    active.emplace(take(take(root.field(UINT32_C(9)), "drain text")
                            .acquire(),
                        "drain access"));
    require(fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner)
                .active_accesses == UINT64_C(1));

    std::atomic<std::uint32_t> status{UINT32_MAX};
    std::thread invalidator([&] {
        auto invalidated = owner.invalidate();
        status.store(invalidated.has_value() ? UINT32_C(0)
                                             : invalidated.error().code(),
                     std::memory_order_release);
    });
    fastdb::payload::view::PayloadOwnerTestAccess::wait_until_invalidating(
        owner);
    const auto invalidating =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(invalidating.invalidating);
    require(!invalidating.invalidated);
    require(invalidating.generation == UINT64_C(1));
    require(invalidating.active_accesses == UINT64_C(1));
    require(status.load(std::memory_order_acquire) == UINT32_MAX);
    auto rejected_identity = root.graph_identity();
    require(!rejected_identity.has_value());
    require_error(rejected_identity.error(), FDB_PAYLOAD_E_VIEW_INVALIDATED,
                  "/view", "view_invalidated");
    auto rejected_target = reference.ref_target();
    require(!rejected_target.has_value());
    require_error(rejected_target.error(), FDB_PAYLOAD_E_VIEW_INVALIDATED,
                  "/view", "view_invalidated");
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(0));

    active.reset();
    invalidator.join();
    require(status.load(std::memory_order_acquire) == UINT32_C(0));
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    const auto invalidated =
        fastdb::payload::view::PayloadOwnerTestAccess::barrier_facts(owner);
    require(!invalidated.invalidating);
    require(invalidated.invalidated);
    require(invalidated.generation == UINT64_C(2));
    require(invalidated.active_accesses == UINT64_C(0));
    require(!root.field(UINT32_C(0)).has_value());
    require(owner.invalidate().has_value());
    require(backing.release_count.load(std::memory_order_relaxed) ==
            UINT32_C(1));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    try {
        const auto cases =
            fastdb::test::payload::load_graph_binary_golden_corpus(
                FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
        require(cases.size() == 7U);
        if (test_all_value_navigation(cases) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_cycles_sharing_and_payload_scope(cases) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_nested_null_and_empty_values(cases) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_nested_root_and_ref_lists() != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_child_retains_owner(cases) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_lifetime_invalidation_and_concurrency(cases) !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        if (test_generation_and_allocation_failures(cases) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
        return test_external_invalidation_drain(cases);
    } catch (const std::exception& error) {
        std::cerr << "P3 Task 6 graph-view fixture failed: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
