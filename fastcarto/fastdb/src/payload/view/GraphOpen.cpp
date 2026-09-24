#include "payload/view/GraphOpen.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/InputSpan.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/layout/TextEncoding.hpp"
#include "payload/view/OpenCommon.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::payload::view {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonPointerBuilder;
using json::JsonValue;
using layout::ComponentFieldLayout;
using layout::ComponentLayout;
using layout::EntryDescriptor;
using layout::RuntimeSchema;
using layout::RuntimeType;
using open_common::RegionFields;
using open_common::WorkCounter;
using open_common::allocation_error;
using open_common::binary_entry_path;
using open_common::binary_header_path;
using open_common::binary_region_path;
using open_common::byte_hex;
using open_common::noncanonical;
using open_common::noncanonical_exact;
using open_common::read_region_fields;
using open_common::read_u32;
using open_common::read_u64;
using open_common::require_zero;
using open_common::resource_error;
using open_common::simple_error;
using open_common::validate_region_fields;
using spec::Cardinality;
using spec::Profile;
using spec::StorageRole;
using spec::TypeKind;

struct HeaderFacts final {
    std::uint32_t region_count;
    std::uint64_t entry_directory_offset;
    std::uint32_t entry_count;
    std::uint64_t root_value_count;
    std::uint64_t entry_end;
};

struct EntryState final {
    EntryDescriptor descriptor;
    RegionFields validity;
    RegionFields values;
    bool has_validity;
};

struct ObjectPoolState final {
    std::uint32_t component_index;
    RegionFields values;
};

bool variable_kind(TypeKind kind) noexcept {
    return kind == TypeKind::str || kind == TypeKind::wstr ||
           kind == TypeKind::bytes;
}

bool structural_container(const RuntimeType& type) noexcept {
    return type.source != nullptr &&
           (type.source->kind == TypeKind::list ||
            type.storage_role == StorageRole::inline_component);
}

const PoolMetadata* find_pool(const std::vector<PoolMetadata>& pools,
                              TypeKind kind) noexcept {
    const layout::RegionKind wanted =
        kind == TypeKind::str    ? layout::RegionKind::utf8_pool
        : kind == TypeKind::wstr ? layout::RegionKind::utf16_pool
                                 : layout::RegionKind::bytes_pool;
    const auto found = std::find_if(
        pools.begin(), pools.end(),
        [wanted](const PoolMetadata& pool) { return pool.kind == wanted; });
    return found == pools.end() ? nullptr : &*found;
}

struct PoolCursors final {
    std::uint64_t utf8{UINT64_C(0)};
    std::uint64_t utf16{UINT64_C(0)};
    std::uint64_t opaque{UINT64_C(0)};

    std::uint64_t& for_kind(TypeKind kind) noexcept {
        if (kind == TypeKind::str) {
            return utf8;
        }
        if (kind == TypeKind::wstr) {
            return utf16;
        }
        return opaque;
    }
};

struct ListRegionState final {
    std::uint32_t owner_runtime_type_id;
    std::uint32_t item_runtime_type_id;
    std::uint32_t validity_region_index;
    std::uint32_t items_region_index;
    RegionFields validity;
    RegionFields items;
    std::uint64_t cursor{UINT64_C(0)};
};

struct ListPartitions final {
    std::vector<ListRegionState> states;
    std::vector<std::uint32_t> indexes;

    ListRegionState* find(std::uint32_t runtime_type_id) noexcept {
        if (runtime_type_id >= indexes.size()) {
            return nullptr;
        }
        const std::uint32_t index = indexes[runtime_type_id];
        return index < states.size() ? &states[index] : nullptr;
    }
};

bool graph_container_kind(const RuntimeSchema& runtime,
                          const spec::TypeNode& type) noexcept {
    const std::uint32_t type_id = runtime.runtime_id(type);
    const RuntimeType* const runtime_type = runtime.find_type(type_id);
    return runtime_type != nullptr && structural_container(*runtime_type);
}

bool noncanonical_f32_nan(std::uint32_t bits) noexcept {
    const bool nan = (bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) &&
                     (bits & UINT32_C(0x007fffff)) != UINT32_C(0);
    return nan && bits != UINT32_C(0x7fc00000);
}

bool noncanonical_f64_nan(std::uint64_t bits) noexcept {
    const bool nan =
        (bits & UINT64_C(0x7ff0000000000000)) ==
            UINT64_C(0x7ff0000000000000) &&
        (bits & UINT64_C(0x000fffffffffffff)) != UINT64_C(0);
    return nan && bits != UINT64_C(0x7ff8000000000000);
}

Result<HeaderFacts> validate_header(
    const spec::CompiledSpec& compiled,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t expected_regions,
    OpenOptions limits,
    WorkCounter& work) {
    const JsonPointer path = binary_header_path();
    if (byte_count > limits.max_total_bytes) {
        return Result<HeaderFacts>::failure(resource_error(
            path, "total_bytes", byte_count, limits.max_total_bytes));
    }
    if (bytes == nullptr || !layout::input_span_is_addressable(byte_count)) {
        return Result<HeaderFacts>::failure(simple_error(
            FDB_PAYLOAD_E_OUT_OF_BOUNDS, path,
            "Portable payload byte span is not addressable",
            "binary_span_out_of_bounds"));
    }
    if (byte_count < layout::header_size) {
        return Result<HeaderFacts>::failure(simple_error(
            FDB_PAYLOAD_E_OUT_OF_BOUNDS, path,
            "Portable payload is shorter than the fixed header",
            "header_out_of_bounds"));
    }
    auto charged = work.charge(UINT64_C(1), path);
    if (!charged.has_value()) {
        return Result<HeaderFacts>::failure(std::move(charged).error());
    }
    if (!std::equal(layout::binary_magic.begin(),
                    layout::binary_magic.end(), bytes)) {
        return Result<HeaderFacts>::failure(Error::from_details(
            FDB_PAYLOAD_E_INVALID_MAGIC, path.append("magic"),
            "Portable payload magic is invalid",
            JsonValue::object({
                JsonValue::Member{
                    "actual", JsonValue{byte_hex(
                                  bytes, layout::binary_magic.size())}},
                JsonValue::Member{
                    "expected", JsonValue{byte_hex(
                                    layout::binary_magic.data(),
                                    layout::binary_magic.size())}},
                JsonValue::Member{"reason", JsonValue{"invalid_magic"}},
            })));
    }
    auto major = layout::load_u16_le(
        bytes, byte_count, layout::header_major_offset,
        path.append("major"));
    auto minor = layout::load_u16_le(
        bytes, byte_count, layout::header_minor_offset,
        path.append("minor"));
    if (!major.has_value()) {
        return Result<HeaderFacts>::failure(std::move(major).error());
    }
    if (!minor.has_value()) {
        return Result<HeaderFacts>::failure(std::move(minor).error());
    }
    if (major.value() != layout::binary_major) {
        return Result<HeaderFacts>::failure(Error::from_details(
            FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION,
            path.append("major"),
            "Portable payload binary version is unsupported",
            JsonValue::object({
                JsonValue::Member{
                    "actual", JsonValue{std::to_string(major.value())}},
                JsonValue::Member{
                    "expected",
                    JsonValue{std::to_string(layout::binary_major)}},
                JsonValue::Member{
                    "reason", JsonValue{"unsupported_binary_major"}},
            })));
    }
    if (minor.value() != layout::binary_minor) {
        return Result<HeaderFacts>::failure(Error::from_details(
            FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION,
            path.append("minor"),
            "Portable payload binary version is unsupported",
            JsonValue::object({
                JsonValue::Member{
                    "actual", JsonValue{std::to_string(minor.value())}},
                JsonValue::Member{
                    "expected",
                    JsonValue{std::to_string(layout::binary_minor)}},
                JsonValue::Member{
                    "reason", JsonValue{"unsupported_binary_minor"}},
            })));
    }
    auto size = read_u32(bytes, byte_count, layout::header_size_offset,
                         path.append("size"));
    auto profile = read_u32(bytes, byte_count, layout::header_profile_offset,
                            path.append("profile"));
    auto flags = read_u32(bytes, byte_count, layout::header_flags_offset,
                          path.append("flags"));
    auto total = read_u64(bytes, byte_count,
                          layout::header_total_length_offset,
                          path.append("total_length"));
    if (!size.has_value()) {
        return Result<HeaderFacts>::failure(std::move(size).error());
    }
    if (!profile.has_value()) {
        return Result<HeaderFacts>::failure(std::move(profile).error());
    }
    if (!flags.has_value()) {
        return Result<HeaderFacts>::failure(std::move(flags).error());
    }
    if (!total.has_value()) {
        return Result<HeaderFacts>::failure(std::move(total).error());
    }
    if (size.value() != layout::header_size) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("size"), "header_size",
            std::to_string(size.value()),
            std::to_string(layout::header_size)));
    }
    if (profile.value() != FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("profile"), "profile",
            std::to_string(profile.value()),
            std::to_string(FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1)));
    }
    if (flags.value() != UINT32_C(0)) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("flags"), "header_flags",
            std::to_string(flags.value()), "0"));
    }
    if (total.value() > byte_count) {
        return Result<HeaderFacts>::failure(simple_error(
            FDB_PAYLOAD_E_OUT_OF_BOUNDS, path.append("total_length"),
            "Declared portable payload length exceeds supplied bytes",
            "declared_total_out_of_bounds"));
    }
    if (total.value() != byte_count) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("total_length"), "trailing_binary_bytes",
            std::to_string(total.value()), std::to_string(byte_count)));
    }
    const std::uint8_t* const digest_bytes =
        bytes + static_cast<std::ptrdiff_t>(
                    layout::header_spec_digest_offset);
    if (!std::equal(compiled.digest().begin(), compiled.digest().end(),
                    digest_bytes)) {
        return Result<HeaderFacts>::failure(Error::from_details(
            FDB_PAYLOAD_E_DIGEST_MISMATCH, path.append("spec_sha256"),
            "Portable payload spec digest does not match",
            JsonValue::object({
                JsonValue::Member{
                    "actual", JsonValue{byte_hex(
                                  digest_bytes, compiled.digest().size())}},
                JsonValue::Member{
                    "expected", JsonValue{byte_hex(
                                    compiled.digest().data(),
                                    compiled.digest().size())}},
                JsonValue::Member{
                    "reason", JsonValue{"spec_digest_mismatch"}},
            })));
    }
    auto reserved = require_zero(
        bytes, byte_count, layout::header_reserved_offset,
        layout::header_size, work, path.append("reserved"),
        "nonzero_header_reserved", false, true);
    if (!reserved.has_value()) {
        return Result<HeaderFacts>::failure(std::move(reserved).error());
    }

    auto region_offset = read_u64(
        bytes, byte_count, layout::header_region_directory_offset,
        path.append("region_directory_offset"));
    auto region_count = read_u32(
        bytes, byte_count, layout::header_region_count_offset,
        path.append("region_count"));
    auto region_size = read_u32(
        bytes, byte_count, layout::header_region_descriptor_size_offset,
        path.append("region_descriptor_size"));
    auto entry_offset = read_u64(
        bytes, byte_count, layout::header_entry_directory_offset,
        path.append("entry_directory_offset"));
    auto entry_count = read_u32(
        bytes, byte_count, layout::header_entry_count_offset,
        path.append("entry_count"));
    auto entry_size = read_u32(
        bytes, byte_count, layout::header_entry_descriptor_size_offset,
        path.append("entry_descriptor_size"));
    auto root_count = read_u64(
        bytes, byte_count, layout::header_root_value_count_offset,
        path.append("root_value_count"));
    if (!region_offset.has_value()) {
        return Result<HeaderFacts>::failure(std::move(region_offset).error());
    }
    if (!region_count.has_value()) {
        return Result<HeaderFacts>::failure(std::move(region_count).error());
    }
    if (!region_size.has_value()) {
        return Result<HeaderFacts>::failure(std::move(region_size).error());
    }
    if (!entry_offset.has_value()) {
        return Result<HeaderFacts>::failure(std::move(entry_offset).error());
    }
    if (!entry_count.has_value()) {
        return Result<HeaderFacts>::failure(std::move(entry_count).error());
    }
    if (!entry_size.has_value()) {
        return Result<HeaderFacts>::failure(std::move(entry_size).error());
    }
    if (!root_count.has_value()) {
        return Result<HeaderFacts>::failure(std::move(root_count).error());
    }
    if (region_offset.value() != layout::header_size) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("region_directory_offset"),
            "region_directory_offset",
            std::to_string(region_offset.value()),
            std::to_string(layout::header_size)));
    }
    if (region_count.value() != expected_regions) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("region_count"), "region_count",
            std::to_string(region_count.value()),
            std::to_string(expected_regions)));
    }
    if (region_size.value() != layout::region_descriptor_size) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("region_descriptor_size"),
            "region_descriptor_size",
            std::to_string(region_size.value()),
            std::to_string(layout::region_descriptor_size)));
    }
    const std::uint64_t expected_entries =
        compiled.resolved().entries().size();
    if (entry_count.value() != expected_entries) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("entry_count"), "entry_count",
            std::to_string(entry_count.value()),
            std::to_string(expected_entries)));
    }
    if (entry_size.value() != layout::entry_descriptor_size) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("entry_descriptor_size"),
            "entry_descriptor_size",
            std::to_string(entry_size.value()),
            std::to_string(layout::entry_descriptor_size)));
    }
    auto region_bytes = layout::checked_multiply_u64(
        region_count.value(), layout::region_descriptor_size,
        path.append("region_count"));
    if (!region_bytes.has_value()) {
        return Result<HeaderFacts>::failure(
            std::move(region_bytes).error());
    }
    auto expected_entry_offset = layout::checked_add_u64(
        layout::header_size, region_bytes.value(),
        path.append("entry_directory_offset"));
    if (!expected_entry_offset.has_value()) {
        return Result<HeaderFacts>::failure(
            std::move(expected_entry_offset).error());
    }
    if (entry_offset.value() != expected_entry_offset.value()) {
        return Result<HeaderFacts>::failure(noncanonical_exact(
            path.append("entry_directory_offset"),
            "entry_directory_not_contiguous",
            std::to_string(entry_offset.value()),
            std::to_string(expected_entry_offset.value())));
    }
    auto entry_bytes = layout::checked_multiply_u64(
        entry_count.value(), layout::entry_descriptor_size,
        path.append("entry_count"));
    if (!entry_bytes.has_value()) {
        return Result<HeaderFacts>::failure(std::move(entry_bytes).error());
    }
    auto entry_end = layout::checked_range_end(
        entry_offset.value(), entry_bytes.value(), byte_count,
        path.append("entry_directory_offset"));
    if (!entry_end.has_value()) {
        return Result<HeaderFacts>::failure(std::move(entry_end).error());
    }
    return Result<HeaderFacts>::success(HeaderFacts{
        region_count.value(), entry_offset.value(), entry_count.value(),
        root_count.value(), entry_end.value()});
}

