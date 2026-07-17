# FastDB Portable Payload Record Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete FastDB P2 with one Core-owned deterministic `record.v1` runtime for the full legal non-`ref` V1 algebra, including the normative `fastdb.payload.bin.v1` layout, builder and immutable plan, heap/external final backing, truthful direct/staged execution, hardened open, payload ownership, checked views, detached materialization, and an invalidation barrier exposed through the stable C ABI and thin C++17 RAII facade.

**Architecture:** The frozen P1 `CompiledSpec::resolved()` model is compiled once into a finite runtime schema and AoS wire layout; it is never reconstructed from canonical JSON or a binding. A flat logical-value arena feeds one deterministic record encoder, while one hardened reader validates the same canonical region model before any `PayloadOwner` becomes observable. Backing ownership and checked access are explicit: scoped access pins protect borrowed spans, and invalidation blocks new pins, drains active pins, advances the generation, and releases the backing before returning.

**Tech Stack:** C++17, C11 ABI smoke tests, CMake/CTest, existing pinned yyjson/double-conversion/PicoSHA2 snapshots, fixed-width little-endian wire primitives, C++ atomics/mutex/condition variable, libFuzzer, AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer where available, GitHub Actions, Python/uv and TypeScript/WASM regression gates.

## Global Constraints

- The accepted [portable payload design](../specs/2026-07-16-portable-payload-foundation-design.md), [ADR-0001](../../decisions/0001-portable-payload-core-authority.md), [Issue 0001](../../issues/0001-portable-payload-deferred-capabilities.md), and frozen [P1 plan](2026-07-16-portable-payload-core-contract.md) remain normative.
- Work from branch `socu/portable-payload-foundation` at or after P1 freeze commit `c84a39e`; inspect and reconcile every later commit rather than resetting it.
- Consume `spec::CompiledSpec`, `spec::ResolvedSpec`, stable indexes, `error::Error`, and `error::Result` directly. Do not parse canonical JSON, manifest JSON, or schema JSON inside P2.
- The C++ Core remains the sole authority for type checking, quantization, runtime layout, binary bytes, binary validation, backing execution, lifetime, materialization, and invalidation.
- P2 implements `record.v1` only, but it implements every legal non-`ref` type/nullability/cardinality composition: `bool,u8,u16,u32,i32,u8n,u16n,f32,f64,str,wstr,bytes,component,list`, `one`, and `many`.
- `object_graph.v1` remains compile/query-only until P3. A P2 runtime call for that profile returns stable `2009 RUNTIME_UNAVAILABLE` details and Issue 0002 states the exact limit; P2 must not implement a partial graph path.
- `record.v1` is AoS with strided field access. No P2 source, public symbol, manifest fact, or documentation may call it columnar or introduce `columnar.v1`.
- Raw-file bytes, object-store multipart behavior, C-Two CRM/routes/transports/leases, Toodle resources/policy, and Kubernetes remain outside FastDB.
- Public C ABI declarations use opaque handles, exact-width integer fields, pointer-plus-`uint64_t` spans, owned errors, and versioned structs. Do not expose C++ types, exceptions, `size_t`, `wchar_t`, `bool`, C enum layout, `long`, STL, or Rust layout.
- Existing P1 symbol meanings and struct prefixes do not change. P2 adds functions, constants, opaque handles, and versioned structs; unknown flags/reserved values fail and larger future tails are ignored without being read or overwritten.
- All immutable handles use saturating atomic retain/release and are thread-safe. `PayloadBuilder` is uniquely owned and thread-confined. Scoped access pins are uniquely owned; they may be released on another thread but cannot be concurrently queried and released.
- All persisted integers are little-endian. Every addition, multiplication, alignment, offset, length, count conversion, and validation-work increment is checked before use.
- No native recursion is permitted in value-arena teardown, deep-list authoring, runtime-schema traversal, planning, encoding, opening, view traversal, materialization, or validation.
- The encoder and normative byte-layout document/goldens land in the same reviewed task. No second production encoder, decoder, quantizer, text validator, or layout planner is allowed in tests, bindings, legacy code, or C++ facade.
- Direct means no complete payload image existed outside final backing before commit. Staged means a complete image existed elsewhere and was copied. Small bounded metadata/output chunks are allowed in direct mode; a complete hidden vector is not.
- V1 final backing is one contiguous readable committed region. Segmented/multipart backing, streaming authoring, dynamic-graph direct construction, extra guaranteed platforms, native Node, and Go remain only the five deferrals in Issue 0001.
- Every deliberately limited behavior is recorded in `docs/issues/` with current limit, reason, impact, dependencies, and closure criteria. Missing P2 work never moves to Issue 0001.
- Do not remove call-db, `fastdb.schema.v1`, `columnar.v1`, or `ColumnEngine` in P2; do not add new users. Their clean cut remains P5.
- Do not create Rust/Python/TypeScript portable projections or codegen in P2. Existing language suites are regression gates only; P4 owns projections and artifact generation.
- Follow RED -> GREEN -> focused suite -> broad suite -> commit for each task. A fresh independent reviewer must approve specification compliance and code quality before the task is marked complete.
- Do not push, tag, publish, bump package versions, or claim hosted CI success without explicit authority.

---

## Starting Point and Preflight

- [ ] Require `git status --short --branch` to show `socu/portable-payload-foundation` with a clean tracked worktree and index.
- [ ] Run `git log --oneline c84a39e..HEAD`; if non-empty, read every intervening diff and update the paths/interfaces in this plan before implementation.
- [ ] Read `AGENTS.md`, the governing design/ADR/issues, P1 plan, `.superpowers/sdd/progress.md`, `.superpowers/sdd/task-9-report.md`, `spec/Model.hpp`, `spec/CompiledSpec.hpp`, `spec/Manifest.*`, `error/*`, `abi/*`, both public payload headers, payload CMake targets, CI, and the current golden/ABI tooling.
- [ ] Run the clean P1 native baseline:

  ```bash
  cmake -S fastcarto -B build/p2-baseline -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p2-baseline --parallel
  ctest --test-dir build/p2-baseline --output-on-failure
  python3 tools/check_payload_abi_symbols.py --build-dir build/p2-baseline
  ```

  Expected: 19/19 CTest cases and exactly 33 existing `fdb_payload_v1_*` exports pass before P2 changes.

- [ ] Run `uv run pytest tests/python -q`, `uv run python -m compileall -q python/fastdb4py tests/python`, `bash ts/build-wasm.sh`, `npm --prefix ts/fastdb4ts run build`, and `npm run test:ts`; record exact counts and toolchain limitations.
- [ ] Confirm Issue 0002 says P1 is locally frozen, hosted execution is pending, P2 is ready, and P2-P5/0.2.0 remain incomplete.

## Slice Boundary

### Delivered by this plan

- A complete normative `schemas/fastdb.payload.bin.v1.md` for canonical record payload bytes.
- A finite runtime schema derived from the frozen P1 resolved model, with stable runtime type IDs and reusable component layouts.
- A flat, iterative logical-value arena and complete record-profile builder state machine.
- Deterministic record planning/encoding/opening for every legal non-`ref` V1 type composition.
- Immutable repeatable `BuildPlan`, heap and external backing, direct/staged truth, commit/rollback, and execution reports.
- Hardened copy/external open, resource limits, canonical layout validation, and immutable `PayloadOwner`.
- Checked sequence/component/list/scalar/text/bytes views, scoped span access, detached materialization, and the active-access invalidation barrier.
- Additive C ABI and thin C++17 RAII wrappers for the P2 handles and operations.
- Ordered binary/malformed goldens, ABI symbol gate, fuzzing, sanitizers, concurrency tests, CI wiring, package truth, and Issue 0002 evidence.

### Explicitly not claimed by this plan

- Object pools, roots, refs, stable object IDs, shared references, or cycles; these are P3.
- Rust, Python, or TypeScript/WASM portable runtime projections; these are P4.
- Binding-owned high-level materialized objects. P2 returns a Core-owned detached view; P4 converts it into each language's ordinary owned value without reading the binary independently.
- Payload code generation, C-Two contract composition, clean-cut removal, the `RecordEngine` public rename, version 0.2.0 metadata, publication, or release.
- An unsafe raw-span API. Scoped access is the P2 checked lifetime contract; payload/UTF-8/opaque bytes may be zero-copy while UTF-16 units use a safe Core-owned projection. Adding a deliberately unrevocable native escape hatch is optional future API work and is not required by the accepted success criteria.

P3 extends this same binary document with object-graph region kinds and rules in the same reviewed change as its graph encoder/open implementation. It must not change any P2 record-profile byte defined here.

## File and Dependency Map

```text
spec::CompiledSpec / spec::ResolvedSpec
    -> layout::RuntimeSchema
    -> build::ValueArena / build::PayloadBuilder
    -> layout::RecordLayout
    -> build::RecordEncoder / build::BuildPlan
    -> backing::Backing / backing::HeapBacking
    -> view::Open / view::PayloadOwner
    -> view::View / view::Materialize / view::AccessBarrier
    -> abi::opaque handles and fastdb_payload.h
    -> fastdb_payload.hpp RAII only
```

New focused modules:

| Path | Single responsibility |
|---|---|
| `src/payload/layout/CheckedMath.hpp` | Checked integer conversion/add/multiply/alignment and little-endian load/store primitives |
| `src/payload/layout/BinaryFormat.hpp` | Wire constants and POD Core descriptors; no I/O or semantic traversal |
| `src/payload/layout/TextEncoding.{hpp,cpp}` | Shared strict UTF-8/UTF-16 validation and explicit UTF-16LE emission |
| `src/payload/layout/NormalizedInteger.{hpp,cpp}` | Exact binary64-rational u8n/u16n quantize/dequantize independent of host FP environment |
| `src/payload/layout/RuntimeSchema.{hpp,cpp}` | Stable type IDs, reachable-type inventory, and reusable AoS component slot layouts from `ResolvedSpec` |
| `src/payload/layout/RecordLayout.{hpp,cpp}` | Canonical region inventory/offsets/counts from runtime schema plus frozen logical values |
| `src/payload/build/ValueArena.{hpp,cpp}` | Flat immutable/mutable logical nodes and owned text/byte storage |
| `src/payload/build/PayloadBuilder.{hpp,cpp}` | Entry-index authoring state machine and atomic freeze into `LogicalPayload` |
| `src/payload/build/RecordEncoder.{hpp,cpp}` | The one deterministic writer from `RecordLayout`/`LogicalPayload` to a bounded `ByteSink` |
| `src/payload/build/BuildPlan.{hpp,cpp}` | Immutable repeatable plan, plan facts, and backing execution orchestration |
| `src/payload/backing/Backing.{hpp,cpp}` | Validated callback-table adapter and reservation state machine |
| `src/payload/backing/HeapBacking.{hpp,cpp}` | Core default contiguous backing implementation |
| `src/payload/view/Open.{hpp,cpp}` | The one hardened wire reader/validator producing an immutable `PayloadIndex` |
| `src/payload/view/PayloadOwner.{hpp,cpp}` | Spec/backing/index ownership, optional execution report, generation, and barrier |
| `src/payload/view/AccessBarrier.{hpp,cpp}` | Pin acquisition/drain/invalidation synchronization only |
| `src/payload/view/View.{hpp,cpp}` | Checked logical navigation and scalar/span projection over `PayloadIndex` or detached arena |
| `src/payload/view/Materialize.{hpp,cpp}` | Iterative subtree decode into detached `ValueArena` |

The public ABI implementation remains in `src/payload/abi/fastdb_payload.cpp` and private handle layouts remain in `src/payload/abi/Handles.hpp`. If either file becomes difficult to review, split implementation-only helpers into `abi/BuilderAbi.cpp`, `abi/PayloadAbi.cpp`, and `abi/ViewAbi.cpp` while preserving one public header and one exported symbol inventory.

---

## Frozen P2 Runtime Contract

### Stable runtime type IDs

`layout::RuntimeSchema::compile(const spec::CompiledSpec&)` assigns a finite, zero-based `uint32_t` runtime type ID to **every source `TypeNode`**, including nodes owned by unreachable components, without expanding component DAGs. `UINT32_MAX` is permanently reserved as a wire sentinel and is never a type ID. More than `UINT32_MAX` source type nodes fails before layout creation with `RESOURCE_LIMIT` at `/runtime/types`.

The assignment algorithm is normative:

1. Set `next_id = 0`.
2. Visit entries by stable entry index. For each entry, visit its root source node first and assign `next_id++`; while that node is `list`, visit and assign its `items` node immediately, continuing outer-to-inner in preorder until the first non-list item. A `component` or `ref` node records its resolved target index but does not inline-visit target fields.
3. Visit **all** components by stable component index, reachable or not. Within each component visit fields by stable field index. For each field, assign its root and nested `list.items` chain using the same parent-before-child preorder.
4. Record-profile reachability is a separate iterative pass starting at every entry root and following `list.items` plus by-value component field edges. It decides which component layouts, list regions, and value pools exist; it never renumbers source nodes.
5. `ref` reached by a record runtime is an internal invariant failure because P1 rejects it for `record.v1`; `object_graph.v1` is rejected before P2 runtime-schema creation with `RUNTIME_UNAVAILABLE`.

Thus source-path order alone determines IDs: `/entries/{stable-index}/type[/items...]`, followed by `/components/{stable-index}/fields/{stable-index}/type[/items...]`. The normative binary document repeats this algorithm. Goldens must pin an unreachable component ordered before a reachable component, a reused component DAG, and nested lists so reachability changes cannot silently renumber the same compiled spec. Runtime type IDs are binary-local validation facts; they do not change canonical JSON, payload digest, or P1 stable entry/component/field indexes.

### Flat authoring model

The internal interface is:

```cpp
namespace fastdb::payload::build {

using NodeIndex = std::uint64_t;

enum class ValueTag : std::uint8_t {
    null_value,
    boolean,
    u8,
    u16,
    u32,
    i32,
    u8n,
    u16n,
    f32,
    f64,
    str,
    wstr,
    bytes,
    component,
    list,
    sequence,
};

struct ValueNode final {
    std::uint32_t runtime_type_id;
    ValueTag tag;
    std::uint8_t reserved8[3];
    std::uint64_t scalar_bits_or_offset;
    std::uint64_t byte_length;
    NodeIndex first_child;
    NodeIndex next_sibling;
    std::uint64_t child_count;
};

struct BuilderLimits final {
    std::uint64_t max_value_nodes;
    std::uint64_t max_list_elements;
    std::uint64_t max_text_bytes;
    std::uint64_t max_opaque_bytes;
    std::uint64_t max_nesting_depth;
    std::uint64_t max_total_builder_bytes;
};

struct FixedRun final {
    const std::uint8_t* data;
    std::uint64_t data_byte_length;
    std::uint64_t count;
    std::uint64_t stride_bytes;
    const std::uint8_t* validity;
    std::uint64_t validity_byte_length;
    std::uint64_t validity_bit_offset;
};

class LogicalPayload final {
public:
    const spec::CompiledSpec& spec() const noexcept;
    const std::vector<ValueNode>& nodes() const noexcept;
    const std::vector<NodeIndex>& entry_roots() const noexcept;
    std::string_view byte_storage() const noexcept;
};

class PayloadBuilder final {
public:
    static error::Result<PayloadBuilder> create(
        spec::CompiledSpec spec,
        BuilderLimits limits = default_builder_limits());

    error::Result<void> begin_entry(std::uint32_t entry_index,
                                    std::uint64_t value_count);
    error::Result<void> push_null();
    error::Result<void> push_bool(std::uint8_t value);
    error::Result<void> push_u8(std::uint8_t value);
    error::Result<void> push_u16(std::uint16_t value);
    error::Result<void> push_u32(std::uint32_t value);
    error::Result<void> push_i32(std::int32_t value);
    error::Result<void> push_u8n_bits(std::uint64_t binary64_bits);
    error::Result<void> push_u16n_bits(std::uint64_t binary64_bits);
    error::Result<void> push_f32_bits(std::uint32_t binary32_bits);
    error::Result<void> push_f64_bits(std::uint64_t binary64_bits);
    error::Result<void> push_str(std::string_view utf8);
    error::Result<void> push_wstr(const std::uint16_t* units,
                                  std::uint64_t unit_count);
    error::Result<void> push_bytes(const std::uint8_t* bytes,
                                   std::uint64_t byte_count);
    error::Result<void> push_fixed_run(const FixedRun& run);
    error::Result<void> begin_component();
    error::Result<void> begin_list(std::uint64_t item_count);
    error::Result<LogicalPayload> freeze();
};

}  // namespace fastdb::payload::build
```

