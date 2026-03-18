# fastdb Development Guide

## Overview

fastdb is a high-performance columnar storage library with two language bindings that sit on top of a shared C++ core:

```
┌─────────────────────┐  ┌──────────────────────────┐
│   fastdb4py         │  │   fastdb4ts               │
│   (Python binding)  │  │   (TypeScript/WASM binding│
│   pip install       │  │   npm install fastdb4ts)  │
└────────┬────────────┘  └────────────┬─────────────┘
         │ SWIG                        │ Emscripten/Embind
         └──────────────┬─────────────┘
                        │
           ┌────────────▼────────────┐
           │   C++ Core              │
           │   fastcarto/fastdb/     │
           │   wx namespace, pimpl   │
           └─────────────────────────┘
```

Both bindings share the same binary wire format, enabling seamless data exchange between Python (server) and TypeScript (browser).

---

## C++ Core (`fastcarto/fastdb/`)

The C++ core is the foundation. Neither binding modifies it for language-specific purposes.

### Key classes (`wx` namespace)

| C++ class | Role |
|---|---|
| `FastVectorDb` | Immutable read-only database |
| `FastVectorDbBuild` | Write-phase builder for database |
| `FastVectorDbLayer` | Immutable read-only table (layer) |
| `FastVectorDbLayerBuild` | Write-phase builder for a table |
| `FastVectorDbFeature` | Single row accessor |
| `MemoryStream` | In-memory byte buffer |
| `chunk_data_t` | Contiguous column data block |

### Design patterns

- **Pimpl pattern**: All public classes use opaque `Impl*` pointers for ABI stability. Never access `impl_` directly from bindings.
- **Builder / immutable split**: Write via `*Build` classes, then call `post()` to get the immutable read-only counterpart.
- **All classes in `wx` namespace**; public headers in `fastcarto/fastdb/include/`, implementation in `fastcarto/fastdb/src/`.
- **C++17 required**; CMake >= 3.16.

### Build (C++ only)

The C++ core is never built standalone — it is always built as part of one of the two binding build flows below.

### Modification workflow

1. Edit `fastcarto/fastdb/src/` and `fastcarto/fastdb/include/`.
2. Rebuild the binding that uses it (Python or WASM — see below).
3. If the change affects `chunk_data_t` layout or any wire-format struct, update `fastdb-config.h` and verify both bindings still interoperate.

> **Wire-format note**: structs used in the serialized buffer must use fixed-width types (`u32`, `u64`, etc.), not `size_t`. `size_t` is 8 bytes in native C++ but 4 bytes in WASM — using it in wire structs breaks cross-language compatibility.

---

## Python Binding (`python/fastdb4py/`)

### Layer stack

```
python/fastdb4py/
├── type.py          TypeVar aliases → OriginFieldType enum
├── feature/
│   ├── feature.py   Feature base class (__getattr__/__setattr__ dispatch)
│   └── utils.py     parse_defns() / get_all_defns()
├── orm/
│   ├── __init__.py  ORM lifecycle (create/truncate/load/push/share/save/close)
│   └── table.py     Table[T] + ColumnAccessor + StridedColumn
├── serializer.py    FastSerializer (binary object graph serialization)
└── core/            SWIG-generated native bindings — DO NOT EDIT MANUALLY
```

### Build & test

```bash
# Build C++ core + SWIG bindings
./py_utils.sh --build          # or: uv pip install -e .

# Run all Python tests
uv run pytest

# Single test file / function
uv run pytest tests/python/test_column_way.py
uv run pytest tests/python/test_column_way.py::test_basic_column_access

# Benchmark
uv run python tests/python/benchmark_comprehensive.py --quick
```

Build requires: C++17 compiler, CMake >= 3.16, SWIG >= 4.0, NumPy.

### Key patterns

**Feature definition** — subclass `Feature` with TypeVar-aliased fields:
```python
from fastdb4py import Feature, F64, U32, STR

class Point(Feature):
    x: F64
    y: F64
    label: STR
```

**Two-mode Feature objects**:
- Pure Python mode (`_origin is None`): reads/writes go to `_cache` dict.
- DB-mapped mode (`_origin` set): reads/writes dispatch to C++ getters/setters via SWIG.
- `Feature.fixed` property distinguishes modes.

**Field dispatch**: `parse_defns()` introspects annotations to build `(OriginFieldType, field_index)` mapping stored in `_origin_hints` for O(1) lookup during `__getattr__`/`__setattr__`.

