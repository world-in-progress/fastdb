#include "payload/build/BuildPlan.hpp"

#include "payload/backing/HeapBacking.hpp"
#include "payload/build/GraphEncoder.hpp"
#include "payload/build/RecordEncoder.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"
#include "payload/layout/CheckedMath.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace fastdb::payload::build {
namespace {

using error::Error;
using error::Result;
using json::JsonPointer;
using json::JsonValue;
using layout::GraphLayout;
using layout::RecordLayout;
using layout::RegionKind;
using layout::RuntimeSchema;

constexpr std::uint32_t success_status = UINT32_C(0);

Error allocation_error() {
    return Error::from_details(
        FDB_PAYLOAD_E_ALLOCATION_FAILED, JsonPointer{},
        "Portable payload plan allocation failed",
        JsonValue::object({JsonValue::Member{
            "reason", JsonValue{"allocation_failed"}}}));
}

Error backing_error(std::uint32_t code, const char* reason) {
    return Error::from_details(
        code, JsonPointer{}.append("backing"),
        "Portable payload backing execution failed",
        JsonValue::object({JsonValue::Member{"reason", JsonValue{reason}}}));
}

Error callback_contract_error(const char* callback,
                              std::uint32_t status) {
    return Error::from_details(
        FDB_PAYLOAD_E_BACKING_CONTRACT,
        JsonPointer{}.append("backing"),
        "Portable payload backing callback returned an unknown status",
        JsonValue::object({
            JsonValue::Member{"callback", JsonValue{callback}},
            JsonValue::Member{"callback_status",
                              JsonValue{static_cast<double>(status)}},
        }));
}

Error committed_image_validation_error(const Error& original) {
    return Error::from_details(
        FDB_PAYLOAD_E_BACKING_CONTRACT,
        JsonPointer{}.append("backing"),
        "Committed portable payload image failed validation",
        JsonValue::object({
            JsonValue::Member{
                "original_code",
                JsonValue{static_cast<double>(original.code())}},
            JsonValue::Member{
                "original_details_json",
                JsonValue{std::string(original.details_json())}},
            JsonValue::Member{
                "original_path", JsonValue{std::string(original.path())}},
            JsonValue::Member{
                "original_symbol",
                JsonValue{std::string(original.symbol())}},
            JsonValue::Member{
                "reason",
                JsonValue{"committed_image_validation_failed"}},
        }));
}

Error rollback_failure(const Error& original, std::uint32_t status) {
    const std::uint32_t classified = backing::classify_callback_status(
        backing::CallbackOperation::rollback, status);
    const bool known = classified == FDB_PAYLOAD_E_ROLLBACK_FAILED;
    JsonValue::Object details{
        JsonValue::Member{"original_code",
                          JsonValue{static_cast<double>(original.code())}},
        JsonValue::Member{"original_symbol",
                          JsonValue{std::string(original.symbol())}},
        JsonValue::Member{"rollback_status",
                          JsonValue{static_cast<double>(status)}},
    };
    if (!known) {
        details.push_back(
            JsonValue::Member{"callback", JsonValue{"rollback"}});
        details.push_back(JsonValue::Member{
            "callback_status", JsonValue{static_cast<double>(status)}});
    }
    return Error::from_details(
        classified,
        JsonPointer{}.append("backing").append("rollback"),
        "Portable payload backing rollback failed",
        JsonValue::object(std::move(details)));
}

class CommittedImage final {
public:
    CommittedImage(backing::CommittedBacking backing_value,
                   ExecutionReport report_value) noexcept
        : backing(std::move(backing_value)), report(report_value) {}

    CommittedImage(const CommittedImage&) = delete;
    CommittedImage& operator=(const CommittedImage&) = delete;
    CommittedImage(CommittedImage&&) noexcept = default;
    CommittedImage& operator=(CommittedImage&&) noexcept = default;
    ~CommittedImage() = default;

    backing::CommittedBacking backing;
    ExecutionReport report;
};

Result<CommittedImage> fail_reserved(
    backing::BackingReservation& reservation,
    Error original) {
    const std::uint32_t rollback_status = reservation.rollback();
    if (rollback_status == success_status) {
        return Result<CommittedImage>::failure(std::move(original));
    }
    return Result<CommittedImage>::failure(
        rollback_failure(original, rollback_status));
}

class StableSpanSink final : public ByteSink {
public:
    StableSpanSink(std::uint8_t* data, std::uint64_t capacity) noexcept
        : data_(data), capacity_(capacity) {}