`begin_entry` accepts entries in any order exactly once. It validates `one -> value_count == 1`; `many` accepts zero or more. A `many` entry whose root is `component` is the Core-defined typed **record-batch frame**: `value_count` is the row count, each `begin_component` starts the next row, and the row component plus field order come only from `RuntimeSchema`. The caller never supplies a record schema, field offset, packed-row layout, or wire descriptor. Value operations consume the next expected type in prefix order. `begin_component` pushes its fields in field-index order and `begin_list(count)` pushes exactly `count` item expectations; frames auto-close when their declared children are complete, including empty batches/components/lists. No end-call ambiguity exists.

`push_fixed_run` is the one bulk exact-width input operation. It consumes one or more consecutive expectations only when every expectation has the same runtime type ID and that type is a fixed scalar. The Core derives the element representation from that expectation: `bool/u8` use `uint8_t`, `u16` uses `uint16_t`, `u32` uses `uint32_t`, `i32` uses `int32_t`, `f32` uses `uint32_t` IEEE bits, and `f64/u8n/u16n` use `uint64_t` binary64 bits. Elements are native exact-width object representations copied with `memcpy`; persisted endian conversion remains Core-owned. `stride_bytes == 0` means tightly packed, otherwise stride is at least the derived width. Checked `(count - 1) * stride + width` must fit `data_byte_length`. `count` must be non-zero and `data` must be non-null for the resulting non-zero required span.

A null validity pointer means every element is present and requires zero validity length/offset. Otherwise bits are low-bit first, `validity_bit_offset + count` must fit the declared bitmap bytes, and a zero bit emits explicit null only for a nullable expected type. Data storage is still required for every element; bytes for null elements are ignored. The operation prevalidates the complete run, all limits, and all expectation transitions before reserving/publishing any node, so a bad bit, stride, range, later expectation, or allocation leaves the builder unchanged. It is intentionally not a second schema or a serialized record-batch format.

All input spans are borrowed only for the call and copied into the arena. `str` validates strict UTF-8, `wstr` validates Unicode scalar UTF-16 units, normalized integers accept finite binary64 logical values in the closed declared interval and use round-to-nearest ties-to-even, and every NaN input is canonicalized only during wire encoding. A failed `freeze` leaves the builder mutable and unchanged; a successful freeze atomically seals it. Every later mutation/freeze returns `BUILDER_STATE`.

Builder defaults and accounting are frozen and platform-independent:

| Limit | Default |
|---|---:|
| value nodes | 10,000,000 |
| list elements | 10,000,000 |
| text bytes (`str` bytes plus `wstr` units times 2) | 1 GiB |
| opaque bytes | 1 GiB |
| nesting depth | 1,024 |
| total logical builder bytes | 1 GiB |

`max_total_builder_bytes` is a deterministic logical-memory budget, not allocator resident size: charge 64 bytes per value node, 64 bytes per currently open expectation frame, 8 bytes per entry-root slot allocated at builder creation, each copied UTF-8 byte, two bytes per copied UTF-16 unit, and each copied opaque byte. Checked arithmetic and the new total are validated **before** allocation growth. Vector capacity, allocator headers, `sizeof` differences, the shared `CompiledSpec`, and derived immutable runtime-schema storage are not charged; their inputs are already bounded by compile limits. Closing a frame removes its 64-byte transient charge, while nodes and copied storage remain charged. Separate node/list/text/opaque/depth limits still apply even when the total budget has room.

Logical error paths are stable RFC 6901 paths using schema IDs and numeric sequence positions:

```text
/entries/{entry-id}
/entries/{entry-id}/{many-index}
/entries/{entry-id}/{many-index}/{field-id}
/entries/{entry-id}/{many-index}/{field-id}/{list-index}
```

For cardinality `one`, omit the many index. IDs already satisfy the P1 ASCII grammar; JSON Pointer escaping remains applied by the shared `JsonPointer` implementation.

### Normative `fastdb.payload.bin.v1` record layout

The Task 2 implementation and `schemas/fastdb.payload.bin.v1.md` must state the following exact byte contract. All offsets below are decimal byte offsets from the start of the payload and all multibyte values are little-endian.

#### Header: exactly 128 bytes

| Offset | Size | Field | V1 rule |
|---:|---:|---|---|
| 0 | 8 | magic | bytes `46 44 42 50 41 59 31 00` (`FDBPAY1\0`) |
| 8 | 2 | major | `1` |
| 10 | 2 | minor | `0` |
| 12 | 4 | header size | `128` |
| 16 | 4 | profile | `FDB_PAYLOAD_PROFILE_RECORD_V1` (`1`) in P2 |
| 20 | 4 | flags | `0` |
| 24 | 8 | total length | exact supplied/committed byte length, including final zero padding |
| 32 | 32 | spec SHA-256 | exact `CompiledSpec::digest()` bytes |
| 64 | 8 | region directory offset | `128` |
| 72 | 4 | region count | exact canonical descriptor count |
| 76 | 4 | region descriptor size | `56` |
| 80 | 8 | entry directory offset | `128 + region_count * 56` |
| 88 | 4 | entry count | exact compiled-spec entry count |
| 92 | 4 | entry descriptor size | `40` |
| 96 | 8 | root value count | checked sum of all entry value counts |
| 104 | 24 | reserved | all zero |

V1 readers reject any other magic, major/minor/header/descriptor size, profile, flag, or non-zero reserved byte. Forward extension uses a new binary version or an accepted flag/record-size contract; old readers fail closed rather than guessing unknown tails.

#### Region descriptor: exactly 56 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | kind |
| 4 | 4 | flags, zero in V1 |
| 8 | 4 | owner index |
| 12 | 4 | runtime type ID |
| 16 | 8 | absolute data offset |
| 24 | 8 | byte length |
| 32 | 8 | logical element count |
| 40 | 4 | fixed element stride, or zero for validity/pool regions |
| 44 | 4 | required alignment: exactly `1`, `2`, `4`, or `8` |
| 48 | 8 | reserved, zero |

Region kinds are fixed-width macros, not C enums:

```text
1 ENTRY_VALUES
2 ENTRY_VALIDITY
3 LIST_ITEMS
4 LIST_VALIDITY
5 UTF8_POOL
6 UTF16_POOL
7 BYTES_POOL
```

Every field combination is normative; readers reject any other value:

| Kind | Present when | Owner index | Runtime type ID | Byte length | Logical element count | Stride | Alignment |
|---|---|---:|---:|---:|---:|---:|---:|
| `ENTRY_VALIDITY` | entry root is nullable | stable entry index | entry root type ID | `ceil(value_count / 8)` | entry value count | `0` | `1` |
| `ENTRY_VALUES` | always | stable entry index | entry root type ID | `value_count * root_slot_stride` | entry value count | root slot stride | root slot alignment |
| `LIST_VALIDITY` | reachable list item type is nullable | owning list node type ID | item type ID | `ceil(item_count / 8)` | aggregated item count for that list node | `0` | `1` |
| `LIST_ITEMS` | for every reachable list node | owning list node type ID | item type ID | `item_count * item_slot_stride` | aggregated item count for that list node | item slot stride | item slot alignment |
| `UTF8_POOL` | a reachable `str` exists | `UINT32_MAX` | `UINT32_MAX` | UTF-8 pool bytes | UTF-8 pool bytes | `0` | `1` |
| `UTF16_POOL` | a reachable `wstr` exists | `UINT32_MAX` | `UINT32_MAX` | UTF-16LE pool bytes | UTF-16 code-unit count (`byte_length / 2`) | `0` | `2` |
| `BYTES_POOL` | a reachable `bytes` exists | `UINT32_MAX` | `UINT32_MAX` | opaque pool bytes | opaque pool bytes | `0` | `1` |

All products, bitmap ceilings, and conversions to the descriptor's `uint32_t` stride are checked. A slot stride greater than `UINT32_MAX` fails layout with `RESOURCE_LIMIT` at its logical type path. Pool logical counts deliberately use bytes except UTF-16, whose count uses 16-bit code units. A validity logical count is the number of represented values/items, not its byte length.

#### Entry descriptor: exactly 40 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | stable entry index, equal to directory position |
| 4 | 4 | entry root runtime type ID |
| 8 | 4 | cardinality: `1` one, `2` many |
| 12 | 4 | flags: bit 0 means root type nullable; all other bits zero |
| 16 | 8 | value count (`1` for one) |
| 24 | 4 | ENTRY_VALUES region index |
| 28 | 4 | ENTRY_VALIDITY region index, or `UINT32_MAX` when non-nullable |
| 32 | 8 | reserved, zero |

#### Canonical directory and data order

The directory contains exactly:

1. each entry by entry index: nullable validity region first when present, then values region;
2. each reachable list node by ascending runtime type ID: nullable-item validity region first when present, then items region;
3. one UTF8, UTF16, and/or bytes pool in that order when the reachable type graph contains the corresponding kind, even when the encoded pool length is zero.

The entry directory follows the region directory with no gap. Region data begins at `align_up(entry_directory_end, 8)`. Each region begins at the smallest offset at or after the previous region end satisfying its declared alignment. All inter-region and final `align_up(end, 8)` padding bytes are zero. Regions are canonical, non-overlapping, and fully inside total length; alternative ordering, gaps beyond required alignment, aliases, trailing bytes, or non-zero padding are invalid.

A zero-length region is still present when required by the canonical inventory: an empty `many`, a reachable list with no items, and a reachable variable kind with no non-empty values all keep their descriptors. Its offset is the smallest aligned current end; adjacent zero-length regions may share that boundary, but neither can point inside a non-empty region or create an optional gap. Values/list regions retain their non-zero canonical stride and alignment even when count is zero; validity/pool stride remains zero. The open validator recomputes the complete required inventory from `RuntimeSchema`, so descriptors cannot be omitted merely because their data is empty.

#### Fixed value and component representation

| Type | Slot bytes | Alignment | Wire rule |
|---|---:|---:|---|
| `bool` | 1 | 1 | exactly `0` or `1` |
| `u8`, `u8n` | 1 | 1 | raw unsigned or quantized code |
| `u16`, `u16n` | 2 | 2 | little-endian raw unsigned or quantized code |
| `u32`, `i32`, `f32` | 4 | 4 | little-endian integer/two's-complement/IEEE bits |
| `f64` | 8 | 8 | little-endian IEEE bits |
| `str`, `wstr`, `bytes` | 16 | 8 | pool byte offset then pool byte length, both `uint64_t` |
| `list` | 16 | 8 | first item index then item count, both `uint64_t` |
| `component` | component stride | component alignment | inline AoS component record |

Every component record starts with `ceil(nullable_immediate_field_count / 8)` validity bytes, low bit first in nullable-field order. Fields follow in stable field order at their smallest aligned offsets. Tail padding extends the record to its maximum field alignment. An empty component has stride `1`, alignment `1`, and a zero byte. Nested components use their own complete inline record layout; the containing field's nullable bit controls the whole nested slot.

Sequence validity regions use one bit per top-level/list-item value, low bit first: `1` is present and `0` is null. Unused high bits are zero. Non-nullable sequences have no validity region. Component-field validity uses the same bit meaning inline.

Every null slot, all descendants of a null component, and all fixed/padding bytes not carrying a present value are zero. Open rejects non-zero null storage or padding so one logical value has one canonical byte representation.

`f32` preserves finite values, signed zero, and infinities but maps every NaN to `0x7fc00000`; `f64` maps every NaN to `0x7ff8000000000000`. Open rejects every non-canonical NaN bit pattern. Normalized integers use the accepted P1 bounds and exact `round_even((value-min)*Q/(max-min))` rule; views decode through the Core formula and return binary64 bits.

Quantization/dequantization decomposes finite binary64 inputs/bounds into sign, integer significand, and power-of-two exponent; a small Core-owned arbitrary-width unsigned integer compares the exact rational against `q/Q` and performs nearest ties-to-even without host floating arithmetic or the caller's rounding mode. Dequantization correctly rounds the exact real formula back to binary64 bits. Linux x86-64, macOS arm64, and wasm32 must match independently calculated endpoint/tie goldens before P2 can freeze; a toolchain disagreement is a contract blocker, not permission for platform-specific bytes.

#### Variable pools and lists

- `str`, `wstr`, and `bytes` use the one corresponding global pool. The descriptor's first word is a **pool-relative byte offset**, never an absolute payload offset; the second word is a byte length. Non-null values append exact bytes in canonical logical traversal order with no terminator, deduplication, aliasing, or unrequired padding.
- `wstr` pool offsets and lengths are bytes and must both be even. Its descriptor participates in byte-based pool partitioning, while the region's logical element count remains UTF-16 code units. The bytes are well-formed UTF-16LE.
- A non-null empty variable value stores the current pool-relative byte cursor and length zero. A null value stores two zero words and does not advance a cursor.
- Each reachable list type node owns one aggregated `LIST_ITEMS` region and optional `LIST_VALIDITY` region. A non-null list descriptor's first word is an **item index relative to the start of that exact list node's item region**, not a byte offset and not an index into another list type; the second word is item count. A null list stores two zero words and does not advance the cursor.
- In canonical parent traversal, every non-null variable descriptor must start at the current cursor, including empty values, then advance by its checked length/count. Null descriptors remain zero/zero. Final pool cursors equal pool byte lengths; final list cursors equal their region logical counts. This exact partition rule rejects gaps, overlap, aliases, reordering, and unconsumed tails even when all individual spans are in bounds.
- List item fixed layout is the item type's slot/component layout. Nested list descriptors address the separately aggregated region owned by the child list runtime type ID.

Canonical traversal is entry index -> entry value index -> component field index/list item index, depth first, with an explicit stack. It is used consistently for pool append order, list aggregation, validation, views, and materialization.

### Stable runtime error mapping

P2 adds these stable errors in addition to `2009 RUNTIME_UNAVAILABLE`:

