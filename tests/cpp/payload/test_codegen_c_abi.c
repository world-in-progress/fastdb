#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <fastdb_payload.h>

_Static_assert(sizeof(fdb_payload_v1_codegen_options_t) ==
                   FDB_PAYLOAD_V1_CODEGEN_OPTIONS_V1_SIZE,
               "codegen options ABI size");
_Static_assert(FDB_PAYLOAD_V1_CODEGEN_OPTIONS_V1_SIZE == UINT32_C(48),
               "codegen options V1 size");
_Static_assert(offsetof(fdb_payload_v1_codegen_options_t, reserved) ==
                   UINT32_C(24),
               "codegen options reserved tail offset");

static int all_zero(const uint8_t *bytes, size_t size) {
  size_t index = 0;
  for (index = 0; index < size; ++index) {
    if (bytes[index] != UINT8_C(0)) {
      return 0;
    }
  }
  return 1;
}

static int all_equal(const uint8_t *bytes, size_t size, uint8_t expected) {
  size_t index = 0;
  for (index = 0; index < size; ++index) {
    if (bytes[index] != expected) {
      return 0;
    }
  }
  return 1;
}

static void release_error(fdb_payload_v1_error_t **error) {
  fdb_payload_v1_error_release(*error);
  *error = (fdb_payload_v1_error_t *)0;
}

