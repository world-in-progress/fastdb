# FastDB Batch/Array Call-DB Fast Path Design

**Date:** 2026-05-26
**Status:** Partially implemented. Phases 2 and 3 are implemented; Phase 4 has
an initial Python runtime allocation surface; Phases 9 and 10 have an initial
transport-neutral writer plan and require-envelope fixed-columnar layer-import path. Direct
layer section writers, object-graph slot semantics, snapshot caching, and
arena/stub allocation remain follow-up work.
**Scope:** FastDB-owned semantics and optimization plan for `Batch`, `Array`, `Table`, and generic call-db encode/view paths. This document is intentionally independent of C-Two route identity, relay behavior, CRM bridges, and transport lease policy.

## Context

FastDB currently exposes three related but not fully separated concepts. `Batch[T]` and `Array[T]` are CRM/RPC ABI markers that describe payload shapes, while `Table[T]` is the Python runtime object for a named physical table/layer inside a `ColumnEngine` or loaded FastDB buffer. Generic call-db currently bridges those concepts by mapping each logical non-scalar call value to a FastDB table: `Batch[Feature]` becomes a feature table, `Array[Scalar]` becomes a synthetic one-column feature table with a `value` field, and scalar arguments or returns are packed into a synthetic scalar table.

This works functionally, but it makes performance depend on the runtime object shape instead of the logical payload type. If a resource returns a `Table[T]`, call-db can use a bulk path, but it still builds a new call-db database, creates a target table such as `return_0`, copies source columns into that table, combines the DB, and returns a new bytes payload. If a resource returns `list[T]` or another iterable of rows, call-db falls back to row-oriented coercion and `push_many(...)`. The result is a semantic mismatch: user-facing signatures say `Batch[T]` or `Array[T]`, but the fast path is available only when the implementation happens to return the right physical `Table[T]`.

Recent C-Two/FastDB benchmark investigation showed that this mismatch is the dominant cost for large cached payloads. For a 3M-row numeric batch, old raw structured-array transfer could produce wire bytes with `arr.tobytes()` in roughly 1-3 ms, while current FastDB call-db bulk encoding of an equivalent 80 MiB numeric table takes roughly 70-90 ms because it repacks the table into a new call-db database. Client-side view creation and aggregation are not the bottleneck; the bottleneck is server-side call-db repacking.

A follow-up C-Two direct-SHM discussion identified a second, related cost. For multi-slot payloads such as `(fdb.I32, fdb.Batch[Feature], fdb.Array[fdb.F32])`, FastDB currently constructs one final call-db backing buffer in FastDB/Python-owned memory, then the downstream IPC transport copies that buffer into shared memory. The desired optimization is not to make every `Batch.allocate(...)` use transport shared memory. The desired optimization is to let FastDB plan the final multi-table call-db payload size, let a downstream allocator reserve one contiguous destination buffer, and let FastDB build the final call-db backing directly in that destination.

## Decision

Treat `Batch[T]` and `Array[T]` as first-class payload concepts, and treat `Table[T]` as one possible storage-backed implementation of `Batch[T]`, not as the payload concept itself. FastDB should provide runtime objects that can be allocated, filled, viewed, materialized, positionally encoded, and exported without forcing downstream RPC/resource code to know physical table names such as `return_0`.

The intended user-level model is:

| Concept | Meaning | Public role |
| --- | --- | --- |
| `Feature` | One logical row or object record | User-facing schema and single-value payload |
| `Batch[Feature]` | A logical batch of feature rows | User-facing CRM/RPC payload type |
| `Array[Scalar]` | A logical homogeneous scalar vector | User-facing CRM/RPC payload type |
| `Table[Feature]` | A named physical FastDB table/layer in an engine or loaded buffer | Storage/runtime implementation detail that may back a `Batch` |
| `BatchView` / `ArrayView` | Buffer-backed logical payload view with owner/lifetime metadata | Runtime value returned by call-db view paths |
| Call-envelope binding | Positional mapping from CRM/input/output value order to call-db wire tables | Binding descriptor concern, not a user-authored name |

FastDB should not make users return raw `bytes` just to get performance. A resource should be able to return a logical `Batch[T]` or `Array[T]` value, and FastDB should choose the fastest correct encode/export path from the value's backing metadata.

The model must keep feature/schema names and physical storage layer names out of ordinary RPC payload authoring. The call-db binding still owns wire table names internally, but public FastDB authoring should be positional: the first aggregate value produced by `fdb.require(...)` maps to the first aggregate call-db slot, the second value maps to the second slot, and so on. There is no public slot-name layer in the first version.

