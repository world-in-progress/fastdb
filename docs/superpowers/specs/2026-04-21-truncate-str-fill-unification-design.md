# Truncate STR Fill Unification Design

## Problem

`ColumnEngine.truncate()` now supports `STR`, but the fixed-table write path is still split:

- numeric columns use `Table.fill(...)`
- string columns use `table.column.<name>.fill(...)`
- fixed-table string writes fall back to `_rewrite_string_column(...)`, which decodes data into Python objects, rebuilds the whole database, and republishes it

This creates both a performance problem and a mental-model problem. The benchmarked `column_trunc_str` path is slower than `column_push` because it is not a true native bulk-write path.

## Goal

Make fixed truncate tables support a single bulk-write model:

- `Table.fill(**cols)` accepts both numeric and `STR` fields
- string writes no longer rebuild the whole database in Python
- column-level string helpers remain available, but become thin wrappers over the same write core
- validation happens before any write so mixed-column fills cannot leave partial updates

This round is limited to Python binding fixed truncate tables. It does not redesign `load()` / shared-memory loaded tables into writable objects.

## Non-Goals

- No redesign of the broader `ColumnEngine` lifecycle
- No new required public API for normal users
- No attempt to make loaded read-only databases writable
- No requirement to match Arrow performance in the first iteration

## Recommended Approach

Use `Table.fill(**cols)` as the unified batch-write coordinator for fixed truncate tables.

Internally, add a fixed-table write bridge that owns the native write path for both:

- numeric columns
- string columns via `set_string_column_bulk(...)`

The current `_rewrite_string_column(...)` path should be removed from the fixed truncate string fill flow.

## API Shape

### Public API

`Table.fill(**cols)` becomes the recommended batch-ingest entrypoint for fixed truncate tables.

Examples:

```python
tbl.fill(row_id=ids, x=xs, y=ys, z=zs, name=names)
tbl.fill(name=names)
```

`StringColumn.fill()` and `StringColumn.fill_utf8()` remain supported, but are no longer separate write systems. They delegate into the same fixed-table write core used by `Table.fill(...)`.

### Read-only behavior

Tables mapped from `ColumnEngine.load(...)` or shared-memory readers remain read-only. Calling `fill()` or string bulk-write helpers on those tables continues to raise an explicit error.

## Internal Design

### 1. Table.fill as coordinator

`Table.fill(**cols)` is responsible for:

1. checking the table is fixed-scale and writable
2. checking at least one field was provided
3. resolving each field name through schema metadata
4. validating every provided column length against `len(table)`
5. validating all provided column lengths are mutually consistent
6. normalizing payloads into native-friendly contiguous buffers
7. dispatching numeric and string writes through one bridge
8. publishing exactly once after the full batch succeeds

If validation fails, `fill()` must raise before any native mutation begins.

### 2. Fixed-table write bridge

The bridge is an internal object or internal `ColumnEngine` facility that retains the mutable truncate build state needed for batch updates while the public table remains mapped to the readable fixed snapshot.

Responsibilities:

- write numeric columns through a native bulk numeric path
- write string columns through `set_string_column_bulk(...)`
- publish one new fixed snapshot after the batch completes
- remap cached `Table` objects and column accessors to the new snapshot

This keeps a single source of truth for fixed-table batch writes.

### 3. String delegation

`StringColumn.fill(strings)`:

- encodes Python strings into UTF-8 bytes and offsets
- delegates into the same bridge used by `Table.fill(...)`

`StringColumn.fill_utf8(offsets, data)`:

- validates offsets and byte-count invariants
- delegates into the same bridge without round-tripping through Python strings

Neither method may rebuild the database row-by-row.

## Validation Rules

Before any write begins:

- the table must be fixed-scale
- the table must be writable
- each field must exist in the mapped schema
- every input column must have length `feature_count`
- all provided column lengths must agree
- string offsets must be 1D `uint32`, start at `0`, be monotonic, and end at `len(data)`
- string data must be a 1D contiguous `uint8` buffer

Error shape:

- unsupported table mode -> `RuntimeError`
- unknown field name -> existing field-resolution error style
- mismatched lengths -> `ValueError`
- invalid UTF-8 buffer contract -> `ValueError`

Length mismatch errors should include the field name, expected length, and actual length.

## Data Flow

For `tbl.fill(row_id=ids, x=xs, name=names)` on a fixed truncate table:

1. Python validates fields and lengths
2. numeric payloads become contiguous arrays if needed
3. string payloads become either:
   - direct `(offsets, data)` buffers, or
   - encoded UTF-8 buffers derived from Python strings
4. the bridge applies all numeric writes
5. the bridge applies all string writes
6. the engine publishes once
7. cached table handles are remapped once

The key invariant is that one user-visible `fill()` call corresponds to one coordinated native write batch and one publish/remap cycle.

## Testing

Add or update tests to cover:

- mixed numeric + `STR` `Table.fill(...)`
- string-only `Table.fill(...)`
- mismatched input lengths across columns
- a single field whose length differs from feature count
- unknown field names
- read-only loaded table rejecting `fill(...)`
- `StringColumn.fill()` and `fill_utf8()` using the unified bridge
- UTF-8 correctness for empty strings, ASCII, and multi-byte text

Benchmark updates:

- make the fixed truncate string benchmark use unified `tbl.fill(..., name=names)` as the primary path
- compare against the previous split-path implementation

## Success Criteria

- fixed-table `STR` writes no longer use `_rewrite_string_column(...)`
- `Table.fill(...)` can ingest mixed numeric and string columns in one call
- no partial updates occur on validation failure
- `column_trunc_str` becomes clearly faster than `column_push` in the existing benchmark

## Out of Scope Follow-up

If this lands cleanly, a later round can consider:

- a dedicated encoded-string public helper for advanced callers
- broader lifecycle cleanup between writable truncate state and read snapshots
- more aggressive performance work aimed at closing additional gap to Arrow
