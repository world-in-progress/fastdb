# ColumnEngine String Column Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add real Arrow-style UTF-8 string columns to `ColumnEngine` so `truncate()` supports `STR` with bulk write, bulk read, and backward-compatible runtime reads.

**Architecture:** Keep `layer_header_t` and `field_desc_ex_t` sizes unchanged. Introduce a new per-field variable-length string section (`offsets + utf8 bytes`) for new `STR` columns while preserving legacy string-table reads for old databases. On the Python side, expose strings as a dedicated `StringColumn` wrapper instead of pretending they are NumPy arrays.

**Tech Stack:** C++17 core, SWIG Python binding, Python 3.10+, NumPy, pytest, shared memory IPC

**Spec:** `docs/superpowers/specs/2026-04-21-columnengine-string-column-design.md`

**Branch:** `feature/unified-engine`

---

## File Structure Map

### New files to create

| File | Responsibility |
|---|---|
| `python/fastdb4py/string_column.py` | Python `StringColumn` wrapper for `get()`, `to_pylist()`, `fill()`, and `fill_utf8()` |
| `tests/python/test_string_column.py` | End-to-end tests for truncate + string bulk fill, mapped reads, legacy compatibility, and shared memory |

### Files to modify

| File | Responsibility |
|---|---|
| `fastcarto/fastdb/include/fastdb.h` | Public C++ declarations for string-view and string-column APIs |
| `fastcarto/fastdb/src/FastVectorDbLayerBuild_p.h` | Private builder structs for varlen string sections |
| `fastcarto/fastdb/src/FastVectorDbLayerBuild.cpp` | New string-column build path, bulk setter, and section writer |
| `fastcarto/fastdb/src/FastVectorDbLayer_p.h` | Reader-side caches for parsed string sections |
| `fastcarto/fastdb/src/FastVectorDbLayer.cpp` | Legacy/new STR dual-read logic and string-view accessors |
| `fastcarto/fastdb/swig/fastdb4py.i` | SWIG renames and Python helper exposure for string buffers |
| `python/fastdb4py/orm/table.py` | Column accessor dispatch that returns `StringColumn` for `STR` fields |
| `python/fastdb4py/column_engine.py` | Remove `STR` rejection in `truncate()` and wire new string columns |
| `python/fastdb4py/reader.py` | Read string fields from string views instead of only `const char*` |
| `python/fastdb4py/push.py` | Route `OriginFieldType.str` through string-view setter |
| `tests/python/benchmark_kostya_orm2.py` | Add string-aware truncate benchmark cases |
| `README.md` | Document `StringColumn` and `ColumnEngine.truncate()` string support |
| `CHANGELOG.md` | Add `fastdb4py` entry for real string column support |

---

## Phase 1: Tests First

### Task 1: Add failing end-to-end tests for string columns

**Files:**
- Create: `tests/python/test_string_column.py`

- [ ] **Step 1: Write failing tests for `truncate()` + `StringColumn`**

```python
# tests/python/test_string_column.py
import secrets
import numpy as np

from fastdb4py import ColumnEngine, Layout, feature, F64, U32, STR

@feature
class CEStringPoint:
    row_id: U32
    x: F64
    name: STR

def _pack_utf8(strings: list[str]):
    raw = bytearray()
    offsets = [0]
    for s in strings:
        raw.extend(s.encode("utf-8"))
        offsets.append(len(raw))
    return np.array(offsets, dtype=np.uint32), np.frombuffer(bytes(raw), dtype=np.uint8)

def test_column_engine_truncate_supports_str_fill_utf8():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 3)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(row_id=np.array([1, 2, 3], dtype=np.uint32), x=np.array([1.0, 2.0, 3.0], dtype=np.float64))
    offsets, data = _pack_utf8(["a", "be", "中"])
    tbl.column.name.fill_utf8(offsets, data)
    assert tbl.column.name.get(0) == "a"
    assert tbl.column.name.get(2) == "中"
    assert tbl[1].name == "be"
```

- [ ] **Step 2: Add failing legacy-compatibility and shared-memory tests**

