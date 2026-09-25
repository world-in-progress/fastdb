# fastdb

`fastdb` is the safe Rust projection of the FastDB portable-payload C ABI. It
owns Core handles with Rust lifetimes and `Drop`, preserves all five Core error
fields, and exposes compile/query, author/freeze, execute/backing, open,
checked view, graph identity, invalidation, and detached materialization.

The C++ Core is the sole semantic authority. This crate contains no schema or
binary parser, canonicalizer, digest implementation, layout planner, graph
algorithm, materializer, or codegen implementation. Its behavior must remain
in parity with the C++, Python, and TypeScript/WebAssembly projections.

Add the safe crate with an exact release dependency:

```toml
[dependencies]
fastdb = "=0.2.1"
```

Install the matching `fastdb-core-0.2.1-<target>.tar.gz` asset from the
[FastDB v0.2.1 release](https://github.com/world-in-progress/fastdb/releases/tag/v0.2.1).
Set `FASTDB_PAYLOAD_LINK_MODE=system` and set
`FASTDB_PAYLOAD_SYSTEM_LIB_DIR` to the absolute extracted `lib` directory.
Configure `LD_LIBRARY_PATH` on Linux, `DYLD_LIBRARY_PATH` on macOS, or `PATH` on Windows to load
that directory when running the consumer. The native release targets are
`x86_64-unknown-linux-gnu`, `aarch64-apple-darwin` and `x86_64-pc-windows-msvc`.

The safe crate requires the exact `fastdb-sys 0.2.1` projection. Both crates
use the released Core/C ABI bundle and contain no duplicate Core source tree.
Repository builds use checkout-only source mode by default.
`CompiledSpec::compile` checks the real Core ABI version before publishing a
safe handle. See the [raw binding's linking contract](https://github.com/world-in-progress/fastdb/blob/v0.2.1/bindings/rust/fastdb-sys/README.md)
for linker and loader setup.

`CompiledSpec::generate` projects Core-owned deterministic C++/Rust/Python/
TypeScript artifacts as an immutable `ArtifactSet`. Artifact paths, bytes,
SHA-256 receipts, limits, provenance, and errors all come from the stable C
ABI. Rust contains no generator or privileged private-Core route.

Release validation executes extracted crate archives outside the source
checkout against the matching native bundle. It covers compilation, codegen,
build/open, checked views, invalidation, and detached materialization.
