# `graph-null-empty` layout receipt

- Binary profile: `object_graph.v1` (`2`); total length `744`;
  root-value count `2`; graph-object count `1`;
  eager validation work `69`.
- Header: `[0,128)`; region directory: `[128,576)` with
  8 canonical 56-byte descriptors. The first data boundary is
  offset `616`; directory padding and every unlisted byte span are zero.
- Root and ref coordinates are little-endian `uint64_t` slots selected by the
  compiled type. No separate root/reference table exists.

| Region | Kind | Owner | Runtime type | Offset | Bytes | Elements | Stride | Align |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | ENTRY_VALIDITY | 0 | 0 | 616 | 1 | 2 | 0 | 1 |
| 1 | ENTRY_VALUES | 0 | 0 | 624 | 16 | 2 | 8 | 8 |
| 2 | OBJECT_VALUES | 1 | UINT32_MAX | 640 | 104 | 1 | 104 | 8 |
| 3 | LIST_VALIDITY | 5 | 6 | 744 | 0 | 0 | 0 | 1 |
| 4 | LIST_ITEMS | 5 | 6 | 744 | 0 | 0 | 1 | 1 |
| 5 | UTF8_POOL | 4294967295 | UINT32_MAX | 744 | 0 | 0 | 0 | 1 |
| 6 | UTF16_POOL | 4294967295 | UINT32_MAX | 744 | 0 | 0 | 0 | 2 |
| 7 | BYTES_POOL | 4294967295 | UINT32_MAX | 744 | 0 | 0 | 0 | 1 |

Hand-audited coordinates and content:

- Entry directory is `[576,616)`. Validity byte `01` means root `Node/0` is
  present and the second root is null; `[624,640)` contains ID `0` followed by
  the required zeroed null slot. Alignment padding `[617,624)` is zero.
- Canonical component order is `Meta,Node`; only Node owns an identity pool.
  Node object `0` at `[640,744)` begins with validity byte `0e`: null text,
  present-empty wide/bytes/list values, null inline Meta, and null ref. Every
  zero-length descriptor is `(0,0)` and every absent slot plus component
  padding is zero.
- The empty list validity/items and all three empty variable pools are still
  present at the shared canonical boundary `744`. The UTF-16 pool keeps
  alignment `2` even at zero length; there is no final padding.

The descriptor order is entries, identity-component object pools, reachable
list nodes by runtime type, then UTF-8, UTF-16LE, and opaque byte pools. Empty
required regions remain present at their shared canonical boundary.

Independent binary receipt:

```sh
xxd -r -p valid/graph-null-empty.bin.hex | shasum -a 256
```

Expected SHA-256:
`467739a1c928280e2eb57f8acce1bc66df217874f245d57392cf558fee930edc`.
