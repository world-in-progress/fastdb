# fastdb4py

Python bindings for `fastdb`, built on top of the C++ core in `fastcarto/fastdb/` and exposed through SWIG.

This README is the binding-specific companion to the repository root `README.md`. The root document introduces the project as a whole; this document focuses on the Python-facing API, its architecture, and common usage patterns.

## What `fastdb4py` provides

`fastdb4py` is designed as a high-performance local data layer for Python applications that need compact binary storage, efficient numerical access, and typed object graphs.

Its main strengths are:

- **Zero-copy columnar access**
  - numeric columns can be exposed directly as NumPy arrays backed by native memory
- **Typed object graphs**
  - `Feature` objects can reference other `Feature` objects across tables
- **Shared-memory transport**
  - databases can be published to shared memory and consumed in other Python processes
- **Compact binary persistence**
  - databases can be saved to files or serialized to bytes
- **Graph serialization**
  - `FastSerializer` supports nested features, cyclic references, and heterogeneous list payloads

## Binding architecture

The Python stack is layered:

1. **C++ core** — `fastcarto/fastdb/`
   - owns the binary format, storage layout, geometry encoding, string tables, and reference model
2. **SWIG/native bridge** — `python/fastdb4py/core/`
   - generated wrappers and compiled native extension
3. **High-level Python API** — `python/fastdb4py/`
   - ergonomic `@feature`, `ColumnEngine`, `ObjectEngine`, `Table`, and `FastSerializer` abstractions

Important directories:

- `python/fastdb4py/type.py`
  - field types such as `U32`, `F64`, `STR`, `BYTES`
- `python/fastdb4py/feature/`
  - schema discovery, feature dispatch, caching, runtime access
- `python/fastdb4py/orm/`
  - shared `Table` class, column access, and iteration helpers used by both engines
- `python/fastdb4py/serializer.py`
  - graph serialization on top of `fastdb`
- `python/fastdb4py/core/`
  - generated binding layer; do not edit manually

## Installation

From PyPI:

```bash
pip install fastdb4py
```

From the repository root during development:

```bash
./py_utils.sh --setup
```

Prebuilt wheels are expected for the main supported platforms. Source builds require a C++17 compiler, CMake, SWIG, and NumPy.

## Quick start

```python
from fastdb4py import feature, ColumnEngine, Layout, F64
import numpy as np


@feature
class Point:
    x: F64
    y: F64
    z: F64


db = ColumnEngine.truncate([Layout(Point, 5)])
table = db.table(Point)

table.fill(
    x=np.linspace(0.0, 1.0, 5),
    y=np.zeros(5),
    z=np.ones(5),
)

print(table[2].x)
print(table.column.x.mean())
```

## Field types

`fastdb4py` uses binding-specific field aliases rather than plain Python primitives.

| Python alias | Native storage | Description |
|--------------|----------------|-------------|
| `fx.U8` | `uint8_t` | Unsigned 8-bit integer |
| `fx.U16` | `uint16_t` | Unsigned 16-bit integer |
| `fx.U32` | `uint32_t` | Unsigned 32-bit integer |
| `fx.I32` | `int32_t` | Signed 32-bit integer |
| `fx.F32` | `float` | 32-bit float |
| `fx.F64` | `double` | 64-bit float |
| `fx.STR` | UTF-8 string storage | String column / per-row string value |
| `fx.WSTR` | wide string table index | Legacy wide string |
| `fx.BYTES` | blob / raw geometry payload | Byte storage |
| `fx.U8N` | normalized `uint8_t` | Quantized float in a configured range |
| `fx.U16N` | normalized `uint16_t` | Quantized float in a configured range |
| `OtherFeature` | feature reference | Typed cross-table reference |

## Defining schemas with `Feature`

Users model rows by decorating plain classes with `@feature` and annotating fields:

```python
from fastdb4py import feature, U32, F64, F32


@feature
class Particle:
    id: U32
    x: F64
    y: F64
    mass: F32
```

The field order is part of the schema contract. It affects table layout, serializer traversal order, and binary compatibility.

## Database creation patterns

### Fixed-size tables with `ColumnEngine.truncate`

Use `truncate` when the row count is known ahead of time. For fixed-size tables, there are two UTF-8 string-ingest tiers:

- **Default high-level path** — `tbl.fill(..., name=[...])` now routes raw strings through the native batch string-column API
- **Advanced prepacked path** — `pack_utf8_column([...]) + tbl.column.name.fill_utf8(...)`

