# ColumnEngine String Column Design

## Problem

`ColumnEngine.truncate()` rejects `STR`, `WSTR`, and `BYTES`, even though dynamic push already supports `STR`. The current C++ layout stores string-table IDs in fixed-width row slots, which works for per-row writes but does not provide a real columnar string representation or a good bulk API.

This blocks the main fast path for mixed numeric + string schemas and makes future bindings awkward. TypeScript, Go, and Fortran would all prefer a language-neutral variable-length column layout over Python-specific row-wise string setters.

## Goal

Add real columnar `STR` support for `ColumnEngine`, modeled after Arrow-style variable-length UTF-8 columns:

- `truncate()` supports `STR`
- bulk write path exists for strings
- bulk read path exists for strings
- wire format remains backward-readable
- phase 1 focuses on plain UTF-8 columns, not dictionary encoding

## Approach

Represent each `STR` field as a variable-length field section containing:

- `offsets[N+1]`
- `utf8_bytes[...]`

Strings use strict Arrow-style encoding:

- UTF-8 payload only
- no trailing `\0`
- offsets define boundaries

This makes the true low-level interface a string view / string column buffer API. `getFieldAsString()` remains only as a convenience compatibility wrapper.

## Wire Format

### Invariants

- Do **not** change `layer_header_t` size
- Do **not** change `field_desc_ex_t` size
- Keep old databases readable

### Field descriptor split

Two `STR` encodings coexist:

| Encoding | `field_desc.type` | `field_desc.size` | Storage |
|---|---:|---:|---|
| Legacy | `ftSTR` | `2` or `4` | row slot stores string-table ID |
| New | `ftSTR` | `0` | per-field variable-length section |

New runtime code must support both.

### Variable-length section format

Existing tail-section infrastructure currently handles list columns. In the new design it becomes a general variable-length field section parser keyed by field type.

For phase 1 `STR`, the section payload is:

1. `field_id: u32`
2. `codec: u32`
3. `offset_count: u32` (`N + 1`)
4. `byte_count: u64`
5. `offsets[offset_count]: u32`
6. `utf8_bytes[byte_count]: u8`

Codec values:

- `1 = plain_utf8`
- `2 = dictionary_utf8` (reserved for future work)

Reader interpretation is driven by `field_desc.type` plus `codec`, not by adding a larger header.

## C++ Core Changes

### Reader

Add low-level string APIs to `FastVectorDbLayer` / `FastVectorDbFeature`:

- `chunk_data_t getFieldAsStringView(u32 ix)`
- `chunk_data_t getStringColumnOffsets(u32 ix)`
- `chunk_data_t getStringColumnData(u32 ix)`

Behavior:

- legacy `STR` returns a view synthesized from the string table entry
- new `STR` returns a view into `utf8_bytes[offsets[row]:offsets[row+1]]`

`getFieldAsString(u32 ix)` stays available but becomes a convenience wrapper over the view path rather than the primary storage contract.

### Builder

Add low-level write APIs to `FastVectorDbLayerBuild`:

- `setFieldStringView(ix, const char* data, u32 len)`
- `setStringColumnBulk(field_id, const u32* offsets, u32 n_offsets, const u8* data, u64 nbytes)`

Keep `setField(ix, const char*)` as a compatibility wrapper that computes `strlen()` and delegates to `setFieldStringView()`.

Internally, phase 1 needs a new per-field string build buffer storing:

- appended offsets
- appended UTF-8 bytes

`truncate(nfeatures)` must initialize fixed row count and allow later population of string offsets/data without requiring string-table pre-estimation.

## Python Binding Changes

### Read side

Numeric columns continue to return NumPy arrays. String columns must no longer pretend to be NumPy.

`tbl.column.name` should return a `StringColumn` object with:

- `get(i) -> str`
- `to_pylist() -> list[str]`
- `fill(strings)`
- `fill_utf8(offsets, data)`

`fill_utf8()` is the real bulk ABI. `fill(strings)` is a convenience layer that packs Python strings into offsets + UTF-8 bytes.

### Write side

`ColumnEngine.truncate()` should allow `STR` in phase 1.

`WSTR` and `BYTES` remain unsupported in phase 1.

`Table.fill(...)` remains numeric-only for clarity. String bulk writes go through `StringColumn.fill()` or `StringColumn.fill_utf8()`.

### Single-value reads

`reader.py` should move single-value string reads onto the string-view path. The existing Python-facing semantics remain:

- `None` written into a string field becomes `""`
- reading a string field yields Python `str`

## Compatibility Strategy

### Read compatibility

New runtime reads:

- old string-table databases
- new UTF-8 column databases

This is required before any default writer behavior changes.

### Write compatibility

Phase 1 writer rollout should be scoped:

1. enable new encoding first for `ColumnEngine.truncate()`
2. keep other writers conservative until validation is complete

This limits blast radius while unlocking the most important bulk path.

## Performance Expectations

### Expected wins

- `truncate + fill_utf8(offsets, data)` becomes two bulk copies
- string column reads become binding-friendly and bulk-oriented
- cross-language bindings can consume offsets + bytes directly

### Expected tradeoffs

- repeated strings may use more space than legacy string-table encoding
- single-value `feature.name` reads become decode-from-slice operations

This is acceptable because the design prioritizes:

1. full expression of string columns
2. strong truncate/bulk performance
3. clean cross-language ABI

Space compression for repeated strings is deferred to dictionary encoding.

## Scope Boundaries

### Phase 1

- plain UTF-8 string columns
- backward-compatible reader
- `ColumnEngine.truncate()` accepts `STR`
- Python `StringColumn`
- benchmarks for unique-heavy and duplicate-heavy strings

### Not in phase 1

- nullable strings
- dictionary encoding
- `WSTR`
- `BYTES`
- NumPy object-array string columns

## Testing

Required coverage:

- empty string
- UTF-8 multibyte strings
- large strings
- mixed numeric + string schemas
- old DB read compatibility
- shared-memory load path
- truncate + `fill_utf8()`
- duplicate-heavy vs unique-heavy benchmark cases

## Recommended Implementation Order

1. C++ wire format reader/writer support for new `STR`
2. SWIG exposure of string view / column buffers
3. Python `StringColumn`
4. `ColumnEngine.truncate()` support for `STR`
5. benchmark and interop validation
6. evaluate whether dictionary encoding is worth phase 2