**Zero-copy NumPy columns**: `table.column.x` returns a NumPy array backed by C++ memory via `__array_interface__` on `chunk_data_t`. `ColumnAccessor` dynamically creates accessors matching the Feature subclass fields.

**Thread-safe caching**: Feature hints, field definitions, class schemas, and column accessors all use `WeakKeyDictionary` + `Lock`.

**ORM lifecycle**:
```python
# Fixed-size (fastest)
orm = ORM.truncate([TableDefn(Point, 1000)])
tbl = orm.get(Point)

# Dynamic append
orm = ORM.create()
orm.push(Point(x=1.0, y=2.0))
orm._combine()

# Shared memory IPC
orm.share("my_db")          # publish to POSIX shared memory
orm2 = ORM.load("my_db")   # zero-copy cross-process access
orm.unlink("my_db")         # release segment
```

### SWIG interface

- `fastcarto/fastdb/swig/fastdb4py.i` — single interface file, Python-only.
- `python/fastdb4py/core/` — SWIG-generated output, never edit manually.
- Renames C++ `FastVectorDb` → Python `WxDatabase` etc. for Pythonic naming.
- NumPy zero-copy via `%array_interface` on `chunk_data_t`.

### Testing

Key test files in `tests/python/`:
- `test_column_way.py` — ORM.truncate + columnar NumPy access
- `test_shared_memory.py` — ORM create/push/share/load across processes
- `test_truncate_block.py` — Truncate block operations
- `test_fast_serializer.py` — FastSerializer (nested objects, cyclic refs, tree structures)

### Performance optimization

Tracked incrementally in `optimize/`. Each round: one logical change → rebuild → benchmark → write `optimize/oN/benchmark.md` with `## Changes` + `## Expected Improvement` sections + full output. Compare against previous round, update `plan.md`.

Key targets: `orm/table.py` (ColumnAccessor), `feature/feature.py` (`__getattr__`/`__setattr__`), `orm/__init__.py` (lifecycle), `swig/fastdb4py.i` (requires recompile).

---

## TypeScript/WASM Binding (`ts/fastdb4ts/`)

The TypeScript binding compiles the same C++ core to WebAssembly via Emscripten + Embind, then wraps it in a TypeScript ORM that mirrors the Python API. Targets browser environments only (no Node file I/O, no shared memory IPC).

### Directory layout

```
ts/
├── embind/
│   ├── CMakeLists.txt      WASM-only CMake entry (fails if not Emscripten)
│   └── fastdb4ts.cpp       All C++↔WASM glue (EMSCRIPTEN_BINDINGS)
├── fastdb4ts/              npm package (published as fastdb4ts)
│   ├── src/
│   │   ├── types.ts        Field type constants (U8, F64, STR, …)
│   │   ├── feature.ts      Feature base class + Proxy dispatch + defineSchema
│   │   ├── orm.ts          ORM.truncate / create / fromBuffer / toBuffer
│   │   ├── table.ts        Table<T> with get(i) and Symbol.iterator
│   │   ├── column.ts       StridedColumn (.get/.set/.fill/.toArray)
│   │   ├── serializer.ts   FastSerializer.dumps / loads
│   │   ├── wasm-loader.ts  loadFastdbWasm() singleton promise
│   │   └── wasm/           Emscripten output — DO NOT EDIT MANUALLY
│   │       ├── fastdb4ts.js
│   │       └── fastdb4ts.wasm
│   ├── package.json        version: 0.0.1, publishConfig.access: public
│   └── tsconfig.json
├── tests/                  Node-based TS tests (tsx runner)
│   ├── test-orm.ts
│   ├── test-serializer.ts
│   └── test-serializer-interop.ts
└── build-wasm.sh           Emscripten WASM build script
```

### Build & test

```bash
# 1. Build WASM (requires Emscripten emsdk activated)
bash ts/build-wasm.sh

# 2. Build TypeScript
npm --prefix ts/fastdb4ts run build

# Run TypeScript unit tests
npm --prefix ts/fastdb4ts run test:ts

# Run serializer tests
npm --prefix ts/fastdb4ts run test:serializer

# Run cross-language interop tests (Python ↔ TS)
npm --prefix ts/fastdb4ts run test:serializer:interop
```

### Key patterns

**Schema definition** — static `defineSchema` field (no decorators required):
```typescript
import { Feature, defineSchema, F64, U32, STR } from 'fastdb4ts';

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64, label: STR });
  x!: number;
  y!: number;
  label!: string;
}
```