```text
2010 INVALID_TEXT_ENCODING
3009 NON_CANONICAL_BINARY
3010 INVALID_BINARY_VALUE
```

The mapping is closed before implementation:

| Failure | Stable error |
|---|---|
| builder Boolean byte other than `0/1`; normalized logical value non-finite or outside its declared interval | `OUT_OF_RANGE` |
| builder invalid UTF-8 or unpaired UTF-16 | `INVALID_TEXT_ENCODING` |
| wrong builder operation/fixed-run expectation | `TYPE_MISMATCH` |
| fixed-run zero count, invalid null/length pointer pair, non-zero flags/reserved fields | `INVALID_ARGUMENT`; undersized versioned C descriptor is `UNSUPPORTED_ABI` |
| fixed-run span/stride/bitmap arithmetic overflow | `LENGTH_OVERFLOW` |
| fixed-run declared span too short | `OUT_OF_BOUNDS` |
| binary bad magic; unsupported major/minor; digest mismatch | existing `INVALID_MAGIC`; `UNSUPPORTED_BINARY_VERSION`; `DIGEST_MISMATCH` |
| binary checked arithmetic overflow; span outside total bytes; descriptor offset violates required alignment | existing `LENGTH_OVERFLOW`; `OUT_OF_BOUNDS`; `MISALIGNED` |
| unknown flags/kinds, wrong descriptor inventory/order/owner/type/count/stride/alignment, non-zero reserved/padding/null storage/validity tail, non-canonical NaN, or non-canonical list/pool partition | `NON_CANONICAL_BINARY` |
| binary Boolean byte other than `0/1` or a logical descriptor value invalid for its declared type but otherwise structurally bounded | `INVALID_BINARY_VALUE` |
| malformed UTF-8/UTF-16LE during eager or checked lazy validation | `INVALID_TEXT_ENCODING` |
| graph reference failure in P3 | existing `INVALID_REFERENCE` (unused by record P2) |
| caller-supplied open/builder work limit exceeded | `RESOURCE_LIMIT` for open/layout and `RESOURCE_LIMIT` for runtime-schema/builder logical limits; compile-source limits remain `SPEC_RESOURCE_LIMIT` |

Each failure also freezes its first deterministic RFC 6901 path and canonical expected/actual or reason details in focused fixtures. `fastdb_payload.h`, `symbol_for_code`, C/C++ error parity, and the normative binary document land with the first task that can emit the code; no implementation may substitute a generic `INVALID_ARGUMENT` or `INTERNAL` for malformed caller data.

### Hardened open and resource limits

`view::open_record` validates the complete image before publishing a `PayloadIndex`. Validation is iterative and checks, in order:

1. open-option prefix/flags/reserved fields and caller limits;
2. total bytes before reading the fixed header;
3. magic/version/profile/flags/total length/spec digest/header reserved bytes;
4. checked directory byte ranges and exact entry/region counts against the compiled spec/runtime schema;
5. exact canonical descriptor order, kinds, owner/type IDs, counts, strides, alignments, offsets, zero padding, and non-overlap;
6. validity lengths/tail bits, nullable rules, component inline maps, null-zero storage, and Boolean bytes;
7. integer/float canonicality, descriptor arithmetic, list partitions, pool partitions, UTF-8, and UTF-16LE;
8. exact consumption of list regions/pools and exact final total length.

Open flags are `FDB_PAYLOAD_OPEN_VALIDATE_TEXT_EAGER = 1 << 0`; the initializer enables it. With the flag cleared, structural validation still completes before publication and each checked string access validates its selected span under the access pin. No unchecked text is returned through a checked accessor.

The readable base may have any byte alignment. Every wire scalar/header/descriptor load uses bounded byte operations and explicit little-endian conversion; `MISALIGNED` applies to a descriptor offset relative to the payload, not to the caller's base address. Heap/direct build still requests the plan's maximum alignment for efficient writing, but external open and copy-open have identical binary meaning.

Validation work is deterministic and never depends on allocator or platform layout. Charge exactly one unit for the fixed header, each region descriptor, each entry descriptor, each inspected validity byte, each inspected alignment/final-padding byte, and each logical value slot visited in canonical traversal (entry root, present component field, or list item). A list item is a value slot and is not charged twice. A null scalar/list/descriptor is one bounded slot check; a null component is charged once plus one unit for every byte of its inline component slot scanned to prove all descendant/padding storage is zero, and it does not traverse or advance child pool/list cursors. Eager text validation additionally charges one unit per UTF-8 byte and one per UTF-16 code unit. Present fixed scalar payload bytes and opaque byte contents have no per-byte charge because their one slot validation covers the bounded load/partition check. Every addition is checked and compared before the corresponding read.

With eager text disabled, structural open excludes text-content units. The owner retains `max_validation_work` and `max_string_bytes`; each later checked string access applies those limits independently to the selected span while holding its access pin. This is a per-access cap, not a mutable global budget, so immutable concurrent queries are deterministic. `BuildPlan::validation_work` reports the exact work of opening its output with default eager text enabled.

Safe zero-selecting defaults are:

| Limit | Default |
|---|---:|
| total bytes | 1 GiB |
| regions | 1,000,000 |
| entries | 65,536 |
| components | 65,536 |
| nesting depth | 1,024 |
| list elements | 10,000,000 |
| graph objects | 10,000,000 (accepted now, unused by record P2) |
| string bytes | 1 GiB |
| validation work | 100,000,000 |

Defaults protect untrusted input but are not format maxima; callers may raise a limit explicitly. A limit rejects with `RESOURCE_LIMIT` and stable details but never changes canonical bytes or identity.

### Immutable plans and final backing

`PayloadBuilder::freeze` first produces an immutable `LogicalPayload`; `BuildPlan::create` then owns that value, the `CompiledSpec`, `RuntimeSchema`, `RecordLayout`, exact resource facts, and direct viability. The plan is immutable, retainable, thread-safe, and repeatable. Concurrent executions require distinct backing contexts.

Every P2 record plan is layout-direct-eligible because all final region sizes and offsets are known. Execution policy is exactly:

```text
ALLOW_STAGING = 1
REQUIRE_DIRECT = 2
```

Reserve modes are `DIRECT = 1` and `STAGED = 2`. Execution modes use the same numbers. Stable fallback reasons are:

```text
0 NONE
1 PLAN_REQUIRES_STAGING     # reserved for the accepted P3 dynamic-graph case
2 BACKING_DECLINED_DIRECT
```

A null backing pointer selects Core heap backing and succeeds directly. An external execution first requests `DIRECT`. If and only if reserve returns `DIRECT_UNAVAILABLE`, `ALLOW_STAGING` builds one complete heap image, requests `STAGED`, and copies the exact same bytes; `REQUIRE_DIRECT` returns `DIRECT_UNAVAILABLE` with `{"reason":"backing_declined_direct"}`. Other callback failures never trigger a semantic fallback.

Direct execution either writes into the stable reserved span or emits monotonically increasing, non-overlapping range writes using at most 64 KiB of scratch. It covers the final image exactly once, including zero padding, and never allocates a complete image. Staged execution reports `staging_bytes == total_length`; direct reports zero.

Callback order is exact:

```text
reserve -> zero or more ascending write calls -> commit -> owner
reserve failure -> error (no reservation, rollback, or release)
reserve success -> writes -> failure -> rollback
reserve success -> writes -> commit failure -> rollback
reserve success -> commit success -> Core validation/publication failure -> release
```

After a successful reserve, exactly one rollback occurs on every pre-commit/commit failure and none after commit success. Commit is atomic: failure leaves the reservation uncommitted and rollback-capable. If rollback also fails, `ROLLBACK_FAILED` is returned with the original code/symbol and rollback status in canonical details. Commit success returns the exact contiguous readable base/length and transfers the reservation's initial owner reference to `PayloadOwner`; if the shared hardened reader or owner allocation then fails, Core releases that committed reference exactly once and never rolls it back.

### Payload owner, checked access, and materialization

`PayloadOwner::State` owns one `CompiledSpec`, one committed/read-retained backing, one immutable `PayloadIndex`, optional build execution report, and this barrier state:

```cpp
struct AccessBarrierState final {
    std::mutex mutex;
    std::condition_variable drained;
    std::uint64_t generation{UINT64_C(1)};
    std::uint64_t active_accesses{UINT64_C(0)};
    bool invalidating{false};
    bool invalidated{false};
};
```

A checked view retains shared owner state and captures its generation. Scalar/navigation calls acquire a short internal pin. Borrowed payload/UTF-8/opaque-byte pointers require an explicit unique `Access` pin. Pin acquisition under the mutex succeeds only when the owner is neither invalidating nor invalidated and the captured generation matches. The pin increments `active_accesses`; release decrements and wakes invalidation when it reaches zero.

`wstr` access never reinterprets UTF-16LE wire bytes as C/C++ objects. While holding the pin, `Access` performs explicit little-endian loads into a Core-owned, correctly aligned `std::vector<std::uint16_t>` and validates the selected text; `access_wstr` returns that vector's pointer only for the `Access` lifetime. Detached-arena `wstr` uses the same projection. The access retains its pin until release for one uniform barrier rule. Consequently payload/UTF-8/opaque spans are zero-copy, while wide-string access may copy and endian-convert; “scoped access” is the lifetime contract, not a universal zero-copy claim.

Invalidation is idempotent and linearized under the same mutex:

1. one caller sets `invalidating=true`, preventing every new pin;
2. it waits until `active_accesses == 0`;
3. it advances generation with checked non-wrapping semantics, moves the retained backing out, marks invalidated, but keeps `invalidating=true` while it unlocks and invokes the external release callback;
4. it locks again, clears `invalidating`, wakes concurrent invalidators, and only then returns success. Concurrent invalidators wait for that final release-complete transition rather than returning early.

After return, stale view objects may still exist but no checked operation can touch the old backing. A test-only near-overflow hook must prove generation cannot wrap into a valid stale generation; production treats exhaustion as an immortal invalidated state.

Payload metadata that needs no backing—compiled-spec digest, profile, and a real stored execution report—remains queryable after invalidation. Binary-copy/acquire/entry-view calls and every stale backed-view operation fail with `VIEW_INVALIDATED`. Detached materialized views remain unaffected.

`view::materialize` holds one source pin while it iteratively decodes the selected subtree into a separate flat `ValueArena`. It returns a `View` backed by that detached immutable arena, not by the original owner. The detached view survives source invalidation/release and uses the same navigation/scalar/span surface. P4 language wrappers recursively convert this detached Core view to ordinary binding-owned values; P2 documentation must not call a Core handle a Python/Rust/TypeScript-owned value.

### Derived manifest runtime facts

P2 extends the closed `fastdb.payload.manifest.v1` schema with one required top-level `runtime` object derived only from `ResolvedSpec`. It contains exactly these fields:

| Field | Contract |
|---|---|
| `status` | `available` for `record.v1` after P2; `not_evaluated` for `object_graph.v1` until P3 |
| `layout_model` | `record_aos` for record; `object_pool_aos` for object graph, matching the Accepted profile model without claiming P3 bytes exist |
| `required_pools` | fixed-order subset of `utf8`, `utf16le`, `bytes`, `list_items`, `objects`, `references`, `roots`, derived by the exact rules below |
| `fixed_width_values_only` | true only when no reachable `str`, `wstr`, `bytes`, `list`, or `ref` value requires a variable descriptor/pool |
| `has_runtime_sized_regions` | true when value/cardinality counts or a reachable variable/list/object/reference/root pool makes region size depend on payload values |
| `reachable_type_count` | count of reachable source type nodes, distinct from the all-source-node runtime-ID count |
| `reachable_component_count` | count of components reachable from entries through by-value/ref edges appropriate to the profile |
| `reachable_list_type_count` | count of reachable list source nodes |

Manifest reachability starts at every entry root, follows `list.items`, follows component fields once per resolved component, and for object graph also follows each ref target component; visited-component marks break cycles without omitting their fields. Add `utf8`/`utf16le`/`bytes` for reachable `str`/`wstr`/`bytes`, `list_items` for any reachable list, `objects` for an object-graph reachable component, `references` for a reachable ref, and `roots` when an object-graph entry root/list leaf can designate a component or ref object. Emit the fixed order above regardless of discovery order. `fixed_width_values_only` is false exactly when `str`, `wstr`, `bytes`, `list`, or `ref` is reachable. `has_runtime_sized_regions` is true exactly when any entry is `many`, a variable/list kind is reachable, or object graph requires objects/references/roots; otherwise a fixed record spec reports false.

`required_pools` describes semantic storage requirements, not a second wire directory; P2 record bytes remain governed only by `fastdb.payload.bin.v1`, and P3 must map its already accepted object/ref/root needs into that same binary document. The object contains no instance value counts, byte sizes, routes, tables, CRM facts, or backing identity. Manifest changes never alter canonical payload JSON or its digest. `capabilities.direct_build` remains the sole direct-eligibility result: record becomes `eligible/record_layout_exact`; object graph remains `not_evaluated/runtime_slice_not_implemented` until P3. Record and object-graph manifest schema/goldens pin every field and canonical pool order.

### Public C ABI additions

The following pointer-free structs have exact prefixes on every supported target:

```c
#define FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE UINT32_C(88)
#define FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE UINT32_C(112)
#define FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE UINT32_C(104)
#define FDB_PAYLOAD_V1_EXECUTION_REPORT_V1_SIZE UINT32_C(72)

typedef struct fdb_payload_v1_builder_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t max_value_nodes;
    uint64_t max_list_elements;
    uint64_t max_text_bytes;
    uint64_t max_opaque_bytes;
    uint64_t max_nesting_depth;
    uint64_t max_total_builder_bytes;
    uint64_t reserved[4];
} fdb_payload_v1_builder_options_t;

typedef struct fdb_payload_v1_open_options {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t max_total_bytes;
    uint64_t max_regions;
    uint64_t max_entries;
    uint64_t max_components;
    uint64_t max_nesting_depth;
    uint64_t max_list_elements;
    uint64_t max_graph_objects;
    uint64_t max_string_bytes;
    uint64_t max_validation_work;
    uint64_t reserved[4];
} fdb_payload_v1_open_options_t;

typedef struct fdb_payload_v1_plan_info {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t total_bytes;
    uint64_t region_count;
    uint64_t logical_value_count;
    uint64_t list_element_count;
    uint64_t text_bytes;
    uint64_t opaque_bytes;
    uint64_t validation_work;
    uint32_t max_alignment;
    uint32_t direct_build_status;
    uint64_t reserved[4];
} fdb_payload_v1_plan_info_t;

typedef struct fdb_payload_v1_execution_report {
    uint32_t struct_size;
    uint32_t mode;
    uint32_t fallback_reason;
    uint32_t reserved32;
    uint64_t requested_bytes;
    uint64_t used_bytes;
    uint64_t staging_bytes;
    uint64_t region_count;
    uint64_t backing_capacity;
    uint64_t reserved64[2];
} fdb_payload_v1_execution_report_t;
```

Initializers accept null. Present outputs receive safe defaults/zeros and exact `struct_size`. A caller-supplied zero limit selects the corresponding default. Flags and all reserved fields are zero except the documented eager-text open bit.

