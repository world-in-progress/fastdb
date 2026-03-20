# fastdb

[![PyPI version](https://badge.fury.io/py/fastdb4py.svg)](https://badge.fury.io/py/fastdb4py)
[![npm version](https://badge.fury.io/js/fastdb4ts.svg)](https://badge.fury.io/js/fastdb4ts)
[![Run Tests](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml/badge.svg)](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml)

`fastdb` is a C++ local database library designed as a fast, lightweight, and easy-to-use data communication layer for RPC and coupled modeling in scientific computing.

This repository now contains three closely related layers:

- **C++ core** — native storage engine, binary layout, and serialization primitives
- **`fastdb4py`** — Python bindings via SWIG, with NumPy-oriented columnar access and shared-memory IPC
- **`fastdb4ts`** — TypeScript bindings via WebAssembly/Embind, focused on browser-friendly typed data access and serializer compatibility

**Core design goals:**
- **Zero-copy columnar access** — efficient field-oriented access for high-volume numerical workloads
- **Ref-graph support** — Features can reference other Features across tables, forming typed object graphs
- **Compact binary transport** — save/load databases as binary buffers or files
- **Cross-binding consistency** — Python and TypeScript bindings share the same native storage model and serializer semantics
- **Schema-driven codegen** — Python Feature classes serve as the single source of truth; the `fdb codegen` CLI generates equivalent TypeScript schemas automatically

## Documentation map

- **Python binding (`fastdb4py`)**: see [`python/README.md`](python/README.md)
- **TypeScript binding (`fastdb4ts`)**: see [`ts/README.md`](ts/README.md)
- **C++ core (`fastcarto/fastdb`)**: see [`fastcarto/README.md`](fastcarto/README.md)
- **TypeScript/WASM analysis docs**: see [`ts/analysis/`](ts/analysis/)
- **Codegen CLI (`fdb codegen`)**: see [CLI tools](#cli-tools) below, or the full reference in [`python/README.md`](python/README.md)

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for per-binding unreleased changes.  
For historical release notes, see the [GitHub Releases](https://github.com/world-in-progress/fastdb/releases) page.

## Installation

### Python binding (fastdb4py)

```bash
pip install fastdb4py
```

### TypeScript binding (fastdb4ts)

```bash
npm install fastdb4ts
```

## Quick start

For a minimal end-to-end example, start with:

- [`python/README.md`](python/README.md) for `fastdb4py`
- [`ts/README.md`](ts/README.md) for `fastdb4ts`

If you are working on native internals or storage layout, start with:

- [`fastcarto/README.md`](fastcarto/README.md)

## CLI tools

`fastdb4py` ships a CLI named `fdb` for cross-language tooling. Currently it provides the `codegen` subcommand.

### `fdb codegen` — Python → TypeScript schema generator

Generate TypeScript `Feature` classes from a directory of Python feature definitions:

```bash
fdb codegen --ts ./python_features/ ./ts_features/
```

This mirrors the input directory structure, generating one `.ts` file per `.py` file. Each Python `Feature` subclass becomes a TypeScript class with `defineSchema(...)` and `declare` fields.

Features:
- All scalar types (`U8`–`F64`, `STR`, `WSTR`, `BYTES`, `BOOL`) and native Python types (`int`, `float`, `str`, `bool`) are mapped automatically
- Feature references → `ref(ClassName)`, lists of Features → `listOf(ref(ClassName))`
- Circular/self-referential types → lazy refs `ref(() => ClassName)` detected automatically
- Cross-file dependencies → relative `import` statements in the generated TypeScript
- Topological ordering ensures dependency classes are emitted before dependents
- Same class name in different files is legal — each file is an independent module, all are generated

Example input (`geometry.py`):

```python
from fastdb4py import Feature, F64, STR

class Point(Feature):
    x: F64
    y: F64
    label: STR
```

Generated output (`geometry.ts`):

```typescript
import { F64, Feature, STR, defineSchema } from 'fastdb4ts';

export class Point extends Feature {
  static schema = defineSchema({
    x: F64,
    y: F64,
    label: STR,
  });
  declare x: number;
  declare y: number;
  declare label: string;
}
```

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

## Free-threaded Python (PEP 703)

`fastdb4py` includes preliminary support for Python 3.13+ free-threaded builds (`python3.13t`).

### Thread-safety guarantees

| Component | Thread-safe? | Notes |
|---|---|---|
| Module-level caches (`get_class_schema`, serializer schema) | ✅ Yes | Protected by `threading.Lock`; safe under both GIL and free-threaded builds |
| `ColumnAccessor` column cache (`table.column.x`) | ✅ Yes | Cold path (first access) is lock-protected; hot path (cache hit) is lock-free |
| `Feature` instances | ❌ No | Instance-level `_cache` dict is not synchronized — use external locking or one instance per thread |
| `ORM` / `Table` instances | ❌ No | Not designed for concurrent mutation — create separate ORM instances per thread, or synchronize externally |
| SWIG C++ calls | ✅ Yes | Long-running pure C++ operations release the GIL via `%feature("threadallow")` |

### Recommended patterns for multi-threaded code

```python
# ✅ Good: each thread owns its own ORM view
def worker():
    orm = ORM.truncate([TableDefn(Point, 1000)])
    tbl = orm[Point][Point]
    tbl.fill(x=np.arange(1000, dtype=np.float64))

# ✅ Good: shared ORM with read-only access (after truncate/combine)
shared_orm = ORM.truncate([TableDefn(Point, N)])
# ... fill data ...
# Multiple threads can safely read table.column.x concurrently

# ⚠️ Caution: sharing Feature instances across threads
lock = threading.Lock()
feat = Point(x=1.0)
with lock:           # external synchronization required
    feat.x = 2.0
```

### Build configuration

The CI tests against Python 3.13t (free-threaded) in addition to standard 3.12. The `setup.py` auto-detects `Py_GIL_DISABLED` and passes the flag to the C++ build.

## Development

This project uses DevContainer for the development environment. See `.devcontainer/devcontainer.example.json` for configuration details. Requires Docker/Podman and the VSCode DevContainer extension.

Common development commands from the repository root:

```bash
./py_utils.sh --clean   # remove C++ build artifacts and SWIG-generated bindings
./py_utils.sh --build   # build C++ core + Python bindings
./py_utils.sh --test    # run Python unit tests
bash ts/build-wasm.sh   # build the WebAssembly module for fastdb4ts
npm run test:ts         # run root TypeScript tests
fdb codegen --ts <input_dir> <output_dir>  # generate TypeScript schemas from Python features
```

Build requirements depend on the layer you are working on:

- **Python binding**: C++17 compiler, CMake >= 3.16, SWIG >= 4.0, NumPy
- **TypeScript/WASM binding**: Emscripten, Node.js, npm
- **Native core**: C++17 compiler and CMake
