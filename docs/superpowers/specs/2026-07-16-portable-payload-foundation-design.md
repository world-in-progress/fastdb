# FastDB Portable Payload Foundation Design

- **Status:** Accepted
- **Accepted:** 2026-07-16
- **Target release:** FastDB 0.2.0
- **Normative decision:** [ADR-0001](../../decisions/0001-portable-payload-core-authority.md)
- **Tracked deferrals:** [Issue 0001](../../issues/0001-portable-payload-deferred-capabilities.md)

## 1. Objective

FastDB 0.2.0 will provide a domain-neutral, cross-language payload substrate for scientific-computing and RPC systems. The substrate must let C++, Rust, Python, and TypeScript/WASM compile one payload specification, build or open the same binary payload, expose checked views, materialize owned values, and generate matching language projections without reimplementing schema or layout semantics in each binding.

This is foundation work, not a C-Two feature embedded in FastDB. C-Two consumes the foundation from its own `c-two.contract.v2` super-schema and remains responsible for CRM methods, bindings, identity, routes, transports, leases, and lifecycle. Toodle consumes C-Two and FastDB through those owner boundaries. Raw-file bytes remain in object/file storage such as RustFS/OpenDAL/lakeFS; FastDB carries structured RPC values and metadata, not the raw-file object store itself.

The target is a clean 0.x correction. FastDB 0.2.0 does not preserve the current public call-db abstraction, `fastdb.schema.v1`, `columnar.v1`, `ColumnEngine`, or C-Two-shaped compatibility aliases.

## 2. Decision summary

1. The C++ Core is the only semantic authority for `fastdb.payload.v1`.
2. The Core performs strict parsing, normalization, RFC 8785 JSON Canonicalization Scheme serialization, SHA-256 identity, profile validation, layout planning, binary build/open, checked view behavior, and portable code generation.
3. All native and WASM consumers use a versioned, opaque-handle C ABI named with the `fdb_payload_v1_*` prefix.
4. C++ users consume a RAII wrapper over that C ABI; the public C++ class ABI is not the cross-language contract.
5. Rust, Python, and TypeScript/WASM are projections over the same Core semantics. No binding owns an independent parser, digest algorithm, profile validator, layout planner, or binary reader.
6. `record.v1` names FastDB's AoS record engine with strided field access. It is not described as columnar storage.
7. `object_graph.v1` owns identity-bearing component pools, references, lists, and cycles.
8. Payload build follows `CompiledSpec -> PayloadBuilder -> BuildPlan -> PayloadOwner -> CheckedViews/MaterializedValues`.
9. Final backing is an explicit callback contract with direct/staged truth reporting, commit/rollback, and retain/release.
10. FastDB code generation returns an in-memory artifact set. A downstream orchestrator such as `c3` composes and writes final project files.
11. Every intentionally deferred capability is recorded in a FastDB-owned documentation issue with its present limit, rationale, and closure criteria.

## 3. Ownership and boundaries

### 3.1 FastDB owns

- the `fastdb.payload.v1` source and canonical schema;
- the native type algebra and the `record.v1` and `object_graph.v1` profiles;
- strict JSON parsing, normalization, RFC 8785 canonicalization, and SHA-256 digest computation;
- the derived manifest and capability report;
- `fastdb.payload.bin.v1`, including validation, layout, fixed-width wire types, and reference encoding;
- C++ Core builders, plans, payload owners, views, materialization, invalidation, and resource limits;
- the stable C ABI and C++ RAII facade;
- final-backing callbacks and the direct/staged execution report;
- Rust, Python, and TypeScript/WASM portable payload bindings;
- payload-only C++, Rust, Python, and TypeScript code generation;
- stable error codes and structured error details.

### 3.2 C-Two owns

- `c-two.contract.v2`, which contains a nested `fastdb.payload.v1` document rather than an independent FastDB sidecar;
- parsing and assembling the whole C-Two contract JSON;
- CRM method parameters/results and their mapping to FastDB entry IDs;
- provider and client bindings, contract identity, routes, transports, leases, and lifecycle;
- the single user-facing `c3` code-generation entry point;
- composition of FastDB's returned artifacts with C-Two RPC artifacts;
- parity of C-Two Rust and Python SDK behavior.

C-Two passes the exact UTF-8 source slice, or an equivalently duplicate-preserving serialized subtree, to the FastDB library. It does not reconstruct, reorder, normalize, or digest the FastDB value itself. A C-Two parser must reject duplicate keys in the outer document rather than collapse them before delegation.

### 3.3 Toodle and storage systems own

Toodle owns resource identity, revisions, tags, policy, Resource Services, runtime activation, discovery, and federation. Object/file systems own raw-file bytes, multipart transfer, and object-store lifecycle. Neither responsibility moves into FastDB merely because a FastDB payload describes a request or response related to a file.

### 3.4 Always / ask / never

**Always**

- route payload semantics through the C++ Core;
- use explicit schema/profile/binary/ABI versions;
- reject unknown fields and invalid states rather than infer intent;
- use fixed-width wire and ABI types;
- report a current limitation in `docs/issues/` before merging a deliberately limited behavior;
- add cross-language golden tests for every semantic change.

**Ask for a new ADR before changing**