The one pointer-bearing typed-input descriptor has a target-dependent `sizeof` prefix, just like the backing table; no cross-pointer-size numeric macro is invented:

```c
typedef struct fdb_payload_v1_fixed_run_v1 {
    uint32_t struct_size;
    uint32_t flags;
    const void* data;
    uint64_t data_byte_length;
    uint64_t count;
    uint64_t stride_bytes;
    const uint8_t* validity;
    uint64_t validity_byte_length;
    uint64_t validity_bit_offset;
    uint64_t reserved[4];
} fdb_payload_v1_fixed_run_v1_t;
```

Its initializer sets both pointers to C `NULL`, integers/reserved fields to zero, and `struct_size = sizeof(fdb_payload_v1_fixed_run_v1_t)`. The descriptor is borrowed only for `builder_value_fixed_run`; Core derives kind/width/nullability from the current expectation and never treats it as schema or wire layout.

`fdb_payload_v1_fixed_run_init` and `fdb_payload_v1_backing_init` assign each pointer/function-pointer member to C `NULL` explicitly rather than assuming an all-bits-zero pointer representation; they zero only integer reserved fields and set the current target's `sizeof` prefix.

The target-dependent callback table is initialized with `sizeof(fdb_payload_v1_backing_v1_t)` and is prefix-validated through `release`; its size is intentionally not a fixed numeric macro because it contains process pointers/function pointers:

```c
typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_reserve_fn)(
    void* context,
    uint32_t reserve_mode,
    uint64_t minimum_capacity,
    uint32_t alignment,
    void** out_owner_token,
    uint8_t** out_writable_data,
    uint64_t* out_capacity);

typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_write_fn)(
    void* context,
    void* owner_token,
    uint64_t offset,
    const uint8_t* source,
    uint64_t source_size);

typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_commit_fn)(
    void* context,
    void* owner_token,
    uint64_t used_size,
    const uint8_t** out_readable_data,
    uint64_t* out_readable_size);

typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_rollback_fn)(
    void* context,
    void* owner_token);

typedef fdb_payload_v1_status_t (*fdb_payload_v1_backing_retain_fn)(
    void* context,
    void* owner_token);

typedef void (*fdb_payload_v1_backing_release_fn)(
    void* context,
    void* owner_token);

typedef struct fdb_payload_v1_backing_v1 {
    uint32_t struct_size;
    uint32_t flags;
    void* context;
    fdb_payload_v1_backing_reserve_fn reserve;
    fdb_payload_v1_backing_write_fn write;
    fdb_payload_v1_backing_commit_fn commit;
    fdb_payload_v1_backing_rollback_fn rollback;
    fdb_payload_v1_backing_retain_fn retain;
    fdb_payload_v1_backing_release_fn release;
    uint64_t reserved[4];
} fdb_payload_v1_backing_v1_t;
```

Build execution requires reserve/commit/rollback/release; `write` may be null only when reserve returns a non-null stable writable base. External open requires retain/release; construction callbacks may be null because they are not called. Every callback receives the table's borrowed context plus the reservation/external owner token. Core copies the validated callback prefix into owner state; callback/context lifetime is therefore required through final release.

The opaque owner-token value may itself be null; Core never dereferences it and always passes it back to the callbacks. Ownership is defined by the successful reserve/retain and matching rollback/release calls, not by token non-nullness.

Callback status mapping is closed: reserve may return success, `DIRECT_UNAVAILABLE` for a direct request, or `ALLOCATION_FAILED`; write/commit/rollback/retain may return success or `ALLOCATION_FAILED`. Any other callback status becomes `BACKING_CONTRACT` with `callback` and `callback_status` details. A non-zero commit becomes `COMMIT_FAILED` unless allocation is the direct cause; a non-zero rollback becomes `ROLLBACK_FAILED` and preserves the original failure in details. Release returns void and must not unwind across C.

Ownership transitions are also closed. Core initializes every callback output to null/zero before entry and ignores all outputs on failure:

| Callback result | Ownership/output rule | Core's next action |
|---|---|---|
| `reserve` success | creates exactly one live **uncommitted reservation reference**; token may be null; capacity and writable-span combination must validate | write/commit, or exactly one rollback on later failure |
| `reserve` failure | creates no reservation/reference; callback must not retain storage; any written outputs are ignored | no rollback or release |
| `write` success | no ownership change | next ascending write or commit |
| `write` failure | reservation remains uncommitted | exactly one rollback |
| `commit` success | consumes the uncommitted state and yields exactly one committed initial owner reference plus the exact readable span | never rollback; transfer to owner, or release once if Core validation/publication later fails |
| `commit` failure | no committed reference/readable span is produced; reservation remains uncommitted; outputs ignored | exactly one rollback |
| `rollback` return | the reservation is dead after this one attempted call even if rollback itself reports failure | never release or retry rollback; report `ROLLBACK_FAILED` when needed |
| `retain` success | acquires exactly one committed reference | transfer to open owner or release once on later failure |
| `retain` failure | acquires no reference | no release |
| `release` | consumes exactly one committed reserve/retain reference | no further callback for that reference |

Core never calls a user callback while holding an unrelated Core mutex; invalidation specifically drops the access-barrier mutex before `release`. Callback functions are synchronous, non-throwing, and **must not re-enter FastDB operations on the same backing context/owner token when that operation can invoke or wait for the same callback** (including same-owner execute/open/invalidate/release). Such same-owner callback reentrancy is outside V1 and can deadlock or violate reference balance; unrelated spec queries and operations on a distinct context/token are allowed. This restriction, reason, impact, and the closure condition for any future reentrant contract are recorded in Issue 0002. Tests prove callbacks run outside unrelated locks and prove distinct-context reentry, but do not invoke forbidden same-owner reentry.

New opaque declarations are:

```c
typedef struct fdb_payload_v1_builder fdb_payload_v1_builder_t;
typedef struct fdb_payload_v1_plan fdb_payload_v1_plan_t;
typedef struct fdb_payload_v1_payload fdb_payload_v1_payload_t;
typedef struct fdb_payload_v1_view fdb_payload_v1_view_t;
typedef struct fdb_payload_v1_access fdb_payload_v1_access_t;
```

Builder and access are unique mutable/scoped handles with release only. Plan, payload, and view are immutable atomic retain/release handles.

Constants added in P2 are fixed-width macros:

```text
operation: BUILD=1<<2, OPEN=1<<3, VIEW=1<<4, MATERIALIZE=1<<5, INVALIDATE=1<<6
direct status: NOT_EVALUATED=0, ELIGIBLE=1, UNAVAILABLE=2
build policy: ALLOW_STAGING=1, REQUIRE_DIRECT=2
reserve/execution mode: DIRECT=1, STAGED=2
fallback: NONE=0, PLAN_REQUIRES_STAGING=1, BACKING_DECLINED_DIRECT=2
open flag: VALIDATE_TEXT_EAGER=1<<0
view kind: SEQUENCE=1, BOOL=2, U8=3, U16=4, U32=5, I32=6,
           U8N=7, U16N=8, F32=9, F64=10, STR=11, WSTR=12,
           BYTES=13, COMPONENT=14, LIST=15, REF=16
error: RUNTIME_UNAVAILABLE=2009, INVALID_TEXT_ENCODING=2010,
       NON_CANONICAL_BINARY=3009, INVALID_BINARY_VALUE=3010
```

After P2, record specs report operations compile/query/build/open/view/materialize/invalidate and direct status eligible with reason `record_layout_exact`; object-graph specs remain compile/query and not-evaluated with reason `runtime_slice_not_implemented`. These manifest/capability changes do not alter payload canonical bytes or digest.

The complete additive function set is:

```c
void fdb_payload_v1_builder_options_init(fdb_payload_v1_builder_options_t*);
void fdb_payload_v1_fixed_run_init(fdb_payload_v1_fixed_run_v1_t*);
void fdb_payload_v1_open_options_init(fdb_payload_v1_open_options_t*);
void fdb_payload_v1_plan_info_init(fdb_payload_v1_plan_info_t*);
void fdb_payload_v1_execution_report_init(fdb_payload_v1_execution_report_t*);
void fdb_payload_v1_backing_init(fdb_payload_v1_backing_v1_t*);

fdb_payload_v1_status_t fdb_payload_v1_builder_create(
    const fdb_payload_v1_spec_t*, const fdb_payload_v1_builder_options_t*,
    fdb_payload_v1_builder_t**, fdb_payload_v1_error_t**);
void fdb_payload_v1_builder_release(fdb_payload_v1_builder_t*);
fdb_payload_v1_status_t fdb_payload_v1_builder_entry_begin(
    fdb_payload_v1_builder_t*, uint32_t, uint64_t,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_null(
    fdb_payload_v1_builder_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_bool(
    fdb_payload_v1_builder_t*, uint8_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_u8(
    fdb_payload_v1_builder_t*, uint8_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_u16(
    fdb_payload_v1_builder_t*, uint16_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_u32(
    fdb_payload_v1_builder_t*, uint32_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_i32(
    fdb_payload_v1_builder_t*, int32_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_u8n_f64_bits(
    fdb_payload_v1_builder_t*, uint64_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_u16n_f64_bits(
    fdb_payload_v1_builder_t*, uint64_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_f32_bits(
    fdb_payload_v1_builder_t*, uint32_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_f64_bits(
    fdb_payload_v1_builder_t*, uint64_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_str(
    fdb_payload_v1_builder_t*, const uint8_t*, uint64_t,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_wstr(
    fdb_payload_v1_builder_t*, const uint16_t*, uint64_t,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_bytes(
    fdb_payload_v1_builder_t*, const uint8_t*, uint64_t,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_fixed_run(
    fdb_payload_v1_builder_t*, const fdb_payload_v1_fixed_run_v1_t*,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_component_begin(
    fdb_payload_v1_builder_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_list_begin(
    fdb_payload_v1_builder_t*, uint64_t, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_freeze(
    fdb_payload_v1_builder_t*, fdb_payload_v1_plan_t**,
    fdb_payload_v1_error_t**);

void fdb_payload_v1_plan_retain(fdb_payload_v1_plan_t*);
void fdb_payload_v1_plan_release(fdb_payload_v1_plan_t*);
fdb_payload_v1_status_t fdb_payload_v1_plan_info(
    const fdb_payload_v1_plan_t*, fdb_payload_v1_plan_info_t*,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_plan_execute(
    const fdb_payload_v1_plan_t*, uint32_t,
    const fdb_payload_v1_backing_v1_t*, fdb_payload_v1_payload_t**,
    fdb_payload_v1_execution_report_t*, fdb_payload_v1_error_t**);

fdb_payload_v1_status_t fdb_payload_v1_payload_open_copy(
    const fdb_payload_v1_spec_t*, const uint8_t*, uint64_t,
    const fdb_payload_v1_open_options_t*, fdb_payload_v1_payload_t**,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_payload_open_external(
    const fdb_payload_v1_spec_t*, const uint8_t*, uint64_t,
    const fdb_payload_v1_backing_v1_t*, void*,
    const fdb_payload_v1_open_options_t*, fdb_payload_v1_payload_t**,
    fdb_payload_v1_error_t**);
void fdb_payload_v1_payload_retain(fdb_payload_v1_payload_t*);
void fdb_payload_v1_payload_release(fdb_payload_v1_payload_t*);
fdb_payload_v1_status_t fdb_payload_v1_payload_sha256(
    const fdb_payload_v1_payload_t*,
    uint8_t[FDB_PAYLOAD_V1_SHA256_SIZE], fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_payload_profile(
    const fdb_payload_v1_payload_t*, fdb_payload_v1_profile_t*,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_payload_execution_report(
    const fdb_payload_v1_payload_t*, fdb_payload_v1_execution_report_t*,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_payload_binary_blob(
    const fdb_payload_v1_payload_t*, fdb_payload_v1_blob_t**,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_payload_acquire(
    const fdb_payload_v1_payload_t*, fdb_payload_v1_access_t**,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_payload_invalidate(
    fdb_payload_v1_payload_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_payload_entry_view(
    const fdb_payload_v1_payload_t*, uint32_t, fdb_payload_v1_view_t**,
    fdb_payload_v1_error_t**);

void fdb_payload_v1_view_retain(fdb_payload_v1_view_t*);
void fdb_payload_v1_view_release(fdb_payload_v1_view_t*);
fdb_payload_v1_status_t fdb_payload_v1_view_kind(
    const fdb_payload_v1_view_t*, uint32_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_is_null(
    const fdb_payload_v1_view_t*, uint8_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_length(
    const fdb_payload_v1_view_t*, uint64_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_at(
    const fdb_payload_v1_view_t*, uint64_t, fdb_payload_v1_view_t**,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_component_index(
    const fdb_payload_v1_view_t*, uint32_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_field_count(
    const fdb_payload_v1_view_t*, uint32_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_field(
    const fdb_payload_v1_view_t*, uint32_t, fdb_payload_v1_view_t**,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_bool(
    const fdb_payload_v1_view_t*, uint8_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_u8(
    const fdb_payload_v1_view_t*, uint8_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_u16(
    const fdb_payload_v1_view_t*, uint16_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_u32(
    const fdb_payload_v1_view_t*, uint32_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_i32(
    const fdb_payload_v1_view_t*, int32_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_u8n_f64_bits(
    const fdb_payload_v1_view_t*, uint64_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_u16n_f64_bits(
    const fdb_payload_v1_view_t*, uint64_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_f32_bits(
    const fdb_payload_v1_view_t*, uint32_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_get_f64_bits(
    const fdb_payload_v1_view_t*, uint64_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_acquire(
    const fdb_payload_v1_view_t*, fdb_payload_v1_access_t**,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_materialize(
    const fdb_payload_v1_view_t*, fdb_payload_v1_view_t**,
    fdb_payload_v1_error_t**);

void fdb_payload_v1_access_release(fdb_payload_v1_access_t*);
fdb_payload_v1_status_t fdb_payload_v1_access_payload_bytes(
    const fdb_payload_v1_access_t*, const uint8_t**, uint64_t*,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_access_str(
    const fdb_payload_v1_access_t*, const uint8_t**, uint64_t*,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_access_wstr(
    const fdb_payload_v1_access_t*, const uint16_t**, uint64_t*,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_access_bytes(
    const fdb_payload_v1_access_t*, const uint8_t**, uint64_t*,
    fdb_payload_v1_error_t**);
```

This is 66 additive exports and 99 total `fdb_payload_v1_*` symbols after P2. Task 9 freezes an exact 72-symbol intermediate allowlist and Task 10 the exact 99-symbol final allowlist; a count-only check remains forbidden.

All fallible functions retain the P1 meta-contract: non-null `out_error`, clear error/value outputs before ordinary validation, status equals owned error code, no partial handle publication, and caught exceptions. `payload_execution_report` on a payload created by `open_*` returns `PLAN_STATE` with `{"reason":"opened_payload_has_no_execution_report"}`; it never fabricates a direct/staged history.

---

## Task 1: Add the flat logical-value arena and complete internal record builder

**Files:**

