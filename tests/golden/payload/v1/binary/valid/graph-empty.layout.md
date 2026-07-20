# `graph-empty` layout receipt

- Binary profile: `object_graph.v1` (`2`); total length `280`; root-value
  count `0`; spec SHA-256
  `7840e3a36978b51412dba2668f7921e0106d6801be92aaab18e84da6869e0c80`.
- Header: `[0,128)`; region directory: `[128,240)` with two 56-byte
  descriptors; entry directory: `[240,280)` with one 40-byte descriptor.
- Region 0 is `ENTRY_VALUES`: owner entry `0`, runtime type `0`, offset
  `280`, byte length/count `0`, stride/alignment `8/8`.
- Region 1 is `OBJECT_VALUES`: owner component `0` (`Node`), runtime type
  sentinel `UINT32_MAX`, offset `280`, byte length/count `0`,
  stride/alignment `1/1`.
- Entry 0 is cardinality `many`, non-nullable, and points to region 0. Its
  required zero-count identity pool is region 1. Both empty regions share the
  canonical boundary at byte `280`; there are no root slots or padding spans.

Independent binary receipt:

```sh
xxd -r -p valid/graph-empty.bin.hex | shasum -a 256
```

Expected SHA-256:
`d7b60b2926e6a933b5f677699befa5eb28cff16d18fe713d1cfe24d674ca6499`.