```python
def test_column_engine_dynamic_push_still_reads_strings():
    engine = ColumnEngine.create()
    engine.push(CEStringPoint(row_id=1, x=1.5, name="legacy"))
    engine.combine()
    assert engine.table(CEStringPoint)[0].name == "legacy"

def test_column_engine_share_load_keeps_string_column():
    shm_name = f"fastdb_str_{secrets.token_hex(4)}"
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(row_id=np.array([10, 11], dtype=np.uint32), x=np.array([3.0, 4.0], dtype=np.float64))
    offsets, data = _pack_utf8(["alpha", "beta"])
    tbl.column.name.fill_utf8(offsets, data)
    engine.share(shm_name)
    loaded = ColumnEngine.load(shm_name)
    try:
        assert loaded.table(CEStringPoint).column.name.to_pylist() == ["alpha", "beta"]
    finally:
        loaded.unlink()
```

- [ ] **Step 3: Add a regression check that `Table.fill()` stays numeric-only**

```python
def test_table_fill_rejects_string_field_keyword():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 1)])
    tbl = engine.table(CEStringPoint)
    try:
        tbl.fill(name=np.array(["bad"], dtype=object))
    except Exception as exc:
        assert "StringColumn.fill" in str(exc)
    else:
        raise AssertionError("expected string fill rejection")
```

- [ ] **Step 4: Run tests to verify current failure**

Run: `uv run pytest tests/python/test_string_column.py -v`

Expected: FAIL with `ValueError` from `ColumnEngine.truncate()` rejecting `STR`.

- [ ] **Step 5: Commit**

```bash
git add tests/python/test_string_column.py
git commit -m "test: add failing string column integration tests"
```

---

## Phase 2: C++ Core Wire Format

### Task 2: Add public/private C++ API for string views and string sections

**Files:**
- Modify: `fastcarto/fastdb/include/fastdb.h`
- Modify: `fastcarto/fastdb/src/FastVectorDbLayerBuild_p.h`
- Modify: `fastcarto/fastdb/src/FastVectorDbLayer_p.h`

- [ ] **Step 1: Add public declarations in `fastdb.h`**

```cpp
// FastVectorDbLayerBuild
void setFieldStringView(unsigned ix, const char* data, unsigned len);
void setStringColumnBulk(unsigned field_id, const u32* offsets, unsigned n_offsets, const u8* data, u64 nbytes);

// FastVectorDbLayer
chunk_data_t getFieldAsStringView(u32 ix);
chunk_data_t getStringColumnOffsets(u32 ix);
chunk_data_t getStringColumnData(u32 ix);

// FastVectorDbFeature
chunk_data_t getFieldAsStringView(u32 ix);
```

- [ ] **Step 2: Add private builder-side storage in `FastVectorDbLayerBuild_p.h`**

```cpp
struct StringFieldBuildData {
    int         field_id;
    u32         codec;      // 1 = plain_utf8
    vector<u32> offsets;    // length = feature_count + 1
    vector<u8>  data;
};
vector<StringFieldBuildData> m_string_fields;
bool m_enable_varlen_string_columns;
```

- [ ] **Step 3: Add private reader-side caches in `FastVectorDbLayer_p.h`**

```cpp
struct StringFieldData {
    u32 field_id;
    u32 codec;
    u32 offset_count;
    u64 byte_count;
    u32* offsets_ptr;
    u8*  data_ptr;
};
vector<StringFieldData> m_string_fields;
```

- [ ] **Step 4: Rebuild to surface missing implementations**

Run: `./py_utils.sh --build`

Expected: build FAILS on missing method definitions declared above.

- [ ] **Step 5: Commit**

```bash
git add fastcarto/fastdb/include/fastdb.h fastcarto/fastdb/src/FastVectorDbLayerBuild_p.h fastcarto/fastdb/src/FastVectorDbLayer_p.h
git commit -m "refactor: declare string view and string column core APIs"
```

---

### Task 3: Implement C++ write path for plain UTF-8 string columns

**Files:**
- Modify: `fastcarto/fastdb/src/FastVectorDbLayerBuild.cpp`

