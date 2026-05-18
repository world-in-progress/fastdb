# fastdb

[![PyPI version](https://badge.fury.io/py/fastdb4py.svg)](https://badge.fury.io/py/fastdb4py)
[![npm version](https://badge.fury.io/js/fastdb4ts.svg)](https://badge.fury.io/js/fastdb4ts)
[![Run Tests](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml/badge.svg)](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml)

`fastdb` is a C++ local database library designed as a fast, lightweight, and easy-to-use data communication layer for RPC and coupled modeling in scientific computing.

This repository now contains three closely related layers:

- **C++ core** — native storage engine, binary layout, and serialization primitives
- **`fastdb4py`** — Python bindings via SWIG, with NumPy-oriented columnar access and shared-memory IPC
- **`fastdb4ts`** — TypeScript bindings via WebAssembly/Embind, focused on browser-friendly typed data access and schema-compatible table access

**Core design goals:**
- **Zero-copy columnar access** — efficient field-oriented access for high-volume numerical workloads
- **Ref-graph support** — Features can reference other Features across tables, forming typed object graphs
- **Compact binary transport** — save/load databases as binary buffers or files; shared-memory deserialization for zero-copy IPC
- **Cross-binding consistency** — Python and TypeScript bindings share the same native storage model and schema semantics
- **Schema-driven codegen** — Python `@feature` classes can serve as the source of truth; the `fdb codegen` CLI generates equivalent TypeScript schemas automatically
- **Provider-friendly schema identity** — future `fastdb.schema.v1` descriptors should let runtimes such as C-Two treat fastdb as an opaque payload codec family rather than as a service IDL

## Documentation map

- **Python binding (`fastdb4py`)**: see [`python/README.md`](python/README.md)
- **TypeScript binding (`fastdb4ts`)**: see [`ts/README.md`](ts/README.md)
- **C++ core (`fastcarto/fastdb`)**: see [`fastcarto/README.md`](fastcarto/README.md)
- **C-Two provider architecture**: see [`docs/c-two-provider-architecture.md`](docs/c-two-provider-architecture.md)
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

## Python `ColumnEngine.truncate()` with `STR`

`fastdb4py` `ColumnEngine.truncate()` now supports UTF-8 `STR` fields in two usage tiers:

- **Default high-level path** — `tbl.fill(..., name=[...])` now routes raw strings through the native batch string-column API
- **Advanced prepacked path** — `pack_utf8_column([...]) + tbl.column.name.fill_utf8(...)`

For fixed tables, the high-level `Table.fill(...)` path batches numeric columns and `STR` payloads together. Raw string inputs are packed inside the native batch API, while numeric columns still remain NumPy-backed after publication and string columns are exposed as `StringColumn` wrappers via `table.column.<name>`. If your input already starts as Python `str` objects, prefer this default raw path; use the prepacked path only when an upstream stage already produced UTF-8 offsets/data buffers.

```python
import numpy as np
from fastdb4py import ColumnEngine, Layout, F64, STR, feature, pack_utf8_column

@feature
class Point:
    x: F64
    y: F64
    name: STR

orm = ColumnEngine.truncate([Layout(Point, 3)])
tbl = orm.table(Point)

tbl.fill(
    x=np.array([1.0, 2.0, 3.0], dtype=np.float64),
    y=np.array([4.0, 5.0, 6.0], dtype=np.float64),
    name=["a", "bb", "ccc"],
)

# If you already own pre-encoded UTF-8 buffers, use the advanced path directly:
offsets_u32, utf8_bytes_u8 = pack_utf8_column(["a", "bb", "ccc"])
tbl.column.name.fill_utf8(offsets_u32, utf8_bytes_u8)
```

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
from fastdb4py import feature, F64, STR


@feature
class Point:
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

### `fdb codegen --c-two-ts` — C-Two codec helper generator

Generate provider-owned TypeScript helpers for fastdb payload codecs referenced by a C-Two `c-two.contract.v1` descriptor:

```bash
fdb codegen --c-two-ts \
  --schema ./point.fastdb.schema.json \
  ./grid.contract.json \
  ./fastdb-c2-codecs.ts
```

The command reads only C-Two codec requirements and `fastdb.schema.v1` descriptors. It does not parse CRM methods as fastdb service definitions, does not import C-Two, and emits fastdb4ts `Feature` schema classes plus explicit codec binding stubs keyed by `schema_sha256`. Runtime `encode` / `decode` bodies currently throw until the TypeScript/WASM codec runtime is wired in, so generated helpers are honest integration placeholders rather than fake binary implementations.

## Serialization And Provider Direction

`FastSerializer` is now considered a legacy object-graph serializer. It remains in the package for existing users, benchmarks, and migration work, but new C-Two integration should be based on neutral `fastdb.schema.v1` export plus explicit ColumnEngine/ObjectEngine codec profiles instead of the old FastSerializer hybrid blob protocol.

For C-Two and other RPC runtimes, fastdb behaves as a provider-owned payload codec family: fastdb exports schema identity and adapters through `fastdb4py.schema` and `fastdb4py.c_two_provider`, while the runtime records an opaque codec reference and invokes encode/decode hooks without understanding fastdb internals. When C-Two is installed, `fastdb4py.c_two_provider.install_c_two_provider()` registers a fastdb-owned optional wrapper provider with `cc.use_codec(...)`; C-Two core still does not import fastdb. Provider codegen is also fastdb-owned through `fdb codegen --c-two-ts`, which consumes `fastdb.schema.v1` descriptors referenced by C-Two codec requirements and generates TypeScript payload helper stubs without making fastdb a CRM IDL.

## Performance Notes

| Pattern | Throughput | Notes |
|---------|-----------|-------|
| `table.column.x[:]` columnar read/write | **~100 ns** for any N | Zero-copy NumPy view, 1 SWIG call |
| `Table.fill(**cols)` | **~2 µs** per column | 1 SWIG call + memcpy per written column |
| `feature.read_all_scalars()` | **~200 ns** for 3 fields | 1 SWIG call for all scalar fields |
| `table.iter_reuse()` row access | **~350 ns/row** | Reuses Feature wrapper, no allocation |
| `for feat in table` row access | **~1.2 µs/row** | Allocates Feature wrapper per row |
| `feat.x` single field read (db-mapped) | **~420 ns** | 1 SWIG call |
| `FastSerializer.dumps/loads` (Python, legacy) | **~70 µs** (complex graph) | Retained for compatibility; not the foundation for new C-Two provider work |
| `FastSerializer.dumps/loads` (TypeScript, legacy) | **~75 µs** (complex graph) | Retained for compatibility; not the foundation for new C-Two provider work |

**Recommended patterns by use case:**

- **Bulk read/write of one field across all rows** → `table.column.x` (columnar, zero-copy)
- **Bulk fill fixed-size tables** → `ColumnEngine.truncate` + `table.fill(...)`
- **Bulk fill pre-encoded UTF-8 buffers** → `table.column.name.fill_utf8(...)`
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
| `ColumnEngine` / `ObjectEngine` / `Table` instances | ❌ No | Not designed for concurrent mutation — create separate engine instances per thread, or synchronize externally |
| SWIG C++ calls | ✅ Yes | Long-running pure C++ operations release the GIL via `%feature("threadallow")` |

### Recommended patterns for multi-threaded code

```python
import threading
import numpy as np
from fastdb4py import ColumnEngine, Layout, feature, F64


@feature
class Point:
    x: F64

# ✅ Good: each thread owns its own truncate view
def worker():
    orm = ColumnEngine.truncate([Layout(Point, 1000)])
    tbl = orm.table(Point)
    tbl.fill(x=np.arange(1000, dtype=np.float64))

# ✅ Good: shared truncate engine with read-only access after publication
shared_orm = ColumnEngine.truncate([Layout(Point, N)])
# ... fill data ...
# Multiple threads can safely read table.column.x concurrently

# ⚠️ Caution: sharing Feature instances across threads
lock = threading.Lock()
feat = Point()
feat.x = 1.0
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
