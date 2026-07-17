# fastcarto / fastdb C++ core

This directory contains the native implementation that the higher-level bindings build on.

> **Portable payload status:** The C++ Core now owns the P1
> `fastdb.payload.v1` compiler, RFC 8785 canonicalization, SHA-256 identity,
> manifest/index/capability facts, stable owned errors, and the exact 33-symbol
> C query ABI plus C++17 RAII facade. P1 does not include
> `fastdb.payload.bin.v1`, layout/build/open, checked lifetimes, final backing,
> language projections, or payload code generation. See the [current status
> issue](../docs/issues/0002-portable-payload-foundation-implementation-status.md)
> and [accepted design](../docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md).

For the `fastdb` project, `fastcarto/fastdb/` is the authoritative storage engine. Both `fastdb4py` and `fastdb4ts` rely on it for current binary layout, row access, geometry storage, and serialization behavior. The accepted 0.2.0 work extends that authority to the portable payload schema and every cross-language semantic decision.

## What is inside `fastcarto/`

- `fastdb/`
  - the database engine itself
- `dump-fastdb/`
  - native utilities for inspecting database files and payloads
- `make-fastdb/`
  - native utilities for constructing database files
- `lib/`
  - bundled third-party native dependencies used by the core
- `CMakeLists.txt`
  - native build entry used by both normal and WASM-oriented builds

## Core responsibilities

The C++ core is responsible for:

- the binary database format
- table and feature layout in memory
- geometry storage and decoding
- numeric and normalized field encoding
- string table storage
- cross-feature references
- buffer ownership and lifecycle
- compatibility between build-time and read-time representations

If one of these behaviors changes, all higher-level bindings must be revalidated.

## Integration boundary

The C++ core owns generic FastDB binary, storage, table, feature, buffer, and accepted portable-payload semantics. Binding layers and RPC systems build on these semantics, but CRM contracts, route identity, relay behavior, call envelopes, generated RPC helpers, and transport-specific lease policy do not belong in the core.

## Public API shape

The current 0.1.x storage API lives in:

- `fastcarto/fastdb/include/fastdb.h`
- `fastcarto/fastdb/include/fastdb-config.h`

The implemented P1 portable surface adds the stable C header
`fastdb_payload.h` and the query-only C++ RAII facade `fastdb_payload.hpp`.
Later runtime/lifetime families remain required by the accepted 0.2.0 design.

Important public classes:

### Build/write path

- `FastVectorDbBuild`
  - top-level database builder
- `FastVectorDbLayerBuild`
  - per-layer/table builder
- `MemoryStream`
  - in-memory write stream used for serialization

### Read/query path

- `FastVectorDb`
  - loaded database
- `FastVectorDbLayer`
  - layer/table view
- `FastVectorDbFeature`
  - row/feature handle

### Supporting types

- `chunk_data_t`
  - raw buffer pointer + size
- `FastVectorDbFeatureRef`
  - cross-layer reference representation
- `GeometryLikeEnum`, `CoordinateFormatEnum`, `FieldTypeEnum`

## Internal structure

The implementation uses a pimpl-style split:

- public declarations in `include/`
- implementation in `src/`
- private implementation details in `_p.h` headers

This matters for bindings because:

- the public API remains relatively stable
- native object ownership stays encapsulated
- binding layers can work with explicit pointers and small wrapper surfaces

## Binary format notes

The `fastdb` buffer format is produced by the build/write classes and consumed by the read/query classes.

The format includes:

- a database header and per-layer headers
- field descriptors
- table row payloads
- geometry/raw blob payloads
- optional string and wide-string tables

One important rule learned during the TypeScript/WASM work: **wire-format structures should use fixed-width integer fields instead of platform-sized types**. Otherwise native and WASM builds can disagree on layout.

## C++ usage example

```cpp
#include "fastdb.h"

using namespace wx;

int main() {
    FastVectorDbBuild db;
    db.begin("");

    auto* layer = db.createLayerBegin("Point");
    layer->setGeometryType(gtAny, cfDefault, false);
    layer->addField("x", ftF64, 0.0, 1.0);
    layer->addField("y", ftF64, 0.0, 1.0);

    layer->addFeatureBegin();
    layer->setField(0, 1.5);
    layer->setField(1, 2.5);
    layer->addFeatureEnd();

    db.createLayerEnd();

    MemoryStream stream;
    db.post(&stream);
    return 0;
}
```

## Relationship to bindings

### Python

`fastdb4py` consumes the core through SWIG and adds:

- `Feature` annotations
- `ORM`
- NumPy zero-copy column access
- shared-memory APIs
- `FastSerializer`

### TypeScript

`fastdb4ts` consumes the same core through Emscripten + Embind and adds:

- `Feature` + `defineSchema(...)`
- `ORM` / `Table`
- `StridedColumn`
- `FastSerializer`
- `Uint8Array` / `ArrayBuffer` transport

## Build paths

From the repository root, the common development path is:

```bash
./py_utils.sh --build
```

The TypeScript/WASM build reuses the same core through:

```bash
bash ts/build-wasm.sh
```

## Contributor guidance

- place public API declarations in `include/`
- keep implementation details in `src/` and private `_p.h` headers
- preserve ABI/binding stability where possible
- avoid architecture-dependent wire-format fields
- when changing binary layout or serialization, validate both Python and TypeScript bindings
