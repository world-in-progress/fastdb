# FastDB payload schemas

The files in this directory describe authoring and diagnostic surfaces for the
Core-owned portable payload contract. They help editors and external tooling,
but they do not replace the C++ Core's strict parser, normalization, profile
validation, resolution, canonicalization, or digest authority.

[`fastdb.payload.bin.v1.md`](fastdb.payload.bin.v1.md) is the normative binary
contract for both `record.v1` profile 1 and `object_graph.v1` profile 2. It
freezes byte offsets, profile-dispatched region inventories, slots, inline and
object-pool AoS layout, canonical padding and partition rules, numeric
representation, hardened-open accounting, and resource limits. Its explicit
ordered record and first annotated graph images are in the
[binary golden index](../tests/golden/payload/v1/binary/index.json).
The frozen record mapping from accepted requirements to Core symbols, named
tests, and ordered goldens remains the [P2 requirement-to-test
traceability](../docs/issues/0002-portable-payload-foundation-implementation-status.md#p2-requirement-to-test-traceability)
table. The same issue's [P3 requirement-to-proof
traceability](../docs/issues/0002-portable-payload-foundation-implementation-status.md#p3-requirement-to-proof-traceability)
links the locally frozen Core graph runtime to every P3 design Section 4-21,
active-goal Stage B requirement, exact ABI-105, ordered graph goldens, and the
17-class executable proof map. These are local P3 facts only; hosted results,
P4 language projections/codegen, P5 clean cut, and release remain open.
The reviewed binary-open seed inventory and exact byte recipes live in
[`tests/fuzz/payload/binary-corpus.json`](../tests/fuzz/payload/binary-corpus.json);
the deterministic runner and coverage-guided target both consume those bytes
through the public C ABI without introducing another binary reader.

`fastdb.payload.v1.schema.json` and
`fastdb.payload.manifest.v1.schema.json` use JSON Schema draft 2020-12 and are
embedded as raw UTF-8 source bytes by
`tools/generate_embedded_payload_schemas.py`. Checked-in JSON may be formatted
for review. The schema bytes returned by the Core are its RFC 8785 JSON
Canonicalization Scheme output, and
`fastdb.payload.v1.schema.sha256` pins the SHA-256 digest of those canonical
bytes, not the formatted source file.

The machine schema closes every declared object shape and covers the exact V1
root fields, identifier syntax, profiles, cardinalities, record component kind,
and recursive type forms. Some rules require Core semantic analysis and cannot
be truthfully expressed by this schema alone:

- entry, component, and per-component field ID uniqueness;
- normalized integer bounds with `min < max`;
- component and reference target resolution;
- acyclic by-value component containment;
- profile legality, including rejecting references under `record.v1`.

The manifest schema describes the exact Core-derived
`fastdb.payload.manifest.v1` object. It closes every object shape and permits
`component_index` only on `component` types and
`target_component_index` only on `ref` types. The Core constructs and checks
that manifest with its own table-driven contract assertions; JSON Schema is
not a runtime parser or a second semantic authority.

Regenerate the embedded source after editing a schema:

```sh
python3 tools/generate_embedded_payload_schemas.py
python3 tools/generate_embedded_payload_schemas.py --check
```

The generator copies bytes only. It does not parse, canonicalize, validate, or
hash schemas; those operations remain in FastDB Core.
