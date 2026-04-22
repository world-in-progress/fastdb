# Truncate String Ingest Optimization Design

## Problem

`ColumnEngine.truncate()` fixed-table writes are already much faster than the old split string path, but the benchmark still shows a stable gap versus Arrow on schemas that include `STR`.

Current evidence:

- `column_trunc_str` is now clearly faster than `column_push`
- wire size is already near-identical to Arrow
- almost all remaining cost sits inside the build phase
- the main Python hot path is `StringColumn._normalize_fill_values()`

At `N = 1_000_000`, the measured cost breaks down roughly into:

- Python name generation: ~220 ms
- Python UTF-8 normalization into `offsets + data`: ~186 ms
- fixed-table mixed publish with pre-encoded payloads: ~42 ms

This means the current bottleneck is no longer the old Python whole-database rebuild. It is now the Python-side conversion from `Iterable[str]` into the native UTF-8 column payload.

## Goal

Reduce the remaining `ColumnEngine.truncate() + STR` ingest cost by addressing the real hot path in two layers:

1. accelerate the default high-level `Table.fill(..., name=strings)` path
2. expose a first-class pre-encoded UTF-8 helper so advanced callers and benchmarks can bypass repeated string packing work

The intent is to lower the build-phase gap without changing the fixed-table read model or the current successful unified `Table.fill(**cols)` semantics.

## Non-Goals

- No redesign of the current fixed-table publish/remap model in this round
- No new writable semantics for `load()` or shared-memory readers
- No change to the wire format
- No attempt to redesign `FastVectorDbBuild` / `FastVectorDbLayerBuild` into a new truncate-only storage engine yet
- No requirement to fully match Arrow in this round

## Recommended Approach

Implement this as two consecutive optimizations in one project:

### Phase A — Faster default Python string ingestion

Keep `Table.fill(..., name=strings)` as the default user-facing path, but optimize the string normalization path for common real inputs:

- `list[str]`
- `tuple[str]`
- already-materialized string sequences with known length

The fast path should avoid the most expensive generic behaviors in the current implementation:

- per-item fallback through `str(value)` for already-string inputs
- repeated container growth patterns where a better pre-sized strategy is available
- unnecessary conversions between Python containers and NumPy buffers

Generic iterables must still remain supported, but they can fall back to the slower compatibility path.

### Phase B — Explicit pre-encoded UTF-8 helper

Add a public helper that converts Python strings into the exact `(offsets:uint32, data:uint8)` payload accepted by `StringColumn.fill_utf8(...)`.

This gives three benefits:

1. advanced callers can cache or reuse encoded payloads across writes
2. benchmarks can separate "string preparation" from "native ingest"
3. the public API makes the fast low-level path discoverable instead of leaving it buried behind ad hoc benchmark helpers

## Public API

### Existing API that remains primary

`Table.fill(**cols)` remains the recommended fixed-table batch-ingest API.

Example:

```python
tbl.fill(row_id=ids, x=xs, y=ys, z=zs, name=names)
```

### New API for advanced callers

Add a public helper with a name in this shape:

```python
offsets, data = pack_utf8_column(strings)
tbl.column.name.fill_utf8(offsets, data)
```

The exact exported name can be finalized during implementation, but the API contract must be:

- input: Python string-like sequence / iterable
- output:
  - `offsets`: contiguous 1D `np.uint32`
  - `data`: contiguous 1D `np.uint8`
- semantics must match `StringColumn.fill(strings)` exactly for:
  - empty strings
  - `None` coercion to `""`
  - ASCII
  - multi-byte UTF-8 text

`StringColumn.fill_utf8(...)` remains the low-level write entrypoint. The new helper simply makes its input format easy and official.

## Internal Design

### 1. Split normalization into fast path and compatibility path

Refactor `StringColumn._normalize_fill_values()` into:

- a fast path for common string sequences
- a compatibility path for generic iterables / mixed values

The fast path should:

- detect when the input is already a concrete sequence
- avoid calling `str()` for values that are already `str`
- build `offsets` and `data` with fewer intermediate objects

The compatibility path should preserve today's behavior for arbitrary iterables.

### 2. Share one UTF-8 packing core

The new public helper and `StringColumn.fill()` must share the same packing implementation. We do not want:

- one packing algorithm for `fill()`
- another packing algorithm for the helper
- a third benchmark-only helper

There should be one source of truth for:

- string-to-bytes encoding
- offsets construction
- output dtypes / contiguity guarantees
- validation of the resulting payload

### 3. Keep native write flow unchanged in this round

Once `(offsets, data)` exist, the native write flow remains:

1. fixed-table bridge dispatch
2. `set_string_column_bulk(...)`
3. one publish/remap cycle

This round optimizes the ingress to that bridge, not the bridge itself.

## Benchmark Plan

Update the existing truncate benchmark so the full-schema section distinguishes:

1. raw Python strings through `tbl.fill(..., name=names)`
2. prepacked UTF-8 through `pack_utf8_column(names)` + `fill_utf8(...)`

This is important because it lets us answer two separate questions:

- how fast is the default user path?
- how fast is the native fixed-table path once string packing is out of the way?

The benchmark analysis after this round should explicitly report both numbers.

## Testing

Add or update tests to cover:

- helper output dtypes (`uint32` offsets, `uint8` data)
- helper correctness for empty strings, ASCII, and multi-byte UTF-8
- helper + `fill_utf8(...)` round-trip equivalence with `fill(strings)`
- compatibility behavior for `None`
- continued correctness of mixed numeric + `STR` `Table.fill(...)`
- benchmark-facing tests or assertions if any benchmark helper becomes public

## Success Criteria

- `Table.fill(..., name=strings)` becomes faster on the existing Kostya benchmark
- benchmark output includes a prepacked UTF-8 comparison path
- prepacked UTF-8 ingest demonstrates that most remaining cost after Phase A is outside the native string bulk setter
- public docs can clearly explain when to use:
  - `Table.fill(..., name=strings)`
  - `pack_utf8_column(strings)` + `fill_utf8(...)`

## Deferred Architectural Follow-up

The user's larger idea is valid and should remain an explicit follow-up:

> For truncate mode, add a dedicated C++ implementation that directly materializes the final fixed columnar memory product instead of reusing the current `dbbuild` row-recording path.

That direction is promising because it could attack two separate residual gaps at once:

- the numeric gap versus Arrow (`setNumericColumnBulk()` still performs row-wise scatter copies)
- the publish/materialization cost of the current fixed-table builder model

However, it is intentionally out of scope for this round because it is not a local optimization. It is a new truncate-specific build architecture and deserves its own spec, benchmark targets, and rollout plan.
