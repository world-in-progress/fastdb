# FastDB Portable Payload Core Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the first end-to-end FastDB 0.2.0 portable-payload slice: one C++ Core that strictly compiles the complete `fastdb.payload.v1` source model, produces normalized RFC 8785 canonical bytes and SHA-256 identity, derives resolved indexes/manifest/capabilities, and exposes those facts through an owned-error, opaque-handle C ABI plus a thin C++ RAII facade.

**Architecture:** Vendored, pinned JSON/number-format/SHA primitives feed a one-way Core pipeline: duplicate-preserving JSON document -> exact typed parse -> normalization -> semantic resolution/profile validation -> Core-owned canonical JSON and digest -> immutable `CompiledSpec`. The public boundary is `fastdb_payload.h`; every query projects Core-owned results and no binding parses or hashes the schema independently. This slice deliberately stops before payload binary layout, builder/plan/backing, open/view/materialization, graph runtime, bindings, and code generation, while recording those temporary implementation gaps in a FastDB-owned issue so they cannot be mistaken for completed 0.2.0 behavior.

**Tech Stack:** C++17, C11 ABI smoke tests, CMake 3.16-3.31, CTest, [yyjson 0.12.0](https://github.com/ibireme/yyjson/tree/0.12.0), [double-conversion 3.4.0](https://github.com/google/double-conversion/tree/v3.4.0), [PicoSHA2 1.0.1](https://github.com/okdshin/PicoSHA2/tree/v1.0.1), RFC 8785/JCS vectors, SHA-256 vectors, libFuzzer, AddressSanitizer, UndefinedBehaviorSanitizer, GitHub Actions.

## Global Constraints

- The accepted [portable payload design](../specs/2026-07-16-portable-payload-foundation-design.md), [ADR-0001](../../decisions/0001-portable-payload-core-authority.md), and [Issue 0001](../../issues/0001-portable-payload-deferred-capabilities.md) remain normative.
- FastDB C++ Core is the only parser, normalizer, JCS serializer, digest authority, profile validator, resolved-model authority, and manifest producer.
- Do not add C-Two CRM, contract assembly, route, relay, transport, lease, lifecycle, or Toodle concepts anywhere in this repository.
- Implement the complete declared V1 source algebra in this slice: `bool,u8,u16,u32,i32,u8n,u16n,f32,f64,str,wstr,bytes,component,ref,list`, explicit canonical nullability, `record.v1`, and `object_graph.v1` compile-time semantics.
- Use exact-width public C types. `fastdb_payload.h` must not include `fastdb-config.h`, whose legacy `wx::i32` alias is unsigned, and must not expose `size_t`, `wchar_t`, compiler enums, STL types, exceptions, or Rust layout.
- Do not use `FetchContent`, package-manager lookup, or any build-time network access. Source distributions, wheels, native builds, and WASM builds consume checked-in dependency snapshots.
- Every external snapshot records source URL, tag, resolved commit, license, retained files, and update procedure. The reproducibility check must compare checked-in bytes with the pinned upstream commit.
- Unknown fields and duplicate keys fail at every object level. The compiler does not coerce strings, infer profiles, collapse duplicate members, or silently accept binding-specific type names such as `text`.
- Compile limits affect acceptance only; they never enter canonical bytes, payload digest, manifest indexes, or schema digest.
- Entry order and field order remain semantic. Source component order does not: components are sorted by ASCII ID before canonicalization and index assignment.
- RFC 8785 object-key ordering uses UTF-16 code units, not UTF-8 byte order or locale collation. Number formatting uses ECMAScript-compatible binary64 serialization and rejects non-finite source numbers.
- Every contract-conforming fallible C ABI call supplies a non-null `out_error`; every failure then returns the same non-zero status stored in an owned immutable error handle. There is no thread-local last error. A null error sink is an ABI misuse detected as described below, because no API can place an owned handle into an absent output location.
- All public immutable handles use atomic retain/release. A reference count can never wrap: reaching `UINT64_MAX` saturates the handle into an immortal state. Borrowed bytes remain valid until the owning handle is released; every such duration is documented in the header.
- This slice must report unavailable runtime operations truthfully. It must not advertise build, open, view, backing, materialization, codegen, or direct-build eligibility before those implementations land.
- A temporary implementation gap is not a 0.2.0 deferral. Track it in `docs/issues/0002-portable-payload-foundation-implementation-status.md`; keep Issue 0001 reserved for capabilities intentionally deferred beyond 0.2.0.
- Do not remove current call-db/`ColumnEngine` surfaces in this slice. Do not add new users either. Their clean-cut removal remains a later program gate and the open status issue must say so explicitly.
- Follow strict red -> green -> focused test -> broader test -> commit order for every task. Do not combine a task's red and green evidence into one unverifiable statement.
- Use `apply_patch` for hand edits. Preserve unrelated worktree changes and stop if the starting tree is not clean.

---

## Starting Point and Preflight

The plan was written against branch `socu/portable-payload-foundation` at design commit `282355d`.

- [ ] Run `git status --short --branch` and require a clean worktree on `socu/portable-payload-foundation`.
- [ ] Run `git rev-parse --short HEAD`; if it is not `282355d`, inspect every intervening commit and update file paths/interfaces in this plan before implementation.
- [ ] Read the accepted design, ADR-0001, Issue 0001, `AGENTS.md`, `fastcarto/CMakeLists.txt`, `fastcarto/fastdb/CMakeLists.txt`, `fastcarto/lib/CMakeLists.txt`, `setup.py`, `ts/embind/CMakeLists.txt`, and `.github/workflows/tests.yml`.
- [ ] Run the current native baseline:

  ```bash
  cmake -S fastcarto -B build/baseline -DBUILD_TESTING=OFF -DBUILD_TOOLS=OFF
  cmake --build build/baseline --parallel
  ```

  Expected: configure and build pass before portable-payload changes.

- [ ] Run `uv run pytest tests/python -q` and record the current pass count in the final implementation handoff.
- [ ] If Emscripten is active, run `bash ts/build-wasm.sh && npm --prefix ts/fastdb4ts run build && npm run test:ts`; otherwise record the missing `emcmake` prerequisite and rely on the required CI job added in Task 9 rather than claiming local WASM verification.

## Slice Boundary

### Delivered by this plan

- Strict duplicate-preserving UTF-8 JSON compilation with configurable resource limits.
- Exact `fastdb.payload.v1` source schema and typed Core model.
- Omitted `nullable` normalization, component sorting, semantic indexes, reference resolution, by-value cycle rejection, and profile validation.
- RFC 8785 canonical JSON, raw SHA-256 digest, lowercase digest text in the manifest, and official-vector coverage.
- Immutable `CompiledSpec`, source schema bytes/digest, resolved manifest, and truthful capability report.
- Stable spec/blob/error/query C ABI, pure-C compilation/link smoke, atomic lifetimes, and concurrent immutable queries.
- Header-only C++ RAII facade over that ABI.
- Shared golden corpus, fuzz target, native Linux x86-64 and macOS arm64 CI, and sanitizer gates.

### Explicitly not claimed by this plan

- `fastdb.payload.bin.v1` byte layout or encoder/decoder.
- `PayloadBuilder`, `BuildPlan`, final-backing callbacks, direct/staged execution, or `PayloadOwner`.
- Checked views, materialization, invalidation barriers, or payload open validation.
- Runtime record or object-graph values, including graph pools/cycles at the binary layer.
- Rust, Python, or TypeScript/WASM portable-payload projections.
- C++/Rust/Python/TypeScript artifact generation.
- Public call-db removal, `ColumnEngine` -> `RecordEngine`, packaging version 0.2.0, or a release claim.
- C-Two super-schema composition. That work belongs to a later C-Two-owned plan after FastDB's ABI and runtime close.

These are temporary implementation gaps, not permission to remove them from the 0.2.0 success criteria. Issue 0002 stays open until the complete program map closes.

## Program Map Beyond This Plan

| Program slice | Owner and result | Entry gate | Exit gate |
|---|---|---|---|
| P1. Core contract compiler/query ABI | FastDB; **this plan** | Accepted design/ADR and pinned dependencies | Full V1 specs compile/reject deterministically; Core returns canonical bytes, digest, manifest, indexes, capabilities, and stable errors through C/C++ APIs |
| P2. Record binary/runtime/lifetime | FastDB | P1 ABI and typed model frozen | Exact `fastdb.payload.bin.v1.md`; complete non-`ref` type algebra; record builder/plan/backing/build/open/view/materialize/invalidate; deterministic record goldens |
| P3. Object-graph runtime | FastDB | P2 binary/backing/lifetime contracts proven | Ordinary graph pools, roots, refs, cycles, build/open/view/materialize/invalidate pass; only Issue 0001 D1 direct-dynamic case remains deferred |
| P4. Projections and payload codegen | FastDB | P2/P3 runtime ABI frozen | C++/Rust/Python/TypeScript-WASM parity and deterministic four-target in-memory `ArtifactSet` generation |
| P5. Clean cut, release, and downstream composition | FastDB, then C-Two in its own repository | P1-P4 non-deferred criteria pass | FastDB public call-db/columnar authority removed, `RecordEngine` shipped, 0.2.0 gates pass; C-Two wraps FastDB inside `c-two.contract.v2` without duplicated semantics |

Do not write speculative P2-P5 public interfaces into this implementation. Write each following plan from the actual frozen output of its entry gate.

## Exact First-Slice Contract

### Canonical compile pipeline

```text
source bytes + compile limits
  -> strict UTF-8/RFC 8259 yyjson document
  -> iterative value/depth/duplicate-member audit
  -> exact object-shape parser
  -> typed source model
  -> nullable=false insertion
  -> component sort by ASCII ID
  -> ID/reference/profile/by-value-cycle resolution
  -> normalized Core JsonValue
  -> RFC 8785 JCS UTF-8 bytes
  -> SHA-256 bytes
  -> immutable CompiledSpec
  -> resolved manifest + capability report
```

Parsing and normalization must complete before any `CompiledSpec` becomes observable. No partially compiled handle crosses the ABI.

### Public C types

The first-slice public declarations in `fastcarto/fastdb/include/fastdb_payload.h` use this shape. Function comments carry nullability, ownership, and borrow-duration rules.

```c
#define FDB_PAYLOAD_V1_ABI_VERSION UINT32_C(1)
#define FDB_PAYLOAD_V1_SHA256_SIZE UINT32_C(32)

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
```

Rules:

- `flags` and every reserved field are zero in V1. Non-zero input returns `FDB_PAYLOAD_E_UNSUPPORTED_ABI`.
- `fdb_payload_v1_compile_options_init` fills `struct_size` and safe defaults: 16 MiB source, 1,000,000 JSON values, depth 128, 65,536 entries, 65,536 components, 65,536 fields per component, and 1,000,000 total fields.
- A null options pointer selects those defaults. A zero limit inside a supplied initialized struct also selects the corresponding default.
- JSON root depth is 1. The value count includes each object, array, string, number, Boolean, and null value; member names are not separate values.
- `record.v1` is profile constant 1 and `object_graph.v1` is profile constant 2. These are fixed-width integer macros, not C enums.
- Public names are `FDB_PAYLOAD_PROFILE_RECORD_V1`, `FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1`, `FDB_PAYLOAD_SEMANTIC_HAS_NULLABLE`, `FDB_PAYLOAD_SEMANTIC_HAS_LISTS`, `FDB_PAYLOAD_SEMANTIC_HAS_REFERENCES`, `FDB_PAYLOAD_SEMANTIC_HAS_VARIABLE_WIDTH`, `FDB_PAYLOAD_SEMANTIC_HAS_NORMALIZED_INTEGERS`, `FDB_PAYLOAD_OPERATION_COMPILE`, `FDB_PAYLOAD_OPERATION_QUERY`, and `FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED`.
- Fixed values are: record profile `UINT32_C(1)`, object-graph profile `UINT32_C(2)`; semantic bits `UINT64_C(1) << 0` through `<< 4` in the name order above; operation compile `UINT64_C(1) << 0`; operation query `UINT64_C(1) << 1`; direct-build not-evaluated `UINT32_C(0)`.
- `FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE` is 80 and `FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE` is 72. C and C++ smoke tests statically verify those sizes on every supported target.
- At P1 completion, operation bits contain only `FDB_PAYLOAD_OPERATION_COMPILE | FDB_PAYLOAD_OPERATION_QUERY`; codegen target bits are zero; direct-build status is `FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED`.
- P2-P4 may add operation/codegen bits or change direct-build status for a spec without changing its payload digest. Unknown future bits must be ignored by consumers.

### Public C functions

```c
FDB_PAYLOAD_API uint32_t fdb_payload_v1_abi_version(void);
FDB_PAYLOAD_API void fdb_payload_v1_compile_options_init(
    fdb_payload_v1_compile_options_t *options);
FDB_PAYLOAD_API void fdb_payload_v1_capabilities_init(
    fdb_payload_v1_capabilities_t *capabilities);

FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_compile_json(
    const uint8_t *source,
    uint64_t source_size,
    const fdb_payload_v1_compile_options_t *options,
    fdb_payload_v1_spec_t **out_spec,
    fdb_payload_v1_error_t **out_error);

FDB_PAYLOAD_API void fdb_payload_v1_spec_retain(fdb_payload_v1_spec_t *spec);
FDB_PAYLOAD_API void fdb_payload_v1_spec_release(fdb_payload_v1_spec_t *spec);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_canonical_json(
    const fdb_payload_v1_spec_t *spec,
    fdb_payload_v1_blob_t **out_blob,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_sha256(
    const fdb_payload_v1_spec_t *spec,
    uint8_t out_digest[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_manifest_json(
    const fdb_payload_v1_spec_t *spec,
    fdb_payload_v1_blob_t **out_blob,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_profile(
    const fdb_payload_v1_spec_t *spec,
    fdb_payload_v1_profile_t *out_profile,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_capabilities(
    const fdb_payload_v1_spec_t *spec,
    fdb_payload_v1_capabilities_t *out_capabilities,
    fdb_payload_v1_error_t **out_error);

FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_entry_count(
    const fdb_payload_v1_spec_t *spec,
    uint32_t *out_count,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_entry_id(
    const fdb_payload_v1_spec_t *spec,
    uint32_t entry_index,
    fdb_payload_v1_blob_t **out_id,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_entry_index(
    const fdb_payload_v1_spec_t *spec,
    const uint8_t *id,
    uint64_t id_size,
    uint32_t *out_entry_index,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_count(
    const fdb_payload_v1_spec_t *spec,
    uint32_t *out_count,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_id(
    const fdb_payload_v1_spec_t *spec,
    uint32_t component_index,
    fdb_payload_v1_blob_t **out_id,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_index(
    const fdb_payload_v1_spec_t *spec,
    const uint8_t *id,
    uint64_t id_size,
    uint32_t *out_component_index,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_field_count(
    const fdb_payload_v1_spec_t *spec,
    uint32_t component_index,
    uint32_t *out_count,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_field_id(
    const fdb_payload_v1_spec_t *spec,
    uint32_t component_index,
    uint32_t field_index,
    fdb_payload_v1_blob_t **out_id,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_spec_component_field_index(
    const fdb_payload_v1_spec_t *spec,
    uint32_t component_index,
    const uint8_t *id,
    uint64_t id_size,
    uint32_t *out_field_index,
    fdb_payload_v1_error_t **out_error);

FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_source_schema_json(
    fdb_payload_v1_blob_t **out_blob,
    fdb_payload_v1_error_t **out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t fdb_payload_v1_source_schema_sha256(
    uint8_t out_digest[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t **out_error);

FDB_PAYLOAD_API void fdb_payload_v1_blob_retain(fdb_payload_v1_blob_t *blob);
FDB_PAYLOAD_API void fdb_payload_v1_blob_release(fdb_payload_v1_blob_t *blob);
FDB_PAYLOAD_API const uint8_t *fdb_payload_v1_blob_data(
    const fdb_payload_v1_blob_t *blob);
FDB_PAYLOAD_API uint64_t fdb_payload_v1_blob_size(
    const fdb_payload_v1_blob_t *blob);

FDB_PAYLOAD_API void fdb_payload_v1_error_retain(fdb_payload_v1_error_t *error);
FDB_PAYLOAD_API void fdb_payload_v1_error_release(fdb_payload_v1_error_t *error);
FDB_PAYLOAD_API uint32_t fdb_payload_v1_error_code(
    const fdb_payload_v1_error_t *error);
FDB_PAYLOAD_API void fdb_payload_v1_error_symbol(
    const fdb_payload_v1_error_t *error,
    const uint8_t **out_data,
    uint64_t *out_size);
FDB_PAYLOAD_API void fdb_payload_v1_error_path(
    const fdb_payload_v1_error_t *error,
    const uint8_t **out_data,
    uint64_t *out_size);
FDB_PAYLOAD_API void fdb_payload_v1_error_message(
    const fdb_payload_v1_error_t *error,
    const uint8_t **out_data,
    uint64_t *out_size);
FDB_PAYLOAD_API void fdb_payload_v1_error_details_json(
    const fdb_payload_v1_error_t *error,
    const uint8_t **out_data,
    uint64_t *out_size);
```

- ID input is `const uint8_t *id, uint64_t id_size`.
- Counts/indexes are `uint32_t` because compile limits cap each collection to `UINT32_MAX` or lower.
- Field queries accept `uint32_t component_index` before the field ID/index.
- ID getters return owned blobs so no caller depends on a nested string's internal address.
- Error string getters write `const uint8_t **out_data, uint64_t *out_size`; the bytes are borrowed until the error handle's next release to zero.
- `out_error` is a mandatory sink for every fallible function. A null sink returns 7001 without dereferencing it; this meta-contract misuse is the only case in which the caller made delivery of an owned error impossible. All other failures return an owned error whose code equals the status.
- With a non-null error sink, every fallible function clears all value outputs and `*out_error` before work. Success leaves `*out_error == NULL`; failure leaves value outputs empty and returns an owned error whose code equals the status.
- `source == NULL && source_size != 0`, null required outputs, bad `struct_size`, and non-zero reserved fields return an ABI-domain error instead of dereferencing invalid input.

### C++ facade shape

`fastdb_payload.hpp` adds ownership ergonomics only. Its P1 public shape is:

```cpp
namespace fastdb::payload::v1 {

struct ByteView {
    const std::uint8_t* data;
    std::uint64_t size;
};

enum class Profile : std::uint32_t {
    record_v1 = FDB_PAYLOAD_PROFILE_RECORD_V1,
    object_graph_v1 = FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1,
};

class Blob {
public:
    Blob(const Blob&);
    Blob(Blob&&) noexcept;
    Blob& operator=(const Blob&);
    Blob& operator=(Blob&&) noexcept;
    ~Blob();
    ByteView bytes() const noexcept;
    std::string_view as_string_view() const noexcept;
};

class PayloadError : public std::runtime_error {
public:
    std::uint32_t code() const noexcept;
    std::string_view symbol() const noexcept;
    std::string_view path() const noexcept;
    std::string_view details_json() const noexcept;
};

struct CompileOptions {
    CompileOptions() noexcept;
    fdb_payload_v1_compile_options_t value;
};

struct Capabilities {
    fdb_payload_v1_capabilities_t value;
};

class CompiledSpec {
public:
    static CompiledSpec compile(
        std::string_view source,
        const CompileOptions& options = CompileOptions{});
    CompiledSpec(const CompiledSpec&);
    CompiledSpec(CompiledSpec&&) noexcept;
    CompiledSpec& operator=(const CompiledSpec&);
    CompiledSpec& operator=(CompiledSpec&&) noexcept;
    ~CompiledSpec();

    Blob canonical_json() const;
    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> sha256() const;
    Blob manifest_json() const;
    Profile profile() const;
    Capabilities capabilities() const;
    std::uint32_t entry_count() const;
    Blob entry_id(std::uint32_t index) const;
    std::uint32_t entry_index(std::string_view id) const;
    std::uint32_t component_count() const;
    Blob component_id(std::uint32_t index) const;
    std::uint32_t component_index(std::string_view id) const;
    std::uint32_t component_field_count(std::uint32_t component_index) const;
    Blob component_field_id(
        std::uint32_t component_index,
        std::uint32_t field_index) const;
    std::uint32_t component_field_index(
        std::uint32_t component_index,
        std::string_view id) const;
};

Blob source_schema();
std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> source_schema_sha256();

}  // namespace fastdb::payload::v1
```

The implementation hides raw handles in private members/constructors. `PayloadError` copies immutable error fields before releasing the C handle, making the exception normally copyable. `Blob::as_string_view()` is a non-owning view valid for the `Blob` lifetime; P1 uses it only for Core-guaranteed UTF-8 JSON and IDs.

### Error allocation

Keep the accepted 1000-6999 and 9001 codes. Add the first implementation's missing ABI/query range to the governing design when the header lands:

| Code | Symbol | Meaning |
|---|---|---|
| 1010 | `SPEC_RESOURCE_LIMIT` | Strict source compilation exceeded a caller/default compile limit |
| 7001 | `INVALID_ARGUMENT` | Required ABI pointer/span/output precondition failed |
| 7002 | `UNSUPPORTED_ABI` | Input `struct_size`, flag, or reserved-field contract is unsupported |
| 7003 | `NOT_FOUND` | An ID lookup did not match a compiled entry/component/field |
| 7004 | `INDEX_OUT_OF_RANGE` | A numeric compiled index is outside its collection |

Errors contain immutable `code`, `symbol`, RFC 6901 `path`, diagnostic `message`, and JCS-canonical `details_json`. An allocation failure while constructing an error returns an immortal, release-safe `FDB_PAYLOAD_E_ALLOCATION_FAILED` emergency handle, so the ABI never violates the owned-error rule.

### Resolved manifest

`fdb_payload_v1_spec_manifest_json` returns JCS-canonical bytes conforming to `schemas/fastdb.payload.manifest.v1.schema.json`. The exact P1 object is:

```json
{
  "schema": "fastdb.payload.manifest.v1",
  "payload": {
    "schema": "fastdb.payload.v1",
    "profile": "record.v1",
    "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
  },
  "entries": [
    {
      "index": 0,
      "id": "points",
      "cardinality": "many",
      "type": {
        "kind": "component",
        "nullable": false,
        "id": "Point",
        "component_index": 0
      }
    }
  ],
  "components": [
    {
      "index": 0,
      "id": "Point",
      "kind": "record",
      "fields": [
        {
          "index": 0,
          "id": "x",
          "type": {"kind": "f64", "nullable": false}
        }
      ]
    }
  ],
  "facts": {
    "has_lists": false,
    "has_normalized_integers": false,
    "has_nullable": false,
    "has_references": false,
    "has_variable_width": false
  },
  "capabilities": {
    "operations": ["compile", "query"],
    "codegen_targets": [],
    "direct_build": {
      "status": "not_evaluated",
      "reason": "runtime_slice_not_implemented"
    }
  }
}
```

The all-zero `sha256` string above demonstrates the required 64-character shape; the Core inserts the actual compiled payload digest.

Manifest type nodes repeat normalized type facts. `component` adds `component_index`; `ref` adds `target_component_index`; `list` recursively contains `items`; normalized integers retain binary64 `min` and `max`. This manifest is informative derived data and is not included in the payload digest.

## File and Responsibility Map

### Build, vendoring, schemas, and tracking

| Path | Responsibility |
|---|---|
| `tools/vendor_portable_payload_deps.sh` | Reproduce or verify immutable dependency snapshots from pinned commits without build-time network access |
| `tools/generate_embedded_payload_schemas.py` | Deterministically embed checked-in schema source bytes; `--check` fails on drift and performs no writes |
| `tools/check_payload_abi_symbols.py` | Compare exported `fdb_payload_v1_*` symbols with a checked-in allowlist |
| `fastcarto/lib/yyjson/` | yyjson `src/yyjson.c`, `src/yyjson.h`, license, provenance |
| `fastcarto/lib/double-conversion/` | Eight upstream `.cc` files, required headers, license, provenance; built by FastDB's CMake rather than upstream CMake 3.29 project |
| `fastcarto/lib/picosha2/` | `picosha2.h`, license, provenance |
| `schemas/fastdb.payload.v1.schema.json` | Machine-readable authoring schema; Core typed parser remains execution authority |
| `schemas/fastdb.payload.v1.schema.sha256` | Lowercase SHA-256 of the Core-returned JCS schema bytes, independently checked as a downstream pin |
| `schemas/fastdb.payload.manifest.v1.schema.json` | Exact resolved-manifest contract |
| `schemas/README.md` | Authority, versioning, embedding, and digest rules |
| `docs/issues/0002-portable-payload-foundation-implementation-status.md` | Temporary gap, impact, program checklist, and closure criteria through 0.2.0 |
| `docs/issues/README.md` | Index Issue 0002 and distinguish milestone gaps from post-milestone deferrals |

### Core internals and public boundary

| Path | Responsibility |
|---|---|
| `fastcarto/fastdb/include/fastdb_payload.h` | Pure-C ABI authority for this slice |
| `fastcarto/fastdb/include/fastdb_payload.hpp` | Header-only C++17 RAII projection; no independent semantics |
| `fastcarto/fastdb/src/payload/json/JsonValue.hpp` | Owned JSON value tree used only for normalized output, manifests, and error details |
| `fastcarto/fastdb/src/payload/json/JsonPointer.{hpp,cpp}` | RFC 6901 escaping and immutable path construction |
| `fastcarto/fastdb/src/payload/json/JsonDocument.{hpp,cpp}` | yyjson ownership, strict parsing, iterative limits, duplicate-member audit, source cursors |
| `fastcarto/fastdb/src/payload/json/Jcs.{hpp,cpp}` | RFC 8785 serializer, UTF-16 key ordering, ECMAScript number rendering |
| `fastcarto/fastdb/src/payload/identity/Sha256.{hpp,cpp}` | One Core wrapper returning exactly 32 digest bytes and lowercase hex when requested |
| `fastcarto/fastdb/src/payload/error/Error.{hpp,cpp}` | Stable diagnostic object and code/symbol mapping |
| `fastcarto/fastdb/src/payload/error/Result.hpp` | Move-aware internal success/error carrier; no exception crosses ABI |
| `fastcarto/fastdb/src/payload/spec/Model.hpp` | Typed `Profile`, `Cardinality`, native `TypeKind`, `TypeNode`, `Entry`, `Field`, `Component`, `ResolvedSpec` |
| `fastcarto/fastdb/src/payload/spec/Parse.{hpp,cpp}` | Exact object-shape/type parser and omitted-nullability normalization |
| `fastcarto/fastdb/src/payload/spec/Resolve.{hpp,cpp}` | ID uniqueness, ASCII sorting/indexes, target resolution, cycle/profile checks, semantic facts |
| `fastcarto/fastdb/src/payload/spec/Manifest.{hpp,cpp}` | Exact derived manifest value and capabilities |
| `fastcarto/fastdb/src/payload/spec/SchemaRepository.{hpp,cpp}` | Embedded source/manifest schema parsing, Core JCS bytes, and schema digest |
| `fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc` | Generated raw schema byte arrays; never hand edited |
| `fastcarto/fastdb/src/payload/spec/CompiledSpec.{hpp,cpp}` | Immutable orchestration result and query indexes |
| `fastcarto/fastdb/src/payload/abi/Handles.hpp` | Atomic opaque handle storage and safe internal conversions |
| `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp` | Exception firewall, ABI validation, compile/query/error/blob functions |

### Tests and fixtures

| Path | Responsibility |
|---|---|
| `tests/cpp/CMakeLists.txt` | Named CTest targets, strict warnings, sanitizer/fuzz toggles |
| `tests/cpp/payload/TestSupport.hpp` | `require` helpers that remain active under `NDEBUG`, byte/hex fixture loading |
| `tests/cpp/payload/GoldenCorpus.{hpp,cpp}` | Shared indexed fixture loader |
| `tests/cpp/payload/test_dependency_smoke.cpp` | Prove pinned primitives provide duplicate retention, ECMAScript numbers, SHA-256 |
| `tests/cpp/payload/test_jcs.cpp` | Official and adversarial JCS/SHA vectors |
| `tests/cpp/payload/test_error.cpp` | Stable error fields, canonical details, emergency error, retain/release |
| `tests/cpp/payload/test_json_document.cpp` | UTF-8, duplicate paths, syntax, depth/value/source limits |
| `tests/cpp/payload/test_spec_parse.cpp` | Exact shapes, complete type algebra, defaults, order preservation |
| `tests/cpp/payload/test_spec_resolve.cpp` | IDs, indexes, refs, cycles, profiles, semantic facts |
| `tests/cpp/payload/test_compiled_spec.cpp` | End-to-end canonical/digest/manifest/schema golden results |
| `tests/cpp/payload/test_spec_abi.cpp` | C ABI success/failure/output clearing/lifetime/concurrency |
| `tests/cpp/payload/test_cpp_facade.cpp` | C++ RAII copy/move/error/query behavior |
| `tests/cpp/payload/test_c_header_smoke.c` | C11 header compile and full compile/query/release link smoke |
| `tests/fuzz/payload/fuzz_spec_compile.cpp` | Untrusted source/options fuzz entry with balanced releases |
| `tests/golden/payload/v1/` | Cross-language-ready JCS, valid spec, identity, manifest, and invalid-error corpus |
| `tests/abi/fastdb_payload_v1_symbols.txt` | Exact P1 exported symbol allowlist |

## Task 1: Vendor pinned primitives and establish the native test seam

**Files:**

- Create: `tools/vendor_portable_payload_deps.sh`
- Create: `fastcarto/lib/yyjson/src/yyjson.c`
- Create: `fastcarto/lib/yyjson/src/yyjson.h`
- Create: `fastcarto/lib/yyjson/LICENSE`
- Create: `fastcarto/lib/yyjson/UPSTREAM.md`
- Create: `fastcarto/lib/double-conversion/double-conversion/*.{h,cc}` from the explicit upstream allowlist
- Create: `fastcarto/lib/double-conversion/LICENSE`
- Create: `fastcarto/lib/double-conversion/UPSTREAM.md`
- Create: `fastcarto/lib/picosha2/picosha2.h`
- Create: `fastcarto/lib/picosha2/LICENSE`
- Create: `fastcarto/lib/picosha2/UPSTREAM.md`
- Modify: `fastcarto/lib/CMakeLists.txt`
- Modify: `fastcarto/CMakeLists.txt`
- Modify: `fastcarto/fastdb/CMakeLists.txt`
- Modify: `setup.py`
- Modify: `ts/embind/CMakeLists.txt`
- Modify: `MANIFEST.in`
- Create: `tests/cpp/CMakeLists.txt`
- Create: `tests/cpp/payload/TestSupport.hpp`
- Create: `tests/cpp/payload/test_dependency_smoke.cpp`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`
- Modify: `docs/issues/README.md`

**Interfaces produced:** `fastdb_yyjson`, `fastdb_double_conversion`, `fastdb_picosha2`, `BUILD_TESTING`, `FASTDB_ENABLE_SANITIZERS`, and a reusable `add_fastdb_payload_test` helper with explicit extra-library support.

- [ ] Write the first `test_dependency_smoke.cpp` so it includes the planned vendored headers and asserts:

  ```cpp
  require(duplicate_member_count(R"({"x":1,"x":2})") == 2);
  require(ecmascript_number(1e30) == "1e+30");
  require(sha256_hex("The quick brown fox jumps over the lazy dog") ==
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  ```

- [ ] Configure with `cmake -S fastcarto -B build/payload-red -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF`.

  Expected red: CMake cannot resolve the three planned dependency targets/headers and no `payload.dependency_smoke` test exists.

- [ ] Implement `tools/vendor_portable_payload_deps.sh` with `write` and `--check` modes and these immutable coordinates:

  ```text
  yyjson            tag 0.12.0  commit 8b4a38dc994a110abaec8a400615567bd996105f
  double-conversion tag v3.4.0  commit 9dd6227ee3e29807183e56877f0282aeb40b8b1e
  PicoSHA2          tag v1.0.1  commit 161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29
  ```

  The script must clone into an owned `mktemp -d`, verify `git rev-parse HEAD`, copy only the allowlisted files, reject symlinks, and either atomically replace its own vendor directories in `write` mode or compare them with `git diff --no-index --no-renames --` in `--check` mode.

- [ ] Retain exactly eight double-conversion translation units: `bignum.cc`, `bignum-dtoa.cc`, `cached-powers.cc`, `double-to-string.cc`, `fast-dtoa.cc`, `fixed-dtoa.cc`, `string-to-double.cc`, and `strtod.cc`. Retain exactly these headers: `bignum.h`, `bignum-dtoa.h`, `cached-powers.h`, `diy-fp.h`, `double-conversion.h`, `double-to-string.h`, `fast-dtoa.h`, `fixed-dtoa.h`, `ieee.h`, `string-to-double.h`, `strtod.h`, and `utils.h`. Do not call upstream `add_subdirectory`; its CMake minimum is newer than FastDB's 3.16 floor.
- [ ] Write `UPSTREAM.md` in each snapshot with URL, tag, full commit, license identifier, retained-file list, and the exact `tools/vendor_portable_payload_deps.sh --check` command.
- [ ] Add CMake targets:

  ```cmake
  add_library(fastdb_yyjson STATIC yyjson/src/yyjson.c)
  target_compile_definitions(fastdb_yyjson PRIVATE
      YYJSON_DISABLE_NON_STANDARD=1
      YYJSON_DISABLE_UTILS=1)
  set_target_properties(fastdb_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_library(fastdb_double_conversion STATIC ${DOUBLE_CONVERSION_SOURCES})
  set_target_properties(fastdb_double_conversion PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_library(fastdb_picosha2 INTERFACE)
  ```

- [ ] Add `include(CTest)` to `fastcarto/CMakeLists.txt` and, under `BUILD_TESTING`, add `../tests/cpp` with an explicit binary directory. Set `BUILD_TESTING=OFF` in `setup.py` and force it off before FastDB is added by `ts/embind/CMakeLists.txt`, so wheel and WASM production builds do not compile native test executables.
- [ ] Add `FASTDB_ENABLE_SANITIZERS` default `OFF`; when enabled under Clang/GCC, apply address+undefined sanitizer compile/link options to portable Core and its tests. Reject this option with a clear CMake error for unsupported compilers instead of silently doing nothing.
- [ ] Add a `tests/cpp/CMakeLists.txt` helper that names CTest cases `payload.<name>` and never uses C `assert`/C++ `assert`. `TestSupport.hpp::require` must print file, line, expression, and optional message before returning non-zero.
- [ ] Update `MANIFEST.in` to include schema `*.json`, `*.sha256`, and `*.md` files plus the vendored sources/licenses/provenance. The existing recursive `fastcarto/lib` rule remains, but an explicit schema rule prevents sdist omission.
- [ ] Update Issue 0002's P1 row from `Planned` to `In progress: vendored primitives and native test seam`; keep the current-surface section explicit that no portable payload API is shipped after Task 1.
- [ ] Run:

  ```bash
  tools/vendor_portable_payload_deps.sh --check
  cmake -S fastcarto -B build/payload -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF
  cmake --build build/payload --parallel
  ctest --test-dir build/payload -R '^payload\.dependency_smoke$' --output-on-failure
  ```

  Expected green: vendor comparison is clean and one dependency smoke test passes.

- [ ] Run `git diff --check` and inspect `git diff --stat` for unexpected generated or binary build outputs.
- [ ] Commit: `build: vendor portable payload primitives`

## Task 2: Add Core-owned JSON canonicalization and SHA-256 identity primitives

**Files:**

- Create: `fastcarto/fastdb/src/payload/json/JsonValue.hpp`
- Create: `fastcarto/fastdb/src/payload/json/JsonPointer.hpp`
- Create: `fastcarto/fastdb/src/payload/json/JsonPointer.cpp`
- Create: `fastcarto/fastdb/src/payload/json/Jcs.hpp`
- Create: `fastcarto/fastdb/src/payload/json/Jcs.cpp`
- Create: `fastcarto/fastdb/src/payload/identity/Sha256.hpp`
- Create: `fastcarto/fastdb/src/payload/identity/Sha256.cpp`
- Create: `tests/cpp/payload/test_jcs.cpp`
- Create: `tests/golden/payload/v1/jcs/UPSTREAM.md`
- Create: `tests/golden/payload/v1/jcs/rfc8785-values.input.json`
- Create: `tests/golden/payload/v1/jcs/rfc8785-values.canonical.hex`
- Create: `tests/golden/payload/v1/jcs/rfc8785-numbers.input.json`
- Create: `tests/golden/payload/v1/jcs/rfc8785-numbers.canonical.hex`
- Modify: `fastcarto/fastdb/CMakeLists.txt`
- Modify: `tests/cpp/CMakeLists.txt`

**Interfaces produced:** `JsonValue`, `JsonPointer`, `JcsFailure`, `jcs_serialize(const JsonValue&)`, `sha256(bytes)`, and `sha256_lower_hex`.

- [ ] Add failing tests for RFC 6901 escaping: `a/b` -> `/a~1b`, `m~n` -> `/m~0n`, array index append, and root empty pointer.
- [ ] Add failing JCS tests covering null/Boolean/string escapes, no slash escaping, control characters, non-ASCII UTF-8 preservation, no Unicode normalization, array-order preservation, and object-key order by UTF-16 code units.
- [ ] Add the RFC 8785 number corpus and require exact encodings for `0`, `-0` -> `0`, `1e+30`, `1e-7`, the integer/exponent boundary cases, minimum positive binary64, and maximum finite binary64. Require NaN and both infinities to return `JcsFailure::non_finite_number`; Task 3 maps that closed internal failure to stable error code 1009.
- [ ] In this task, construct the RFC vector `JsonValue` trees explicitly and compare them with the checked-in canonical hex. Task 4 must additionally parse the `.input.json` files through `JsonDocument` before JCS serialization, proving the source parser and serializer compose correctly without a test-only semantic parser.
- [ ] Run `cmake --build build/payload --parallel && ctest --test-dir build/payload -R '^payload\.jcs$' --output-on-failure`.

  Expected red: `JsonValue`, JCS, pointer, and SHA APIs do not exist.

- [ ] Implement `JsonValue` as a closed variant of null, Boolean, binary64 number, UTF-8 string, ordered array, and object member vector. Preserve insertion order in the model; sort a temporary member-reference vector only while serializing.
- [ ] Now that real payload sources exist, split `src/payload/*.cpp` files out of the existing recursive `fastdb` source glob into a `fastdb_payload_core` object target. Give only that target strict warnings (`/W4 /WX` on MSVC; `-Wall -Wextra -Wpedantic -Wconversion -Werror` elsewhere), C++17, position-independent code, internal include paths, and `FASTDB_PAYLOAD_BUILDING=1`. Add its objects to the existing `fastdb` shared/static target and link all three dependency targets privately.
- [ ] Validate every `JsonValue` string as strict UTF-8 before serialization. Sort object keys lexicographically by decoded UTF-16 code units, with shorter prefix first. Reject duplicate keys defensively even though source duplicate detection occurs earlier.
- [ ] Serialize numbers only through `double-conversion::DoubleToStringConverter::EcmaScriptConverter()`. Do not use streams, `std::to_chars`, locale formatting, yyjson output, or a binding runtime for canonical numbers.
- [ ] Serialize strings with RFC 8785/ECMAScript escaping: short escapes for backspace/tab/newline/form-feed/carriage-return, lowercase `\u00xx` for other U+0000-U+001F controls, quote/backslash escapes, and literal UTF-8 for other valid scalars.
- [ ] Wrap PicoSHA2 behind `identity/Sha256`; accept byte spans as `(const uint8_t*, uint64_t)` internally after checked conversion to iterator distance. Return `std::array<uint8_t,32>`; lowercase hex is a presentation helper, not a second digest.
- [ ] In `UPSTREAM.md`, identify the exact RFC 8785 sections/reference corpus used and explain why expected canonical bytes are stored as hexadecimal: fixture newlines cannot accidentally enter identity bytes.
- [ ] Run:

  ```bash
  cmake --build build/payload --parallel
  ctest --test-dir build/payload -R '^payload\.(dependency_smoke|jcs)$' --output-on-failure
  ```

  Expected green: dependency and JCS/SHA suites pass in Debug and Release configurations.

- [ ] Configure and run a Release check to prove tests do not disappear under `NDEBUG`:

  ```bash
  cmake -S fastcarto -B build/payload-release -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build build/payload-release --parallel
  ctest --test-dir build/payload-release -R '^payload\.(dependency_smoke|jcs)$' --output-on-failure
  ```

- [ ] Run `git diff --check`.
- [ ] Commit: `feat(core): add portable payload identity primitives`

## Task 3: Define owned errors, blobs, and the pure-C ABI base

**Files:**

- Create: `fastcarto/fastdb/include/fastdb_payload.h`
- Create: `fastcarto/fastdb/src/payload/error/Error.hpp`
- Create: `fastcarto/fastdb/src/payload/error/Error.cpp`
- Create: `fastcarto/fastdb/src/payload/error/Result.hpp`
- Create: `fastcarto/fastdb/src/payload/abi/Handles.hpp`
- Create: `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp`
- Create: `tests/cpp/payload/test_error.cpp`
- Create: `tests/cpp/payload/test_c_header_smoke.c`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md`

**Interfaces produced:** exact-width status/profile/constants, compile/capability structs and initializers, opaque spec/blob/error declarations, blob/error retain/release/query functions, and ABI version query.

- [ ] Write a C11 smoke program that includes only `<stdint.h>` plus `fastdb_payload.h`, initializes both public structs, checks ABI version 1 and zero reserved fields, then exits. Compile it as C, not C++.
- [ ] Add `_Static_assert(sizeof(fdb_payload_v1_compile_options_t) == FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE, "compile options ABI size");` and `_Static_assert(sizeof(fdb_payload_v1_capabilities_t) == FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE, "capabilities ABI size");` to the C smoke; add equivalent `static_assert` checks to C++ tests.
- [ ] Configure the smoke target with `C_STANDARD 11`, `C_STANDARD_REQUIRED ON`, and `LINKER_LANGUAGE CXX`: the translation unit must compile as pure C while the final executable links the C++ Core runtime correctly.
- [ ] Add failing error tests for every accepted error symbol plus 1010 and 7001-7004, RFC 6901 path bytes, diagnostic message, JCS-canonical details, atomic retain/release, and null-safe `release(NULL)`.
- [ ] Add an injected-allocation-failure test that requires a non-null immortal emergency error with code 5002 and release-safe behavior.
- [ ] Build the focused targets.

  Expected red: the public header, handle implementation, and exported C symbols do not exist.

- [ ] Define `FDB_PAYLOAD_API` inside `fastdb_payload.h` using only C-preprocessor platform checks and `FASTDB_PAYLOAD_BUILDING`. Do not include legacy FastDB headers.
- [ ] Use macro constants backed by `UINT32_C`/`UINT64_C`, not public C enum declarations. Add ABI/query errors 1010 and 7001-7004 to design section 12 as an implementation clarification without changing existing meanings.
- [ ] Implement internal immutable `Error` fields, `Result<T>`, and `Result<void>` so each result is either one value or one error. Error details are constructed as `JsonValue` and serialized by the single JCS implementation. Map `JcsFailure::non_finite_number`, `invalid_utf8`, and `duplicate_member` to 1009, 1001, and 1002 respectively; an impossible failure while canonicalizing Core-constructed error details becomes 9001 with canonical `{}` details.
- [ ] Implement opaque handle structs with `std::atomic<uint64_t>` reference counts. Retain uses a compare/exchange loop; a count at `UINT64_MAX` remains saturated and release becomes a no-op for that immortal handle. Ordinary release uses acquire-release semantics and deletes exactly once. The emergency allocation error starts immortal and stores code/symbol/path/message/`{}` details entirely in static literals, so creating it cannot allocate.
- [ ] Make public byte getters return pointer/length views into immutable owned strings/vectors. Null getter input returns null/zero and never dereferences.
- [ ] Catch `std::bad_alloc`, known internal diagnostics, `std::exception`, and unknown exceptions at every ABI function body. Map them respectively to allocation, exact known code, or 9001 internal errors. No exception crosses `extern "C"`.
- [ ] Run:

  ```bash
  cmake --build build/payload --parallel
  ctest --test-dir build/payload -R '^payload\.(c_header_smoke|error)$' --output-on-failure
  ```

  Expected green: the header compiles and links as C; error/blob lifetime tests pass.

- [ ] Inspect the preprocessed C header and verify no C++ namespace/type leaked:

  ```bash
  cc -std=c11 -E -Ifastcarto/fastdb/include tests/cpp/payload/test_c_header_smoke.c >/tmp/fastdb_payload_header.i
  rg -n '\b(size_t|wchar_t|std::|namespace|class|template|bool|enum|long)\b' fastcarto/fastdb/include/fastdb_payload.h
  ```

  Expected: preprocessing succeeds and `rg` returns no matches.

- [ ] Run `git diff --check`.
- [ ] Commit: `feat(core): add portable payload error ABI`

## Task 4: Add strict duplicate-aware JSON documents, limits, and embedded schemas

**Files:**

- Create: `fastcarto/fastdb/src/payload/json/JsonDocument.hpp`
- Create: `fastcarto/fastdb/src/payload/json/JsonDocument.cpp`
- Create: `schemas/fastdb.payload.v1.schema.json`
- Create: `schemas/README.md`
- Create: `tools/generate_embedded_payload_schemas.py`
- Create: `fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc`
- Create: `fastcarto/fastdb/src/payload/spec/SchemaRepository.hpp`
- Create: `fastcarto/fastdb/src/payload/spec/SchemaRepository.cpp`
- Create: `tests/cpp/payload/test_json_document.cpp`
- Modify: `tests/cpp/payload/test_jcs.cpp`
- Modify: `tests/cpp/CMakeLists.txt`

**Interfaces produced:** `JsonDocument::parse`, duplicate-safe cursors, compile limit enforcement, embedded source schema bytes, and Core-canonical source schema/digest.

- [ ] Add failing tests for empty/truncated JSON, trailing content, invalid UTF-8, invalid escape/surrogate, overflow number `1e9999`, duplicate root/nested/member keys, and source/value/depth limits.
- [ ] Require exact duplicate paths. For the second `id` in entry zero, assert path `/entries/0/id` and canonical details:

  ```json
  {"first_member_index":0,"key":"id","second_member_index":1}
  ```

- [ ] Add a test that parses the machine schema itself, verifies draft 2020-12 metadata and root `additionalProperties:false`, then asks `SchemaRepository` for canonical bytes and a 32-byte digest.
- [ ] Run the focused test.

  Expected red: strict document and schema repository APIs are absent.

- [ ] Parse with `yyjson_read_opts` using `YYJSON_READ_NOFLAG`; precheck source size and pointer/length conversion before calling yyjson. Never enable comments, trailing commas, inf/NaN, invalid Unicode, or duplicate-collapsing transforms.
- [ ] Traverse the yyjson tree iteratively before typed recursion. Count values, enforce root depth 1/max depth, and visit every object member in source order. For each object, detect the second duplicate while retaining the first and second member indexes.
- [ ] After the audit succeeds, provide a generic document-to-`JsonValue` conversion that maps every finite JSON number to binary64 and preserves array/member content. Use it for embedded schema and RFC-vector JCS tests only; normalized payload canonicalization must still originate from the typed spec model.
- [ ] Build JSON Pointer paths from source member names using `JsonPointer`; do not concatenate unescaped user text.
- [ ] Map yyjson syntax/UTF-8/number failures to 1001 or 1009 with stable details. Map configured source/value/depth limits to 1010 with `limit`, `actual`, and `kind` fields.
- [ ] Write `fastdb.payload.v1.schema.json` with four required root fields, no extra fields at any level, ID regex, exact profiles/cardinalities/component kind, and a recursive `$defs/type` `oneOf` for:

  ```text
  scalar:     kind + optional nullable
  normalized: kind + min + max + optional nullable
  component:  kind + id + optional nullable
  ref:        kind + target + optional nullable
  list:       kind + items + optional nullable
  ```

  JSON Schema cannot express ID uniqueness, `min < max`, component resolution, by-value acyclicity, or profile legality; document those as Core semantic checks in `schemas/README.md` rather than pretending the schema proves them.
- [ ] Set the source schema metadata exactly to `"$schema":"https://json-schema.org/draft/2020-12/schema"` and `"$id":"urn:fastdb:schema:fastdb.payload.v1"`. Explain in `schemas/README.md` that checked-in files may be formatted for review while the Core-returned bytes and `.sha256` pin are over Core JCS output.
- [ ] Implement `generate_embedded_payload_schemas.py` using only the Python standard library. It reads raw schema bytes and emits deterministic `uint8_t` arrays and explicit `uint64_t` lengths. `--check` compares expected output in memory with `EmbeddedSchemas.inc` and exits non-zero on drift without writing.
- [ ] `SchemaRepository` must parse embedded raw bytes through `JsonDocument`, convert them to `JsonValue`, serialize with Core JCS, and hash with Core SHA-256. The generator must not canonicalize or hash.
- [ ] Run:

  ```bash
  python3 tools/generate_embedded_payload_schemas.py --check
  cmake --build build/payload --parallel
  ctest --test-dir build/payload -R '^payload\.json_document$' --output-on-failure
  ```

  Expected green: all malformed/limit/duplicate cases return exact diagnostics and the embedded schema is current.

- [ ] Run `git diff --check`.
- [ ] Commit: `feat(core): add strict payload JSON documents`

## Task 5: Parse and normalize the complete `fastdb.payload.v1` type algebra

**Files:**

- Create: `fastcarto/fastdb/src/payload/spec/Model.hpp`
- Create: `fastcarto/fastdb/src/payload/spec/Parse.hpp`
- Create: `fastcarto/fastdb/src/payload/spec/Parse.cpp`
- Create: `tests/cpp/payload/test_spec_parse.cpp`
- Create: `tests/golden/payload/v1/spec/valid/empty-record.source.json`
- Create: `tests/golden/payload/v1/spec/valid/record-all-types.source.json`
- Create: `tests/golden/payload/v1/spec/valid/object-graph.source.json`
- Create: `tests/golden/payload/v1/spec/valid/nullable-omitted.source.json`
- Create: `tests/golden/payload/v1/spec/valid/nullable-explicit.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/unknown-field.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/missing-field.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/bad-kind.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/bad-id.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/bad-range.source.json`
- Modify: `tests/cpp/CMakeLists.txt`

**Interfaces consumed:** `JsonDocument`, `JsonPointer`, `Error`, compile limits.

**Interfaces produced:** move-only typed source nodes and `parse_and_normalize_source`.

- [ ] Write table-driven failing tests that cover all 12 scalar kinds, all three structural kinds, nested list nullability, both profiles, both cardinalities, empty entries, empty components, and every optional `nullable` position.
- [ ] Require unknown and missing fields to fail at exact pointers. Require `text`, `columnar.v1`, inferred profile, non-Boolean nullable, non-string IDs, extra normalized fields, and normalized `min >= max` to fail without coercion.
- [ ] Require omitted `nullable` to become `false` on every nested type node while explicit `true` remains true. Assert entry and field vector order exactly matches source.
- [ ] Run `ctest --test-dir build/payload -R '^payload\.spec_parse$' --output-on-failure`.

  Expected red: typed model/parser APIs do not exist.

- [ ] Define scoped internal C++ enums only inside `Model.hpp`:

  ```cpp
  enum class Profile : uint8_t { record_v1, object_graph_v1 };
  enum class Cardinality : uint8_t { one, many };
  enum class TypeKind : uint8_t {
      boolean, u8, u16, u32, i32, u8n, u16n, f32, f64,
      str, wstr, bytes, component, ref, list
  };
  ```

  These never cross the C ABI.
- [ ] Model recursive `list.items` with unique ownership. Store component/ref source IDs until resolution; store `min/max` as finite binary64; represent `nullable` as a required Boolean in the typed node even when omitted in source.
- [ ] Implement one exact allowed-key set and required-key set per object shape. Report 1003 for an unknown member and 1005 with `missing_field` details for a missing/ill-typed member.
- [ ] Validate IDs as non-empty ASCII `[A-Za-z_][A-Za-z0-9_]*`. Do not apply Unicode normalization or target-language keyword escaping during compilation.
- [ ] Parse `u8n/u16n` bounds as finite binary64, require both keys and `min < max`, and preserve signed zero/number semantics for later JCS serialization.
- [ ] Increment total-field counts and enforce entry/component/per-component/total-field limits before growing destination vectors beyond configured bounds.
- [ ] Convert the normalized typed model back to `JsonValue` with explicit `nullable` at every node. Preserve entry/field arrays. Sort object members only in JCS, not in the typed parser.
- [ ] Run:

  ```bash
  cmake --build build/payload --parallel
  ctest --test-dir build/payload -R '^payload\.(json_document|spec_parse)$' --output-on-failure
  ```

  Expected green: the complete source algebra parses and normalizes; invalid shape/type/range cases have stable code/path/details.

- [ ] Run `git diff --check`.
- [ ] Commit: `feat(core): parse and normalize payload specs`

## Task 6: Resolve IDs, indexes, cycles, profiles, and semantic facts

**Files:**

- Create: `fastcarto/fastdb/src/payload/spec/Resolve.hpp`
- Create: `fastcarto/fastdb/src/payload/spec/Resolve.cpp`
- Create: `tests/cpp/payload/test_spec_resolve.cpp`
- Create: `tests/golden/payload/v1/spec/valid/component-order-a.source.json`
- Create: `tests/golden/payload/v1/spec/valid/component-order-b.source.json`
- Create: `tests/golden/payload/v1/spec/valid/entry-order-a.source.json`
- Create: `tests/golden/payload/v1/spec/valid/entry-order-b.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/duplicate-entry.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/duplicate-component.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/duplicate-field.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/unresolved-component.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/by-value-cycle.source.json`
- Create: `tests/golden/payload/v1/spec/invalid/record-ref.source.json`
- Modify: `tests/cpp/CMakeLists.txt`

**Interfaces consumed:** normalized typed source model.

**Interfaces produced:** immutable `ResolvedSpec`, stable indexes, resolved target indexes, profile result, and semantic fact bits.

- [ ] Add failing tests proving component source reorder yields the same sorted component indexes, entry reorder changes entry indexes, and field order remains untouched.
- [ ] Add exact duplicate-ID tests: point at the second declaration and include first declaration pointer in canonical details.
- [ ] Add resolution tests for entry component IDs, nested list component IDs, ref targets, shared targets, and self/mutual ref cycles under `object_graph.v1`.
- [ ] Add by-value cycle tests for direct component, component-through-list, and multi-component cycles. Require 1005 at the closing component node with `reason:"by_value_cycle"` and a deterministic array of component IDs.
- [ ] Add `record.v1` ref rejection tests for refs in entries, fields, and nested lists. Treat every declared component and every entry as a profile-validation root, so an unused declared ref cannot hide in a record-profile spec.
- [ ] Run `ctest --test-dir build/payload -R '^payload\.spec_resolve$' --output-on-failure`.

  Expected red: resolver and stable index APIs do not exist.

- [ ] Check entry IDs in source order; check component IDs before sorting; check field IDs within each source component. Then sort components with ASCII byte comparison and assign `uint32_t` component indexes.
- [ ] Resolve `component.id` and `ref.target` to stable component indexes without replacing the source ID text. Unresolved targets return 1007 at `/id` or `/target`.
- [ ] Build a component containment graph from every by-value `component` edge, including edges under lists. Exclude `ref` edges. Use deterministic white/gray/black DFS in sorted component order and field order so cycle diagnostics do not depend on hash iteration.
- [ ] Reject any `ref` in `record.v1`; allow it in `object_graph.v1`. Do not infer or change the declared profile.
- [ ] Compute semantic facts by walking entries and every declared component: nullable, list, ref, variable width (`str,wstr,bytes,list` and transitive component containment), and normalized integer.
- [ ] Produce normalized `JsonValue` after component sorting, keeping entries/fields stable and omitting derived indexes from canonical payload source.
- [ ] Run:

  ```bash
  cmake --build build/payload --parallel
  ctest --test-dir build/payload -R '^payload\.(spec_parse|spec_resolve)$' --output-on-failure
  ```

  Expected green: all deterministic ordering, target resolution, cycle, profile, and fact tests pass.

- [ ] Run `git diff --check`.
- [ ] Commit: `feat(core): resolve payload profiles and indexes`

## Task 7: Assemble immutable compiled identity, schemas, manifest, and golden corpus

**Files:**

- Create: `schemas/fastdb.payload.manifest.v1.schema.json`
- Create: `schemas/fastdb.payload.v1.schema.sha256`
- Modify: `schemas/README.md`
- Modify: `tools/generate_embedded_payload_schemas.py`
- Regenerate: `fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc`
- Create: `fastcarto/fastdb/src/payload/spec/Manifest.hpp`
- Create: `fastcarto/fastdb/src/payload/spec/Manifest.cpp`
- Create: `fastcarto/fastdb/src/payload/spec/CompiledSpec.hpp`
- Create: `fastcarto/fastdb/src/payload/spec/CompiledSpec.cpp`
- Create: `tests/cpp/payload/GoldenCorpus.hpp`
- Create: `tests/cpp/payload/GoldenCorpus.cpp`
- Create: `tests/cpp/payload/test_compiled_spec.cpp`
- Create: `tests/golden/payload/v1/spec/index.json`
- Create: matching `*.canonical.hex`, `*.sha256`, `*.manifest.hex`, and `*.error.json` expectations for every Task 5-6 source fixture
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** strict document, normalized/resolved model, JCS, SHA-256, embedded schemas.

**Interfaces produced:** immutable `CompiledSpec::compile`, canonical bytes/digest, source schema bytes/digest, exact manifest, and truthful P1 capabilities.

- [ ] Add end-to-end failing tests proving:

  - omitted nullable and explicit false have identical canonical hex/digest;
  - component-order A/B have identical canonical hex/digest/indexes;
  - entry-order A/B and field reorder have different canonical bytes/digest;
  - whitespace and source object-key order do not affect identity;
  - manifest `payload.sha256` exactly matches compiled digest lowercase hex;
  - manifest indexes match ABI query order planned for Task 8;
  - source schema bytes are JCS-canonical and its digest is stable;
  - all invalid fixtures match exact status/path/details.

- [ ] Run `ctest --test-dir build/payload -R '^payload\.compiled_spec$' --output-on-failure`.

  Expected red: compiled spec, manifest, full schema embedding, and corpus loader do not exist.

- [ ] Define `CompiledSpec` as an immutable shared internal object owning canonical bytes, digest bytes, `ResolvedSpec`, lookup maps, manifest bytes, profile, and capability facts. Construction is private and only a fully successful static `compile` returns it.
- [ ] Implement the pipeline in the exact order stated above. Do not digest source JSON, schema JSON, or manifest JSON in place of normalized canonical payload bytes.
- [ ] Add the manifest schema matching the exact object in this plan. For resolved type nodes, require derived component indexes only on `component`/`ref`; prohibit extra fields everywhere.
- [ ] Set the manifest schema metadata exactly to `"$schema":"https://json-schema.org/draft/2020-12/schema"` and `"$id":"urn:fastdb:schema:fastdb.payload.manifest.v1"`.
- [ ] Produce `schemas/fastdb.payload.v1.schema.sha256` from the independently verified JCS schema bytes, store exactly 64 lowercase hexadecimal characters plus the text-file newline, and test that its decoded value equals `SchemaRepository`'s raw 32-byte result.
- [ ] Independently verify schema canonical bytes and digest with the official JCS reference at commit `19d51d7fe467d4706a3ff08adf8a748f29fc21e0`:

  ```bash
  reference_dir="$(mktemp -d)"
  canonical_file="$(mktemp)"
  git clone --quiet --no-checkout \
    https://github.com/cyberphone/json-canonicalization.git "$reference_dir"
  git -C "$reference_dir" checkout --quiet 19d51d7fe467d4706a3ff08adf8a748f29fc21e0
  JCS_REFERENCE="$reference_dir/node-es6/canonicalize.js" \
  SCHEMA_FILE="schemas/fastdb.payload.v1.schema.json" \
  node -e 'const fs=require("fs"); const c=require(process.env.JCS_REFERENCE); process.stdout.write(c(JSON.parse(fs.readFileSync(process.env.SCHEMA_FILE,"utf8"))));' \
    >"$canonical_file"
  python3 -c 'import hashlib, pathlib, sys; print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())' \
    "$canonical_file"
  ```

  Expected: the printed digest exactly equals `schemas/fastdb.payload.v1.schema.sha256`; the reference tool is a verification-only input and is never a build/runtime dependency.
- [ ] Set P1 capabilities to operations `compile,query`, no codegen targets, and direct build `not_evaluated/runtime_slice_not_implemented`. Never return `eligible:false`, because layout has not evaluated the spec.
- [ ] Generate expected payload canonical hex by review of normalized JCS output, then compute `.sha256` from the decoded expected bytes using an independent platform SHA-256 command. Do not ask the implementation under test to rewrite its own golden expectations.
- [ ] Make `index.json` explicit and ordered; do not discover fixtures through filesystem iteration. Each case names its source and exactly one expected success tuple or expected error object.
- [ ] Require manifest JCS output to validate structurally against the checked-in manifest schema through the same table-driven field/type assertions used by Core. The machine schema is a published description; it is not a second runtime parser.
- [ ] Update Issue 0002: mark the internal Core compiler/identity/manifest stage complete, but state no public spec compile/query function exists until Task 8 and no runtime payload exists in P1.
- [ ] Run:

  ```bash
  python3 tools/generate_embedded_payload_schemas.py --check
  cmake --build build/payload --parallel
  ctest --test-dir build/payload -R '^payload\.(jcs|json_document|spec_parse|spec_resolve|compiled_spec)$' --output-on-failure
  ```

  Expected green: the full compile corpus passes and schema embedding is clean.

- [ ] Run `git diff --check`.
- [ ] Commit: `feat(core): compile portable payload identities`

## Task 8: Expose the complete P1 query ABI and C++ RAII facade

**Files:**

- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Create: `fastcarto/fastdb/include/fastdb_payload.hpp`
- Modify: `fastcarto/fastdb/src/payload/abi/Handles.hpp`
- Modify: `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp`
- Modify: `tests/cpp/payload/test_c_header_smoke.c`
- Create: `tests/cpp/payload/test_spec_abi.cpp`
- Create: `tests/cpp/payload/test_cpp_facade.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** immutable `CompiledSpec` and Core-owned error/blob/schema values.

**Interfaces produced:** every public function in the Exact First-Slice Contract and `fastdb::payload::v1` C++ wrappers.

- [ ] Expand the C smoke into a complete valid empty-spec compile -> canonical blob/digest/profile/capabilities/count queries -> release flow. Add one invalid duplicate-key compile and verify status equals error code before release.
- [ ] Add failing ABI tests for null pointer/span combinations, the explicit null-`out_error` meta-contract exception, output clearing, unsupported struct size/flags/reserved values, custom low compile limits, ID not found, index out of range, schema queries, exact digest bytes, and all entry/component/field index round trips.
- [ ] Add lifetime tests: independently retain/release spec/blob/error, release spec before returned blob, copy digest then release, and balance every handle under injected failures.
- [ ] Add a 16-thread test where all threads retain one spec, repeatedly query canonical/manifest/digest/counts/indexes, release returned blobs, and release the spec. Require byte-for-byte equality and zero failures under ThreadSanitizer when that optional local toolchain is available; ASan/UBSan remain required CI.
- [ ] Add failing C++ facade tests for copy/move/self-assignment, `CompiledSpec::compile`, `Blob::bytes`, digest array, profile/capabilities/count/index queries, source schema, and a thrown `PayloadError` preserving code/symbol/path/message/details copied from the C error.
- [ ] Run focused tests.

  Expected red: compile/query symbols and C++ facade do not exist.

- [ ] Implement all explicit C signatures. Keep `spec`, `blob`, and `error` handle layouts private. Convert every `uint64_t` size/index input with checked bounds before native allocation or indexing.
- [ ] Validate options/capability `struct_size` against the V1 known prefix, require known flags/reserved values to be zero, and leave room for a larger future tail. Never read beyond the supplied `struct_size`.
- [ ] For each fallible call, clear value outputs and `out_error` first, validate arguments, invoke Core, allocate owned outputs, then publish them only on success. If output allocation fails, keep the spec valid and return the owned allocation error.
- [ ] Implement ID lookup using exact UTF-8 byte equality after validating input IDs; do not accept NUL-terminated assumptions, case folding, or target-language aliases.
- [ ] Return new retained blob handles for canonical JSON, manifest JSON, schema JSON, and ID getters. Blobs own or share immutable storage independently of the caller's spec handle.
- [ ] Implement `fastdb_payload.hpp` as a header-only C++17 wrapper in `fastdb::payload::v1` with `Blob`, `CompiledSpec`, `CompileOptions`, `Capabilities`, and `PayloadError`. Wrapper constructors call the C initializers; wrapper copies call retain; destructors call release; no wrapper parses JSON or computes a digest.
- [ ] Run:

  ```bash
  cmake --build build/payload --parallel
  ctest --test-dir build/payload -R '^payload\.(c_header_smoke|spec_abi|cpp_facade)$' --output-on-failure
  ```

  Expected green: C and C++ consumers observe the same Core-owned facts and all ownership/error paths pass.

- [ ] Run the full native suite and sanitizer suite:

  ```bash
  ctest --test-dir build/payload --output-on-failure
  cmake -S fastcarto -B build/payload-sanitize \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DFASTDB_ENABLE_SANITIZERS=ON \
    -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/payload-sanitize --parallel
  ctest --test-dir build/payload-sanitize --output-on-failure
  ```

  Expected: all CTest cases pass; ASan/UBSan report no findings.

- [ ] Update Issue 0002's current surface to “P1 compiler/query ABI implemented” and list every remaining P2-P5 gap and impact. Keep its status Open.
- [ ] Run `git diff --check`.
- [ ] Commit: `feat(core): expose portable payload spec queries`

## Task 9: Add fuzzing, ABI drift checks, CI platforms, packaging checks, and final docs

**Files:**

- Create: `tests/fuzz/payload/fuzz_spec_compile.cpp`
- Create: `tests/fuzz/payload/corpus/empty-record.json`
- Create: `tests/fuzz/payload/corpus/object-graph.json`
- Create: `tests/fuzz/payload/corpus/duplicate-key.json`
- Create: `tests/fuzz/payload/corpus/arbitrary-bytes.bin`
- Create: `tests/abi/fastdb_payload_v1_symbols.txt`
- Create: `tools/check_payload_abi_symbols.py`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `.github/workflows/tests.yml`
- Modify: `README.md`
- Modify: `fastcarto/README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`
- Modify: `docs/issues/README.md`

**Interfaces produced:** reproducible fuzz target, exact symbol gate, Linux/macOS native CI, sanitizer CI, and evidence-bounded shipped-state documentation.

- [ ] Add a failing `check_payload_abi_symbols.py` test invocation before creating the allowlist. It must locate the platform shared library, normalize leading underscores, filter only `fdb_payload_v1_*`, sort uniquely, and compare exact lines with `tests/abi/fastdb_payload_v1_symbols.txt`.
- [ ] Add a libFuzzer entry that consumes arbitrary bytes plus deterministic option variants derived from the first input byte, calls `spec_compile_json`, queries every result on success, verifies status/error equality on failure, and balances every handle. It must never interpret input as NUL-terminated text.
- [ ] Add `FASTDB_BUILD_FUZZERS` default `OFF`. Require Clang when enabled; compile with `-fsanitize=fuzzer,address,undefined` and link the existing portable Core.
- [ ] Seed the fuzz corpus from the valid and invalid golden source files, then run:

  ```bash
  cmake -S fastcarto -B build/payload-fuzz \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DFASTDB_BUILD_FUZZERS=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  cmake --build build/payload-fuzz --target fuzz_payload_spec_compile --parallel
  cmake -E make_directory build/payload-fuzz/runtime-corpus
  build/payload-fuzz/tests/cpp/fuzz_payload_spec_compile \
    build/payload-fuzz/runtime-corpus tests/fuzz/payload/corpus \
    -runs=10000 -max_len=1048576
  ```

  Expected: 10,000 runs complete without crash, sanitizer report, or leak.

- [ ] Add `native_tests` to `.github/workflows/tests.yml` with matrix `ubuntu-24.04` and `macos-15`. Assert `uname -m` is `x86_64` on Ubuntu and `arm64` on macOS, run vendor/schema checks, configure/build CMake with tests, run CTest, and run the ABI symbol check.
- [ ] Add a Linux `native_sanitizers` job using Clang, ASan, and UBSan. Add a short fuzz smoke (`-runs=1000`) after the deterministic suite.
- [ ] Extend path filtering so changes under `schemas/**`, `tools/vendor_portable_payload_deps.sh`, `tools/generate_embedded_payload_schemas.py`, `fastcarto/lib/{yyjson,double-conversion,picosha2}/**`, `tests/cpp/**`, `tests/fuzz/**`, `tests/golden/payload/**`, and `tests/abi/**` trigger native, Python package, and WASM jobs as appropriate.
- [ ] Add `native_tests` and `native_sanitizers` to the aggregate `test` job. A failed or cancelled required native job must fail the aggregate; a path-skipped job may remain skipped.
- [ ] Update README files and changelog with exact shipped wording: P1 compile/query Core and ABI exist; payload runtime and language projections do not yet exist; FastDB 0.1.x public call-db/`ColumnEngine` remains only until the planned 0.2.0 clean cut. Link Issue 0002 for the current gap and Issue 0001 only for post-0.2 deferrals.
- [ ] Review Issue 0002 against the code and ensure every unavailable P2-P5 operation has current limit, reason, impact, next owner slice, and closure criteria. Do not close it.
- [ ] Run the complete local gate:

  ```bash
  tools/vendor_portable_payload_deps.sh --check
  python3 tools/generate_embedded_payload_schemas.py --check
  cmake -S fastcarto -B build/payload-final -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF
  cmake --build build/payload-final --parallel
  ctest --test-dir build/payload-final --output-on-failure
  python3 tools/check_payload_abi_symbols.py --build-dir build/payload-final

  cmake -S fastcarto -B build/payload-final-sanitize \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DFASTDB_ENABLE_SANITIZERS=ON
  cmake --build build/payload-final-sanitize --parallel
  ctest --test-dir build/payload-final-sanitize --output-on-failure

  uv run pytest tests/python -q
  uv run python -m compileall -q python/fastdb4py tests/python
  uv build
  ```

  Expected: all commands pass; sdist/wheel include the schemas and vendored dependency bytes.

- [ ] With Emscripten active, run:

  ```bash
  bash ts/build-wasm.sh
  npm --prefix ts/fastdb4ts run build
  npm run test:ts
  ```

  Expected: the existing TypeScript/WASM package still builds and passes while linking the enlarged Core; this does not claim the new portable TypeScript projection exists.

- [ ] Inspect package contents:

  ```bash
  wheel="$(find dist -maxdepth 1 -type f -name '*.whl' -print | sort | tail -n 1)"
  sdist="$(find dist -maxdepth 1 -type f -name '*.tar.gz' -print | sort | tail -n 1)"
  test -n "$wheel" && test -n "$sdist"
  python3 -m zipfile -l "$wheel" | rg 'fastdb'
  tar -tf "$sdist" | rg 'schemas/fastdb\.payload|fastcarto/lib/(yyjson|double-conversion|picosha2)'
  ```

  Expected: sdist includes both schemas and all pinned source/license/provenance files; wheel contains the linked native library and Python package without source-tree absolute paths.

- [ ] Scan only the new authority path for forbidden coupling and public C++ ABI leakage:

  ```bash
  if rg -n -i '\b(c-two|crm|relay|route|lease|toodle|call-db|columnar\.v1|org\.fastdb\.columnar)\b' \
    schemas fastcarto/fastdb/src/payload fastcarto/fastdb/include/fastdb_payload.h \
    fastcarto/fastdb/include/fastdb_payload.hpp; then
    echo 'forbidden downstream or legacy authority term in portable Core surface' >&2
    exit 1
  fi
  if rg -n '"text"|TypeKind::text' \
    schemas fastcarto/fastdb/src/payload fastcarto/fastdb/include/fastdb_payload.h \
    fastcarto/fastdb/include/fastdb_payload.hpp; then
    echo 'binding-owned text type leaked into the FastDB native algebra' >&2
    exit 1
  fi
  if rg -n '\b(size_t|wchar_t|std::|namespace|class|template|bool|enum|long)\b' \
    fastcarto/fastdb/include/fastdb_payload.h; then
    echo 'non-portable type or C++ construct in the public C header' >&2
    exit 1
  fi
  ```

  Expected: no coupling/type-algebra matches in schemas/Core/public headers, and no non-portable type or C++ construct appears in the C header. Rejection fixtures may contain forbidden input strings only under `tests/`.

- [ ] Run `git diff --check`, inspect `git status --short`, and verify no build output, generated WASM, wheel, or temporary corpus is staged.
- [ ] Commit: `test(core): harden portable payload contract`

## Final Review Gate

- [ ] Trace every P1 requirement in the accepted design to at least one exact test name and record the mapping in the implementation handoff.
- [ ] Search this plan and all new docs for unresolved placeholders:

  ```bash
  rg -n 'TB[D]|TO[D]O|implement[[:space:]]+later|similar[[:space:]]+to[[:space:]]+Task|appropriate[[:space:]]+error[[:space:]]+handling|write[[:space:]]+tests[[:space:]]+for[[:space:]]+the[[:space:]]+above|fill[[:space:]]+in' \
    docs/superpowers/plans/2026-07-16-portable-payload-core-contract.md \
    docs/issues/0002-portable-payload-foundation-implementation-status.md \
    schemas README.md fastcarto/README.md
  ```

  Expected: no matches.

- [ ] Verify all fenced JSON in changed Markdown parses with duplicate-key rejection.
- [ ] Verify all changed Markdown relative links resolve.
- [ ] Review the final diff for accidental FastDB/C-Two coupling, a second semantic parser/digest, implementation-dependent manifest ordering, public C++ layout, unchecked size conversions, non-atomic handles, or an overstated 0.2.0 claim.
- [ ] Run the complete Task 9 gate again from a clean build directory after the final review fixes.
- [ ] Record exact command outcomes, platform/architecture, test counts, fuzz run count, sanitizer result, package artifact names, and any unavailable local toolchain. Do not convert a CI expectation into a local pass claim.
- [ ] If final review fixes changed tracked files, rerun the full gate and commit `fix(core): address portable payload contract review`. If no tracked fix was needed, require a clean worktree and record that no follow-up commit was created.
- [ ] Do not tag or bump to 0.2.0. P2-P5 and every non-deferred success criterion must close first.

## Completion Definition

This plan is complete only when a C and C++ caller can compile any valid complete `fastdb.payload.v1` document, receive the same Core-owned normalized canonical bytes/digest/manifest/indexes/capabilities, receive stable owned diagnostics for every invalid corpus case, safely retain/release/query immutable handles across threads, and reproduce all identity facts on Linux x86-64 and macOS arm64. The implementation must still say plainly that it cannot build or open a payload binary yet, and Issue 0002 must remain open with P2-P5 closure gates.