Result<std::vector<EntryDescriptor>> read_entries(
    const RuntimeSchema& runtime,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    const HeaderFacts& header,
    WorkCounter& work) {
    std::vector<EntryDescriptor> entries;
    entries.reserve(header.entry_count);
    std::uint64_t summed_roots = UINT64_C(0);
    std::uint32_t next_region = UINT32_C(0);
    const auto& source_entries = runtime.spec().resolved().entries();
    for (std::uint32_t index = UINT32_C(0); index < header.entry_count;
         ++index) {
        const JsonPointer path = binary_entry_path(index);
        auto charged = work.charge(UINT64_C(1), path);
        if (!charged.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(charged).error());
        }
        const std::uint64_t base =
            header.entry_directory_offset +
            static_cast<std::uint64_t>(index) *
                layout::entry_descriptor_size;
        auto actual_index = read_u32(
            bytes, byte_count, base + layout::entry_index_offset,
            path.append("index"));
        auto type_id = read_u32(
            bytes, byte_count, base + layout::entry_runtime_type_id_offset,
            path.append("runtime_type_id"));
        auto cardinality = read_u32(
            bytes, byte_count, base + layout::entry_cardinality_offset,
            path.append("cardinality"));
        auto flags = read_u32(
            bytes, byte_count, base + layout::entry_flags_offset,
            path.append("flags"));
        auto value_count = read_u64(
            bytes, byte_count, base + layout::entry_value_count_offset,
            path.append("value_count"));
        auto values_region = read_u32(
            bytes, byte_count,
            base + layout::entry_values_region_index_offset,
            path.append("values_region_index"));
        auto validity_region = read_u32(
            bytes, byte_count,
            base + layout::entry_validity_region_index_offset,
            path.append("validity_region_index"));
        auto reserved = read_u64(
            bytes, byte_count, base + layout::entry_reserved_offset,
            path.append("reserved"));
        if (!actual_index.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(actual_index).error());
        }
        if (!type_id.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(type_id).error());
        }
        if (!cardinality.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(cardinality).error());
        }
        if (!flags.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(flags).error());
        }
        if (!value_count.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(value_count).error());
        }
        if (!values_region.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(values_region).error());
        }
        if (!validity_region.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(validity_region).error());
        }
        if (!reserved.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(reserved).error());
        }
        const spec::Entry& source = source_entries[index];
        const std::uint32_t expected_type = runtime.runtime_id(source.type);
        const std::uint32_t expected_cardinality =
            source.cardinality == Cardinality::one ? UINT32_C(1)
                                                   : UINT32_C(2);
        const std::uint32_t expected_flags =
            source.type.nullable ? UINT32_C(1) : UINT32_C(0);
        const std::uint32_t expected_validity =
            source.type.nullable ? next_region++ : UINT32_MAX;
        const std::uint32_t expected_values = next_region++;
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 7> facts{{
            {actual_index.value(), index},
            {type_id.value(), expected_type},
            {cardinality.value(), expected_cardinality},
            {flags.value(), expected_flags},
            {values_region.value(), expected_values},
            {validity_region.value(), expected_validity},
            {reserved.value(), UINT64_C(0)},
        }};
        const std::array<const char*, 7> names{{
            "index", "runtime_type_id", "cardinality", "flags",
            "values_region_index", "validity_region_index", "reserved"}};
        const std::array<const char*, 7> reasons{{
            "entry_index", "entry_runtime_type_id", "entry_cardinality",
            "entry_flags", "entry_values_region_index",
            "entry_validity_region_index", "entry_reserved"}};
        for (std::size_t fact = 0U; fact < facts.size(); ++fact) {
            if (facts[fact].first != facts[fact].second) {
                return Result<std::vector<EntryDescriptor>>::failure(
                    noncanonical_exact(
                        path.append(names[fact]), reasons[fact],
                        std::to_string(facts[fact].first),
                        std::to_string(facts[fact].second)));
            }
        }
        if (source.cardinality == Cardinality::one &&
            value_count.value() != UINT64_C(1)) {
            return Result<std::vector<EntryDescriptor>>::failure(
                noncanonical_exact(
                    path.append("value_count"),
                    "entry_one_value_count",
                    std::to_string(value_count.value()), "1"));
        }
        auto roots = layout::checked_accumulate_u64(
            summed_roots, value_count.value(), path);
        if (!roots.has_value()) {
            return Result<std::vector<EntryDescriptor>>::failure(
                std::move(roots).error());
        }
        entries.push_back(EntryDescriptor{
            index, expected_type, expected_cardinality, expected_flags,
            value_count.value(), expected_values, expected_validity});
    }
    if (summed_roots != header.root_value_count) {
        return Result<std::vector<EntryDescriptor>>::failure(
            noncanonical_exact(
                binary_header_path().append("root_value_count"),
                "root_value_count_mismatch",
                std::to_string(header.root_value_count),
                std::to_string(summed_roots)));
    }
    return Result<std::vector<EntryDescriptor>>::success(std::move(entries));
}

Result<void> require_validity_tail(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    const RegionFields& validity,
    WorkCounter& work,
    const JsonPointer& path) {
    auto charged = work.charge(validity.byte_length, path);
    if (!charged.has_value()) {
        return charged;
    }
    auto end = layout::checked_range_end(
        validity.data_offset, validity.byte_length, byte_count, path);
    if (!end.has_value()) {
        return Result<void>::failure(std::move(end).error());
    }
    if (validity.element_count == UINT64_C(0)) {
        return Result<void>::success();
    }
    const std::uint32_t used = static_cast<std::uint32_t>(
        validity.element_count % UINT64_C(8));
    if (used == UINT32_C(0)) {
        return Result<void>::success();
    }
    const std::uint8_t tail = bytes[static_cast<std::ptrdiff_t>(
        validity.data_offset + validity.byte_length - UINT64_C(1))];
    const std::uint8_t mask = static_cast<std::uint8_t>(
        UINT8_C(0xff) << used);
    if ((tail & mask) != UINT8_C(0)) {
        return Result<void>::failure(noncanonical(
            path, "nonzero_validity_tail"));
    }
    return Result<void>::success();
}

