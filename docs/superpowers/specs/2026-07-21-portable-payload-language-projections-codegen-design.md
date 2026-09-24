# FastDB Portable Payload Language Projections and Codegen Design

Date: 2026-07-21

Status: accepted P4 delta under the active implementation Goal

Scope: FastDB P4 only

## 1. Relationship to the accepted foundation

This document is the implementation-level P4 delta for the accepted
[portable payload foundation design](2026-07-16-portable-payload-foundation-design.md)
and [ADR 0001](../../decisions/0001-portable-payload-core-authority.md).
It consumes the locally frozen P1-P3 boundary recorded in
[Issue 0002](../../issues/0002-portable-payload-foundation-implementation-status.md):
the C++ Core, `fastdb.payload.v1`, `fastdb.payload.bin.v1`, runtime/lifetime
semantics, stable structured errors, and the exact 105-symbol C ABI are inputs,
not P4 redesign targets.

P4 adds official Rust, Python, and TypeScript/Wasm projections over that
boundary and adds one C++ Core-owned four-target code generator. It does not
remove legacy public surfaces. The removal and `RecordEngine` rename remain a
separate P5 clean cut after P4 is frozen.

No decision here changes the trust boundary, binary version, source schema,
profile algebra, or ABI versioning rules. No new ADR is required.

## 2. Outcome

At P4 closure:

- C++, Rust, Python, and TypeScript/Wasm compile and query the same spec through
  the same Core;
- all four author, freeze, execute, open, view, materialize, and invalidate the
  same record and object-graph payloads;
- every binding preserves the Core error code, symbol, JSON Pointer path,
  message, and canonical details JSON;
- every binding obtains canonical identity, binary meaning, direct/staged
  facts, graph identity, sharing, cycles, and checked lifetime from the C ABI;
- the Core generates deterministic payload-only C++, Rust, Python, and
  TypeScript artifacts in memory;
- generated artifacts call the official projection instead of parsing specs or
  binary data; and
- manifests advertise codegen only after all four targets really generate and
  compile/import/type-check.

The result is a reusable FastDB substrate. It contains no C-Two CRM, route,
relay, transport, lease, policy, Toodle, GIS, or object-store semantics.

## 3. Explicit non-goals

P4 does not:

- reopen P1-P3 layout, graph, backing, error, or lifetime semantics without a
  separately reproduced owner defect;
- create a second C++ runtime or class ABI;
- make Rust, Python, or TypeScript a parser, canonicalizer, digest, layout, or
  binary authority;
- make one language more semantically capable than the others;
- generate CRM methods, clients, routes, transports, leases, policies, domain
  models, or final project layouts;
- write generated artifacts to a destination directory from the library API;
- remove call-db, `fastdb.schema.v1`, `columnar.v1`, or `ColumnEngine`;
- implement native Node, Go, segmented backing, streaming builders, or new
  guaranteed platforms from deferred D2-D5;
- change package versions, prepare 0.2.0 metadata, push, tag, publish, or make a
  hosted-CI claim; or
- modify C-Two or Toodle.

## 4. Live delta audit

### 4.1 Frozen ABI-105 already supplies the runtime

The live [C header](../../../fastcarto/fastdb/include/fastdb_payload.h) already
provides:

- strict JSON compile options and `spec_compile_json`;
- canonical JSON, SHA-256, manifest, profile, capabilities, source schema,
  and stable entry/component/field queries;
- all V1 builder values, fixed runs, object declaration/fill, roots, refs,
  nulls, lists, and freeze;
- immutable plan facts and execution against either the internal Core heap or
  caller callbacks;
- copied and external open, payload identity/profile/report/binary access,
  retain/release, and invalidation;
- checked record/list/component/object/ref traversal and graph identity;
- scalar bit-exact getters plus scoped `str`, `wstr`, bytes, and payload byte
  access;
- detached materialization; and
- owned structured error and blob handles.

P4 bindings therefore project existing behavior. They do not need new runtime
semantics or convenience-only Core entry points.

### 4.2 Existing C++ facade is reused

