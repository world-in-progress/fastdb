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
#define FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE UINT32_C(88)
#define FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE UINT32_C(112)
#define FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE UINT32_C(104)
#define FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE UINT32_C(72)

#define FDB_PAYLOAD_BINARY_V1_HEADER_SIZE UINT32_C(128)
#define FDB_PAYLOAD_BINARY_V1_REGION_DESCRIPTOR_SIZE UINT32_C(56)
#define FDB_PAYLOAD_BINARY_V1_ENTRY_DESCRIPTOR_SIZE UINT32_C(40)

#define FDB_PAYLOAD_REGION_ENTRY_VALUES UINT32_C(1)
#define FDB_PAYLOAD_REGION_ENTRY_VALIDITY UINT32_C(2)
#define FDB_PAYLOAD_REGION_LIST_ITEMS UINT32_C(3)
#define FDB_PAYLOAD_REGION_LIST_VALIDITY UINT32_C(4)
#define FDB_PAYLOAD_REGION_UTF8_POOL UINT32_C(5)
#define FDB_PAYLOAD_REGION_UTF16_POOL UINT32_C(6)
#define FDB_PAYLOAD_REGION_BYTES_POOL UINT32_C(7)

#define FDB_PAYLOAD_PROFILE_RECORD_V1 UINT32_C(1)
#define FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1 UINT32_C(2)

#define FDB_PAYLOAD_SEMANTIC_HAS_NULLABLE (UINT64_C(1) << 0)
#define FDB_PAYLOAD_SEMANTIC_HAS_LISTS (UINT64_C(1) << 1)
#define FDB_PAYLOAD_SEMANTIC_HAS_REFERENCES (UINT64_C(1) << 2)
#define FDB_PAYLOAD_SEMANTIC_HAS_VARIABLE_WIDTH (UINT64_C(1) << 3)
#define FDB_PAYLOAD_SEMANTIC_HAS_NORMALIZED_INTEGERS (UINT64_C(1) << 4)

#define FDB_PAYLOAD_OPERATION_COMPILE (UINT64_C(1) << 0)
#define FDB_PAYLOAD_OPERATION_QUERY (UINT64_C(1) << 1)
#define FDB_PAYLOAD_OPERATION_BUILD (UINT64_C(1) << 2)
#define FDB_PAYLOAD_OPERATION_OPEN (UINT64_C(1) << 3)
#define FDB_PAYLOAD_OPERATION_INVALIDATE (UINT64_C(1) << 6)

#define FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED UINT32_C(0)
#define FDB_PAYLOAD_DIRECT_BUILD_ELIGIBLE UINT32_C(1)
#define FDB_PAYLOAD_DIRECT_BUILD_UNAVAILABLE UINT32_C(2)

#define FDB_PAYLOAD_BUILD_ALLOW_STAGING UINT32_C(1)
#define FDB_PAYLOAD_BUILD_REQUIRE_DIRECT UINT32_C(2)

#define FDB_PAYLOAD_RESERVE_DIRECT UINT32_C(1)
#define FDB_PAYLOAD_RESERVE_STAGED UINT32_C(2)
#define FDB_PAYLOAD_EXECUTION_DIRECT UINT32_C(1)
#define FDB_PAYLOAD_EXECUTION_STAGED UINT32_C(2)

#define FDB_PAYLOAD_FALLBACK_NONE UINT32_C(0)
#define FDB_PAYLOAD_FALLBACK_PLAN_REQUIRES_STAGING UINT32_C(1)
#define FDB_PAYLOAD_FALLBACK_BACKING_DECLINED_DIRECT UINT32_C(2)

#define FDB_PAYLOAD_OPEN_VALIDATE_TEXT_EAGER (UINT32_C(1) << 0)

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
#define FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE UINT32_C(2009)
#define FDB_PAYLOAD_E_INVALID_TEXT_ENCODING UINT32_C(2010)
#define FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW UINT32_C(2011)
#define FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS UINT32_C(2012)
#define FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT UINT32_C(2013)