bool validity_present(const std::uint8_t* bytes,
                      const RegionFields& validity,
                      std::uint64_t index) noexcept {
    const std::uint8_t byte = bytes[static_cast<std::ptrdiff_t>(
        validity.data_offset + index / UINT64_C(8))];
    return ((byte >> (index % UINT64_C(8))) & UINT8_C(1)) != UINT8_C(0);
}

struct ValidationSlot final {
    const spec::TypeNode* type;
    std::uint64_t offset;
    std::uint64_t structural_depth;
    bool present;
    bool charged;
    JsonPointerBuilder::Mark path_mark;
};

struct ListContinuation final {
    const spec::TypeNode* item_type;
    ListRegionState* state;
    std::uint64_t first_index;
    std::uint64_t item_count;
    std::uint64_t next_index;
    std::uint64_t structural_depth;
    JsonPointerBuilder::Mark path_mark;
};

struct ComponentContinuation final {
    const ComponentLayout* component;
    const spec::Component* source_component;
    std::uint64_t offset;
    std::uint64_t structural_depth;
    std::size_t next_field;
    std::uint64_t cursor;
    JsonPointerBuilder::Mark path_mark;
};

using ValidationFrame =
    std::variant<ValidationSlot, ListContinuation, ComponentContinuation>;

Result<void> validate_slot_tree(
    const RuntimeSchema& runtime,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    const spec::TypeNode& root_type,
    std::uint64_t root_offset,
    bool root_present,
    bool root_already_charged,
    std::uint64_t root_structural_depth,
    const std::vector<std::uint64_t>& object_counts,
    const std::vector<PoolMetadata>& pools,
    PoolCursors& pool_cursors,
    ListPartitions& list_partitions,
    std::vector<VariableSlotMetadata>& variables,
    bool validate_text_eager,
    WorkCounter& work,
    std::uint64_t max_nesting_depth,
    const JsonPointer& root_path) {
    std::vector<ValidationFrame> pending;
    JsonPointerBuilder path;
    path.assign(root_path);
    std::uint64_t root_depth = root_structural_depth;
    if (root_present && graph_container_kind(runtime, root_type)) {
        auto incremented = layout::checked_add_u64(
            root_depth, UINT64_C(1), root_path);
        if (!incremented.has_value()) {
            return Result<void>::failure(std::move(incremented).error());
        }
        root_depth = incremented.value();
    }
    pending.push_back(ValidationSlot{
        &root_type, root_offset, root_depth, root_present,
        root_already_charged,
        path.mark()});
    while (!pending.empty()) {
        if (auto* component =
                std::get_if<ComponentContinuation>(&pending.back())) {
            path.rewind(component->path_mark);
            if (component->next_field == component->component->fields.size()) {
                auto tail = require_zero(
                    bytes, byte_count, component->offset + component->cursor,
                    component->offset + component->component->stride, work,
                    path.snapshot(), "nonzero_component_padding", true, false);
                if (!tail.has_value()) {
                    return tail;
                }
                pending.pop_back();
                continue;
            }
            const std::size_t index = component->next_field++;
            const ComponentFieldLayout& field =
                component->component->fields[index];
            const spec::Field& source_field =
                component->source_component->fields[index];
            path.append(source_field.id);
            const JsonPointer field_path = path.snapshot();
            auto padding = require_zero(
                bytes, byte_count, component->offset + component->cursor,
                component->offset + field.offset, work, field_path,
                "nonzero_component_padding", true, false);
            if (!padding.has_value()) {
                return padding;
            }
            auto field_work = work.charge(UINT64_C(1), field_path);
            if (!field_work.has_value()) {
                return field_work;
            }
            const bool present =
                field.validity_bit == UINT32_MAX ||
                ((bytes[static_cast<std::ptrdiff_t>(
                       component->offset +
                       field.validity_bit / UINT32_C(8))] >>
                  (field.validity_bit % UINT32_C(8))) &
                 UINT8_C(1)) != UINT8_C(0);
            auto end = layout::checked_add_u64(
                field.offset, field.slot_stride, field_path);
            if (!end.has_value()) {
                return Result<void>::failure(std::move(end).error());
            }
            component->cursor = end.value();
            std::uint64_t depth = component->structural_depth;
            if (present && graph_container_kind(runtime, source_field.type)) {
                auto incremented = layout::checked_add_u64(
                    depth, UINT64_C(1), field_path);
                if (!incremented.has_value()) {
                    return Result<void>::failure(
                        std::move(incremented).error());
                }
                depth = incremented.value();
            }
            pending.push_back(ValidationSlot{
                &source_field.type, component->offset + field.offset, depth,
                present, true, path.mark()});
            continue;
        }
        if (auto* list = std::get_if<ListContinuation>(&pending.back())) {
            path.rewind(list->path_mark);
            if (list->next_index == list->item_count) {
                pending.pop_back();
                continue;
            }
            const std::uint64_t local_index = list->next_index++;
            path.append(local_index);
            const JsonPointer item_path = path.snapshot();
            auto item_work = work.charge(UINT64_C(1), item_path);
            if (!item_work.has_value()) {
                return item_work;
            }
            auto aggregate_index = layout::checked_add_u64(
                list->first_index, local_index, item_path);
            if (!aggregate_index.has_value()) {
                return Result<void>::failure(
                    std::move(aggregate_index).error());
            }
            auto relative = layout::checked_multiply_u64(
                aggregate_index.value(), list->state->items.stride, item_path);
            if (!relative.has_value()) {
                return Result<void>::failure(std::move(relative).error());
            }
            auto absolute = layout::checked_add_u64(
                list->state->items.data_offset, relative.value(), item_path);
            if (!absolute.has_value()) {
                return Result<void>::failure(std::move(absolute).error());
            }
            bool present = true;
            if (list->state->validity_region_index != UINT32_MAX) {
                auto validity_byte = layout::checked_add_u64(
                    list->state->validity.data_offset,
                    aggregate_index.value() / UINT64_C(8), item_path);
                if (!validity_byte.has_value()) {
                    return Result<void>::failure(
                        std::move(validity_byte).error());
                }
                present =
                    ((bytes[static_cast<std::ptrdiff_t>(
                          validity_byte.value())] >>
                      (aggregate_index.value() % UINT64_C(8))) &
                     UINT8_C(1)) != UINT8_C(0);
            }
            std::uint64_t depth = list->structural_depth;
            if (present && graph_container_kind(runtime, *list->item_type)) {
                auto incremented = layout::checked_add_u64(
                    depth, UINT64_C(1), item_path);
                if (!incremented.has_value()) {
                    return Result<void>::failure(
                        std::move(incremented).error());
                }
                depth = incremented.value();
            }
            pending.push_back(ValidationSlot{
                list->item_type, absolute.value(), depth, present,
                true, path.mark()});
            continue;
        }

        ValidationSlot slot =
            std::move(std::get<ValidationSlot>(pending.back()));
        pending.pop_back();
        path.rewind(slot.path_mark);
        const JsonPointer slot_path = path.snapshot();
        if (!slot.charged) {
            auto charged = work.charge(UINT64_C(1), slot_path);
            if (!charged.has_value()) {
                return charged;
            }
        }
        const std::uint32_t type_id = runtime.runtime_id(*slot.type);
        const RuntimeType* const type = runtime.find_type(type_id);
        if (type_id == UINT32_MAX || type == nullptr ||
            type->source == nullptr) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, slot_path,
                "Portable payload slot type metadata is missing",
                "slot_runtime_type_missing"));
        }
        if (slot.present && structural_container(*type) &&
            slot.structural_depth > max_nesting_depth) {
            return Result<void>::failure(resource_error(
                slot_path, "nesting_depth", slot.structural_depth,
                max_nesting_depth));
        }
        auto slot_end = layout::checked_range_end(
            slot.offset, type->slot.stride, byte_count, slot_path);
        if (!slot_end.has_value()) {
            return Result<void>::failure(std::move(slot_end).error());
        }
        if (!slot.present) {
            if (!slot.type->nullable) {
                return Result<void>::failure(noncanonical(
                    slot_path, "null_nonnullable_value"));
            }
            const bool nested =
                type->storage_role == StorageRole::inline_component;
            auto zero = require_zero(
                bytes, byte_count, slot.offset, slot_end.value(), work,
                slot_path, "nonzero_null_slot", nested, false);
            if (!zero.has_value()) {
                return zero;
            }
            if (variable_kind(slot.type->kind)) {
                variables.push_back(VariableSlotMetadata{
                    slot.type->kind, slot.offset, UINT64_C(0), UINT64_C(0),
                    false});
            }
            continue;
        }

        if (type->storage_role == StorageRole::object_root_id ||
            type->storage_role == StorageRole::reference_id) {
            auto object_id =
                read_u64(bytes, byte_count, slot.offset, slot_path);
            if (!object_id.has_value()) {
                return Result<void>::failure(std::move(object_id).error());
            }
            const std::uint32_t component =
                type->source->resolved_component_index;
            if (component >= object_counts.size() ||
                object_id.value() >= object_counts[component]) {
                const char* const reason =
                    type->storage_role == StorageRole::object_root_id
                        ? "root_object_id_out_of_range"
                        : "reference_object_id_out_of_range";
                return Result<void>::failure(Error::from_details(
                    FDB_PAYLOAD_E_INVALID_REFERENCE, slot_path,
                    "Portable payload object reference is invalid",
                    JsonValue::object({
                        JsonValue::Member{
                            "object_id",
                            JsonValue{std::to_string(object_id.value())}},
                        JsonValue::Member{"reason", JsonValue{reason}},
                    })));
            }
            continue;
        }

        if (variable_kind(slot.type->kind)) {
            auto relative_offset =
                read_u64(bytes, byte_count, slot.offset, slot_path);
            auto length_offset = layout::checked_add_u64(
                slot.offset, UINT64_C(8), slot_path);
            if (!length_offset.has_value()) {
                return Result<void>::failure(
                    std::move(length_offset).error());
            }
            auto byte_length = read_u64(
                bytes, byte_count, length_offset.value(), slot_path);
            if (!relative_offset.has_value() || !byte_length.has_value()) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_OUT_OF_BOUNDS, slot_path,
                    "Portable payload variable descriptor is outside the image",
                    "variable_descriptor_out_of_bounds"));
            }
            if (slot.type->kind == TypeKind::wstr &&
                ((relative_offset.value() | byte_length.value()) &
                 UINT64_C(1)) != UINT64_C(0)) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_MISALIGNED, slot_path,
                    "Portable payload UTF-16LE descriptor is misaligned",
                    "utf16_descriptor_misaligned"));
            }
            const PoolMetadata* const pool = find_pool(pools, slot.type->kind);
            if (pool == nullptr) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, slot_path,
                    "Portable payload variable pool metadata is missing",
                    "variable_pool_missing"));
            }
            std::uint64_t& pool_cursor =
                pool_cursors.for_kind(slot.type->kind);
            auto advanced = layout::checked_partition_advance(
                pool_cursor, relative_offset.value(), byte_length.value(),
                pool->byte_length, slot_path);
            if (!advanced.has_value()) {
                return Result<void>::failure(std::move(advanced).error());
            }
            pool_cursor = advanced.value();
            auto absolute = layout::checked_add_u64(
                pool->data_offset, relative_offset.value(), slot_path);
            if (!absolute.has_value()) {
                return Result<void>::failure(std::move(absolute).error());
            }
            auto bounded = layout::checked_range_end(
                absolute.value(), byte_length.value(), byte_count, slot_path);
            if (!bounded.has_value()) {
                return Result<void>::failure(std::move(bounded).error());
            }
            variables.push_back(VariableSlotMetadata{
                slot.type->kind, slot.offset, relative_offset.value(),
                byte_length.value(), true});
            if (validate_text_eager && slot.type->kind != TypeKind::bytes) {
                const std::uint64_t units =
                    slot.type->kind == TypeKind::str
                        ? byte_length.value()
                        : byte_length.value() / UINT64_C(2);
                auto content_work = work.charge(units, slot_path);
                if (!content_work.has_value()) {
                    return content_work;
                }
                Result<void> valid =
                    slot.type->kind == TypeKind::str
                        ? layout::validate_utf8(
                              std::string_view{
                                  reinterpret_cast<const char*>(bytes) +
                                      static_cast<std::ptrdiff_t>(
                                          absolute.value()),
                                  static_cast<std::size_t>(
                                      byte_length.value())},
                              slot_path)
                        : layout::validate_utf16le(
                              bytes + static_cast<std::ptrdiff_t>(
                                          absolute.value()),
                              byte_length.value(), slot_path);
                if (!valid.has_value()) {
                    return valid;
                }
            }
            continue;
        }

        if (slot.type->kind == TypeKind::list) {
            auto first_index = read_u64(
                bytes, byte_count, slot.offset, slot_path);
            auto count_offset = layout::checked_add_u64(
                slot.offset, UINT64_C(8), slot_path);
            if (!count_offset.has_value()) {
                return Result<void>::failure(
                    std::move(count_offset).error());
            }
            auto item_count = read_u64(
                bytes, byte_count, count_offset.value(), slot_path);
            if (!first_index.has_value() || !item_count.has_value()) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_OUT_OF_BOUNDS, slot_path,
                    "Portable payload list descriptor is outside the image",
                    "list_descriptor_out_of_bounds"));
            }
            ListRegionState* const state = list_partitions.find(type_id);
            if (state == nullptr || slot.type->items == nullptr ||
                state->item_runtime_type_id !=
                    runtime.runtime_id(*slot.type->items)) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, slot_path,
                    "Portable payload list region metadata is missing",
                    "list_region_metadata_missing"));
            }
            auto first_byte = layout::checked_multiply_u64(
                first_index.value(), state->items.stride, slot_path);
            auto item_bytes = layout::checked_multiply_u64(
                item_count.value(), state->items.stride, slot_path);
            if (!first_byte.has_value()) {
                return Result<void>::failure(std::move(first_byte).error());
            }
            if (!item_bytes.has_value()) {
                return Result<void>::failure(std::move(item_bytes).error());
            }
            auto bounded = layout::checked_range_end(
                first_byte.value(), item_bytes.value(),
                state->items.byte_length, slot_path);
            if (!bounded.has_value()) {
                return Result<void>::failure(std::move(bounded).error());
            }
            auto advanced = layout::checked_partition_advance(
                state->cursor, first_index.value(), item_count.value(),
                state->items.element_count, slot_path);
            if (!advanced.has_value()) {
                return Result<void>::failure(std::move(advanced).error());
            }
            state->cursor = advanced.value();
            if (item_count.value() != UINT64_C(0)) {
                pending.push_back(ListContinuation{
                    slot.type->items.get(), state, first_index.value(),
                    item_count.value(), UINT64_C(0), slot.structural_depth,
                    slot.path_mark});
            }
            continue;
        }

        if (type->storage_role == StorageRole::inline_component) {
            const ComponentLayout* const component = runtime.component(
                slot.type->resolved_component_index);
            if (component == nullptr ||
                slot.type->resolved_component_index >=
                    runtime.spec().resolved().components().size()) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, slot_path,
                    "Graph component layout is missing",
                    "component_layout_missing"));
            }
            auto validity = work.charge(component->validity_bytes, slot_path);
            if (!validity.has_value()) {
                return validity;
            }
            if (component->validity_bytes != UINT32_C(0)) {
                const std::uint32_t nullable_count =
                    static_cast<std::uint32_t>(std::count_if(
                        component->fields.begin(), component->fields.end(),
                        [](const ComponentFieldLayout& field) {
                            return field.validity_bit != UINT32_MAX;
                        }));
                const std::uint32_t used = nullable_count % UINT32_C(8);
                if (used != UINT32_C(0)) {
                    const std::uint8_t tail = bytes[
                        static_cast<std::ptrdiff_t>(
                            slot.offset + component->validity_bytes -
                            UINT32_C(1))];
                    if ((tail & static_cast<std::uint8_t>(
                                    UINT8_C(0xff) << used)) != UINT8_C(0)) {
                        return Result<void>::failure(noncanonical(
                            slot_path, "nonzero_component_validity_tail"));
                    }
                }
            }
            pending.push_back(ComponentContinuation{
                component,
                &runtime.spec().resolved().components()[
                    slot.type->resolved_component_index],
                slot.offset, slot.structural_depth, 0U,
                component->validity_bytes, slot.path_mark});
            continue;
        }

        switch (slot.type->kind) {
        case TypeKind::boolean:
            if (bytes[static_cast<std::ptrdiff_t>(slot.offset)] > UINT8_C(1)) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INVALID_BINARY_VALUE, slot_path,
                    "Portable payload binary value is invalid",
                    "invalid_boolean_byte"));
            }
            break;
        case TypeKind::f32: {
            auto bits = layout::load_u32_le(
                bytes, byte_count, slot.offset, slot_path);
            if (!bits.has_value()) {
                return Result<void>::failure(std::move(bits).error());
            }
            if (noncanonical_f32_nan(bits.value())) {
                return Result<void>::failure(noncanonical(
                    slot_path, "noncanonical_f32_nan"));
            }
            break;
        }
        case TypeKind::f64: {
            auto bits = layout::load_u64_le(
                bytes, byte_count, slot.offset, slot_path);
            if (!bits.has_value()) {
                return Result<void>::failure(std::move(bits).error());
            }
            if (noncanonical_f64_nan(bits.value())) {
                return Result<void>::failure(noncanonical(
                    slot_path, "noncanonical_f64_nan"));
            }
            break;
        }
        case TypeKind::u8:
        case TypeKind::u16:
        case TypeKind::u32:
        case TypeKind::i32:
        case TypeKind::u8n:
        case TypeKind::u16n:
            break;
        case TypeKind::str:
        case TypeKind::wstr:
        case TypeKind::bytes:
        case TypeKind::component:
        case TypeKind::ref:
        case TypeKind::list:
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, slot_path,
                "Portable graph slot has an inconsistent storage role",
                "graph_slot_storage_role_mismatch"));
        }
    }
    return Result<void>::success();
}