The live [C++ facade](../../../fastcarto/fastdb/include/fastdb_payload.hpp)
already has RAII `Blob`, `PayloadError`, `CompiledSpec`, `Builder`,
`BuildPlan`, `Payload`, `View`, `Access`, `ObjectHandle`, option/report types,
and typed scalar/text/binary conveniences. P4 adds only the codegen result and
artifact facade plus a convenience proven necessary by generated C++ output.
It does not duplicate any P1-P3 class.

### 4.3 Current language bindings are migration inputs

- The existing SWIG extension binds the legacy `fastdb.h` database API. There
  is no `fastdb4py.payload` projection.
- The existing Embind module binds the legacy database API. There is no
  `fastdb4ts/src/payload` projection and the portable C ABI is not exported to
  JavaScript.
- The existing Python-to-TypeScript generator reads Python feature classes. It
  is not Core-owned payload codegen.
- Hand-written Python/TypeScript schema and call-db modules remain P5 migration
  inputs. P4 must neither use nor extend them as portable authority.
- No Rust workspace or crate currently exists.

### 4.4 Existing build channels can be reused

The native FastDB shared library already includes the payload Core and is
packaged beside the Python extension. The TypeScript build already compiles the
same Core to Wasm. CMake already isolates all payload Core translation units in
one object target. P4 needs a linkable payload-only native target for Rust and
a mechanical Wasm export surface; it does not need a copied Core source tree.

## 5. Authority and dependency direction

```text
fastdb.payload.v1 JSON
        |
        v
C++ Core: compile -> resolved model -> identity/runtime/codegen
        |
        v
stable fdb_payload_v1_* C ABI
        |
        +----------------+----------------+----------------+
        v                v                v                v
 C++17 RAII       Rust raw + safe   Python payload   TypeScript/Wasm
        ^                ^                ^                ^
        +----------------+ generated per-spec ergonomics --+
```

The arrows never reverse. A projection may translate ownership and language
types, but cannot answer a semantic question without calling the ABI.
Generated code may embed Core-canonical source so that it can ask the Core to
compile the same spec; embedding does not make the generated module a parser.

## 6. Common projection contract

### 6.1 Public concepts

Each projection exposes language-idiomatic equivalents of:

- `CompiledSpec`, `Capabilities`, and stable index queries;
- `Builder`, `ObjectHandle`, `BuildPlan`, `PlanInfo`, `BuildPolicy`, and
  `ExecutionReport`;
- `Payload`, `View`, `ViewKind`, `GraphIdentity`, and `Access`;
- `Blob` or an immutable owned byte value;
- `PayloadError`; and
- after codegen lands, `CodegenTarget`, `ArtifactKind`, `Artifact`, and
  `ArtifactSet`.

Names may follow language conventions. Values and behavior may not diverge.

### 6.2 Handle ownership

| Core handle | Copy/clone rule | Destruction rule |
| --- | --- | --- |
| spec | retainable | release exactly once per owned reference |
| builder | unique, movable | release on close/drop/dispose |
| plan | retainable | release exactly once per owned reference |
| payload | retainable | release exactly once per owned reference |
| view | retainable | release exactly once per owned reference |
| access | unique scoped pin | release before borrowed span can be reused |
| codegen result | retainable immutable ArtifactSet | release exactly once per owned reference |
| blob | retainable immutable bytes | release exactly once per owned reference |
| error | copied into a language error or retained | release after fields are captured |

Move-from, closed, dropped, and disposed handles cannot issue another ABI call.
Bindings must make double close harmless locally without changing Core
retain/release semantics.

### 6.3 Errors

Every non-zero ABI status becomes a language error containing all five Core
fields:

```text
code, symbol, path, message, details_json
```

`details_json` remains the exact Core-owned canonical bytes or decoded text; a
binding may offer a convenience JSON object only in addition to the exact
text. Bindings do not collapse codes, infer categories from messages, or use a
thread-local last error. The C++ facade's existing `PayloadError` is the model.

### 6.4 Numeric and text values

- `u8`, `u16`, `u32`, and `i32` preserve their exact ranges.
- `f32` and `f64` cross the ABI as exact bits and are reconstructed by the
  projection.
- `u8n` and `u16n` expose the Core-decoded binary64 value.
- FastDB names remain `str` and `wstr`; no projection introduces `text`.
- `str` is UTF-8, `wstr` is a sequence of Unicode scalar content carried by
  the ABI's aligned host-endian UTF-16-unit access, and bytes stay opaque.