If your pipeline starts from Python `str` values, prefer the default raw path. Reach for the prepacked path only when you already have UTF-8 offsets/data buffers from an upstream step.

```python
from fastdb4py import feature, ColumnEngine, Layout, F64, F32
import numpy as np


@feature
class Particle:
    x: F64
    y: F64
    vx: F64
    vy: F64
    mass: F32


N = 100_000
db = ColumnEngine.truncate([Layout(Particle, N)])
tbl = db.table(Particle)

tbl.fill(
    x=np.random.uniform(-1.0, 1.0, N),
    y=np.random.uniform(-1.0, 1.0, N),
    vx=np.zeros(N),
    vy=np.zeros(N),
    mass=np.ones(N, dtype=np.float32),
)
```

Multiple tables can be created in one call:

```python
from fastdb4py import feature, Layout, U32, F64


@feature
class Cell:
    id: U32
    temperature: F64


db = ColumnEngine.truncate([
    Layout(Particle, 50_000),
    Layout(Cell, 1_000),
])
```

For fixed-size tables with string columns, the default batch-ingest API is still `Table.fill(...)`. It batches numeric data and raw Python `STR` values together, and the raw-string path now routes through the native batch string-column API with upfront length validation:

```python
from fastdb4py import feature, ColumnEngine, Layout, U32, F64, STR, pack_utf8_column
import numpy as np


@feature
class Sample:
    row_id: U32
    value: F64
    name: STR


db = ColumnEngine.truncate([Layout(Sample, 3)])
tbl = db.table(Sample)
tbl.fill(
    row_id=np.array([1, 2, 3], dtype=np.uint32),
    value=np.array([0.5, 1.5, 2.5], dtype=np.float64),
    name=["a", "be", "中"],
)

# If your pipeline already produced UTF-8 offsets/data buffers, use the
# advanced prepacked path directly instead of re-encoding Python strings:
offsets_u32, utf8_bytes_u8 = pack_utf8_column(["a", "be", "中"])
tbl.column.name.fill_utf8(offsets_u32, utf8_bytes_u8)
```

### Dynamic tables with `ObjectEngine.create` + `push`

Use `ObjectEngine` when the final row count is not known in advance or when your schema includes REF fields / graph structure.

```python
from fastdb4py import feature, ObjectEngine, U8, U32, STR


@feature
class LogEntry:
    level: U8
    code: U32
    message: STR


db = ObjectEngine.create()
db.push(LogEntry(level=1, code=200, message="ok"))
db.push(LogEntry(level=3, code=500, message="internal error"))

db.combine()
```

## Reading data

### Columnar access

The fastest read path in Python is columnar access. Numeric fields return NumPy arrays directly backed by the native storage. UTF-8 string fields return a `StringColumn` wrapper.

```python
xs = tbl.column.x
ys = tbl.column.y

xs += 0.01 * tbl.column.vx
ys += 0.01 * tbl.column.vy

print(xs.mean())
```

```python
names = tbl.column.name
print(names.get(0))
print(names.to_pylist())
```

### Row access

```python
first = tbl[0]
print(first.x, first.y)
```

### Iteration

```python
for feat in tbl:
    print(feat.x, feat.y)

for feat in tbl.iter_reuse():
    print(feat.x, feat.y)
```

`iter_reuse()` is the high-performance iterator. It reuses the same wrapper object and should be preferred in tight loops.

## Feature references

Reference fields let one feature point at another feature, possibly in a different table. This is handled by `ObjectEngine`, not `ColumnEngine`.

```python
from fastdb4py import feature, ObjectEngine, F64


@feature
class Point:
    x: F64
    y: F64
    z: F64


@feature
class Triangle:
    a: Point
    b: Point
    c: Point


db = ObjectEngine.create()
p0 = Point(); p0.x = 0.0; p0.y = 0.0; p0.z = 0.0
p1 = Point(); p1.x = 1.0; p1.y = 0.5; p1.z = 0.0
p2 = Point(); p2.x = 2.0; p2.y = 1.0; p2.z = 0.0
tri = Triangle(); tri.a = p0; tri.b = p1; tri.c = p2

db.push(tri)
db.combine()
loaded = db.get(Triangle, 0, mode="copy")

print(loaded.a.x, loaded.b.x, loaded.c.x)
```