    Result<void> write(std::uint64_t offset,
                       const std::uint8_t* data,
                       std::uint64_t size) override {
        if ((data == nullptr && size != UINT64_C(0)) ||
            offset != next_offset_ || offset > capacity_ ||
            size > capacity_ - offset) {
            return Result<void>::failure(
                backing_error(FDB_PAYLOAD_E_BACKING_CONTRACT,
                              "direct_write_out_of_bounds"));
        }
        if (size != UINT64_C(0)) {
            std::memcpy(data_ + offset, data,
                        static_cast<std::size_t>(size));
        }
        auto next = layout::checked_add_u64(
            next_offset_, size,
            JsonPointer{}.append("backing").append("write"));
        if (!next.has_value()) {
            return Result<void>::failure(std::move(next).error());
        }
        next_offset_ = next.value();
        return Result<void>::success();
    }

    std::uint64_t bytes_written() const noexcept { return next_offset_; }

private:
    std::uint8_t* data_;
    std::uint64_t capacity_;
    std::uint64_t next_offset_{UINT64_C(0)};
};

class RangeCallbackSink final : public ByteSink {
public:
    RangeCallbackSink(backing::BackingReservation& reservation,
                      std::uint64_t capacity) noexcept
        : reservation_(reservation), capacity_(capacity) {}

    Result<void> write(std::uint64_t offset,
                       const std::uint8_t* data,
                       std::uint64_t size) override {
        if ((data == nullptr && size != UINT64_C(0)) ||
            offset != next_offset_ || offset > capacity_ ||
            size > capacity_ - offset) {
            return Result<void>::failure(
                backing_error(FDB_PAYLOAD_E_BACKING_CONTRACT,
                              "range_write_out_of_bounds"));
        }
        std::uint64_t consumed = UINT64_C(0);
        while (consumed < size) {
            const std::uint64_t chunk = std::min<std::uint64_t>(
                size - consumed, UINT64_C(65536));
            auto chunk_offset = layout::checked_add_u64(
                offset, consumed,
                JsonPointer{}.append("backing").append("write"));
            if (!chunk_offset.has_value()) {
                return Result<void>::failure(
                    std::move(chunk_offset).error());
            }
            const std::uint32_t status = reservation_.write(
                chunk_offset.value(),
                data + static_cast<std::size_t>(consumed), chunk);
            const std::uint32_t classified =
                backing::classify_callback_status(
                    backing::CallbackOperation::write, status);
            if (classified != success_status) {
                if (classified == FDB_PAYLOAD_E_ALLOCATION_FAILED) {
                    return Result<void>::failure(backing_error(
                        FDB_PAYLOAD_E_ALLOCATION_FAILED,
                        "write_allocation_failed"));
                }
                return Result<void>::failure(
                    callback_contract_error("write", status));
            }
            auto advanced = layout::checked_add_u64(
                consumed, chunk,
                JsonPointer{}.append("backing").append("write"));
            if (!advanced.has_value()) {
                return Result<void>::failure(
                    std::move(advanced).error());
            }
            consumed = advanced.value();
        }
        auto next = layout::checked_add_u64(
            next_offset_, size,
            JsonPointer{}.append("backing").append("write"));
        if (!next.has_value()) {
            return Result<void>::failure(std::move(next).error());
        }
        next_offset_ = next.value();
        return Result<void>::success();
    }

    std::uint64_t bytes_written() const noexcept { return next_offset_; }

private:
    backing::BackingReservation& reservation_;
    std::uint64_t capacity_;
    std::uint64_t next_offset_{UINT64_C(0)};
};

Result<void> encode_profile(const ProfileLayout& profile_layout,
                            const LogicalPayload& values,
                            ByteSink& sink) {
    return std::visit(
        [&](const auto& selected_layout) -> Result<void> {
            using Layout = std::decay_t<decltype(selected_layout)>;
            if constexpr (std::is_same_v<Layout, RecordLayout>) {
                return encode_record(selected_layout, values, sink);
            } else {
                static_assert(std::is_same_v<Layout, GraphLayout>);
                return encode_graph(selected_layout, values, sink);
            }
        },
        profile_layout);
}

}  // namespace

