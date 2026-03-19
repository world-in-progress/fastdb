# fastdb4ts

Browser-oriented TypeScript bindings for `fastdb`, powered by WebAssembly.

`fastdb4ts` brings the `fastdb` storage model to TypeScript with a browser-first API centered around typed schemas, structured table access, binary buffer transport, and graph serialization.

## What this package provides

- **Typed schemas**
  - model records with `Feature` subclasses and `defineSchema(...)`
- **Structured table access**
  - use `ORM`, `TableDefn`, `Table`, and column accessors
- **Binary buffer roundtrips**
  - import/export databases as `Uint8Array` / `ArrayBuffer`
- **Graph serialization**
  - use `FastSerializer` for nested features, lists, and cyclic references
- **Shared native semantics**
  - backed by the same C++ core as `fastdb4py`

## Current scope

This package currently targets the browser/WebAssembly use case.

Included:

- browser-first WASM runtime
- `Feature` + `defineSchema(...)`
- `ORM`, `TableDefn`, `Table`, column access
- `FastSerializer`
- `Uint8Array` / `ArrayBuffer` import/export

Not included in the current TS package:

- shared-memory IPC
- direct filesystem persistence APIs

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

Unlike `fastdb4py`, the TypeScript package uses explicit schema definitions rather than Python annotations.

You can write these schemas by hand, or **auto-generate them** from Python Feature classes using the `fdb codegen` CLI (see [Generating schemas from Python](#generating-schemas-from-python) below).

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

## FastSerializer

`FastSerializer` is the graph-oriented layer for feature graphs that are more naturally represented as nested objects than flat tables.

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

## Generating schemas from Python

If you define your canonical Feature classes in Python (`fastdb4py`), you can generate the equivalent TypeScript schemas automatically using the `fdb codegen` CLI:

```bash
pip install fastdb4py
fdb codegen --ts ./python_features/ ./ts_features/
```

This generates one `.ts` file per `.py` file, with:

- all field types mapped (`F64`, `STR`, `ref(...)`, `listOf(...)`, etc.)
- cross-file imports resolved to relative paths
- circular references using lazy refs `ref(() => ClassName)`
- topological class ordering within each file

See the [Python binding README](../../python/README.md) for full codegen documentation.

## Repository documentation

For fuller project documentation, see:

- repository overview: [`README.md`](../../README.md)
- TypeScript/WASM binding guide: [`ts/README.md`](../README.md)
- C++ core guide: [`fastcarto/README.md`](../../fastcarto/README.md)
- Python binding guide: [`python/README.md`](../../python/README.md)

