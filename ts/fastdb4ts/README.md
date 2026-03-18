# fastdb4ts

Browser-oriented TypeScript bindings for `fastdb`, powered by WebAssembly.

## Current scope

- Browser-first WASM runtime
- `Feature` + `defineSchema(...)`
- `ORM`, `TableDefn`, `Table`, column access
- `FastSerializer`
- `Uint8Array` / `ArrayBuffer` import-export

Not included in the current TS package:

- shared-memory IPC
- filesystem persistence APIs

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

Serializer interop validation is still available separately:

```bash
npm --prefix ts/fastdb4ts run test:serializer:interop
```

## Example

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

const db = ORM.truncate([new TableDefn(Point, 2)]);
const table = db.table(Point);

table.column.x.fill([1.5, 2.5]);
table.column.y.fill([3.5, 4.5]);
```