- Create: `fastcarto/fastdb/src/payload/build/ValueArena.hpp`
- Create: `fastcarto/fastdb/src/payload/build/ValueArena.cpp`
- Create: `fastcarto/fastdb/src/payload/build/PayloadBuilder.hpp`
- Create: `fastcarto/fastdb/src/payload/build/PayloadBuilder.cpp`
- Create: `fastcarto/fastdb/src/payload/layout/TextEncoding.hpp`
- Create: `fastcarto/fastdb/src/payload/layout/TextEncoding.cpp`
- Create: `fastcarto/fastdb/src/payload/layout/NormalizedInteger.hpp`
- Create: `fastcarto/fastdb/src/payload/layout/NormalizedInteger.cpp`
- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Modify: `fastcarto/fastdb/src/payload/error/Error.cpp`
- Create: `tests/cpp/payload/test_payload_builder.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** `CompiledSpec::resolved()`, stable entry/component/field indexes, `TypeNode` bounds/nullability, shared `JsonPointer`, `Error`, and `Result`.

**Interfaces produced:** the exact `ValueNode`, `BuilderLimits`, `FixedRun`, `LogicalPayload`, and internal `PayloadBuilder` contract above; reusable strict UTF-8/UTF-16 validation; exact binary64 finite/range validation seam for later quantization; additive error constants/symbols `2009 RUNTIME_UNAVAILABLE` and `2010 INVALID_TEXT_ENCODING`.

- [ ] Add RED tests that compile explicit record specs and exercise `PayloadBuilder` directly:

  - empty payload and empty `many`;
  - every scalar, signed zero, infinities, distinct NaNs, normalized endpoints/ties;
  - `one` versus `many`, nullable present/null, null versus empty string/bytes/list;
  - nested/empty/reused components and list/component compositions;
  - a named `many + component` record-batch frame whose row count/component/field order come only from the compiled schema;
  - tightly packed and strided fixed runs for every fixed scalar representation, nullable validity with non-zero bit offset, fixed runs under scalar `many` and homogeneous list items, and equivalence to individual pushes;
  - transactional fixed-run rejection for zero count, short data/bitmap spans, stride/offset arithmetic, a non-fixed or different next expectation, invalid Boolean/range/null bits, and injected allocation failure;
  - entry authoring out of order while preserving stable entry roots;
  - duplicate entry, wrong cardinality count, wrong typed operation, invalid null, invalid Boolean byte, non-finite/out-of-range normalized value, invalid UTF-8, unpaired UTF-16 surrogate, missing entry/field, partial list, and mutation/re-freeze after successful freeze;
  - exact code/path/details for each failure and no arena mutation on a failed operation;
  - exact default limits and deterministic node/frame/root/text/wtext/opaque logical-byte charges, rejection immediately before allocation growth, and success when only the controlling limit is raised;
  - a 20,000-level nested-list build/freeze/release and high-fan-out component/list arena teardown without recursion;
  - `object_graph.v1` builder creation fails with `2009 RUNTIME_UNAVAILABLE`, not `PROFILE_VIOLATION` and not a partial builder.

- [ ] Configure/build the new test target and run:

  ```bash
  cmake -S fastcarto -B build/p2-task1 -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p2-task1 --target fastdb_payload_test_payload_builder --parallel
  ctest --test-dir build/p2-task1 -R '^payload\.payload_builder$' --output-on-failure
  ```

  Expected RED: `ValueArena`, `PayloadBuilder`, UTF helpers, and `RUNTIME_UNAVAILABLE` do not exist.

- [ ] Implement `TextEncoding` as one Core helper used by builder now and binary open later:

  ```cpp
  error::Result<void> validate_utf8(std::string_view bytes,
                                    const json::JsonPointer& path);
  error::Result<void> validate_utf16(const std::uint16_t* units,
                                     std::uint64_t count,
                                     const json::JsonPointer& path);
  void append_utf16le(std::vector<std::uint8_t>& output,
                      const std::uint16_t* units,
                      std::uint64_t count);
  ```

  It rejects overlong UTF-8, surrogate UTF-8, invalid continuation, truncation, and unpaired UTF-16; it contains no binding/platform text conversion.

- [ ] Implement the arena as vectors/indices and a byte store. Store binary32/binary64 input bits exactly, copy spans during the call, and use checked offsets/counts. Do not use a recursive variant, owning child pointer, or `std::function` recursion.
- [ ] Implement an explicit expectation-frame stack `{type_id,parent,last_child,remaining,path_state}`. Auto-close completed frames and update the parent's sibling links iteratively. Component frames enumerate exact stable fields; list frames enumerate one repeated item type.
- [ ] Make every authoring operation transactional: validate expected type/null/range/text/limits and reserve all required vector/byte capacity before publishing node/link/count changes. Implement the frozen row-batch semantics and fixed-run preflight without accepting a caller schema/layout. Allocation failure leaves the builder valid and retryable.
- [ ] Implement only the builder-called normalized input finite/range check from decomposed binary64 bits and store the logical bits, not a binding-rounded quantized code. Task 3 adds quantize/dequantize to this focused module together with their first encoder/open callers; Task 1 creates no unused function.
- [ ] Implement freeze as validate-all -> move arena/root indexes into immutable `LogicalPayload` -> seal. Failed validation returns the first deterministic missing path and does not seal.
- [ ] Update Issue 0002 with the exact internal builder surface and state plainly that no binary, plan, backing, public builder ABI, open, owner, or view exists yet.
- [ ] Run focused tests plus `payload.spec_parse`, `payload.spec_resolve`, `payload.compiled_spec`, `payload.spec_abi`, and the sanitizer build. Require no sanitizer finding and no recursion failure.
- [ ] Run `git diff --check` and commit:

  ```bash
  git add fastcarto/fastdb/src/payload/build \
    fastcarto/fastdb/src/payload/layout/TextEncoding.hpp \
    fastcarto/fastdb/src/payload/layout/TextEncoding.cpp \
    fastcarto/fastdb/src/payload/layout/NormalizedInteger.hpp \
    fastcarto/fastdb/src/payload/layout/NormalizedInteger.cpp \
    fastcarto/fastdb/include/fastdb_payload.h \
    fastcarto/fastdb/src/payload/error/Error.cpp \
    tests/cpp/payload/test_payload_builder.cpp tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): add portable record value builder"
  ```

## Task 2: Freeze the normative wire layout with the first encoder/open goldens

**Files:**

- Create: `schemas/fastdb.payload.bin.v1.md`
- Modify: `schemas/README.md`
- Modify: `MANIFEST.in`
- Create: `fastcarto/fastdb/src/payload/layout/CheckedMath.hpp`
- Create: `fastcarto/fastdb/src/payload/layout/BinaryFormat.hpp`
- Create: `fastcarto/fastdb/src/payload/layout/RuntimeSchema.hpp`
- Create: `fastcarto/fastdb/src/payload/layout/RuntimeSchema.cpp`
- Create: `fastcarto/fastdb/src/payload/layout/RecordLayout.hpp`
- Create: `fastcarto/fastdb/src/payload/layout/RecordLayout.cpp`
- Create: `fastcarto/fastdb/src/payload/build/RecordEncoder.hpp`
- Create: `fastcarto/fastdb/src/payload/build/RecordEncoder.cpp`
- Create: `fastcarto/fastdb/src/payload/view/Open.hpp`
- Create: `fastcarto/fastdb/src/payload/view/Open.cpp`
- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Modify: `fastcarto/fastdb/src/payload/error/Error.cpp`
- Extend: `tests/cpp/payload/GoldenCorpus.hpp`
- Extend: `tests/cpp/payload/GoldenCorpus.cpp`
- Create: `tests/cpp/payload/test_record_layout.cpp`
- Create: `tests/cpp/payload/test_record_binary.cpp`
- Create: `tests/golden/payload/v1/binary/index.json`
- Create: `tests/golden/payload/v1/binary/valid/empty.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/empty.sha256`
- Create: `tests/golden/payload/v1/binary/valid/fixed-scalars.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/fixed-scalars.sha256`
- Create: matching source-spec fixtures under `tests/golden/payload/v1/binary/spec/`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** frozen `LogicalPayload`, P1 resolved model/digest/indexes, and the exact normative layout in this plan.

**Interfaces produced:** `RuntimeSchema`, checked wire primitives, canonical `RecordLayout`, one bounded `ByteSink` writer, initial strict `open_record`, stable `NON_CANONICAL_BINARY`/`INVALID_BINARY_VALUE` errors, and the first independently reviewable bytes locking header/directories/scalar slots.

- [ ] Add RED layout tests for exact zero-based runtime type IDs, parent-before-child nested-list preorder, a lower-index unreachable component before a reachable component, finite component DAG reuse, reachability exclusion without renumbering, field offsets/alignments/stride, empty-component stride one, nullable bitmap positions, list-node inventory, and checked arithmetic at every `uint32_t`/`uint64_t` boundary.
- [ ] Add RED binary tests for exact empty/fixed-scalar golden hex, SHA-256 of decoded golden bytes, build determinism across repeated builders/plans, header/directory field offsets, the complete seven-kind region field matrix, zero-length region boundaries, pool-relative byte offsets, list-region-relative item indexes, exact partition consumption, zero padding, spec-digest embedding, signed zero/infinity/canonical NaN, and open observation of the same values.
- [ ] Create `binary/index.json` with exact schema `fastdb.payload.golden.binary-index.v1` and explicit ordered cases. Each success names one source spec, one C++ scenario key, one `.bin.hex`, and one independent `.sha256`; no filesystem discovery and no value JSON parser are allowed.
- [ ] Hand-review the initial hex against the tables in `fastdb.payload.bin.v1.md`, then compute the `.sha256` from decoded checked-in hex using a platform SHA-256 tool. The Core encoder/open code must never rewrite its expectations.
- [ ] Run:

  ```bash
  cmake -S fastcarto -B build/p2-task2 -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p2-task2 --target \
    fastdb_payload_test_record_layout fastdb_payload_test_record_binary --parallel
  ctest --test-dir build/p2-task2 -R '^payload\.(record_layout|record_binary)$' --output-on-failure
  ```

  Expected RED: no wire document, runtime schema, layout, encoder, open reader, or binary corpus exists.

- [ ] Implement `CheckedMath.hpp` with narrow, add, multiply, align-up, range-end, and little-endian load/store helpers returning `Result`, never unchecked casts. Unit-test `UINT64_MAX`, exact boundary, and zero cases.
- [ ] Implement `BinaryFormat.hpp` with named offsets/sizes/kinds and static assertions matching the normative tables. It may define internal descriptors but may not overlay untrusted bytes with packed/native structs; open uses byte loads.
- [ ] Implement runtime type-ID assignment and iterative reachable-component/type analysis exactly as frozen above. Assign every source node before applying the separate reachable set, keep `UINT32_MAX` reserved, and emit exact ID goldens. Cache reusable component layouts by stable component index; reject an internal by-value cycle/ref rather than recurse.
- [ ] Implement `RecordLayout::plan` for empty/fixed scalar/component-free logical payloads first, using the complete canonical directory algorithm already capable of listing future list/pool regions. Every offset/length/count is checked before allocation.
- [ ] Define the single writer seam:

  ```cpp
  class ByteSink {
  public:
      virtual ~ByteSink() = default;
      virtual error::Result<void> write(std::uint64_t offset,
                                        const std::uint8_t* data,
                                        std::uint64_t size) = 0;
  };

  error::Result<void> encode_record(const RecordLayout& layout,
                                    const build::LogicalPayload& values,
                                    ByteSink& sink);
  ```

  Implement header/directory/fixed scalar writes in ascending offset order with bounded local buffers. Do not build a hidden full image inside `encode_record`.
- [ ] Implement the first `open_record` as a byte-reading validator for the complete header/directory contract and the implemented fixed scalar slots. It returns a private immutable `PayloadIndex`; it never exposes a partially validated object.
- [ ] Write the normative Markdown document in the same commit. Include the zero-based all-source runtime-ID algorithm, every offset, the seven-kind field matrix/count unit, region order/zero-length boundary, component/validity rule, pool-relative/list-relative descriptor and exact partition rule, numeric canonicality/error mapping, overflow/alignment rule, open work accounting, resource-limit behavior, and forward-compatibility rejection stated in this plan; link the design/ADR/issues and golden index.
- [ ] Update packaging so the sdist includes `schemas/fastdb.payload.bin.v1.md`; inspect the archive rather than trusting the glob.
- [ ] Update Issue 0002: the wire contract and initial encoder/open exist, but variable pools, lists, public plan/backing/owner/views, complete C ABI, and full P2 remain open.
- [ ] Run focused/sanitizer suites, decode and hash every golden, validate the binary index as strict duplicate-free JSON, resolve Markdown links, and run `git diff --check`.
- [ ] Commit the encoder, exact document, and goldens atomically:

  ```bash
  git add schemas MANIFEST.in fastcarto/fastdb/src/payload/layout \
    fastcarto/fastdb/src/payload/build/RecordEncoder.hpp \
    fastcarto/fastdb/src/payload/build/RecordEncoder.cpp \
    fastcarto/fastdb/src/payload/view/Open.hpp \
    fastcarto/fastdb/src/payload/view/Open.cpp \
    fastcarto/fastdb/include/fastdb_payload.h \
    fastcarto/fastdb/src/payload/error/Error.cpp \
    tests/cpp/payload tests/golden/payload/v1/binary \
    tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): freeze portable record binary layout"
  ```

## Task 3: Complete fixed-width scalars and inline AoS components

**Files:**

- Modify: `fastcarto/fastdb/src/payload/layout/RuntimeSchema.*`
- Modify: `fastcarto/fastdb/src/payload/layout/RecordLayout.*`
- Modify: `fastcarto/fastdb/src/payload/layout/NormalizedInteger.hpp`
- Modify: `fastcarto/fastdb/src/payload/layout/NormalizedInteger.cpp`
- Modify: `fastcarto/fastdb/src/payload/build/RecordEncoder.*`
- Modify: `fastcarto/fastdb/src/payload/view/Open.*`
- Extend: `tests/cpp/payload/test_record_layout.cpp`
- Extend: `tests/cpp/payload/test_record_binary.cpp`
- Add: `tests/golden/payload/v1/binary/valid/numeric-edges.bin.hex`
- Add: `tests/golden/payload/v1/binary/valid/numeric-edges.sha256`
- Add: `tests/golden/payload/v1/binary/valid/nested-components.bin.hex`
- Add: `tests/golden/payload/v1/binary/valid/nested-components.sha256`
- Add: exact malformed fixtures/expectations for fixed/component cases
- Create: `tests/wasm/payload_runtime_harness.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `tests/golden/payload/v1/binary/index.json`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** canonical header/directories, runtime type IDs, flat logical arena.

**Interfaces produced:** deterministic build/open for all fixed-width scalars and arbitrary acyclic inline component reuse under `one` and `many`.

