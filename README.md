# fastdb

[![PyPI version](https://badge.fury.io/py/fastdb4py.svg)](https://badge.fury.io/py/fastdb4py)
[![npm version](https://badge.fury.io/js/fastdb4ts.svg)](https://badge.fury.io/js/fastdb4ts)
[![Run Tests](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml/badge.svg)](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml)

`fastdb` is a C++ local database library designed as a fast, lightweight, and easy-to-use data communication layer for RPC and coupled modeling in scientific computing.

## Portable payload status and 0.2.0 direction

The current tree implements the P1 `fastdb.payload.v1` compiler/query Core,
the P2 non-reference `record.v1` runtime, the complete ordinary P3
`object_graph.v1` Core/runtime, and the public P4 four-language artifact
generator. The pure-C boundary is now locally frozen at exactly 117
`fdb_payload_v1_*` exports: the historical P3 boundary remains exactly 105,
and P4 adds only the reviewed three provenance guards plus nine immutable
ArtifactSet/codegen functions. The frozen P2 sub-boundary remains exactly 99;
that is exactly 99 `fdb_payload_v1_*` exports, and neither P3 nor P4 rewrites
its meaning. The combined runtime surface covers all V1
values and nullability, graph roots/refs, sharing and cycles, immutable
repeatable plans, truthful direct/staged heap and external final backing,
hardened copy/external open, checked views and scoped access, detached
reachable-closure materialization, drain-before-release invalidation, and
stable owned errors. The header-only C++17 facade projects the same C ABI
rather than defining a second parser, topology, layout, graph runtime, or
lifetime model.

Native coverage-guided targets are a sanitizer configuration, not an isolated
executable toggle. `FASTDB_BUILD_FUZZERS=ON` requires `BUILD_TESTING=ON` and
Clang or AppleClang, and enables ASan+UBSan consistently for native libraries,
ordinary tests, and the libFuzzer executable.

The P3 quality boundary includes a reviewed 16-seed record/graph binary-open
corpus, graph-aware deterministic/fuzz traversal, a 17-class executable proof
map, exact Sections 4-21 and active Stage B traceability, full Core
WebAssembly graph execution, and exact native/applicable-Wasm ABI-105 checks.
D1 is closed by the large range-write-only direct test, allocation threshold,
heap-reserve observation, staged byte-identity check, execution reports, and
source audit. The complete local gate is green. The context-owning primary
agent performed the final P3 review with no unresolved Critical, Important, or
material Minor finding; by explicit user direction it was not delegated, so
no independent/subagent review is claimed. WebAssembly pthread behavior is not
inferred from the single-thread proof; native tests plus ThreadSanitizer remain
the concurrency authority. Hosted Linux/macOS results remain pending because
this branch has not been pushed.

P4 is locally complete through Tasks 1-9. Those tasks provide safe Rust,
Python 3.10+, and official TypeScript/WASM portable projections over the
unchanged C ABI alongside the existing C++ facade. Their shared executable map
covers canonical identity,
record and graph binary bytes, complete logical values, five-field errors,
ownership/invalidation/materialization, and truthful direct/staged execution.
Rust source/system link seams, installed Python wheels, and the packed
browser-capable `fastdb4ts/payload` subpath are locally exercised. Core-owned four-language
code generation returns deterministic in-memory C++/Rust/Python/
TypeScript artifacts through every projection; a clean-tree harness compiles,
imports, type-checks, and executes all four against their official runtimes.
The closing four-shape hostile matrix covers all values, recursive lists,
keyword/generated-prefix collisions, and shared cyclic graphs. Native tests
also freeze exact per-target output ceilings and concurrent byte/path/SHA-256
determinism.
The C++ Core remains the only semantic authority; none of the bindings or
generated outputs contains a second parser, canonicalizer, digest, layout,
binary, graph, or materialization model.

P5 local clean cut is complete at exact ABI-117. Tasks 1-7 remove the duplicate
authority without aliases, expose the final `RecordEngine` name directly,
state the retained standalone boundaries, enforce the exact clean-cut policy
over tracked plus untracked/non-ignored files, and pass the fresh local
release-readiness matrix. FastDB implementation commit
`7eb74734926bd8fe911229eee9744a6dd8172487` packages that frozen owner
boundary into a seven-artifact local candidate at unchanged ABI-117 and
unchanged package versions. C-Two implementation commit
`bf6f5c950959bcd2723cf3c7bfe772c9ee91dc02` consumes those exact hashes in
isolated Rust, Python 3.10/current, and Node environments and passes its
18-row Rust/Python and 12-row generated TypeScript local matrices. The retained
FastDB manifest has SHA-256
`9a1c7c83dca16237257dcc50d5917f10d032e02ffceae1278ae331d101b69d32`;
see the [local-candidate evidence](docs/issues/evidence/README.md).

Hosted execution, versioning, publication, and official downstream package
composition remain open. Those external facts are tracked in [Issue
0002](docs/issues/0002-portable-payload-foundation-implementation-status.md).
No local result or workflow definition is represented as a hosted pass; the
new projection jobs are definitions until an authorized hosted run exists.

The source package version remains 0.1.x after local release readiness.
Previously published 0.1.x artifacts are migration inputs, not APIs to extend,
and no version change, tag, publication, or release is implied. This source
tree exposes `RecordEngine` without a compatibility alias. Capabilities
deliberately deferred beyond 0.2.0 are tracked separately in [Issue
0001](docs/issues/0001-portable-payload-deferred-capabilities.md).

- [Accepted portable payload design](docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
- [ADR-0001: Core authority and clean cut](docs/decisions/0001-portable-payload-core-authority.md)
- [Issue 0002: current implementation status](docs/issues/0002-portable-payload-foundation-implementation-status.md)
- [Issue 0001: post-0.2 deferrals](docs/issues/0001-portable-payload-deferred-capabilities.md)

This repository now contains three closely related layers:

- **C++ core** — native storage engine, binary layout, and serialization primitives
- **`fastdb4py`** — Python bindings via SWIG, with NumPy-oriented strided field access and shared-memory IPC
- **`fastdb4ts`** — TypeScript bindings via WebAssembly/Embind, focused on browser-friendly typed data access and schema-compatible table access

**Current storage strengths and accepted direction:**

- **Efficient strided field access** — the existing AoS record engine exposes fast field-oriented access without claiming Arrow-like SoA storage
- **Ref-graph support** — Features can reference other Features across tables, forming typed object graphs
- **Compact binary transport** — save/load databases as binary buffers or files; shared-memory deserialization for zero-copy IPC
- **Cross-binding consistency** — the 0.2.0 target makes the C++ Core, rather than a language binding, the semantic authority
- **Schema-driven codegen** — the target Core returns deterministic C++/Rust/Python/TypeScript payload artifacts in memory
- **Portable record, object-graph runtime, codegen, and clean cut** — P1 provides canonical identity, P2 provides the frozen record path, P3 provides the frozen C/C++ graph build/open/view/materialize/invalidate path at the historical 105-symbol boundary, P4 provides equal language projections plus Core-owned four-target codegen at the exact 117-symbol boundary, and P5 is locally complete without a version or release claim

## Documentation map

- **Python binding (`fastdb4py`)**: see [`python/README.md`](python/README.md)
- **TypeScript binding (`fastdb4ts`)**: see [`ts/README.md`](ts/README.md)
- **C++ core (`fastcarto/fastdb`)**: see [`fastcarto/README.md`](fastcarto/README.md)
- **Accepted portable payload target**: see [`docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md`](docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
- **Architecture decisions**: see [`docs/decisions/`](docs/decisions/)
- **Known intentional limitations**: see [`docs/issues/`](docs/issues/)
- **TypeScript/WASM analysis docs**: see [`ts/analysis/`](ts/analysis/)
- **Core-owned codegen CLI (`fdb codegen`)**: see [Core-owned codegen CLI](#core-owned-codegen-cli) below

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for per-binding unreleased changes. For historical release notes, see the [GitHub Releases](https://github.com/world-in-progress/fastdb/releases) page.

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

## Python Backed View Lifetimes

`fastdb4py` distinguishes owned Python `@feature` objects from backed table views. Owned objects keep normal `__dict__` read/write behavior. Backed rows, checked numeric columns, `StringColumn`, and `BytesColumn` can be tied to a `FdbViewOwner`; after `fdb.invalidate(owner_or_view)`, later checked reads or writes raise `FdbViewInvalidatedError`. Standalone FastDB tables remain trusted and return raw NumPy numeric columns by default, while integrations with reusable memory leases should pass `FdbViewOwner(checked=True, ...)` and use `fdb.materialize(...)` or `value.to_owned()` before retaining data beyond the lease. See [`python/README.md#backed-view-lifetimes`](python/README.md#backed-view-lifetimes) for the Python API details.

For safety-sensitive integrations, pass `writeable=False` to expose read-only backed rows and checked numeric columns. This blocks row field writes and column writes even when the owner itself is an unchecked trusted owner.

## Standalone Python `RecordEngine.truncate()` with `STR`

> This source tree uses the final `RecordEngine` name directly and provides no
> alias for the pre-0.2 engine name.

`fastdb4py` `RecordEngine.truncate()` now supports UTF-8 `STR` fields in two usage tiers:

- **Default high-level path** — `tbl.fill(..., name=[...])` now routes raw strings through the native batch string-column API
- **Advanced prepacked path** — `pack_utf8_column([...]) + tbl.column.name.fill_utf8(...)`

For fixed tables, the high-level `Table.fill(...)` path batches numeric columns and `STR` payloads together. Raw string inputs are packed inside the native batch API, scalar `BOOL` columns use the same explicit bool parser as mutable engine writes before bulk numeric storage, ordinary `U8` columns remain numeric casts, numeric columns still remain NumPy-backed after publication, and string columns are exposed as `StringColumn` wrappers via `table.column.<name>`. If your input already starts as Python `str` objects, prefer this default raw path; use the prepacked path only when an upstream stage already produced UTF-8 offsets/data buffers.

```python
import numpy as np
from fastdb4py import RecordEngine, Layout, F64, STR, feature, pack_utf8_column

@feature
class Point:
    x: F64
    y: F64
    name: STR

orm = RecordEngine.truncate([Layout(Point, 3)])
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

## Core-owned codegen CLI

`fastdb4py` ships `fdb codegen` as a thin filesystem facade over the
in-memory ArtifactSet returned by the C++ Core. The CLI accepts a portable
specification and one of the four official targets:

```bash
fdb codegen specification.json \
  --target rust \
  --output ./generated-fastdb
```

The output directory must not already exist. The CLI validates every
Core-returned relative path, writes a private staging tree with exclusive
file creation, and publishes the complete tree without replacement. It does
not discover Python classes, parse or normalize the specification, calculate
identity, render target source, or add downstream-domain artifacts.

## Current 0.1.x Performance Notes

| Pattern | Throughput | Notes |
|---------|-----------|-------|
| `table.column.x[:]` columnar read/write | **~100 ns** for any N | Zero-copy NumPy view, 1 SWIG call |
| `Table.fill(**cols)` | **~2 µs** per column | 1 SWIG call + memcpy per written column |
| `feature.read_all_scalars()` | **~200 ns** for 3 fields | 1 SWIG call for all scalar fields |
| `table.iter_reuse()` row access | **~350 ns/row** | Reuses Feature wrapper, no allocation |
| `for feat in table` row access | **~1.2 µs/row** | Allocates Feature wrapper per row |
| `feat.x` single field read (db-mapped) | **~420 ns** | 1 SWIG call |
| `FastSerializer.dumps/loads` (Python, legacy) | **~70 µs** (complex graph) | Retained for compatibility; not the foundation for new external RPC integration work |
| `FastSerializer.dumps/loads` (TypeScript, legacy) | **~75 µs** (complex graph) | Retained for compatibility; not the foundation for new external RPC integration work |

**Recommended patterns by use case:**

- **Bulk read/write of one field across all rows** → `table.column.x` (columnar, zero-copy)
- **Bulk fill fixed-size tables** → `RecordEngine.truncate` + `table.fill(...)`
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
| `Table` row reads (`table[i]`, iteration, `iter_reuse()`, fallback string lookup) | ✅ Yes | Per-table row materialization uses a read lock around native `tryGetFeature(...)` calls |
| `Feature` instances | ❌ No | Instance-level `_cache` dict is not synchronized — use external locking or one instance per thread |
| `RecordEngine` / `ObjectEngine` / `Table` mutation | ❌ No | Not designed for concurrent mutation — create separate engine instances per thread, or synchronize externally |
| SWIG C++ calls | ✅ Yes | Long-running pure C++ operations release the GIL via `%feature("threadallow")` |

### Recommended patterns for multi-threaded code

```python
import threading
import numpy as np
from fastdb4py import RecordEngine, Layout, feature, F64


@feature
class Point:
    x: F64

# ✅ Good: each thread owns its own truncate view
def worker():
    orm = RecordEngine.truncate([Layout(Point, 1000)])
    tbl = orm.table(Point)
    tbl.fill(x=np.arange(1000, dtype=np.float64))

# ✅ Good: shared truncate engine with read-only access after publication
shared_orm = RecordEngine.truncate([Layout(Point, N)])
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
./py_utils.sh --setup   # initial Python environment and editable native binding
uv pip install --reinstall -e .  # force a fresh editable native binding
uv sync                 # restore locked dependency versions after the rebuild
./py_utils.sh --test    # run Python unit tests
uv run pytest tests/python -q  # run the Python test suite directly
uv build             # build the fastdb4py sdist + local wheel
bash ts/build-wasm.sh   # build the WebAssembly module for fastdb4ts
npm run test:ts         # run root TypeScript tests
fdb codegen specification.json --target typescript --output generated-fastdb
```

Build requirements depend on the layer you are working on:

- **Python binding**: C++17 compiler, CMake >= 3.16, SWIG >= 4.0, NumPy
- **TypeScript/WASM binding**: Emscripten, Node.js, npm
- **Native core**: C++17 compiler and CMake

### Python release checklist

Before publishing `fastdb4py`, bump `[project].version` in `pyproject.toml`, refresh `uv.lock`, update the `fastdb4py` section in `CHANGELOG.md`, and verify that the release tag `py/v<version>` does not already exist. The PyPI workflow publishes only when `pyproject.toml` changes and the tag for that version is absent.
