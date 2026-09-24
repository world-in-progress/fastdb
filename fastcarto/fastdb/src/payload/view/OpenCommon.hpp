#pragma once

#include "payload/error/Result.hpp"
#include "payload/json/JsonPointer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fastdb::payload::view::open_common {

error::Error simple_error(std::uint32_t code,
                          const json::JsonPointer& path,
                          const char* message,
                          const char* reason);
error::Error noncanonical(const json::JsonPointer& path,
                          const char* reason);
error::Error noncanonical_exact(const json::JsonPointer& path,
                                const char* reason,
                                std::string actual,
                                std::string expected);
error::Result<void> require_canonical_u64(
    std::uint64_t actual,
    std::uint64_t expected,
    const json::JsonPointer& path,
    const char* reason);
error::Error resource_error(const json::JsonPointer& path,
                            const char* resource,
                            std::uint64_t actual,
                            std::uint64_t limit);
error::Error allocation_error();

json::JsonPointer binary_header_path();
json::JsonPointer binary_region_path(std::uint32_t index);
json::JsonPointer binary_entry_path(std::uint32_t index);
std::string byte_hex(const std::uint8_t* bytes, std::size_t size);

class WorkCounter final {
public:
    explicit WorkCounter(std::uint64_t limit) : limit_(limit) {}

    error::Result<void> charge(std::uint64_t units,
                               const json::JsonPointer& path);
    std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t limit_;
    std::uint64_t value_{UINT64_C(0)};
};

error::Result<void> require_zero(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t begin,
    std::uint64_t end,
    WorkCounter& work,
    const json::JsonPointer& path,
    const char* reason,
    bool charge_units = true,
    bool exact_facts = false);

error::Result<std::uint32_t> read_u32(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    const json::JsonPointer& path);
error::Result<std::uint64_t> read_u64(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint64_t offset,
    const json::JsonPointer& path);

struct RegionFields final {
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint32_t owner;
    std::uint32_t runtime_type_id;
    std::uint64_t data_offset;
    std::uint64_t byte_length;
    std::uint64_t element_count;
    std::uint32_t stride;
    std::uint32_t alignment;
    std::uint64_t reserved;
};

error::Result<RegionFields> read_region_fields(
    const std::uint8_t* bytes,
    std::uint64_t byte_count,
    std::uint32_t region_index,
    const json::JsonPointer& path);
error::Result<void> validate_region_fields(
    const RegionFields& actual,
    const RegionFields& expected,
    const json::JsonPointer& path);

}  // namespace fastdb::payload::view::open_common