- [ ] Add RED cases for every fixed kind, nullable roots/fields, empty/nested/reused components, component values directly under entries, component-in-component, and large `many` AoS strides.
- [ ] Add exact numeric cases: minimum/maximum integers, `+0/-0`, positive/negative infinity, multiple input NaN payloads collapsing to one canonical NaN, u8n/u16n endpoints, just-in/out-of-range, and round-even lower/upper ties.
- [ ] Add malformed cases for invalid Boolean, non-canonical NaN, non-zero null slot, non-zero validity tail bit, non-zero field/tail padding, wrong stride/alignment/type ID, count multiplication overflow, and truncated component rows. Each names exact status/path/details.
- [ ] Run the fixed target and confirm RED on component/quantization/canonical-open gaps.
- [ ] Compute component layouts bottom-up with an explicit dependency stack and checked state marks. Put immediate nullable-field bitmap first, align each field slot, use stride one for empty components, and zero every gap/tail.
- [ ] Encode/decode fixed values through named little-endian helpers. Use `memcpy` only between same-width integer/IEEE bit representations; never reinterpret unaligned wire memory.
- [ ] Implement one exact Core normalized-integer helper using binary64 decomposition and a bounded limb vector; support big exponent gaps without overflow, compute nearest-even code by integer comparisons, and correctly round the decoded rational to binary64 bits. It must not depend on `long double`, current `fenv`, fast-math, or a binding. Encoder stores only the code; independently reviewed Python `fractions.Fraction` receipts may establish expected test bits but are verification inputs, never shipped runtime code.
- [ ] Make open validate canonical fixed bytes and produce field slot metadata in `PayloadIndex` without decoding the whole payload into a second value tree.
- [ ] Require repeated internal build -> bytes -> open -> observe parity for every case and byte equality for different input NaN payloads.
- [ ] Add a Core-only Emscripten executable target with output fixed at `build/p2-task3-wasm/wasm-tests/payload_runtime_harness.js`. Its C++ `main` uses internal Core APIs (not embind/TypeScript) to compile the pinned numeric spec, check normalized endpoints/ties and exact decoded bits, build/open the fixed-scalar golden, and compare exact bytes/hash. Run it through Node:

  ```bash
  emcmake cmake -S fastcarto -B build/p2-task3-wasm \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build build/p2-task3-wasm \
    --target fastdb_payload_wasm_runtime_harness --parallel
  node build/p2-task3-wasm/wasm-tests/payload_runtime_harness.js
  ```

  This is a Core/wasm32 contract proof only and creates no P4 TypeScript projection.
- [ ] Update Issue 0002 to mark fixed/AoS component binary semantics implemented while leaving variable/list/backing/lifetime/public ABI gaps explicit.
- [ ] Run Debug, Release, ASan+UBSan, allocation-failure layout/encode/open sweeps, full P1 regression, and `git diff --check`.
- [ ] Commit:

  ```bash
  git add fastcarto/fastdb/src/payload/layout \
    fastcarto/fastdb/src/payload/build/RecordEncoder.* \
    fastcarto/fastdb/src/payload/view/Open.* \
    tests/cpp/payload tests/wasm tests/golden/payload/v1/binary \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): encode portable AoS record values"
  ```

## Task 4: Add canonical UTF-8, UTF-16LE, and opaque-byte pools

**Files:**

- Modify: `fastcarto/fastdb/src/payload/layout/RecordLayout.*`
- Modify: `fastcarto/fastdb/src/payload/build/RecordEncoder.*`
- Modify: `fastcarto/fastdb/src/payload/view/Open.*`
- Modify: `fastcarto/fastdb/src/payload/layout/TextEncoding.*`
- Extend: `tests/cpp/payload/test_record_binary.cpp`
- Add: `tests/golden/payload/v1/binary/valid/text-bytes.bin.hex`
- Add: `tests/golden/payload/v1/binary/valid/text-bytes.sha256`
- Add: exact malformed pool/text fixtures and error expectations
- Modify: `tests/golden/payload/v1/binary/index.json`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** canonical global pool descriptors, shared text validation, component/entry traversal.

**Interfaces produced:** deterministic `str`/`wstr`/`bytes` planning, encoding, eager/lazy validation metadata, and open for every entry/component position.

- [ ] Add RED parity/goldens for null versus empty, ASCII and multi-byte UTF-8, BMP/supplementary UTF-16, embedded NUL, arbitrary bytes, repeated equal values without deduplication, `many`, nested component fields, zero-length pools, and exact traversal-order offsets.
- [ ] Add malformed fixtures for pool out-of-bounds/overflow/overlap, wrong pool kind/owner/alignment, odd UTF-16 offset/length, invalid UTF-8, unpaired UTF-16, alias/gap/out-of-order descriptors, non-zero null descriptors, and unconsumed pool tails.
- [ ] Confirm RED, then make `RecordLayout` sum exact pool sizes and include each reachable pool kind once even when zero length.
- [ ] Encode present values by canonical traversal with checked append cursors and no terminators/deduplication. Convert host `uint16_t` units to explicit UTF-16LE bytes.
- [ ] Extend open with partition cursors per pool and eager/lazy validation flags. Lazy mode stores validated descriptor bounds but validates text contents under each later access pin; bytes never receive text validation. Preserve `wstr` as checked UTF-16LE byte offsets/code-unit counts only—open must not create or cache a `uint16_t*` alias.
- [ ] Verify open never accepts a noncanonical alternative layout for the same logical strings/bytes.
- [ ] Update the issue, run focused/full/sanitizer suites and exact golden hash checks, then commit:

  ```bash
  git add fastcarto/fastdb/src/payload/layout \
    fastcarto/fastdb/src/payload/build/RecordEncoder.* \
    fastcarto/fastdb/src/payload/view/Open.* \
    tests/cpp/payload tests/golden/payload/v1/binary \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): add portable payload value pools"
  ```

## Task 5: Complete recursive lists and the full record non-`ref` algebra

**Files:**

- Modify: `fastcarto/fastdb/src/payload/layout/RuntimeSchema.*`
- Modify: `fastcarto/fastdb/src/payload/layout/RecordLayout.*`
- Modify: `fastcarto/fastdb/src/payload/build/RecordEncoder.*`
- Modify: `fastcarto/fastdb/src/payload/view/Open.*`
- Extend: `tests/cpp/payload/test_record_binary.cpp`
- Add: `tests/golden/payload/v1/binary/valid/nested-lists.bin.hex`
- Add: `tests/golden/payload/v1/binary/valid/nested-lists.sha256`
- Add: `tests/golden/payload/v1/binary/valid/component-list-composition.bin.hex`
- Add: `tests/golden/payload/v1/binary/valid/component-list-composition.sha256`
- Add: exact malformed list fixtures/error expectations
- Modify: `tests/golden/payload/v1/binary/index.json`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** stable list runtime type IDs and all fixed/variable value encodings.

**Interfaces produced:** deterministic build/open for null/empty/nested lists, nullable items, list/component compositions, and therefore the complete record-profile non-`ref` algebra.

- [ ] Add RED cases for null list versus empty, nullable scalar/component/list items, list-of-list, component-in-list, list-in-component, repeated component definitions across entries, strings/bytes inside nested lists, empty inner lists, and 20,000 levels under raised limits.
- [ ] Add malformed cases for item-region count/stride/validity mismatch, first-index/count overflow, gaps/overlap/aliasing/out-of-order partitions, invalid nested descriptor, non-zero null descriptor, list-element/depth/validation-work limits, and cyclic/missing runtime type metadata.
- [ ] Confirm RED, then aggregate one item region per reachable list runtime type ID and encode list descriptors/regions through explicit canonical traversal stacks. A list never creates a per-instance region.
- [ ] Extend open with checked per-list partition cursors and iterative nested validation. Require exact consumed element counts and zero unused validity bits.
- [ ] Prove planning, encoding, opening, validation failure cleanup, and logical observation stay iterative at deep nesting and terminate within explicit time/memory bounds.
- [ ] Extend the ordered corpus so every legal kind appears under root, component, and list contexts where the algebra allows it. Record the coverage matrix in the test source/report rather than asserting “all types” without names.
- [ ] Update Issue 0002: complete record binary/value algebra is implemented internally, but public plan/backing/owner/view/invalidation/ABI still prevents P2 closure.
- [ ] Run Debug, Release, ASan+UBSan, allocation failure, deep-limit, full P1, Python, and TypeScript/WASM regression gates; inspect all golden hashes and `git diff --check`.
- [ ] Commit:

  ```bash
  git add fastcarto/fastdb/src/payload/layout \
    fastcarto/fastdb/src/payload/build/RecordEncoder.* \
    fastcarto/fastdb/src/payload/view/Open.* \
    tests/cpp/payload tests/golden/payload/v1/binary \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): complete portable record binary algebra"
  ```

## Task 6: Add immutable BuildPlan and truthful heap/external final backing

**Files:**

- Create: `fastcarto/fastdb/src/payload/build/BuildPlan.hpp`
- Create: `fastcarto/fastdb/src/payload/build/BuildPlan.cpp`
- Create: `fastcarto/fastdb/src/payload/backing/Backing.hpp`
- Create: `fastcarto/fastdb/src/payload/backing/Backing.cpp`
- Create: `fastcarto/fastdb/src/payload/backing/HeapBacking.hpp`
- Create: `fastcarto/fastdb/src/payload/backing/HeapBacking.cpp`
- Modify: `fastcarto/fastdb/src/payload/build/PayloadBuilder.*`
- Modify: `fastcarto/fastdb/src/payload/build/RecordEncoder.*`
- Create: `tests/cpp/payload/test_payload_backing.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** frozen logical payload, canonical record layout, bounded `ByteSink` encoder.

**Interfaces produced:** immutable repeatable `BuildPlan`, exact plan facts, Core heap backing, validated external callback adapter, direct/staged execution bytes/report, and reservation ownership ready for `PayloadOwner`.

- [ ] Add RED tests for builder freeze returning a plan, exact plan totals/resource facts, caller-source mutation not observed after freeze, repeat execution, and concurrent execution of one plan against distinct contexts.
- [ ] Build a table-driven fake backing that records every callback argument and can inject failure at reserve/write N/commit/rollback/retain. Add RED cases for:

  - null/Core heap direct execution;
  - stable writable span direct execution;
  - range-write-only direct execution with ascending coverage and <=64 KiB source chunks;
  - backing-declined direct -> staged only under `ALLOW_STAGING`;
  - backing-declined direct -> exact `DIRECT_UNAVAILABLE` under `REQUIRE_DIRECT`;
  - direct/staged/Core heap byte-for-byte identity;
  - short capacity, misaligned non-null writable span, no writable span and no write callback, partial/overlapping write behavior, null/short/inconsistent commit readable span/size, and non-zero callback status;
  - a null owner-token value accepted and passed unchanged through commit/release;
  - reserve failure creates no ownership and triggers no rollback/release even if the callback writes garbage outputs;
  - successful reserve followed by write/commit failure triggers exactly one rollback; failed commit outputs are ignored;
  - rollback failure details preserving the original failure;
  - no rollback after commit success; later Core validation/publication failure releases the committed owner exactly once;
  - exactly one final release of each successfully published committed owner token;
  - callbacks run without unrelated Core locks, distinct-context callback reentry succeeds, and the documented same-context/token owner reentry precondition is present in Issue 0002;
  - allocation failure before reserve, after reserve, during staging, and while creating the result object.

- [ ] Run:

  ```bash
  cmake -S fastcarto -B build/p2-task6 -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p2-task6 --target fastdb_payload_test_payload_backing --parallel
  ctest --test-dir build/p2-task6 -R '^payload\.payload_backing$' --output-on-failure
  ```

  Expected RED: no immutable plan or final-backing state machine exists.

- [ ] Define immutable interfaces:

  ```cpp
  struct PlanInfo final {
      std::uint64_t total_bytes;
      std::uint64_t region_count;
      std::uint64_t logical_value_count;
      std::uint64_t list_element_count;
      std::uint64_t text_bytes;
      std::uint64_t opaque_bytes;
      std::uint64_t validation_work;
      std::uint32_t max_alignment;
      std::uint32_t direct_build_status;
  };

  struct ExecutionReport final {
      std::uint32_t mode;
      std::uint32_t fallback_reason;
      std::uint64_t requested_bytes;
      std::uint64_t used_bytes;
      std::uint64_t staging_bytes;
      std::uint64_t region_count;
      std::uint64_t backing_capacity;
  };

  class BuildPlan final {
  public:
      static error::Result<BuildPlan> create(build::LogicalPayload values);
      const PlanInfo& info() const noexcept;
      error::Result<PendingPayload> execute(
          std::uint32_t policy,
          const backing::Callbacks* callbacks) const;
  };
  ```

  `PendingPayload` is an internal move-only committed backing plus validated callback metadata; Task 7 turns it into a published owner only after open validation.

- [ ] Implement `BackingReservation` as an explicit state machine (`empty,reserved,committed,rolled_back`) whose destructor attempts rollback only for a live uncommitted reservation. Initialize callback outputs locally, adopt ownership only on success, and implement the frozen transition table exactly. It must never call user callbacks while holding unrelated Core locks.
- [ ] Implement heap backing through the same reservation/commit/release semantics, not a privileged encoder path. Null public backing selects it internally.
- [ ] Make direct range writes monotonic and complete; make staged execution call the same encoder into a heap sink and then copy those exact bytes. Do not keep two encoders or two layout traversals.
- [ ] On commit success, validate non-null readable base for non-zero size, exact used length, and capacity. A null token remains legal. Transfer rather than duplicate the reservation reference; any later failure uses release, never rollback.
- [ ] Update Issue 0002: plan/backing execution is internally complete and truthful, while open publication, views, lifetime barrier, and C/C++ APIs remain unavailable. Record the same-context/token callback reentrancy restriction with reason, impact, allowed distinct-context behavior, and closure criteria for a future reentrant contract.
- [ ] Run focused tests, the complete binary corpus, a 32-thread distinct-context plan execution test, ASan+UBSan, ThreadSanitizer where available, and allocation-failure balance. Run `git diff --check`.
- [ ] Commit:

  ```bash
  git add fastcarto/fastdb/src/payload/build \
    fastcarto/fastdb/src/payload/backing tests/cpp/payload \
    tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): execute portable payload build plans"
  ```

## Task 7: Harden copy/external open and publish immutable PayloadOwner

**Files:**

- Create: `fastcarto/fastdb/src/payload/view/PayloadOwner.hpp`
- Create: `fastcarto/fastdb/src/payload/view/PayloadOwner.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/Open.*`
- Modify: `fastcarto/fastdb/src/payload/build/BuildPlan.*`
- Modify: `fastcarto/fastdb/src/payload/backing/Backing.*`
- Create: `tests/cpp/payload/test_payload_open.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Add: remaining header/directory/resource malformed fixtures under `tests/golden/payload/v1/binary/invalid/`
- Modify: `tests/golden/payload/v1/binary/index.json`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** complete record reader, committed/external backing ownership, immutable compiled spec.

**Interfaces produced:** safe-default `OpenOptions`, copy/external open, complete-before-publication `PayloadIndex`, immutable shared `PayloadOwner`, digest/profile/optional execution facts, and exact failure cleanup.

- [ ] Add RED tests covering every open validation stage and limit from the frozen contract. Each invalid fixture must fail at the first deterministic path with no owner publication.
- [ ] Add ownership RED cases:

  - `open_copy` copies borrowed source and survives source mutation/free;
  - `open_external` calls retain before validation, balances release on validation/allocation failure, and holds one reference on success;
  - failed retain acquires no reference and receives no compensating release;
  - an unaligned external base opens identically to an aligned base because every wire load is byte-based;
  - execution-created owner adopts the reservation reference without an extra retain;
  - releasing plan/spec/source handles before owner does not change digest/profile/bytes;
  - owner copies share immutable state and one backing reference;
  - opened owners have no fabricated execution report;
  - committed backing corruption/wrong commit read span fails as `BACKING_CONTRACT`, publishes no owner, releases the committed reference exactly once, and never rolls back.