- Python and TypeScript safe conveniences copy text/bytes before releasing an
  access guard. Rust and C++ may return borrows tied to the guard lifetime.
- An explicitly named unsafe/raw escape may exist only if its lifetime risk is
  documented and excluded from safe claims.

### 6.5 Null, list, component, ref, and identity

Null is always queried through `view_is_null`; no language sentinel is passed
to the Core. Empty and null lists remain distinct. Component fields retain
source order and stable indexes. Refs are explicit view nodes and are followed
only with `view_ref_target`. Graph identity is the exact pair
`(component_index, object_id)`, preserving sharing, self cycles, and mutual
cycles without pointer identity claims.

### 6.6 Materialization and invalidation

A checked view remains tied to its source payload generation. After
invalidation, the same Core code/path/details must be observed in every
language. `materialize` returns a detached Core-owned view whose values remain
available after the source payload is invalidated and released. Bindings do
not emulate detachment by recursively copying values themselves.

### 6.7 Backing

Calling `plan_execute` with no callback backing uses the existing Core heap
adapter and returns a truthful direct/staged report. This is the common default
in every projection.

Host-provided backing is an adapter concern:

- Rust supplies a safe callback owner whose reservation and readable storage
  stay alive through Core retain/release callbacks; an explicitly unsafe raw
  ABI entry remains available only in `fastdb-sys`.
- Python pins the buffer owner and callback objects until the Core releases the
  owner token. Callback exceptions are caught and translated to stable backing
  status, never allowed across C.
- TypeScript/Wasm can expose external backing only for storage owned in Wasm
  linear memory. A JavaScript `ArrayBuffer` copied into Wasm is called a copy,
  not zero-copy external backing. The native Wasm host adapter owns and releases
  the token.

All adapters prove commit, rollback, retain, release, too-small capacity,
direct-required rejection, and allow-staging fallback. They never decide
layout or whether a plan is direct-capable.

## 7. C++ projection delta

The C++ facade adds:

```text
CodegenOptions
CodegenTarget
ArtifactKind
Artifact
ArtifactSet
CompiledSpec::generate(target, options)
```

`Artifact` owns its path, bytes, and digest independently of a temporary query.
`ArtifactSet` owns the codegen-result handle and returns artifacts in Core
order. Existing exception and RAII rules remain unchanged. Generated C++ uses
this existing facade; it does not include private Core headers.

## 8. Rust projection

### 8.1 Workspace

The first real Rust slice creates:

```text
bindings/rust/Cargo.toml
bindings/rust/fastdb-sys/
bindings/rust/fastdb/
tests/rust/payload/
```

The workspace uses the repository's 0.x package metadata without publishing or
changing versions. Both crates must have a real compile/query/runtime caller in
the same commit; no placeholder crate lands.

### 8.2 `fastdb-sys`

`fastdb-sys` is a mechanical, unsafe projection of `fastdb_payload.h`:

- exact constants, fixed-width structs, callbacks, opaque handles, and
  functions;
- checked-in generated declarations so normal users do not require libclang;
- an ABI-layout test against a C probe and the exact symbol allowlist; and
- `links = "fastdb_payload_v1"` so two native owners cannot silently link.

In a repository checkout, its build script may invoke the repository CMake
payload-only target. In a packaged/system mode it links a caller-supplied
compatible library and validates the ABI version. It never compiles a copied or
modified semantic implementation. The system/source modes and their package
inventory are tested separately.

### 8.3 Safe `fastdb`

The safe crate:

- keeps all handle construction and FFI pointer use private;
- implements `Drop`, `Clone` only for retainable handles, and unique ownership
  for builder/access;
- ties borrowed bytes/text to `&Access` and never returns a longer-lived slice;
- represents null with `Option` and errors with an owned `PayloadError`;
- exposes safe internal-heap execution and a safe callback-backed resource;
- keeps materialized views independent of the source owner; and
- contains no JSON or binary parser, JCS/SHA implementation, layout constants,
  graph traversal algorithm, or generator.

Rust-specific marker/newtype ergonomics are allowed, but no Rust-only Core
operation is allowed.

## 9. Python projection

