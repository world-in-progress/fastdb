// generated-by: fastdb.payload.codegen.v1
// payload-sha256: 92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71
// core-abi-version: 1
// generator-version: fastdb.payload.codegen.v1
// target: cpp
#pragma once

#include <fastdb_payload.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace fastdb_payload_92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71 {

inline constexpr std::string_view canonical_source =
    R"FDB_PAYLOAD({"components":[],"entries":[],"profile":"record.v1","schema":"fastdb.payload.v1"})FDB_PAYLOAD";
inline constexpr std::string_view payload_sha256 = "92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71";
inline constexpr std::array<std::uint8_t, 32> payload_sha256_bytes{{0x92U, 0xfbU, 0xbcU, 0x65U, 0xfcU, 0x79U, 0xadU, 0x9cU, 0xa6U, 0xe9U, 0x63U, 0x70U, 0x63U, 0xc8U, 0xf1U, 0x5aU, 0x80U, 0x6bU, 0x40U, 0xe7U, 0x88U, 0x53U, 0x22U, 0x25U, 0xcaU, 0xfeU, 0x25U, 0xefU, 0x17U, 0xcdU, 0xfcU, 0x71U}};

inline fastdb::payload::v1::CompiledSpec compile_spec() {
    return fastdb::payload::v1::CompiledSpec::compile(canonical_source);
}

struct IdMetadata final {
    std::string_view symbol;
    std::string_view original_id;
    std::uint32_t stable_index;
};

inline constexpr std::array<IdMetadata, 0> entries{{
}};

inline constexpr std::array<IdMetadata, 0> components{{
}};

struct FieldMetadata final {
    std::string_view symbol;
    std::string_view original_id;
    std::uint32_t component_index;
    std::uint32_t field_index;
};

inline constexpr std::array<FieldMetadata, 0> fields{{
}};

}  // namespace fastdb_payload_92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71
