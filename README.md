# fastdb

[![PyPI version](https://badge.fury.io/py/fastdb4py.svg)](https://badge.fury.io/py/fastdb4py)
[![npm version](https://badge.fury.io/js/fastdb4ts.svg)](https://badge.fury.io/js/fastdb4ts)
[![Run Tests](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml/badge.svg)](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml)

FastDB is an embedded C++ data layer for scientific computing and RPC payload
work. It stores typed record tables and object graphs in a compact binary
format, and defines `fastdb.payload.v1`, a portable payload format with
canonical identity (RFC 8785 canonical JSON plus SHA-256) and a deterministic
binary layout, so the same data means the same bytes in every language.

FastDB is an in-process library, not a database server: there is no network
protocol, query planner, or SQL surface. One C++ core owns portable-payload
schema compilation, canonicalization, binary layout, and lifetime semantics.
The Rust, Python, and TypeScript/WASM portable APIs project that core through
a stable C ABI.

## What you can do with it

- **Typed record tables** — declare plain classes with typed fields
  (`U32`, `F64`, `STR`, ...), fill fixed-size tables in bulk, and read them
  back row-wise or column-wise. In Python, numeric columns are exposed as
  NumPy arrays over native storage.
- **Object graphs** — features can reference other features across tables,
  including shared and cyclic references, with an explicit
  ownership/invalidation model for views over native memory.
- **Portable payload exchange** — compile a payload specification once and
  get canonical identity, validated binary payloads, checked views,
  materialization, and stable structured errors, identically in C++, Rust,
  Python, and TypeScript/WASM. The core can also generate ready-to-use
  payload code for all four languages.
- **Compact binary persistence** — save and load databases as files or
  buffers; the Python binding additionally supports shared-memory handoff
  between processes.

## Packages

| Language | Package | Install source |
| --- | --- | --- |
| C++ (core) | Core/C ABI bundle (`fastdb_payload.h`) | [GitHub releases](https://github.com/world-in-progress/fastdb/releases) |
| Rust | [`fastdb`](https://crates.io/crates/fastdb) (+ raw [`fastdb-sys`](https://crates.io/crates/fastdb-sys)) | crates.io + matching Core bundle |
| Python 3.10+ | [`fastdb4py`](https://pypi.org/project/fastdb4py/) | PyPI |
| TypeScript / WASM | [`fastdb4ts`](https://www.npmjs.com/package/fastdb4ts) | npm |

The Rust, Python, and TypeScript portable APIs are thin projections of one core;
none contains an independent payload parser, layout, or digest implementation
([ADR-0001](docs/decisions/0001-portable-payload-core-authority.md)).

## Installation

**Python** (from [PyPI](https://pypi.org/project/fastdb4py/)):

```bash
uv pip install fastdb4py   # or: pip install fastdb4py
```

**TypeScript** (from [npm](https://www.npmjs.com/package/fastdb4ts)):

```bash
npm install fastdb4ts
```

**Rust** — the safe [`fastdb`](bindings/rust/fastdb/README.md) crate links
against a native Core bundle; it is not a pure-Rust crate. Download the
Core bundle matching your crate version from the GitHub release page, then
set `FASTDB_PAYLOAD_LINK_MODE=system` and set
`FASTDB_PAYLOAD_SYSTEM_LIB_DIR` to the absolute extracted `lib` directory. See the
[linking contract](bindings/rust/fastdb-sys/README.md) for details.

**C++** — use a Core bundle from a tagged release, or build from source with
CMake (see [fastcarto/README.md](fastcarto/README.md)).

Platform coverage follows the native Core bundles attached to each release;
the [0.2.0 release record](docs/releases/0.2.0.md) lists the platforms
verified so far, and additional platforms (including Windows) are tracked in
the [0.2.1 preparation record](docs/releases/0.2.1.md) and
[Issue 0001](docs/issues/0001-portable-payload-deferred-capabilities.md).
For platforms without a published wheel or Core bundle, Python builds from
source need a C++17 compiler, CMake, SWIG, and NumPy.

## Quick start (Python)

```python
import numpy as np
from fastdb4py import RecordEngine, Layout, feature, F64, U32, STR


@feature
class Sample:
    station: U32
    temperature: F64
    label: STR


db = RecordEngine.truncate([Layout(Sample, 3)])
table = db.table(Sample)

table.fill(
    station=np.array([101, 102, 103], dtype=np.uint32),
    temperature=np.array([12.5, 14.8, 9.3]),
    label=["north", "south", "ridge"],
)

# Row readback: fields read straight from native storage.
print(table[1].station, table[1].temperature, table[1].label)
# 102 14.8 south

# Column readback: numeric columns are NumPy arrays over native storage,
# string columns are StringColumn wrappers.
print(table.column.temperature.mean())
print(table.column.label.to_pylist())

# Compact binary persistence.
db.save("samples")
reloaded = RecordEngine.load("samples", from_file=True)
print(reloaded.table(Sample).column.temperature[:])
```

The Python binding also offers `ObjectEngine` for dynamically growing tables
and reference-heavy graphs, and `fdb codegen` for generating payload code
from a portable specification. See
[`python/README.md`](python/README.md) for the full API.

## Portable payloads and standalone engines

FastDB exposes two complementary APIs:

- **The portable payload API** (`fastdb4py.payload`, Rust `fastdb`,
  `fastdb4ts/payload`, C++ `fastdb_payload.h`/`fastdb_payload.hpp`) is the
  cross-language exchange path. A specification is compiled once by the core
  into an immutable `CompiledSpec` with a SHA-256 identity; payloads built
  from it have one validated binary meaning (`fastdb.payload.bin.v1`) across
  languages, with checked views, invalidation, detached materialization, and
  deterministic C++/Rust/Python/TypeScript code generation.
- **The standalone engines** are the in-process storage paths. `RecordEngine`
  manages fixed-size tables of typed records (AoS layout with strided field
  access), and `ObjectEngine` manages dynamically growing tables and typed
  object graphs. These are local libraries for embedding, sharing memory
  between processes, and file persistence — not services.

`RecordEngine`/`ObjectEngine` data and portable payloads are separate
subsystems today; see the
[accepted design](docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
for the target scope and
[Issue 0001](docs/issues/0001-portable-payload-deferred-capabilities.md) for
intentionally deferred capabilities.

## Ownership and view lifetimes

Values that stay in Python (`@feature` instances, materialized copies) are
ordinary owned objects. Views into native storage (table rows, numeric and
string columns) are tied to a `FdbViewOwner`: pass
`FdbViewOwner(checked=True)` when integrating with reusable memory leases,
call `fdb.invalidate(owner_or_view)` when the lease ends so later checked
access fails loudly, and call `fdb.materialize(value)` or
`value.to_owned()` before retaining data beyond the backing buffer's
lifetime. Standalone tables stay trusted by default and return raw NumPy
column views for speed; `unsafe_numpy_view()` is an explicit escape hatch
whose raw arrays cannot be revoked after export. See
[Backed View Lifetimes](python/README.md#backed-view-lifetimes).

## Documentation

- Python binding (`fastdb4py`): [`python/README.md`](python/README.md)
- TypeScript/WASM binding (`fastdb4ts`): [`ts/fastdb4ts/README.md`](ts/fastdb4ts/README.md)
- Rust crates (`fastdb`, `fastdb-sys`): [`bindings/rust/fastdb/README.md`](bindings/rust/fastdb/README.md)
- C++ core (`fastcarto/fastdb`): [`fastcarto/README.md`](fastcarto/README.md)
- Accepted design: [portable payload foundation](docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
- Decisions and status: [`docs/decisions/`](docs/decisions/), [implementation status](docs/issues/0002-portable-payload-foundation-implementation-status.md)
- Releases: [0.2.0 record](docs/releases/0.2.0.md), [0.2.1 preparation](docs/releases/0.2.1.md), [CHANGELOG.md](CHANGELOG.md)

## Development

Build and test the layer you are changing (from the repository root):

```bash
uv run pytest tests/python -q      # Python binding tests
uv build                           # fastdb4py sdist + wheel
bash ts/build-wasm.sh              # WebAssembly module for fastdb4ts
npm --prefix ts/fastdb4ts run build
npm run test:ts                    # TypeScript tests
```

Layer requirements: Python binding — C++17 compiler, CMake ≥ 3.16, SWIG ≥ 4.4,
NumPy; TypeScript/WASM — Emscripten, Node.js, npm; core — C++17 compiler and
CMake. When C++ binary or ABI behavior changes, revalidate the C++, Rust,
Python, and TypeScript consumers together. See
[`fastcarto/README.md`](fastcarto/README.md) for native internals and
[AGENTS.md](AGENTS.md) for repository conventions.

## License

[MIT](LICENSE). Distribution archives retain the notices in
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