- [ ] **Step 1: Implement `setFieldStringView()` and bulk setter**

```cpp
void FastVectorDbLayerBuild::Impl::setFieldStringView(unsigned ix, const char* data, u32 len) {
    if (ix >= m_field_descs.size()) return;
    auto& fd = m_field_descs[ix];
    if (fd.type != ftSTR) return;
    auto* sfd = find_string_field(ix);
    if (sfd == nullptr) return;
    const u8* p = reinterpret_cast<const u8*>(data);
    sfd->data.insert(sfd->data.end(), p, p + len);
    sfd->offsets.push_back((u32)sfd->data.size());
}

void FastVectorDbLayerBuild::Impl::setStringColumnBulk(unsigned field_id, const u32* offsets, u32 n_offsets, const u8* data, u64 nbytes) {
    auto* sfd = find_string_field(field_id);
    if (sfd == nullptr) return;
    sfd->offsets.assign(offsets, offsets + n_offsets);
    sfd->data.assign(data, data + nbytes);
}
```

- [ ] **Step 2: Register new-style `STR` fields with `size = 0` for truncate-backed layers**

```cpp
if (ft == ftSTR && m_enable_varlen_string_columns) {
    fd.size = 0;
    m_string_fields.push_back(StringFieldBuildData{
        (int)(m_field_descs.size() - 1), 1, {0}, {}
    });
}
```

- [ ] **Step 3: Write string field sections after list sections**

```cpp
for (const auto& sfd : m_string_fields) {
    stream->write((void*)&sfd.field_id, sizeof(u32));
    stream->write((void*)&sfd.codec, sizeof(u32));
    u32 offset_count = (u32)sfd.offsets.size();
    u64 byte_count = (u64)sfd.data.size();
    stream->write(&offset_count, sizeof(offset_count));
    stream->write(&byte_count, sizeof(byte_count));
    stream->write((void*)sfd.offsets.data(), offset_count * sizeof(u32));
    if (!sfd.data.empty()) stream->write((void*)sfd.data.data(), sfd.data.size());
}
```

- [ ] **Step 4: Rebuild native code**

Run: `./py_utils.sh --build`

Expected: build FAILS later in the reader/SWIG layers, but writer-side compile errors are gone.

- [ ] **Step 5: Commit**

```bash
git add fastcarto/fastdb/src/FastVectorDbLayerBuild.cpp
git commit -m "feat: add UTF-8 string column write path"
```

---

### Task 4: Implement C++ read path and expose string buffers through SWIG

**Files:**
- Modify: `fastcarto/fastdb/src/FastVectorDbLayer.cpp`
- Modify: `fastcarto/fastdb/swig/fastdb4py.i`

- [ ] **Step 1: Parse new string sections and add dual-read logic**

```cpp
chunk_data_t FastVectorDbLayer::Impl::getFieldAsStringView_internal(u32 ifeature, u32 ix) {
    const field_desc_ex_t* fd = m_field_descs + ix;
    if (fd->type != ftSTR) return {0, nullptr};
    if (fd->size == 0) {
        auto* sfd = find_string_field(ix);
        if (sfd == nullptr || ifeature + 1 >= sfd->offset_count) return {0, nullptr};
        u32 start = sfd->offsets_ptr[ifeature];
        u32 end = sfd->offsets_ptr[ifeature + 1];
        return {(size_t)(end - start), sfd->data_ptr + start};
    }
    const char* text = getFieldAsString_internal(ifeature, ix);
    return text ? chunk_data_t{strlen(text), (u8*)text} : chunk_data_t{0, nullptr};
}
```

- [ ] **Step 2: Expose new APIs in `fastdb4py.i`**

```swig
%rename(get_field_as_string_view) getFieldAsStringView;
%rename(get_string_column_offsets) getStringColumnOffsets;
%rename(get_string_column_data) getStringColumnData;
%rename(set_field_string_view) setFieldStringView;
%rename(set_string_column_bulk) setStringColumnBulk;
```

- [ ] **Step 3: Add Python helper methods for chunk-backed buffers**

