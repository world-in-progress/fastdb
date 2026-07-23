# fastdb4ts

Browser-oriented TypeScript bindings for `fastdb`, powered by WebAssembly.

`fastdb4ts` brings the `fastdb` storage model to TypeScript with a browser-first API centered around typed schemas, structured table access, and binary buffer transport.

## What this package provides

- **Typed schemas**
  - model records with `Feature` subclasses and `defineSchema(...)`
- **Structured table access**
  - use `ORM`, `TableDefn`, `Table`, and column accessors
- **Binary buffer roundtrips**
  - import/export databases as `Uint8Array` / `ArrayBuffer`
- **Legacy graph serialization**
  - `FastSerializer` remains available for existing nested-feature and cyclic-reference users, but new cross-runtime integration work should use neutral schema descriptors and explicit codec profiles
- **Shared native semantics**
  - backed by the same C++ core as `fastdb4py`

## Current scope

This package currently targets the browser/WebAssembly use case.

Included:

- browser-first WASM runtime
- `Feature` + `defineSchema(...)`
- `ORM`, `TableDefn`, `Table`, column access
- `FastSerializer` legacy compatibility APIs
- `Uint8Array` / `ArrayBuffer` import/export

Not included in the current TS package:

- shared-memory IPC
- direct filesystem persistence APIs
- downstream contract, routing, relay, or transport helper generation

## Integration boundary

`fastdb4ts` owns generic browser/WASM access to FastDB schemas and binary buffers. External RPC systems may consume those schemas and buffers, but their contract planning, route identity, relay behavior, and generated client helpers belong in those systems rather than in `fastdb4ts`.

## Portable payload subpath

`fastdb4ts/payload` is the official browser-capable WebAssembly projection of
the stable `fastdb.payload.v1` C ABI:

```ts
import { CompiledSpec, Profile, initPayload } from 'fastdb4ts/payload';

await initPayload();
const source = new TextEncoder().encode(
  '{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]}',
);
const spec = CompiledSpec.compile(source);
try {
  console.assert(spec.profile() === Profile.RecordV1);
} finally {
  spec.dispose();
}
```

The packed subpath includes the reviewed Wasm module and works in browser,
worker, and Node test hosts without a Node-native FastDB projection. It checks
the Core ABI version and calls only exported `fdb_payload_v1_*` functions. It
does not parse portable schemas or binaries, compute canonical identity, plan
layout, walk graphs, or materialize values in TypeScript; the C++ Core remains
the sole authority.

Owned handles use explicit, idempotent `dispose()` with finalization only as a
fallback. Safe text and byte methods copy out of scoped Wasm access, graph
sharing/cycles use Core `(componentIndex, objectId)` coordinates, and detached
materialization is one Core call. `CompiledSpec.generate(...)` returns an
explicitly disposable immutable ArtifactSet from the same Core-owned
C++/Rust/Python/TypeScript generator. Artifact paths, bytes, SHA-256 receipts,
limits, provenance, and errors come through the Wasm C ABI; TypeScript does not
render source. The earlier Python-class discovery generator is removed by the
P5 clean cut rather than extended by this projection.

P4 is locally complete. The official WebAssembly projection type-checks and
executes the four-shape hostile generated-artifact matrix through the same
ABI-117 Core; TypeScript gains no private parser or renderer. P5 local clean
cut is complete. Hosted execution, versioning, publication, and release
evidence remain open.

## Installation

```bash
npm install fastdb4ts
```

If you are working from the repository source instead of npm:

```bash
bash ts/build-wasm.sh
npm --prefix ts/fastdb4ts run build
```

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
  static schema = defineSchema({
    x: F64,
    y: F64,
  });
}

const db = ORM.truncate([new TableDefn(Point, 3)]);
const table = db.table(Point);

table.column.x.fill([1.5, 2.5, 3.5]);
table.column.y.fill([4.5, 5.5, 6.5]);