## Current Behavior To Preserve

- `Table[T]` must remain useful for storage-engine workflows: named layers, direct read/write, fixed-scale fill, mapped rows, column access, materialization, and owner-bound invalidation.
- `Batch[T]` and `Array[T]` must remain valid type-level ABI markers for external systems that derive call-db bindings from signatures.
- `view_call_db(binding, payload, owner=...)` must keep returning owner-bound FastDB-managed views that can be invalidated through `fdb.invalidate(...)`.
- `fdb.materialize(...)` and `.to_owned()` must remain the explicit detach mechanism for data that outlives a backing buffer.
- `unsafe_numpy_view()` remains an explicit trusted escape hatch and must not be used as the default reusable lease path.
- `ColumnEngine` and `ObjectEngine` remain storage engines. They should be usable directly by advanced users, but RPC payload authors should not have to choose them unless they deliberately need engine-level control.

## Target Runtime Capabilities

### Storage-Level Named Layouts

Named physical layouts remain useful for storage-engine workflows and exact-match export tests, but they are not the recommended RPC payload authoring model. Ordinary payload authors should not spell `return_0`, parameter names, or transport slot names. The optimized call-envelope path is `fdb.require(...)`, whose values are positionally matched to the binding descriptor during encode/write.

### Logical Batch And Array Views

Introduce public or semi-public runtime values for logical payloads, such as `BatchView[T]` and `ArrayView[T]`, or make existing call-db views expose equivalent capabilities under stable names. These should carry enough metadata to decide whether the value is compatible with a binding: feature type, scalar kind, row count, owner, generation, writeability, profile, and schema hash.

`Array[Scalar]` should not be permanently modeled as "just a one-column feature table" at the payload layer. The current one-column table can remain an internal encoding fallback, but the logical runtime should expose scalar-vector operations and buffer export directly.

### Logical Allocation API

Add allocation APIs that construct logical payloads without requiring users to create engines, layouts, tables, or call slot names directly. The primary public entry point should be `Batch.allocate(feature_type, capacity, *, profile="auto")`, with an analogous `Array.allocate(item_type, capacity, *, profile="auto")` for scalar arrays.

For fixed columnar-eligible features, `Batch.allocate(...)` should internally choose a `ColumnEngine`-backed fixed table and expose bulk APIs such as `batch.fill(...)`, `batch.column.<field>`, `len(batch)`, `batch[index]`, `.to_owned()`, and `fdb.materialize(batch)`. For object-graph-only features, the same `Batch` public type should internally choose an `ObjectEngine`-backed profile and expose object-graph-oriented write APIs instead of pretending that direct fixed-column mutation exists.

The expected generic FastDB allocation code should be:

```python
batch = fdb.Batch.allocate(Coord, n)
batch.fill(row_id=idx, x=x, y=y, z=z, name=names)
return batch
```

The expected fixed-size RPC call-envelope code should be:

```python
batch = fdb.require(fdb.batch(Coord, rows=n))
batch.fill(row_id=idx, x=x, y=y, z=z, name=names)
return batch
```

not manual `ColumnEngine` / `Layout` / physical table-name construction.

`Batch.allocate(..., profile="auto")` should select columnar when the feature is fixed-column eligible, select object-graph when the feature requires reference/object semantics and is object-graph eligible, and raise a clear `TypeError` with capability diagnostics when no supported backing exists.

### Positional Call-Envelope Binding

Do not add a public API that binds values to slot names in the high-performance authoring path. The public mapping is positional and descriptor-driven. `fdb.require(...)` creates an unbound call-envelope value tuple; `prepare_call_db(...)` validates that those values match the binding slots by order and type.

This binding must be non-destructive: writing a `fdb.require(...)` value into a call-db payload must not rename its underlying layer, mutate cached storage metadata, or invalidate unrelated owners. Any export cache must include the binding schema, profile, value order, owner identity, and generation so a stale encoded snapshot cannot be returned for a different binding.

Physical table/layer names may remain available through an advanced storage API for diagnostics and engine-level workflows, but they should not be ordinary mutable payload attributes. If a caller needs to deliberately wrap a `Table[T]` as payload, it should do so through an explicit adapter such as `Batch.from_table(table)` rather than by treating `Table` as the payload type.

### Exact-Match Buffer Export