### 9.1 Package boundary

The official projection is `fastdb4py.payload`. It loads the same packaged
native FastDB library and binds only `fdb_payload_v1_*`. A thin `ctypes` FFI is
acceptable because it calls the stable native ABI directly, works on the
supported Python 3.10 floor, and avoids merging portable semantics into the
legacy SWIG class wrapper. It is not a Python implementation of the Core.

Library discovery is deterministic:

1. the native library packaged beside `fastdb4py.core`;
2. an explicit development-only environment override documented for source
   testing; or
3. a typed import failure explaining the missing native library.

It does not search arbitrary current-working-directory files.

### 9.2 Python ownership

Owned handles provide `close()`, context-manager support, idempotent local
closure, and a finalizer fallback. Public methods reject use after close.
Errors copy all Core fields before releasing the native error. Safe accessors
return Python-owned `str`, UTF-16-decoded `str`, or `bytes`; any raw memoryview
escape is explicitly unsafe and retains its guard.

Python annotations/decorators, if added, only construct source JSON. This P4
does not require them: `CompiledSpec.compile` accepts UTF-8 JSON and delegates
the complete operation to Core.

### 9.3 Packaging

The current wheel remains one native Core plus its existing extension/binding
library. `fastdb4py.payload` adds Python source, not a second native Core. The
sdist includes the codegen Core and public headers. Python 3.10 compile/import
and wheel inventory are mandatory gates.

## 10. TypeScript/Wasm projection

### 10.1 Mechanical C ABI export

The existing Wasm link is extended to export the exact reviewed
`fdb_payload_v1_*` allowlist plus `_malloc`/`_free`, with Wasm BigInt enabled
for lossless `uint64_t`/`int64_t` values. The exported list is derived
mechanically from the reviewed allowlist and checked against the Wasm object;
there is no hand-maintained second function inventory.

The TypeScript low-level module allocates ABI input/output slots in Wasm
memory, calls the exported function, copies stable error fields, and clears
temporary memory. It does not parse portable JSON or binary payloads.

### 10.2 Public API and disposal

`fastdb4ts/payload` exposes the common projection contract. Handles use Wasm
pointer identity internally, an explicit `dispose()` method, idempotent local
closure, and `FinalizationRegistry` only as a fallback. A disposed handle
cannot call the Core. `Access` pins its Core span until disposal; safe text and
byte APIs copy into JavaScript-owned values.

The package stays browser/worker/Node-test capable. Native Node remains D5 and
is not implied by running the Wasm module under Node tests.

### 10.3 Legacy TypeScript modules

P4 adds `src/payload/` and a package subpath export. Existing `schema.ts`,
`call-db.ts`, and related exports remain untouched migration inputs until P5,
but generated portable artifacts import only the new payload subpath.

## 11. Core-owned codegen model

### 11.1 Placement and inputs

Generator code lives under:

```text
fastcarto/fastdb/src/payload/codegen/
```

It consumes a const `CompiledSpec`/`ResolvedSpec`, its canonical source,
digest, and target. It does not reparse JSON or read manifest JSON back into a
second model.

### 11.2 Target and result

One generation call accepts exactly one target:

```text
cpp | rust | python | typescript
```

and returns one immutable, target-homogeneous `ArtifactSet`. “Four-target”
means that the one Core generator supports all four targets, not that a call
must mix four trees.

The initial inventory is one source artifact per target:

| Target | Relative path suffix | Kind |
| --- | --- | --- |
| C++ | `.hpp` | `source` |
| Rust | `.rs` | `source` |
| Python | `.py` | `source` |
| TypeScript | `.ts` | `source` |

The complete relative path is:

```text
fastdb_payload_<64-lowercase-payload-digest>.<suffix>
```

A flat digest-derived path has no environment input and lets C-Two place the
unchanged artifact under a destination subtree it owns. More artifacts or
kinds require a real generated-code caller and a reviewed additive change.

### 11.3 Artifact invariants

An artifact contains:

```text
relative_path
kind
bytes
sha256
```

The Core validates before publication that:

- the path is non-empty UTF-8, relative, slash-normalized, and contains no
  empty, `.`, or `..` segment, backslash, drive prefix, or NUL;