console.log(table.get(1).x); // 2.5
```

## Defining schemas

Unlike `fastdb4py`, the standalone TypeScript feature layer uses explicit
schema definitions rather than Python annotations. These definitions are
written by the application and are not portable payload authority. Portable
TypeScript artifacts instead come from a Core-compiled specification through
the [portable artifact generator](#generating-portable-payload-artifacts).

```ts
import {
  F64,
  Feature,
  I32,
  STR,
  defineSchema,
  listOf,
  ref,
} from 'fastdb4ts';

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

## Working with tables

### Fixed-size tables

```ts
import { F64, Feature, ORM, TableDefn, defineSchema, initFastdb } from 'fastdb4ts';

await initFastdb();

class Particle extends Feature {
  static schema = defineSchema({
    x: F64,
    y: F64,
    vx: F64,
    vy: F64,
  });
}

const db = ORM.truncate([new TableDefn(Particle, 4)]);
const table = db.table(Particle);

table.column.x.fill([0.0, 1.0, 2.0, 3.0]);
table.column.y.fill([10.0, 20.0, 30.0, 40.0]);
table.column.vx.fill([0.1, 0.1, 0.2, 0.2]);
table.column.vy.fill([0.0, 0.0, 0.0, 0.0]);

console.log(table.get(2).x); // 2.0
```

### Buffer roundtrip

```ts
const bytes = db.toBuffer();
const copy = ORM.fromBuffer(bytes);
const copyTable = copy.table(Particle);

console.log(copyTable.get(2).x); // 2.0
```

## FastSerializer Legacy Compatibility

`FastSerializer` is the legacy graph-oriented layer for feature graphs that are more naturally represented as nested objects than flat tables. It remains available for compatibility and migration tests, but it is not the foundation for new external RPC integration work.

Supported scenarios include:

- nested features
- scalar lists
- string lists
- feature references
- cyclic references
- numeric-list columnar auxiliary layers

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
  static schema = defineSchema({
    id: I32,
    points: listOf(Point),
  });
}

const line = new Line({
  id: 42,
  points: [new Point({ x: 1.0, y: 2.0 }), new Point({ x: 3.0, y: 4.0 })],
});

const bytes = FastSerializer.dumps(line);
const copy = FastSerializer.loads(bytes, Line);

console.log(copy.points[1].y); // 4.0
```

Cycles are preserved:

```ts
import { Feature, I32, FastSerializer, defineSchema, initFastdb, ref } from 'fastdb4ts';

await initFastdb();

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

## Build

From the repository root:

```bash
npm --prefix ts/fastdb4ts run build
```

To rebuild the WASM module as well:

```bash
bash ts/build-wasm.sh
npm --prefix ts/fastdb4ts run build
```

## Tests

Pure TypeScript tests live under `tests/ts/`.

Run them from the repository root:

```bash
npm run test:ts
```

Serializer interop validation is also available:

```bash
npm --prefix ts/fastdb4ts run test:serializer:interop
```

## Generating portable payload artifacts

The `fdb` CLI accepts a portable specification and asks the C++ Core for the
TypeScript ArtifactSet:

```bash
pip install fastdb4py
fdb codegen specification.json \
  --target typescript \
  --output ./generated-fastdb
```

The destination must not already exist. FastDB Core owns the compiled model,
identifier escaping, source rendering, artifact bytes, and hashes. The Python
CLI only validates the returned artifact envelope and publishes the complete
tree with exclusive writes.

See the [Python binding README](../../python/README.md) for full codegen documentation.

## Repository documentation

For fuller project documentation, see:

- repository overview: [`README.md`](../../README.md)
- TypeScript/WASM binding guide: [`ts/README.md`](../README.md)
- C++ core guide: [`fastcarto/README.md`](../../fastcarto/README.md)
- Python binding guide: [`python/README.md`](../../python/README.md)
