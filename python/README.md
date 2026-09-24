# fastdb4py

Python bindings for `fastdb`, built on top of the C++ core in `fastcarto/fastdb/` and exposed through SWIG.

> **0.2.0:** This document covers the standalone storage API and the official `fastdb4py.payload` projection. `RecordEngine` is the standalone AoS engine name. The C++ Core owns `fastdb.payload.v1` compilation, canonicalization, digest, profile validation, layout and binary decoding. Python supplies language-level authoring and ownership interfaces. See the [accepted design](../docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md) and [release status](../docs/releases/0.2.0.md).

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
   - standalone `@feature`, `RecordEngine`, `ObjectEngine`, `Table` and `FastSerializer` abstractions, plus the Core-backed `fastdb4py.payload` projection

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

## Portable payload projection

`fastdb4py.payload` is the official Python 3.10+ projection of
`fastdb.payload.v1`. It binds the stable native ABI packaged beside
`fastdb4py.core`; the C++ Core remains the sole authority for compilation,
canonical JSON, SHA-256, layout, binary validation, graph identity,
materialization, and errors.

```python
from fastdb4py.payload import CompiledSpec, Profile

source = b'{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]}'

with CompiledSpec.compile(source) as spec:
    assert spec.profile() is Profile.RECORD_V1
    assert len(spec.sha256()) == 32
```

Owned payload objects support `close()` and context-manager use. Closure is
locally idempotent, every Core error preserves `code`, `symbol`, `path`,
`message`, and exact `details_json`, and safe `str`, `wstr`, and bytes accessors
copy before their native access guard is released. Checked views are invalid
after Core invalidation; `materialize()` asks Core for a detached value rather
than recursively decoding one in Python.

The wheel contains one native FastDB Core and Python projection source, not a
second Python runtime. Python 3.10 compile/import and installed-wheel execution
are package gates. `CompiledSpec.generate(...)` returns the same Core-owned
deterministic C++/Rust/Python/TypeScript ArtifactSet available to the other
official projections. It never writes a destination tree; the `fdb codegen` CLI publishes the Core-returned artifact set through its filesystem facade.

P4 is locally complete. Python participates in the same hostile all-values,
recursive-list, identifier-collision, and shared-cycle generation matrix as
the other official projections, while all parsing, identity, topology, and
rendering remain in the C++ Core. P5 local clean cut is complete. Hosted
execution and registry publication are tracked in the [release record](../docs/releases/0.2.0.md).

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
from fastdb4py import feature, RecordEngine, Layout, F64
import numpy as np


@feature
class Point:
    x: F64
    y: F64
    z: F64


db = RecordEngine.truncate([Layout(Point, 5)])
table = db.table(Point)

table.fill(
    x=np.linspace(0.0, 1.0, 5),
    y=np.zeros(5),
    z=np.ones(5),
)

print(table[2].x)
print(table.column.x.mean())
```

## Backed View Lifetimes

FastDB Python values have two lifetime modes. A plain `@feature` instance is owned Python data: its fields live in `__dict__`, it can be kept indefinitely, and assigning `point.x = 1.0` is ordinary Python object mutation. A row returned from a mapped `Table` is a backed feature view: schema fields read from the native table row, scalar numeric writes go back to native storage when the view is writeable, and the view is tied to a `FdbViewOwner`.

Standalone FastDB tables use a trusted unchecked owner by default, so existing high-performance workflows keep raw NumPy column access:

```python
table = db.table(Point)
raw_x = table.column.x          # NumPy view for trusted standalone use
raw_x[:] = 1.0
```

Call-scoped integrations that reuse backing memory should pass a checked owner. Checked numeric columns return owner-bound views; `to_numpy()` returns a detached copy, while `unsafe_numpy_view()` explicitly exports the raw array and cannot be revoked after export:

```python
import fastdb4py as fdb

owner = fdb.FdbViewOwner(checked=True, writeable=True)
table = db.table(Point, owner=owner, writeable=True)

row = table[0]
row.x = 2.0                    # checked write-through to native storage

col = table.column.x           # NumericColumnView, not a bare ndarray
copy = col.to_numpy()          # detached copy
raw = col.unsafe_numpy_view()  # trusted escape hatch