- [ ] Run the focused target and confirm RED.
- [ ] Define:

  ```cpp
  struct OpenOptions final {
      bool validate_text_eager;
      std::uint64_t max_total_bytes;
      std::uint64_t max_regions;
      std::uint64_t max_entries;
      std::uint64_t max_components;
      std::uint64_t max_nesting_depth;
      std::uint64_t max_list_elements;
      std::uint64_t max_graph_objects;
      std::uint64_t max_string_bytes;
      std::uint64_t max_validation_work;
  };

  class PayloadOwner final {
  public:
      static error::Result<PayloadOwner> open_copy(
          spec::CompiledSpec spec, const std::uint8_t* bytes,
          std::uint64_t size, OpenOptions options = default_open_options());
      static error::Result<PayloadOwner> open_external(
          spec::CompiledSpec spec, const std::uint8_t* bytes,
          std::uint64_t size, backing::RetainedBacking backing,
          OpenOptions options = default_open_options());
      const std::array<std::uint8_t, 32>& digest() const noexcept;
      spec::Profile profile() const noexcept;
      const std::optional<ExecutionReport>& execution_report() const noexcept;
  };
  ```

- [ ] Make `open_record` consume an immutable byte span and return `PayloadIndex` only after all eager checks pass. Store checked offsets/counts/type metadata, never naked pointers; owner access resolves pointers from retained base plus validated offsets.
- [ ] For external open, validate callback prefix/retain/release, call retain, install an RAII release guard only after retain success, then validate bytes through unaligned-safe loads. Publish the shared state only after success; dismiss the guard into owner state.
- [ ] For plan execution, validate the commit-returned image through the same `open_record` reader and attach the real execution report. A writer cannot bypass the reader's binary meaning.
- [ ] Map every malformed error through the frozen stable table to `/binary/header/...`, `/binary/regions/{i}/...`, `/binary/entries/{i}/...`, or the logical entry/field/list path. Details include exact expected/actual values without platform addresses. Boundary tests pin every validation-work charge, reject before its corresponding read, and succeed when only `max_validation_work` is raised.
- [ ] Update Issue 0002 with hardened owner/open facts and remaining view/barrier/public ABI gaps.
- [ ] Run all valid/malformed goldens, injected retain/allocation failures, Debug/Release/ASan+UBSan, full native P1/P2, and `git diff --check`.
- [ ] Commit:

  ```bash
  git add fastcarto/fastdb/src/payload/view \
    fastcarto/fastdb/src/payload/build/BuildPlan.* \
    fastcarto/fastdb/src/payload/backing \
    tests/cpp/payload tests/golden/payload/v1/binary \
    tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): open hardened portable payloads"
  ```

## Task 8: Add checked views, scoped access, detached materialization, and invalidation barrier

**Files:**