Add a generic API such as `try_export_call_db(binding, value)` or `export_call_db_buffer(binding, value, *, allow_repack=False)`. When the value is already backed by a call-db-compatible buffer with the exact binding-owned wire shape, it should return a buffer-protocol object or owner-bound export without rebuilding a DB. When exact export is impossible, the API should return `None` or raise a structured error so callers can fall back to existing `encode_call_db(...)`.

The first exact-match target remains the common single-output `Batch[T]` case: one fixed columnar backing table, matching feature schema hash, no object-graph dependencies, and a combined or otherwise exportable contiguous backing buffer. General same-buffer export remains exact-match only. Mismatched unbound storage values use the normal encoder; the optimized wire-layer rewrite path is reserved for `fdb.require(...)` envelope values.

### Object-Graph Batch Support

Call-envelope allocation must eventually support object-graph profiles, not only fixed columnar tables. For `Batch[T]` where `T` uses `REF` or `list[REF]`, the binding position should identify the root logical batch while `ObjectEngine` continues to own the root layer, dependency layers, object identity, and reference fixups.

Object-graph call-db must eventually support multiple logical slots with the same feature type, for example two `Batch[Node]` parameters in one method. This requires root-position metadata distinct from feature type and layer name; otherwise the current feature-type/layer-name coupling makes same-type multi-slot calls ambiguous. Until that support exists, validation should fail clearly rather than silently mixing roots.

The initial object-graph implementation may remain copy/materialize oriented and may not support retained zero-copy views. That limitation is acceptable as long as the public `Batch`/`Array` contract is correct and the profile capability advertises whether owner-bound views are available.

### Encoded Snapshot Cache

For values that are frequently returned unchanged, add an encoded snapshot cache keyed by at least `(binding.schema_sha256, value owner identity, generation, value order/profile)`. The cache should live on FastDB-owned objects or an explicit FastDB cache object, not in downstream RPC frameworks. Any write to a backing table must bump the relevant generation and invalidate the cache.

This cache is a fallback after exact buffer export. It is still useful for tables whose physical layer name or layout requires one repack but then remains stable across calls.

### Size Planning And Caller-Provided Buffers

Add a lower-level planning and writer-sink API once logical payloads and positional call-envelope binding are stable. A downstream transport should be able to ask FastDB to plan the encoded size, allocate a writable destination, and ask FastDB to write directly into that destination:

```python
plan = fdb.prepare_call_db(binding, value)
destination = allocator.allocate(plan.byte_length)
plan.write_into(destination.view)
payload = destination.commit()
```

A simpler `encode_call_db_into(binding, value, writable_buffer)` helper may wrap the same machinery. The destination is a generic writable buffer-protocol object, not a C-Two object. This keeps FastDB independent of shared-memory pool implementations while allowing downstream transports to build directly into SHM, mmap, bytearray, WebAssembly memory, or other caller-owned memory.

The planning phase should avoid a wasteful double traversal. For string/list-heavy columnar values and object-graph values, `prepare_call_db(...)` should compute byte length while retaining the normalized row mapping, object identity mapping, string sizes, offsets, dependency order, and positional value decisions needed by `write_into(...)`. `write_into(...)` should then emit bytes from that plan without recomputing graph traversal from scratch.

For fixed numeric columnar batches, size planning should be cheap and deterministic from schema and row count. For strings, lists, and object graphs, size planning must account for variable-length payloads before the caller allocates the destination.

### Planned Direct DB Build

`prepare_call_db(...)` should evolve from "encode later into an arbitrary writer" into a real planned DB build. The plan should know the complete final call-db backing shape before bytes are emitted:

- top-level DB header size and layer count;
- every positional call slot and its target wire-layer section;
- per-layer header, field descriptors, row table size, string sections, list sections, object-graph dependency sections, and total size;
- whether each slot can be imported from an existing backing layer, emitted from scalar/vector inputs, or must fall back to the existing generic encoder;
- the binding-owned layer name to write into each destination layer header.

The downstream allocator should see exactly one byte length for the entire call-db payload. It should not need to allocate one buffer per table, one buffer per `Batch`, or one buffer per `Array`. This keeps the FastDB call-db wire shape as a single contiguous backing buffer and avoids forcing downstream transports to add multi-buffer payload protocol support.

The first implementation may internally use a fixed external `WriteStream` that writes the final `FastVectorDbBuild::post(...)` output into the caller buffer. That removes the Python `bytes` temporary but still leaves builder-owned intermediate vectors. The target state is stronger: a plan writes DB and layer sections directly into precomputed offsets in the caller buffer whenever the value shape supports it.