## File persistence

```python
db.save("simulation_state")

db2 = ColumnEngine.load("simulation_state", from_file=True)
tbl2 = db2.table(Particle)
print(tbl2.column.x[:5])
```

## Shared-memory IPC

Shared memory is available in the Python binding even though it is intentionally absent from the current TypeScript binding.

Publisher:

```python
from fastdb4py import feature, ObjectEngine, F64


@feature
class Signal:
    t: F64
    value: F64


db = ObjectEngine.create()
db.push(Signal(t=0.0, value=3.14))
db.push(Signal(t=0.1, value=2.71))
db.combine()
db.share("my_signals")
```

Reader:

```python
from fastdb4py import feature, ObjectEngine, F64


@feature
class Signal:
    t: F64
    value: F64


db = ObjectEngine.load("my_signals")
tbl = db.table(Signal)
for row in tbl:
    print(row.t, row.value)
db.unlink()
```

## Batch scalar field access

For db-mapped features, scalar fields can be read or written in bulk to reduce per-field bridge overhead.

```python
import numpy as np
from fastdb4py import feature, I32, STR, F32, F64


@feature
class Particle:
    index: I32
    name: STR
    mass: F32
    x: F64
    y: F64


feat = tbl[0]
out = np.empty(4, dtype=np.float64)
feat.read_all_scalars(out)
feat.write_all_scalars(np.array([5.0, 1.5, 3.14, 2.71]))
feat.name = "electron"
```

Covered scalar kinds are `U8`, `U16`, `U32`, `I32`, `F32`, `F64`, `U8N`, and `U16N`.

## FastSerializer

`FastSerializer` serializes an object graph rooted at a `Feature`. It is useful when the data is graph-shaped rather than a single flat numeric table.

Supported scenarios include:

- nested `Feature` lists
- scalar lists
- string lists
- typed references
- cyclic graphs
- **buffer-protocol fast paths** — numpy `ndarray` fields and numeric lists (`List[F64]`, `List[U32]`, `List[I32]`) are stored in dedicated `__fastser_buf__` layers via `memcpy`-level writes

Example:

```python
from typing import List
from fastdb4py import FastSerializer, feature, I32, F64, STR


@feature
class Point:
    x: F64
    y: F64


@feature
class Line:
    id: I32
    label: STR
    points: List[Point]


p1 = Point(x=0.0, y=0.0)
p2 = Point(x=1.0, y=1.0)
line = Line(id=42, label="edge", points=[p1, p2])

blob = FastSerializer.dumps(line)
copy = FastSerializer.loads(blob, Line)
print(copy.label, copy.points[1].x)
```

Cyclic identity is preserved:

```python
@feature
class Node:
    val: I32
    next: "Node"


n1 = Node(val=1)
n2 = Node(val=2)
n1.next = n2
n2.next = n1

check = FastSerializer.loads(FastSerializer.dumps(n1), Node)
assert check.next.next is check
```

### Buffer-protocol optimized fields

Numpy `ndarray` fields and simple numeric lists are stored via dedicated columnar `__fastser_buf__` layers, achieving `memcpy`-level serialization:

```python
import numpy as np
from typing import List
from fastdb4py import FastSerializer, feature, F64, U32


@feature
class PointCloud:
    coords: np.ndarray   # stored as buffer layer (1 SWIG call, memcpy)
    weights: List[F64]   # also stored as buffer layer
    ids: List[U32]       # also stored as buffer layer


cloud = PointCloud(
    coords=np.random.rand(10000, 3),
    weights=list(np.random.rand(10000)),
    ids=list(range(10000)),
)

blob = FastSerializer.dumps(cloud)
loaded = FastSerializer.loads(blob, PointCloud)
# loaded.coords → numpy ndarray
# loaded.weights → numpy ndarray (not Python list)
# loaded.ids → numpy ndarray (not Python list)
```

> **Note**: Numeric lists (`List[F64]`, `List[U32]`, `List[I32]`) deserialized from buffer layers return `numpy.ndarray` instead of Python `list`. Use `.tolist()` if a Python list is needed.

### Shared memory deserialization

`FastSerializer.loads_shm` deserializes directly from a POSIX shared memory segment, avoiding an intermediate `bytes` copy:

```python
from multiprocessing import shared_memory
from fastdb4py import FastSerializer, feature, F64


@feature
class Point:
    x: F64
    y: F64

# Write serialized data into shared memory
blob = FastSerializer.dumps(Point(x=1.0, y=2.0))
shm = shared_memory.SharedMemory(name="my_data", create=True, size=len(blob))
shm.buf[:len(blob)] = blob

# In another process: read directly from shared memory
result = FastSerializer.loads_shm("my_data", length=len(blob), offset=0, root_type=Point)
print(result.x, result.y)  # 1.0 2.0

# Clean up
shm.close()
shm.unlink()
```

All returned objects are fully detached from the shared memory segment (pure Python `_cache` mode). Numpy arrays are copied. The shared memory is closed immediately after deserialization.

For large homogeneous numerical datasets, `ColumnEngine.truncate` plus columnar writes is still the preferred path. `FastSerializer` is aimed at trees, graphs, mesh-like structures, and mixed payloads.

## Running tests

From the repository root:

```bash
uv run pytest tests/python
```

Focused runs:

```bash
uv run pytest tests/python/test_column_way.py
uv run pytest tests/python/test_fast_serializer.py
uv run pytest tests/python/test_fastser_buffer_layers.py
uv run pytest tests/python/test_fastser_loads_shm.py
uv run pytest tests/python/test_codegen.py
```

## CLI tools

`fastdb4py` registers a `fdb` command-line tool through `[project.scripts]`.

### `fdb codegen --ts` — Generate TypeScript Feature classes

When working with both `fastdb4py` (Python) and `fastdb4ts` (TypeScript), you can use `fdb codegen` to automatically generate TypeScript `Feature` classes from your Python definitions. Python `@feature` classes serve as the single source of truth — similar to how `.proto` files work in Protocol Buffers, but without an intermediate format.

```bash
fdb codegen --ts ./features/ ./ts-features/
```

The tool:

1. **Scans** all `.py` files in the input directory recursively
2. **Discovers** all `@feature` classes (ignoring non-feature code)
3. **Analyzes** dependencies, detects cycles, and topologically sorts
4. **Generates** one `.ts` file per `.py` file, preserving the directory structure

#### Type mapping

| Python | TypeScript schema | TypeScript type |
|--------|------------------|-----------------|
| `F64`, `float` | `F64` | `number` |
| `I32`, `int` | `I32` | `number` |
| `STR`, `str` | `STR` | `string` |
| `BOOL`, `bool` | `BOOL` | `boolean` |
| `BYTES` | `BYTES` | `Uint8Array` |
| `OtherFeature` | `ref(OtherFeature)` | `OtherFeature \| null` |
| `List[F64]` | `listOf(F64)` | `number[]` |
| `List[OtherFeature]` | `listOf(ref(OtherFeature))` | `OtherFeature[]` |

All 12 TypeVar field types (`U8`, `U16`, `U32`, `I32`, `U8N`, `U16N`, `F32`, `F64`, `STR`, `WSTR`, `BYTES`, `BOOL`) and 4 native Python types (`int`, `float`, `str`, `bool`) are supported.

#### Cross-file references

When a class in `scene.py` references a class defined in `geometry.py`, the generated `scene.ts` will include the appropriate relative import:

```typescript
import { Point } from './geometry.js';
```

#### Duplicate class names across files

Each `.py` file is treated as an independent module. The same class name (e.g. `Point`) may appear in multiple files — all are generated in their respective `.ts` files without conflict. Within a single file, Python's last-definition-wins rule applies.

#### Circular references

Self-referential and mutually recursive types are detected automatically and use lazy refs:

```python
from fastdb4py import feature, I32


@feature
class Node:
    val: I32
    next: 'Node'  # forward reference
```

Generates:

```typescript
export class Node extends Feature {
  static schema = defineSchema({
    val: I32,
    next: ref(() => Node),  // lazy ref for cycle
  });
  declare val: number;
  declare next: Node | null;
}
```

#### Error handling

The tool is designed to be robust:

- **Syntax errors** in a `.py` file are reported and skipped; other files are still processed
- **Import errors** are similarly skipped with a warning
- **Undeclared type references** produce a warning; the class is still generated
- **Non-Feature classes** (plain classes, dataclasses, functions) are silently ignored

## Development notes

- treat `python/fastdb4py/core/` as generated output
- prefer changes in `python/fastdb4py/` unless the bridge itself must change
- rebuild after C++ or SWIG changes
- when the C++ core wire format changes, revalidate both Python and TypeScript bindings