- Create: `fastcarto/fastdb/src/payload/view/AccessBarrier.hpp`
- Create: `fastcarto/fastdb/src/payload/view/AccessBarrier.cpp`
- Create: `fastcarto/fastdb/src/payload/view/View.hpp`
- Create: `fastcarto/fastdb/src/payload/view/View.cpp`
- Create: `fastcarto/fastdb/src/payload/view/Materialize.hpp`
- Create: `fastcarto/fastdb/src/payload/view/Materialize.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/PayloadOwner.*`
- Create: `tests/cpp/payload/test_checked_view.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** retained owner/index/backing and flat logical arena.

**Interfaces produced:** generation-captured `View`, unique `Access`, complete record navigation/scalar/span queries, detached materialized views, and idempotent drain-before-release invalidation.

- [ ] Add RED view matrix for every root/component/list/scalar/text/bytes context: kind, null, length, index bounds, field count/index, component index, exact scalar bits, normalized decode, and variable spans. Prove payload/UTF-8/bytes accesses borrow pinned backing, while `wstr` access returns an aligned Core-owned `uint16_t` buffer decoded from UTF-16LE for the access lifetime and never aliases wire/detached byte storage. Null getters return `UNEXPECTED_NULL`; wrong getters return `TYPE_MISMATCH`; index failures return `INDEX_OUT_OF_RANGE`.
- [ ] Add RED ownership/materialization cases: views retain owner state, owner handle release does not invalidate them, materialized subtrees preserve all null/empty/numeric/text/bytes distinctions, and detached views remain readable after source invalidation/release.
- [ ] Add the decisive barrier RED test:

  1. acquire a variable-span access and pause it;
  2. start invalidation on another thread and prove it blocks;
  3. prove new scalar/navigation/span access fails once invalidating starts;
  4. release the pin, observe invalidation complete, and immediately reuse/free the fake backing;
  5. prove all stale views return `VIEW_INVALIDATED` without touching reused bytes.

- [ ] Add concurrent tests for many short readers, two concurrent invalidators, repeat invalidation, view/access release races allowed by the contract, detached views, and a near-overflow generation hook. Run under ThreadSanitizer when available.
- [ ] Confirm RED, then implement `AccessBarrier` with one mutex/condition variable state exactly as frozen. Do not wait for view destruction; wait only for active access pins.
- [ ] Represent a view cursor as either `{shared PayloadOwner::State, captured_generation, validated node cursor}` or `{shared detached LogicalPayload, node cursor}`. Never cache a backing pointer in a view object.
- [ ] Implement scalar/navigation calls with an internal short pin. Implement `Access` as the only checked span-lifetime carrier; it owns the active count until release and exposes only the span kind it was acquired for. Payload/UTF-8/bytes keep a borrowed span, while `wstr` explicitly loads little-endian units into aligned owned storage under the same pin. Do not cast wire bytes to `uint16_t*` or claim all access kinds are zero-copy.
- [ ] Implement materialization with an explicit source traversal stack, a transactional destination arena, and one source pin covering the entire copy. Allocation/text failure publishes no partial detached view.
- [ ] Invalidate by preventing new pins, draining, detaching backing, advancing generation, and releasing the backing before success. Do not execute the external release callback while holding the barrier mutex: move the retained backing out and mark invalidated under lock, keep `invalidating=true`, unlock/release, relock/clear `invalidating`, notify waiters, then return. New pins remain impossible and concurrent invalidators cannot return before release completes.
- [ ] Update Issue 0002: internal P2 runtime/lifetime is complete; public C/C++ runtime surface, ABI freeze, CI/package proof, and final review remain.
- [ ] Run focused view/barrier tests, all binary goldens, 100,000-reader stress, ASan+UBSan, ThreadSanitizer if available, allocation-failure materialization sweeps, and full native regression. Run `git diff --check`.
- [ ] Commit:

  ```bash
  git add fastcarto/fastdb/src/payload/view tests/cpp/payload \
    tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): add checked portable payload views"
  ```

## Task 9: Expose builder, plan, backing, build, owner, and open through the C ABI

**Files:**

- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Modify: `fastcarto/fastdb/src/payload/abi/Handles.hpp`
- Modify: `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp`
- Split if needed: `fastcarto/fastdb/src/payload/abi/BuilderAbi.cpp`
- Split if needed: `fastcarto/fastdb/src/payload/abi/PayloadAbi.cpp`
- Modify: `fastcarto/fastdb/src/payload/spec/Manifest.*`
- Modify: `schemas/fastdb.payload.manifest.v1.schema.json`
- Regenerate: `fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc`
- Extend: `tests/cpp/payload/test_c_header_smoke.c`
- Create: `tests/cpp/payload/test_runtime_abi.cpp`
- Modify: `tests/cpp/payload/test_compiled_spec.cpp`
- Modify: `tests/cpp/payload/test_spec_abi.cpp`
- Modify: spec manifest goldens under `tests/golden/payload/v1/spec/`
- Modify: `tests/abi/fastdb_payload_v1_symbols.txt`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** reviewed internal builder/plan/backing/open/owner behavior and the frozen C declarations.

**Interfaces produced:** public P2 initializers/options, unique builder, immutable plan, callback backing, build/open/payload facts/binary-copy/invalidate C functions; record capabilities become truthful for the operations actually public in this task.

- [ ] Expand the pure-C smoke into compile -> builder -> fixed scalar entry -> freeze -> heap execute -> binary blob -> open copy -> digest/profile -> invalidate -> release, plus one exact owned builder error.
- [ ] Add RED ABI tests for every Task 9 function, null/error-sink rule, span null/zero pairs, options future tails/canaries, flags/reserved rejection, output clearing, object-graph runtime unavailable, typed record-batch authoring, fixed-run tight/strided/nullable input and descriptor failures, builder unique lifetime/state, plan atomic retain/release/concurrent queries, callback ordering/failures, external-open retain/release, opened execution-report absence, and invalidation/release balance.
- [ ] Add static assertions for 88/112/104/72 pointer-free struct sizes on arm64/x86_64/wasm32 C11 compilation. For fixed-run/backing pointer-bearing structs, assert field order with `offsetof`, initializer `struct_size == sizeof`, explicit null pointer initialization, and prefix acceptance on the current target rather than inventing a cross-pointer-size constant.
- [ ] Run the focused ABI target and exact symbol checker before adding declarations; expected RED is missing symbols and allowlist mismatch.
- [ ] Add exactly the 39 Task 9 exports from the frozen declarations without changing any P1 prefix/function:

  - 6 initializers: builder options, fixed run, open options, plan info, execution report, backing;
  - 20 builder functions: create/release/entry begin, null and nine fixed scalar operations, three variable operations, fixed run, component/list begin, and freeze;
  - 4 plan functions: retain/release/info/execute;
  - 9 payload functions: open copy/external, retain/release, SHA-256, profile, execution report, binary blob, and invalidate.

  The exact Task 9 inventory is:

  ```text
  fdb_payload_v1_builder_options_init
  fdb_payload_v1_fixed_run_init
  fdb_payload_v1_open_options_init
  fdb_payload_v1_plan_info_init
  fdb_payload_v1_execution_report_init
  fdb_payload_v1_backing_init
  fdb_payload_v1_builder_create
  fdb_payload_v1_builder_release
  fdb_payload_v1_builder_entry_begin
  fdb_payload_v1_builder_value_null
  fdb_payload_v1_builder_value_bool
  fdb_payload_v1_builder_value_u8
  fdb_payload_v1_builder_value_u16
  fdb_payload_v1_builder_value_u32
  fdb_payload_v1_builder_value_i32
  fdb_payload_v1_builder_value_u8n_f64_bits
  fdb_payload_v1_builder_value_u16n_f64_bits
  fdb_payload_v1_builder_value_f32_bits
  fdb_payload_v1_builder_value_f64_bits
  fdb_payload_v1_builder_value_str
  fdb_payload_v1_builder_value_wstr
  fdb_payload_v1_builder_value_bytes
  fdb_payload_v1_builder_value_fixed_run
  fdb_payload_v1_builder_value_component_begin
  fdb_payload_v1_builder_value_list_begin
  fdb_payload_v1_builder_freeze
  fdb_payload_v1_plan_retain
  fdb_payload_v1_plan_release
  fdb_payload_v1_plan_info
  fdb_payload_v1_plan_execute
  fdb_payload_v1_payload_open_copy
  fdb_payload_v1_payload_open_external
  fdb_payload_v1_payload_retain
  fdb_payload_v1_payload_release
  fdb_payload_v1_payload_sha256
  fdb_payload_v1_payload_profile
  fdb_payload_v1_payload_execution_report
  fdb_payload_v1_payload_binary_blob
  fdb_payload_v1_payload_invalidate
  ```

  `payload_acquire`, `payload_entry_view`, every `view_*`, and every `access_*` remain Task 10. Validate every struct before reading limits/callbacks, copy the known callback prefix into Core state, and preserve larger caller tails/canaries.
- [ ] Extend private handles:

  ```cpp
  struct fdb_payload_v1_builder { std::unique_ptr<build::PayloadBuilder> value; };
  struct fdb_payload_v1_plan {
      std::atomic<std::uint64_t> references{UINT64_C(1)};
      const build::BuildPlan value;
  };
  struct fdb_payload_v1_payload {
      std::atomic<std::uint64_t> references{UINT64_C(1)};
      const view::PayloadOwner value;
  };
  ```

  Builder release is unique/non-atomic; plan/payload reuse the proven saturating reference helpers.
- [ ] Project all typed builder operations without conversion beyond exact bits/spans. The fixed-run descriptor is borrowed for the call and maps to the one internal transactional operation; it never becomes a schema/layout authority. `builder_freeze` publishes a plan only after complete success and preserves the retryable builder on ordinary failure.
- [ ] Project plan execution and open through the one Core path. Initialize/copy plan/report structs only through known prefixes; never expose callback/native state or addresses in errors.
- [ ] `payload_binary_blob` copies under an internal access pin into an independently owned blob. It remains valid after payload invalidation/release and is not presented as zero-copy.
- [ ] Extend the closed manifest schema/Core builder with the exact required `runtime` object: canonical required pools, fixed/dynamic layout facts, reachable type/component/list counts, layout model, and runtime status. Record manifests/capabilities expose only public compile/query/build/open/invalidate/direct-eligible behavior available at this commit; object-graph operations/direct status stay compile/query/not-evaluated while its spec-derived runtime facts are still truthful. Regenerate record and object-graph manifest goldens without changing canonical payload/digest goldens.
- [ ] Update the sorted symbol allowlist to exactly 72 unique lines: the 33 P1 exports plus these 39 Task 9 exports, with zero other additions/removals. Keep the final expected 99-symbol target for Task 10.
- [ ] Update Issue 0002 with exact public operations and remaining checked-view/access/materialization/C++/final-quality gaps.
- [ ] Run C11 smoke, runtime/spec ABI, manifest schema/goldens, Debug/Release/ASan+UBSan, full native, Python, TypeScript/WASM, schema generation, ABI symbol check, and `git diff --check`.
- [ ] Commit:

  ```bash
  git add fastcarto/fastdb/include/fastdb_payload.h \
    fastcarto/fastdb/src/payload/abi fastcarto/fastdb/src/payload/spec \
    schemas/fastdb.payload.manifest.v1.schema.json \
    tests/cpp/payload tests/golden/payload/v1/spec \
    tests/abi/fastdb_payload_v1_symbols.txt tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): expose portable payload record runtime"
  ```

## Task 10: Expose checked view/access/materialization ABI and C++17 RAII

**Files:**

- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Modify: `fastcarto/fastdb/include/fastdb_payload.hpp`
- Modify: `fastcarto/fastdb/src/payload/abi/Handles.hpp`
- Modify: `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp`
- Split if needed: `fastcarto/fastdb/src/payload/abi/ViewAbi.cpp`
- Modify: `fastcarto/fastdb/src/payload/spec/Manifest.*`
- Modify: spec manifest goldens
- Extend: `tests/cpp/payload/test_c_header_smoke.c`
- Extend: `tests/cpp/payload/test_runtime_abi.cpp`
- Create: `tests/cpp/payload/test_runtime_cpp_facade.cpp`
- Modify: `tests/abi/fastdb_payload_v1_symbols.txt`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces consumed:** reviewed internal checked view/access/materialization/barrier and Task 9 owner ABI.

**Interfaces produced:** the complete frozen 99-symbol P2 C ABI and thin `Builder`, `BuildPlan`, `Payload`, `View`, `Access`, options/info/report RAII facade with no semantic duplication.

- [ ] Add RED C ABI tests for all navigation/scalar getters, null/type/index errors, view atomic lifetime, scoped payload/text/wtext/bytes access, zero-copy versus copied-wstr behavior, pointer borrow duration, detached materialization, invalidate blocking on an access handle, stale-view failure, and output clearing/allocation failure.
- [ ] Add RED C++ tests for move-only Builder/Access, copyable plan/payload/view, self-assignment, typed authoring, heap/external execution, open copy, copied binary blob, view traversal, scoped span lifetime, materialized view survival, invalidation, and copied `PayloadError` fields.
- [ ] Confirm RED and allowlist mismatch, then add exactly the remaining 27 exports:

  ```text
  fdb_payload_v1_payload_acquire
  fdb_payload_v1_payload_entry_view
  fdb_payload_v1_view_retain
  fdb_payload_v1_view_release
  fdb_payload_v1_view_kind
  fdb_payload_v1_view_is_null
  fdb_payload_v1_view_length
  fdb_payload_v1_view_at
  fdb_payload_v1_view_component_index
  fdb_payload_v1_view_field_count
  fdb_payload_v1_view_field
  fdb_payload_v1_view_get_bool
  fdb_payload_v1_view_get_u8
  fdb_payload_v1_view_get_u16
  fdb_payload_v1_view_get_u32
  fdb_payload_v1_view_get_i32
  fdb_payload_v1_view_get_u8n_f64_bits
  fdb_payload_v1_view_get_u16n_f64_bits
  fdb_payload_v1_view_get_f32_bits
  fdb_payload_v1_view_get_f64_bits
  fdb_payload_v1_view_acquire
  fdb_payload_v1_view_materialize
  fdb_payload_v1_access_release
  fdb_payload_v1_access_payload_bytes
  fdb_payload_v1_access_str
  fdb_payload_v1_access_wstr
  fdb_payload_v1_access_bytes
  ```

  View handles use saturating atomic references. Access handles uniquely own either one active owner pin or a detached-arena pin and release it exactly once.
- [ ] Make every checked pointer obtainable only from a live `Access`; no view function returns a backing pointer. `access_payload_bytes` accepts only payload access, and str/wstr/bytes accessors accept only the corresponding value access. `access_wstr` returns aligned Core-owned units produced by explicit LE loads; only payload/str/bytes accessors may alias backing bytes.
- [ ] Extend `fastdb_payload.hpp` as a header-only wrapper over C calls. It may convert float values to/from exact bits with `memcpy`, hold RAII handles, and expose typed spans while `Access` lives. It may not inspect a header/region descriptor, validate text, quantize, compute offsets, or recreate materialization.
- [ ] Every C++ conversion from public `uint64_t` length/count to `std::size_t` checks `value <= SIZE_MAX` first. The wasm32 facade must return a typed error rather than truncate an otherwise valid 64-bit ABI value.
- [ ] Record operations view/materialize in record manifest/capabilities only now that the public functions exist. Keep canonical spec/digest bytes unchanged.
- [ ] Update the exact sorted ABI allowlist to 99 unique lines and independently diff declared, defined, and exported names on native builds.
- [ ] Update Issue 0002: all P2 public behavior exists; only P2 final hardening/CI/package evidence and independent final review remain.
- [ ] Run pure-C compile/link, runtime ABI, C++ facade parity, exact 99-symbol gate, 32-thread immutable handle/view queries, active-pin barrier tests, Debug/Release/ASan+UBSan, ThreadSanitizer where available, Python/TypeScript/WASM regressions, and `git diff --check`.
- [ ] Commit:

  ```bash
  git add fastcarto/fastdb/include fastcarto/fastdb/src/payload/abi \
    fastcarto/fastdb/src/payload/spec tests/cpp/payload \
    tests/golden/payload/v1/spec tests/abi/fastdb_payload_v1_symbols.txt \
    tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): expose checked portable payload views"
  ```

## Task 11: Add binary robustness, CI/package gates, exact P2 truth, and final review evidence

**Files:**

- Create: `tests/fuzz/payload/fuzz_payload_open.cpp`
- Create: named seed files under `tests/fuzz/payload/binary-corpus/`
- Extend: `tests/cpp/payload/test_record_binary.cpp`
- Extend: `tests/cpp/payload/test_runtime_abi.cpp`
- Extend: `tests/wasm/payload_runtime_harness.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `.github/workflows/tests.yml`
- Modify: `README.md`
- Modify: `fastcarto/README.md`
- Modify: `CHANGELOG.md`
- Modify: `schemas/README.md`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`
- Modify: `docs/issues/README.md`
- Modify if needed: `MANIFEST.in`, `pyproject.toml` package include rules

**Interfaces produced:** deterministic binary-opening robustness target/corpus, exact ABI/CI/package gates, complete P2 requirement-to-proof mapping, and evidence-bounded public documentation.

- [ ] Add a native parser-robustness target that treats every input as a borrowed byte span, derives bounded open options from a few input bytes, opens it against one fixed matching spec plus a deliberate digest-mismatch spec, traverses/materializes/invalidate on success, checks status/error equality on failure, and balances every handle. It does not predict acceptance or duplicate the binary reader.
- [ ] Seed it with named valid empty/fixed/text/list payloads and malformed magic/length/offset/validity/text/list cases. Runtime-generated coverage corpus remains ignored; only reviewed named seeds are tracked.
- [ ] Run a clean Clang ASan+UBSan build and 10,000 local coverage-guided iterations, recording exact compiler/runtime/options/counts. If a local toolchain scheduler requires an adjustment, record the exact limit in Issue 0002 and keep the hosted default schedule unchanged; do not describe an adjusted run as a default run.
- [ ] Add/extend CI so standard `ubuntu-24.04` x64 and `macos-15` arm64 native jobs run the complete P2 suite, exact 99-symbol check, binary corpus/hash checks, and C/C++ ABI sizes. The Emscripten job builds and runs the Core-only Node harness, proving normalized endpoint/tie bits and fixed golden build/open independently of the P4 TypeScript API. Linux sanitizer CI runs both spec-compile and binary-open robustness smokes. The aggregate must distinguish expected path skips from required-job failure/cancellation exactly as P1 does.
- [ ] Add final hostile cases for every mandatory binary-hardening class: magic/version/profile/length/digest, truncation, directory arithmetic, region overlap/gap/misalignment/order, descriptor sizes/flags/reserved, validity length/tail bits/null storage, Boolean/NaN canonicality, string/list partitioning, UTF-8/UTF-16, counts/limits/work, allocation failure, backing callbacks, stale generation, and active-access drain.
- [ ] Add a complete P2 proof map from each accepted design Section 7-12/18 requirement and goal P2 bullet to exact test names/goldens. Explicitly name typed record-batch/fixed-run input, all non-`ref` types and entry/component/list contexts, stable runtime IDs, every region field/count unit/relative descriptor rule, manifest runtime facts, callback ownership/reentrancy, work accounting, copied `wstr`, and the wasm32 Core harness.
- [ ] Update public docs truthfully:

  - P1 compile/query and P2 record binary/runtime/lifetime are implemented and independently reviewed only after the final review passes;
  - `object_graph.v1` runtime, language projections, codegen, clean cut, 0.2.0 metadata, C-Two proof, and hosted outcomes remain open;
  - current 0.1.x call-db/`ColumnEngine` packages are migration inputs, not P2 authority;
  - no raw-file/object-storage, C-Two, Toodle, or Kubernetes capability is claimed.

- [ ] Update Issue 0002's program table: P2 becomes locally complete/frozen only after review; P3 becomes ready; hosted evidence remains pending if no push is authorized. Keep Issue 0002 Open.
- [ ] Run the complete clean local gate from new directories:

  ```bash
  tools/vendor_portable_payload_deps.sh --check
  python3 tools/generate_embedded_payload_schemas.py --check

  cmake -S fastcarto -B build/p2-final-debug \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p2-final-debug --parallel
  ctest --test-dir build/p2-final-debug --output-on-failure
  python3 tools/check_payload_abi_symbols.py --build-dir build/p2-final-debug

  cmake -S fastcarto -B build/p2-final-release \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build build/p2-final-release --parallel
  ctest --test-dir build/p2-final-release --output-on-failure

  cmake -S fastcarto -B build/p2-final-sanitize \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF \
    -DFASTDB_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  cmake --build build/p2-final-sanitize --parallel
  ctest --test-dir build/p2-final-sanitize --output-on-failure

  emcmake cmake -S fastcarto -B build/p2-final-wasm \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build build/p2-final-wasm \
    --target fastdb_payload_wasm_runtime_harness --parallel
  node build/p2-final-wasm/wasm-tests/payload_runtime_harness.js

  uv run pytest tests/python -q
  uv run python -m compileall -q python/fastdb4py tests/python
  uv build

  bash ts/build-wasm.sh
  npm --prefix ts/fastdb4ts run build
  npm run test:ts
  ```

  Expected: every available local command passes with exact counts recorded; sanitizers are clean; exact ABI is 99; the Core wasm32 harness passes; every valid/malformed golden matches; package builds succeed. Hosted jobs remain expected, not locally passed.

- [ ] Inspect wheel/sdist inventories. Require the normative binary Markdown, both schemas/digest pins, vendored provenance, public headers, and native library; forbid build paths, runtime corpus, `.DS_Store`, caches, or untracked generated payload images.
- [ ] Run forbidden boundary scans over Core/public headers/schemas for C-Two/Toodle/call-db/columnar ownership, `"text"`, public C++/platform types, a second binary parser/encoder/quantizer, and overstated 0.2.0 language parity.
- [ ] Validate every changed JSON/YAML, parse every fenced JSON with duplicate-key rejection, resolve every changed Markdown relative link/anchor, run the placeholder scan, `git diff --check`, and inspect exact tracked status.
- [ ] Commit the hardening/docs change:

  ```bash
  git add tests/fuzz/payload tests/cpp/payload tests/wasm \
    tests/cpp/CMakeLists.txt \
    .github/workflows/tests.yml README.md fastcarto/README.md CHANGELOG.md \
    schemas/README.md docs/issues MANIFEST.in pyproject.toml
  git commit -m "test(core): harden portable record runtime"
  ```

- [ ] Freeze the complete Task 11 diff/command evidence and send it to a fresh independent reviewer for both accepted-spec compliance and code quality. Fix every Critical/Important and every material Minor, rerun all affected broad gates, commit `fix(core): address portable record runtime review`, and return to the same reviewer until it reports review clean.
- [ ] After review clean, make a docs-only truth closure if needed, rerun link/JSON/status checks, and obtain a final read-only docs audit. Do not push/tag/publish/bump to 0.2.0.

---

## Per-Task Subagent and Review Protocol

For each Task 1-11:

1. Write a frozen task brief containing the task's exact starting commit, allowed files, consumed/produced interfaces, RED command/expected failure, broad gates, docs obligation, non-goals, and commit subject.
2. Dispatch one fresh implementation agent. No other implementation agent may modify the worktree concurrently.
3. Require strict RED evidence before production changes, then GREEN/focused/broad evidence and one scoped commit.
4. Freeze the resulting commit/diff/test report and dispatch a fresh read-only reviewer for both specification compliance and code quality.
5. Fix every Critical/Important finding. Fix a Minor when it exposes correctness, portability, lifetime, determinism, or coverage risk; otherwise record it in the exact owner issue with closure criteria.
6. Return fixes to the same reviewer. Mark the task complete in `.superpowers/sdd/progress.md` only after review clean.
7. Preserve ignored `.superpowers/sdd/p2-task-<n>-report.md` with exact RED/GREEN commands, platform/toolchain, counts, artifacts, limitations, implementation/review/fix commits, and reviewer result. Prefix every progress-ledger entry with `P2 Task <n>` so P1 evidence is never overwritten.

## Final P2 Review Gate

- [ ] Trace every goal P2 deliverable and accepted design Sections 7-12, 18.2, 18.4, 18.5, 18.7, and 20 step 2-3 to an exact implementation symbol plus test/golden.
- [ ] Confirm `schemas/fastdb.payload.bin.v1.md` and the encoder/open code agree on every offset, size, kind, order, alignment, zero rule, pool/list partition, numeric encoding, limit, and forward-compatibility rejection.
- [ ] Confirm no binding/legacy/C++ facade contains another parser, binary reader/writer, layout planner, quantizer, UTF validator, or generation authority.
- [ ] Confirm every public borrowed backing pointer requires a live access pin and invalidation drains pins before backing release/reuse.
- [ ] Confirm direct/staged reports from heap, stable-span, and range-write backings match actual allocation/copy behavior.
- [ ] Confirm object-graph runtime remains wholly P3, with one stable unavailable error and no partial pools/refs path.
- [ ] Confirm exact 99-symbol ABI and all fixed struct sizes/future-tail canaries on available targets; hosted target results are cited only when an authorized run exists.
- [ ] Search this plan and implementation docs for unresolved placeholders:

  ```bash
  rg -n 'TB[D]|TO[D]O|implement[[:space:]]+later|similar[[:space:]]+to[[:space:]]+Task|appropriate[[:space:]]+error[[:space:]]+handling|write[[:space:]]+tests[[:space:]]+for[[:space:]]+the[[:space:]]+above|fill[[:space:]]+in' \
    docs/superpowers/plans/2026-07-17-portable-payload-record-runtime.md \
    schemas/fastdb.payload.bin.v1.md docs/issues README.md fastcarto/README.md
  ```

  Expected: no matches.

- [ ] Re-run the complete clean Task 11 gate after the last review fix, not only focused tests.
- [ ] Require a clean tracked worktree/index and no build output/generated runtime corpus in the commit.
- [ ] Do not mark the overall FastDB goal complete. After P2 local freeze, write the P3 object-graph plan from the actual binary/backing/lifetime contracts and continue the active P1-P5 goal.

## P2 Completion Definition

P2 is locally complete only when a C and C++ caller can compile a `record.v1` spec, author every legal non-`ref` V1 value composition including the Core-defined record-batch frame and exact-width fixed runs, freeze an immutable repeatable plan, obtain byte-identical direct/staged deterministic payloads in heap or external backing, open the same hardened binary, traverse checked views, materialize a detached subtree, invalidate while correctly draining active access, and release every object/backing without leaks or races. The normative layout and ordered goldens must match exact bytes; runtime IDs, manifest facts, resource accounting, callback ownership, and native/wasm32 numeric behavior must have exact proofs; the public ABI must contain exactly 99 reviewed symbols; all available native/sanitizer/language/package regressions must pass; Issue 0002 must truthfully keep P3-P5/hosted evidence open; and an independent final reviewer must report no unresolved finding.

P2 completion does not claim object-graph runtime, Rust/Python/TypeScript portable parity, codegen, call-db clean cut, package version 0.2.0, C-Two composition, raw-file sharing, or any Toodle application capability.