- the native type algebra;
- profile legality rules;
- canonicalization or digest identity;
- the stable C ABI or backing callback contract;
- view lifetime and invalidation semantics;
- the FastDB/C-Two ownership boundary;
- the clean-cut removal policy.

**Never**

- add CRM methods, route identity, relay behavior, lease policy, or C-Two contract assembly to FastDB;
- use Python or TypeScript as a second schema authority;
- compute canonical JSON or digests independently in a binding;
- expose platform `wchar_t`, `size_t`, compiler-dependent enums, STL types, exceptions, or Rust layout through the C ABI;
- infer a profile from data or type content;
- revive call-db under a compatibility alias;
- claim direct final-backing construction when a complete payload was staged and copied;
- route raw-file object bytes through FastDB as a replacement for object storage.

## 4. Technical baseline

| Concern | Baseline |
|---|---|
| Core implementation | C++17 in `fastcarto/fastdb` |
| Cross-language boundary | C-compatible header with versioned opaque handles and fixed-width integer types |
| Canonical JSON | RFC 8785 JSON Canonicalization Scheme, implemented only by the Core |
| Digest | SHA-256 over normalized canonical UTF-8 bytes |
| Native binary | `fastdb.payload.bin.v1`, little-endian, fixed-width, self-validating |
| C++ projection | RAII facade over the C ABI |
| Rust projection | raw `fastdb-sys` crate plus safe `fastdb` crate |
| Python projection | native binding over the C ABI; Python annotations may be an authoring frontend only |
| TypeScript projection | TypeScript API backed by the same C++ Core compiled to WebAssembly |
| Code generation | Core-owned generator returning an in-memory `ArtifactSet` |
| Tests | C++ unit/golden/fuzz tests plus Rust, Python, TypeScript/WASM parity tests |

An implementation dependency for JSON parsing or SHA-256 may be selected in the implementation plan, but it is acceptable only if duplicate keys can be detected before object construction and the emitted bytes match the RFC 8785 and SHA-256 golden corpus exactly. A library choice cannot become a second semantic contract.

## 5. `fastdb.payload.v1` source model

### 5.1 Root document

The source document has exactly four root fields:

```json
{
  "schema": "fastdb.payload.v1",
  "profile": "record.v1",
  "entries": [],
  "components": []
}
```

Rules:

- `schema` is exactly `fastdb.payload.v1`.
- `profile` is exactly `record.v1` or `object_graph.v1`.
- `entries` is an ordered array. Its order is semantic and determines stable entry indexes.
- `components` is an unordered source array. The Core sorts it by component ID in canonical output.
- All four root fields are required.
- Unknown fields at every object level are errors.
- JSON object keys must be unique in the source text. A parser that silently keeps one duplicate value is non-conforming.
- JSON numbers must be finite and valid under RFC 8785/I-JSON number serialization.
- An empty `entries` array is valid and represents an empty logical payload.

IDs are non-empty ASCII identifiers matching `[A-Za-z_][A-Za-z0-9_]*`. Entry IDs are unique within `entries`, component IDs are unique within `components`, and field IDs are unique within their component. Language generators deterministically escape target-language keywords without changing the schema ID.

### 5.2 Complete example

The authoring form may omit `nullable`; the compiled canonical form inserts it at every type node.

```json
{
  "schema": "fastdb.payload.v1",
  "profile": "record.v1",
  "entries": [
    {
      "id": "points",
      "cardinality": "many",
      "type": {"kind": "component", "id": "Point"}
    },
    {
      "id": "tolerance",
      "cardinality": "one",
      "type": {"kind": "f64"}
    }
  ],
  "components": [
    {
      "id": "Point",
      "kind": "record",
      "fields": [
        {"id": "x", "type": {"kind": "f64"}},
        {"id": "y", "type": {"kind": "f64"}},
        {"id": "label", "type": {"kind": "str", "nullable": true}}
      ]
    }
  ]
}
```

### 5.3 Entries

An entry has exactly these fields:

```json
{
  "id": "points",
  "cardinality": "many",
  "type": {"kind": "component", "id": "Point", "nullable": false}
}
```

- `cardinality` is `one` or `many`.
- `one` carries exactly one logical value of `type`.
- `many` carries an ordered top-level sequence of values of `type`; the sequence may be empty.
- Every entry must be supplied to the builder. There is no implicit optional entry.
- For `one`, `type.nullable` controls whether the one value may be explicit null.
- For `many`, the sequence itself is present and `type.nullable` controls whether individual elements may be null.
- A nullable collection distinct from an empty collection is represented as `cardinality: one` plus a nullable `list` type.
- C-Two parameter/result position is not stored in FastDB. C-Two maps its own binding positions to stable FastDB entry IDs.

### 5.4 Components and fields

A component is a reusable record definition:

```json
{
  "id": "Point",
  "kind": "record",
  "fields": [
    {"id": "x", "type": {"kind": "f64", "nullable": false}},
    {"id": "y", "type": {"kind": "f64", "nullable": false}}
  ]
}
```

- `kind` is exactly `record` in V1.
- Field order is semantic and determines stable field indexes and record layout order.
- Every declared field must be present when a record value is authored.
- Missing a field is different from supplying explicit null.
- Explicit null is accepted only when that field's type has `nullable: true`.
- Components may be reused by entries, by-value component types, lists, and references.
- The by-value component containment graph must be acyclic. Cycles are legal only through `ref` nodes under `object_graph.v1`.