- artifact paths are unique and strictly sorted by unsigned UTF-8 bytes;
- kinds are known target-neutral values;
- bytes are immutable and within configured output limits, with the byte limit
  enforced during renderer construction rather than only after a complete
  oversized draft exists; and
- SHA-256 is computed over exactly those bytes.

Failure publishes no partial result.

### 11.4 Provenance

Every generated file starts with a target-appropriate comment containing:

```text
generated-by: fastdb.payload.codegen.v1
payload-sha256: <64 lowercase hex>
core-abi-version: 1
generator-version: fastdb.payload.codegen.v1
target: <cpp|rust|python|typescript>
```

The generator version is a Core constant. There are no timestamps, host paths,
user names, random values, package destinations, or environment-derived names.

### 11.5 Identifier projection

Source IDs remain the authority and are emitted verbatim in metadata. Public
generated symbols use a total, collision-free target-specific encoding of the
ASCII source bytes rather than a heuristic keyword suffix:

```text
C++:        fdb_cpp_id_<lowercase ASCII hex>
Rust:       fdb_rust_id_<lowercase ASCII hex>
Python:     fdb_python_id_<lowercase ASCII hex>
TypeScript: fdb_ts_id_<lowercase ASCII hex>
```

Type names add a fixed generated prefix/suffix around the same encoded body.
Because every byte is represented, the mapping is reversible, independent of
keyword lists, and collision-free even for IDs deliberately shaped like an
escape. Metadata maps each emitted symbol to the original ID and stable index.

### 11.6 Generated surface

Each target emits only per-spec ergonomics over its official runtime:

- canonical source and digest provenance constants; TypeScript exposes
  immutable canonical text and returns a fresh byte encoding rather than a
  shared mutable `Uint8Array`;
- a function that compiles the embedded canonical source through Core;
- stable entry/component/field index constants and original-ID metadata;
- entry sequence wrappers;
- component view wrappers with schema-specific field accessors and checked
  construction against the Core-owned specification digest and component
  index;
- ref-target helpers only for actual `ref` values, and graph-identity helpers
  only for values whose Core-derived runtime topology has identity;
- builder helpers that select the correct entry indexes and declare only
  identity-bearing components;
- typed scalar conveniences where the official runtime already provides them;
- generic checked list/component view escape for recursively composed values;
  and
- materialization returning the corresponding generated wrapper around the
  Core-detached view.

Generated code never contains schema validation, JCS/SHA, profile decisions,
layout offsets, binary headers, ref reachability, direct/staged decisions, or
recursive materialization. Cycles remain Core graph views; generated code does
not recursively instantiate an infinite object model.

Schema provenance is not inferred from an index. Component and entry indexes
are scoped to one `CompiledSpec`; two unrelated specifications may both have a
component or entry at index zero. The private Task 7 generator can check only
the component index because ABI-105 does not expose a Core-owned provenance
requirement on Builder or View handles. Its artifacts therefore remain private
and unadvertised. Before Task 8 publishes them, Core adds the three exact
`require_spec_sha256` functions below and every projection exposes them without
binding-owned comparison/error semantics. Generated entry factories, builder
helpers, and component constructors call the applicable Core requirement
before using an index. A same-index handle from another specification must
fail with Core `DIGEST_MISMATCH`; it must never become a generated wrapper or
mutate a builder.

Entry wrappers likewise cannot be constructed from an arbitrary sequence
View. C++ keeps the View constructor private and exposes a payload factory;
Rust keeps its View constructor module-private; Python and TypeScript require
their generated module's internal creation token. Task 8 adds the Core payload
provenance guard to each public factory before it selects the generated entry
index.

## 12. Additive stable C ABI

### 12.1 New constants and types

The public header adds, without changing existing values:

```c
#define FDB_PAYLOAD_OPERATION_CODEGEN (UINT64_C(1) << 7)

#define FDB_PAYLOAD_CODEGEN_TARGET_CPP        (UINT64_C(1) << 0)
#define FDB_PAYLOAD_CODEGEN_TARGET_RUST       (UINT64_C(1) << 1)
#define FDB_PAYLOAD_CODEGEN_TARGET_PYTHON     (UINT64_C(1) << 2)
#define FDB_PAYLOAD_CODEGEN_TARGET_TYPESCRIPT (UINT64_C(1) << 3)

#define FDB_PAYLOAD_ARTIFACT_SOURCE UINT32_C(1)

typedef uint64_t fdb_payload_v1_codegen_target_t;
typedef uint32_t fdb_payload_v1_artifact_kind_t;
typedef struct fdb_payload_v1_codegen_result
    fdb_payload_v1_codegen_result_t;
```

