#pragma once

#include "payload/error/Result.hpp"
#include "payload/spec/CompiledSpec.hpp"

#include <cstdint>
#include <vector>

namespace fastdb::payload::view {

struct OpenLimits final {
    std::uint64_t max_total_bytes;
    std::uint64_t max_regions;
    std::uint64_t max_entries;
    std::uint64_t max_components;
    std::uint64_t max_nesting_depth;
    std::uint64_t max_list_elements;
    std::uint64_t max_graph_objects;
    std::uint64_t max_string_bytes;
    std::uint64_t max_validation_work;
};

OpenLimits default_open_limits() noexcept;

struct ObservedScalar final {
    bool present;
    spec::TypeKind kind;
    std::uint64_t bits;
};

class PayloadIndex final {
public:
    std::uint64_t total_length() const noexcept { return total_length_; }
    std::uint32_t entry_count() const noexcept {
        return static_cast<std::uint32_t>(entries_.size());
    }
    std::uint64_t validation_work() const noexcept {
        return validation_work_;
    }
    error::Result<ObservedScalar> scalar(std::uint32_t entry_index,
                                         std::uint64_t value_index) const;

private:
    friend error::Result<PayloadIndex> open_record(
        const spec::CompiledSpec&,
        const std::uint8_t*,
        std::uint64_t,
        OpenLimits);

    std::vector<std::vector<ObservedScalar>> entries_;
    std::uint64_t total_length_{UINT64_C(0)};
    std::uint64_t validation_work_{UINT64_C(0)};
};

error::Result<PayloadIndex> open_record(
    const spec::CompiledSpec& spec,
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    OpenLimits limits = default_open_limits());

}  // namespace fastdb::payload::view