### 5.5 Type algebra

Every type node in canonical form contains an explicit Boolean `nullable`. The authoring form may omit it; the Core inserts `false` before canonicalization and digest computation.

#### Scalars

The scalar kinds are:

```text
bool
u8
u16
u32
i32
u8n
u16n
f32
f64
str
wstr
bytes
```

Except for normalized integers, a scalar node has exactly `kind` and `nullable`:

```json
{"kind": "f64", "nullable": false}
```

Native semantics:

| Kind | Logical and physical rule |
|---|---|
| `bool` | Logical Boolean; physical byte is exactly `0` or `1`; other values are invalid. |
| `u8` | Unsigned 8-bit integer. |
| `u16` | Unsigned 16-bit integer, little-endian in binary payloads. |
| `u32` | Unsigned 32-bit integer, little-endian in binary payloads. |
| `i32` | Signed two's-complement 32-bit integer, little-endian in binary payloads. |
| `f32` | IEEE 754 binary32, including infinities and NaN. Encoding preserves signed zero and infinities and canonicalizes every NaN to quiet-NaN bits `0x7fc00000`. |
| `f64` | IEEE 754 binary64, including infinities and NaN. Encoding preserves signed zero and infinities and canonicalizes every NaN to quiet-NaN bits `0x7ff8000000000000`. |
| `str` | Unicode scalar sequence encoded as strict UTF-8. Invalid UTF-8 is rejected. |
| `wstr` | Unicode scalar sequence encoded as well-formed UTF-16LE. Unpaired surrogates are rejected. The ABI uses `uint16_t` spans, never `wchar_t`. |
| `bytes` | Uninterpreted octets, distinct from `str` and `wstr`. |

Non-finite `f32`/`f64` payload values enter through typed builder operations; they are never JSON numbers in the payload specification. The strict JSON compiler still rejects non-finite source numbers as required by RFC 8785/I-JSON.

`u8n` and `u16n` have explicit finite bounds:

```json
{"kind": "u8n", "min": -1.0, "max": 1.0, "nullable": false}
```

- `min` and `max` are required finite numbers and `min < max`.
- The logical value is floating point and must lie in the closed interval `[min, max]`; out-of-range input is an error, not an implicit clamp.
- Physical code `q` decodes as `min + q * (max - min) / Q`, where `Q` is `255` for `u8n` and `65535` for `u16n`.
- Encoding computes `q = round_even((value - min) * Q / (max - min))` after the finite/range check.
- The Core performs the conversion so bindings cannot diverge in quantization behavior.

#### Component values

```json
{"kind": "component", "id": "Point", "nullable": false}
```

- `id` must resolve to a declared component.
- By-value component expansion must be finite; recursive by-value containment is rejected.
- Under `record.v1`, component values are inline records in AoS order.
- Under `object_graph.v1`, identity-bearing component instances live in a per-component object pool and entry values identify graph roots. By-value nested records remain values; only `ref` introduces shared identity.

#### References

```json
{"kind": "ref", "target": "Node", "nullable": true}
```

- `target` must resolve to a declared component.
- `ref` is legal only under `object_graph.v1`.
- A non-null reference resolves to a valid object ID in the target component pool.
- Null is distinct from every valid object ID.
- Cycles and shared references are legal.

#### Lists

```json
{
  "kind": "list",
  "nullable": true,
  "items": {"kind": "str", "nullable": false}
}
```

- `items` is another complete type node and may recursively be a scalar, component, reference, or list.
- A null list is distinct from an empty list.
- Item nullability is independent of list nullability.
- List order is semantic.

### 5.6 Profile rules

Profiles map one-to-one to engine families:

```text
record.v1       <-> RecordEngine
object_graph.v1 <-> ObjectEngine
```

They do not create FastDB resource classes, and there is no two-resource-by-two-engine cross-product. A payload selects one profile; the matching engine semantics follow from that profile.

#### `record.v1`

- Stores component rows in array-of-structures order.
- Field-oriented access is strided access over AoS records; neither the profile nor its engine is called columnar.
- Allows scalars, by-value acyclic components, and recursively nested lists.
- Rejects `ref` anywhere in the reachable type graph.
- Variable-width values use Core-owned pools referenced by validated offsets/lengths; their presence does not turn the record engine into SoA storage.

#### `object_graph.v1`

- Supports all V1 type nodes.
- Stores identity-bearing instances in per-component pools.
- Uses validated object IDs for references; raw process pointers never appear in the binary format.
- Allows shared references and cycles.
- Defines entry component values as graph roots into those pools.
- Supports ordinary build, open, decode, checked view, materialize, and invalidation in 0.2.0. Only direct final-backing construction for dynamic graphs is deferred.

The source document always declares its profile. The compiler never infers a profile from types or input values.

## 6. Strict compilation, canonicalization, and identity

The only identity pipeline is:

```text
UTF-8 JSON source
  -> duplicate-preserving parse
  -> exact-schema validation
  -> reference/profile/number validation
  -> insert nullable=false at every omitted type node
  -> preserve entries and fields order
  -> sort components by ID
  -> RFC 8785 JCS UTF-8 bytes
  -> SHA-256
```

Consequences:

- An authoring document that omits `nullable` and one that explicitly writes `nullable: false` compile to identical canonical bytes and digest.
- Source component order does not affect identity.
- Entry order and field order do affect identity.
- JSON whitespace and source object-key order do not affect identity.
- Unknown fields, duplicate keys, unresolved components, invalid IDs, illegal profile/type combinations, non-finite numbers, and invalid normalized ranges fail compilation.
- The Core returns the canonical UTF-8 bytes and the raw 32-byte SHA-256 digest.
- JSON fields and artifact metadata named `sha256` render the digest as exactly 64 lowercase hexadecimal digits, matching the nested digest field in `c-two.contract.v2`.
- Bindings may display or transport returned canonical bytes and digest bytes; they may not recreate them.

## 7. Compiled specification, manifest, and capabilities

Successful compilation creates an immutable, retainable `CompiledSpec`. It owns:

- canonical JSON bytes and digest;
- stable entry/component/field indexes;
- resolved type graph;
- profile legality result;
- normalized scalar metadata;
- a derived manifest;
- build/open/codegen capabilities.

The derived manifest is Core-owned canonical JSON with schema `fastdb.payload.manifest.v1`. It contains only payload facts: profile, digest, stable indexes, resolved type shapes, nullability, required pools, reference presence, fixed/dynamic layout facts, supported codegen targets, and direct-build eligibility. It contains no CRM methods, table/route names, policy, tags, transport, or domain metadata. The payload digest is computed from the canonical payload specification, not from the manifest.

The Core also exposes the JCS-canonical machine-readable source schema used to validate `fastdb.payload.v1` and its raw SHA-256 digest. Bindings surface those returned bytes. C-Two pins or bundles that exact schema/digest pair and never maintains a hand-authored copy with independent semantics.

## 8. Runtime state model

```text
CompiledSpec
    -> PayloadBuilder
    -> freeze / plan
    -> BuildPlan
    -> execute against FinalBacking
    -> PayloadOwner
    -> CheckedViews or MaterializedValues
```

### 8.1 `PayloadBuilder`

- Is created from one `CompiledSpec`.
- Accepts values by stable entry index, with ID lookup provided as an ergonomic query rather than a second identity.
- Provides typed operations for scalars, strings, wide strings, bytes, record batches, lists, graph objects, and references.
- Accepts exact-width buffers and Core-defined descriptors, not Python objects, Rust enums, C++ templates, methods, table names, or transport envelopes.
- Distinguishes a missing entry/field from explicit null.
- Rejects type mismatch, range overflow, invalid text, invalid null, unresolved graph object, and duplicate finalization.
- Is mutable and not thread-safe. One builder belongs to one authoring flow.

### 8.2 `BuildPlan`

Freezing seals the builder and produces an immutable plan that owns normalized logical values, layout, sizes, validity maps, variable pools, reference fixups, resource requirements, and direct/staged viability. A plan can execute repeatedly against compatible backings. No user mutation is observed after freeze.

### 8.3 `PayloadOwner`

- Owns or retains one committed backing and one compiled spec.
- Represents an immutable payload generation.
- Is created by executing a plan or opening an existing binary payload.
- Can create checked views and materialized binding-owned values.
- Can be explicitly invalidated before a transport or shared-memory lease is returned.

### 8.4 Views and materialization

- A checked view retains its payload owner and captures the owner's generation.
- Invalidation increments the owner generation; later checked access fails with `FDB_PAYLOAD_E_VIEW_INVALIDATED`.
- A materialized value owns its data in the binding and remains valid after payload invalidation or backing release.
- Normal views are read-only because committed portable payloads are immutable.
- An explicitly unsafe raw pointer/span escape hatch may exist for trusted native use, but it cannot be revoked and is never the default RPC integration path.
- C-Two invalidates FastDB views before releasing a reusable transport backing. C-Two does not implement a competing guard wrapper.

### 8.5 Threading

- Handle retain/release and immutable `CompiledSpec`, `BuildPlan`, `PayloadOwner`, and checked-view queries are thread-safe.
- A `PayloadBuilder` is confined to one thread unless the caller serializes access externally.
- One immutable plan may execute concurrently only with distinct backing contexts. A caller serializes executions that share a backing context.
- Invalidation is a barrier: it prevents new checked accesses, waits for already active checked accesses to leave the backing, advances the generation, and then returns. After it returns, the transport may safely release or reuse the backing.
- Materialized values follow their binding's ordinary owned-value threading rules and have no dependency on the invalidated backing.
- Unsafe raw aliases are outside these guarantees.

## 9. Final-backing contract

V1 writes one contiguous payload into one final backing. The backing interface is a versioned callback table with `struct_size` and these semantics:

| Operation | Contract |
|---|---|
| `reserve` | Return a writable region with at least the requested capacity and an owner token. |
| `write` | Make a validated byte range writable when the backing is not exposed as one stable writable span. |
| `commit` | Publish exactly the used byte length and make the backing readable. Called once after successful execution. |
| `rollback` | Abandon an uncommitted reservation after any failure. Called at most once. |
| `retain` | Retain the backing owner for an opened payload/view lifetime. |
| `release` | Release a prior retain or the final owner reference. |

Rules:

