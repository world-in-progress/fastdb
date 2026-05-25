# FastDB Batch/Array Call-DB Fast Path Design

**Date:** 2026-05-26
**Status:** Proposed
**Scope:** FastDB-owned semantics and optimization plan for `Batch`, `Array`, `Table`, and generic call-db encode/view paths. This document is intentionally independent of C-Two route identity, relay behavior, CRM bridges, and transport lease policy.

## Context

FastDB currently exposes three related but not fully separated concepts. `Batch[T]` and `Array[T]` are CRM/RPC ABI markers that describe payload shapes, while `Table[T]` is the Python runtime object for a named physical table/layer inside a `ColumnEngine` or loaded FastDB buffer. Generic call-db currently bridges those concepts by mapping each logical non-scalar call value to a FastDB table: `Batch[Feature]` becomes a feature table, `Array[Scalar]` becomes a synthetic one-column feature table with a `value` field, and scalar arguments or returns are packed into a synthetic scalar table.

This works functionally, but it makes performance depend on the runtime object shape instead of the logical payload type. If a resource returns a `Table[T]`, call-db can use a bulk path, but it still builds a new call-db database, creates a target table such as `return_0`, copies source columns into that table, combines the DB, and returns a new bytes payload. If a resource returns `list[T]` or another iterable of rows, call-db falls back to row-oriented coercion and `push_many(...)`. The result is a semantic mismatch: user-facing signatures say `Batch[T]` or `Array[T]`, but the fast path is available only when the implementation happens to return the right physical `Table[T]`.

Recent C-Two/FastDB benchmark investigation showed that this mismatch is the dominant cost for large cached payloads. For a 3M-row numeric batch, old raw structured-array transfer could produce wire bytes with `arr.tobytes()` in roughly 1-3 ms, while current FastDB call-db bulk encoding of an equivalent 80 MiB numeric table takes roughly 70-90 ms because it repacks the table into a new call-db database. Client-side view creation and aggregation are not the bottleneck; the bottleneck is server-side call-db repacking.

## Decision

Treat `Batch[T]` and `Array[T]` as first-class payload concepts, and treat `Table[T]` as one possible storage-backed implementation of `Batch[T]`, not as the payload concept itself. FastDB should provide runtime objects or capabilities that answer "is this logical payload already backed by a call-db-compatible buffer?" before falling back to bulk repack or row push.

The intended user-level model is:

| Concept | Meaning | Public role |
| --- | --- | --- |
| `Feature` | One logical row or object record | User-facing schema and single-value payload |
| `Batch[Feature]` | A logical batch of feature rows | User-facing CRM/RPC payload type |
| `Array[Scalar]` | A logical homogeneous scalar vector | User-facing CRM/RPC payload type |
| `Table[Feature]` | A named physical FastDB table/layer in an engine or loaded buffer | Storage/runtime implementation detail that may back a `Batch` |
| `BatchView` / `ArrayView` | Buffer-backed logical payload view with owner/lifetime metadata | Runtime value returned by call-db view paths |

FastDB should not make users return raw `bytes` just to get performance. A resource should be able to return a logical `Batch[T]` or `Array[T]` value, and FastDB should choose the fastest correct encode/export path from the value's backing metadata.

## Current Behavior To Preserve

- `Table[T]` must remain useful for storage-engine workflows: named layers, direct read/write, fixed-scale fill, mapped rows, column access, materialization, and owner-bound invalidation.
- `Batch[T]` and `Array[T]` must remain valid type-level ABI markers for external systems that derive call-db bindings from signatures.
- `view_call_db(binding, payload, owner=...)` must keep returning owner-bound FastDB-managed views that can be invalidated through `fdb.invalidate(...)`.
- `fdb.materialize(...)` and `.to_owned()` must remain the explicit detach mechanism for data that outlives a backing buffer.
- `unsafe_numpy_view()` remains an explicit trusted escape hatch and must not be used as the default reusable lease path.

## Target Runtime Capabilities

### Named Layouts

Add a way to create physical tables with a call-db target name from the start, for example `Layout(Coord, n, name="return_0")` or an equivalent builder API. The current `ColumnEngine.truncate([Layout(Coord, n)])` uses the feature/layer name, which forces later call-db output repacking when the binding expects `return_0` or a parameter name. Named layouts are the smallest prerequisite for exact-match export.

### Logical Batch And Array Views

Introduce public or semi-public runtime values for logical payloads, such as `BatchView[T]` and `ArrayView[T]`, or make existing call-db views expose equivalent capabilities under stable names. These should carry enough metadata to decide whether the value is exact-layout compatible with a binding: feature type, scalar kind, logical table name, row count, owner, generation, writeability, profile, and schema hash.

`Array[Scalar]` should not be permanently modeled as "just a one-column feature table" at the payload layer. The current one-column table can remain an internal encoding fallback, but the logical runtime should expose scalar-vector operations and buffer export directly.

### Exact-Match Buffer Export

