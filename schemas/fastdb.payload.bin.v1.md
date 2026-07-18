# `fastdb.payload.bin.v1`

This is the normative byte contract for the Core-owned portable record format.
It follows the accepted [portable payload design](../docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
and [ADR-0001](../docs/decisions/0001-portable-payload-core-authority.md).
The first independently reviewable images are in the
[ordered binary golden index](../tests/golden/payload/v1/binary/index.json).
[Issue 0002](../docs/issues/0002-portable-payload-foundation-implementation-status.md)
tracks executable coverage; an incomplete runtime tranche does not weaken a
V1 rule in this document.

All offsets below are decimal byte offsets from the payload start. Every
multibyte value is little-endian. Readers use bounded byte loads and must not
overlay untrusted bytes with packed or native structures. The caller's base
address may have any alignment.

## Runtime type IDs

`RuntimeSchema::compile` assigns a finite, zero-based `uint32_t` ID to every
source `TypeNode`, including unreachable-component nodes. `UINT32_MAX` is
permanently reserved as a wire sentinel.

1. Start `next_id` at zero.
2. Visit entries by stable entry index. Assign the root, then each nested
   `list.items` node immediately, outer-to-inner in parent-before-child
   preorder. A `component` or `ref` records its target but does not inline the
   target fields.
3. Visit all components by stable component index, reachable or not. Visit
   fields by stable field index and assign each root and nested list chain with
   the same preorder.
4. Compute record reachability in a separate iterative pass from every entry
   root, following `list.items` and by-value component field edges. It selects
   component layouts, list regions, and pools but never renumbers source nodes.
5. A reached `ref` is an internal record invariant failure. Reject
   `object_graph.v1` with `RUNTIME_UNAVAILABLE` before record runtime creation.

More than `UINT32_MAX` source nodes fails with `BUILDER_RESOURCE_LIMIT` at
`/runtime/types`. IDs are binary-local facts and do not change canonical JSON,
the spec digest, or P1 stable indexes.

## Header: 128 bytes

| Offset | Size | Field | V1 rule |
|---:|---:|---|---|
| 0 | 8 | magic | `46 44 42 50 41 59 31 00` (`FDBPAY1\0`) |
| 8 | 2 | major | `1` |
| 10 | 2 | minor | `0` |
| 12 | 4 | header size | `128` |
| 16 | 4 | profile | `FDB_PAYLOAD_PROFILE_RECORD_V1` (`1`) |
| 20 | 4 | flags | `0` |
| 24 | 8 | total length | exact supplied/committed length including final padding |
| 32 | 32 | spec SHA-256 | exact `CompiledSpec::digest()` bytes |
| 64 | 8 | region directory offset | `128` |
| 72 | 4 | region count | exact canonical descriptor count |
| 76 | 4 | region descriptor size | `56` |
| 80 | 8 | entry directory offset | `128 + region_count * 56` |
| 88 | 4 | entry count | exact compiled-spec entry count |
| 92 | 4 | entry descriptor size | `40` |
| 96 | 8 | root value count | checked sum of entry value counts |
| 104 | 24 | reserved | all zero |

V1 rejects any other magic, version, fixed size, profile, flags, or non-zero
reserved byte. Forward extension requires a new version or an accepted
flag/record-size contract; old readers fail closed instead of guessing tails.

## Region descriptor: 56 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | kind |
| 4 | 4 | flags, zero in V1 |
| 8 | 4 | owner index |
| 12 | 4 | runtime type ID |
| 16 | 8 | absolute data offset |
| 24 | 8 | byte length |
| 32 | 8 | logical element count |
| 40 | 4 | fixed stride, or zero for validity/pools |
| 44 | 4 | required alignment: exactly `1`, `2`, `4`, or `8` |
| 48 | 8 | reserved, zero |

Kinds are fixed-width integer macros, not C enums:

```text
1 ENTRY_VALUES
2 ENTRY_VALIDITY
3 LIST_ITEMS
4 LIST_VALIDITY
5 UTF8_POOL
6 UTF16_POOL
7 BYTES_POOL
```

Every field combination is normative.

| Kind | Present when | Owner index | Runtime type ID | Byte length | Logical count/unit | Stride | Align |
|---|---|---:|---:|---:|---|---:|---:|
| `ENTRY_VALIDITY` | root nullable | stable entry index | root type ID | `ceil(value_count/8)` | entry values | `0` | `1` |
| `ENTRY_VALUES` | always | stable entry index | root type ID | `value_count * root_slot_stride` | entry values | root stride | root align |
| `LIST_VALIDITY` | item nullable | owning list-node type ID | item type ID | `ceil(item_count/8)` | aggregated items for that node | `0` | `1` |
| `LIST_ITEMS` | every reachable list node | owning list-node type ID | item type ID | `item_count * item_slot_stride` | aggregated items for that node | item stride | item align |
| `UTF8_POOL` | reachable `str` | `UINT32_MAX` | `UINT32_MAX` | UTF-8 pool bytes | bytes | `0` | `1` |
| `UTF16_POOL` | reachable `wstr` | `UINT32_MAX` | `UINT32_MAX` | UTF-16LE bytes | code units (`byte_length/2`) | `0` | `2` |
| `BYTES_POOL` | reachable `bytes` | `UINT32_MAX` | `UINT32_MAX` | opaque bytes | bytes | `0` | `1` |

All products, bitmap ceilings, and `uint32_t` stride conversions are checked.
A stride above `UINT32_MAX` is `RESOURCE_LIMIT` at its logical type path.
Validity logical count is represented values/items, not bitmap bytes.

## Entry descriptor: 40 bytes

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | stable entry index, equal to directory position |
| 4 | 4 | root runtime type ID |
| 8 | 4 | cardinality: `1` one, `2` many |
| 12 | 4 | flags: bit 0 root nullable; other bits zero |
| 16 | 8 | value count (`1` for one) |
| 24 | 4 | `ENTRY_VALUES` region index |
| 28 | 4 | `ENTRY_VALIDITY` index or `UINT32_MAX` |
| 32 | 8 | reserved, zero |

## Canonical inventory and offsets

The directory contains exactly:

1. each entry by index: nullable validity first, then values;
2. each reachable list node by ascending runtime type ID: nullable-item
   validity first, then items;
3. UTF-8, UTF-16, and bytes pools in that order when reachable, even if empty.

The entry directory immediately follows the region directory. Data begins at
`align_up(entry_directory_end, 8)`. Each region starts at the smallest offset
at or after the previous end satisfying its declared alignment. All
inter-region and final `align_up(end, 8)` padding is zero. Regions are ordered,
non-overlapping, wholly in bounds, and exactly consume total length. Optional
gaps, aliases, alternative order, trailing bytes, and non-zero padding fail.

A required zero-length region remains present at the smallest aligned current
boundary. Adjacent empty regions may share it. Empty values/list regions keep
their non-zero stride/alignment; validity/pools keep zero stride. Open
recomputes the complete inventory from `RuntimeSchema`.

## Slots and inline components

| Type | Slot bytes | Align | Wire rule |
|---|---:|---:|---|
| `bool` | 1 | 1 | exactly `0` or `1` |
| `u8`, `u8n` | 1 | 1 | raw unsigned or quantized code |
| `u16`, `u16n` | 2 | 2 | little-endian unsigned or quantized code |
| `u32`, `i32`, `f32` | 4 | 4 | integer, two's-complement, or IEEE bits |
| `f64` | 8 | 8 | IEEE bits |
| `str`, `wstr`, `bytes` | 16 | 8 | pool-relative byte offset and byte length |
| `list` | 16 | 8 | list-region-relative item index and count |
| `component` | component stride | component align | inline AoS record |

A component starts with `ceil(nullable_immediate_field_count/8)` validity
bytes, low bit first in nullable-field order. Fields follow by stable index at
their smallest aligned offsets; tail padding reaches maximum field alignment.
An empty component is one zero byte with stride/alignment one. A containing
field's nullable bit controls its whole nested component. Layouts are cached by
stable component index and reused across the finite DAG; internal by-value
cycles/refs fail instead of recursing.

Sequence validity is also low-bit first: one present, zero null. Unused high
bits are zero. Non-nullable sequences have no validity region. Every null slot,
all descendants of a null component, and all unused fixed/padding bytes are
zero.

## Numeric canonicality

`f32` preserves finite values, signed zero, and infinities, but every NaN maps
to `0x7fc00000`. `f64` uses `0x7ff8000000000000`. Open rejects any other NaN
bits with `NON_CANONICAL_BINARY`. Boolean bytes outside `0/1` are
`INVALID_BINARY_VALUE`.

Normalized integers use the P1 bounds and
`round_even((value-min)*Q/(max-min))`. The Core decomposes finite binary64
values/bounds into sign, integer significand, and power-of-two exponent.
Core-owned bounded arbitrary-width arithmetic compares the exact rational and
rounds nearest ties-to-even without host floating arithmetic or caller rounding
mode. Dequantization rounds the exact result to binary64. Linux x86-64, macOS
arm64, and wasm32 must match independent endpoint/tie goldens. The initial Task
2 executable tranche returns `RUNTIME_UNAVAILABLE` with
`normalized_wire_quantization_unavailable`; Issue 0002 retains the exact
arithmetic/wasm proof as a later P2 gate. The types remain part of V1.

## Pools, lists, and exact partitions

- `str`, `wstr`, and `bytes` use one global pool per kind. Descriptor offsets
  are pool-relative bytes, never absolute payload offsets. Present values
  append exact bytes in canonical traversal order without terminators,
  deduplication, aliasing, or extra padding.
- `wstr` offsets/lengths are even bytes and the pool is valid UTF-16LE. Region
  logical count remains code units.
- A present empty variable value stores the current cursor and zero length. A
  null stores zero/zero and does not advance.
- Every reachable list node owns one aggregated items region and optional
  validity. Its descriptor index is relative to that exact item region, not a
  byte offset or another list type.
- Every present descriptor starts at the current cursor, including empties,
  then advances by checked length/count. Final pool cursors equal pool byte
  lengths; final list cursors equal logical counts. Gaps, overlap, aliases,
  reordering, and unconsumed tails fail.
- Nested list descriptors address the separately aggregated child-list region.

Traversal is entry index, entry value index, component field/list item index,
depth first with an explicit stack. Encoding, aggregation, validation, views,
and materialization share it. Variable pools/lists are normative but remain
outside this initial encoder/open tranche, as tracked in Issue 0002.

## Errors and checked arithmetic

| Failure | Stable error |
|---|---|
| bad magic | `INVALID_MAGIC` (`3001`) |
| unsupported version | `UNSUPPORTED_BINARY_VERSION` (`3002`) |
| wire arithmetic overflow | `LENGTH_OVERFLOW` (`3003`) |
| range outside bytes | `OUT_OF_BOUNDS` (`3004`) |
| descriptor offset misaligned | `MISALIGNED` (`3005`) |
| spec digest mismatch | `DIGEST_MISMATCH` (`3006`) |
| caller open/layout limit | `RESOURCE_LIMIT` (`3008`) |
| wrong inventory/fields/order, non-zero reserved/padding/null/tail, non-canonical NaN/partition | `NON_CANONICAL_BINARY` (`3009`) |
| bounded logical value invalid for type | `INVALID_BINARY_VALUE` (`3010`) |
| malformed UTF-8/UTF-16LE | `INVALID_TEXT_ENCODING` (`2010`) |

Builder input failures `2011`–`2013` remain builder failures. They do not
replace binary/layout/open `3003`, `3004`, or `3008`; malformed caller bytes
are not collapsed to `INVALID_ARGUMENT` or `INTERNAL`. Every add, multiply,
narrow, align-up, range end, bitmap ceiling, offset, count, work unit, and
allocation boundary is checked before access or growth.

## Hardened open and resource limits

Open publishes an immutable `PayloadIndex` only after complete validation:

1. option prefix, flags/reserved, and limits;
2. supplied length before the fixed header;
3. magic/version/profile/flags/total/digest/header reserved;
4. checked directory ranges and exact spec/runtime counts;
5. canonical descriptor inventory/order/fields/offsets/padding/non-overlap;
6. validity lengths/tails, nullable rules, component maps, null-zero, Boolean;
7. numeric canonicality, descriptor arithmetic, partitions, UTF-8/UTF-16LE;
8. exact list/pool consumption and final total.

Validation is iterative. Work charges one unit for the fixed header, each
region descriptor, each entry descriptor, each inspected validity byte, each
inspected alignment/final-padding byte, and each logical slot. A null scalar,
list, or descriptor is one slot. A null component is one slot plus each inline
byte scanned to prove descendant/padding zero. Eager text adds one per UTF-8
byte and UTF-16 code unit. Present fixed bytes and opaque contents have no
extra per-byte charge. Add and limit comparison precede the read.

`FDB_PAYLOAD_OPEN_VALIDATE_TEXT_EAGER` is bit zero and enabled by the V1
initializer. If cleared, structural validation still completes; later checked
string access validates its selected span under the access pin and retained
work/string limits independently.

| Limit | Default |
|---|---:|
| total bytes | 1 GiB |
| regions | 1,000,000 |
| entries | 65,536 |
| components | 65,536 |
| nesting depth | 1,024 |
| list elements | 10,000,000 |
| graph objects | 10,000,000 (accepted, unused by record V1) |
| string bytes | 1 GiB |
| validation work | 100,000,000 |

These zero-selecting defaults protect untrusted input but are not format
maxima. Raising a limit never changes canonical bytes or identity.

## Governance links

- [Deferred capabilities](../docs/issues/0001-portable-payload-deferred-capabilities.md)
- [Implementation status](../docs/issues/0002-portable-payload-foundation-implementation-status.md)
- [Source and manifest schemas](README.md)