fdb.invalidate(table)
# row.x and col[0] now raise FdbViewInvalidatedError.
# copy remains usable because it is materialized.
```

Use `fdb.materialize(value)` or `value.to_owned()` to detach FastDB-managed views before retaining data beyond the owner lifetime. `fdb.invalidate(...)` is idempotent and also works on containers such as lists, tuples, and mappings. This model is a correctness guard for stale views, not a hostile in-process sandbox; code that deliberately keeps raw arrays from `unsafe_numpy_view()` remains responsible for its own lifetime discipline.

Explicit read-only views do not require a checked owner:

```python
table = db.table(Point, writeable=False)

try:
    table[0].x = 2.0
except fdb.FdbViewWriteError:
    pass

try:
    table.column.x[0] = 2.0
except fdb.FdbViewWriteError:
    pass
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

### Fixed-size tables with `RecordEngine.truncate`

Use `truncate` when the row count is known ahead of time. For fixed-size tables, there are two UTF-8 string-ingest tiers:

- **Default high-level path** — `tbl.fill(..., name=[...])` now routes raw strings through the native batch string-column API
- **Advanced prepacked path** — `pack_utf8_column([...]) + tbl.column.name.fill_utf8(...)`

If your pipeline starts from Python `str` values, prefer the default raw path. Reach for the prepacked path only when you already have UTF-8 offsets/data buffers from an upstream step.

```python
from fastdb4py import feature, RecordEngine, Layout, F64, F32
import numpy as np


@feature
class Particle:
    x: F64
    y: F64
    vx: F64
    vy: F64
    mass: F32


N = 100_000
db = RecordEngine.truncate([Layout(Particle, N)])
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


db = RecordEngine.truncate([
    Layout(Particle, 50_000),
    Layout(Cell, 1_000),
])
```

For fixed-size tables with string columns, the default batch-ingest API is still `Table.fill(...)`. It batches numeric data and raw Python `STR` values together, the raw-string path routes through the native batch string-column API with upfront length validation, scalar `BOOL` columns use the same explicit bool parser as mutable engine writes before bulk numeric storage, and ordinary `U8` columns remain numeric casts:

```python
from fastdb4py import feature, RecordEngine, Layout, U32, F64, STR, pack_utf8_column
import numpy as np


@feature
class Sample:
    row_id: U32
    value: F64
    name: STR


db = RecordEngine.truncate([Layout(Sample, 3)])
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

Reference fields let one feature point at another feature, possibly in a different table. This is handled by `ObjectEngine`, not `RecordEngine`.

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

db2 = RecordEngine.load("simulation_state", from_file=True)
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

For large homogeneous numerical datasets, `RecordEngine.truncate` plus columnar writes is still the preferred path. `FastSerializer` is aimed at trees, graphs, mesh-like structures, and mixed payloads.

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
uv run pytest tests/python/payload/test_payload_codegen.py
uv run pytest tests/python/test_cli_codegen.py
```

## Core-owned portable artifact CLI

`fastdb4py` registers `fdb codegen` through `[project.scripts]`. It accepts a
portable specification, delegates compilation and generation to the C++ Core,
and writes the returned ArtifactSet to a new destination tree:

```bash
fdb codegen specification.json \
  --target python \
  --output ./generated-fastdb
```

Targets are `cpp`, `rust`, `python`, and `typescript`. The command refuses an
existing output path, unsafe or duplicate artifact paths, unsupported
artifact kinds, and file/directory conflicts. It uses exclusive staging-file
creation and no-replace publication. The Python layer does not discover
classes, parse the portable specification, calculate its digest, or render
source code.

## Development notes

- treat `python/fastdb4py/core/` as generated output
- prefer changes in `python/fastdb4py/` unless the bridge itself must change
- rebuild after C++ or SWIG changes
- when the C++ core wire format changes, revalidate both Python and TypeScript bindings
- before publishing `fastdb4py`, bump `pyproject.toml`, refresh `uv.lock`, update the `fastdb4py` changelog section, and confirm the corresponding `py/v<version>` tag does not already exist
