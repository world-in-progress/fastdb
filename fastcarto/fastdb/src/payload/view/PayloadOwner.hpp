#pragma once

#include "payload/backing/Backing.hpp"
#include "payload/build/ExecutionReport.hpp"
#include "payload/error/Result.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Open.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace fastdb::payload::build {
class BuildPlan;
}

namespace fastdb::payload::view {

struct PayloadOwnerTestAccess;

class PayloadOwner final {
public:
    static error::Result<PayloadOwner> open_copy(
        spec::CompiledSpec spec,
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        OpenOptions options = default_open_options());
    static error::Result<PayloadOwner> open_external(
        spec::CompiledSpec spec,
        const std::uint8_t* bytes,
        std::uint64_t byte_count,
        backing::RetainedBacking retained,
        OpenOptions options = default_open_options());

    PayloadOwner(const PayloadOwner&) noexcept = default;
    PayloadOwner& operator=(const PayloadOwner&) noexcept = default;
    PayloadOwner(PayloadOwner&&) noexcept = default;
    PayloadOwner& operator=(PayloadOwner&&) noexcept = default;
    ~PayloadOwner() = default;

    const std::array<std::uint8_t, 32>& digest() const noexcept {
        return state_->spec.digest();
    }
    spec::Profile profile() const noexcept { return state_->spec.profile(); }
    const std::optional<build::ExecutionReport>& execution_report()
        const noexcept {
        return state_->report;
    }

private:
    struct State final {
        State(backing::CommittedBacking backing_value,
              spec::CompiledSpec spec_value,
              PayloadIndex index_value,
              std::optional<build::ExecutionReport> report_value) noexcept
            : backing(std::move(backing_value)),
              spec(std::move(spec_value)),
              index(std::move(index_value)),
              report(std::move(report_value)) {}

        backing::CommittedBacking backing;
        spec::CompiledSpec spec;
        PayloadIndex index;
        std::optional<build::ExecutionReport> report;
    };

    explicit PayloadOwner(std::shared_ptr<State> state) noexcept
        : state_(std::move(state)) {}

    static error::Result<PayloadOwner> publish(
        backing::CommittedBacking backing,
        spec::CompiledSpec spec,
        PayloadIndex index,
        std::optional<build::ExecutionReport> report);

    friend class build::BuildPlan;
    friend struct PayloadOwnerTestAccess;

    std::shared_ptr<State> state_;
};

}  // namespace fastdb::payload::view
