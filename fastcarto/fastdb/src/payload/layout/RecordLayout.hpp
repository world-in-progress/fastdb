#pragma once

#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/RuntimeSchema.hpp"

#include <cstdint>
#include <vector>

namespace fastdb::payload::layout {

struct DescriptorFact final {
    build::NodeIndex node_index;
    std::uint32_t runtime_type_id;
    std::uint64_t first;
    std::uint64_t count;
};

struct ListAggregate final {
    std::uint32_t owner_runtime_type_id;
    std::uint32_t item_runtime_type_id;
    std::uint32_t validity_region_index;
    std::uint32_t items_region_index;
    std::vector<build::NodeIndex> item_nodes;
};

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
    const DescriptorFact* descriptor_fact(
        build::NodeIndex node_index) const noexcept {
        if (node_index >= descriptor_fact_indexes_.size()) {
            return nullptr;
        }
        const std::uint64_t fact_index =
            descriptor_fact_indexes_[static_cast<std::size_t>(node_index)];
        return fact_index < descriptor_facts_.size()
                   ? &descriptor_facts_[static_cast<std::size_t>(fact_index)]
                   : nullptr;
    }
    const std::vector<DescriptorFact>& descriptor_facts() const noexcept {
        return descriptor_facts_;
    }
    const ListAggregate* list_aggregate(
        std::uint32_t owner_runtime_type_id) const noexcept {
        if (owner_runtime_type_id >= list_aggregate_indexes_.size()) {
            return nullptr;
        }
        const std::uint32_t aggregate_index =
            list_aggregate_indexes_[owner_runtime_type_id];
        return aggregate_index < list_aggregates_.size()
                   ? &list_aggregates_[aggregate_index]
                   : nullptr;
    }
    const std::vector<ListAggregate>& list_aggregates() const noexcept {
        return list_aggregates_;
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
    std::vector<DescriptorFact> descriptor_facts_;
    std::vector<std::uint64_t> descriptor_fact_indexes_;
    std::vector<ListAggregate> list_aggregates_;
    std::vector<std::uint32_t> list_aggregate_indexes_;
    std::uint64_t total_length_{UINT64_C(0)};
    std::uint64_t root_value_count_{UINT64_C(0)};
    std::uint64_t validation_work_{UINT64_C(0)};
};

}  // namespace fastdb::payload::layout