**Feature Proxy dispatch**: `new Feature({…})` returns a `Proxy` (constructor calls `wrapFeature(this)` and returns it). All field access:
- Pure-TS mode (`_origin === null`): reads/writes go to `_cache`.
- DB-mapped mode (`_origin` set): reads/writes dispatch to WASM getters/setters.

**StridedColumn API** — use `.get(i)` / `.set(i, v)` / `.fill(array)` / `.toArray()`. Array-index syntax (`col[i] = v`) does NOT work:
```typescript
const col = orm.table(Point).column.x;  // StridedColumn<Float64Array>
col.set(0, 3.14);
col.fill(new Float64Array([1, 2, 3]));
const arr = col.toArray();  // copies to new Float64Array
```

**ORM lifecycle**:
```typescript
// Fixed-size (fastest)
const orm = await ORM.truncate([new TableDefn(Point, 1000)]);
const tbl = orm.table(Point);

// Dynamic append
const orm2 = await ORM.create();
orm2.push(new Point({ x: 1, y: 2 }));
orm2.combine();

// Round-trip buffer (e.g. received from Python backend)
const buf = orm.toBuffer();
const orm3 = await ORM.fromBuffer(buf);
```

**FastSerializer** for complex object graphs with nested/cyclic refs:
```typescript
import { FastSerializer, listOf, ref } from 'fastdb4ts';

class Node extends Feature {
  static schema = defineSchema({ val: F64, children: listOf(ref(Node)) });
}
const buf = FastSerializer.dumps(root);
const loaded = FastSerializer.loads(buf, Node);
```

### Embind isolation rule

All C++↔WASM bindings must live exclusively in `ts/embind/fastdb4ts.cpp`. The `fastcarto/` directory must NOT be modified to add TS-specific code. The embind CMake entry uses `add_subdirectory` to pull in `fastcarto/` as a static library without touching its sources.

### WASM memory notes

- Do NOT hold raw WASM typed-array views across async boundaries — WASM memory can grow and invalidate views.
- `StridedColumn` reads `HEAPF64`/`HEAP32` on each `.get(i)` call; views are not cached.
- Embind objects must call `.delete()` when done; the TS ORM wrappers manage lifetimes internally.

---

## FastSerializer Protocol (shared)

The binary format is identical between Python and TypeScript, enabling direct cross-language data exchange.

- Scalar fields (`U8`–`F64`) → columnar fastdb storage
- Numeric lists (`List[U32]`, `List[F64]`) → dedicated auxiliary layers named `__fastser_list__|ClassName|FieldName|type`
- Complex fields (nested lists, refs, bytes) → geometry-like raw blob
- Object refs → encoded as `[layer_idx: u16][feature_idx: u32]`
- Cyclic references → identity preserved via two-pass traversal (first pass assigns IDs, second pass writes)

When modifying the serializer in either language, always verify the cross-language interop tests pass.

---

## CI / GitHub Actions

`.github/workflows/tests.yml` uses `dorny/paths-filter@v3` to scope jobs:

| Changed paths | Jobs that run |
|---|---|
| `fastcarto/` | Python tests |
| `python/` | Python tests |
| `ts/` | TypeScript tests |
| `.github/workflows/` | All tests |

A terminal aggregate job named `test` (with `if: always()`) satisfies the branch protection required status check regardless of which scoped jobs ran.

**npm publish** (`.github/workflows/npm_publish.yml`): triggered by git tags matching `npm/v*`. Checks npm registry first (skips if version exists), builds WASM + TS, tests, packs, then publishes via npm OIDC Trusted Publishing (no `NPM_TOKEN` secret). Git tag is created after successful publish, not before. First publish of a new package name must be done manually.

---

## Build System

| Layer | Build tool | Output |
|---|---|---|
| C++ core | CMake (via `setup.py` or embind CMakeLists) | `.so` / `.a` / `.wasm` |
| Python binding | `pyproject.toml` + `setup.py` → CMake + SWIG | `python/fastdb4py/core/` (auto-generated) |
| WASM | `ts/build-wasm.sh` → `emcmake cmake ts/embind` | `ts/fastdb4ts/src/wasm/` (auto-generated) |
| TypeScript | `npm run build` (tsc) | `ts/fastdb4ts/dist/` |

- Python parallel builds limited to 2 jobs to avoid OOM.
- macOS: `ARCHFLAGS` set in `setup.py`; Windows: x64 config.
- UV cache at `.uv_cache` in project root for bind-mount hardlinking.

## Development Environment

Supports DevContainer (see `.devcontainer/devcontainer.example.json`). Requires Docker/Podman + VSCode DevContainer extension.
