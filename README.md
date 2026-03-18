# fastdb

[![PyPI version](https://badge.fury.io/py/fastdb4py.svg)](https://badge.fury.io/py/fastdb4py)
[![Run Tests](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml/badge.svg)](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml)

A C++ local database library with Python bindings (via SWIG). Designed as a fast, lightweight, and easy-to-use data communication layer for RPC and coupled modeling in scientific computing.

**Core design goals:**
- **Zero-copy columnar access** — field data is exposed directly as NumPy arrays backed by C++ memory, no serialization overhead
- **Ref-graph support** — Features can reference other Features across tables, forming typed object graphs
- **Shared memory IPC** — publish a database to POSIX/Windows shared memory and read it from other processes with zero-copy
- **File persistence** — save and load databases as compact binary files

## What's new

- **2026-03-17 (Release 0.1.13)**: Fixed two C++ correctness bugs in the batch field read/write API: (1) `getFieldsAsDoubles` now correctly handles U8/U16/U32/I32 fields (previously returned NAN); (2) `set_field_value_t` now correctly writes U16N normalized fields (missing `memcpy` caused silent data loss). Also added batch scalar field API (`read_all_scalars` / `write_all_scalars`) with up to 12× speedup over per-field access.
- **2026-03-04 (Release 0.1.12)**: Fixed a critical issue where loading large database files (> 2GB) on Linux/Unix systems would fail to read the complete file, leading to missing tables or data corruption. The file reading logic has been improved to correctly handle partial reads for large files. (PR #23)
- **2026-03-04 (Memory Overflow Improvement)**: Enhanced the `MemoryStream` implementation to handle large data sizes exceeding 4GB without causing size overflow in `chunk_data_t.size` (u32). This improvement allows for more robust handling of large datasets in memory. (PR #22)
- **2026-02-28 (Release Improvement)**: Fix bugs related to build process in Windows. (PR #20)
- **2025-12-31 (Bug Fix)**: Fixed an issue where shared memory segments were not being properly unregistered from the resource tracker upon closing, which could lead to resource leaks. (PR #17)
- **2025-12-15 (Release Improvement)**: Enabled distribution of pre-compiled binary wheels for macOS (Intel/Apple Silicon) and Linux (x86_64/aarch64), eliminating the need for local compilation tools during installation. (PR #15)
- **2025-12-10 (Bug Fix)**: Fixed the data type mapping for `U32` fields in Python bindings to ensure correct representation as unsigned 32-bit integers in NumPy arrays. (PR #13)
- **2025-12-10 (Bug Fix)**: Fixed an out-of-bounds access issue in `FastVectorDbLayer::Impl::getFieldOffset()` when the field index is equal to the field count. (PR #12)
- **2025-12-10 (Performance Improvement)**: Modified `ORM.truncate()` to support directly allocating features without initializing them for performance consideration. Note that this change may have side effects; please test thoroughly. (PR #11)

## Installation

```bash
pip install fastdb4py
```

Pre-compiled binary wheels are provided for major platforms (macOS Intel/Apple Silicon, Linux x86_64/aarch64, Windows AMD64). For other systems, the package builds from source and requires a C++ compiler, CMake, and SWIG.

## Quick Start

```python
import fastdb4py as fx
import numpy as np

# 1. Define schema
class Point(fx.Feature):
    x: fx.F64
    y: fx.F64
    z: fx.F64

# 2. Create a fixed-size table and fill via NumPy (fastest path)
N = 10_000
db = fx.ORM.truncate([fx.TableDefn(Point, N)])
tbl = db[Point][Point]

tbl.column.x[:] = np.linspace(0, 1, N)
tbl.column.y[:] = np.zeros(N)
tbl.column.z[:] = np.ones(N)

# 3. Read back as NumPy arrays (zero-copy)
xs = tbl.column.x   # numpy array backed by C++ memory
print(xs[:5])

# 4. Save and reload
db.save("my_data")
db2 = fx.ORM.load("my_data", from_file=True)
```

## Field Types

| Python type | C++ storage | Description |
|-------------|-------------|-------------|
| `fx.U8`     | `uint8_t`   | Unsigned 8-bit integer |
| `fx.U16`    | `uint16_t`  | Unsigned 16-bit integer |
| `fx.U32`    | `uint32_t`  | Unsigned 32-bit integer |
| `fx.I32`    | `int32_t`   | Signed 32-bit integer |
| `fx.F32`    | `float`     | 32-bit float |
| `fx.F64`    | `double`    | 64-bit float |
| `fx.STR`    | string table index | Short ASCII string |
| `fx.WSTR`   | string table index | Unicode string |
| `fx.BYTES`  | blob        | Raw bytes / geometry chunk |
| `fx.U8N`    | `uint8_t`   | Normalized float in [vmin, vmax], stored as u8 |
| `fx.U16N`   | `uint16_t`  | Normalized float in [vmin, vmax], stored as u16 |
| `OtherFeature` | ref     | Cross-table reference to another Feature |

## Usage

### Creating a Database

#### Fixed-size tables (`ORM.truncate`)

Use `ORM.truncate` when you know the number of rows up front. This pre-allocates the full table in one shot and enables the fastest columnar write path.

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

# Vectorized write — 1 memcpy per column, ~1000× faster than row-wise push
tbl.column.x[:]    = np.random.uniform(-1, 1, N)
tbl.column.y[:]    = np.random.uniform(-1, 1, N)
tbl.column.vx[:]   = np.zeros(N)
tbl.column.vy[:]   = np.zeros(N)
tbl.column.mass[:] = np.ones(N, dtype=np.float32)
```

Multiple tables with different schemas can be created in one call:

```python
class Cell(fx.Feature):
    id: fx.U32
    temperature: fx.F64

db = fx.ORM.truncate([
    fx.TableDefn(Particle, 50_000),
    fx.TableDefn(Cell, 1_000),
])
```

#### Dynamic tables (`ORM.create` + `push`)

Use `ORM.create` when the number of rows is not known in advance.

```python
import fastdb4py as fx

class LogEntry(fx.Feature):
    level: fx.U8
    code: fx.U32
    message: fx.STR

db = fx.ORM.create()

db.push(LogEntry(level=1, code=200, message="ok"))
db.push(LogEntry(level=3, code=500, message="internal error"))

db._combine()   # finalize: serialize + reload as immutable db
```

### Reading Data

#### Columnar access (zero-copy NumPy)

The fastest read path. `table.column.field_name` returns a NumPy array that is a **direct view** of the underlying C++ memory — no copy, no allocation.

```python
db = fx.ORM.load("my_data", from_file=True)
tbl = db[Particle][Particle]

xs = tbl.column.x    # np.ndarray, dtype=float64, shape=(N,)
ys = tbl.column.y

# Vectorized operations in-place
xs += 0.01 * tbl.column.vx
ys += 0.01 * tbl.column.vy

# NumPy aggregations
print(f"Mean x: {xs.mean():.4f}")
print(f"Max speed: {np.sqrt(tbl.column.vx**2 + tbl.column.vy**2).max():.4f}")
```

#### Row-wise iteration

```python
# Standard iterator — allocates a new Feature wrapper per row
for p in tbl:
    print(f"x={p.x:.3f}  y={p.y:.3f}")

# High-performance iterator — reuses the same wrapper (no per-row allocation)
# Do NOT hold references to the yielded object across loop iterations.
for p in tbl.iter_reuse():
    print(f"x={p.x:.3f}  y={p.y:.3f}")
```

#### Index access

```python
first = tbl[0]
print(f"Particle 0: x={first.x}, mass={first.mass}")
```

### Feature References (Object Graphs)

Features can store typed references to features in other tables. Assign a Feature instance to a ref field; cross-table links are resolved automatically on read.

```python
import fastdb4py as fx

class Point(fx.Feature):
    x: fx.F64
    y: fx.F64
    z: fx.F64

class Triangle(fx.Feature):
    a: Point    # ref field — links to a Point feature
    b: Point
    c: Point

db = fx.ORM.truncate([
    fx.TableDefn(Point, 6),
    fx.TableDefn(Triangle, 2, 'TriA'),
    fx.TableDefn(Triangle, 2, 'TriB'),
])

points = db[Point][Point]
for i in range(6):
    points[i].x = float(i)
    points[i].y = float(i) * 0.5
    points[i].z = 0.0

tri = db[Triangle]['TriA'][0]
tri.a = points[0]
tri.b = points[1]
tri.c = points[2]

# Read back — ref is resolved transparently
print(tri.a.x, tri.b.x, tri.c.x)   # 0.0  1.0  2.0
```

### File Persistence

```python
# Save to disk
db.save("simulation_state")

# Load from disk
db = fx.ORM.load("simulation_state", from_file=True)
tbl = db[Particle][Particle]

# Access, modify, then save again
tbl.column.x[:] += 1.0
db.save("simulation_state")
```

### Shared Memory IPC

Share a database across processes with zero-copy. The publisher writes once;
all readers map the same physical memory.

```python
# --- Process A: publisher ---
import fastdb4py as fx

class Signal(fx.Feature):
    t: fx.F64
    value: fx.F64

db = fx.ORM.create()
db.push(Signal(t=0.0, value=3.14))
db.push(Signal(t=0.1, value=2.71))

db.share("my_signals", close_after=True)  # publish and release local handle
```

```python
# --- Process B: reader (runs concurrently) ---
import fastdb4py as fx

class Signal(fx.Feature):
    t: fx.F64
    value: fx.F64

db = fx.ORM.load("my_signals")   # load from shared memory
tbl = db[Signal][Signal]

for s in tbl:
    print(f"t={s.t}  value={s.value}")

db.unlink()   # release shared memory segment when done
```

### Batch Scalar Access

For db-mapped Features (e.g. from `table[i]` or `table.iter_reuse()`), you can read or write all scalar fields in a single C++ call instead of one SWIG call per field. This is useful in tight loops or when building custom row-processing kernels.

**Covered types:** `U8`, `U16`, `U32`, `I32`, `F32`, `F64`, `U8N`, `U16N`. Non-scalar fields (`STR`, `WSTR`, `REF`, `BYTES`) are automatically excluded — they are not present in the array and must still be accessed individually via attribute access.

**Array format:** Always `dtype=float64`, regardless of the original field types. All covered types can round-trip through float64 without precision loss on read. On write, values are cast back to each field's native type by the C++ layer.

```python
import numpy as np

class Particle(fx.Feature):
    index: fx.I32   # scalar — position 0 in the array
    name:  fx.STR   # non-scalar — excluded, use feat.name directly
    mass:  fx.F32   # scalar — position 1
    x:     fx.F64   # scalar — position 2
    y:     fx.F64   # scalar — position 3

# ... (db setup as above) ...
feat = tbl[0]

# Read: all scalar fields → float64 array (1 SWIG call)
out = np.empty(4, dtype=np.float64)   # length = number of scalar fields
feat.read_all_scalars(out)
# out[0] = index (i32 → f64, exact)
# out[1] = mass  (f32 → f64, exact)
# out[2] = x     (f64, exact)
# out[3] = y     (f64, exact)

# Write: float64 array → all scalar fields (1 SWIG call)
# Integer fields are truncated (not rounded): 1.9 → 1
feat.write_all_scalars(np.array([5.0, 1.5, 3.14, 2.71]))
# index = (i32)5.0 = 5, mass = (f32)1.5, x = 3.14, y = 2.71

# feat.name must still be set via normal attribute access
feat.name = "electron"
```

> **Write truncation:** On write, the C++ layer casts `double → native type` using C truncation semantics (toward zero, not rounding). For integer fields, always pass exact `.0` values. Passing `1.9` to an `I32` field will write `1`, not `2`.

The array length and field order can be queried at runtime:

```python
n_scalars = len(feat._schema.scalar_field_ids_np)   # number of scalar fields
```

### FastSerializer

`FastSerializer` serializes a `Feature` object graph to bytes and deserializes it back. It is designed for cases where you need to persist or transmit a complete object graph that includes nested Features, cyclic references, and heterogeneous list types — scenarios that go beyond what the ORM columnar tables cover directly.

**Supported field content:**

| Field type | Serialized as |
|------------|---------------|
| Scalar fields (`U8`–`F64`) | fastdb columns (columnar) |
| `List[U32]` / `List[F64]` | dedicated columnar auxiliary layers |
| `List[int]` / `List[float]` / `List[str]` | raw blob |
| `List[Feature]` / ref fields | object ref encoding (`[layer_idx:u16][feature_idx:u32]`) |
| Cyclic references | handled — identity is preserved on load |

```python
from fastdb4py import FastSerializer, Feature, I32, F64, STR
from typing import List

class Point(Feature):
    x: F64
    y: F64

class Line(Feature):
    id:     I32
    label:  STR
    points: List[Point]   # list of Feature refs

# Build an object graph
p1 = Point(x=0.0, y=0.0)
p2 = Point(x=1.0, y=1.0)
line = Line(id=42, label="edge", points=[p1, p2])

# Serialize to bytes
data: bytes = FastSerializer.dumps(line)

# Deserialize
line2 = FastSerializer.loads(data, Line)
print(line2.label)           # "edge"
print(line2.points[1].x)    # 1.0
```

Cyclic references are preserved:

```python
class Node(Feature):
    val:  I32
    next: 'Node'

n1 = Node(val=1)
n2 = Node(val=2)
n1.next = n2
n2.next = n1   # cycle: n1 → n2 → n1

data  = FastSerializer.dumps(n1)
check = FastSerializer.loads(data, Node)
assert check.next.next is check   # identity preserved ✓
```

`FastSerializer` is significantly slower than the native ORM columnar path for large uniform datasets. For bulk numerical data, prefer `ORM.truncate` + columnar writes. Use `FastSerializer` when your data is naturally graph-shaped (trees, linked lists, meshes with shared vertices, etc.).

## Performance Notes

| Pattern | Throughput | Notes |
|---------|-----------|-------|
| `table.column.x[:]` columnar read/write | **~100 ns** for any N | Zero-copy NumPy view, 1 SWIG call |
| `Table.fill(field, array)` | **~2 µs** per column | 1 SWIG call + memcpy |
| `feature.read_all_scalars()` | **~200 ns** for 3 fields | 1 SWIG call for all scalar fields |
| `table.iter_reuse()` row access | **~350 ns/row** | Reuses Feature wrapper, no allocation |
| `for feat in table` row access | **~1.2 µs/row** | Allocates Feature wrapper per row |
| `feat.x` single field read (db-mapped) | **~420 ns** | 1 SWIG call |

**Recommended patterns by use case:**

- **Bulk read/write of one field across all rows** → `table.column.x` (columnar, zero-copy)
- **Bulk fill all fields from arrays** → `ORM.truncate` + `table.column.field[:] = array`
- **Iterate and process all fields per row** → `table.iter_reuse()` + `feat.read_all_scalars()`
- **Sparse random access** → `table[i].field`

## Development

This project uses DevContainer for the development environment. See `.devcontainer/devcontainer.example.json` for configuration details. Requires Docker/Podman and the VSCode DevContainer extension.

The `py_utils.sh` script handles common development tasks:

```bash
./py_utils.sh --clean   # remove C++ build artifacts and SWIG-generated bindings
./py_utils.sh --build   # build C++ core + Python bindings
./py_utils.sh --test    # run Python unit tests
```

Build requirements: C++17 compiler, CMake >= 3.16, SWIG >= 4.0, NumPy.