int main(void) {
  struct extended_codegen_options {
    fdb_payload_v1_codegen_options_t prefix;
    uint8_t unknown_tail[16];
  } extended;
  static const char source[] =
      "{\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\","
      "\"entries\":[],\"components\":[]}";
  static const fdb_payload_v1_codegen_target_t targets[] = {
      FDB_PAYLOAD_CODEGEN_TARGET_CPP,
      FDB_PAYLOAD_CODEGEN_TARGET_RUST,
      FDB_PAYLOAD_CODEGEN_TARGET_PYTHON,
      FDB_PAYLOAD_CODEGEN_TARGET_TYPESCRIPT,
  };
  fdb_payload_v1_codegen_options_t options = {0};
  fdb_payload_v1_spec_t *spec = (fdb_payload_v1_spec_t *)0;
  fdb_payload_v1_error_t *error = (fdb_payload_v1_error_t *)0;
  fdb_payload_v1_status_t status = UINT32_C(0);
  size_t target_index = 0;

  fdb_payload_v1_codegen_options_init(&options);
  if (options.struct_size != FDB_PAYLOAD_V1_CODEGEN_OPTIONS_V1_SIZE ||
      options.flags != UINT32_C(0) || options.max_artifacts != UINT64_C(16) ||
      options.max_total_bytes !=
          UINT64_C(16) * UINT64_C(1024) * UINT64_C(1024) ||
      options.reserved[0] != UINT64_C(0) ||
      options.reserved[1] != UINT64_C(0) ||
      options.reserved[2] != UINT64_C(0)) {
    return 1;
  }
  fdb_payload_v1_codegen_options_init((fdb_payload_v1_codegen_options_t *)0);
  memset(&extended, 0xa5, sizeof(extended));
  fdb_payload_v1_codegen_options_init(&extended.prefix);
  if (extended.prefix.struct_size !=
          FDB_PAYLOAD_V1_CODEGEN_OPTIONS_V1_SIZE ||
      !all_equal(extended.unknown_tail, sizeof(extended.unknown_tail),
                 UINT8_C(0xa5))) {
    return 2;
  }

  status = fdb_payload_v1_spec_compile_json(
      (const uint8_t *)source, (uint64_t)(sizeof(source) - 1U),
      (const fdb_payload_v1_compile_options_t *)0, &spec, &error);
  if (status != UINT32_C(0) || spec == (fdb_payload_v1_spec_t *)0 ||
      error != (fdb_payload_v1_error_t *)0) {
    return 3;
  }

  for (target_index = 0; target_index < sizeof(targets) / sizeof(targets[0]);
       ++target_index) {
    fdb_payload_v1_codegen_result_t *result =
        (fdb_payload_v1_codegen_result_t *)0;
    fdb_payload_v1_blob_t *path = (fdb_payload_v1_blob_t *)0;
    fdb_payload_v1_blob_t *bytes = (fdb_payload_v1_blob_t *)0;
    uint64_t count = UINT64_C(0);
    fdb_payload_v1_artifact_kind_t kind = UINT32_C(0);
    uint8_t digest[FDB_PAYLOAD_V1_SHA256_SIZE] = {0};

    status = fdb_payload_v1_spec_codegen(spec, targets[target_index], &options,
                                         &result, &error);
    if (status != UINT32_C(0) ||
        result == (fdb_payload_v1_codegen_result_t *)0 ||
        error != (fdb_payload_v1_error_t *)0) {
      return 4;
    }
    fdb_payload_v1_codegen_result_retain(result);
    status =
        fdb_payload_v1_codegen_result_artifact_count(result, &count, &error);
    if (status != UINT32_C(0) || count != UINT64_C(1) ||
        error != (fdb_payload_v1_error_t *)0) {
      return 5;
    }
    status = fdb_payload_v1_codegen_result_artifact_relative_path(
        result, UINT64_C(0), &path, &error);
    if (status != UINT32_C(0) || path == (fdb_payload_v1_blob_t *)0 ||
        fdb_payload_v1_blob_size(path) == UINT64_C(0) ||
        fdb_payload_v1_blob_data(path) == (const uint8_t *)0 ||
        error != (fdb_payload_v1_error_t *)0) {
      return 6;
    }
    status = fdb_payload_v1_codegen_result_artifact_kind(result, UINT64_C(0),
                                                         &kind, &error);
    if (status != UINT32_C(0) || kind != FDB_PAYLOAD_ARTIFACT_SOURCE ||
        error != (fdb_payload_v1_error_t *)0) {
      return 7;
    }
    status = fdb_payload_v1_codegen_result_artifact_bytes(result, UINT64_C(0),
                                                          &bytes, &error);
    if (status != UINT32_C(0) || bytes == (fdb_payload_v1_blob_t *)0 ||
        fdb_payload_v1_blob_size(bytes) == UINT64_C(0) ||
        fdb_payload_v1_blob_data(bytes) == (const uint8_t *)0 ||
        error != (fdb_payload_v1_error_t *)0) {
      return 8;
    }
    status = fdb_payload_v1_codegen_result_artifact_sha256(result, UINT64_C(0),
                                                           digest, &error);
    if (status != UINT32_C(0) || all_zero(digest, sizeof(digest)) ||
        error != (fdb_payload_v1_error_t *)0) {
      return 9;
    }

    fdb_payload_v1_codegen_result_release(result);
    if (fdb_payload_v1_blob_size(path) == UINT64_C(0) ||
        fdb_payload_v1_blob_size(bytes) == UINT64_C(0)) {
      return 10;
    }
    fdb_payload_v1_codegen_result_release(result);
    fdb_payload_v1_blob_release(path);
    fdb_payload_v1_blob_release(bytes);
  }

  {
    fdb_payload_v1_codegen_result_t *result =
        (fdb_payload_v1_codegen_result_t *)(uintptr_t)1;
    status = fdb_payload_v1_spec_codegen(
        spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP | FDB_PAYLOAD_CODEGEN_TARGET_RUST,
        &options, &result, &error);
    if (status != FDB_PAYLOAD_E_UNSUPPORTED_TARGET ||
        result != (fdb_payload_v1_codegen_result_t *)0 ||
        error == (fdb_payload_v1_error_t *)0 ||
        fdb_payload_v1_error_code(error) != status) {
      return 11;
    }
    release_error(&error);
  }

  {
    fdb_payload_v1_codegen_result_t *result =
        (fdb_payload_v1_codegen_result_t *)(uintptr_t)1;
    fdb_payload_v1_codegen_options_t invalid = options;
    invalid.struct_size = UINT32_C(0);
    status = fdb_payload_v1_spec_codegen(spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP,
                                         &invalid, &result, &error);
    if (status != FDB_PAYLOAD_E_UNSUPPORTED_ABI ||
        result != (fdb_payload_v1_codegen_result_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 12;
    }
    release_error(&error);
  }

  {
    fdb_payload_v1_codegen_result_t *result =
        (fdb_payload_v1_codegen_result_t *)(uintptr_t)1;
    fdb_payload_v1_codegen_options_t zero = options;
    zero.max_artifacts = UINT64_C(0);
    status = fdb_payload_v1_spec_codegen(spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP,
                                         &zero, &result, &error);
    if (status != FDB_PAYLOAD_E_GENERATOR_FAILED ||
        result != (fdb_payload_v1_codegen_result_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 13;
    }
    release_error(&error);

    zero = options;
    zero.max_total_bytes = UINT64_C(0);
    result = (fdb_payload_v1_codegen_result_t *)(uintptr_t)1;
    status = fdb_payload_v1_spec_codegen(spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP,
                                         &zero, &result, &error);
    if (status != FDB_PAYLOAD_E_GENERATOR_FAILED ||
        result != (fdb_payload_v1_codegen_result_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 14;
    }
    release_error(&error);
  }

  {
    fdb_payload_v1_codegen_result_t *result =
        (fdb_payload_v1_codegen_result_t *)(uintptr_t)1;
    status = fdb_payload_v1_spec_codegen(
        (const fdb_payload_v1_spec_t *)0, FDB_PAYLOAD_CODEGEN_TARGET_CPP,
        &options, &result, (fdb_payload_v1_error_t **)0);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT ||
        result != (fdb_payload_v1_codegen_result_t *)0) {
      return 15;
    }
  }

  {
    uint64_t count = UINT64_MAX;
    fdb_payload_v1_blob_t *path = (fdb_payload_v1_blob_t *)(uintptr_t)1;
    fdb_payload_v1_blob_t *bytes = (fdb_payload_v1_blob_t *)(uintptr_t)1;
    fdb_payload_v1_artifact_kind_t kind = UINT32_MAX;
    uint8_t digest[FDB_PAYLOAD_V1_SHA256_SIZE];
    memset(digest, 0xa5, sizeof(digest));

    status = fdb_payload_v1_codegen_result_artifact_count(
        (const fdb_payload_v1_codegen_result_t *)0, &count,
        (fdb_payload_v1_error_t **)0);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT || count != UINT64_C(0)) {
      return 30;
    }
    status = fdb_payload_v1_codegen_result_artifact_relative_path(
        (const fdb_payload_v1_codegen_result_t *)0, UINT64_C(0), &path,
        (fdb_payload_v1_error_t **)0);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT ||
        path != (fdb_payload_v1_blob_t *)0) {
      return 31;
    }
    status = fdb_payload_v1_codegen_result_artifact_kind(
        (const fdb_payload_v1_codegen_result_t *)0, UINT64_C(0), &kind,
        (fdb_payload_v1_error_t **)0);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT || kind != UINT32_C(0)) {
      return 32;
    }
    status = fdb_payload_v1_codegen_result_artifact_bytes(
        (const fdb_payload_v1_codegen_result_t *)0, UINT64_C(0), &bytes,
        (fdb_payload_v1_error_t **)0);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT ||
        bytes != (fdb_payload_v1_blob_t *)0) {
      return 33;
    }
    status = fdb_payload_v1_codegen_result_artifact_sha256(
        (const fdb_payload_v1_codegen_result_t *)0, UINT64_C(0), digest,
        (fdb_payload_v1_error_t **)0);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT ||
        !all_zero(digest, sizeof(digest))) {
      return 34;
    }
  }

  {
    fdb_payload_v1_codegen_result_t *result =
        (fdb_payload_v1_codegen_result_t *)0;
    fdb_payload_v1_blob_t *blob = (fdb_payload_v1_blob_t *)(uintptr_t)1;
    fdb_payload_v1_artifact_kind_t kind = UINT32_MAX;
    uint8_t digest[FDB_PAYLOAD_V1_SHA256_SIZE];
    memset(digest, 0xa5, sizeof(digest));
    status = fdb_payload_v1_spec_codegen(spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP,
                                         &options, &result, &error);
    if (status != UINT32_C(0)) {
      return 16;
    }
    status = fdb_payload_v1_codegen_result_artifact_relative_path(
        result, UINT64_C(1), &blob, &error);
    if (status != FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE ||
        blob != (fdb_payload_v1_blob_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 17;
    }
    release_error(&error);
    status = fdb_payload_v1_codegen_result_artifact_kind(result, UINT64_C(1),
                                                         &kind, &error);
    if (status != FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE || kind != UINT32_C(0) ||
        error == (fdb_payload_v1_error_t *)0) {
      return 18;
    }
    release_error(&error);
    status = fdb_payload_v1_codegen_result_artifact_sha256(result, UINT64_C(1),
                                                           digest, &error);
    if (status != FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE ||
        !all_zero(digest, sizeof(digest)) ||
        error == (fdb_payload_v1_error_t *)0) {
      return 19;
    }
    release_error(&error);
    blob = (fdb_payload_v1_blob_t *)(uintptr_t)1;
    status = fdb_payload_v1_codegen_result_artifact_bytes(
        result, UINT64_C(1), &blob, &error);
    if (status != FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE ||
        blob != (fdb_payload_v1_blob_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 20;
    }
    release_error(&error);
    fdb_payload_v1_codegen_result_release(result);
  }

  {
    fdb_payload_v1_codegen_result_t *result =
        (fdb_payload_v1_codegen_result_t *)(uintptr_t)1;
    fdb_payload_v1_codegen_options_t invalid = options;
    invalid.flags = UINT32_C(1);
    status = fdb_payload_v1_spec_codegen(spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP,
                                         &invalid, &result, &error);
    if (status != FDB_PAYLOAD_E_UNSUPPORTED_ABI ||
        result != (fdb_payload_v1_codegen_result_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 21;
    }
    release_error(&error);

    invalid = options;
    invalid.reserved[2] = UINT64_C(1);
    result = (fdb_payload_v1_codegen_result_t *)(uintptr_t)1;
    status = fdb_payload_v1_spec_codegen(spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP,
                                         &invalid, &result, &error);
    if (status != FDB_PAYLOAD_E_UNSUPPORTED_ABI ||
        result != (fdb_payload_v1_codegen_result_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 22;
    }
    release_error(&error);

    result = (fdb_payload_v1_codegen_result_t *)(uintptr_t)1;
    status = fdb_payload_v1_spec_codegen(
        spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP,
        (const fdb_payload_v1_codegen_options_t *)0, &result, &error);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT ||
        result != (fdb_payload_v1_codegen_result_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 23;
    }
    release_error(&error);
  }

  {
    fdb_payload_v1_codegen_result_t *result =
        (fdb_payload_v1_codegen_result_t *)0;
    memset(&extended, 0, sizeof(extended));
    fdb_payload_v1_codegen_options_init(&extended.prefix);
    extended.prefix.struct_size = (uint32_t)sizeof(extended);
    memset(extended.unknown_tail, 0xa5, sizeof(extended.unknown_tail));
    status = fdb_payload_v1_spec_codegen(
        spec, FDB_PAYLOAD_CODEGEN_TARGET_CPP, &extended.prefix, &result,
        &error);
    if (status != UINT32_C(0) ||
        result == (fdb_payload_v1_codegen_result_t *)0 ||
        error != (fdb_payload_v1_error_t *)0 ||
        !all_equal(extended.unknown_tail, sizeof(extended.unknown_tail),
                   UINT8_C(0xa5))) {
      return 24;
    }
    fdb_payload_v1_codegen_result_release(result);
  }

  {
    uint64_t count = UINT64_MAX;
    fdb_payload_v1_blob_t *path = (fdb_payload_v1_blob_t *)(uintptr_t)1;
    fdb_payload_v1_blob_t *bytes = (fdb_payload_v1_blob_t *)(uintptr_t)1;
    fdb_payload_v1_artifact_kind_t kind = UINT32_MAX;
    uint8_t digest[FDB_PAYLOAD_V1_SHA256_SIZE];
    memset(digest, 0xa5, sizeof(digest));

    status = fdb_payload_v1_codegen_result_artifact_count(
        (const fdb_payload_v1_codegen_result_t *)0, &count, &error);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT || count != UINT64_C(0) ||
        error == (fdb_payload_v1_error_t *)0) {
      return 25;
    }
    release_error(&error);
    status = fdb_payload_v1_codegen_result_artifact_relative_path(
        (const fdb_payload_v1_codegen_result_t *)0, UINT64_C(0), &path,
        &error);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT ||
        path != (fdb_payload_v1_blob_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 26;
    }
    release_error(&error);
    status = fdb_payload_v1_codegen_result_artifact_kind(
        (const fdb_payload_v1_codegen_result_t *)0, UINT64_C(0), &kind,
        &error);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT || kind != UINT32_C(0) ||
        error == (fdb_payload_v1_error_t *)0) {
      return 27;
    }
    release_error(&error);
    status = fdb_payload_v1_codegen_result_artifact_bytes(
        (const fdb_payload_v1_codegen_result_t *)0, UINT64_C(0), &bytes,
        &error);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT ||
        bytes != (fdb_payload_v1_blob_t *)0 ||
        error == (fdb_payload_v1_error_t *)0) {
      return 28;
    }
    release_error(&error);
    status = fdb_payload_v1_codegen_result_artifact_sha256(
        (const fdb_payload_v1_codegen_result_t *)0, UINT64_C(0), digest,
        &error);
    if (status != FDB_PAYLOAD_E_INVALID_ARGUMENT ||
        !all_zero(digest, sizeof(digest)) ||
        error == (fdb_payload_v1_error_t *)0) {
      return 29;
    }
    release_error(&error);
  }

  fdb_payload_v1_codegen_result_retain((fdb_payload_v1_codegen_result_t *)0);
  fdb_payload_v1_codegen_result_release((fdb_payload_v1_codegen_result_t *)0);
  fdb_payload_v1_spec_release(spec);
  return 0;
}
