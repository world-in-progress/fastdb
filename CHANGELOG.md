# Changelog

All notable changes to `fastdb` and its bindings are documented here.  
Each binding section tracks **unreleased / in-progress** changes for the next version of that binding.  
When a binding is released (tagged), its section is automatically copied to the GitHub Release notes and then reset.

---

<!-- BEGIN:fastdb4py -->
## fastdb4py (Python binding)

### Removed
- **BREAKING**: Deleted `Feature` base class, `ORM`, `ORM2`, `TableDefn`, `ClassSchema`, `FeatureRefList`, `BaseFeature`, `parse_defns`, `get_all_defns`, `get_class_schema`, `make_inlined_dispatch`, `make_batch_inlined_dispatch`, and the `orm._graph` module. Use `@feature` decorator with `ColumnEngine` (columnar/truncate workloads) or `ObjectEngine` (object-graph/serializer workloads) instead. The `feature` and `orm` subpackages now expose only the new minimal surface.

### Changed
- `@feature` decorator now accepts `np.ndarray` field annotations (handled via `FastSerializer` buffer layers).
- `FastSerializer` no longer recognises pre-v2.0 `Feature` subclasses; only `@feature`-decorated classes are accepted.
- Python table rows returned from `Table` now behave as backed `@feature` views instead of detached copies, so scalar numeric field writes can update native storage when the view is writeable.

### Added
- Generic Python call-db runtime APIs: `encode_call_db(...)`, `decode_call_db(...)`, `view_call_db(...)`, `FastdbCallDbBinding`, `FastdbCallDbTable`, scalar field and array item descriptors, retained `FastdbCallDbView`, and scalar `FastdbCallDbArrayView`. This gives Python parity with the existing generic TypeScript call-db runtime while keeping C-Two-specific CRM planning, route identity, bridge derivation, and helper generation outside FastDB.
- `Layout(..., name=...)` for constructing fixed tables with integration-defined logical table names, plus `try_export_call_db(...)` for exact single-table `Batch[Feature]` call-db exports that can reuse an existing buffer before falling back to `encode_call_db(...)`.
- `prepare_call_db(...)`, `encode_call_db_into(...)`, and `FastdbPreparedCallDb` for transport-neutral caller-provided-buffer writes. The first planned build path supports multi-slot columnar call-db payloads and imports compatible backed `Batch[Feature]` layers without per-row or per-column repack.
- `fdb codegen --ts <input_dir> <output_dir>` CLI command: auto-generates TypeScript `Feature` classes from Python Feature definitions, with full type mapping, cross-file import resolution, cycle detection (lazy refs), and topological ordering.
- `FdbViewOwner`, `FdbViewInvalidatedError`, `FdbViewWriteError`, and `fdb.invalidate(...)` for explicit backed-view lifetime management in reusable-buffer integrations.
- `fdb.materialize(...)` and `Table.to_owned()` for recursively detaching FastDB-managed table, row, column, NumPy, and bytes views before retaining data beyond a backing-buffer lifetime.
- Free-threaded Python (PEP 703) support: module-level caches (`get_class_schema`, serializer schema, ColumnAccessor) are now safe under concurrent access; CI tests against Python 3.13t.
- Thread-safety test suite (`test_free_threading.py`): 12 tests covering schema cache, serializer, ColumnAccessor, Feature instances, ORM lifecycle, and mixed-workload stress under concurrent threading.
- `FastSerializer` numpy ndarray buffer layer support (`__fastser_buf__`): numpy arrays are now serialized via dedicated fastdb layers using `memcpy`-level writes and `np.frombuffer` loads, achieving 5–8× speedup over list-based paths for large arrays. Supports float64, float32, uint32, int32, uint16, uint8 dtypes and 1D/2D/3D shapes.
- `FastSerializer.loads_shm(shm_name, length, offset, root_type)`: deserialize a Feature directly from a named shared memory segment without copying to an intermediate `bytes` object. Returns a fully detached Feature (pure Python mode) after closing the shared memory mapping.
- **Native list columns**: `List[F64]`, `List[U32]`, `List[I32]`, `List[F32]`, `List[U8]`, `List[U16]`, and `List[SomeFeature]` are now first-class column types in the ORM. Features with list fields are stored directly in shared-memory ORM layers — no `FastSerializer` needed. Accessing `feature.my_list` returns a zero-copy NumPy array (numeric) or a lazy `FeatureRefList` (refs) backed by C++ memory. Cyclic object graphs are supported via two-pass DFS writing and back-edge patching.
- `ColumnEngine.truncate()` now accepts `STR` fields, exposes them through a dedicated `StringColumn` wrapper with bulk `fill()` / `fill_utf8()` APIs, and lets fixed tables batch mixed numeric + `STR` payloads through `Table.fill(**cols)`.
- Python docs and benchmark now show the two UTF-8 truncate ingest tiers: high-level `tbl.fill(..., name=[...])` and advanced `pack_utf8_column([...]) + tbl.column.name.fill_utf8(...)`, with benchmark output split into raw-string vs prepacked paths.