- Callback tables use fixed-width fields and include `struct_size` for compatible extension.
- `reserve`, `write`, `commit`, and `rollback` callbacks for one execution are synchronous and serialized. Backing `retain` and `release` must be thread-safe.
- Core heap backing is the default implementation.
- External implementations may wrap shared memory, mmap, arenas, or WASM linear memory.
- `ALLOW_STAGING` lets execution fall back to a staged build when direct construction is unavailable.
- `REQUIRE_DIRECT` fails with `FDB_PAYLOAD_E_DIRECT_UNAVAILABLE` rather than silently staging.
- The execution result reports `DIRECT` or `STAGED` and a stable fallback reason when staged.
- `DIRECT` means no complete payload image existed outside the final backing before commit. Small metadata scratch is allowed.
- `STAGED` means a complete payload image was created elsewhere and copied into the final backing.
- Commit failure is an error and is followed by rollback when the backing contract permits it.
- A committed backing is immutable through the portable payload API.

Segmented/multipart final backing is deliberately outside V1 and tracked in Issue 0001.

## 10. `fastdb.payload.bin.v1`

The portable binary container has these mandatory invariants:

- little-endian encoding;
- exact-width integer and IEEE floating-point fields;
- an unambiguous magic and binary format version;
- total length and bounded region directory;
- embedded 32-byte compiled-spec digest;
- explicit profile identifier;
- validated offsets, lengths, alignment, validity maps, string/list pools, component pools, and reference tables;
- no native pointers, `size_t`, `long`, compiler enum layout, `wchar_t`, STL object representation, or language-runtime handles;
- deterministic bytes for the same compiled spec and logical values;
- one contiguous backing in V1.

`record.v1` stores fixed record portions in AoS order and exposes field traversal by stride. Variable-width data is stored in typed pools addressed by validated offsets and lengths. `object_graph.v1` stores per-component object pools and stable object IDs, with explicit root and reference data.

Only the Core reads or writes binary headers. Bindings do not parse the container. The implementation change that introduces the encoder/decoder must atomically add `schemas/fastdb.payload.bin.v1.md` with exact byte offsets, alignment, overflow rules, and forward-compatibility behavior, plus checked-in golden payloads. The encoder is not complete and cannot merge without that normative byte-layout document and fixtures.

Opening a payload validates, before exposing any view:

- magic, binary version, profile, and total length;
- region bounds, non-overlap requirements, integer overflow, and alignment;
- supplied compiled-spec digest against the embedded digest;
- validity bitmap sizes and legal Boolean bytes;
- UTF-8 and UTF-16LE validity when accessed or eagerly when requested by open options;
- list offsets and lengths;
- object IDs, reference targets, and root indexes;
- caller-supplied resource limits.

Open options provide limits for total bytes, regions, entries, components, nesting depth, list elements, graph objects, string bytes, and validation work. Safe defaults are supplied by the Core. Limits affect whether an input is accepted, never its canonical identity.

## 11. Stable C ABI

### 11.1 ABI rules

- Public symbols use the `fdb_payload_v1_*` prefix.
- The header is valid C and has a pure-C compile smoke test.
- All objects are opaque handles.
- Every retainable handle has explicit `retain` and `release` functions.
- Status is returned as a fixed-width code. No C++ exception crosses the boundary.
- Status `0` means success; every non-zero status has a stable error code and owned error handle.
- Failure returns an owned `fdb_payload_v1_error_t`; there is no thread-local “last error”.
- Byte/string spans are pointer plus `uint64_t` length. Borrow duration is stated per function.
- Callback structs begin with `uint32_t struct_size` and reserved zero fields.
- ABI additions use new functions or tail fields guarded by `struct_size`; existing meaning never changes within V1.

### 11.2 Required opaque handles

```text
fdb_payload_v1_spec_t
fdb_payload_v1_builder_t
fdb_payload_v1_plan_t
fdb_payload_v1_payload_t
fdb_payload_v1_view_t
fdb_payload_v1_codegen_result_t
fdb_payload_v1_blob_t
fdb_payload_v1_error_t
```

### 11.3 Required function families

| Family | Required behavior |
|---|---|
| `spec_*` | Compile UTF-8 JSON; retain/release; return canonical bytes, digest, schema, manifest, indexes, and capabilities. |
| `builder_*` | Create from spec; set typed entry/field/list/object/ref/null values; freeze; reject invalid state. |
| `plan_*` | Retain/release; query size/resource/direct viability; execute against backing callbacks. |
| `payload_*` | Open existing backing; retain/release; query digest/profile/execution report; invalidate; create views/materialize. |
| `view_*` | Traverse entries, records, fields, lists, objects, and refs with generation checks. |
| `codegen_*` | Generate a target artifact set from one compiled spec and return deterministic artifacts in memory. |
| `blob_*` | Expose immutable owned bytes and release them. |
| `error_*` | Query code/path/message/details and release the owned error. |

The public C++ API in `fastdb_payload.hpp` is a header-level RAII and typed convenience layer over these functions. It may use C++ types internally but cannot define different schema, lifetime, or error semantics.

## 12. Error contract

Every failure has:

- a stable numeric code;
- a stable symbolic name;
- an RFC 6901 JSON Pointer `path` when the failure maps to source or logical data;
- a human-readable UTF-8 `message` for diagnostics, not program flow;
- JCS-canonical `details_json` for machine-readable facts;
- an owned lifetime independent of the failed call's stack.