#define FDB_PAYLOAD_E_INVALID_MAGIC UINT32_C(3001)
#define FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION UINT32_C(3002)
#define FDB_PAYLOAD_E_LENGTH_OVERFLOW UINT32_C(3003)
#define FDB_PAYLOAD_E_OUT_OF_BOUNDS UINT32_C(3004)
#define FDB_PAYLOAD_E_MISALIGNED UINT32_C(3005)
#define FDB_PAYLOAD_E_DIGEST_MISMATCH UINT32_C(3006)
#define FDB_PAYLOAD_E_INVALID_REFERENCE UINT32_C(3007)
#define FDB_PAYLOAD_E_RESOURCE_LIMIT UINT32_C(3008)
#define FDB_PAYLOAD_E_NON_CANONICAL_BINARY UINT32_C(3009)
#define FDB_PAYLOAD_E_INVALID_BINARY_VALUE UINT32_C(3010)

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
typedef struct fdb_payload_v1_builder fdb_payload_v1_builder_t;
typedef struct fdb_payload_v1_plan fdb_payload_v1_plan_t;
typedef struct fdb_payload_v1_payload fdb_payload_v1_payload_t;

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

typedef struct fdb_payload_v1_builder_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t max_value_nodes;
    uint64_t max_list_elements;
    uint64_t max_text_bytes;
    uint64_t max_opaque_bytes;
    uint64_t max_nesting_depth;
    uint64_t max_total_builder_bytes;
    uint64_t reserved[4];
} fdb_payload_v1_builder_options_t;

typedef struct fdb_payload_v1_open_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t max_total_bytes;
    uint64_t max_regions;
    uint64_t max_entries;
    uint64_t max_components;
    uint64_t max_nesting_depth;
    uint64_t max_list_elements;
    uint64_t max_graph_objects;
    uint64_t max_string_bytes;
    uint64_t max_validation_work;
    uint64_t reserved[4];
} fdb_payload_v1_open_options_t;

typedef struct fdb_payload_v1_plan_info {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t total_bytes;
    uint64_t region_count;
    uint64_t logical_value_count;
    uint64_t list_element_count;
    uint64_t text_bytes;
    uint64_t opaque_bytes;
    uint64_t validation_work;
    uint32_t max_alignment;
    uint32_t direct_build_status;
    uint64_t reserved[4];
} fdb_payload_v1_plan_info_t;

typedef struct fdb_payload_v1_execution_report {
    uint32_t struct_size;
    uint32_t mode;
    uint32_t fallback_reason;
    uint32_t reserved32;
    uint64_t requested_bytes;
    uint64_t used_bytes;
    uint64_t staging_bytes;
    uint64_t region_count;
    uint64_t backing_capacity;
    uint64_t reserved64[2];
} fdb_payload_v1_execution_report_t;

typedef struct fdb_payload_v1_fixed_run_v1 {
    uint32_t struct_size;
    uint32_t flags;
    const void* data;
    uint64_t data_byte_length;
    uint64_t count;
    uint64_t stride_bytes;
    const uint8_t* validity;
    uint64_t validity_byte_length;
    uint64_t validity_bit_offset;
    uint64_t reserved[4];
} fdb_payload_v1_fixed_run_v1_t;

typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_reserve_fn)(
    void* context,
    uint32_t reserve_mode,
    uint64_t minimum_capacity,
    uint32_t alignment,
    void** out_owner_token,
    uint8_t** out_writable_data,
    uint64_t* out_capacity);
typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_write_fn)(
    void* context,
    void* owner_token,
    uint64_t offset,
    const uint8_t* source,
    uint64_t source_size);
typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_commit_fn)(
    void* context,
    void* owner_token,
    uint64_t used_size,
    const uint8_t** out_readable_data,
    uint64_t* out_readable_size);
typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_rollback_fn)(
    void* context,
    void* owner_token);
typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_retain_fn)(
    void* context,
    void* owner_token);
typedef void (*fdb_payload_v1_backing_release_fn)(void* context,
                                                  void* owner_token);

/*
 * A backing table is borrowed for the call, validated through release, and
 * copied into Core-owned state. struct_size must cover that known prefix;
 * known flags and every fully covered known reserved word must be zero. A
 * larger unknown tail is ignored. Build requires reserve/commit/rollback/
 * release; write may be null only when reserve supplies a stable writable
 * base. External open requires retain/release. owner_token may be null and is
 * never dereferenced by Core.
 *
 * Callback statuses are closed. Direct reserve accepts zero,
 * DIRECT_UNAVAILABLE, or ALLOCATION_FAILED; staged reserve accepts zero or
 * ALLOCATION_FAILED. Write and retain accept zero or ALLOCATION_FAILED. Commit
 * additionally accepts COMMIT_FAILED; rollback additionally accepts
 * ROLLBACK_FAILED (and maps ALLOCATION_FAILED to ROLLBACK_FAILED). Every other
 * nonzero status is BACKING_CONTRACT. Core initializes callback outputs to
 * null/zero and ignores any values written by a failed callback.
 *
 * Successful reserve creates one uncommitted reference: a later pre-commit
 * failure gets exactly one rollback, while successful commit transfers one
 * committed reference that is never rolled back and is eventually released.
 * Reserve failure creates no reference and gets neither rollback nor release.
 * Successful retain creates one committed reference; retain failure creates
 * none and gets no release. The context and copied callbacks must remain
 * callable until the corresponding rollback or release completes.
 *
 * Callbacks are synchronous and non-throwing. Within one plan execution, Core
 * invokes reserve/write/commit/rollback serially. retain and release must be
 * thread-safe because independent payload/open lifetimes may invoke them
 * concurrently. A callback must not re-enter a FastDB operation on the same
 * context/owner token when that operation can invoke or wait for the same
 * callback. Distinct contexts/tokens may re-enter.
 */
