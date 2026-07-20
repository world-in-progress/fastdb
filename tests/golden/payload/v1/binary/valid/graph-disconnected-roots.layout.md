# `graph-disconnected-roots` layout receipt

- Binary profile: `object_graph.v1` (`2`); total length `328`;
  root-value count `2`; graph-object count `2`;
  eager validation work `30`.
- Header: `[0,128)`; region directory: `[128,240)` with
  2 canonical 56-byte descriptors. The first data boundary is
  offset `280`; directory padding and every unlisted byte span are zero.
- Root and ref coordinates are little-endian `uint64_t` slots selected by the
  compiled type. No separate root/reference table exists.

| Region | Kind | Owner | Runtime type | Offset | Bytes | Elements | Stride | Align |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | ENTRY_VALUES | 0 | 0 | 280 | 16 | 2 | 8 | 8 |
| 1 | OBJECT_VALUES | 0 | UINT32_MAX | 296 | 32 | 2 | 16 | 8 |

Hand-audited coordinates and content:

- Entry directory is `[240,280)` and declares two roots. `[280,296)` is the
  exact little-endian ID sequence `[0,1]`; target traversal does not reorder
  the object pool.
- Node object `0` is `[296,312)`: null-ref validity byte `00`, three zero
  alignment bytes, `u32=11`, and a zeroed null-ref slot. Object `1` is
  `[312,328)` with the same shape and `u32=22`.
- Directory and data are contiguous; there is no inter-region or final
  padding. Both independently rooted objects are required by reachability.

The descriptor order is entries, identity-component object pools, reachable
list nodes by runtime type, then UTF-8, UTF-16LE, and opaque byte pools. Empty
required regions remain present at their shared canonical boundary.

Independent binary receipt:

```sh
xxd -r -p valid/graph-disconnected-roots.bin.hex | shasum -a 256
```

Expected SHA-256:
`d97eb8a39c8d59dba1f2950b0236ccb50de55090be2b6f9c55bd7e85c1791d3e`.
