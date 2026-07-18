#pragma once

#include "payload/backing/Backing.hpp"
#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"
#include "payload/layout/RecordLayout.hpp"

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

struct ExecutionReport final {
    std::uint32_t mode;
    std::uint32_t fallback_reason;
    std::uint64_t requested_bytes;
    std::uint64_t used_bytes;
    std::uint64_t staging_bytes;
    std::uint64_t region_count;
    std::uint64_t backing_capacity;
};

class PendingPayload final {
public:
    PendingPayload(const PendingPayload&) = delete;
    PendingPayload& operator=(const PendingPayload&) = delete;
    PendingPayload(PendingPayload&&) noexcept = default;
    PendingPayload& operator=(PendingPayload&&) noexcept = default;
    ~PendingPayload() = default;

    const std::uint8_t* readable_data() const noexcept {
        return backing_.readable_data();
    }
    std::uint64_t readable_size() const noexcept {
        return backing_.readable_size();
    }
    std::uint64_t backing_capacity() const noexcept {
        return backing_.capacity();
    }
    const ExecutionReport& execution_report() const noexcept {
        return report_;
    }
    backing::CommittedBacking take_backing() && noexcept {
        return std::move(backing_);
    }

private:
    friend class BuildPlan;

    PendingPayload(backing::CommittedBacking backing,
                   ExecutionReport report) noexcept
        : backing_(std::move(backing)), report_(report) {}

    backing::CommittedBacking backing_;
    ExecutionReport report_;
};

class BuildPlan final {
public:
    static error::Result<BuildPlan> create(LogicalPayload&& values);

    BuildPlan(const BuildPlan&) = delete;
    BuildPlan& operator=(const BuildPlan&) = delete;
    BuildPlan(BuildPlan&&) noexcept = default;
    BuildPlan& operator=(BuildPlan&&) noexcept = default;
    ~BuildPlan() = default;

    const PlanInfo& info() const noexcept { return info_; }
    error::Result<PendingPayload> execute(
        std::uint32_t policy,
        const backing::Callbacks* callbacks) const;

private:
    BuildPlan(LogicalPayload values,
              layout::RecordLayout record_layout,
              PlanInfo info) noexcept
        : values_(std::move(values)),
          record_layout_(std::move(record_layout)),
          info_(info) {}

    LogicalPayload values_;
    layout::RecordLayout record_layout_;
    PlanInfo info_;
};

}  // namespace fastdb::payload::build
