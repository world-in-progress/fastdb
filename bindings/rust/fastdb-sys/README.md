# fastdb-sys

`fastdb-sys` is the mechanical Rust declaration layer for the stable FastDB
portable-payload C ABI. The C++ Core remains the only semantic authority; this
crate does not parse schemas, canonicalize JSON, plan layouts, read binaries,
walk graphs, materialize values, or generate code.

The default `source` mode is for a FastDB repository checkout. It builds the
payload-only Core target from that checkout:

```text
FASTDB_PAYLOAD_LINK_MODE=source
```

Relocated/package consumers use an already built compatible shared FastDB
library:

```text
FASTDB_PAYLOAD_LINK_MODE=system
FASTDB_PAYLOAD_SYSTEM_LIB_DIR=/absolute/path/to/lib
```

The directory must contain the platform library named by FastDB
(`libfastdb.dylib`, `libfastdb.so`, or the Windows import library). The build
fails closed for an unknown mode, a relative/missing directory, or a missing
library. The raw ABI test and the safe crate call `fdb_payload_v1_abi_version`
against the linked library; a mismatched ABI is rejected rather than adapted.
The dynamic loader path remains the embedding application's responsibility.

The current ABI version remains 1 and the reviewed allowlist contains exactly
117 symbols. The historical P3 runtime sub-boundary is ABI-105; the twelve P4
additions are only three Core provenance guards and nine immutable
ArtifactSet/codegen functions.
P4 is locally complete at ABI-117. Hosted execution and the P5 legacy clean
cut remain separate, pending evidence.

Only one of these link modes selects where the same Core comes from. Neither
mode creates a Rust implementation of FastDB semantics.