Initial code allocation:

| Range | Domain | Required initial codes |
|---|---|---|
| 1000-1999 | Spec compile | `1001 INVALID_JSON`, `1002 DUPLICATE_KEY`, `1003 UNKNOWN_FIELD`, `1004 UNSUPPORTED_SCHEMA`, `1005 INVALID_TYPE`, `1006 DUPLICATE_ID`, `1007 UNRESOLVED_COMPONENT`, `1008 PROFILE_VIOLATION`, `1009 INVALID_NUMBER` |
| 2000-2999 | Builder/plan | `2001 MISSING_ENTRY`, `2002 MISSING_FIELD`, `2003 UNEXPECTED_NULL`, `2004 TYPE_MISMATCH`, `2005 OUT_OF_RANGE`, `2006 BUILDER_STATE`, `2007 DIRECT_UNAVAILABLE`, `2008 PLAN_STATE` |
| 3000-3999 | Binary/open | `3001 INVALID_MAGIC`, `3002 UNSUPPORTED_BINARY_VERSION`, `3003 LENGTH_OVERFLOW`, `3004 OUT_OF_BOUNDS`, `3005 MISALIGNED`, `3006 DIGEST_MISMATCH`, `3007 INVALID_REFERENCE`, `3008 RESOURCE_LIMIT` |
| 4000-4999 | View/lifetime | `4001 VIEW_INVALIDATED`, `4002 STALE_GENERATION`, `4003 READ_ONLY` |
| 5000-5999 | Backing | `5001 BACKING_CONTRACT`, `5002 ALLOCATION_FAILED`, `5003 COMMIT_FAILED`, `5004 ROLLBACK_FAILED` |
| 6000-6999 | Codegen | `6001 UNSUPPORTED_TARGET`, `6002 INVALID_ARTIFACT_PATH`, `6003 GENERATOR_FAILED` |
| 9000-9999 | Internal | `9001 INTERNAL` |

Codes are never reused for a different meaning. Bindings expose the source code, path, and details without translating them into a smaller binding-specific enum. C-Two may add its own context by prefixing the JSON Pointer path at its outer contract boundary, but it preserves the FastDB code and details as the cause.

Public C constants use `FDB_PAYLOAD_E_` followed by the symbolic token in the table, such as `FDB_PAYLOAD_E_DUPLICATE_KEY` and `FDB_PAYLOAD_E_VIEW_INVALIDATED`.

An `ALLOW_STAGING` fallback is a successful execution report, not an error. A `REQUIRE_DIRECT` failure uses `2007 DIRECT_UNAVAILABLE` with a stable reason in `details_json`.

## 13. Language projections

### 13.1 C++

- `fastdb_payload.h` is the ABI authority.
- `fastdb_payload.hpp` owns RAII handles, typed spans, result wrappers, and builder/view conveniences.
- C++ exceptions may be used inside the RAII facade only if they are derived from the stable error object and never cross C callbacks.
- The existing C++ storage classes are implementation inputs, not the portable ABI.

### 13.2 Rust

- `fastdb-sys` exposes the raw C ABI only.
- `fastdb` owns safe lifetime wrappers and keeps `unsafe` inside the FFI boundary.
- Rust does not parse payload specs, compute JCS/digests, plan layouts, or read binary headers.
- Generated Rust code contains payload types and ergonomic builder/view/materialize calls only.
- Any FastDB behavior needed equally by C-Two users belongs in FastDB Core/bindings, not in a privileged C-Two Rust SDK path.

### 13.3 Python

- Python annotations or decorators may produce an authoring JSON document.
- The final JSON document is compiled by the Core; Python does not canonicalize or validate it independently.
- Python views preserve the same checked generation and materialization semantics as Rust and C++.
- Python has no hidden sentinel for null and no Python-only native type names.
- `str` and `wstr` remain the FastDB type names; `text` is not a FastDB type.

### 13.4 TypeScript/WASM

- The first official TypeScript portable runtime uses the same C++ Core compiled to WebAssembly.
- The current hand-written call-db parser/validator is removed.
- TypeScript handles use WASM-owned opaque IDs and explicit disposal/finalization helpers.
- Generated TypeScript code provides payload types and ergonomic builder/view/materialize calls without duplicating layout or digest logic.

All four projections differ only in language ergonomics. They expose the same types, nullability, profile errors, canonical digest, logical values, invalidation, and backing execution result.

## 14. Code generation contract

Code generation consumes one `CompiledSpec` and a target (`cpp`, `rust`, `python`, or `typescript`). It returns an in-memory artifact set:

```text
ArtifactSet
  artifacts[]
    relative_path
    kind
    bytes
    sha256
```

Rules:

- `relative_path` is normalized, relative, contains no `..`, and is deterministic for the same input/options.
- `kind` is a stable target-neutral artifact category.
- `bytes` are immutable generated contents.
- `sha256` is computed by the Core over `bytes` and rendered as lowercase hexadecimal in text surfaces.
- Every generated file includes provenance: payload digest, Core ABI version, generator version, and target.
- FastDB generates payload types, builder APIs, checked views, and materialization helpers only.
- FastDB never generates CRM methods, route clients, transport code, leases, policy, or Toodle resources.
- FastDB does not write the final destination directory as part of the library contract.
- The generic `fdb` CLI may inspect, validate, and emit FastDB-only artifacts for diagnostics.
- C-Two's user workflow remains the single `c3` entry. `c3` parses the complete `c-two.contract.v2`, delegates its nested FastDB value to the Core, receives an artifact set, composes C-Two artifacts, and writes the final tree.

