# fastdb4ts

TypeScript and WebAssembly bindings for `fastdb`, intended primarily for browser-side use.

This directory is the TypeScript/WASM counterpart to `python/`. Where `fastdb4py` uses SWIG and NumPy, `fastdb4ts` uses Emscripten + Embind and TypeScript-facing wrappers that mirror the high-level concepts of the Python API.

## Goals

`fastdb4ts` is designed to preserve the useful parts of the `fastdb4py` experience while adapting them to browser and TypeScript constraints.

The current design goals are:

- **Typed schema modeling**
  - `Feature` subclasses with `defineSchema(...)`
- **Structured database access**
  - `ORM`, `TableDefn`, `Table`, and column accessors
- **Binary data exchange**
  - `Uint8Array` / `ArrayBuffer` in and out
- **Legacy graph serialization**
  - `FastSerializer` remains available for nested features and cyclic references, but new cross-runtime provider work should use neutral schema descriptors and explicit codec profiles
- **Browser-friendly deployment**
  - isolated WASM build path without forcing the Python build to depend on Emscripten

The current TypeScript binding intentionally does **not** implement:

- shared-memory IPC
- direct file persistence APIs

## Directory layout

### `embind/`

The C++/Embind bridge used only for the WebAssembly build.

Important files:

- `ts/embind/fastdb4ts.cpp`
  - the binding surface exposed to JavaScript
- `ts/embind/CMakeLists.txt`
  - Emscripten-only build entry
- `ts/embind/post.js`
  - glue customization for the generated module

### `fastdb4ts/`

The publishable TypeScript package.

Important files:

- `ts/fastdb4ts/src/types.ts`
  - field kinds such as `U32`, `F64`, `STR`, `ref(...)`, `listOf(...)`
- `ts/fastdb4ts/src/feature.ts`
  - `Feature` model, proxy-backed field access, mapped vs pure-TS modes
- `ts/fastdb4ts/src/orm.ts`
  - `ORM`, `TableDefn`, buffer import/export
- `ts/fastdb4ts/src/table.ts`
  - row access, iteration, column accessor creation
- `ts/fastdb4ts/src/column.ts`
  - `StridedColumn`
- `ts/fastdb4ts/src/serializer.ts`
  - Legacy `FastSerializer`
- `ts/fastdb4ts/src/wasm-loader.ts`
  - module initialization and typed WASM handle definitions

### `analysis/`

Design notes and architecture references collected during the WASM binding effort.

Files:

- `ts/analysis/FASTDB_WASM_ARCHITECTURE_ANALYSIS.md`
- `ts/analysis/WASM_BINDING_QUICK_REFERENCE.md`
- `ts/analysis/ANALYSIS_DELIVERABLES.md`

### `tests/`

- `tests/ts/`
  - pure TypeScript tests that do not rely on shared memory
- `ts/tests/test-serializer-interop.mjs`
  - Python ↔ TypeScript serializer compatibility check

## Build flow

The TS/WASM binding is intentionally isolated from the default Python build.

From the repository root:

```bash
bash ts/build-wasm.sh
npm --prefix ts/fastdb4ts run build
```

This will:

1. compile the C++ core for Emscripten
2. build the Embind bridge
3. generate `fastdb4ts.js` + `fastdb4ts.wasm`
4. compile the TypeScript package into `ts/fastdb4ts/dist/`

## Installation model

For development inside this repository:

```bash
npm --prefix ts/fastdb4ts install
npm --prefix ts/fastdb4ts run build
```

For publication, `ts/fastdb4ts/` is set up as the npm package directory.

## Quick start

```ts
import {
  F64,
  Feature,
  ORM,
  TableDefn,
  defineSchema,
  initFastdb,
} from 'fastdb4ts';

await initFastdb();

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64 });
}

const db = ORM.truncate([new TableDefn(Point, 3)]);
const table = db.table(Point);

table.column.x.fill([1.5, 2.5, 3.5]);
table.column.y.fill([4.5, 5.5, 6.5]);

console.log(table.get(1).x); // 2.5
```

## Schema and field types

Unlike the Python binding, the TypeScript side uses explicit schema definition rather than type annotations:

```ts
import { Feature, F64, I32, STR, defineSchema, listOf, ref } from 'fastdb4ts';

class Point extends Feature {
  static schema = defineSchema({
    x: F64,
    y: F64,
  });
}

class Line extends Feature {
  static schema = defineSchema({
    id: I32,
    name: STR,
    points: listOf(Point),
    first: ref(Point),
  });
}
```

Common field helpers:

- `U8`, `U16`, `U32`, `I32`
- `F32`, `F64`
- `STR`, `WSTR`
- `BYTES`
- `ref(TargetFeature)`
- `listOf(...)`

## ORM usage

### Fixed-size tables

```ts
class Particle extends Feature {
  static schema = defineSchema({
    x: F64,
    y: F64,
    vx: F64,
    vy: F64,
  });
}

const db = ORM.truncate([new TableDefn(Particle, 4)]);
const tbl = db.table(Particle);

tbl.column.x.fill([0.0, 1.0, 2.0, 3.0]);
tbl.column.y.fill([10.0, 20.0, 30.0, 40.0]);
tbl.column.vx.fill([0.1, 0.1, 0.2, 0.2]);
tbl.column.vy.fill([0.0, 0.0, 0.0, 0.0]);
```

### Buffer roundtrip

```ts
const bytes = db.toBuffer();
const copy = ORM.fromBuffer(bytes);
const copyTable = copy.table(Particle);
console.log(copyTable.get(2).x);
```

## Legacy serializer usage

`FastSerializer` is the legacy graph-oriented transport layer. It remains available for compatibility and migration tests, but it is not the foundation for new C-Two provider work.

It supports:

- nested features
- lists of scalar values
- lists of strings
- lists of feature references
- cyclic references
- numeric-list columnar auxiliary layers

Example:

```ts
import {
  F64,
  FastSerializer,
  Feature,
  I32,
  defineSchema,
  initFastdb,
  listOf,
} from 'fastdb4ts';

await initFastdb();

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64 });
}

class Line extends Feature {
  static schema = defineSchema({ id: I32, points: listOf(Point) });
}

const line = new Line({
  id: 42,
  points: [new Point({ x: 1.0, y: 2.0 }), new Point({ x: 3.0, y: 4.0 })],
});

const blob = FastSerializer.dumps(line);
const copy = FastSerializer.loads(blob, Line);
console.log(copy.points[1].y); // 4.0
```

Cyclic references are preserved:

```ts
class Node extends Feature {
  static schema = defineSchema({
    val: I32,
    next: ref(() => Node),
  });
}

const a = new Node({ val: 1 });
const b = new Node({ val: 2 });
a.next = b;
b.next = a;

const copy = FastSerializer.loads(FastSerializer.dumps(a), Node);
console.log(copy.next.next === copy); // true
```

## Testing

Pure TypeScript tests are intentionally kept under `tests/ts/` and do not depend on shared memory.

From the repository root:

```bash
npm run test:ts
```

Serializer compatibility with Python:

```bash
npm --prefix ts/fastdb4ts run test:serializer:interop
```

## Relationship to the Python binding

`fastdb4ts` is inspired by `fastdb4py`, but the implementation is not a direct copy.

Examples of deliberate differences:

- Python type annotations → TS `defineSchema(...)`
- NumPy array access → `StridedColumn`
- shared memory APIs → currently omitted
- file-centric load/save → `Uint8Array` / `ArrayBuffer` transport

The goal is semantic compatibility where it matters, especially in schema layout and legacy `FastSerializer` protocol behavior.

### Generating TypeScript schemas from Python

If you define your Feature classes in Python and want to keep TypeScript schemas in sync automatically, use the `fdb codegen` CLI shipped with `fastdb4py`:

```bash
pip install fastdb4py  # or: uv pip install -e . from repo root
fdb codegen --ts ./python_features/ ./ts_features/
```

This generates one `.ts` file per `.py` file, with cross-file imports, topological ordering, and automatic lazy-ref detection for circular references. See [`python/README.md`](../python/README.md) for full documentation.

## Contributor notes

- keep Embind-only code isolated in `ts/embind/`
- avoid mixing TS/WASM-specific code into the C++ core unless it is truly a format-level concern
- when the C++ core changes, rebuild both the Python package and WASM module before validating the TS layer
