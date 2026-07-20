# `graph-nested-lists` layout receipt

- Binary profile: `object_graph.v1` (`2`); total length `1296`;
  root-value count `4`; graph-object count `1`;
  eager validation work `99`.
- Header: `[0,128)`; region directory: `[128,968)` with
  15 canonical 56-byte descriptors. The first data boundary is
  offset `1048`; directory padding and every unlisted byte span are zero.
- Root and ref coordinates are little-endian `uint64_t` slots selected by the
  compiled type. No separate root/reference table exists.

| Region | Kind | Owner | Runtime type | Offset | Bytes | Elements | Stride | Align |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | ENTRY_VALUES | 0 | 0 | 1048 | 8 | 1 | 8 | 8 |
| 1 | ENTRY_VALIDITY | 1 | 1 | 1056 | 1 | 3 | 0 | 1 |
| 2 | ENTRY_VALUES | 1 | 1 | 1064 | 48 | 3 | 16 | 8 |
| 3 | OBJECT_VALUES | 1 | UINT32_MAX | 1112 | 40 | 1 | 40 | 8 |
| 4 | LIST_VALIDITY | 1 | 2 | 1152 | 1 | 2 | 0 | 1 |
| 5 | LIST_ITEMS | 1 | 2 | 1160 | 32 | 2 | 16 | 8 |
| 6 | LIST_VALIDITY | 2 | 3 | 1192 | 1 | 1 | 0 | 1 |
| 7 | LIST_ITEMS | 2 | 3 | 1194 | 2 | 1 | 2 | 2 |
| 8 | LIST_ITEMS | 4 | 5 | 1200 | 32 | 2 | 16 | 8 |
| 9 | LIST_VALIDITY | 5 | 6 | 1232 | 1 | 2 | 0 | 1 |
| 10 | LIST_ITEMS | 5 | 6 | 1233 | 2 | 2 | 1 | 1 |
| 11 | LIST_VALIDITY | 7 | 8 | 1235 | 1 | 3 | 0 | 1 |
| 12 | LIST_ITEMS | 7 | 8 | 1240 | 48 | 3 | 16 | 8 |
| 13 | LIST_VALIDITY | 8 | 9 | 1288 | 1 | 2 | 0 | 1 |
| 14 | LIST_ITEMS | 8 | 9 | 1290 | 4 | 2 | 2 | 2 |

Hand-audited coordinates and content:

- Entry directory `[968,1048)` has root counts `1,3`. The root slot is object
  ID `0`. `matrices` validity is `05`; its three descriptors are
  `(0,2)`, zeroed null, and `(2,0)`.
- Canonical component order is `Box,Node`; Node is the sole identity pool.
  Node object `0` at `[1112,1152)` carries matrix descriptor `(0,3)` and an
  inline Box whose nullable list is present with descriptor `(0,2)`.
- Runtime-list partitions are exact and monotonic: region pairs describe
  `2,1,2,2,3,2` items. Their checked bytes include `(0,1)/(1,0)`, scalar `9`,
  `(0,2)/(2,0)`, scalar validity `01` with `07 00`, and outer
  `(0,2)/null/(2,0)` followed by scalar validity `01` with `1,0`.
- Zero spans are `[1057,1064)`, `[1153,1160)`, `[1193,1194)`,
  `[1196,1200)`, `[1236,1240)`, `[1289,1290)`, and final padding
  `[1294,1296)`.

The descriptor order is entries, identity-component object pools, reachable
list nodes by runtime type, then UTF-8, UTF-16LE, and opaque byte pools. Empty
required regions remain present at their shared canonical boundary.

Independent binary receipt:

```sh
xxd -r -p valid/graph-nested-lists.bin.hex | shasum -a 256
```

Expected SHA-256:
`f120410724051a31185c65a5b8f0e4c3f6d6ece4496981ec88a02dc904b2c180`.
