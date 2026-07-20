# `graph-shared-cycle` layout receipt

- Binary profile: `object_graph.v1` (`2`); total length `456`; root-value
  count `2`; spec SHA-256
  `4c7f636065873ab89867d2e4eb4252048bed4569d1200f197d296237913f9ed9`.
- Header: `[0,128)`; region directory: `[128,296)` with three 56-byte
  descriptors; entry directory: `[296,376)` with two 40-byte descriptors.
- Region 0 is entry 0's runtime-type-0 root slot `[376,384)`; region 1 is
  entry 1's runtime-type-1 root slot `[384,392)`. Both have count 1 and
  stride/alignment `8/8`; neither has a validity region.
- The root slots are respectively
  `00 00 00 00 00 00 00 00` and `01 00 00 00 00 00 00 00`, selecting dense
  object IDs 0 and 1. No physical root/reference table exists.
- Region 2 is `OBJECT_VALUES` for component 0 (`Node`): offset `392`, byte
  length `64`, object count `2`, stride/alignment `32/8`, runtime type
  sentinel `UINT32_MAX`. Object IDs 0/1 occupy `[392,424)` and `[424,456)`.
- Object 0 stores `u32(100)` at `[392,396)`, padding `[396,400)`, self ID 0
  at `[400,408)`, shared ID 1 at `[408,416)`, inline `Pair(true,0x1234)` at
  `[416,420)`, and tail padding `[420,424)`. Pair's internal padding is byte
  417.
- Object 1 stores `u32(200)` at `[424,428)`, padding `[428,432)`, self ID 1
  at `[432,440)`, shared ID 1 at `[440,448)`, inline
  `Pair(false,0xabcd)` at `[448,452)`, and tail padding `[452,456)`. Pair's
  internal padding is byte 449. Neither object has an object-level validity
  bitmap.

Independent binary receipt:

```sh
xxd -r -p valid/graph-shared-cycle.bin.hex | shasum -a 256
```

Expected SHA-256:
`a6623d795b9fe018942015c04971a4a8f5129483e15d9f2ac1ff9f35b37b8627`.