```python
def get_string_column_offsets(self, index):
    return self.getStringColumnOffsets(index).as_array(np.uint32)

def get_string_column_data(self, index):
    return self.getStringColumnData(index).as_array(np.uint8)
```

- [ ] **Step 4: Rebuild and rerun the failing string test**

Run:

1. `./py_utils.sh --build`
2. `uv run pytest tests/python/test_string_column.py::test_column_engine_truncate_supports_str_fill_utf8 -v`

Expected: test still FAILS, but now on missing Python `StringColumn` integration rather than native `STR` rejection.

- [ ] **Step 5: Commit**

```bash
git add fastcarto/fastdb/src/FastVectorDbLayer.cpp fastcarto/fastdb/swig/fastdb4py.i
git commit -m "feat: expose UTF-8 string column read APIs"
```

---

## Phase 3: Python Integration

### Task 5: Add `StringColumn` and integrate it into `Table`, `ColumnEngine`, `reader`, and `push`

**Files:**
- Create: `python/fastdb4py/string_column.py`
- Modify: `python/fastdb4py/orm/table.py`
- Modify: `python/fastdb4py/column_engine.py`
- Modify: `python/fastdb4py/reader.py`
- Modify: `python/fastdb4py/push.py`

- [ ] **Step 1: Create `python/fastdb4py/string_column.py`**

```python
from __future__ import annotations
import numpy as np

class StringColumn:
    def __init__(self, table_origin, field_index: int):
        self._table_origin = table_origin
        self._field_index = field_index

    def _offsets(self) -> np.ndarray:
        return self._table_origin.get_string_column_offsets(self._field_index)

    def _data(self) -> np.ndarray:
        return self._table_origin.get_string_column_data(self._field_index)

    def get(self, index: int) -> str:
        offsets = self._offsets()
        data = self._data()
        start = int(offsets[index])
        end = int(offsets[index + 1])
        return bytes(data[start:end]).decode("utf-8")

    def to_pylist(self) -> list[str]:
        return [self.get(i) for i in range(len(self._offsets()) - 1)]

    def fill(self, strings: list[str]) -> None:
        raw = bytearray()
        offsets = [0]
        for s in strings:
            raw.extend(s.encode("utf-8"))
            offsets.append(len(raw))
        self.fill_utf8(
            np.array(offsets, dtype=np.uint32),
            np.frombuffer(bytes(raw), dtype=np.uint8),
        )

    def fill_utf8(self, offsets: np.ndarray, data: np.ndarray) -> None:
        self._table_origin.set_string_column_bulk(
            self._field_index, offsets, data
        )
```

- [ ] **Step 2: Return `StringColumn` from `orm/table.py` for `STR` fields**

```python
from ..string_column import StringColumn
from ..type import OriginFieldType

fd = schema.fields[idx]
if fd.field_type == OriginFieldType.str:
    arr = StringColumn(table_origin, idx)
else:
    arr = table_origin.get_column(idx).as_nparray()
```

- [ ] **Step 3: Allow `truncate()` to accept `STR`, but still reject `WSTR`/`BYTES`**

```python
if fd.field_type in (OriginFieldType.bytes, OriginFieldType.wstr):
    raise ValueError(
        f'Truncate still does not support field "{fd.name}" of type "{fd.field_type.name}".'
    )
```

- [ ] **Step 4: Move single-value reads and push writes onto the string-view path**

```python
# reader.py
if fd.field_type == OriginFieldType.str:
    raw = feat.get_field_as_string_view(fd.field_id)
    return raw.to_bytes().decode("utf-8")

# push.py
elif ft == OriginFieldType.str:
    encoded = (str(value) if value is not None else "").encode("utf-8")
    layer_build.set_field_string_view(fid, encoded, len(encoded))
```

- [ ] **Step 5: Run targeted tests**

Run:

1. `./py_utils.sh --build`
2. `uv run pytest tests/python/test_string_column.py tests/python/test_column_engine.py -v`

Expected: PASS for new string-column tests and existing column engine tests.

- [ ] **Step 6: Commit**

