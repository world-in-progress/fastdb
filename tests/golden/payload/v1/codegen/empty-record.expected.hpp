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