struct ObjectCoordinate final {
    std::uint32_t component_index;
    std::uint64_t object_id;
};

struct ReachSlot final {
    const spec::TypeNode* type;
    std::uint64_t offset;
    bool present;
    bool charged;
    JsonPointerBuilder::Mark path_mark;
};

struct ReachListContinuation final {
    const spec::TypeNode* item_type;
    const ListRegionState* state;
    std::uint64_t first_index;
    std::uint64_t item_count;
    std::uint64_t next_index;
    JsonPointerBuilder::Mark path_mark;
};

struct ReachComponentContinuation final {
    const ComponentLayout* component;
    const spec::Component* source_component;
    std::uint64_t offset;
    std::size_t next_field;
    JsonPointerBuilder::Mark path_mark;
};

using ReachFrame =
    std::variant<ReachSlot, ReachListContinuation,
                 ReachComponentContinuation>;

Result<void> revisit_slot_tree(
    const RuntimeSchema& runtime,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    const spec::TypeNode& root_type,
    std::uint64_t root_offset,
    bool root_present,
    bool root_already_charged,
    const ListPartitions& list_partitions,
    std::vector<std::vector<std::uint8_t>>& reached,
    std::vector<ObjectCoordinate>& queue,
    WorkCounter& work,
    std::vector<ReachFrame>& pending,
    const JsonPointer& root_path) {
    pending.clear();
    JsonPointerBuilder path;
    path.assign(root_path);
    pending.push_back(ReachSlot{
        &root_type, root_offset, root_present, root_already_charged,
        path.mark()});

    const auto enqueue = [&](std::uint32_t component_index,
                             std::uint64_t object_id,
                             const JsonPointer& slot_path) -> Result<void> {
        if (component_index >= reached.size() ||
            object_id >= reached[component_index].size()) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, slot_path,
                "Validated graph coordinate is outside its object pool",
                "validated_object_coordinate_out_of_range"));
        }
        std::uint8_t& marker =
            reached[component_index][static_cast<std::size_t>(object_id)];
        if (marker == UINT8_C(0)) {
            marker = UINT8_C(1);
            queue.push_back(ObjectCoordinate{component_index, object_id});
        }
        return Result<void>::success();
    };

    while (!pending.empty()) {
        if (auto* component =
                std::get_if<ReachComponentContinuation>(&pending.back())) {
            path.rewind(component->path_mark);
            if (component->next_field == component->component->fields.size()) {
                pending.pop_back();
                continue;
            }
            const std::size_t field_index = component->next_field++;
            const ComponentFieldLayout& field =
                component->component->fields[field_index];
            const spec::Field& source_field =
                component->source_component->fields[field_index];
            path.append(source_field.id);
            const JsonPointer field_path = path.snapshot();
            auto charged = work.charge(UINT64_C(1), field_path);
            if (!charged.has_value()) {
                return charged;
            }
            const bool present =
                field.validity_bit == UINT32_MAX ||
                ((bytes[static_cast<std::ptrdiff_t>(
                       component->offset +
                       field.validity_bit / UINT32_C(8))] >>
                  (field.validity_bit % UINT32_C(8))) &
                 UINT8_C(1)) != UINT8_C(0);
            pending.push_back(ReachSlot{
                &source_field.type, component->offset + field.offset,
                present, true, path.mark()});
            continue;
        }
        if (auto* list =
                std::get_if<ReachListContinuation>(&pending.back())) {
            path.rewind(list->path_mark);
            if (list->next_index == list->item_count) {
                pending.pop_back();
                continue;
            }
            const std::uint64_t local_index = list->next_index++;
            path.append(local_index);
            const JsonPointer item_path = path.snapshot();
            auto charged = work.charge(UINT64_C(1), item_path);
            if (!charged.has_value()) {
                return charged;
            }
            auto aggregate_index = layout::checked_add_u64(
                list->first_index, local_index, item_path);
            if (!aggregate_index.has_value()) {
                return Result<void>::failure(
                    std::move(aggregate_index).error());
            }
            auto relative = layout::checked_multiply_u64(
                aggregate_index.value(), list->state->items.stride,
                item_path);
            if (!relative.has_value()) {
                return Result<void>::failure(std::move(relative).error());
            }
            auto absolute = layout::checked_add_u64(
                list->state->items.data_offset, relative.value(), item_path);
            if (!absolute.has_value()) {
                return Result<void>::failure(std::move(absolute).error());
            }
            bool present = true;
            if (list->state->validity_region_index != UINT32_MAX) {
                const std::uint64_t validity_offset =
                    list->state->validity.data_offset +
                    aggregate_index.value() / UINT64_C(8);
                present =
                    ((bytes[static_cast<std::ptrdiff_t>(validity_offset)] >>
                      (aggregate_index.value() % UINT64_C(8))) &
                     UINT8_C(1)) != UINT8_C(0);
            }
            pending.push_back(ReachSlot{
                list->item_type, absolute.value(), present, true,
                path.mark()});
            continue;
        }

        ReachSlot slot = std::move(std::get<ReachSlot>(pending.back()));
        pending.pop_back();
        path.rewind(slot.path_mark);
        const JsonPointer slot_path = path.snapshot();
        if (!slot.charged) {
            auto charged = work.charge(UINT64_C(1), slot_path);
            if (!charged.has_value()) {
                return charged;
            }
        }
        if (!slot.present) {
            continue;
        }
        const std::uint32_t type_id = runtime.runtime_id(*slot.type);
        const RuntimeType* const type = runtime.find_type(type_id);
        if (type_id == UINT32_MAX || type == nullptr ||
            type->source == nullptr) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, slot_path,
                "Reachability runtime type metadata is missing",
                "reachability_runtime_type_missing"));
        }
        if (type->storage_role == StorageRole::object_root_id ||
            type->storage_role == StorageRole::reference_id) {
            auto object_id =
                read_u64(bytes, byte_count, slot.offset, slot_path);
            if (!object_id.has_value()) {
                return Result<void>::failure(std::move(object_id).error());
            }
            auto queued = enqueue(
                type->source->resolved_component_index, object_id.value(),
                slot_path);
            if (!queued.has_value()) {
                return queued;
            }
            continue;
        }
        if (slot.type->kind == TypeKind::list) {
            auto first_index = read_u64(
                bytes, byte_count, slot.offset, slot_path);
            auto item_count = read_u64(
                bytes, byte_count, slot.offset + UINT64_C(8), slot_path);
            const ListRegionState* const state =
                type_id < list_partitions.indexes.size() &&
                        list_partitions.indexes[type_id] <
                            list_partitions.states.size()
                    ? &list_partitions.states[
                          list_partitions.indexes[type_id]]
                    : nullptr;
            if (!first_index.has_value() || !item_count.has_value() ||
                state == nullptr || slot.type->items == nullptr) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, slot_path,
                    "Validated graph list metadata is missing",
                    "validated_list_metadata_missing"));
            }
            if (item_count.value() != UINT64_C(0)) {
                pending.push_back(ReachListContinuation{
                    slot.type->items.get(), state, first_index.value(),
                    item_count.value(), UINT64_C(0), slot.path_mark});
            }
            continue;
        }
        if (type->storage_role == StorageRole::inline_component) {
            const std::uint32_t component_index =
                slot.type->resolved_component_index;
            const ComponentLayout* const component =
                runtime.component(component_index);
            if (component == nullptr ||
                component_index >=
                    runtime.spec().resolved().components().size()) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, slot_path,
                    "Validated graph component metadata is missing",
                    "validated_component_metadata_missing"));
            }
            pending.push_back(ReachComponentContinuation{
                component,
                &runtime.spec().resolved().components()[component_index],
                slot.offset, 0U, slot.path_mark});
        }
    }
    return Result<void>::success();
}