### Layer Import And Partial Paste

Require-backed `Batch[T]`, `Array[T]`, and call-db views should not be repacked row by row or column by column when their backing layer is already binary-compatible with the target binding position. FastDB should add a layer import path:

- validate schema hash, feature type, profile, row count, string/list layout, and owner generation;
- locate the source layer byte range inside the source backing buffer;
- copy that contiguous layer range into the destination DB payload;
- write only binding-owned destination metadata, such as the layer name, when the binary layout permits it;
- reject or fall back clearly when the source layer cannot be imported safely.

This is the FastDB-owned version of "partial copy/partial paste." It preserves the single final call-db backing while avoiding the current bulk-repack path for common cached payloads. The first target should be fixed columnar batches. Variable-length strings and lists can follow once layer size and offset validation are covered. Object-graph layer import needs root-slot and dependency-order rules before it is enabled.

### Direct Layer Section Writers

For values that are not already backed by an importable layer, FastDB should write the target layer sections directly into the planned destination offsets instead of first building `m_table_buffer`, string vectors, list vectors, and then flushing them into a second buffer.

This matters for synthetic scalar tables, scalar arrays, and newly created columnar batches filled from NumPy arrays or Python sequences. Fixed numeric fields can write by row stride directly into the destination row table section. String and list fields require a planning pass for offsets/data lengths and an emission pass that writes offsets and payload data into their final sections.

The API should keep this direct layer writer generic. It writes to any writable buffer-protocol destination or native writer supplied by a caller. It must not name C-Two, shared memory, relay, or RPC transports.

### Future Output Arena Allocation

A later optimization can let a framework create an output arena before user code runs so `Batch.allocate(...)` can build directly on a caller-provided writable backing region. This is a stronger model than `encode_call_db_into(...)`: user code fills the final output buffer rather than building a FastDB value and exporting it later. It is useful for long-lived stub-supported RPC paths and fixed-size scientific kernels, but it requires explicit lifetime, commit/rollback, and partial-write semantics. It should be designed after logical `Batch`/`Array` allocation and writer-sink export are correct.

### Future SoA Profile

Consider a future call-db profile for true columnar/SoA record batches. Current FastDB fixed table storage is efficient for direct mapped row/table access, but bulk encoding from separate numeric columns into a row-major table buffer creates avoidable column-to-row copy costs. A `Batch[T]` payload profile that stores fixed fields as separate buffers plus string offsets/data would align better with scientific RPC workloads and with formats such as Arrow RecordBatch. This is a larger format decision and should not block exact-match export for the current profile.

## Rejected Or Deferred Ideas

### Public Global DB

A global DB that stores all current feature values is not the right primary abstraction. It would create hidden lifetime, naming, concurrency, generation, and schema-version coupling. The safer version is local object-backed generation tracking plus explicit encoded snapshot caches.

### C-Two-Specific FastDB APIs

FastDB should not know C-Two route names, CRM namespaces, relay state, `cc.hold`, or bridge policy. Generic call-db binding descriptors and buffer owners are acceptable. C-Two-specific planning and transport policy belong in C-Two.

### Making Users Return Raw Bytes

Raw bytes may remain an escape hatch for low-level code, but the normal high-performance path should be logical `Batch`/`Array` values with backing metadata. If users must return `bytes` to get performance, the payload model has leaked implementation details.

### Public Mutable Physical Table Names

Making `Batch` or `Array` expose a normal mutable `table_name` attribute is not the right call-envelope model. It conflates physical storage identity with wire-slot identity, creates cache invalidation hazards, and does not generalize to `Array` or object-graph batches that may not have exactly one physical table. Use positional `fdb.require(...)` envelopes instead.

### FastDB-Owned SHM Pools

FastDB should not own downstream shared-memory pools or expose C-Two-specific allocation APIs. It should expose size planning and writer-sink APIs that accept generic writable buffers; downstream runtimes decide whether those buffers come from SHM, files, inline byte arrays, or another transport allocation strategy.

### Per-Batch Transport Allocation

Do not make `Batch.allocate(...)` or `Array.allocate(...)` allocate one downstream transport buffer per value as the normal RPC path. Multi-slot RPC payloads need one final call-db backing buffer so the existing single-payload protocol remains valid. Per-value transport allocation is deferred to a future arena/stub design with explicit protocol and lifetime rules.

## Implementation Phases

### Phase 1: Document And Type The Semantic Boundary

