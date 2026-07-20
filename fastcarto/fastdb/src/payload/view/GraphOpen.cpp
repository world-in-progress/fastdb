#include "payload/view/GraphOpen.hpp"

#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/CheckedMath.hpp"
#include "payload/layout/InputSpan.hpp"
#include "payload/layout/RuntimeSchema.hpp"
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
#include <utility>
#include <variant>
#include <vector>

namespace fastdb::payload::view {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
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

bool fixed_graph_kind(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
    case TypeKind::u8n:
    case TypeKind::u16n:
    case TypeKind::f32:
    case TypeKind::f64:
    case TypeKind::component:
    case TypeKind::ref:
        return true;
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
    case TypeKind::list:
        return false;
    }
    return false;
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

struct PendingSlot final {
    std::uint32_t runtime_type_id;
    std::uint64_t offset;
    std::uint64_t container_depth;
    bool present;
};

struct PendingComponent final {
    const ComponentLayout* component;
    std::uint64_t offset;
    std::uint64_t component_depth;
    std::size_t next_field;
    std::uint64_t cursor;
};

using Pending = std::variant<PendingSlot, PendingComponent>;

Result<void> validate_slot_tree(
    const RuntimeSchema& runtime,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint32_t root_runtime_type_id,
    std::uint64_t root_offset,
    bool root_present,
    bool root_already_charged,
    std::uint64_t root_container_depth,
    const std::vector<std::uint64_t>& object_counts,
    WorkCounter& work,
    std::uint64_t max_nesting_depth,
    const JsonPointer& path) {
    std::vector<Pending> pending;
    pending.push_back(PendingSlot{
        root_runtime_type_id, root_offset, root_container_depth,
        root_present});
    bool first = true;
    while (!pending.empty()) {
        if (auto* component = std::get_if<PendingComponent>(&pending.back())) {
            if (component->next_field == component->component->fields.size()) {
                auto tail = require_zero(
                    bytes, byte_count, component->offset + component->cursor,
                    component->offset + component->component->stride, work,
                    path, "nonzero_component_padding", true, false);
                if (!tail.has_value()) {
                    return tail;
                }
                pending.pop_back();
                continue;
            }
            const ComponentFieldLayout& field =
                component->component->fields[component->next_field++];
            auto padding = require_zero(
                bytes, byte_count, component->offset + component->cursor,
                component->offset + field.offset, work, path,
                "nonzero_component_padding", true, false);
            if (!padding.has_value()) {
                return padding;
            }
            const bool present =
                field.validity_bit == UINT32_MAX ||
                ((bytes[static_cast<std::ptrdiff_t>(
                       component->offset +
                       field.validity_bit / UINT32_C(8))] >>
                  (field.validity_bit % UINT32_C(8))) &
                 UINT8_C(1)) != UINT8_C(0);
            auto end = layout::checked_add_u64(
                field.offset, field.slot_stride, path);
            if (!end.has_value()) {
                return Result<void>::failure(std::move(end).error());
            }
            component->cursor = end.value();
            pending.push_back(PendingSlot{
                field.runtime_type_id, component->offset + field.offset,
                component->component_depth, present});
            continue;
        }

        const PendingSlot slot = std::get<PendingSlot>(pending.back());
        pending.pop_back();
        if (!first || !root_already_charged) {
            auto charged = work.charge(UINT64_C(1), path);
            if (!charged.has_value()) {
                return charged;
            }
        }
        first = false;
        const RuntimeType* const type =
            runtime.find_type(slot.runtime_type_id);
        if (type == nullptr || !fixed_graph_kind(type->source->kind)) {
            return Result<void>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, path,
                "Graph open reached a value outside the initial fixed slice",
                "initial_graph_binary_slice_unavailable"));
        }
        auto slot_end = layout::checked_range_end(
            slot.offset, type->slot.stride, byte_count, path);
        if (!slot_end.has_value()) {
            return Result<void>::failure(std::move(slot_end).error());
        }
        if (!slot.present) {
            if (!type->source->nullable) {
                return Result<void>::failure(noncanonical(
                    path, "null_nonnullable_value"));
            }
            const bool nested =
                type->storage_role == StorageRole::inline_component;
            auto zero = require_zero(
                bytes, byte_count, slot.offset, slot_end.value(), work, path,
                "nonzero_null_slot", nested, false);
            if (!zero.has_value()) {
                return zero;
            }
            continue;
        }

