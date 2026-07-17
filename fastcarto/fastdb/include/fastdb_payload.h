#ifndef FASTDB_PAYLOAD_H
#define FASTDB_PAYLOAD_H

#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(FASTDB_PAYLOAD_BUILDING)
#define FDB_PAYLOAD_API __declspec(dllexport)
#else
#define FDB_PAYLOAD_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define FDB_PAYLOAD_API __attribute__((visibility("default")))
#else
#define FDB_PAYLOAD_API
#endif

#define FDB_PAYLOAD_V1_ABI_VERSION UINT32_C(1)
#define FDB_PAYLOAD_V1_SHA256_SIZE UINT32_C(32)
#define FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE UINT32_C(80)
#define FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE UINT32_C(72)

#define FDB_PAYLOAD_PROFILE_RECORD_V1 UINT32_C(1)
#define FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1 UINT32_C(2)

#define FDB_PAYLOAD_SEMANTIC_HAS_NULLABLE (UINT64_C(1) << 0)
#define FDB_PAYLOAD_SEMANTIC_HAS_LISTS (UINT64_C(1) << 1)
#define FDB_PAYLOAD_SEMANTIC_HAS_REFERENCES (UINT64_C(1) << 2)
#define FDB_PAYLOAD_SEMANTIC_HAS_VARIABLE_WIDTH (UINT64_C(1) << 3)
#define FDB_PAYLOAD_SEMANTIC_HAS_NORMALIZED_INTEGERS (UINT64_C(1) << 4)

#define FDB_PAYLOAD_OPERATION_COMPILE (UINT64_C(1) << 0)
#define FDB_PAYLOAD_OPERATION_QUERY (UINT64_C(1) << 1)

#define FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED UINT32_C(0)

#define FDB_PAYLOAD_E_INVALID_JSON UINT32_C(1001)
#define FDB_PAYLOAD_E_DUPLICATE_KEY UINT32_C(1002)
#define FDB_PAYLOAD_E_UNKNOWN_FIELD UINT32_C(1003)
#define FDB_PAYLOAD_E_UNSUPPORTED_SCHEMA UINT32_C(1004)
#define FDB_PAYLOAD_E_INVALID_TYPE UINT32_C(1005)
#define FDB_PAYLOAD_E_DUPLICATE_ID UINT32_C(1006)
#define FDB_PAYLOAD_E_UNRESOLVED_COMPONENT UINT32_C(1007)
#define FDB_PAYLOAD_E_PROFILE_VIOLATION UINT32_C(1008)
#define FDB_PAYLOAD_E_INVALID_NUMBER UINT32_C(1009)
#define FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT UINT32_C(1010)

#define FDB_PAYLOAD_E_MISSING_ENTRY UINT32_C(2001)
#define FDB_PAYLOAD_E_MISSING_FIELD UINT32_C(2002)
#define FDB_PAYLOAD_E_UNEXPECTED_NULL UINT32_C(2003)
#define FDB_PAYLOAD_E_TYPE_MISMATCH UINT32_C(2004)
#define FDB_PAYLOAD_E_OUT_OF_RANGE UINT32_C(2005)
#define FDB_PAYLOAD_E_BUILDER_STATE UINT32_C(2006)
#define FDB_PAYLOAD_E_DIRECT_UNAVAILABLE UINT32_C(2007)
#define FDB_PAYLOAD_E_PLAN_STATE UINT32_C(2008)

#define FDB_PAYLOAD_E_INVALID_MAGIC UINT32_C(3001)
#define FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION UINT32_C(3002)
#define FDB_PAYLOAD_E_LENGTH_OVERFLOW UINT32_C(3003)
#define FDB_PAYLOAD_E_OUT_OF_BOUNDS UINT32_C(3004)
#define FDB_PAYLOAD_E_MISALIGNED UINT32_C(3005)
#define FDB_PAYLOAD_E_DIGEST_MISMATCH UINT32_C(3006)
#define FDB_PAYLOAD_E_INVALID_REFERENCE UINT32_C(3007)
#define FDB_PAYLOAD_E_RESOURCE_LIMIT UINT32_C(3008)

#define FDB_PAYLOAD_E_VIEW_INVALIDATED UINT32_C(4001)
#define FDB_PAYLOAD_E_STALE_GENERATION UINT32_C(4002)
#define FDB_PAYLOAD_E_READ_ONLY UINT32_C(4003)