Add a generic API such as `try_export_call_db(binding, value)` or `export_call_db_buffer(binding, value, *, allow_repack=False)`. When the value is already backed by a call-db-compatible buffer, it should return a buffer-protocol object or owner-bound export without rebuilding a DB. When the value is compatible only after a table-name alias or metadata adjustment, the API should use a metadata-only path if the binary layout permits it. When exact export is impossible, the API should return `None` or raise a structured error so callers can fall back to existing `encode_call_db(...)`.

The first exact-match target should be the common single-output `Batch[T]` case: one fixed table, matching feature schema hash, matching call-db logical table name, no object-graph dependencies, and a combined or otherwise exportable contiguous backing buffer.

### Encoded Snapshot Cache

For values that are frequently returned unchanged, add an encoded snapshot cache keyed by at least `(binding.schema_sha256, value owner identity, generation, logical table name/profile)`. The cache should live on FastDB-owned objects or an explicit FastDB cache object, not in downstream RPC frameworks. Any write to a backing table must bump the relevant generation and invalidate the cache.

This cache is a fallback after exact buffer export. It is still useful for tables whose physical layer name or layout requires one repack but then remains stable across calls.

### Encode Into Caller-Provided Buffer

Add a lower-level `encode_call_db_into(binding, value, writer)` or `post_into(buffer)` style API once exact export and caching are working. Downstream transports can then provide a target shared-memory allocation, and FastDB can serialize directly into that memory instead of first allocating Python `bytes`. This should stay generic: the writer is a buffer sink, not a C-Two object.

### Future SoA Profile

Consider a future call-db profile for true columnar/SoA record batches. Current FastDB fixed table storage is efficient for direct mapped row/table access, but bulk encoding from separate numeric columns into a row-major table buffer creates avoidable column-to-row copy costs. A `Batch[T]` payload profile that stores fixed fields as separate buffers plus string offsets/data would align better with scientific RPC workloads and with formats such as Arrow RecordBatch. This is a larger format decision and should not block exact-match export for the current profile.

## Rejected Or Deferred Ideas

### Public Global DB

A global DB that stores all current feature values is not the right primary abstraction. It would create hidden lifetime, naming, concurrency, generation, and schema-version coupling. The safer version is local object-backed generation tracking plus explicit encoded snapshot caches.

### C-Two-Specific FastDB APIs

FastDB should not know C-Two route names, CRM namespaces, relay state, `cc.hold`, or bridge policy. Generic call-db binding descriptors and buffer owners are acceptable. C-Two-specific planning and transport policy belong in C-Two.

### Making Users Return Raw Bytes

Raw bytes may remain an escape hatch for low-level code, but the normal high-performance path should be logical `Batch`/`Array` values with backing metadata. If users must return `bytes` to get performance, the payload model has leaked implementation details.

## Implementation Phases

### Phase 1: Document And Type The Semantic Boundary

Clarify in Python docs and type comments that `Batch` and `Array` are payload concepts, while `Table` is a storage-backed implementation. Update examples to avoid implying that `Table` is the only runtime representation of a batch.

### Phase 2: Named Table Construction

Extend `Layout` or `ColumnEngine.truncate(...)` to support explicit table/layer names. Add tests proving a table can be built with a call-db-compatible name and read back through ordinary `engine.table(feature_type, name=...)`.

### Phase 3: Exact-Match Export

Implement `try_export_call_db(...)` for the single fixed `Batch[T]` case. Add tests that compare exported bytes/views with the existing `encode_call_db(...)` fallback and prove no row-oriented `push_many(...)` path is used.

### Phase 4: Encoded Snapshot Cache

Add generation-based cache invalidation for repeated exports. Add mutation tests proving writes invalidate stale cached payloads and read-only views cannot mutate cached backing state.

### Phase 5: Array Runtime Cleanup

Introduce a real scalar array view/export path for `Array[Scalar]` and keep the synthetic one-column table only as an encoding compatibility fallback. Add owner invalidation and materialization tests for scalar arrays.

### Phase 6: Direct Writer API

Add an encode-into or writer-sink API after the exact export and cache semantics are stable. Keep it transport-neutral.

## Acceptance Criteria

- `Batch[T]` and `Array[T]` are documented as payload-granularity concepts distinct from `Table[T]`.
- A table can be constructed with the exact call-db table name required by a binding.
- Single fixed `Batch[T]` exact-match export avoids creating a new `ColumnEngine`, avoids `push_many(...)`, and avoids table-level column repack.
- Repeated export of an unchanged table can reuse an encoded snapshot or direct backing buffer.
- Mutating a backing table invalidates cached call-db exports.
- `view_call_db(...)` retained views remain owner-bound and invalidated by `fdb.invalidate(...)`.
- No FastDB API introduced by this work imports or names C-Two.

## Verification

Run Python verification for FastDB-only changes:

```bash
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
git diff --check
uv build
```

When the implementation touches binary layout, C++ APIs, or TypeScript/WASM parity, also run:

```bash
bash ts/build-wasm.sh
npm --prefix ts/fastdb4ts run build
npm run test:ts
```