        if (type->storage_role == StorageRole::object_root_id ||
            type->storage_role == StorageRole::reference_id) {
            auto object_id = read_u64(bytes, byte_count, slot.offset, path);
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
                    FDB_PAYLOAD_E_INVALID_REFERENCE, path,
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

        if (type->storage_role == StorageRole::inline_component) {
            auto component_depth = layout::checked_add_u64(
                slot.container_depth, UINT64_C(1), path);
            if (!component_depth.has_value()) {
                return Result<void>::failure(
                    std::move(component_depth).error());
            }
            if (component_depth.value() > max_nesting_depth) {
                return Result<void>::failure(resource_error(
                    path, "nesting_depth", component_depth.value(),
                    max_nesting_depth));
            }
            const ComponentLayout* const component = runtime.component(
                type->source->resolved_component_index);
            if (component == nullptr) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, path,
                    "Graph component layout is missing",
                    "component_layout_missing"));
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
                    const std::uint8_t tail = bytes[
                        static_cast<std::ptrdiff_t>(
                            slot.offset + component->validity_bytes -
                            UINT32_C(1))];
                    if ((tail & static_cast<std::uint8_t>(
                                    UINT8_C(0xff) << used)) != UINT8_C(0)) {
                        return Result<void>::failure(noncanonical(
                            path, "nonzero_component_validity_tail"));
                    }
                }
            }
            pending.push_back(PendingComponent{
                component, slot.offset, component_depth.value(), 0U,
                component->validity_bytes});
            continue;
        }

        switch (type->source->kind) {
        case TypeKind::boolean:
            if (bytes[static_cast<std::ptrdiff_t>(slot.offset)] > UINT8_C(1)) {
                return Result<void>::failure(simple_error(
                    FDB_PAYLOAD_E_INVALID_BINARY_VALUE, path,
                    "Portable payload binary value is invalid",
                    "invalid_boolean"));
            }
            break;
        case TypeKind::f32: {
            auto bits = layout::load_u32_le(
                bytes, byte_count, slot.offset, path);
            if (!bits.has_value()) {
                return Result<void>::failure(std::move(bits).error());
            }
            if (noncanonical_f32_nan(bits.value())) {
                return Result<void>::failure(noncanonical(
                    path, "noncanonical_f32_nan"));
            }
            break;
        }
        case TypeKind::f64: {
            auto bits = layout::load_u64_le(
                bytes, byte_count, slot.offset, path);
            if (!bits.has_value()) {
                return Result<void>::failure(std::move(bits).error());
            }
            if (noncanonical_f64_nan(bits.value())) {
                return Result<void>::failure(noncanonical(
                    path, "noncanonical_f64_nan"));
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
                FDB_PAYLOAD_E_INTERNAL, path,
                "Graph open reached a value outside the initial fixed slice",
                "initial_graph_binary_slice_unavailable"));
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
    for (const ComponentFieldLayout& field : component->fields) {
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
            runtime, bytes, byte_count, field.runtime_type_id,
            absolute.value() + field.offset, present, false, UINT64_C(1),
            object_counts, work, max_nesting_depth, path);
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

Result<GraphOpenFacts> open_graph(const spec::CompiledSpec& compiled,
                                  const std::uint8_t* bytes,
                                  std::uint64_t byte_count,
                                  OpenOptions limits) {
    try {
        if (compiled.profile() != Profile::object_graph_v1) {
            return Result<GraphOpenFacts>::failure(simple_error(
                FDB_PAYLOAD_E_PROFILE_VIOLATION, JsonPointer{},
                "Graph open requires object_graph.v1",
                "graph_profile_required"));
        }
        const std::uint64_t expected_entries =
            compiled.resolved().entries().size();
        const std::uint64_t expected_components =
            compiled.resolved().components().size();
        if (expected_entries > limits.max_entries) {
            return Result<GraphOpenFacts>::failure(resource_error(
                binary_header_path().append("entry_count"), "entries",
                expected_entries, limits.max_entries));
        }
        if (expected_components > limits.max_components) {
            return Result<GraphOpenFacts>::failure(resource_error(
                binary_header_path(), "components", expected_components,
                limits.max_components));
        }
        auto runtime = RuntimeSchema::compile(compiled);
        if (!runtime.has_value()) {
            return Result<GraphOpenFacts>::failure(
                std::move(runtime).error());
        }
        if (!runtime.value().list_nodes().empty() ||
            runtime.value().has_utf8_pool() ||
            runtime.value().has_utf16_pool() ||
            runtime.value().has_bytes_pool()) {
            return Result<GraphOpenFacts>::failure(simple_error(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{},
                "Graph open reached a value outside the initial fixed slice",
                "initial_graph_binary_slice_unavailable"));
        }
        for (std::uint32_t type_id = UINT32_C(0);
             type_id < runtime.value().type_count(); ++type_id) {
            const RuntimeType* const type = runtime.value().find_type(type_id);
            if (type != nullptr && type->reachable &&
                !fixed_graph_kind(type->source->kind)) {
                return Result<GraphOpenFacts>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, JsonPointer{},
                    "Graph open reached a value outside the initial fixed "
                    "slice",
                    "initial_graph_binary_slice_unavailable"));
            }
        }

        std::uint64_t expected_regions = expected_entries;
        for (const spec::Entry& entry : compiled.resolved().entries()) {
            if (entry.type.nullable) {
                auto added = layout::checked_accumulate_u64(
                    expected_regions, UINT64_C(1),
                    JsonPointer{}.append("regions"));
                if (!added.has_value()) {
                    return Result<GraphOpenFacts>::failure(
                        std::move(added).error());
                }
            }
        }
        auto added = layout::checked_accumulate_u64(
            expected_regions, runtime.value().identity_components().size(),
            JsonPointer{}.append("regions"));
        if (!added.has_value()) {
            return Result<GraphOpenFacts>::failure(std::move(added).error());
        }
        if (expected_regions > limits.max_regions) {
            return Result<GraphOpenFacts>::failure(resource_error(
                binary_header_path().append("region_count"), "regions",
                expected_regions, limits.max_regions));
        }

        WorkCounter work(limits.max_validation_work);
        auto header = validate_header(
            compiled, bytes, byte_count, expected_regions, limits, work);
        if (!header.has_value()) {
            return Result<GraphOpenFacts>::failure(
                std::move(header).error());
        }
        auto entries = read_entries(
            runtime.value(), bytes, byte_count, header.value(), work);
        if (!entries.has_value()) {
            return Result<GraphOpenFacts>::failure(
                std::move(entries).error());
        }
        auto data_start = layout::checked_align_up_u64(
            header.value().entry_end, UINT32_C(8),
            binary_header_path().append("entry_directory_offset"));
        if (!data_start.has_value()) {
            return Result<GraphOpenFacts>::failure(
                std::move(data_start).error());
        }
        auto directory_padding = require_zero(
            bytes, byte_count, header.value().entry_end,
            data_start.value(), work,
            binary_header_path().append("entry_directory_offset"),
            "nonzero_directory_padding", true, true);
        if (!directory_padding.has_value()) {
            return Result<GraphOpenFacts>::failure(
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
                return Result<GraphOpenFacts>::failure(simple_error(
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
                    return Result<GraphOpenFacts>::failure(
                        std::move(charged).error());
                }
                auto fields = read_region_fields(
                    bytes, byte_count, region_index, path);
                if (!fields.has_value()) {
                    return Result<GraphOpenFacts>::failure(
                        std::move(fields).error());
                }
                const std::uint32_t alignment =
                    validity ? UINT32_C(1) : type->slot.alignment;
                const std::uint32_t stride =
                    validity ? UINT32_C(0) : type->slot.stride;
                auto aligned = layout::checked_align_up_u64(
                    cursor, alignment, path.append("data_offset"));
                if (!aligned.has_value()) {
                    return Result<GraphOpenFacts>::failure(
                        std::move(aligned).error());
                }
                auto length = layout::checked_multiply_u64(
                    entry.value_count, type->slot.stride, path);
                if (validity) {
                    auto rounded = layout::checked_add_u64(
                        entry.value_count, UINT64_C(7), path);
                    if (!rounded.has_value()) {
                        return Result<GraphOpenFacts>::failure(
                            std::move(rounded).error());
                    }
                    length = Result<std::uint64_t>::success(
                        rounded.value() / UINT64_C(8));
                }
                if (!length.has_value()) {
                    return Result<GraphOpenFacts>::failure(
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
                    return Result<GraphOpenFacts>::failure(
                        std::move(valid).error());
                }
                auto padding = require_zero(
                    bytes, byte_count, cursor, aligned.value(), work,
                    path.append("data_offset"),
                    "nonzero_inter_region_padding", true, true);
                if (!padding.has_value()) {
                    return Result<GraphOpenFacts>::failure(
                        std::move(padding).error());
                }
                auto end = layout::checked_range_end(
                    fields.value().data_offset,
                    fields.value().byte_length, byte_count,
                    path.append("byte_length"));
                if (!end.has_value()) {
                    return Result<GraphOpenFacts>::failure(
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
                        return Result<GraphOpenFacts>::failure(
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
                return Result<GraphOpenFacts>::failure(
                    std::move(charged).error());
            }
            auto fields = read_region_fields(
                bytes, byte_count, region_index, path);
            if (!fields.has_value()) {
                return Result<GraphOpenFacts>::failure(
                    std::move(fields).error());
            }
            const ComponentLayout* const component =
                runtime.value().component(component_index);
            if (component == nullptr) {
                return Result<GraphOpenFacts>::failure(simple_error(
                    FDB_PAYLOAD_E_INTERNAL, path,
                    "Graph component layout is missing",
                    "component_layout_missing"));
            }
            auto length = layout::checked_multiply_u64(
                fields.value().element_count, component->stride, path);
            if (!length.has_value()) {
                return Result<GraphOpenFacts>::failure(
                    std::move(length).error());
            }
            auto aligned = layout::checked_align_up_u64(
                cursor, component->alignment,
                path.append("data_offset"));
            if (!aligned.has_value()) {
                return Result<GraphOpenFacts>::failure(
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
                return Result<GraphOpenFacts>::failure(
                    std::move(valid).error());
            }
            auto padding = require_zero(
                bytes, byte_count, cursor, aligned.value(), work,
                path.append("data_offset"),
                "nonzero_inter_region_padding", true, true);
            if (!padding.has_value()) {
                return Result<GraphOpenFacts>::failure(
                    std::move(padding).error());
            }
            auto end = layout::checked_range_end(
                fields.value().data_offset, fields.value().byte_length,
                byte_count, path.append("byte_length"));
            if (!end.has_value()) {
                return Result<GraphOpenFacts>::failure(
                    std::move(end).error());
            }
            cursor = end.value();
            object_counts[component_index] = fields.value().element_count;
            auto sum = layout::checked_accumulate_u64(
                graph_object_count, fields.value().element_count, path);
            if (!sum.has_value()) {
                return Result<GraphOpenFacts>::failure(
                    std::move(sum).error());
            }
            if (graph_object_count > limits.max_graph_objects) {
                return Result<GraphOpenFacts>::failure(resource_error(
                    path.append("element_count"), "graph_objects",
                    graph_object_count, limits.max_graph_objects));
            }
            auto object_work = work.charge(
                fields.value().element_count, path);
            if (!object_work.has_value()) {
                return Result<GraphOpenFacts>::failure(
                    std::move(object_work).error());
            }
            pools.push_back(ObjectPoolState{
                component_index, fields.value()});
            ++region_index;
        }
        if (region_index != header.value().region_count) {
            return Result<GraphOpenFacts>::failure(noncanonical_exact(
                binary_header_path().append("region_count"),
                "region_inventory_not_consumed",
                std::to_string(header.value().region_count),
                std::to_string(region_index)));
        }
        auto canonical_total = layout::checked_align_up_u64(
            cursor, UINT32_C(8),
            binary_header_path().append("total_length"));
        if (!canonical_total.has_value()) {
            return Result<GraphOpenFacts>::failure(
                std::move(canonical_total).error());
        }
        if (canonical_total.value() != byte_count) {
            return Result<GraphOpenFacts>::failure(noncanonical_exact(
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
            return Result<GraphOpenFacts>::failure(
                std::move(final_padding).error());
        }

        for (const EntryState& entry : entry_states) {
            if (entry.has_validity) {
                auto tail = require_validity_tail(
                    bytes, byte_count, entry.validity, work,
                    binary_region_path(entry.descriptor.validity_region_index));
                if (!tail.has_value()) {
                    return Result<GraphOpenFacts>::failure(
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
                    return Result<GraphOpenFacts>::failure(
                        std::move(offset).error());
                }
                auto absolute = layout::checked_add_u64(
                    entry.values.data_offset, offset.value(),
                    binary_entry_path(entry.descriptor.entry_index));
                if (!absolute.has_value()) {
                    return Result<GraphOpenFacts>::failure(
                        std::move(absolute).error());
                }
                auto valid = validate_slot_tree(
                    runtime.value(), bytes, byte_count,
                    entry.descriptor.runtime_type_id, absolute.value(),
                    present, true, UINT64_C(0), object_counts, work,
                    limits.max_nesting_depth,
                    binary_entry_path(entry.descriptor.entry_index)
                        .append(index));
                if (!valid.has_value()) {
                    return Result<GraphOpenFacts>::failure(
                        std::move(valid).error());
                }
            }
        }
        for (const ObjectPoolState& pool : pools) {
            for (std::uint64_t object_id = UINT64_C(0);
                 object_id < pool.values.element_count; ++object_id) {
                auto valid = validate_object_record(
                    runtime.value(), bytes, byte_count, pool, object_id,
                    object_counts, work, limits.max_nesting_depth);
                if (!valid.has_value()) {
                    return Result<GraphOpenFacts>::failure(
                        std::move(valid).error());
                }
            }
        }
        return Result<GraphOpenFacts>::success(GraphOpenFacts{
            byte_count, header.value().root_value_count,
            graph_object_count, work.value(), header.value().region_count,
            header.value().entry_count});
    } catch (const std::bad_alloc&) {
        return Result<GraphOpenFacts>::failure(allocation_error());
    } catch (const std::length_error&) {
        return Result<GraphOpenFacts>::failure(allocation_error());
    }
}

}  // namespace fastdb::payload::view