The C-Two contract schema is therefore a super-schema wrapping FastDB's schema. It is not a pair of unrelated schema files joined by convention.

## 15. Clean-cut migration to 0.2.0

The implementation branch may contain old and new paths temporarily while tests move. The 0.2.0 release surface contains only the target path.

Required removals and renames:

- remove public `fastdb.call-db.schema.v1`, `org.fastdb.call-db`, call-db binding objects, Python `call_db.py`, TypeScript `call-db.ts`, call-db codegen, and associated public tests/docs;
- remove `fastdb.schema.v1` as the portable payload authority;
- remove `columnar.v1` and `org.fastdb.columnar` profile names;
- rename public `ColumnEngine` to `RecordEngine` without an alias;
- keep `ObjectEngine` as the standalone object-graph engine name while routing portable graph semantics through the new Core;
- refactor useful `require`, allocator, final-backing, owner, view, and materialization mechanics into generic payload facilities rather than retaining call-db wrappers;
- allow the existing feature schema only as an independent authoring convenience; it cannot remain a digest sidecar or competing portable schema;
- allow `FastSerializer` to remain as a clearly labeled legacy standalone serializer; it is not a portable RPC foundation;
- remove C-Two-specific names and assumptions from FastDB source, tests, and public documentation;
- add no compatibility parser, deprecated alias, dual digest, or silent profile conversion.

The target release is 0.2.0 because these are intentional 0.x breaking corrections. Package version changes occur with the implementation/release change, not with this design-only commit.

## 16. Target project structure

```text
schemas/
  fastdb.payload.v1.schema.json
  fastdb.payload.bin.v1.md

fastcarto/fastdb/
  include/
    fastdb_payload.h
    fastdb_payload.hpp
  src/payload/
    json/
    spec/
    layout/
    build/
    backing/
    view/
    codegen/
    error/

bindings/rust/
  Cargo.toml
  fastdb-sys/
  fastdb/

python/fastdb4py/payload/
ts/fastdb4ts/src/payload/

tests/
  cpp/payload/
  rust/payload/
  python/payload/
  ts/payload/
  golden/payload/v1/
  fuzz/payload/

docs/
  decisions/
  issues/
  superpowers/specs/
```

Directories and crates are created only when their first tested end-to-end slice lands. The dependency direction is Core -> C ABI -> language projection. No projection source is linked back into the Core.

## 17. Code and interface conventions

### C++ Core and C ABI

- Use exact-width integer types for persisted and ABI values.
- Check every size addition, multiplication, conversion, offset, and alignment before use.
- Keep parser, normalization, resolved model, layout, and binary validation as separate modules with one-way dependencies.
- Represent immutable compiled specs, plans, payloads, and views with RAII internally.
- Catch every exception at the C ABI boundary and convert it to an owned stable error.
- Do not expose compiler layout or platform text types.
- Treat all incoming JSON and binary payloads as untrusted.

### Bindings

- Bindings call the ABI and translate ownership ergonomically; they do not reproduce semantics.
- Binding enums are generated or mechanically mapped from stable ABI constants.
- A binding test that passes only because it accepts more input than Core is a failure.
- Rust safe wrappers contain all FFI `unsafe`; Python and TypeScript keep native/WASM handle disposal explicit and testable.

### Generated code

- Generated paths and bytes are deterministic.
- Identifier escaping is target-specific but reversible to the original schema ID through generated metadata.
- Generated code contains no environment paths, timestamps, random IDs, routes, or transport assumptions.
- Regeneration over an unchanged spec produces a clean working tree.

## 18. Testing strategy

### 18.1 Schema and identity matrix

- Every native type under every legal profile.
- Every nullable position: scalar, component, ref, list container, list item, entry `one`, and entry `many` element.
- Omitted `nullable` versus explicit `false` produces identical canonical bytes and digest.
- Source component reordering does not change digest; entry/field reordering does.
- RFC 8785 and SHA-256 official vectors.
- Duplicate keys, unknown fields, illegal IDs, non-finite source numbers, bad normalized ranges, unresolved targets, by-value cycles, and profile violations fail with exact code/path/details.

### 18.2 Build/open/value matrix

For every type/profile combination, test compile, build, open, decode, checked view, materialize, invalidate, and release. Test null versus empty, missing versus null, shared refs, cycles, invalid refs, invalid UTF-8/UTF-16LE, quantization endpoints/ties, numeric boundaries, signed zero, infinities, canonical NaNs, and deterministic bytes.

### 18.3 Cross-language golden parity

C++, Rust, Python, and TypeScript/WASM must produce or observe:

- identical canonical JSON and digest;
- identical deterministic binary bytes for the same logical fixture;
- identical logical values and null distinctions;
- identical stable error code/path/details;
- identical direct/staged result and fallback reason;
- identical invalidation behavior.

Golden fixtures live in `tests/golden/payload/v1/` and are read by every projection. A binding-specific reimplementation cannot be used to generate its own expected values.

### 18.4 Backing and lifetime tests