typedef struct fdb_payload_v1_backing_v1 {
    uint32_t struct_size;
    uint32_t flags;
    void* context;
    fdb_payload_v1_backing_reserve_fn reserve;
    fdb_payload_v1_backing_write_fn write;
    fdb_payload_v1_backing_commit_fn commit;
    fdb_payload_v1_backing_rollback_fn rollback;
    fdb_payload_v1_backing_retain_fn retain;
    fdb_payload_v1_backing_release_fn release;
    uint64_t reserved[4];
} fdb_payload_v1_backing_v1_t;

#ifdef __cplusplus
extern "C" {
#endif

FDB_PAYLOAD_API uint32_t fdb_payload_v1_abi_version(void);

/* A null output is accepted. Present output receives compile V1 defaults. */
FDB_PAYLOAD_API void fdb_payload_v1_compile_options_init(
    fdb_payload_v1_compile_options_t* options);

/* A null output is accepted. Present output receives empty V1 capabilities. */
FDB_PAYLOAD_API void fdb_payload_v1_capabilities_init(
    fdb_payload_v1_capabilities_t* capabilities);
/* Present output receives builder-limit V1 defaults; null is accepted. */
FDB_PAYLOAD_API void fdb_payload_v1_builder_options_init(
    fdb_payload_v1_builder_options_t* options);
/* Present output receives target sizeof and null/zero members; null is accepted. */
FDB_PAYLOAD_API void fdb_payload_v1_fixed_run_init(
    fdb_payload_v1_fixed_run_v1_t* run);
/* Present output receives safe open-limit V1 defaults; null is accepted. */
FDB_PAYLOAD_API void fdb_payload_v1_open_options_init(
    fdb_payload_v1_open_options_t* options);
/* Present output receives a zeroed V1 plan-info report; null is accepted. */
FDB_PAYLOAD_API void fdb_payload_v1_plan_info_init(
    fdb_payload_v1_plan_info_t* info);
/* Present output receives a zeroed V1 execution report; null is accepted. */
FDB_PAYLOAD_API void fdb_payload_v1_execution_report_init(
    fdb_payload_v1_execution_report_t* report);
/* Present output receives target sizeof and null/zero members; null is accepted. */
FDB_PAYLOAD_API void fdb_payload_v1_backing_init(
    fdb_payload_v1_backing_v1_t* backing);

/*
 * Every fallible function below requires a non-null out_error sink. A null
 * sink returns INVALID_ARGUMENT without touching any value output. With a
 * present sink, the function first clears *out_error and every present value
 * output. Success returns zero, publishes the complete value, and leaves
 * *out_error null. Failure leaves value outputs cleared and publishes one
 * caller-owned, releasable error reference whose code equals the returned
 * status.
 */

/*
 * A builder is unique, thread-confined mutable state and is not thread-safe.
 * Input strings, byte spans, wide-string code units, and fixed-run
 * descriptors/storage are borrowed only for the call. Successful freeze seals
 * the builder and publishes one immutable plan. Ordinary freeze failure
 * publishes no plan and leaves the same builder retryable.
 */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_create(
    const fdb_payload_v1_spec_t* spec,
    const fdb_payload_v1_builder_options_t* options,
    fdb_payload_v1_builder_t** out_builder,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API void fdb_payload_v1_builder_release(
    fdb_payload_v1_builder_t* builder);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_entry_begin(
    fdb_payload_v1_builder_t* builder,
    uint32_t entry_index,
    uint64_t value_count,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_null(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_bool(
    fdb_payload_v1_builder_t* builder,
    uint8_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_u8(
    fdb_payload_v1_builder_t* builder,
    uint8_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_u16(
    fdb_payload_v1_builder_t* builder,
    uint16_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_u32(
    fdb_payload_v1_builder_t* builder,
    uint32_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_i32(
    fdb_payload_v1_builder_t* builder,
    int32_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_builder_value_u8n_f64_bits(
    fdb_payload_v1_builder_t* builder,
    uint64_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_builder_value_u16n_f64_bits(
    fdb_payload_v1_builder_t* builder,
    uint64_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_builder_value_f32_bits(
    fdb_payload_v1_builder_t* builder,
    uint32_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_builder_value_f64_bits(
    fdb_payload_v1_builder_t* builder,
    uint64_t value,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_str(
    fdb_payload_v1_builder_t* builder,
    const uint8_t* value,
    uint64_t value_size,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_wstr(
    fdb_payload_v1_builder_t* builder,
    const uint16_t* value,
    uint64_t value_size,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_value_bytes(
    fdb_payload_v1_builder_t* builder,
    const uint8_t* value,
    uint64_t value_size,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_builder_value_fixed_run(
    fdb_payload_v1_builder_t* builder,
    const fdb_payload_v1_fixed_run_v1_t* run,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_builder_value_component_begin(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_builder_value_list_begin(
    fdb_payload_v1_builder_t* builder,
    uint64_t item_count,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_builder_freeze(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_plan_t** out_plan,
    fdb_payload_v1_error_t** out_error);

/*
 * Plans are immutable, repeatable, atomically retained handles; retain,
 * release, and info queries are thread-safe. The same plan may execute
 * concurrently with Core heap storage or distinct caller backing contexts.
 * The caller must serialize executions that share one backing context.
 */
FDB_PAYLOAD_API void fdb_payload_v1_plan_retain(fdb_payload_v1_plan_t* plan);
FDB_PAYLOAD_API void fdb_payload_v1_plan_release(fdb_payload_v1_plan_t* plan);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_plan_info(
    const fdb_payload_v1_plan_t* plan,
    fdb_payload_v1_plan_info_t* out_info,
    fdb_payload_v1_error_t** out_error);
/*
 * policy is exactly ALLOW_STAGING or REQUIRE_DIRECT. A null backing selects
 * Core heap storage. The report describes the actual direct/staged execution;
 * no payload or partial report is published on failure.
 */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_plan_execute(
    const fdb_payload_v1_plan_t* plan,
    uint32_t policy,
    const fdb_payload_v1_backing_v1_t* backing,
    fdb_payload_v1_payload_t** out_payload,
    fdb_payload_v1_execution_report_t* out_report,
    fdb_payload_v1_error_t** out_error);

/*
 * open_copy copies the borrowed byte span before returning. open_external
 * acquires one reference with backing.retain before Core validation; failure
 * after a successful retain releases it exactly once, and success holds it
 * until invalidation or final payload release. Both use the same hardened Core
 * reader and reject profiles whose runtime is unavailable.
 */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_payload_open_copy(
    const fdb_payload_v1_spec_t* spec,
    const uint8_t* bytes,
    uint64_t byte_count,
    const fdb_payload_v1_open_options_t* options,
    fdb_payload_v1_payload_t** out_payload,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_payload_open_external(
    const fdb_payload_v1_spec_t* spec,
    const uint8_t* bytes,
    uint64_t byte_count,
    const fdb_payload_v1_backing_v1_t* backing,
    void* owner_token,
    const fdb_payload_v1_open_options_t* options,
    fdb_payload_v1_payload_t** out_payload,
    fdb_payload_v1_error_t** out_error);
/* Payload handles are immutable, thread-safe, and atomically retained. */
FDB_PAYLOAD_API void fdb_payload_v1_payload_retain(
    fdb_payload_v1_payload_t* payload);
FDB_PAYLOAD_API void fdb_payload_v1_payload_release(
    fdb_payload_v1_payload_t* payload);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_payload_sha256(
    const fdb_payload_v1_payload_t* payload,
    uint8_t out_digest[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_payload_profile(
    const fdb_payload_v1_payload_t* payload,
    fdb_payload_v1_profile_t* out_profile,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_payload_execution_report(
    const fdb_payload_v1_payload_t* payload,
    fdb_payload_v1_execution_report_t* out_report,
    fdb_payload_v1_error_t** out_error);
/*
 * Digest, profile, and a built payload's execution report remain queryable
 * after invalidation. Payloads created by open have no execution report and
 * return PLAN_STATE instead of fabricating one.
 *
 * binary_blob is a fresh independent copy made under a checked access pin. It
 * survives payload invalidation/release; requesting a new copy after
 * invalidation returns VIEW_INVALIDATED.
 */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_payload_binary_blob(
    const fdb_payload_v1_payload_t* payload,
    fdb_payload_v1_blob_t** out_blob,
    fdb_payload_v1_error_t** out_error);
/*
 * Invalidation is idempotent: it prevents new backing access, drains active
 * access pins, releases external backing outside the barrier lock, and returns
 * only after that release completes.
 */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_payload_invalidate(
    fdb_payload_v1_payload_t* payload,
    fdb_payload_v1_error_t** out_error);

/*
 * Compiles the borrowed source byte span before returning. A null source is
 * valid only when source_size is zero. A null options pointer selects V1
 * defaults. A supplied options structure must declare at least the V1 prefix;
 * a larger future tail is ignored. On success, out_spec receives one owned,
 * immutable, thread-safe handle.
 */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_compile_json(
    const uint8_t* source,
    uint64_t source_size,
    const fdb_payload_v1_compile_options_t* options,
    fdb_payload_v1_spec_t** out_spec,
    fdb_payload_v1_error_t** out_error);

/* Null is accepted. Each retain owns one matching release. */
FDB_PAYLOAD_API void fdb_payload_v1_spec_retain(fdb_payload_v1_spec_t* spec);
FDB_PAYLOAD_API void fdb_payload_v1_spec_release(fdb_payload_v1_spec_t* spec);

/*
 * Immutable spec queries are thread-safe. Each blob output is a fresh owned
 * handle whose bytes remain valid independently of the spec. Digest, profile,
 * capability, count, and index outputs are copied. A capabilities structure
 * must declare at least the V1 prefix; only that prefix is written and a
 * larger future tail is preserved.
 */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_canonical_json(
    const fdb_payload_v1_spec_t* spec,
    fdb_payload_v1_blob_t** out_blob,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_sha256(
    const fdb_payload_v1_spec_t* spec,
    uint8_t out_digest[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_manifest_json(
    const fdb_payload_v1_spec_t* spec,
    fdb_payload_v1_blob_t** out_blob,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_profile(
    const fdb_payload_v1_spec_t* spec,
    fdb_payload_v1_profile_t* out_profile,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_capabilities(
    const fdb_payload_v1_spec_t* spec,
    fdb_payload_v1_capabilities_t* out_capabilities,
    fdb_payload_v1_error_t** out_error);

FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_entry_count(
    const fdb_payload_v1_spec_t* spec,
    uint32_t* out_count,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_entry_id(
    const fdb_payload_v1_spec_t* spec,
    uint32_t entry_index,
    fdb_payload_v1_blob_t** out_id,
    fdb_payload_v1_error_t** out_error);
/* ID bytes are borrowed only for the duration of the call. */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_entry_index(
    const fdb_payload_v1_spec_t* spec,
    const uint8_t* id,
    uint64_t id_size,
    uint32_t* out_entry_index,
    fdb_payload_v1_error_t** out_error);

FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_count(
    const fdb_payload_v1_spec_t* spec,
    uint32_t* out_count,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_id(
    const fdb_payload_v1_spec_t* spec,
    uint32_t component_index,
    fdb_payload_v1_blob_t** out_id,
    fdb_payload_v1_error_t** out_error);
/* ID bytes are borrowed only for the duration of the call. */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_index(
    const fdb_payload_v1_spec_t* spec,
    const uint8_t* id,
    uint64_t id_size,
    uint32_t* out_component_index,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_spec_component_field_count(
    const fdb_payload_v1_spec_t* spec,
    uint32_t component_index,
    uint32_t* out_count,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_spec_component_field_id(
    const fdb_payload_v1_spec_t* spec,
    uint32_t component_index,
    uint32_t field_index,
    fdb_payload_v1_blob_t** out_id,
    fdb_payload_v1_error_t** out_error);
/* ID bytes are borrowed only for the duration of the call. */
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_spec_component_field_index(
    const fdb_payload_v1_spec_t* spec,
    uint32_t component_index,
    const uint8_t* id,
    uint64_t id_size,
    uint32_t* out_field_index,
    fdb_payload_v1_error_t** out_error);

/* Schema queries are thread-safe. Each blob is independent; digest is copied. */
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_source_schema_json(
    fdb_payload_v1_blob_t** out_blob,
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_source_schema_sha256(
    uint8_t out_digest[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error);

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
