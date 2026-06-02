# Neutral Allocator Destructive Update Vision

**Date:** 2026-05-28
**Status:** P0 architectural direction
**Weight:** This document is higher priority than incremental optimization plans. If an implementation plan conflicts with this document, update the plan or explicitly document why this vision is being superseded.

## Purpose

FastDB must support neutral memory allocation at the C++ core layer, not only through Python helper protocols. Downstream runtimes such as C-Two need to provide shared-memory backed allocations, but the FastDB design must remain runtime-neutral so Python, TypeScript/WASM, Rust, Go, and future bindings do not each reimplement allocator semantics.

The target model is:

```text
FastDB schema + call binding + layout/size plan
    -> FastDB build scratch memory for analysis and staging
    -> neutral final backing resource for the completed DB buffer
    -> typed Batch / Array / Feature views over that backing
```

The non-goal is adding more Python-side adapter layers around the current builder. The current builder still owns intermediate vectors and then writes them out. That can be a fallback, but it is not the direct-allocation architecture.

## Current Problem

Current fixed-size `Batch.allocate(...)` and `fdb.require(...)` eventually create normal FastDB-owned backing through `ColumnEngine.truncate(...)`. C++ builders keep row tables, geometry, list payloads, string offsets, and string payloads in builder-owned `std::vector` storage, then flush through `WriteStream`.

This means a downstream runtime can ask FastDB to write final bytes into a caller-provided buffer, but FastDB may still have already built another backing buffer first. That is only a prepared writer optimization, not direct allocation.

The destructive update is to make memory roles explicit in the C++ core. There are at least two different roles, and collapsing them into one "allocator" hides the design boundary:

- build-time scratch memory for dynamic construction and analysis;
- final/static backing ownership for completed DB buffers, loaded buffers, mmap files, and shared-memory regions.

## Required C++ Core Abstraction

FastDB should introduce neutral memory resource interfaces in the public C++ core. They must be usable without Python.

The API should be derived from refactoring the current C++ builder, not invented in a downstream runtime first. FastDB should first identify every current allocation role in `FastVectorDbBuild`, `FastVectorDbLayerBuild`, `MemoryStream`, loaded DB buffers, and mapped table/view ownership, then expose the smallest interfaces that those roles actually need.

### Build Scratch Resource

The build scratch resource is for temporary construction state:

- row staging buffers;
- variable-length string/list packing;
- geometry conversion;
- object graph traversal;
- REF/list[REF] dependency discovery;
- reference fixup maps;
- planning caches.

This resource can remain FastDB-owned heap memory by default. Downstream runtimes should not be required to implement it, and C-Two should not initially provide it. Dynamic build scratch does not become a transport payload merely because it is allocated through a neutral interface.

### Final Backing Resource

The final backing resource is for completed DB memory that can be viewed, saved, loaded, or transported:

```cpp
struct FastdbAllocation {
    void* data;
    size_t size;
    size_t alignment;
    void* cookie;
};

class FastdbBackingResource {
public:
    virtual FastdbAllocation allocate(size_t size, size_t alignment) = 0;
    virtual void commit(FastdbAllocation allocation) = 0;
    virtual void rollback(FastdbAllocation allocation) = 0;
    virtual ~FastdbBackingResource() = default;
};
```

The exact API can differ, but the final backing properties are mandatory:

- C++ is the semantic owner of the allocator contract.
- Default implementation uses normal owned heap memory.
- External implementations can back final allocations with shared memory, mmap, arena memory, or WASM heap regions.
- Allocation failure, rollback, and commit are explicit.
- FastDB never sees downstream runtime route names, pool names, transport handles, or wire metadata.
- Python and TypeScript bindings expose capability, not independent allocator semantics.

Loaded/static backing also needs ownership semantics, not only allocation semantics. FastDB must be able to adopt or map an existing buffer from a file, SHM region, mmap, WASM heap, or downstream transport, associate it with a lifetime owner, and invalidate checked views when that owner is released. This should be modeled as backing ownership/lifetime, not as build scratch allocation.

