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

Core-owned four-target payload codegen is still open during P4 Task 6. No Rust
generator or privileged private-Core route is provided here.