A target argument must contain exactly one known target bit. Capability target
flags use the same bits and may contain all supported targets.
The Core-internal target enum uses these exact bit values; the ABI adapter
validates one known bit and does not maintain a second ordinal mapping.

### 12.2 Options prefix

```c
typedef struct fdb_payload_v1_codegen_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t max_artifacts;
    uint64_t max_total_bytes;
    uint64_t reserved[3];
} fdb_payload_v1_codegen_options_t;
```

The V1 size macro is frozen from the actual C layout with cross-platform C
assertions. `flags` and every reserved field must be zero. Initial defaults are
`max_artifacts = 16` and `max_total_bytes = 16 MiB`. Zero for either limit is a
literal zero limit, not an implicit default. Limits affect success/failure but
never artifact bytes.

### 12.3 Exact additive function family

Generated schema-specific ergonomics first require three Core-owned provenance
guards over the specification digest associated with each handle:

```c
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_builder_require_spec_sha256(
    const fdb_payload_v1_builder_t* builder,
    const uint8_t expected_sha256[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_payload_require_spec_sha256(
    const fdb_payload_v1_payload_t* payload,
    const uint8_t expected_sha256[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error);
FDB_PAYLOAD_API fdb_payload_v1_status_t
fdb_payload_v1_view_require_spec_sha256(
    const fdb_payload_v1_view_t* view,
    const uint8_t expected_sha256[FDB_PAYLOAD_V1_SHA256_SIZE],
    fdb_payload_v1_error_t** out_error);
```

The fixed-size argument is borrowed only for the call. A mismatch reuses the
Core binary-open digest contract exactly: code `3006`, symbol
`DIGEST_MISMATCH`, message `Portable payload spec digest does not match`, and
canonical details
`{"actual":"<handle lowercase hex>","expected":"<argument lowercase hex>","reason":"spec_digest_mismatch"}`.
The path is `/builder/spec_sha256`, `/payload/spec_sha256`, or
`/view/spec_sha256`. Null arguments retain the ordinary invalid-argument
contract. These guards do not parse, canonicalize, hash, or change P3 runtime
meaning; they project identity already owned by the handle. Detached views
must retain that Core-owned identity through materialization so their guard
has the same result as the source view.

The immutable ArtifactSet/codegen family is:

```text
fdb_payload_v1_codegen_options_init
fdb_payload_v1_spec_codegen
fdb_payload_v1_codegen_result_retain
fdb_payload_v1_codegen_result_release
fdb_payload_v1_codegen_result_artifact_count
fdb_payload_v1_codegen_result_artifact_relative_path
fdb_payload_v1_codegen_result_artifact_kind
fdb_payload_v1_codegen_result_artifact_bytes
fdb_payload_v1_codegen_result_artifact_sha256
```

Together these are an exact twelve-symbol additive P4 design over ABI-105. If
implementation proves this family sufficient, the reviewed post-P4 boundary
will be 117. That number is not claimed as frozen until the native and Wasm
export scanners see the implemented symbols and every generated target passes.

`spec_codegen` accepts a non-null compiled spec, one target, a non-null
initialized options prefix, and result/error outputs. Query functions accept a
non-null result and a zero-based artifact index. Path and bytes queries return
independently owned blob handles. The result is immutable and retainable.

### 12.4 Output-clearing and exception rules

Before any validation or allocation:

- every result/blob output is set to null;
- counts and kinds are set to zero;
- digest outputs are zeroed; and
- `out_error`, when non-null, is set to null by the common guard.

Invalid options prefixes use the existing 7002 contract. Unknown/multi-target
input uses 6001. Invalid internally generated paths use 6002. Generator output
limit, template, or invariant failure uses 6003 with a stable canonical
`reason`. No C++ exception crosses C and no partially populated ArtifactSet is
published.

## 13. Manifest truthfulness

