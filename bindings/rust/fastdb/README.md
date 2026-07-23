# fastdb

`fastdb` is the safe Rust projection of the FastDB portable-payload C ABI. It
owns Core handles with Rust lifetimes and `Drop`, preserves all five Core error
fields, and exposes compile/query, author/freeze, execute/backing, open,
checked view, graph identity, invalidation, and detached materialization.

The C++ Core is the sole semantic authority. This crate contains no schema or
binary parser, canonicalizer, digest implementation, layout planner, graph
algorithm, materializer, or codegen implementation. Its behavior must remain
in parity with the C++, Python, and TypeScript/WebAssembly projections.

Repository builds use `fastdb-sys` source mode by default. A relocated
consumer can select the documented `FASTDB_PAYLOAD_LINK_MODE=system` boundary
and point `FASTDB_PAYLOAD_SYSTEM_LIB_DIR` at a compatible shared FastDB
library. `CompiledSpec::compile` checks the real Core ABI version before
publishing a safe handle.

`CompiledSpec::generate` projects Core-owned deterministic C++/Rust/Python/
TypeScript artifacts as an immutable `ArtifactSet`. Artifact paths, bytes,
SHA-256 receipts, limits, provenance, and errors all come from the stable C
ABI. Rust contains no generator or privileged private-Core route.
