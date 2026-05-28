# FastDB Require Envelope And Neutral Allocator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a FastDB-owned, transport-neutral `fdb.require(...)` envelope allocation path so fixed-size columnar call-db payloads can be planned once and built directly into caller-provided memory without exposing `ColumnEngine`, physical `Table` names, C-Two concepts, or raw bytes to normal users.

**Architecture:** FastDB owns payload semantics, call-db layout planning, typed requirement specs, envelope ownership, validation, direct section writers, and generic allocator interfaces. Downstream runtimes provide a neutral writable allocator or a writable buffer, but FastDB never imports C-Two, route identity, SHM pool APIs, CRM policy, or relay concepts. The public authoring API is position-based and type-friendly: `fdb.require(fdb.batch(Cell, rows=n), fdb.array(fdb.F32, rows=m))` returns typed logical `Batch` and `Array` values in the same order.

**Tech Stack:** Python 3.10+, `fastdb4py`, existing C++/SWIG FastVectorDB core, `pytest`, `uv`, generic buffer protocol, future TypeScript/WASM parity target.

---

## Context

The current FastDB call-db writer can compute `nbytes` and write an existing payload or imported layer into a caller-provided writable destination through `FastdbPreparedCallDb.write_into(...)`. That removes a temporary Python `bytes` object for some direct IPC paths, but it does not yet give resource code a way to build the final call-db envelope directly. A cached `Batch.allocate(Coord, n)` still creates a normal FastDB backing first, then the call-db writer copies that backing layer into the final envelope. For 3M coordinate rows this leaves a large response copy and cold-read cost even when C-Two uses `cc.hold(...)`.

The design discussion rejected three public API directions:

- No per-`Batch` transport allocation for ordinary RPC calls. Multi-slot payloads must remain one contiguous call-db backing buffer.
- No public `DB` return type as the high-performance CRM model. A storage DB does not explain which layer maps to which input or output slot.
- No user-visible logical slot names, aliases, `return_0` strings, or mutable physical table names in the first version. Call-db binding order already defines slot order.

This plan supersedes the earlier public slot-alias wording in `docs/opt/batch-array-call-db-fast-path-design.md` for the first implementation. Internal slot metadata remains required, but the public `fdb.require(...)` path is positional.

## Target User Model

Single return:

```python
cells = fdb.require(fdb.batch(Cell, rows=n))
cells.fill(row_id=ids, x=x, y=y, z=z)
return cells
```

Multiple returns:

```python
cells, residual = fdb.require(
    fdb.batch(Cell, rows=n),
    fdb.array(fdb.F32, rows=m),
)
cells.fill(...)
residual.fill(...)
return cells, residual
```

The returned values are normal logical FastDB values:

- `cells` is a `fdb.Batch[Cell]` at runtime and in typing.
- `residual` is a `fdb.Array[fdb.F32]` at runtime and in typing.
- The requirement index maps to call-db value position. Slot names stay internal.
- If a returned tuple is reordered, FastDB/C-Two should report a type/slot mismatch during prepare/write.

The first implementation only targets fixed-size columnar values:

- supported: fixed-size scalar CRM slots as ordinary values, fixed `Array[Scalar]`, fixed `Batch[Feature]` where the feature is columnar eligible;
- rejected or fallback: object graph, `REF`, nested objects, variable-size list fields, unknown row counts, partial/streaming writes;
- first-version policy for `STR`/`BYTES`: normal `prepare_call_db(...)` may fall back to the existing encoder, but strict direct-build helpers must reject these shapes with an `unsupported direct build` error until byte-size planning and direct string/bytes section writers exist.

## Ownership Boundary

### FastDB Owns

- `fdb.batch(...)`, `fdb.array(...)`, and `fdb.require(...)` public APIs.
- Requirement spec validation and Python typing overloads.
- Call-db binding compatibility checks.
- Envelope owner, slot index metadata, generation, writeability, and commit/rollback state.
- Final call-db byte layout, direct DB/layer section planning, direct section writers, layer import, and fallback decisions.
- Generic neutral allocator protocol and byte-buffer writer semantics.
- FastDB view invalidation and materialization semantics.

### Downstream Runtimes Own

- The allocator implementation, such as SHM, mmap, bytearray, WASM memory, or file-backed memory.
- Allocation lifetime, rollback, and transport metadata.
- Whether to use direct allocation for a specific call.

### FastDB Must Not Own

