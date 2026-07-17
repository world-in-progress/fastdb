#pragma once

#include "payload/build/ValueArena.hpp"
#include "payload/error/Result.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace fastdb::payload::build {

struct BuilderLimits final {
    std::uint64_t max_value_nodes;
    std::uint64_t max_list_elements;
    std::uint64_t max_text_bytes;
    std::uint64_t max_opaque_bytes;
    std::uint64_t max_nesting_depth;
    std::uint64_t max_total_builder_bytes;
};

BuilderLimits default_builder_limits() noexcept;

struct FixedRun final {
    const std::uint8_t* data;
    std::uint64_t data_byte_length;
    std::uint64_t count;
    std::uint64_t stride_bytes;
    const std::uint8_t* validity;
    std::uint64_t validity_byte_length;
    std::uint64_t validity_bit_offset;
};

class PayloadBuilder final {
public:
    static error::Result<PayloadBuilder> create(
        spec::CompiledSpec spec,
        BuilderLimits limits = default_builder_limits());

    PayloadBuilder(const PayloadBuilder&) = delete;
    PayloadBuilder& operator=(const PayloadBuilder&) = delete;
    PayloadBuilder(PayloadBuilder&&) noexcept;
    PayloadBuilder& operator=(PayloadBuilder&&) noexcept;
    ~PayloadBuilder();

    error::Result<void> begin_entry(std::uint32_t entry_index,
                                    std::uint64_t value_count);
    error::Result<void> push_null();
    error::Result<void> push_bool(std::uint8_t value);
    error::Result<void> push_u8(std::uint8_t value);
    error::Result<void> push_u16(std::uint16_t value);
    error::Result<void> push_u32(std::uint32_t value);
    error::Result<void> push_i32(std::int32_t value);
    error::Result<void> push_u8n_bits(std::uint64_t binary64_bits);
    error::Result<void> push_u16n_bits(std::uint64_t binary64_bits);
    error::Result<void> push_f32_bits(std::uint32_t binary32_bits);
    error::Result<void> push_f64_bits(std::uint64_t binary64_bits);
    error::Result<void> push_str(std::string_view utf8);
    error::Result<void> push_wstr(const std::uint16_t* units,
                                  std::uint64_t unit_count);
    error::Result<void> push_bytes(const std::uint8_t* bytes,
                                   std::uint64_t byte_count);
    error::Result<void> push_fixed_run(const FixedRun& run);
    error::Result<void> begin_component();
    error::Result<void> begin_list(std::uint64_t item_count);
    error::Result<LogicalPayload> freeze();

private:
    struct State;

    explicit PayloadBuilder(std::unique_ptr<State> state) noexcept;

    error::Result<void> begin_entry_impl(std::uint32_t entry_index,
                                         std::uint64_t value_count);
    error::Result<void> push_null_impl();
    error::Result<void> push_scalar_impl(spec::TypeKind kind,
                                         ValueTag tag,
                                         std::uint64_t bits,
                                         std::string_view operation);
    error::Result<void> push_normalized_impl(spec::TypeKind kind,
                                             ValueTag tag,
                                             std::uint64_t bits,
                                             std::string_view operation);
    error::Result<void> push_storage_impl(spec::TypeKind kind,
                                          ValueTag tag,
                                          const std::uint8_t* bytes,
                                          std::uint64_t byte_count,
                                          bool text,
                                          std::string_view operation);
    error::Result<void> push_fixed_run_impl(const FixedRun& run);
    error::Result<void> begin_component_impl();
    error::Result<void> begin_list_impl(std::uint64_t item_count);
    error::Result<LogicalPayload> freeze_impl();

    State* state_pointer() noexcept;
    const State* state_pointer() const noexcept;

    std::unique_ptr<State> state_;
};

}  // namespace fastdb::payload::build