### Performance
- Raw `STR` writes now route `tbl.fill(..., name=[...])` through a native string-column batch API in the C++ core, reducing Python-side UTF-8 packing overhead while preserving `pack_utf8_column(...) + fill_utf8(...)` as the advanced path.
- `FastSerializer` numeric list encoding/decoding now uses numpy instead of `struct.pack`/`struct.unpack`, yielding ~64% faster List[U32] dumps for N=10000.
- `FastSerializer` loads path: lazy initialization of auxiliary layer data, eliminated redundant schema lookups in scalar read path.
- Overall `FastSerializer` geometric mean improvement: **32%** (44.26 → 30.09 µs across all test cases).
- `FastSerializer` numeric lists (`List[F64]`, `List[U32]`, `List[I32]`) now route through dedicated `__fastser_buf__` buffer layers using `struct.pack` for dumps and `np.frombuffer` for loads, achieving ~39% speedup for complex Features.
- `FastSerializer` type discovery (`_discover_types`) is now cached per root type, eliminating redundant type-hint traversal on repeated `loads()` calls.
- `FastSerializer` buffer layer references now store absolute database layer indices, enabling O(1) direct layer access on loads (eliminates full layer scan).
- `FastSerializer.dumps()` pre-computes `ref_traversal_fields` per class schema, skipping scalar/numeric-list fields during `register()` traversal.
- Cumulative `FastSerializer` improvement on complex PointCloud benchmark: **54%** (153.93 → 70.01 µs geo-mean); loads at N=10000 is now **21× faster than pickle**.

### Fixed
- Checked FastDB table, row, numeric-column, string-column, and bytes-column views now raise after their owner is invalidated, preventing stale child views from silently surviving a released backing buffer.
- `table(..., writeable=False)` now enforces explicit read-only behavior for both row field writes and numeric column writes, even when no checked lifetime owner is provided.
- `ColumnEngine.truncate()` now preserves later tables in mixed layouts when an earlier `STR` column is still empty at combine time.
- `ColumnEngine.truncate()` fixed-table writes now route mixed numeric + `STR` `Table.fill(**cols)` batches through the retained truncate build, with upfront length validation, shared `StringColumn.fill()` / `fill_utf8()` plumbing, and no Python-side whole-database rebuild for string columns.
- SWIG/Python bindings now expose UTF-8 string-column reader APIs (`get_field_as_string_view`, `get_string_column_offsets`, `get_string_column_data`) for upcoming `ColumnEngine` string-column integration work.
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

### Performance
- `FastSerializer` dumps: TypedArray bulk write for numeric lists replaces per-element DataView calls (~15% speedup).
- `FastSerializer` dumps: pre-allocated `ByteWriter` with `ArrayBuffer` + `DataView` replaces chunked `Uint8Array[]` concatenation.
- `FastSerializer` dumps: removed unnecessary `O(n log n)` object sort — registration order already correct.
- `FastSerializer` dumps/loads: module-level `TextEncoder`/`TextDecoder` reuse eliminates repeated constructor overhead (~7% speedup).
- `FastSerializer` dumps: `register()` now uses pre-computed `refTraversalFields`, skipping non-ref fields during graph traversal.
- `FastSerializer` loads: numeric `objectCache` key `(layerIdx << 20 | featureIdx)` replaces string concatenation; `layer.name()` results cached.
- Cumulative `FastSerializer` improvement: **~25%** (99.02 → ~74.50 µs geometric mean on PointCloud benchmark with STR + U32 + F64 + listOf(F64) + listOf(U32) + listOf(STR)).