- C-Two route names, CRM namespaces, route fingerprints, `cc.hold`, `InputLifetime`, relay behavior, or C-Two codegen.
- A C-Two-specific allocator class.
- Per-value transport buffers as the normal call-db model.

## Public API Shape

### Requirement specs

Add lightweight immutable spec builders:

```python
spec = fdb.batch(Cell, rows=n)
spec = fdb.array(fdb.F32, rows=m)
```

Rules:

- Specs are not runtime values and do not allocate by themselves.
- Specs do not accept `name`, `alias`, `return_0`, or physical table names.
- `rows` must be a non-negative `int`.
- `batch(feature, rows=...)` requires a FastDB `@feature` class.
- `array(item_type, rows=...)` requires a supported FastDB scalar alias.
- Scalar CRM slots are handled as ordinary fixed-size values by call-db planning in the first version; there is no public `fdb.scalar(...)` requirement builder in this plan.

### `fdb.require(...)`

Add typed overloads for common arities:

```python
cells = fdb.require(fdb.batch(Cell, rows=n))
cells, weights = fdb.require(fdb.batch(Cell, rows=n), fdb.array(fdb.F32, rows=m))
```

Runtime rules:

- One spec returns one logical value, not a one-item tuple.
- Multiple specs return a tuple of logical values in the same order.
- The returned values share one internal envelope owner.
- No public index accessor is the primary API. Do not expose `out.batch(0)` as the normal user path.
- The envelope can later bind to a call-db binding by value position.

### Neutral allocator protocol

Provide a FastDB-owned protocol shape such as:

```python
class WritableAllocation:
    @property
    def buffer(self) -> memoryview: ...
    def commit(self) -> object: ...
    def rollback(self) -> None: ...

class WritableAllocator:
    def allocate(self, nbytes: int) -> WritableAllocation: ...
```

Rules:

- The protocol is generic. It must not mention SHM, C-Two, route identity, or transport handles.
- FastDB may also accept a plain writable buffer for tests and local builds.
- FastDB closes or marks its own build views invalid after commit/rollback.
- Downstream runtimes keep ownership of allocation release after commit.

## Implementation Tasks

### Task 1: Add requirement spec types and public builders

**Files:**

- Modify: `python/fastdb4py/type.py`
- Modify: `python/fastdb4py/__init__.py`
- Test: `tests/python/test_require_envelope.py`

- [ ] Add private immutable spec classes with public constructor functions:

```python
@dataclass(frozen=True)
class BatchRequirement(Generic[T]):
    feature_type: type[T]
    rows: int
    profile: str = "auto"

@dataclass(frozen=True)
class ArrayRequirement(Generic[T]):
    item_type: object
    rows: int
```

- [ ] Add public builders:

```python
def batch(feature_type: type[T], *, rows: int, profile: str = "auto") -> BatchRequirement[T]: ...
def array(item_type: object, *, rows: int) -> ArrayRequirement[Any]: ...
```

- [ ] Validate that `rows` is a non-negative `int`, feature types are FastDB features, and array item types are supported scalar aliases.
- [ ] Export `batch`, `array`, `BatchRequirement`, and `ArrayRequirement` from `fastdb4py.__init__`.
- [ ] Add tests for valid specs, invalid row counts, invalid feature types, invalid scalar kinds, and public exports.

### Task 2: Add envelope owner and typed `require(...)`

**Files:**

- Modify: `python/fastdb4py/type.py`
- Create: `python/fastdb4py/require.py`
- Modify: `python/fastdb4py/__init__.py`
- Test: `tests/python/test_require_envelope.py`

- [ ] Implement an internal `RequireEnvelope` owner that stores specs in order, value objects, generation, and state (`open`, `committed`, `rolled_back`).
- [ ] Implement `require(...)` with typing overloads for at least arities 1 through 5:

```python
@overload
def require(__a: BatchRequirement[T]) -> Batch[T]: ...

@overload
def require(__a: BatchRequirement[T], __b: ArrayRequirement[U]) -> tuple[Batch[T], Array[U]]: ...
```

- [ ] Runtime `require(...)` should allocate logical `Batch`/`Array` values backed by the shared envelope owner.
- [ ] For columnar `Batch`, use the existing `Batch.allocate(...)` storage behavior during this phase and attach envelope metadata so Task 3 writer planning can discover the positional requirement.
- [ ] For `Array`, use the existing Python logical array storage behavior during this phase, attach envelope metadata, and record the fixed row count.
- [ ] Tests should assert that single-spec `require` returns a value, multi-spec `require` returns a tuple, each value has expected runtime type, and ordering is preserved.
- [ ] Tests should assert there is no public `alias` or public slot-name parameter in builder signatures.