```bash
git add python/fastdb4py/string_column.py python/fastdb4py/orm/table.py python/fastdb4py/column_engine.py python/fastdb4py/reader.py python/fastdb4py/push.py tests/python/test_string_column.py
git commit -m "feat: add Python StringColumn integration"
```

---

## Phase 4: Compatibility, Benchmarks, and Docs

### Task 6: Add compatibility coverage, benchmark strings, and document the new API

**Files:**
- Modify: `tests/python/test_string_column.py`
- Modify: `tests/python/benchmark_kostya_orm2.py`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Extend tests for duplicate-heavy strings and explicit `fill(strings)`**

```python
def test_string_column_fill_python_strings():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 3)])
    tbl = engine.table(CEStringPoint)
    tbl.column.name.fill(["same", "same", "diff"])
    assert tbl.column.name.to_pylist() == ["same", "same", "diff"]
```

- [ ] **Step 2: Add benchmark cases for unique-heavy and duplicate-heavy strings**

```python
@feature
class CoordWithName:
    row_id: U32
    x: F64
    y: F64
    z: F64
    name: STR

def _pack_utf8(strings: list[str]):
    raw = bytearray()
    offsets = [0]
    for s in strings:
        raw.extend(s.encode("utf-8"))
        offsets.append(len(raw))
    return np.array(offsets, dtype=np.uint32), np.frombuffer(bytes(raw), dtype=np.uint8)

def bench_column_truncate_str(N: int, reps: int, unique_mod: int) -> dict:
    ids = np.arange(N, dtype=np.uint32)
    xs = np.arange(N, dtype=np.float64) * 0.1
    ys = np.arange(N, dtype=np.float64) * 0.2
    zs = np.arange(N, dtype=np.float64) * 0.3
    names = [f"name_{i % unique_mod}" for i in range(N)]
    offsets, data = _pack_utf8(names)
    orm = ColumnEngine.truncate([Layout(CoordWithName, N)])
    tbl = orm.table(CoordWithName)
    tbl.fill(row_id=ids, x=xs, y=ys, z=zs)
    tbl.column.name.fill_utf8(offsets, data)
```

- [ ] **Step 3: Document the new API in `README.md`**

```python
from fastdb4py import ColumnEngine, Layout, feature, F64, STR
import numpy as np

@feature
class Point:
    x: F64
    name: STR

orm = ColumnEngine.truncate([Layout(Point, 3)])
tbl = orm.table(Point)
tbl.fill(x=np.array([1.0, 2.0, 3.0], dtype=np.float64))
tbl.column.name.fill(["a", "be", "see"])
assert tbl.column.name.get(1) == "be"
```

- [ ] **Step 4: Add changelog entry**

```markdown
<!-- BEGIN:fastdb4py -->
## fastdb4py (Python binding)

### Added
- Added Arrow-style UTF-8 string columns for `ColumnEngine.truncate()`, including `StringColumn.fill()` / `fill_utf8()` bulk APIs and backward-compatible runtime reads for legacy string-table databases.
<!-- END:fastdb4py -->
```

- [ ] **Step 5: Run verification**

Run:

1. `./py_utils.sh --build`
2. `uv run pytest tests/python/test_string_column.py tests/python/test_column_engine.py tests/python/test_shared_memory.py -v`
3. `uv run pytest tests/python -q`
4. `uv run python tests/python/benchmark_kostya_orm2.py --quick`

Expected:

- build succeeds
- targeted tests PASS
- full Python test suite PASS
- benchmark prints both numeric-only and string-aware truncate results

- [ ] **Step 6: Commit**

```bash
git add tests/python/test_string_column.py tests/python/benchmark_kostya_orm2.py README.md CHANGELOG.md
git commit -m "docs: document string column support and add benchmarks"
```

---

## Self-Review Checklist

- Spec coverage: tasks cover wire format, C++ APIs, Python `StringColumn`, compatibility, benchmarks, and docs.
- Placeholder scan: no `TODO`/`TBD`/“similar to above” shortcuts remain.
- Type consistency: `StringColumn`, `getFieldAsStringView`, `getStringColumnOffsets`, `getStringColumnData`, `setFieldStringView`, and `setStringColumnBulk` are named consistently across tasks.
