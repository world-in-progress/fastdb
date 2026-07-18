#pragma once

#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/RuntimeSchema.hpp"

#include <cstdint>
#include <vector>

namespace fastdb::payload::layout {

class RecordLayout final {
public:
    static error::Result<RecordLayout> plan(
        const RuntimeSchema& runtime_schema,
        const build::LogicalPayload& values);

    std::uint64_t total_length() const noexcept { return total_length_; }
    std::uint64_t root_value_count() const noexcept {
        return root_value_count_;
    }
    std::uint64_t validation_work() const noexcept {
        return validation_work_;
    }
    std::uint32_t region_count() const noexcept {
        return static_cast<std::uint32_t>(regions_.size());
    }
    std::uint32_t entry_count() const noexcept {
        return static_cast<std::uint32_t>(entries_.size());
    }
    const std::vector<RegionDescriptor>& regions() const noexcept {
        return regions_;
    }
    const std::vector<EntryDescriptor>& entries() const noexcept {
        return entries_;
    }
    const std::vector<std::vector<build::NodeIndex>>& entry_values()
        const noexcept {
        return entry_values_;
    }
    const std::vector<build::NodeIndex>& variable_values() const noexcept {
        return variable_values_;
    }
    const RuntimeSchema& runtime_schema() const noexcept {
        return runtime_schema_;
    }

private:
    explicit RecordLayout(RuntimeSchema runtime_schema)
        : runtime_schema_(std::move(runtime_schema)) {}

    RuntimeSchema runtime_schema_;
    std::vector<RegionDescriptor> regions_;
    std::vector<EntryDescriptor> entries_;
    std::vector<std::vector<build::NodeIndex>> entry_values_;
    std::vector<build::NodeIndex> variable_values_;
    std::uint64_t total_length_{UINT64_C(0)};
    std::uint64_t root_value_count_{UINT64_C(0)};
    std::uint64_t validation_work_{UINT64_C(0)};
};

}  // namespace fastdb::payload::layout
