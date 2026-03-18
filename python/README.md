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
   - ergonomic `Feature`, `ORM`, `Table`, and `FastSerializer` abstractions

Important directories:

- `python/fastdb4py/type.py`
  - field types such as `U32`, `F64`, `STR`, `BYTES`
- `python/fastdb4py/feature/`
  - schema discovery, feature dispatch, caching, runtime access
- `python/fastdb4py/orm/`
  - `ORM`, `Table`, `TableDefn`, column access, iteration
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
./py_utils.sh --build
```

Prebuilt wheels are expected for the main supported platforms. Source builds require a C++17 compiler, CMake, SWIG, and NumPy.

## Quick start

```python
import fastdb4py as fx
import numpy as np


class Point(fx.Feature):
    x: fx.F64
    y: fx.F64
    z: fx.F64


db = fx.ORM.truncate([fx.TableDefn(Point, 5)])
table = db[Point][Point]

table.column.x[:] = np.linspace(0.0, 1.0, 5)
table.column.y[:] = np.zeros(5)
table.column.z[:] = np.ones(5)

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
| `fx.STR` | string table index | Short string |
| `fx.WSTR` | wide string table index | Unicode string |
| `fx.BYTES` | blob / raw geometry payload | Byte storage |
| `fx.U8N` | normalized `uint8_t` | Quantized float in a configured range |
| `fx.U16N` | normalized `uint16_t` | Quantized float in a configured range |
| `OtherFeature` | feature reference | Typed cross-table reference |

## Defining schemas with `Feature`

Users model rows by subclassing `Feature` and annotating fields:

```python
import fastdb4py as fx


class Particle(fx.Feature):
    id: fx.U32
    x: fx.F64
    y: fx.F64
    mass: fx.F32
```

The field order is part of the schema contract. It affects table layout, serializer traversal order, and binary compatibility.

## Database creation patterns

### Fixed-size tables with `ORM.truncate`

Use `truncate` when the row count is known ahead of time. This is the fastest path for bulk numeric workloads.

```python
import fastdb4py as fx
import numpy as np


class Particle(fx.Feature):
    x: fx.F64
    y: fx.F64
    vx: fx.F64
    vy: fx.F64
    mass: fx.F32


N = 100_000
db = fx.ORM.truncate([fx.TableDefn(Particle, N)])
tbl = db[Particle][Particle]

tbl.column.x[:] = np.random.uniform(-1.0, 1.0, N)
tbl.column.y[:] = np.random.uniform(-1.0, 1.0, N)
tbl.column.vx[:] = np.zeros(N)
tbl.column.vy[:] = np.zeros(N)
tbl.column.mass[:] = np.ones(N, dtype=np.float32)
```

Multiple tables can be created in one call:

```python
class Cell(fx.Feature):
    id: fx.U32
    temperature: fx.F64


db = fx.ORM.truncate([
    fx.TableDefn(Particle, 50_000),
    fx.TableDefn(Cell, 1_000),
])
```

### Dynamic tables with `ORM.create` + `push`

Use `create` when the final row count is not known in advance.

```python
import fastdb4py as fx


class LogEntry(fx.Feature):
    level: fx.U8
    code: fx.U32
    message: fx.STR


db = fx.ORM.create()
db.push(LogEntry(level=1, code=200, message="ok"))
db.push(LogEntry(level=3, code=500, message="internal error"))

db._combine()
```

## Reading data

### Columnar access

The fastest read path in Python is columnar access. `table.column.field_name` returns a NumPy array directly backed by the native storage.

```python
xs = tbl.column.x
ys = tbl.column.y

xs += 0.01 * tbl.column.vx
ys += 0.01 * tbl.column.vy

print(xs.mean())
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

Reference fields let one feature point at another feature, possibly in a different table.

```python
import fastdb4py as fx


class Point(fx.Feature):
    x: fx.F64
    y: fx.F64
    z: fx.F64


class Triangle(fx.Feature):
    a: Point
    b: Point
    c: Point


db = fx.ORM.truncate([
    fx.TableDefn(Point, 6),
    fx.TableDefn(Triangle, 2, "TriA"),
])

points = db[Point][Point]
for i in range(6):
    points[i].x = float(i)
    points[i].y = float(i) * 0.5
    points[i].z = 0.0

tri = db[Triangle]["TriA"][0]
tri.a = points[0]
tri.b = points[1]
tri.c = points[2]

print(tri.a.x, tri.b.x, tri.c.x)
```

## File persistence

```python
db.save("simulation_state")

db2 = fx.ORM.load("simulation_state", from_file=True)
tbl2 = db2[Particle][Particle]
print(tbl2.column.x[:5])
```

## Shared-memory IPC

Shared memory is available in the Python binding even though it is intentionally absent from the current TypeScript binding.

Publisher:

```python
import fastdb4py as fx


class Signal(fx.Feature):
    t: fx.F64
    value: fx.F64


db = fx.ORM.create()
db.push(Signal(t=0.0, value=3.14))
db.push(Signal(t=0.1, value=2.71))
db.share("my_signals", close_after=True)
```

Reader:

```python
import fastdb4py as fx


class Signal(fx.Feature):
    t: fx.F64
    value: fx.F64


db = fx.ORM.load("my_signals")
tbl = db[Signal][Signal]
for row in tbl:
    print(row.t, row.value)
db.unlink()
```

## Batch scalar field access

For db-mapped features, scalar fields can be read or written in bulk to reduce per-field bridge overhead.

```python
import numpy as np
import fastdb4py as fx


class Particle(fx.Feature):
    index: fx.I32
    name: fx.STR
    mass: fx.F32
    x: fx.F64
    y: fx.F64


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
- numeric-list fast paths through auxiliary columnar layers

Example:

```python
from typing import List
from fastdb4py import FastSerializer, Feature, I32, F64, STR


class Point(Feature):
    x: F64
    y: F64


class Line(Feature):
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
class Node(Feature):
    val: I32
    next: "Node"


n1 = Node(val=1)
n2 = Node(val=2)
n1.next = n2
n2.next = n1

check = FastSerializer.loads(FastSerializer.dumps(n1), Node)
assert check.next.next is check
```

For large homogeneous numerical datasets, `ORM.truncate` plus columnar writes is still the preferred path. `FastSerializer` is aimed at trees, graphs, mesh-like structures, and mixed payloads.

## Running tests

From the repository root:

```bash
uv run pytest tests/python
```

Focused runs:

```bash
uv run pytest tests/python/test_column_way.py
uv run pytest tests/python/test_fast_serializer.py
```

## Development notes

- treat `python/fastdb4py/core/` as generated output
- prefer changes in `python/fastdb4py/` unless the bridge itself must change
- rebuild after C++ or SWIG changes
- when the C++ core wire format changes, revalidate both Python and TypeScript bindings