#define FDB_PAYLOAD_E_BACKING_CONTRACT UINT32_C(5001)
#define FDB_PAYLOAD_E_ALLOCATION_FAILED UINT32_C(5002)
#define FDB_PAYLOAD_E_COMMIT_FAILED UINT32_C(5003)
#define FDB_PAYLOAD_E_ROLLBACK_FAILED UINT32_C(5004)

#define FDB_PAYLOAD_E_UNSUPPORTED_TARGET UINT32_C(6001)
#define FDB_PAYLOAD_E_INVALID_ARTIFACT_PATH UINT32_C(6002)
#define FDB_PAYLOAD_E_GENERATOR_FAILED UINT32_C(6003)

#define FDB_PAYLOAD_E_INVALID_ARGUMENT UINT32_C(7001)
#define FDB_PAYLOAD_E_UNSUPPORTED_ABI UINT32_C(7002)
#define FDB_PAYLOAD_E_NOT_FOUND UINT32_C(7003)
#define FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE UINT32_C(7004)

#define FDB_PAYLOAD_E_INTERNAL UINT32_C(9001)

typedef uint32_t fdb_payload_v1_status_t;
typedef uint32_t fdb_payload_v1_profile_t;

typedef struct fdb_payload_v1_spec fdb_payload_v1_spec_t;
typedef struct fdb_payload_v1_blob fdb_payload_v1_blob_t;
typedef struct fdb_payload_v1_error fdb_payload_v1_error_t;

typedef struct fdb_payload_v1_compile_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t max_source_bytes;
    uint64_t max_json_values;
    uint32_t max_nesting_depth;
    uint32_t max_entries;
    uint32_t max_components;
    uint32_t max_fields_per_component;
    uint64_t max_total_fields;
    uint64_t reserved[4];
} fdb_payload_v1_compile_options_t;

typedef struct fdb_payload_v1_capabilities {
    uint32_t struct_size;
    uint32_t profile;
    uint64_t semantic_flags;
    uint64_t operation_flags;
    uint64_t codegen_target_flags;
    uint32_t direct_build_status;
    uint32_t reserved32;
    uint64_t reserved64[4];
} fdb_payload_v1_capabilities_t;

#ifdef __cplusplus
extern "C" {
#endif

FDB_PAYLOAD_API uint32_t fdb_payload_v1_abi_version(void);

/* A null output is accepted. Present output receives V1 defaults. */
FDB_PAYLOAD_API void fdb_payload_v1_compile_options_init(
    fdb_payload_v1_compile_options_t* options);

/* A null output is accepted. Present output receives a zeroed V1 report. */
FDB_PAYLOAD_API void fdb_payload_v1_capabilities_init(
    fdb_payload_v1_capabilities_t* capabilities);

/* Null is accepted. Each retain owns one matching release. */
FDB_PAYLOAD_API void fdb_payload_v1_blob_retain(fdb_payload_v1_blob_t* blob);
FDB_PAYLOAD_API void fdb_payload_v1_blob_release(fdb_payload_v1_blob_t* blob);

/* Returned bytes are borrowed while the caller owns the handle. */
FDB_PAYLOAD_API const uint8_t* fdb_payload_v1_blob_data(
    const fdb_payload_v1_blob_t* blob);
FDB_PAYLOAD_API uint64_t fdb_payload_v1_blob_size(
    const fdb_payload_v1_blob_t* blob);

/* Null is accepted. Each retain owns one matching release. */
FDB_PAYLOAD_API void fdb_payload_v1_error_retain(
    fdb_payload_v1_error_t* error);
FDB_PAYLOAD_API void fdb_payload_v1_error_release(
    fdb_payload_v1_error_t* error);

/* A null handle returns zero. */
FDB_PAYLOAD_API uint32_t fdb_payload_v1_error_code(
    const fdb_payload_v1_error_t* error);

/*
 * Output locations may be null. Present outputs receive null and zero for a
 * null handle. Returned bytes are borrowed until the next release to zero.
 */
FDB_PAYLOAD_API void fdb_payload_v1_error_symbol(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size);
FDB_PAYLOAD_API void fdb_payload_v1_error_path(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size);
FDB_PAYLOAD_API void fdb_payload_v1_error_message(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size);
FDB_PAYLOAD_API void fdb_payload_v1_error_details_json(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size);

#ifdef __cplusplus
}
#endif

#endif