Result<void> validate_reachability(
    const RuntimeSchema& runtime,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    const std::vector<EntryState>& entries,
    const std::vector<ObjectPoolState>& pools,
    const std::vector<std::uint64_t>& object_counts,
    std::uint64_t graph_object_count,
    const ListPartitions& list_partitions,
    WorkCounter& work) {
    std::vector<std::vector<std::uint8_t>> reached;
    const auto marker_capacity = static_cast<std::uint64_t>(
        std::vector<std::uint8_t>{}.max_size());
    for (const std::uint64_t count : object_counts) {
        if (count > marker_capacity) {
            return Result<void>::failure(resource_error(
                JsonPointer{}.append("objects"), "graph_objects", count,
                marker_capacity));
        }
    }
    std::vector<ObjectCoordinate> queue;
    if (graph_object_count > static_cast<std::uint64_t>(queue.max_size())) {
        return Result<void>::failure(resource_error(
            JsonPointer{}.append("objects"), "graph_objects",
            graph_object_count,
            static_cast<std::uint64_t>(queue.max_size())));
    }
    if (object_counts.size() > reached.max_size()) {
        return Result<void>::failure(resource_error(
            JsonPointer{}.append("objects"), "components",
            object_counts.size(), reached.max_size()));
    }
    reached.reserve(object_counts.size());
    for (const std::uint64_t count : object_counts) {
        reached.emplace_back(static_cast<std::size_t>(count), UINT8_C(0));
    }
    queue.reserve(static_cast<std::size_t>(graph_object_count));
    std::vector<std::uint32_t> pool_indexes(
        object_counts.size(), UINT32_MAX);
    for (std::uint32_t index = UINT32_C(0); index < pools.size(); ++index) {
        if (pools[index].component_index >= pool_indexes.size()) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("objects"),
                "Validated object pool index is out of range",
                "validated_object_pool_out_of_range"));
        }
        pool_indexes[pools[index].component_index] = index;
    }
    std::vector<ReachFrame> pending;

    const auto& source_entries = runtime.spec().resolved().entries();
    for (const EntryState& entry : entries) {
        const spec::Entry& source =
            source_entries[entry.descriptor.entry_index];
        for (std::uint64_t index = UINT64_C(0);
             index < entry.descriptor.value_count; ++index) {
            const JsonPointer path =
                JsonPointer{}.append("entries").append(source.id).append(index);
            const bool present =
                !entry.has_validity ||
                validity_present(bytes, entry.validity, index);
            auto relative = layout::checked_multiply_u64(
                index, entry.values.stride, path);
            if (!relative.has_value()) {
                return Result<void>::failure(std::move(relative).error());
            }
            auto absolute = layout::checked_add_u64(
                entry.values.data_offset, relative.value(), path);
            if (!absolute.has_value()) {
                return Result<void>::failure(std::move(absolute).error());
            }
            auto revisited = revisit_slot_tree(
                runtime, bytes, byte_count, source.type, absolute.value(),
                present, false, list_partitions, reached, queue, work,
                pending, path);
            if (!revisited.has_value()) {
                return revisited;
            }
        }
    }

    std::size_t next = 0U;
    while (next < queue.size()) {
        const ObjectCoordinate coordinate = queue[next++];
        const JsonPointer object_path =
            JsonPointer{}
                .append("objects")
                .append(runtime.spec().resolved().components()[
                    coordinate.component_index]
                            .id)
                .append(coordinate.object_id);
        auto object_work = work.charge(UINT64_C(1), object_path);
        if (!object_work.has_value()) {
            return object_work;
        }
        if (coordinate.component_index >= pool_indexes.size() ||
            pool_indexes[coordinate.component_index] >= pools.size()) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, object_path,
                "Validated object pool metadata is missing",
                "validated_object_pool_missing"));
        }
        const ObjectPoolState& pool =
            pools[pool_indexes[coordinate.component_index]];
        const ComponentLayout* const component =
            runtime.component(coordinate.component_index);
        if (component == nullptr) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, object_path,
                "Validated object component metadata is missing",
                "validated_object_component_missing"));
        }
        auto relative = layout::checked_multiply_u64(
            coordinate.object_id, component->stride, object_path);
        if (!relative.has_value()) {
            return Result<void>::failure(std::move(relative).error());
        }
        auto absolute = layout::checked_add_u64(
            pool.values.data_offset, relative.value(), object_path);
        if (!absolute.has_value()) {
            return Result<void>::failure(std::move(absolute).error());
        }
        const spec::Component& source_component =
            runtime.spec().resolved().components()[
                coordinate.component_index];
        for (std::size_t field_index = 0U;
             field_index < component->fields.size(); ++field_index) {
            const ComponentFieldLayout& field =
                component->fields[field_index];
            const spec::Field& source_field =
                source_component.fields[field_index];
            const JsonPointer field_path = object_path.append(source_field.id);
            auto field_work = work.charge(UINT64_C(1), field_path);
            if (!field_work.has_value()) {
                return field_work;
            }
            const bool present =
                field.validity_bit == UINT32_MAX ||
                ((bytes[static_cast<std::ptrdiff_t>(
                       absolute.value() +
                       field.validity_bit / UINT32_C(8))] >>
                  (field.validity_bit % UINT32_C(8))) &
                 UINT8_C(1)) != UINT8_C(0);
            auto revisited = revisit_slot_tree(
                runtime, bytes, byte_count, source_field.type,
                absolute.value() + field.offset, present, true,
                list_partitions, reached, queue, work, pending, field_path);
            if (!revisited.has_value()) {
                return revisited;
            }
        }
    }

    for (std::uint32_t component_index = UINT32_C(0);
         component_index < reached.size(); ++component_index) {
        for (std::uint64_t object_id = UINT64_C(0);
             object_id < reached[component_index].size(); ++object_id) {
            const JsonPointer path =
                JsonPointer{}
                    .append("objects")
                    .append(runtime.spec().resolved().components()[
                        component_index]
                                .id)
                    .append(object_id);
            auto marker_work = work.charge(UINT64_C(1), path);
            if (!marker_work.has_value()) {
                return marker_work;
            }
            if (reached[component_index]
                       [static_cast<std::size_t>(object_id)] == UINT8_C(0)) {
                return Result<void>::failure(noncanonical(
                    path, "unreachable_object"));
            }
        }
    }
    return Result<void>::success();
}