### Historical releases (pre-CHANGELOG)

- **2026-03-18 (fastdb4ts rollout)**: Added the `fastdb4ts` TypeScript/WebAssembly binding, including isolated Emscripten build infrastructure, Embind bridge layer, ORM/table/column APIs, graph serializer support, root-level TypeScript tests, npm packaging flow, and scoped CI for Python/TS/core changes.
<!-- END:fastdb4ts -->

---

<!-- BEGIN:fastdb-core -->
## fastdb C++ core

### Added
- **Native list column support**: `ftList=12` field type with backwards-compatible wire format. New public API on `FastVectorDbLayerBuild`: `add_list_field(name, element_type)`, `set_field_list_numeric(idx, data, nbytes)`, `set_field_list_refs(idx, refs, count)`, `update_feature_ref(feature_idx, field_idx, ref)`, `update_list_ref_at(feature_idx, field_idx, list_idx, ref)`. New public API on `FastVectorDbFeature`: `getFieldAsListView(idx)`, `getFieldListSize(idx)`, `getFieldListRefAt(idx, list_idx)`. Wire format: `element_type` field added in 2 previously-unused padding bytes of `field_desc_ex_t`; `n_list_fields` in `layer_header_t`; list data section appended after wstrings in each layer binary. Fully backwards-compatible — old databases with zero-filled padding read as no list data.

### Fixed
- `FastVectorDbLayer` now reads varlen UTF-8 string columns (`ftSTR + size=0`) from trailing offsets/data sections and exposes zero-copy reader buffers for per-row string views and whole-column offsets/data access.
- Empty truncated UTF-8 string columns now serialize valid offset tables, preventing later layers from being misaligned in multi-layer databases.

### Changed
- SWIG interface (`fastdb4py.i`): added `%feature("threadallow")` for pure C++ operations and `Py_BEGIN_ALLOW_THREADS` around `copy_to_buffer` memcpy, improving multi-threaded throughput and preparing for free-threaded Python.
- CMakeLists.txt: SWIG invocation now conditionally passes `-nogil` flag when `Py_GIL_DISABLED` is set, leveraging SWIG 4.4+ native free-threading support.
- `FastVectorDbFeatureRef` struct moved from private header (`FastVectorDbBuild_p.h`) to public header (`fastdb.h`) so SWIG and external consumers can access `ilayer`, `ifeature`, `ifeatureH` fields directly.

### Historical releases (pre-CHANGELOG)

- **2026-03-18 (Documentation refresh)**: Reorganized binding-specific documentation into dedicated subdirectory READMEs. The root README now focuses on project-level overview, while detailed Python, TypeScript/WASM, and C++ core documentation lives in `python/README.md`, `ts/README.md`, and `fastcarto/README.md`.
- **2026-03-17 (Release 0.1.13)**: Fixed two C++ correctness bugs in the batch field read/write API: (1) `getFieldsAsDoubles` now correctly handles U8/U16/U32/I32 fields (previously returned NAN); (2) `set_field_value_t` now correctly writes U16N normalized fields (missing `memcpy` caused silent data loss).
- **2026-03-04 (Release 0.1.12)**: Fixed a critical issue where loading large database files (> 2GB) on Linux/Unix systems would fail to read the complete file, leading to missing tables or data corruption. The file reading logic has been improved to correctly handle partial reads for large files. (PR #23)
- **2026-03-04 (Memory Overflow Improvement)**: Enhanced the `MemoryStream` implementation to handle large data sizes exceeding 4GB without causing size overflow in `chunk_data_t.size` (u32). This improvement allows for more robust handling of large datasets in memory. (PR #22)
- **2025-12-10 (Bug Fix)**: Fixed an out-of-bounds access issue in `FastVectorDbLayer::Impl::getFieldOffset()` when the field index is equal to the field count. (PR #12)
<!-- END:fastdb-core -->