### Task 3: Teach call-db planning to recognize envelope-backed values

**Files:**

- Modify: `python/fastdb4py/call_db.py`
- Modify: `python/fastdb4py/type.py`
- Test: `tests/python/test_call_db_runtime.py`
- Test: `tests/python/test_require_envelope.py`

- [ ] Add internal helpers that detect whether aggregate values for a call-db binding belong to one compatible `RequireEnvelope`.
- [ ] Validate that binding value positions match requirement positions.
- [ ] Validate kind compatibility:
  - `BatchRequirement[T]` matches feature table `T` with cardinality `many`;
  - `ArrayRequirement[S]` matches array table item kind `S`;
  - ordinary fixed-size scalar slots may be direct-written without a requirement envelope;
  - non-envelope `Batch` or `Array` values stay on the existing export/import path.
- [ ] Reject mismatched ordering with a clear error such as `call-db slot 1 expected Array[F32], got Batch[Cell]`.
- [ ] Reject mixed envelope and non-envelope aggregate values in the first direct-envelope path; allow ordinary fixed-size scalar values in the same call-db payload.
- [ ] Fall back to existing `prepare_call_db(...)` only when safe and byte-for-byte compatible.
- [ ] Preserve existing exact export and layer import paths for ordinary `Batch.allocate(...)` values.

### Task 4: Add neutral direct build plan

**Files:**

- Modify: `python/fastdb4py/call_db.py`
- Create: `python/fastdb4py/allocator.py`
- Test: `tests/python/test_require_envelope.py`
- Test: `tests/python/test_call_db_runtime.py`

- [ ] Introduce a plan object for direct envelope builds, either by extending `FastdbPreparedCallDb` or adding an internal subclass with the same public surface:

```python
plan = fdb.prepare_call_db(binding, values)
plan.nbytes
plan.write_into(destination)
```

- [ ] The plan must compute one byte length for the full call-db backing before allocation.
- [ ] The first direct build target is fixed columnar batches and scalar arrays with known row count.
- [ ] For unsupported fields (`REF`, object graph, variable-size lists, unsupported `STR`/`BYTES` direct sections before their planning exists), default `prepare_call_db(...)` must use the existing fallback path when it can still produce correct bytes.
- [ ] Add a strict direct-build mode for allocator-backed helpers; in that mode unsupported fields must raise a structured `unsupported direct build` error before allocation.
- [ ] Add tests proving `prepare_call_db(...).write_into(bytearray(...))` equals `encode_call_db(...)` byte-for-byte for single and multi-slot fixed numeric envelopes.
- [ ] Add a scalar-plus-envelope test such as `(fdb.I32, fdb.Batch[Cell], fdb.Array[fdb.F32])` to prove ordinary fixed-size scalar slots can share one final backing with envelope-backed aggregate slots.
- [ ] Add tests proving too-small destinations fail before publishing success.

### Task 5: Add direct fixed-column section writers

**Files:**

- Modify: `python/fastdb4py/call_db.py`
- Keep unchanged in the first Python implementation unless a failing capability or performance test proves Python cannot write final sections through existing APIs:
  - `fastcarto/fastdb/src/FastVectorDbBuild.cpp`
  - `fastcarto/fastdb/src/FastVectorDbBuild_p.h`
  - `fastcarto/fastdb/src/FastVectorDbLayerBuild.cpp`
  - `fastcarto/fastdb/src/FastVectorDbLayerBuild_p.h`
  - `fastcarto/fastdb/include/fastdb.h`
  - `fastcarto/fastdb/swig/fastdb4py.i`
- Test: `tests/python/test_require_envelope.py`

- [ ] Plan final DB header, layer headers, field descriptors, row table offsets, and total layer sizes.
- [ ] For fixed numeric fields, emit data directly into planned final row sections when the envelope-backed batch has column buffers.
- [ ] For scalar arrays, emit the one-column payload section directly without first constructing a temporary one-column table when possible.
- [ ] Keep string/bytes direct writers disabled until byte-size planning exists.
- [ ] Add a regression test that instruments or monkeypatches the old fallback encoder to prove the direct numeric envelope path does not call it.