Result<BuildPlan> BuildPlan::create(LogicalPayload&& values) try {
    auto runtime = RuntimeSchema::compile(values.spec());
    if (!runtime.has_value()) {
        return Result<BuildPlan>::failure(std::move(runtime).error());
    }

    const auto finalize = [&values](auto selected_layout)
        -> Result<BuildPlan> {
        using Layout = std::decay_t<decltype(selected_layout)>;
        std::uint64_t list_elements = UINT64_C(0);
        for (const layout::ListAggregate& aggregate :
             selected_layout.list_aggregates()) {
            auto added = layout::checked_add_u64(
                list_elements,
                static_cast<std::uint64_t>(aggregate.item_nodes.size()),
                JsonPointer{}.append("plan").append(
                    "list_element_count"));
            if (!added.has_value()) {
                return Result<BuildPlan>::failure(
                    std::move(added).error());
            }
            list_elements = added.value();
        }

        std::uint64_t text_bytes = UINT64_C(0);
        std::uint64_t opaque_bytes = UINT64_C(0);
        std::uint32_t max_alignment = UINT32_C(1);
        for (const layout::RegionDescriptor& region :
             selected_layout.regions()) {
            max_alignment = std::max(max_alignment, region.alignment);
            if (region.kind == RegionKind::utf8_pool ||
                region.kind == RegionKind::utf16_pool) {
                auto added = layout::checked_add_u64(
                    text_bytes, region.byte_length,
                    JsonPointer{}.append("plan").append("text_bytes"));
                if (!added.has_value()) {
                    return Result<BuildPlan>::failure(
                        std::move(added).error());
                }
                text_bytes = added.value();
            } else if (region.kind == RegionKind::bytes_pool) {
                auto added = layout::checked_add_u64(
                    opaque_bytes, region.byte_length,
                    JsonPointer{}.append("plan").append("opaque_bytes"));
                if (!added.has_value()) {
                    return Result<BuildPlan>::failure(
                        std::move(added).error());
                }
                opaque_bytes = added.value();
            }
        }

        const std::uint64_t graph_object_count = [&] {
            if constexpr (std::is_same_v<Layout, GraphLayout>) {
                return selected_layout.graph_object_count();
            }
            return UINT64_C(0);
        }();
        PlanInfo info{
            selected_layout.total_length(),
            static_cast<std::uint64_t>(selected_layout.region_count()),
            static_cast<std::uint64_t>(values.nodes().size()),
            list_elements,
            text_bytes,
            opaque_bytes,
            selected_layout.validation_work(),
            max_alignment,
            FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE,
            graph_object_count,
        };
        return Result<BuildPlan>::success(BuildPlan(
            std::move(values),
            ProfileLayout{std::move(selected_layout)}, info));
    };

    if (values.spec().profile() == spec::Profile::record_v1) {
        auto record_layout = RecordLayout::plan(runtime.value(), values);
        if (!record_layout.has_value()) {
            return Result<BuildPlan>::failure(
                std::move(record_layout).error());
        }
        return finalize(std::move(record_layout).value());
    }
    auto graph_layout = GraphLayout::plan(runtime.value(), values);
    if (!graph_layout.has_value()) {
        return Result<BuildPlan>::failure(
            std::move(graph_layout).error());
    }
    return finalize(std::move(graph_layout).value());
} catch (const std::bad_alloc&) {
    return Result<BuildPlan>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<BuildPlan>::failure(allocation_error());
}

view::OpenOptions BuildPlan::publication_options(
    const PlanInfo& info) const noexcept {
    view::OpenOptions options = view::default_open_options();
    options.max_total_bytes =
        std::max(options.max_total_bytes, info.total_bytes);
    options.max_regions = std::max(options.max_regions, info.region_count);
    options.max_entries = std::max(
        options.max_entries,
        static_cast<std::uint64_t>(
            values_.spec().resolved().entries().size()));
    options.max_components = std::max(
        options.max_components,
        static_cast<std::uint64_t>(
            values_.spec().resolved().components().size()));
    options.max_nesting_depth =
        std::max(options.max_nesting_depth, info.logical_value_count);
    options.max_list_elements =
        std::max(options.max_list_elements, info.list_element_count);
    options.max_graph_objects =
        std::max(options.max_graph_objects, info.graph_object_count);
    options.max_string_bytes =
        std::max(options.max_string_bytes, info.text_bytes);
    options.max_validation_work =
        std::max(options.max_validation_work, info.validation_work);
    options.validate_text_eager = true;
    return options;
}

