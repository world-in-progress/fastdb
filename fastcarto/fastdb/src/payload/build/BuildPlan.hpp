#pragma once

#include "payload/backing/Backing.hpp"
#include "payload/build/ExecutionReport.hpp"
#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/layout/RecordLayout.hpp"
#include "payload/view/PayloadOwner.hpp"

#include <cstdint>

namespace fastdb::payload::build {

struct PlanInfo final {
    std::uint64_t total_bytes;
    std::uint64_t region_count;
    std::uint64_t logical_value_count;
    std::uint64_t list_element_count;
    std::uint64_t text_bytes;
    std::uint64_t opaque_bytes;
    std::uint64_t validation_work;
    std::uint32_t max_alignment;
    std::uint32_t direct_build_status;
};

struct BuildPlanTestAccess;

class BuildPlan final {
public:
    static error::Result<BuildPlan> create(LogicalPayload&& values);

    BuildPlan(const BuildPlan&) = delete;
    BuildPlan& operator=(const BuildPlan&) = delete;
    BuildPlan(BuildPlan&&) noexcept = default;
    BuildPlan& operator=(BuildPlan&&) noexcept = default;
    ~BuildPlan() = default;

    const PlanInfo& info() const noexcept { return info_; }
    error::Result<view::PayloadOwner> execute(
        std::uint32_t policy,
        const backing::Callbacks* callbacks) const;

private:
    BuildPlan(LogicalPayload values,
              layout::RecordLayout record_layout,
              PlanInfo info) noexcept
        : values_(std::move(values)),
          record_layout_(std::move(record_layout)),
          info_(info) {}

    view::OpenOptions publication_options(const PlanInfo& info) const noexcept;

    friend struct BuildPlanTestAccess;

    LogicalPayload values_;
    layout::RecordLayout record_layout_;
    PlanInfo info_;
};

}  // namespace fastdb::payload::build
