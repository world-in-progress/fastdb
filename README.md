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

## Documentation map

- **Python binding (`fastdb4py`)**: see [`python/README.md`](python/README.md)
- **TypeScript binding (`fastdb4ts`)**: see [`ts/README.md`](ts/README.md)
- **C++ core (`fastcarto/fastdb`)**: see [`fastcarto/README.md`](fastcarto/README.md)
- **TypeScript/WASM analysis docs**: see [`ts/analysis/`](ts/analysis/)

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for per-binding unreleased changes.  
For historical release notes, see the [GitHub Releases](https://github.com/world-in-progress/fastdb/releases) page.

## Installation

```bash
pip install fastdb4py
```

Pre-compiled Python wheels are provided for major platforms. For TypeScript/WASM usage and repository-local development flows, see the binding-specific guides:

- [`python/README.md`](python/README.md)
- [`ts/README.md`](ts/README.md)
- [`fastcarto/README.md`](fastcarto/README.md)

## Quick start

For a minimal end-to-end example, start with:

- [`python/README.md`](python/README.md) for `fastdb4py`
- [`ts/README.md`](ts/README.md) for `fastdb4ts`

If you are working on native internals or storage layout, start with:

- [`fastcarto/README.md`](fastcarto/README.md)

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

Common development commands from the repository root:

```bash
./py_utils.sh --clean   # remove C++ build artifacts and SWIG-generated bindings
./py_utils.sh --build   # build C++ core + Python bindings
./py_utils.sh --test    # run Python unit tests
bash ts/build-wasm.sh   # build the WebAssembly module for fastdb4ts
npm run test:ts         # run root TypeScript tests
```

Build requirements depend on the layer you are working on:

- **Python binding**: C++17 compiler, CMake >= 3.16, SWIG >= 4.0, NumPy
- **TypeScript/WASM binding**: Emscripten, Node.js, npm
- **Native core**: C++17 compiler and CMake