Result<void> validate_object_record(
    const RuntimeSchema& runtime,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    const ObjectPoolState& pool,
    std::uint64_t object_id,
    const std::vector<std::uint64_t>& object_counts,
    const std::vector<PoolMetadata>& variable_pools,
    PoolCursors& pool_cursors,
    ListPartitions& list_partitions,
    std::vector<VariableSlotMetadata>& variables,
    bool validate_text_eager,
    WorkCounter& work,
    std::uint64_t max_nesting_depth) {
    const ComponentLayout* const component =
        runtime.component(pool.component_index);
    if (component == nullptr) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("objects"),
            "Graph component layout is missing",
            "component_layout_missing"));
    }
    if (pool.component_index >=
        runtime.spec().resolved().components().size()) {
        return Result<void>::failure(simple_error(
            FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("objects"),
            "Graph source component is missing",
            "component_source_missing"));
    }
    const spec::Component& source_component =
        runtime.spec().resolved().components()[pool.component_index];
    const JsonPointer path = JsonPointer{}
                                 .append("objects")
                                 .append(pool.component_index)
                                 .append(object_id);
    if (max_nesting_depth < UINT64_C(1)) {
        return Result<void>::failure(resource_error(
            path, "nesting_depth", UINT64_C(1), max_nesting_depth));
    }
    auto offset = layout::checked_multiply_u64(
        object_id, component->stride, path);
    if (!offset.has_value()) {
        return Result<void>::failure(std::move(offset).error());
    }
    auto absolute = layout::checked_add_u64(
        pool.values.data_offset, offset.value(), path);
    if (!absolute.has_value()) {
        return Result<void>::failure(std::move(absolute).error());
    }
    auto validity = work.charge(component->validity_bytes, path);
    if (!validity.has_value()) {
        return validity;
    }
    if (component->validity_bytes != UINT32_C(0)) {
        const std::uint32_t nullable_count =
            static_cast<std::uint32_t>(std::count_if(
                component->fields.begin(), component->fields.end(),
                [](const ComponentFieldLayout& field) {
                    return field.validity_bit != UINT32_MAX;
                }));
        const std::uint32_t used = nullable_count % UINT32_C(8);
        if (used != UINT32_C(0)) {
            const std::uint8_t tail = bytes[static_cast<std::ptrdiff_t>(
                absolute.value() + component->validity_bytes - UINT32_C(1))];
            if ((tail & static_cast<std::uint8_t>(
                            UINT8_C(0xff) << used)) != UINT8_C(0)) {
                return Result<void>::failure(noncanonical(
                    path, "nonzero_component_validity_tail"));
            }
        }
    }
    std::uint64_t cursor = component->validity_bytes;
    for (std::size_t field_index = 0U;
         field_index < component->fields.size(); ++field_index) {
        const ComponentFieldLayout& field = component->fields[field_index];
        const spec::Field& source_field = source_component.fields[field_index];
        auto padding = require_zero(
            bytes, byte_count, absolute.value() + cursor,
            absolute.value() + field.offset, work, path,
            "nonzero_component_padding", true, false);
        if (!padding.has_value()) {
            return padding;
        }
        const bool present =
            field.validity_bit == UINT32_MAX ||
            ((bytes[static_cast<std::ptrdiff_t>(
                   absolute.value() + field.validity_bit / UINT32_C(8))] >>
              (field.validity_bit % UINT32_C(8))) &
             UINT8_C(1)) != UINT8_C(0);
        auto validated = validate_slot_tree(
            runtime, bytes, byte_count, source_field.type,
            absolute.value() + field.offset, present, false, UINT64_C(1),
            object_counts, variable_pools, pool_cursors, list_partitions,
            variables, validate_text_eager, work, max_nesting_depth,
            path.append(source_field.id));
        if (!validated.has_value()) {
            return validated;
        }
        auto end = layout::checked_add_u64(
            field.offset, field.slot_stride, path);
        if (!end.has_value()) {
            return Result<void>::failure(std::move(end).error());
        }
        cursor = end.value();
    }
    return require_zero(
        bytes, byte_count, absolute.value() + cursor,
        absolute.value() + component->stride, work, path,
        "nonzero_component_padding", true, false);
}

}  // namespace

