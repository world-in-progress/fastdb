# fastdb-sys

`fastdb-sys` is the mechanical Rust declaration layer for the stable FastDB
portable-payload C ABI. The C++ Core remains the only semantic authority; this
crate does not parse schemas, canonicalize JSON, plan layouts, read binaries,
walk graphs, materialize values, or generate code.

Install the `fastdb-core-0.2.1-<target>.tar.gz` Core/C ABI bundle from the
[FastDB v0.2.1 release](https://github.com/world-in-progress/fastdb/releases/tag/v0.2.1)
and verify it against that release's artifact manifest. The native release
targets are `x86_64-unknown-linux-gnu`, `aarch64-apple-darwin` and
`x86_64-pc-windows-msvc`. The extracted bundle contains
`include/fastdb_payload.h` and `lib/libfastdb.so` (Linux),
`lib/libfastdb.dylib` (macOS), or both `lib/fastdb.lib` and
`lib/fastdb.dll` (Windows).

Registry consumers select the `system` link mode and use the extracted `lib`
directory. For example, after extracting the bundle under
`/opt/fastdb/0.2.1/<target>`:

```sh
export FASTDB_PAYLOAD_LINK_MODE=system
export FASTDB_PAYLOAD_SYSTEM_LIB_DIR=/opt/fastdb/0.2.1/x86_64-unknown-linux-gnu/lib
# Linux runtime loader:
export LD_LIBRARY_PATH="$FASTDB_PAYLOAD_SYSTEM_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# On macOS, use DYLD_LIBRARY_PATH with the same directory instead.
cargo run
```

For Windows x64, use the matching MSVC bundle and add its DLL directory to
the process loader path:

```powershell
$env:FASTDB_PAYLOAD_LINK_MODE = "system"
$env:FASTDB_PAYLOAD_SYSTEM_LIB_DIR = "C:\fastdb\0.2.1\x86_64-pc-windows-msvc\lib"
$env:PATH = "$env:FASTDB_PAYLOAD_SYSTEM_LIB_DIR;$env:PATH"
cargo run
```

Replace `<target>` with the selected native target. Keep the shared library
available at runtime; `FASTDB_PAYLOAD_SYSTEM_LIB_DIR` configures the linker,
while the application's loader configuration selects the runtime library.
The crate never downloads native libraries or searches other project checkouts.

The default `source` mode is reserved for a FastDB repository checkout. It
builds the payload-only Core target from the single Core source tree:

```text
FASTDB_PAYLOAD_LINK_MODE=source
```

Source mode deliberately fails for a registry crate because crate archives do
not contain a second copy of Core. The system directory must be absolute and
contain the library named by FastDB. The build
fails closed for an unknown mode, a relative/missing directory, or a missing
library. The raw ABI test and the safe crate call `fdb_payload_v1_abi_version`
against the linked library; a mismatched ABI is rejected rather than adapted.

The ABI-117 boundary uses ABI version 1 and exactly 117 exported symbols.
The historical P3 runtime sub-boundary is ABI-105; the twelve P4
additions are only three Core provenance guards and nine immutable
ArtifactSet/codegen functions.
Only one of these link modes selects where the same Core comes from. Neither
mode creates a Rust implementation of FastDB semantics.
