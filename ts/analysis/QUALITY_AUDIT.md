# fastdb4ts Quality Audit

Audit performed after v0.0.1 release. Tracks identified issues, their severity, the fix applied, and test coverage.

---

## Alignment / Memory Layout

**Question**: Does JavaScript's 4-byte TypedArray alignment requirement cause hidden bugs in cross-language data exchange?

**Answer: No hidden bug with current code.**

- `TypedArray` views (e.g. `new Uint32Array(buffer, offset)`) require `offset` to be a multiple of the element size, otherwise they throw a `RangeError`.
- However, `fastdb4ts` uses **DataView** exclusively for all multi-byte reads and writes in `column.ts` (StridedColumn) and `serializer.ts` (ByteReader/ByteWriter). `DataView` supports arbitrary byte offsets with no alignment restriction.
- Column data is accessed via `DataView(HEAPU8.buffer)` with an absolute WASM linear-memory address computed from `getAddress() + getFieldOffset()`. WASM linear addresses remain stable across WASM memory growth; only the JS typed array wrapping changes, which is handled by re-fetching `module.HEAPU8.buffer` on each call.
- `_malloc` / heap allocations are always word-aligned.
- The serializer binary format uses `struct.pack('<I')` (Python) and `setUint32(..., true)` (TypeScript) — both explicitly little-endian, no alignment dependency.

**Future risk**: If StridedColumn is ever changed to use TypedArray views instead of DataView for performance, column byte offsets must be verified to be aligned to their element size.

---

## Issues

### P0 — Critical

| ID | Issue | Status | Fix |
|----|-------|--------|-----|
| P0-bounds | ByteReader/ByteWriter no bounds checking | ✅ Fixed | `checkRead(n)` throws `FastdbRuntimeError` before each read |
| P0-str-write | `writeMappedField` throws for str/wstr/bytes instead of falling back to cache | ✅ Fixed | Falls back to `feature._getCache()[name] = value` (in-memory only) |

**P0-bounds detail**: Before the fix, reading past the end of a truncated blob would silently return garbage (DataView fills with 0 for out-of-bounds) or throw a generic `RangeError`. After the fix, a clear `FastdbRuntimeError` is thrown with offset context.

**P0-str-write detail**: `WxFeatureHandle` does not expose `setFieldString` (string fields cannot be modified on an immutable db). The correct behaviour for writes to str/wstr/bytes db-mapped fields is to write to the in-memory cache only — consistent with how `ref` fields already behave. This means the change is not persisted to the WASM database, but does not crash.

---

### P1 — High

| ID | Issue | Status | Fix |
|----|-------|--------|-----|
| P1-wstr-read | WSTR reads use `getFieldAsString` (same as STR) | ✅ Fixed | Embind now exposes `getFieldAsWString` / `setFieldWString`; mapped features, object-graph rows, and C-Two call-db retained table columns have WSTR coverage. |
| P1-close | No `ORM.close()` for WASM resource cleanup | ✅ Fixed | `ORM.close()` deletes the database origin; idempotent |
| P1-heapu8 | StridedColumn.basePtr could appear stale after WASM memory growth | ✅ No action needed | `basePtr` is a WASM linear-memory address (index), not a JS pointer. It remains valid after growth. `getDataView()` re-fetches `HEAPU8.buffer` each call, so always uses the current heap buffer. Added explanatory comment. |

---

### P2 — Medium

| ID | Issue | Status | Fix |
|----|-------|--------|-----|
| P2-dispatch | Scalar field dispatch logic duplicated across feature.ts, orm.ts, serializer.ts | ⏳ Pending | Extract to shared helper in types.ts or fieldDispatch.ts |
| P2-init-error | ORM.create() / truncate() give unfriendly error if called before initFastdb() | ✅ Fixed | Error message updated to mention `await initFastdb()` |

---

## Test Coverage

| Test file | What it covers |
|-----------|----------------|
| `tests/ts/test_bounds.mjs` | ByteReader throws on truncated blob; ByteWriter roundtrip; various read sizes at boundary |
| `tests/ts/test_feature_write.mjs` | str/wstr write to db-mapped feature falls back to cache; close() is idempotent; ref write stores in cache |
| `tests/ts/test_column_way.mjs` | (existing) StridedColumn get/set/fill, buffer roundtrip |
| `tests/ts/test_fast_serializer.mjs` | (existing) FastSerializer full roundtrip, cyclic refs, numeric lists |
| `tests/ts/test_c_two_runtime.mjs` | C-Two call-db runtime WSTR/BYTES encode/decode and retained table-column access |