Clarify in Python docs and type comments that `Batch` and `Array` are payload concepts, while `Table` is a storage-backed implementation. Update examples to avoid implying that `Table` is the only runtime representation of a batch.

### Phase 2: Named Table Construction

Status: implemented for Python `ColumnEngine.truncate(...)` via `Layout(feature_type, capacity, name=...)`, with tests proving named fixed tables can be read back through `engine.table(feature_type, name=...)`.

### Phase 3: Exact-Match Export

Status: implemented for the single fixed `Batch[T]` case through `try_export_call_db(...)`, returning a `memoryview` over the existing backing buffer when the database contains exactly the named call-db table. Tests cover exact export, non-exact table-name miss, and `encode_call_db(...)` choosing exact export before bulk repack.

### Phase 4: Logical Allocation API

Add logical `Batch` and `Array` runtime allocation APIs. `Batch.allocate(feature_type, capacity, profile="auto")` must hide `ColumnEngine`/`ObjectEngine` selection, expose fixed-column fill and column access when supported, expose object-graph write capability when selected, and preserve owner/generation metadata. `Array.allocate(item_type, capacity, profile="auto")` must expose scalar-vector semantics instead of forcing users through synthetic one-column tables.

Status: initially implemented for Python. `Batch.allocate(...)` creates a
columnar fixed table for fixed-table-eligible features and an object-graph row
container for object-graph-eligible features. `Array.allocate(...)` provides a
logical scalar array value for call-db authoring. Full object-graph write/view
parity and direct scalar-array backing remain follow-up work.

### Phase 5: Positional Envelope Values And Logical Return Values

Make call-db logical view paths return `Batch`/`Array` runtime values rather than raw `Table` objects. Add non-destructive require-envelope metadata so `prepare_call_db(...)` can validate positional values against binding slots without mutating physical storage names. Keep `.table(...)` or equivalent low-level access available only as an explicit view/debug/storage API.

### Phase 6: Array Runtime Cleanup

Introduce a real scalar array view/export path for `Array[Scalar]` and keep the synthetic one-column table only as an encoding compatibility fallback. Add owner invalidation and materialization tests for scalar arrays.

### Phase 7: Object-Graph Batch Semantics

Extend logical allocation and positional envelope binding to object-graph batches. The binding position identifies a logical root batch while `ObjectEngine` owns physical layers and dependencies. Add tests for nested references, list references, empty root batches, same-feature-type multi-slot rejection or support, and materialized decode parity.

### Phase 8: Encoded Snapshot Cache

Add generation-based cache invalidation for repeated exports. Add mutation tests proving writes invalidate stale cached payloads and read-only views cannot mutate cached backing state. Cache keys must include binding schema, profile, slot mapping, owner identity, and generation.

### Phase 9: Planned Direct DB Build API

Add `prepare_call_db(...)` and `encode_call_db_into(...)` after logical allocation, positional envelope binding, and cache semantics are stable. Keep it transport-neutral. The planner must return byte length and reusable emission state so downstream allocators can reserve exactly once and FastDB can write directly into a caller-provided buffer.

Status: initially implemented for Python call-db payloads. The public
`FastdbPreparedCallDb` plan exposes `nbytes`, `byte_length`, `write_into(...)`,
and `to_bytes()`. `encode_call_db_into(...)` uses the same plan. The current
planned writer avoids intermediate Python `bytes` for its layer-segment path,
but some fallback shapes still build an intermediate FastDB DB before writing.

Exit criteria:

- a multi-slot call-db payload has a single planned byte length for the full backing DB;
- the writer can emit the final DB bytes into a caller-provided writable buffer without allocating intermediate Python `bytes`;
- failures during planning or writing are deterministic and leave ownership of the destination buffer with the caller;
- tests compare `encode_call_db(...)` and `prepare_call_db(...).write_into(bytearray(...))` byte-for-byte for fixed columnar scalar, array, feature, and mixed multi-slot payloads.

### Phase 10: Layer Import And Metadata Patch

Add layer import for compatible backed fixed columnar `fdb.require(...)` `Batch[T]` values. Import should copy the source layer byte range into the planned destination DB and write only validated binding-owned metadata into the destination layer header.

Status: initially implemented for compatible fixed-columnar `Batch[T]` /
`Table[T]` layer import in planned multi-slot payloads. Exact buffer export
remains strict: `try_export_call_db(...)` only returns an existing buffer when
the backing DB already matches the binding-owned wire shape. `fdb.require(...)`
envelope values use `prepare_call_db(...).write_into(...)` so the destination
payload can write the binding-owned layer name without mutating the source
backing.