## Planned DB Build

FastDB needs a planned DB build path before it can safely write into an external final backing resource. The plan must know:

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

Known row count is not enough. The direct path requires known final byte layout. Variable-length strings, lists, converted geometry, and object graph dependencies may need a build scratch planning phase before FastDB can request a final backing allocation.

## Direct Final Backing Target

The first direct allocator target should be narrow:

- fixed-size columnar `Batch[Feature]`;
- scalar `Array[T]` with known length;
- no dynamic `push(...)`;
- no object graph dependencies;
- no unknown-size string/list/object traversal during write;
- one final call-db backing buffer per request or response.

This target matches high-performance RPC payloads and avoids protocol changes for multiple buffers.

For fixed numeric columns, FastDB should write row-table bytes directly into the planned row-table section. For string columns where all offsets and data sizes are known before allocation, a two-pass plan/write implementation is acceptable. If size cannot be known up front, use fallback.

This target is a final backing optimization, not a promise that every temporary build allocation uses external memory.

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

ObjectEngine may become more optimizable later if FastDB gains a fully declared object-graph plan with stable root positions, dependency layer order, and reference fixup rules. Until then, dynamic object graph construction is a fallback by design.

## Required Public Semantics

`Batch` and `Array` remain logical payload concepts. `Table` remains a storage implementation detail. Users should not need to spell physical layer names such as `return_0` to get the high-performance path.

Preferred user model:

```python
cells = fdb.require(fdb.batch(Coord, rows=n))
cells.fill(row_id=ids, x=x, y=y, z=z)
return cells
```

When a call-scoped final backing context exists and the payload has a known final byte layout, `fdb.require(...)` may build directly into that context. When no context exists, or when the payload needs dynamic analysis before final size is known, it must fall back to FastDB-owned memory with identical logical semantics.

## Binding And Context Rules

FastDB may expose a call-scoped build context, but it must be runtime-neutral:

- no C-Two names in FastDB public APIs;
- no transport-specific pool object in FastDB schemas;
- no public mutable physical table names for call slots;
- positional call-envelope binding remains the main RPC authoring model;
- object-graph root positions must eventually be distinct from feature type names.

The allocator context must be explicit enough for tests to prove whether direct allocation was used.

The context should distinguish scratch and final backing roles. A downstream runtime may provide only final backing. FastDB must not assume that the same provider can satisfy dynamic scratch allocation, loaded-buffer ownership, and final transport backing.

## View Lifetime Rules

Direct allocator backing does not replace FastDB view lifetime checks. Backed views still need owner/generation invalidation. If a downstream runtime releases a backing allocation, FastDB-managed views over that allocation must fail on later checked access.

Raw pointer or unsafe NumPy escapes remain unsafe. FastDB should document and test checked view invalidation, not claim impossible raw pointer invalidation.

## Tests Required Before Claiming Success

No implementation may claim "direct allocator" or "direct SHM build" without tests proving:

- `fdb.require(...)` can build a fixed-size columnar payload without `WxMemoryStream().data().tobytes()`;
- the final DB backing lives in the memory resource allocation supplied by the caller;
- dynamic scratch allocation remains FastDB-owned unless a separate scratch resource is explicitly configured;
- fallback paths are observable and not counted as direct;
- rollback frees or invalidates failed allocations;
- held/borrowed checked views fail after backing invalidation;
- object-engine dynamic push falls back clearly;
- direct and fallback encodings are byte-compatible or logically equivalent for supported shapes.

## Migration Stance

FastDB is still early enough to make destructive internal changes. Do not preserve incorrect builder-only abstractions as first-class public design. Keep compatibility only where it protects real users:

- existing `MemoryStream` and owned heap build stay as fallback;
- existing storage APIs remain usable for engine workflows;
- direct final backing APIs should be introduced as the new high-performance foundation for planned payloads.

