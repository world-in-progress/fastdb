# fastdb - CLAUDE.md

## Project Overview

fastdb (`fastdb4py` on PyPI) is a C++ local database library with Python bindings (via SWIG). It provides fast, lightweight columnar data storage aimed at RPC and coupled modeling in scientific computing. Current version: **0.1.13**, requires Python >= 3.10.

## Repository Structure

```
fastdb/
├── fastcarto/                    # C++ core library
│   ├── fastdb/
│   │   ├── include/              # Public C++ headers (fastdb.h, fastdb-config.h)
│   │   ├── src/                  # C++ implementation (pimpl pattern)
│   │   ├── swig/                 # SWIG interface files (.i)
│   │   └── CMakeLists.txt
│   ├── lib/                      # Third-party C/C++ libs (clipper, gaiageo, gpc)
│   ├── dump-fastdb/              # CLI: dump/inspect .fastdb files
│   ├── make-fastdb/              # CLI: create .fastdb files
│   └── CMakeLists.txt
├── python/
│   └── fastdb4py/                # Python package
│       ├── __init__.py           # Public API exports
│       ├── type.py               # Field type system (TypeVar aliases, OriginFieldType enum)
│       ├── feature/
│       │   ├── base.py           # BaseFeature (ABCMeta)
│       │   ├── feature.py        # Feature class (core data model with __getattr__/__setattr__ dispatch)
│       │   └── utils.py          # parse_defns / get_all_defns (type hint introspection)
│       ├── orm/
│       │   ├── __init__.py       # ORM, Table, TableDefn, TableBuilder
│       │   └── table.py          # Table[T] with column accessor and iteration
│       ├── serializer.py         # FastSerializer (binary serialization for Feature object graphs)
│       └── core/                 # SWIG-generated native bindings (auto-generated, do not edit)
├── tests/python/                 # Python test suite
├── examples/python/              # Python usage examples
├── pyproject.toml                # Python build config
├── setup.py                      # CMake + SWIG build integration
└── py_utils.sh                   # Dev helper: --clean, --build, --test
```

## Architecture

### Layer Stack (bottom-up)

1. **C++ Core** (`fastcarto/fastdb/`) — Columnar storage engine with pimpl pattern. Key classes in `wx` namespace: `FastVectorDb`, `FastVectorDbBuild`, `FastVectorDbLayer`, `FastVectorDbLayerBuild`, `FastVectorDbFeature`, `MemoryStream`.
2. **SWIG Bindings** (`fastcarto/fastdb/swig/fastdb4py.i`) — Generates Python wrappers with NumPy C-API integration, zero-copy `__array_interface__`, and Pythonic renames (e.g. `FastVectorDb` → `WxDatabase`).
3. **Type System** (`python/fastdb4py/type.py`) — `TypeVar` aliases (`U8`, `U16`, `U32`, `I32`, `F32`, `F64`, `STR`, `WSTR`, `REF`, `BYTES`, `BOOL`) mapping to `OriginFieldType` enum values.
4. **Feature** (`python/fastdb4py/feature/feature.py`) — Core ORM model. Users subclass `Feature` with type-annotated fields. `__getattr__`/`__setattr__` dispatches to C++ column storage when database-mapped (`_origin` is set), or to a Python `_cache` dict otherwise.
5. **ORM** (`python/fastdb4py/orm/__init__.py`) — Database lifecycle: `ORM.create()`, `ORM.truncate()`, `ORM.load()`, `ORM.push()`, `ORM.get()`, `ORM.share()`, `ORM.save()`, `ORM.close()`.
6. **FastSerializer** (`python/fastdb4py/serializer.py`) — High-performance binary serializer for Feature object graphs. Supports nested types, cyclic references, and numeric list columns.

### Builder Pattern

Database creation uses separate Build classes:
- **Write phase**: `WxDatabaseBuild` / `WxLayerTableBuild` — define schema, add fields, push features.
- **Read phase**: After `post()` or `save()` + `load()`, produces immutable `WxDatabase` / `WxLayerTable`.
- `ORM.truncate()` pre-allocates fixed-size tables, then calls `_combine()` to serialize and reload as an immutable database.

### Zero-Copy NumPy Integration