Exit criteria:

- require-backed batches can be exported into a binding-owned wire slot without per-row or per-column repack when the binary layout is compatible;
- same-feature multi-slot payloads remain distinct through positional envelope order and do not mix the wrong layer;
- mutation or owner-generation changes invalidate import/export caches;
- unsupported imports fall back to the generic encoder or raise a structured "not importable" result without corrupting the destination buffer.

### Phase 11: Direct Layer Section Writers

Add direct writers for values that are not already backed by importable layers. Start with synthetic scalar tables and fixed numeric arrays/batches, then add string/list sections once planning and offset tests are complete.

Exit criteria:

- fixed numeric scalar/array/batch payloads can write directly into planned destination offsets without first materializing a full intermediate DB buffer;
- string/list direct writers have planning tests for offsets, byte counts, empty values, and invalid offset rejection before they are enabled;
- object-graph direct write remains rejected or falls back until root-slot and dependency rules are specified.

### Phase 12: Output Arena Allocation

Design a future allocation context where `Batch.allocate(...)` can build on caller-provided output memory. Treat this as a separate high-performance path with explicit commit/rollback and lifetime rules, not as a requirement for the normal CRM payload authoring API.

## Acceptance Criteria

- `Batch[T]` and `Array[T]` are documented as payload-granularity concepts distinct from `Table[T]`.
- `Batch.allocate(...)` and `Array.allocate(...)` let payload authors create logical values without manually constructing `ColumnEngine`, `ObjectEngine`, `Layout`, `Table`, or `return_N` names.
- `fdb.require(...)` maps `Batch`/`Array` values onto binding slots positionally without mutating physical storage names.
- Single fixed `Batch[T]` exact-match export avoids creating a new `ColumnEngine`, avoids `push_many(...)`, and avoids table-level column repack.
- Object-graph `Batch[T]` has a defined root-position model that does not confuse call slot names with dependency layer names.
- Repeated export of an unchanged table can reuse an encoded snapshot or direct backing buffer.
- Mutating a backing table invalidates cached call-db exports.
- `view_call_db(...)` retained views remain owner-bound and invalidated by `fdb.invalidate(...)`.
- `prepare_call_db(...)` or an equivalent writer-sink API can compute the full multi-slot backing size and write one final call-db DB into a caller-provided writable buffer without allocating intermediate Python `bytes` on the success path.
- Importable backed batches use layer import/partial paste instead of per-row or per-column repack.
- Non-backed fixed numeric scalar/array/batch values can use direct layer section writers after planning.
- The normal multi-slot RPC payload model remains one contiguous call-db backing buffer; per-value transport buffers are deferred to a separate arena/stub design.
- No FastDB API introduced by this work imports or names C-Two.

## Verification

Run Python verification for FastDB-only changes:

```bash
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
git diff --check
uv build
```

Add focused verification as direct-build phases land:

```bash
uv run pytest tests/python/test_call_db_runtime.py -q
uv run pytest tests/python -q -k "call_db or batch or array or materialize"
```

Direct-build and layer-import tests should prove:

- `prepare_call_db(...).write_into(bytearray(...))` produces byte-for-byte equivalent payloads to `encode_call_db(...)` for fixed columnar scalar, array, feature, and mixed multi-slot bindings;
- writing into a too-small destination fails before corrupting the destination;
- writer failure leaves destination ownership with the caller and does not publish a partially committed payload;
- importable require-backed batches avoid row/column repack and still write the requested binding slot in the destination payload;
- same-feature multi-slot payloads keep distinct positions after import;
- mutation or owner-generation changes invalidate cached or import-planned exports;
- unsupported object-graph direct builds are rejected or materialized through an explicit fallback until root-slot dependency rules are implemented.

Run boundary scans before claiming FastDB completion:

```bash
rg -n "c_two|C-Two|cc\\.hold|InputLifetime|relay|route identity|CRM namespace" python/fastdb4py fastcarto/fastdb ts/fastdb4ts/src
```

Expected result: no FastDB runtime/API code depends on C-Two concepts. Documentation, historical tests, and downstream integration notes may mention external consumers, but Python, C++, and TypeScript runtime APIs must remain transport-neutral.

When the implementation touches binary layout, C++ APIs, or TypeScript/WASM parity, also run:

```bash
bash ts/build-wasm.sh
npm --prefix ts/fastdb4ts run build
npm run test:ts
```
