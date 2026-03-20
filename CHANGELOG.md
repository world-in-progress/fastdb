# Changelog

All notable changes to `fastdb` and its bindings are documented here.  
Each binding section tracks **unreleased / in-progress** changes for the next version of that binding.  
When a binding is released (tagged), its section is automatically copied to the GitHub Release notes and then reset.

---

<!-- BEGIN:fastdb4py -->
## fastdb4py (Python binding)

### Added
- `fdb codegen --ts <input_dir> <output_dir>` CLI command: auto-generates TypeScript `Feature` classes from Python Feature definitions, with full type mapping, cross-file import resolution, cycle detection (lazy refs), and topological ordering.
- Free-threaded Python (PEP 703) support: module-level caches (`get_class_schema`, serializer schema, ColumnAccessor) are now safe under concurrent access; CI tests against Python 3.13t.
- Thread-safety test suite (`test_free_threading.py`): 12 tests covering schema cache, serializer, ColumnAccessor, Feature instances, ORM lifecycle, and mixed-workload stress under concurrent threading.

### Fixed
- `fdb codegen --ts` no longer warns or skips when the same class name (e.g. `Point`) appears in different `.py` files. Each file is treated as an independent module; all classes are generated in their respective `.ts` files.
- `_schema.py`: `WeakKeyDictionary` reads moved fully under lock to prevent data races in free-threaded Python; `cls.__dict__` remains the lock-free fast path.
- `serializer.py`: `_CLASS_SCHEMA_CACHE` reads moved fully under lock (removed unsafe lock-free pre-check).
- `table.py`: `ColumnAccessor._name_cache` cold-path writes now protected by a per-instance lock.
- `feature.py`: `_cache` dict changed from lazy `None` to eager `{}` allocation, eliminating a race window in `_get_cache()`.

### Changed
- `setup.py` now auto-detects `Py_GIL_DISABLED` and passes the flag to the CMake build.
- CI matrix expanded from Python 3.12-only to include Python 3.13t (free-threaded, `continue-on-error`).
- SWIG interface: long-running pure C++ operations (`load`, `post`, `save`, `truncate`, `getFieldsAsDoubles`, `setFieldsFromDoubles`) now release the GIL via `%feature("threadallow")`; `copy_to_buffer` releases GIL during `memcpy`.
- Build system: `pyproject.toml` now requires `swig>=4.4` (was `>=4.0`); CMakeLists.txt passes `-nogil` to SWIG when building on free-threaded Python, enabling automatic `Py_MOD_GIL_NOT_USED` declaration.

### Historical releases (pre-CHANGELOG)

- **2026-03-17 (Release 0.1.13)**: Added batch scalar field API (`read_all_scalars` / `write_all_scalars`) with up to 12× speedup over per-field access.
- **2026-02-28 (Release Improvement)**: Fix bugs related to build process in Windows. (PR #20)
- **2025-12-31 (Bug Fix)**: Fixed an issue where shared memory segments were not being properly unregistered from the resource tracker upon closing, which could lead to resource leaks. (PR #17)
- **2025-12-15 (Release Improvement)**: Enabled distribution of pre-compiled binary wheels for macOS (Intel/Apple Silicon) and Linux (x86_64/aarch64), eliminating the need for local compilation tools during installation. (PR #15)
- **2025-12-10 (Bug Fix)**: Fixed the data type mapping for `U32` fields in Python bindings to ensure correct representation as unsigned 32-bit integers in NumPy arrays. (PR #13)
- **2025-12-10 (Performance Improvement)**: Modified `ORM.truncate()` to support directly allocating features without initializing them for performance consideration. Note that this change may have side effects; please test thoroughly. (PR #11)
<!-- END:fastdb4py -->

---

<!-- BEGIN:fastdb4ts -->
## fastdb4ts (TypeScript/WASM binding)

### Historical releases (pre-CHANGELOG)

- **2026-03-18 (fastdb4ts rollout)**: Added the `fastdb4ts` TypeScript/WebAssembly binding, including isolated Emscripten build infrastructure, Embind bridge layer, ORM/table/column APIs, graph serializer support, root-level TypeScript tests, npm packaging flow, and scoped CI for Python/TS/core changes.
<!-- END:fastdb4ts -->

---

<!-- BEGIN:fastdb-core -->
## fastdb C++ core

### Changed
- SWIG interface (`fastdb4py.i`): added `%feature("threadallow")` for pure C++ operations and `Py_BEGIN_ALLOW_THREADS` around `copy_to_buffer` memcpy, improving multi-threaded throughput and preparing for free-threaded Python.
- CMakeLists.txt: SWIG invocation now conditionally passes `-nogil` flag when `Py_GIL_DISABLED` is set, leveraging SWIG 4.4+ native free-threading support.

### Historical releases (pre-CHANGELOG)

- **2026-03-18 (Documentation refresh)**: Reorganized binding-specific documentation into dedicated subdirectory READMEs. The root README now focuses on project-level overview, while detailed Python, TypeScript/WASM, and C++ core documentation lives in `python/README.md`, `ts/README.md`, and `fastcarto/README.md`.
- **2026-03-17 (Release 0.1.13)**: Fixed two C++ correctness bugs in the batch field read/write API: (1) `getFieldsAsDoubles` now correctly handles U8/U16/U32/I32 fields (previously returned NAN); (2) `set_field_value_t` now correctly writes U16N normalized fields (missing `memcpy` caused silent data loss).
- **2026-03-04 (Release 0.1.12)**: Fixed a critical issue where loading large database files (> 2GB) on Linux/Unix systems would fail to read the complete file, leading to missing tables or data corruption. The file reading logic has been improved to correctly handle partial reads for large files. (PR #23)
- **2026-03-04 (Memory Overflow Improvement)**: Enhanced the `MemoryStream` implementation to handle large data sizes exceeding 4GB without causing size overflow in `chunk_data_t.size` (u32). This improvement allows for more robust handling of large datasets in memory. (PR #22)
- **2025-12-10 (Bug Fix)**: Fixed an out-of-bounds access issue in `FastVectorDbLayer::Impl::getFieldOffset()` when the field index is equal to the field count. (PR #12)
<!-- END:fastdb-core -->
