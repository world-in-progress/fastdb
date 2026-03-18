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

## What's new

- **2026-03-18 (Documentation refresh)**: Reorganized binding-specific documentation into dedicated subdirectory READMEs. The root README now focuses on project-level overview, while detailed Python, TypeScript/WASM, and C++ core documentation lives in `python/README.md`, `ts/README.md`, and `fastcarto/README.md`.
- **2026-03-18 (fastdb4ts rollout)**: Added the `fastdb4ts` TypeScript/WebAssembly binding, including isolated Emscripten build infrastructure, Embind bridge layer, ORM/table/column APIs, graph serializer support, root-level TypeScript tests, npm packaging flow, and scoped CI for Python/TS/core changes.
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