Column data is exposed directly via `__array_interface__` on `chunk_data_t`. Access pattern:
```python
table.column.x  # returns a numpy array (zero-copy view of C++ memory)
```
The `ColumnAccessor` class (in `table.py`) dynamically creates accessors matching the Feature subclass fields.

### Shared Memory IPC

`ORM.share(shm_name)` publishes the entire database buffer to POSIX/Windows shared memory. Another process can `ORM.load(shm_name)` for zero-copy cross-process access.

### FastSerializer Protocol

Hybrid storage: scalar fields → columnar, complex fields (list/ref/bytes) → geometry-like raw blob, numeric lists (`List[U32]`/`List[F64]`) → dedicated auxiliary layers. Object refs encoded as `[layer_idx:u16][feature_idx:u32]`.

## Key Patterns to Follow

- **Thread-safe caching**: Feature hints, field definitions, class schemas, and column accessors all use `WeakKeyDictionary` + `Lock` for thread-safe caching.
- **Pimpl in C++**: All C++ classes use opaque `Impl*` pointers for ABI stability.
- **Field dispatch via type hints**: `parse_defns()` introspects field annotations (`F64`, `U32`, etc.) to build the `(OriginFieldType, field_index)` mapping used at runtime.
- **Two-mode Feature objects**: `Feature.fixed` property distinguishes between pure Python mode (`_origin is None`, reads/writes go to `_cache`) and database-mapped mode (`_origin` set, reads/writes dispatch to C++ getters/setters).

## Build & Development

```bash
# Build C++ core + Python bindings
./py_utils.sh --build

# Run tests
./py_utils.sh --test

# Clean build artifacts
./py_utils.sh --clean
```

Build requires: C++17 compiler, CMake >= 3.16, SWIG >= 4.0, NumPy.

## Testing

Tests are in `tests/python/` and run with pytest:
```bash
uv run pytest
```

Key test files:
- `test_column_way.py` — ORM.truncate + columnar numpy access
- `test_shared_memory.py` — ORM create/push/share/load across processes
- `test_truncate_block.py` — Truncate block operations
- `test_fast_serializer.py` — FastSerializer (nested objects, cyclic refs, tree structures, numeric lists)

## Public Python API

```python
import fastdb4py

# Types: BOOL, U8, U16, U32, I32, U8N, U16N, F32, F64, STR, WSTR, REF, BYTES
# Classes: Feature, ORM, Table, TableDefn, FastSerializer
```

## Performance Optimization Workflow

This project follows an incremental optimization process. Each round of optimization is tracked in the `optimize/` directory:

```
optimize/
├── o0/benchmark.md   ← original baseline (no changes)
├── o1/benchmark.md   ← after first optimization
├── o2/benchmark.md   ← after second optimization
└── ...
```

### Rules for each optimization round

1. **Make code changes** according to `plan.md` (one logical optimization per round).
2. **Rebuild** if C++ or SWIG files were modified: `~/.local/bin/uv pip install -e .`
3. **Run the benchmark**: `~/.local/bin/uv run python tests/python/benchmark_comprehensive.py --quick`
4. **Create a new folder** `optimize/oN/` where N = previous highest + 1.
5. **Write results** to `optimize/oN/benchmark.md`. The file MUST begin with:
   - A `## Changes` section listing every modified file and a concise explanation of what was changed and why.
   - A `## Expected Improvement` section stating what metric should improve and by how much.
   - The full benchmark output table(s) below.
6. **Compare** the new benchmark against `optimize/o(N-1)/benchmark.md` and note the delta.
7. **Update `plan.md`** to mark the completed optimization and record actual vs. expected gains.

### Key files for optimization context

- `plan.md` — optimization strategy, priority list, and progress tracker
- `optimize/o0/benchmark.md` — original baseline numbers
- `tests/python/benchmark_comprehensive.py` — the benchmark tool (covers micro/meso/macro/serializer)
- `python/fastdb4py/orm/table.py` — ColumnAccessor (OPT-1 target)
- `python/fastdb4py/feature/feature.py` — __getattr__/__setattr__ dispatch (OPT-2, OPT-3 targets)
- `python/fastdb4py/orm/__init__.py` — ORM lifecycle (OPT-5 target)
- `fastcarto/fastdb/swig/fastdb4py.i` — SWIG interface (OPT-7 target, requires recompile)