The implementation should remove misleading Python-only allocator claims once the C++ contract exists. Any future Python allocator helper should be a binding of the C++ resource roles, not a parallel contract.

## 2026-05-28 Implementation Checkpoint

The current V1 implementation has landed the first final-backing mechanics, but it must not be described as the full neutral allocator architecture yet.

Implemented:

- C++ `FastVectorDbBuild::byteLength()` and `postToBuffer(...)` can compute the exact final DB length and write the final DB bytes into a caller-provided writable buffer without `MemoryStream(vector) -> Python bytes`.
- C++ `ScratchAllocator` / `ScratchAllocation` and a default heap-backed implementation now exist as an explicit build-scratch role. This establishes the public role boundary separately from final backing allocation; the current builder still uses existing internal vectors for most dynamic scratch.
- C++ `FinalBackingResource` / `FinalBackingAllocation` and a default heap-backed implementation are now consumed by `FastVectorDbBuild::postToFinalBacking(...)`, so the final-backing allocation/commit path exists in the C++ core instead of only as a Python protocol.
- Python `build_call_db(..., allocator, direct_required=True)` uses the C++ final writer for eligible fixed columnar payloads, supports native `HeapFinalBackingResource`, keeps rollback on write/commit failure for Python writable allocators using the same `commit(used_size)` semantic as native final backing, and exposes `build_mode` / `fallback_reason` on prepared plans. Native final backing resources also accept fallback prepared plans, so fallback remains logically equivalent instead of forcing callers into strict direct mode.
- Python `call_db_build_context(binding, allocator)` lets call-scoped `fdb.require(...)` allocate one caller-provided final backing for fixed numeric `Batch` / `Array` slots and later commit that same allocation through `build_call_db(...)`. This path accepts both Python writable allocators and native `FinalBackingResource` instances.
- Committed native `FinalBackingAllocation` instances can be passed directly to `view_call_db(...)` / `decode_call_db(...)`; retained views keep the allocation owner alive and checked view invalidation remains the lifetime boundary. Uncommitted or rolled-back allocations cannot be read through `_readonly_buffer()` or `to_bytes()`.
- The require-context path now puts C++ fixed layers in a deferred table-buffer mode for the initial snapshot: layer headers and table offsets are still written by C++, but the builder writes the zero-filled table section directly to the caller backing instead of retaining an equal-size `m_table_buffer` scratch vector before user code fills the mapped `Batch` / `Array` views.
- Strict fixed-numeric `build_call_db(..., direct_required=True)` values that are not already in a require-context can also use the mapped final-backing path. FastDB plans the fixed layout before allocation, writes the initial C++ snapshot into the caller backing without materialized table-buffer scratch, maps views over that backing, and fills numeric columns there. Prepacked string feature columns still use the C++ final-writer path.
- Strict direct `fdb.require(...)` rejects non-columnar `BatchRequirement` profiles before allocation instead of silently overriding the requested storage profile.
- Strict `prepare_call_db(..., direct_required=True)` is limited to already-backed/importable layer payloads. It must not create temporary staged layers and then label the plan direct; direct construction of new final backing belongs to `build_call_db(..., allocator, direct_required=True)`.
- Existing dynamic/object graph paths remain fallback.

Not implemented yet:

- Full allocator-aware replacement of the current builder's internal dynamic scratch containers. Build-time dynamic scratch remains FastDB-owned heap/vector state unless a future refactor routes specific builder containers through `ScratchAllocator`.
- A fully C++ row-table-section direct writer for every strict direct shape. Fixed numeric Python call-db values now use mapped final-backing fill without builder table-buffer scratch, but prepacked string feature columns still stage through the C++ builder's existing string/table state before `postToFinalBacking(...)`.
- Require-context direct strings, even prepacked strings. The current context rejects string slots because the backing is allocated before user fill. Prepacked string direct build is available only through the `build_call_db(...)` final-writer path when the source value is already backed.
- Object graph, list/ref traversal, bytes payloads, unknown-size strings, and dynamic push direct allocation.