- heap and external backing;
- direct and staged execution;
- `ALLOW_STAGING` and `REQUIRE_DIRECT`;
- reserve/write/commit/rollback ordering, including injected failures;
- retain/release balance;
- payload/view retention and generation invalidation;
- concurrent checked reads and invalidation-barrier behavior;
- materialized values surviving invalidation;
- read-only enforcement;
- unsafe raw view explicitly outside revocation guarantees.

### 18.5 Binary hardening

- Pure-C header compile and ABI symbol test.
- Golden header/layout tests.
- Fuzz strict JSON compilation and binary opening.
- Corpus cases for duplicate keys, deep nesting, integer overflow, truncated regions, overlapping ranges, misalignment, invalid lengths, invalid text, and bad refs.
- ASan and UBSan native jobs.
- Resource-limit tests that terminate predictably without excessive allocation or recursion.

### 18.6 Codegen tests

- Generate all four targets from the same spec.
- Compile C++ and Rust outputs, import Python output, and type-check/build TypeScript output.
- Verify artifact paths, hashes, provenance, keyword escaping, cyclic refs, and deterministic regeneration.
- Verify generated APIs contain no CRM/transport/lease surface.

### 18.7 Required CI platforms

- Linux x86-64 native;
- macOS arm64 native;
- `wasm32` through the TypeScript/WASM build.

Additional platform guarantees remain tracked in Issue 0001 until their golden, ABI, and sanitizer-equivalent gates exist.

### 18.8 Clean-cut gates

Before 0.2.0 release:

- scan exported symbols, package exports, source, tests, and current public docs for forbidden call-db/columnar authority names;
- allow those terms only in this migration record, ADR history, issue history, and changelog explanation;
- ensure no Rust/Python/TypeScript parser or digest implementation exists outside Core;
- ensure the C++ header exports only fixed-width C-compatible ABI types;
- regenerate all artifacts and require a clean diff.

## 19. Verification commands

The implementation must integrate with these repository-level checks:

```bash
cmake -S fastcarto -B build/native -DBUILD_TESTING=ON
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure

uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
uv build

bash ts/build-wasm.sh
npm --prefix ts/fastdb4ts run build
npm run test:ts

cargo test --manifest-path bindings/rust/Cargo.toml --workspace --all-features

git diff --check
```

The implementation plan must add focused commands for golden parity, fuzz targets, sanitizer builds, generated-artifact compilation, ABI symbol inspection, and forbidden-term scans. A command becomes a release gate in the same change that creates the corresponding target; the design does not claim those not-yet-created targets already pass.

## 20. Required implementation order

1. Land the strict source schema, Core compiler, normalization, canonical bytes, digest, resolved model, manifest, error object, and C ABI query surface.
2. Freeze the exact `fastdb.payload.bin.v1` byte layout with golden fixtures and implement record build/open for the complete non-ref type algebra.
3. Add backing callbacks, build plan, direct/staged truth reporting, payload ownership, checked views, materialization, and invalidation.
4. Add the complete ordinary `object_graph.v1` build/open/view/materialize path, including cycles and refs; dynamic graph `REQUIRE_DIRECT` remains the recorded deferral.
5. Add C++ RAII, Rust raw/safe, Python, and TypeScript/WASM projections against the same ABI with parity tests.
6. Add Core-owned four-target artifact generation and C-Two composition integration.
7. Remove the old call-db/columnar authority path, rename `ColumnEngine` to `RecordEngine`, update package surfaces, and pass clean-cut gates.
8. Release as 0.2.0 only after every non-deferred success criterion passes.

This order builds the foundation before downstream convenience APIs while keeping each landed slice end-to-end and testable.

## 21. Success criteria

The foundation is complete only when:

1. One C++ Core compiles every valid `fastdb.payload.v1` and rejects every invalid fixture consistently.
2. Canonical bytes and SHA-256 identity are identical across all bindings because bindings obtain them from Core.
3. `record.v1` and ordinary `object_graph.v1` support the full declared V1 type/nullability semantics.
4. `fastdb.payload.bin.v1` has an exact normative byte-layout document, deterministic golden fixtures, and hardened open validation.
5. The stable C ABI builds as C, uses opaque handles/fixed-width types, and passes lifetime/error/backing tests.
6. C++, Rust, Python, and TypeScript/WASM compile/build/open/view/materialize the shared golden corpus with equivalent behavior.
7. Direct versus staged construction is truthful and `REQUIRE_DIRECT` never falls back silently.
8. C++/Rust/Python/TypeScript codegen outputs compile or import, are deterministic, and contain only FastDB payload concerns.
9. C-Two can wrap and delegate the nested spec without duplicating FastDB semantics, and its Rust/Python user surfaces remain capability-equivalent.
10. Public call-db, `fastdb.schema.v1`, `columnar.v1`, and `ColumnEngine` authority surfaces are absent from the 0.2.0 release.
11. Every remaining limitation is present in `docs/issues/` with rationale and closure criteria.

FastDB satisfying these criteria establishes a portable payload foundation. It does not by itself claim that Toodle raw-file sharing, policy enforcement, resource discovery, or federation is complete.

## 22. Open questions

None. Capabilities intentionally outside 0.2.0 are decisions recorded in Issue 0001 rather than unresolved design questions.