Only the final public codegen task changes manifests. At that point:

- operation flags add `FDB_PAYLOAD_OPERATION_CODEGEN`;
- manifest operations append `"codegen"` after `"invalidate"`;
- target flags are the OR of all four target bits; and
- `codegen_targets` is exactly
  `["cpp","rust","python","typescript"]` in that order.

The manifest schema, embedded schema bytes, digest expectations, conformance
checker, C ABI capabilities, and golden manifests change together. No target
is advertised while its generated output cannot pass its real compiler or
import/type-check gate.

## 14. Shared proof matrix

### 14.1 Runtime parity

Every projection consumes the same checked-in fixtures and proves:

- canonical JSON and SHA-256;
- manifest/profile/capabilities and stable indexes;
- deterministic record and graph binary bytes;
- every scalar, normalized value, `str`, `wstr`, bytes, nested component,
  recursive list, null versus empty, and cardinality;
- object declaration/fill, disconnected roots, sharing, forward/self/mutual
  refs, and graph identity;
- exact error code/path/details for representative compile, builder, open,
  type, backing, and invalidation failures;
- internal-heap direct execution, forced fallback, and direct-required failure;
- copied and host-external ownership where the host can represent it safely;
- checked invalidation and detached materialization; and
- release/drop/dispose order, clone/retain, and failure cleanup.

The expected bytes and errors always come from `tests/golden/payload/v1/`, not
from a binding-generated expectation.

### 14.2 Codegen parity

For record-all-types, keyword/escape collision, recursive lists, and cyclic
object-graph specs, tests prove:

- repeated generation returns identical artifact count/order/path/kind/bytes
  and digest;
- every reported SHA-256 independently matches bytes;
- provenance is exact and environment-free;
- original IDs and generated names are reversible and collision-free;
- invalid target/options/index outputs are cleared and errors exact;
- C++ and Rust artifacts compile and execute a runtime smoke;
- Python artifacts import and execute a runtime smoke on supported syntax;
- TypeScript artifacts type-check/build and execute against Wasm; and
- generated trees contain no forbidden downstream semantics.

### 14.3 ABI and package proof

- Native and Wasm exact-symbol scanners use the reviewed allowlist.
- Pure C and C++ facade tests cover the additive family and V1/V2 prefix
  behavior where applicable.
- Rust raw layouts are compared with a C probe.
- Python wheel/sdist inventory includes the new Python source and Core codegen
  sources without a second native Core.
- TypeScript package exports the payload subpath and its Wasm exports match the
  same allowlist.
- Rust source/system link modes and package inventory are exercised locally.

## 15. Implementation slices

The executable sequence is defined in the companion
[implementation plan](../plans/2026-07-21-portable-payload-language-projections-codegen.md).
It proceeds vertically:

1. common source/link seam plus four-language compile/query;
2. authoring and immutable plans;
3. execution, copied/external backing, and reports;
4. complete record views/access/materialization/invalidation;
5. complete graph identity/sharing/cycles;
6. shared parity and package/lifetime hardening;
7. private Core ArtifactSet/four-target generator;
8. additive codegen ABI, C++ facade, generated-output executability, and
   truthful manifests; and
9. complete robustness/workflow/docs/review closure.

No slice gives Rust a privileged private-Core path. A language joins a
capability slice only through the same public C ABI.

## 16. P4 closure and P5 handoff

P4 closes only when:

- all four projections pass the shared runtime matrix;
- all four codegen targets pass deterministic generation and real generated
  output compilation/import/type-check;
- the actual native/Wasm ABI is frozen against the reviewed additive list;
- manifests truthfully advertise the exact executable targets;
- native, sanitizer, focused TSan, fuzz/corpus, Wasm, Rust, Python 3.10,
  TypeScript, package, schema, documentation, and clean-worktree gates pass to
  the extent locally available;
- every material primary-agent spec/code-quality finding is fixed and gates
  rerun; and
- Issue 0002 records exact local evidence, toolchain limits, and the fact that
  review is primary-agent rather than independent.

P5 then receives a complete replacement surface and can remove legacy
authority without aliases or compatibility parsers. Version change, hosted
CI, push, tag, publication, and downstream C-Two composition are still not P4
facts.