Result<PayloadIndex> open_graph(const spec::CompiledSpec& compiled,
                                  const std::uint8_t* bytes,
                                  std::uint64_t byte_count,
                                  OpenOptions limits) {
    try {
        if (compiled.profile() != Profile::object_graph_v1) {
            return Result<PayloadIndex>::failure(simple_error(
                FDB_PAYLOAD_E_PROFILE_VIOLATION, JsonPointer{},
                "Graph open requires object_graph.v1",
                "graph_profile_required"));
        }
        const std::uint64_t expected_entries =
            compiled.resolved().entries().size();
        const std::size_t expected_component_count =
            compiled.resolved().components().size();
        const std::uint64_t expected_components =
            expected_component_count;
        if (expected_entries > limits.max_entries) {
            return Result<PayloadIndex>::failure(resource_error(
                binary_header_path().append("entry_count"), "entries",
                expected_entries, limits.max_entries));
        }
        if (expected_components > limits.max_components) {
            return Result<PayloadIndex>::failure(resource_error(
                binary_header_path(), "components", expected_components,
                limits.max_components));
        }
        auto runtime = RuntimeSchema::compile(compiled);
        if (!runtime.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(runtime).error());
        }
        PayloadIndex output;
        output.list_slots_.assign(runtime.value().type_count(), std::nullopt);
        output.object_pools_.assign(expected_component_count, std::nullopt);

        std::uint64_t expected_regions = expected_entries;
        for (const spec::Entry& entry : compiled.resolved().entries()) {
            if (entry.type.nullable) {
                auto added = layout::checked_accumulate_u64(
                    expected_regions, UINT64_C(1),
                    JsonPointer{}.append("regions"));
                if (!added.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(added).error());
                }
            }
        }
        auto added = layout::checked_accumulate_u64(
            expected_regions, runtime.value().identity_components().size(),
            JsonPointer{}.append("regions"));
        if (!added.has_value()) {
            return Result<PayloadIndex>::failure(std::move(added).error());
        }
        for (const layout::ListNodeLayout& list :
             runtime.value().list_nodes()) {
            added = layout::checked_accumulate_u64(
                expected_regions, list.item_nullable ? UINT64_C(2)
                                                     : UINT64_C(1),
                JsonPointer{}.append("regions"));
            if (!added.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(added).error());
            }
        }
        const std::uint64_t pool_regions =
            (runtime.value().has_utf8_pool() ? UINT64_C(1) : UINT64_C(0)) +
            (runtime.value().has_utf16_pool() ? UINT64_C(1) : UINT64_C(0)) +
            (runtime.value().has_bytes_pool() ? UINT64_C(1) : UINT64_C(0));
        added = layout::checked_accumulate_u64(
            expected_regions, pool_regions,
            JsonPointer{}.append("regions"));
        if (!added.has_value()) {
            return Result<PayloadIndex>::failure(std::move(added).error());
        }
        if (expected_regions > limits.max_regions) {
            return Result<PayloadIndex>::failure(resource_error(
                binary_header_path().append("region_count"), "regions",
                expected_regions, limits.max_regions));
        }

        WorkCounter work(limits.max_validation_work);
        auto header = validate_header(
            compiled, bytes, byte_count, expected_regions, limits, work);
        if (!header.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(header).error());
        }
        auto entries = read_entries(
            runtime.value(), bytes, byte_count, header.value(), work);
        if (!entries.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(entries).error());
        }
        auto data_start = layout::checked_align_up_u64(
            header.value().entry_end, UINT32_C(8),
            binary_header_path().append("entry_directory_offset"));
        if (!data_start.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(data_start).error());
        }
        auto directory_padding = require_zero(
            bytes, byte_count, header.value().entry_end,
            data_start.value(), work,
            binary_header_path().append("entry_directory_offset"),
            "nonzero_directory_padding", true, true);
        if (!directory_padding.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(directory_padding).error());
        }

        std::uint64_t cursor = data_start.value();
        std::uint32_t region_index = UINT32_C(0);
        std::vector<EntryState> entry_states;
        entry_states.reserve(entries.value().size());
        for (const EntryDescriptor& entry : entries.value()) {
            EntryState state{entry, RegionFields{}, RegionFields{}, false};
            const RuntimeType* const type =
                runtime.value().find_type(entry.runtime_type_id);
            if (type == nullptr) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, JsonPointer{},
                    "Graph entry runtime type is missing",
                    "entry_runtime_type_missing"));
            }
            const std::uint32_t local_count =
                (entry.flags & UINT32_C(1)) != UINT32_C(0)
                    ? UINT32_C(2)
                    : UINT32_C(1);
            for (std::uint32_t local = UINT32_C(0); local < local_count;
                 ++local) {
                const bool validity = local_count == UINT32_C(2) &&
                                      local == UINT32_C(0);
                const JsonPointer path = binary_region_path(region_index);
                auto charged = work.charge(UINT64_C(1), path);
                if (!charged.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(charged).error());
                }
                auto fields = read_region_fields(
                    bytes, byte_count, region_index, path);
                if (!fields.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(fields).error());
                }
                const std::uint32_t alignment =
                    validity ? UINT32_C(1) : type->slot.alignment;
                const std::uint32_t stride =
                    validity ? UINT32_C(0) : type->slot.stride;
                auto aligned = layout::checked_align_up_u64(
                    cursor, alignment, path.append("data_offset"));
                if (!aligned.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(aligned).error());
                }
                auto length = layout::checked_multiply_u64(
                    entry.value_count, type->slot.stride, path);
                if (validity) {
                    auto rounded = layout::checked_add_u64(
                        entry.value_count, UINT64_C(7), path);
                    if (!rounded.has_value()) {
                        return Result<PayloadIndex>::failure(
                            std::move(rounded).error());
                    }
                    length = Result<std::uint64_t>::success(
                        rounded.value() / UINT64_C(8));
                }
                if (!length.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(length).error());
                }
                const RegionFields expected{
                    static_cast<std::uint32_t>(
                        validity ? layout::RegionKind::entry_validity
                                 : layout::RegionKind::entry_values),
                    UINT32_C(0), entry.entry_index, entry.runtime_type_id,
                    aligned.value(), length.value(), entry.value_count,
                    stride, alignment, UINT64_C(0)};
                auto valid = validate_region_fields(
                    fields.value(), expected, path);
                if (!valid.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(valid).error());
                }
                auto padding = require_zero(
                    bytes, byte_count, cursor, aligned.value(), work,
                    path.append("data_offset"),
                    "nonzero_inter_region_padding", true, true);
                if (!padding.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(padding).error());
                }
                auto end = layout::checked_range_end(
                    fields.value().data_offset,
                    fields.value().byte_length, byte_count,
                    path.append("byte_length"));
                if (!end.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(end).error());
                }
                cursor = end.value();
                if (validity) {
                    state.validity = fields.value();
                    state.has_validity = true;
                } else {
                    state.values = fields.value();
                    auto slot_work = work.charge(
                        entry.value_count, path);
                    if (!slot_work.has_value()) {
                        return Result<PayloadIndex>::failure(
                            std::move(slot_work).error());
                    }
                }
                ++region_index;
            }
            entry_states.push_back(state);
        }

        std::vector<ObjectPoolState> pools;
        pools.reserve(runtime.value().identity_components().size());
        std::vector<std::uint64_t> object_counts(
            compiled.resolved().components().size(), UINT64_C(0));
        std::uint64_t graph_object_count = UINT64_C(0);
        for (const std::uint32_t component_index :
             runtime.value().identity_components()) {
            const JsonPointer path = binary_region_path(region_index);
            auto charged = work.charge(UINT64_C(1), path);
            if (!charged.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(charged).error());
            }
            auto fields = read_region_fields(
                bytes, byte_count, region_index, path);
            if (!fields.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(fields).error());
            }
            const ComponentLayout* const component =
                runtime.value().component(component_index);
            if (component == nullptr) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, path,
                    "Graph component layout is missing",
                    "component_layout_missing"));
            }
            auto length = layout::checked_multiply_u64(
                fields.value().element_count, component->stride, path);
            if (!length.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(length).error());
            }
            auto aligned = layout::checked_align_up_u64(
                cursor, component->alignment,
                path.append("data_offset"));
            if (!aligned.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(aligned).error());
            }
            const RegionFields expected{
                static_cast<std::uint32_t>(
                    layout::RegionKind::object_values),
                UINT32_C(0), component_index, UINT32_MAX, aligned.value(),
                length.value(), fields.value().element_count,
                component->stride, component->alignment, UINT64_C(0)};
            auto valid = validate_region_fields(
                fields.value(), expected, path);
            if (!valid.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(valid).error());
            }
            auto padding = require_zero(
                bytes, byte_count, cursor, aligned.value(), work,
                path.append("data_offset"),
                "nonzero_inter_region_padding", true, true);
            if (!padding.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(padding).error());
            }
            auto end = layout::checked_range_end(
                fields.value().data_offset, fields.value().byte_length,
                byte_count, path.append("byte_length"));
            if (!end.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(end).error());
            }
            cursor = end.value();
            object_counts[component_index] = fields.value().element_count;
            auto sum = layout::checked_accumulate_u64(
                graph_object_count, fields.value().element_count, path);
            if (!sum.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(sum).error());
            }
            if (graph_object_count > limits.max_graph_objects) {
                return Result<PayloadIndex>::failure(resource_error(
                    path.append("element_count"), "graph_objects",
                    graph_object_count, limits.max_graph_objects));
            }
            auto object_work = work.charge(
                fields.value().element_count, path);
            if (!object_work.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(object_work).error());
            }
            pools.push_back(ObjectPoolState{
                component_index, fields.value()});
            output.object_pools_[component_index] = ObjectPoolMetadata{
                component_index, region_index, fields.value().data_offset,
                fields.value().element_count, component->stride,
                component->alignment};
            ++region_index;
        }

        ListPartitions list_partitions;
        list_partitions.indexes.assign(runtime.value().type_count(),
                                       UINT32_MAX);
        list_partitions.states.reserve(runtime.value().list_nodes().size());
        std::uint64_t total_list_elements = UINT64_C(0);
        for (const layout::ListNodeLayout& list :
             runtime.value().list_nodes()) {
            RegionFields validity{};
            std::uint32_t validity_region_index = UINT32_MAX;
            if (list.item_nullable) {
                const JsonPointer path = binary_region_path(region_index);
                auto charged = work.charge(UINT64_C(1), path);
                if (!charged.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(charged).error());
                }
                auto fields = read_region_fields(
                    bytes, byte_count, region_index, path);
                if (!fields.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(fields).error());
                }
                auto rounded = layout::checked_add_u64(
                    fields.value().element_count, UINT64_C(7), path);
                if (!rounded.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(rounded).error());
                }
                auto aligned = layout::checked_align_up_u64(
                    cursor, UINT32_C(1), path.append("data_offset"));
                if (!aligned.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(aligned).error());
                }
                const RegionFields expected{
                    static_cast<std::uint32_t>(
                        layout::RegionKind::list_validity),
                    UINT32_C(0), list.owner_runtime_type_id,
                    list.item_runtime_type_id, aligned.value(),
                    rounded.value() / UINT64_C(8),
                    fields.value().element_count, UINT32_C(0), UINT32_C(1),
                    UINT64_C(0)};
                auto valid = validate_region_fields(
                    fields.value(), expected, path);
                if (!valid.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(valid).error());
                }
                auto padding = require_zero(
                    bytes, byte_count, cursor, aligned.value(), work,
                    path.append("data_offset"),
                    "nonzero_inter_region_padding", true, true);
                if (!padding.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(padding).error());
                }
                auto end = layout::checked_range_end(
                    fields.value().data_offset,
                    fields.value().byte_length, byte_count,
                    path.append("byte_length"));
                if (!end.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(end).error());
                }
                validity = fields.value();
                validity_region_index = region_index;
                cursor = end.value();
                ++region_index;
            }

            const JsonPointer path = binary_region_path(region_index);
            auto charged = work.charge(UINT64_C(1), path);
            if (!charged.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(charged).error());
            }
            auto fields = read_region_fields(
                bytes, byte_count, region_index, path);
            if (!fields.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(fields).error());
            }
            auto expected_length = layout::checked_multiply_u64(
                fields.value().element_count, list.item_stride, path);
            if (!expected_length.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(expected_length).error());
            }
            auto aligned = layout::checked_align_up_u64(
                cursor, list.item_alignment, path.append("data_offset"));
            if (!aligned.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(aligned).error());
            }
            if (fields.value().data_offset % list.item_alignment !=
                UINT64_C(0)) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_MISALIGNED, path.append("data_offset"),
                    "Portable payload list item region offset is misaligned",
                    "region_offset_misaligned"));
            }
            const RegionFields expected{
                static_cast<std::uint32_t>(layout::RegionKind::list_items),
                UINT32_C(0), list.owner_runtime_type_id,
                list.item_runtime_type_id, aligned.value(),
                expected_length.value(), fields.value().element_count,
                list.item_stride, list.item_alignment, UINT64_C(0)};
            auto valid = validate_region_fields(fields.value(), expected, path);
            if (!valid.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(valid).error());
            }
            if (list.item_nullable &&
                validity.element_count != fields.value().element_count) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("element_count"),
                    "list_validity_element_count",
                    std::to_string(fields.value().element_count),
                    std::to_string(validity.element_count)));
            }
            auto padding = require_zero(
                bytes, byte_count, cursor, aligned.value(), work,
                path.append("data_offset"),
                "nonzero_inter_region_padding", true, true);
            if (!padding.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(padding).error());
            }
            auto end = layout::checked_range_end(
                fields.value().data_offset, fields.value().byte_length,
                byte_count, path.append("byte_length"));
            if (!end.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(end).error());
            }
            auto accumulated = layout::checked_accumulate_u64(
                total_list_elements, fields.value().element_count,
                path.append("element_count"));
            if (!accumulated.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(accumulated).error());
            }
            if (total_list_elements > limits.max_list_elements) {
                return Result<PayloadIndex>::failure(resource_error(
                    path.append("element_count"), "list_elements",
                    total_list_elements, limits.max_list_elements));
            }
            if (list.owner_runtime_type_id >=
                    list_partitions.indexes.size() ||
                list_partitions.indexes[list.owner_runtime_type_id] !=
                    UINT32_MAX) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL,
                    JsonPointer{}.append("runtime").append("lists"),
                    "Portable payload list runtime inventory is invalid",
                    "list_runtime_inventory_mismatch"));
            }
            list_partitions.indexes[list.owner_runtime_type_id] =
                static_cast<std::uint32_t>(list_partitions.states.size());
            list_partitions.states.push_back(ListRegionState{
                list.owner_runtime_type_id, list.item_runtime_type_id,
                validity_region_index, region_index, validity,
                fields.value(), UINT64_C(0)});
            output.list_slots_[list.owner_runtime_type_id] =
                ListSlotMetadata{
                    list.item_runtime_type_id, fields.value().data_offset,
                    fields.value().element_count, fields.value().stride,
                    validity_region_index != UINT32_MAX,
                    validity_region_index == UINT32_MAX
                        ? UINT64_C(0)
                        : validity.data_offset,
                    validity_region_index == UINT32_MAX
                        ? UINT64_C(0)
                        : validity.byte_length};
            cursor = end.value();
            ++region_index;
        }

        std::vector<PoolMetadata> variable_pools;
        variable_pools.reserve(3U);
        const std::array<layout::RegionKind, 3> pool_order{{
            layout::RegionKind::utf8_pool,
            layout::RegionKind::utf16_pool,
            layout::RegionKind::bytes_pool}};
        std::uint64_t text_pool_bytes = UINT64_C(0);
        for (const layout::RegionKind expected_kind : pool_order) {
            const bool required =
                expected_kind == layout::RegionKind::utf8_pool
                    ? runtime.value().has_utf8_pool()
                : expected_kind == layout::RegionKind::utf16_pool
                    ? runtime.value().has_utf16_pool()
                    : runtime.value().has_bytes_pool();
            if (!required) {
                continue;
            }
            const JsonPointer path = binary_region_path(region_index);
            auto charged = work.charge(UINT64_C(1), path);
            if (!charged.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(charged).error());
            }
            auto fields = read_region_fields(
                bytes, byte_count, region_index, path);
            if (!fields.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(fields).error());
            }
            const std::uint32_t expected_alignment =
                expected_kind == layout::RegionKind::utf16_pool
                    ? UINT32_C(2)
                    : UINT32_C(1);
            auto aligned = layout::checked_align_up_u64(
                cursor, expected_alignment, path.append("data_offset"));
            if (!aligned.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(aligned).error());
            }
            if (fields.value().data_offset % expected_alignment !=
                UINT64_C(0)) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_MISALIGNED, path.append("data_offset"),
                    "Portable payload pool offset is misaligned",
                    "region_offset_misaligned"));
            }
            if (expected_kind == layout::RegionKind::utf16_pool &&
                fields.value().byte_length % UINT64_C(2) != UINT64_C(0)) {
                return Result<PayloadIndex>::failure(noncanonical_exact(
                    path.append("byte_length"), "odd_utf16_pool_length",
                    std::to_string(fields.value().byte_length), "even"));
            }
            const std::uint64_t expected_elements =
                expected_kind == layout::RegionKind::utf16_pool
                    ? fields.value().byte_length / UINT64_C(2)
                    : fields.value().byte_length;
            const RegionFields expected{
                static_cast<std::uint32_t>(expected_kind), UINT32_C(0),
                UINT32_MAX, UINT32_MAX, aligned.value(),
                fields.value().byte_length, expected_elements, UINT32_C(0),
                expected_alignment, UINT64_C(0)};
            auto valid = validate_region_fields(fields.value(), expected, path);
            if (!valid.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(valid).error());
            }
            auto padding = require_zero(
                bytes, byte_count, cursor, aligned.value(), work,
                path.append("data_offset"),
                "nonzero_inter_region_padding", true, true);
            if (!padding.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(padding).error());
            }
            auto end = layout::checked_range_end(
                fields.value().data_offset, fields.value().byte_length,
                byte_count, path.append("byte_length"));
            if (!end.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(end).error());
            }
            if (expected_kind != layout::RegionKind::bytes_pool) {
                auto accumulated = layout::checked_accumulate_u64(
                    text_pool_bytes, fields.value().byte_length,
                    path.append("byte_length"));
                if (!accumulated.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(accumulated).error());
                }
                if (text_pool_bytes > limits.max_string_bytes) {
                    return Result<PayloadIndex>::failure(resource_error(
                        path.append("byte_length"), "string_bytes",
                        text_pool_bytes, limits.max_string_bytes));
                }
            }
            variable_pools.push_back(PoolMetadata{
                expected_kind, region_index, fields.value().data_offset,
                fields.value().byte_length, fields.value().element_count});
            cursor = end.value();
            ++region_index;
        }
        if (region_index != header.value().region_count) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                binary_header_path().append("region_count"),
                "region_inventory_not_consumed",
                std::to_string(header.value().region_count),
                std::to_string(region_index)));
        }
        auto canonical_total = layout::checked_align_up_u64(
            cursor, UINT32_C(8),
            binary_header_path().append("total_length"));
        if (!canonical_total.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(canonical_total).error());
        }
        if (canonical_total.value() != byte_count) {
            return Result<PayloadIndex>::failure(noncanonical_exact(
                binary_header_path().append("total_length"),
                "canonical_total_length_mismatch",
                std::to_string(byte_count),
                std::to_string(canonical_total.value())));
        }
        auto final_padding = require_zero(
            bytes, byte_count, cursor, canonical_total.value(), work,
            binary_header_path().append("total_length"),
            "nonzero_final_padding", true, true);
        if (!final_padding.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(final_padding).error());
        }

        for (const ListRegionState& list : list_partitions.states) {
            if (list.validity_region_index == UINT32_MAX) {
                continue;
            }
            auto tail = require_validity_tail(
                bytes, byte_count, list.validity, work,
                binary_region_path(list.validity_region_index));
            if (!tail.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(tail).error());
            }
        }

        PoolCursors pool_cursors;
        std::vector<VariableSlotMetadata> variables;
        for (const EntryState& entry : entry_states) {
            if (entry.has_validity) {
                auto tail = require_validity_tail(
                    bytes, byte_count, entry.validity, work,
                    binary_region_path(entry.descriptor.validity_region_index));
                if (!tail.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(tail).error());
                }
            }
            for (std::uint64_t index = UINT64_C(0);
                 index < entry.descriptor.value_count; ++index) {
                const bool present =
                    !entry.has_validity ||
                    validity_present(bytes, entry.validity, index);
                auto offset = layout::checked_multiply_u64(
                    index, entry.values.stride,
                    binary_entry_path(entry.descriptor.entry_index));
                if (!offset.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(offset).error());
                }
                auto absolute = layout::checked_add_u64(
                    entry.values.data_offset, offset.value(),
                    binary_entry_path(entry.descriptor.entry_index));
                if (!absolute.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(absolute).error());
                }
                const spec::Entry& source =
                    runtime.value().spec().resolved().entries()[
                        entry.descriptor.entry_index];
                auto valid = validate_slot_tree(
                    runtime.value(), bytes, byte_count, source.type,
                    absolute.value(), present, true, UINT64_C(0), object_counts,
                    variable_pools, pool_cursors, list_partitions, variables,
                    limits.validate_text_eager, work,
                    limits.max_nesting_depth,
                    binary_entry_path(entry.descriptor.entry_index)
                        .append(index));
                if (!valid.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(valid).error());
                }
            }
        }
        for (const ObjectPoolState& pool : pools) {
            for (std::uint64_t object_id = UINT64_C(0);
                 object_id < pool.values.element_count; ++object_id) {
                auto valid = validate_object_record(
                    runtime.value(), bytes, byte_count, pool, object_id,
                    object_counts, variable_pools, pool_cursors,
                    list_partitions, variables, limits.validate_text_eager,
                    work, limits.max_nesting_depth);
                if (!valid.has_value()) {
                    return Result<PayloadIndex>::failure(
                        std::move(valid).error());
                }
            }
        }
        for (const ListRegionState& list : list_partitions.states) {
            auto consumed = layout::require_partition_consumed(
                list.cursor, list.items.element_count,
                binary_region_path(list.items_region_index)
                    .append("element_count"));
            if (!consumed.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(consumed).error());
            }
        }
        for (const PoolMetadata& pool : variable_pools) {
            const TypeKind kind =
                pool.kind == layout::RegionKind::utf8_pool
                    ? TypeKind::str
                : pool.kind == layout::RegionKind::utf16_pool
                    ? TypeKind::wstr
                    : TypeKind::bytes;
            auto consumed = layout::require_partition_consumed(
                pool_cursors.for_kind(kind), pool.byte_length,
                binary_region_path(pool.region_index).append("byte_length"));
            if (!consumed.has_value()) {
                return Result<PayloadIndex>::failure(
                    std::move(consumed).error());
            }
        }
        auto reachable = validate_reachability(
            runtime.value(), bytes, byte_count, entry_states, pools,
            object_counts, graph_object_count, list_partitions, work);
        if (!reachable.has_value()) {
            return Result<PayloadIndex>::failure(
                std::move(reachable).error());
        }
        output.entries_.reserve(entry_states.size());
        for (const EntryState& entry : entry_states) {
            const RuntimeType* const type = runtime.value().find_type(
                entry.descriptor.runtime_type_id);
            if (type == nullptr || type->source == nullptr) {
                return Result<PayloadIndex>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("entries"),
                    "Graph entry runtime type metadata is missing",
                    "entry_runtime_type_missing"));
            }
            output.entries_.push_back(EntrySlotMetadata{
                entry.descriptor.runtime_type_id, type->source->kind,
                entry.values.data_offset, entry.descriptor.value_count,
                entry.values.stride, entry.has_validity,
                entry.has_validity ? entry.validity.data_offset : UINT64_C(0),
                entry.has_validity ? entry.validity.byte_length
                                   : UINT64_C(0)});
        }
        output.pools_ = std::move(variable_pools);
        output.variable_slots_ = std::move(variables);
        output.runtime_schema_ = std::make_shared<const RuntimeSchema>(
            std::move(runtime).value());
        output.total_length_ = byte_count;
        output.root_value_count_ = header.value().root_value_count;
        output.graph_object_count_ = graph_object_count;
        output.validation_work_ = work.value();
        output.region_count_ = header.value().region_count;
        output.retained_max_string_bytes_ = limits.max_string_bytes;
        output.retained_max_validation_work_ = limits.max_validation_work;
        output.text_validated_eagerly_ = limits.validate_text_eager;
        return Result<PayloadIndex>::success(std::move(output));
    } catch (const std::bad_alloc&) {
        return Result<PayloadIndex>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<PayloadIndex>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::view
