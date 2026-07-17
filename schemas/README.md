# FastDB payload schemas

The files in this directory describe authoring and diagnostic surfaces for the
Core-owned portable payload contract. They help editors and external tooling,
but they do not replace the C++ Core's strict parser, normalization, profile
validation, resolution, canonicalization, or digest authority.

`fastdb.payload.v1.schema.json` uses JSON Schema draft 2020-12 and is embedded
as raw UTF-8 source bytes by `tools/generate_embedded_payload_schemas.py`.
Checked-in JSON may be formatted for review. The schema bytes returned by the
Core are its RFC 8785 JSON Canonicalization Scheme output, and
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

Regenerate the embedded source after editing a schema:

```sh
python3 tools/generate_embedded_payload_schemas.py
python3 tools/generate_embedded_payload_schemas.py --check
```

The generator copies bytes only. It does not parse, canonicalize, validate, or
hash schemas; those operations remain in FastDB Core.
