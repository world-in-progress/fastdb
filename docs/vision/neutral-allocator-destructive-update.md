# Neutral Allocator Destructive Update Vision

**Date:** 2026-05-28
**Status:** P0 architectural direction
**Weight:** This document is higher priority than incremental optimization plans. If an implementation plan conflicts with this document, update the plan or explicitly document why this vision is being superseded.

## Purpose

FastDB must support neutral memory allocation at the C++ core layer, not only through Python helper protocols. Downstream runtimes such as C-Two need to provide shared-memory backed allocations, but the FastDB design must remain runtime-neutral so Python, TypeScript/WASM, Rust, Go, and future bindings do not each reimplement allocator semantics.

The target model is:

```text
FastDB schema + call binding + size plan
    -> neutral C++ memory resource
    -> one planned DB backing buffer
    -> typed Batch / Array / Feature views over that backing
```

The non-goal is adding more Python-side adapter layers around the current builder. The current builder still owns intermediate vectors and then writes them out. That can be a fallback, but it is not the direct-allocation architecture.

## Current Problem

Current fixed-size `Batch.allocate(...)` and `fdb.require(...)` eventually create normal FastDB-owned backing through `ColumnEngine.truncate(...)`. C++ builders keep row tables, geometry, list payloads, string offsets, and string payloads in builder-owned `std::vector` storage, then flush through `WriteStream`.

This means a downstream runtime can ask FastDB to write final bytes into a caller-provided buffer, but FastDB may still have already built another backing buffer first. That is only a prepared writer optimization, not direct allocation.

The destructive update is to make memory ownership an explicit C++ core dependency of planned DB construction.

## Required C++ Core Abstraction

FastDB should introduce a neutral memory resource interface in the public C++ core. The interface must be usable without Python:

```cpp
struct FastdbAllocation {
    void* data;
    size_t size;
    size_t alignment;
    void* cookie;
};

class FastdbMemoryResource {
public:
    virtual FastdbAllocation allocate(size_t size, size_t alignment) = 0;
    virtual void commit(FastdbAllocation allocation) = 0;
    virtual void rollback(FastdbAllocation allocation) = 0;
    virtual ~FastdbMemoryResource() = default;
};
```

The exact API can differ, but the properties are mandatory:

- C++ is the semantic owner of the allocator contract.
- Default implementation uses normal owned heap memory.
- External implementations can back allocations with shared memory, mmap, arena memory, or WASM heap regions.
- Allocation failure, rollback, and commit are explicit.
- FastDB never sees downstream runtime route names, pool names, transport handles, or wire metadata.
- Python and TypeScript bindings expose capability, not independent allocator semantics.

## Planned DB Build

FastDB needs a planned DB build path before it can safely write into external memory. The plan must know:

- top-level DB header size;
- layer count and layer order;
- each layer header offset and total size;
- field descriptor section size;
- row table offset and size;
- geometry section offset and size;
- string section offsets and byte sizes;
- list section offsets and byte sizes;
- object graph dependency/root metadata when supported;
- whether a layer can be imported as a contiguous binary section.

Only after this plan exists should FastDB request one destination allocation for the complete DB backing.

## Direct Build Target

The first direct allocator target should be narrow:

- fixed-size columnar `Batch[Feature]`;
- scalar `Array[T]` with known length;
- no dynamic `push(...)`;
- no object graph dependencies;
- no unknown-size string/list/object traversal during write;
- one final call-db backing buffer per request or response.

This target matches high-performance RPC payloads and avoids protocol changes for multiple buffers.

For fixed numeric columns, FastDB should write row-table bytes directly into the planned row-table section. For string columns where all offsets and data sizes are known before allocation, a two-pass plan/write implementation is acceptable. If size cannot be known up front, use fallback.

## Fallback Boundary

Fallback is not a failure. It is required for correctness.

These cases should stay on the existing owned build path until a real planned implementation exists:

- dynamic `ObjectEngine.create().push(...)`;
- dynamic `ColumnEngine.create().push(...)`;
- object graphs with REF/list[REF] dependency discovery;
- repeated feature identity resolution and reference fixups;
- dynamic strings/lists where total payload size is not known before build;
- geometry formats requiring conversion where final size is not known cheaply;
- any shape where direct build would require mutating public physical table names;
- any caller that lacks a neutral memory resource.

For these cases FastDB may build with heap vectors, `MemoryStream`, or existing buffer export, then write/copy into the downstream transport. The API must report that fallback happened so benchmarks do not mislabel it as direct allocation.

## Required Public Semantics

`Batch` and `Array` remain logical payload concepts. `Table` remains a storage implementation detail. Users should not need to spell physical layer names such as `return_0` to get the high-performance path.

Preferred user model:

```python
cells = fdb.require(fdb.batch(Coord, rows=n))
cells.fill(row_id=ids, x=x, y=y, z=z)
return cells
```

When a call-scoped allocator context exists, `fdb.require(...)` may build directly into that context. When no context exists, it must fall back to FastDB-owned memory with identical logical semantics.

## Binding And Context Rules

FastDB may expose a call-scoped build context, but it must be runtime-neutral:

- no C-Two names in FastDB public APIs;
- no transport-specific pool object in FastDB schemas;
- no public mutable physical table names for call slots;
- positional call-envelope binding remains the main RPC authoring model;
- object-graph root positions must eventually be distinct from feature type names.

The allocator context must be explicit enough for tests to prove whether direct allocation was used.

## View Lifetime Rules

Direct allocator backing does not replace FastDB view lifetime checks. Backed views still need owner/generation invalidation. If a downstream runtime releases a backing allocation, FastDB-managed views over that allocation must fail on later checked access.

Raw pointer or unsafe NumPy escapes remain unsafe. FastDB should document and test checked view invalidation, not claim impossible raw pointer invalidation.

## Tests Required Before Claiming Success

No implementation may claim "direct allocator" or "direct SHM build" without tests proving:

- `fdb.require(...)` can build a fixed-size columnar payload without `WxMemoryStream().data().tobytes()`;
- the final DB backing lives in the memory resource allocation supplied by the caller;
- fallback paths are observable and not counted as direct;
- rollback frees or invalidates failed allocations;
- held/borrowed checked views fail after backing invalidation;
- object-engine dynamic push falls back clearly;
- direct and fallback encodings are byte-compatible or logically equivalent for supported shapes.

## Migration Stance

FastDB is still early enough to make destructive internal changes. Do not preserve incorrect builder-only abstractions as first-class public design. Keep compatibility only where it protects real users:

- existing `MemoryStream` and owned heap build stay as fallback;
- existing storage APIs remain usable for engine workflows;
- direct allocator APIs should be introduced as the new high-performance foundation.

The implementation should remove misleading Python-only allocator claims once the C++ contract exists.