Result<view::PayloadOwner> BuildPlan::execute(
    std::uint32_t policy,
    const backing::Callbacks* callbacks) const try {
    if (policy != UINT32_C(1) && policy != UINT32_C(2)) {
        return Result<view::PayloadOwner>::failure(
            backing_error(FDB_PAYLOAD_E_INVALID_ARGUMENT,
                          "invalid_execution_policy"));
    }
    const backing::Callbacks selected =
        callbacks == nullptr ? backing::heap_callbacks() : *callbacks;
    if (selected.reserve == nullptr || selected.commit == nullptr ||
        selected.rollback == nullptr || selected.release == nullptr) {
        return Result<view::PayloadOwner>::failure(
            backing_error(FDB_PAYLOAD_E_BACKING_CONTRACT,
                          "missing_required_callback"));
    }

    auto reserve_failure = [](std::uint32_t status,
                              std::uint32_t classified) {
        if (classified == FDB_PAYLOAD_E_ALLOCATION_FAILED) {
            return Result<view::PayloadOwner>::failure(backing_error(
                FDB_PAYLOAD_E_ALLOCATION_FAILED,
                "reserve_allocation_failed"));
        }
        return Result<view::PayloadOwner>::failure(
            callback_contract_error("reserve", status));
    };

    auto execute_reserved =
        [this](backing::BackingReservation& reservation,
                          std::uint32_t mode,
                          std::uint32_t fallback_reason,
                          const std::uint8_t* staged_image)
        -> Result<CommittedImage> {
        const bool misaligned =
            reservation.writable_data() != nullptr &&
            (reinterpret_cast<std::uintptr_t>(
                 reservation.writable_data()) &
             (static_cast<std::uintptr_t>(info_.max_alignment) -
              std::uintptr_t{1})) != std::uintptr_t{0};
        if (reservation.capacity() < info_.total_bytes || misaligned ||
            (reservation.writable_data() == nullptr &&
             reservation.callbacks().write == nullptr)) {
            return fail_reserved(
                reservation,
                backing_error(FDB_PAYLOAD_E_BACKING_CONTRACT,
                              "invalid_reservation"));
        }

        Result<void> written = Result<void>::success();
        std::uint64_t bytes_written = UINT64_C(0);
        if (reservation.writable_data() != nullptr) {
            StableSpanSink sink(reservation.writable_data(),
                                reservation.capacity());
            written = staged_image == nullptr
                          ? encode_profile(profile_layout_, values_, sink)
                          : sink.write(UINT64_C(0), staged_image,
                                       info_.total_bytes);
            bytes_written = sink.bytes_written();
        } else {
            RangeCallbackSink sink(reservation, reservation.capacity());
            written = staged_image == nullptr
                          ? encode_profile(profile_layout_, values_, sink)
                          : sink.write(UINT64_C(0), staged_image,
                                       info_.total_bytes);
            bytes_written = sink.bytes_written();
        }
        if (!written.has_value()) {
            return fail_reserved(reservation,
                                 std::move(written).error());
        }
        if (bytes_written != info_.total_bytes) {
            return fail_reserved(
                reservation,
                backing_error(FDB_PAYLOAD_E_BACKING_CONTRACT,
                              "incomplete_write_coverage"));
        }

        const std::uint32_t commit_status =
            reservation.commit(info_.total_bytes);
        const std::uint32_t commit_classification =
            backing::classify_callback_status(
                backing::CallbackOperation::commit, commit_status);
        if (commit_classification != success_status) {
            if (commit_classification == FDB_PAYLOAD_E_ALLOCATION_FAILED) {
                return fail_reserved(
                    reservation,
                    backing_error(FDB_PAYLOAD_E_ALLOCATION_FAILED,
                                  "commit_allocation_failed"));
            }
            if (commit_classification == FDB_PAYLOAD_E_COMMIT_FAILED) {
                return fail_reserved(
                    reservation,
                    backing_error(FDB_PAYLOAD_E_COMMIT_FAILED,
                                  "commit_failed"));
            }
            return fail_reserved(
                reservation,
                callback_contract_error("commit", commit_status));
        }
        backing::CommittedBacking committed =
            reservation.take_committed();
        if ((committed.readable_data() == nullptr &&
             info_.total_bytes != UINT64_C(0)) ||
            committed.readable_size() != info_.total_bytes ||
            info_.total_bytes > committed.capacity()) {
            return Result<CommittedImage>::failure(
                backing_error(FDB_PAYLOAD_E_BACKING_CONTRACT,
                              "invalid_committed_span"));
        }

        ExecutionReport report{
            mode,
            fallback_reason,
            info_.total_bytes,
            info_.total_bytes,
            mode == UINT32_C(2) ? info_.total_bytes : UINT64_C(0),
            info_.region_count,
            committed.capacity(),
        };
        return Result<CommittedImage>::success(
            CommittedImage{std::move(committed), report});
    };

    auto publish_image = [this](CommittedImage image)
        -> Result<view::PayloadOwner> {
        auto opened = view::open_payload(
            values_.spec(), image.backing.readable_data(),
            image.backing.readable_size(), publication_options(info_));
        if (!opened.has_value()) {
            if (opened.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED) {
                return Result<view::PayloadOwner>::failure(
                    std::move(opened).error());
            }
            return Result<view::PayloadOwner>::failure(
                committed_image_validation_error(opened.error()));
        }
        return view::PayloadOwner::publish(
            std::move(image.backing), values_.spec(),
            std::move(opened).value(), image.report);
    };

    backing::BackingReservation direct(selected);
    const std::uint32_t direct_status = direct.reserve(
        UINT32_C(1), info_.total_bytes, info_.max_alignment);
    const std::uint32_t direct_classification =
        backing::classify_callback_status(
            backing::CallbackOperation::reserve_direct, direct_status);
    if (direct_classification == success_status) {
        auto image = execute_reserved(
            direct, UINT32_C(1), UINT32_C(0), nullptr);
        if (!image.has_value()) {
            return Result<view::PayloadOwner>::failure(
                std::move(image).error());
        }
        return publish_image(std::move(image).value());
    }
    if (direct_classification != FDB_PAYLOAD_E_DIRECT_UNAVAILABLE) {
        return reserve_failure(direct_status, direct_classification);
    }
    if (policy == UINT32_C(2)) {
        return Result<view::PayloadOwner>::failure(
            backing_error(FDB_PAYLOAD_E_DIRECT_UNAVAILABLE,
                          "backing_declined_direct"));
    }

    backing::BackingReservation heap(backing::heap_callbacks());
    const std::uint32_t heap_status = heap.reserve(
        UINT32_C(1), info_.total_bytes, info_.max_alignment);
    const std::uint32_t heap_classification =
        backing::classify_callback_status(
            backing::CallbackOperation::reserve_direct, heap_status);
    if (heap_classification != success_status) {
        return reserve_failure(heap_status, heap_classification);
    }
    auto heap_image = execute_reserved(
        heap, UINT32_C(1), UINT32_C(0), nullptr);
    if (!heap_image.has_value()) {
        return Result<view::PayloadOwner>::failure(
            std::move(heap_image).error());
    }
    backing::BackingReservation staged(selected);
    const std::uint32_t staged_status = staged.reserve(
        UINT32_C(2), info_.total_bytes, info_.max_alignment);
    const std::uint32_t staged_classification =
        backing::classify_callback_status(
            backing::CallbackOperation::reserve_staged, staged_status);
    if (staged_classification != success_status) {
        return reserve_failure(staged_status, staged_classification);
    }
    auto final_image = execute_reserved(
        staged, UINT32_C(2), UINT32_C(2),
        heap_image.value().backing.readable_data());
    if (!final_image.has_value()) {
        return Result<view::PayloadOwner>::failure(
            std::move(final_image).error());
    }
    return publish_image(std::move(final_image).value());
} catch (const std::bad_alloc&) {
    return Result<view::PayloadOwner>::failure(allocation_error());
} catch (const std::length_error&) {
    return Result<view::PayloadOwner>::failure(allocation_error());
}

}  // namespace fastdb::payload::build
