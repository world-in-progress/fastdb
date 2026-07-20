# `graph-single-root` layout receipt

- Binary profile: `object_graph.v1` (`2`); total length `536`; root-value
  count `2`; spec SHA-256
  `1b76708ea2040f4a55283c7e78703b1fc06645cf75c99dd9931d714e4f0feded`.
- Header: `[0,128)`; region directory: `[128,408)` with five 56-byte
  descriptors; entry directory: `[408,488)` with two 40-byte descriptors.
- Regions 0/1 belong to entry 0 (runtime type 0): validity `[488,489)`,
  padding `[489,496)`, and one 8-byte root-ID slot `[496,504)`.
- Regions 2/3 belong to entry 1 (runtime type 1): validity `[504,505)`,
  padding `[505,512)`, and one 8-byte ref-ID slot `[512,520)`.
- Region 4 is `OBJECT_VALUES` for component 0 (`Node`): offset `520`, byte
  length `16`, object count `1`, stride/alignment `16/8`, runtime type
  sentinel `UINT32_MAX`. Object ID 0 therefore occupies `[520,536)`.
- Entry 0 validity is `01` and its slot is
  `00 00 00 00 00 00 00 00`: object ID zero is present. Entry 1 validity is
  `00` and its slot has the same eight zero bytes: it is null.
- The object record is: immediate validity `00` at byte 520, padding byte 521,
  `u16` value `34 12` at `[522,524)`, padding `[524,528)`, and the nullable
  `next` ref's zero slot at `[528,536)`. There is no object-level validity
  bitmap and no final padding.

Independent binary receipt:

```sh
xxd -r -p valid/graph-single-root.bin.hex | shasum -a 256
```

Expected SHA-256:
`caad4de512cb74be8b979adb78a59478e9998b3bd0a28564e768ccfff25c760d`.