### Task 6: Add neutral allocator integration helpers

**Files:**

- Create: `python/fastdb4py/allocator.py`
- Modify: `python/fastdb4py/call_db.py`
- Test: `tests/python/test_require_envelope.py`

- [ ] Add a small in-memory allocator implementation for tests:

```python
class BytearrayAllocator:
    def allocate(self, nbytes: int) -> WritableAllocation: ...
```

- [ ] Add `build_call_db(binding, values, allocator, *, direct_required=False)` that plans, allocates once, writes once, and commits or rolls back.
- [ ] Prove rollback happens when `write_into(...)` raises.
- [ ] Prove the allocator receives exactly one allocation for a multi-slot payload.
- [ ] Do not expose transport-specific names.

### Task 7: Add object-graph and variable-size guardrails

**Files:**

- Modify: `python/fastdb4py/call_db.py`
- Test: `tests/python/test_require_envelope.py`

- [ ] Reject object-graph `BatchRequirement` direct build with a clear message that root-slot and dependency-layer direct build is not implemented yet.
- [ ] Reject nested object, `REF`, `list[REF]`, and non-native list direct builds in the first phase.
- [ ] Treat `STR` and `BYTES` as fallback-only in default `prepare_call_db(...)` until direct byte-size planning exists; add tests proving fallback bytes match the existing encoder.
- [ ] Treat `STR` and `BYTES` as rejected in strict direct-build allocator mode until direct string/bytes section writers exist; add tests proving no allocation is committed after the rejection.
- [ ] Keep `Batch.allocate(..., profile="object_graph")` behavior intact for non-direct ordinary FastDB use.

### Task 8: Update docs and examples

**Files:**

- Modify: `README.md`
- Modify: `docs/opt/batch-array-call-db-fast-path-design.md`
- Test: documentation scans

- [ ] Document `fdb.require(...)` as a high-performance call-envelope allocation API, not a replacement for ordinary `Batch.allocate(...)`.
- [ ] Remove or supersede public `bind_call_slot(...)`, `with_call_slot(...)`, `alias(...)`, or `name="return_0"` guidance from current recommended docs.
- [ ] Document that position, not name, is the public mapping rule.
- [ ] Document that `Table` and `ColumnEngine` remain advanced storage APIs, not the CRM authoring surface.

### Task 9: Benchmark direct envelope build

**Files:**

- Modify: `tests/python/benchmark_kostya.py`
- Modify: `docs/opt/kostya-push-optimization-report.md` only after measured data exists

- [ ] Add benchmark variants:
  - current `Batch.allocate(...)` then `prepare_call_db(...)`;
  - `fdb.require(...)` direct envelope build into `bytearray`;
  - direct envelope build plus unsafe read;
  - fallback encode for string/bytes fields, labeled as non-direct.
- [ ] Report build, write, view, and read phases separately.
- [ ] Do not compare numeric-only historical raw ndarray results as equivalent to full mixed-field FastDB CRM payloads.

## Verification

Run after Python-only implementation slices:

```bash
uv run pytest tests/python/test_require_envelope.py -q
uv run pytest tests/python/test_call_db_runtime.py -q
uv run pytest tests/python -q -k "call_db or batch or array or materialize"
uv run python -m compileall -q python/fastdb4py tests/python
git diff --check
uv build
```

Run before completion:

```bash
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
git diff --check
uv build
```

Boundary scan:

```bash
rg -n "c_two|C-Two|cc\\.hold|InputLifetime|relay|route identity|CRM namespace|SHM pool" python/fastdb4py fastcarto/fastdb ts/fastdb4ts/src
```

Expected: no runtime/API code depends on C-Two or transport-specific concepts.

## Review Checklist

- [ ] Public API has no `alias`, `name`, `return_0`, or physical table name in the first `require(...)` path.
- [ ] `fdb.require(...)` is typed and does not force users through `out.batch(0)` or index access.
- [ ] Multi-slot payloads allocate one final call-db backing, not one buffer per value.
- [ ] Neutral allocator protocol is generic and transport-free.
- [ ] Direct build is limited to fixed-size columnar shapes until object-graph and variable-size planning is specified.
- [ ] Existing `Batch.allocate(...)`, `Array.allocate(...)`, `Table`, `ColumnEngine`, and `ObjectEngine` behavior remains available for standalone FastDB use.
- [ ] Unsupported shapes fail clearly or fall back through documented paths.
