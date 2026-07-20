# `graph-all-values` layout receipt

- Binary profile: `object_graph.v1` (`2`); total length `1304`;
  root-value count `10`; graph-object count `2`;
  eager validation work `146`.
- Header: `[0,128)`; region directory: `[128,856)` with
  13 canonical 56-byte descriptors. The first data boundary is
  offset `1056`; directory padding and every unlisted byte span are zero.
- Root and ref coordinates are little-endian `uint64_t` slots selected by the
  compiled type. No separate root/reference table exists.

| Region | Kind | Owner | Runtime type | Offset | Bytes | Elements | Stride | Align |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | ENTRY_VALUES | 0 | 0 | 1056 | 8 | 1 | 8 | 8 |
| 1 | ENTRY_VALUES | 1 | 1 | 1064 | 8 | 1 | 8 | 8 |
| 2 | ENTRY_VALIDITY | 2 | 2 | 1072 | 1 | 2 | 0 | 1 |
| 3 | ENTRY_VALUES | 2 | 2 | 1080 | 16 | 2 | 8 | 8 |
| 4 | ENTRY_VALUES | 3 | 3 | 1096 | 3 | 3 | 1 | 1 |
| 5 | ENTRY_VALUES | 4 | 4 | 1100 | 6 | 3 | 2 | 2 |
| 6 | OBJECT_VALUES | 0 | UINT32_MAX | 1112 | 32 | 1 | 32 | 8 |
| 7 | OBJECT_VALUES | 2 | UINT32_MAX | 1144 | 120 | 1 | 120 | 8 |
| 8 | LIST_VALIDITY | 22 | 23 | 1264 | 1 | 3 | 0 | 1 |
| 9 | LIST_ITEMS | 22 | 23 | 1268 | 12 | 3 | 4 | 4 |
| 10 | UTF8_POOL | 4294967295 | UINT32_MAX | 1280 | 8 | 8 | 0 | 1 |
| 11 | UTF16_POOL | 4294967295 | UINT32_MAX | 1288 | 6 | 3 | 0 | 2 |
| 12 | BYTES_POOL | 4294967295 | UINT32_MAX | 1294 | 3 | 3 | 0 | 1 |

Hand-audited coordinates and content:

- Entry directory `[856,1056)` contains five descriptors with root counts
  `1,1,2,3,3`. Entry values encode Node ID `0`, Asset ID `0`, nullable Node
  refs `[0,null]` with validity byte `01`, `u8n` codes `00 80 ff`, and `u16n`
  codes `0000 0080 ffff`.
- Canonical component order is `Asset,Inline,Node`; only Asset and Node own
  identity pools. Asset object `0` occupies `[1112,1144)`: validity byte `01`,
  the `str` descriptor `(offset=0,bytes=4)`, and owner ref `Node/0`.
- Node object `0` occupies `[1144,1264)`. Its fixed prefix contains Bool `1`,
  `u8=0x12`, `u16=0x3456`, `u32=0x789abcde`, `i32=-1234567`, midpoint
  normalized codes, canonical f32/f64 NaNs, then descriptors
  `str=(0,4)`, `wstr=(0,6)`, `bytes=(0,3)`. The inline component has a null
  Bool and `u16=0xbeef`; its list descriptor is `(first=0,count=3)`; refs are
  `Node/0` and `Asset/0`.
- List validity is `05`; item bytes are negative zero, a zeroed null slot, and
  canonical f32 NaN. UTF-8 bytes are `same` twice (`73616d6573616d65`), proving
  occurrences are not deduplicated. UTF-16LE is `41003dd800de`; opaque bytes
  are `00ff7e`.
- Zero spans are `[1073,1080)`, `[1099,1100)`, `[1106,1112)`,
  `[1265,1268)`, and final padding `[1297,1304)`.

The descriptor order is entries, identity-component object pools, reachable
list nodes by runtime type, then UTF-8, UTF-16LE, and opaque byte pools. Empty
required regions remain present at their shared canonical boundary.

Independent binary receipt:

```sh
xxd -r -p valid/graph-all-values.bin.hex | shasum -a 256
```

Expected SHA-256:
`728c4880df6797954f36e6af1ad0991e739f1c4da93b33e2917cceaac91f866c`.
