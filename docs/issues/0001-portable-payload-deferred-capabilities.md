# Issue 0001: Portable Payload Capabilities Deferred Beyond 0.2.0

- **Status:** Open
- **Opened:** 2026-07-16
- **Owner:** FastDB
- **Governing design:** [FastDB Portable Payload Foundation Design](../superpowers/specs/2026-07-16-portable-payload-foundation-design.md)

## Purpose

FastDB 0.2.0 must establish the complete semantic foundation, not merely the shortest working path. Open items below are deferred only where they require an additional storage/backing/platform design that is not necessary to make the declared V1 semantics correct and usable. Closed items remain in place with their resolution evidence so a historical limit is neither mistaken for a permanent protocol rule nor silently erased.

## D1. Dynamic `object_graph.v1` with `REQUIRE_DIRECT` — Closed

- **Closed locally:** 2026-07-20
- **Design:** [Portable Payload Object-Graph Runtime
  Design](../superpowers/specs/2026-07-20-portable-payload-object-graph-runtime-design.md)
- **Implementation:** `5ee2eb8` (exact graph plan and direct/staged
  execution), `2f06e52` (public ABI projection), and `2730e9e` (hostile-input,
  fuzz, Wasm, ABI, package, and proof hardening); `5d5939b` verifies the
  open-to-closed D1 proof-map transition itself, and `1f0be6c` fail-closes the
  complete closure traceability contract

### Resolution

Complete logical authoring freezes into an exact immutable `GraphLayout` before
execution. `GraphEncoder::encode_graph` writes through `AscendingWriter` and a
`ByteSink`; the direct branch in `BuildPlan::execute` supplies
`RangeCallbackSink` over the caller's final reservation. No growth callback,
backing-ABI change, arbitrary maximum reservation, or second complete encoded
image is required.

The named executable proof is
`tests/cpp/payload/test_graph_backing.cpp::test_direct_graph_has_no_full_image_allocation`.
It builds a variable-width graph larger than four MiB containing cycles,
sharing, lists, `str`, `wstr`, and bytes, then proves together that:

- the caller backing receives exactly one direct reserve and no staged reserve;
- the Core heap-backing observer records no direct or staged heap-image reserve;
- range writes are monotonic, non-overlapping, and cover the exact final length;
- an allocation threshold below one full image permits direct execution while a
  deliberate complete-image allocation fails;
- the direct report is `DIRECT/NONE` with `staging_bytes = 0`;
- `test_graph_staging_policy_and_cleanup` proves allowed staged execution is
  byte-identical and reports the actual fallback and staging byte count;
- injected pre/post-commit failures preserve exact rollback/release behavior.

The source audit covers `GraphEncoder.cpp` (`AscendingWriter`, `ByteSink`,
`zero_until`, and `encode_graph`), `BuildPlan.cpp` (`RangeCallbackSink`,
`encode_profile`, reservation commit, and `ExecutionReport`), and the
`BUILD_TESTING`-only observer in `HeapBacking.cpp`. The complete local P3 gate
ran on macOS 26.5.2 arm64 with AppleClang 21.0.0, CMake 4.3.2, CMake's system
Python 3.14.5, uv's test environment on Python 3.14.3, Node 25.8.1, and
Emscripten 5.0.2. Apple ASan required `detect_leaks=0`, so no local
LeakSanitizer result is claimed; hosted Linux/macOS results remain pending.

The context-owning primary agent reviewed the complete P3 range and the D1
source/evidence boundary with no unresolved Critical, Important, or material
Minor finding. Per explicit user direction this was not delegated, so no
independent/subagent review is claimed.

### Historical concern retained

At issue opening, mutable authoring was assumed to imply that final pool sizes
and fixups could remain unknown when final backing was reserved. That concern
correctly rejected a later full-image copy mislabeled as direct and rejected an
arbitrary maximum reservation. P3 superseded only the assumption: V1 already
freezes complete logical authoring, and the resulting immutable plan knows all
sizes, IDs, offsets, and fixups before execution.

### Current impact

There is no remaining D1 limit for ordinary `object_graph.v1`. A caller that
requires direct execution receives it when the backing accepts the exact direct
reservation; a backing that declines direct still produces
`FDB_PAYLOAD_E_DIRECT_UNAVAILABLE` under `REQUIRE_DIRECT`, while
`ALLOW_STAGING` may make a truthful staged fallback. D2-D5 below remain open.

## D2. Segmented or multipart final backing

### Current limit

`fastdb.payload.bin.v1` uses one contiguous final backing. Large payloads cannot be published as a scatter/gather region set or multipart object through the V1 payload owner.

### Why it is deferred

Segmented backing changes offset representation, view traversal, reference validation, retain/release topology, transport handoff, and possibly the binary format. Adding it to the first stable ABI before contiguous semantics and lifetime behavior are proven would make every binding and validation path more complex.

### Impact and dependencies

- A V1 payload must fit one contiguous addressable backing; scatter/gather and object-store multipart data are outside the payload owner.
- Large raw files continue to use their object/file-storage path rather than being forced through FastDB.
- Core, backing, binary-layout, view, and every binding team must agree on region identity and lifetime before implementation.
- Deferral keeps current bounds/reference validation unambiguous; a later design must preserve existing contiguous payload meaning.

### Closure criteria

- A new ADR defines whether segmentation extends V1 callbacks or requires a versioned binary/ABI profile.
- Region identity, bounds, alignment, hashing, commit/rollback, retain/release, and checked views are specified across segment boundaries.
- Native and WASM golden tests cover scatter/gather open, malformed segment tables, partial commit failure, and materialization.
- Existing contiguous V1 payloads retain their meaning.

## D3. Incremental or streaming builder

### Current limit

The V1 `PayloadBuilder` freezes a complete logical value set into an immutable `BuildPlan`. It is not a streaming encoder and does not promise bounded memory independent of payload size.

### Why it is deferred

Deterministic layout, null validity, list offsets, normalized values, object identity, reference fixups, final size, and direct/staged truth all depend on information that may arrive late. A streaming API needs explicit ordering, backpressure, partial-failure, digest, and graph-forward-reference semantics rather than a thin loop around the complete builder.

### Impact and dependencies

- V1 authoring memory can grow with the complete logical payload; callers cannot rely on a bounded-memory streaming contract.
- Developers must not expose a binding-only iterator API that secretly buffers the whole payload while claiming streaming.
- Closure depends on Core layout, backing, error/cancellation, and cross-language backpressure design.
- Keeping one complete-plan model in V1 protects deterministic bytes and consistent graph/reference failures across languages.

### Closure criteria

- A new ADR defines streaming ordering, bounded-memory guarantees, forward references, cancellation, and partial failure.
- Streaming output is byte-identical to the complete builder for the same logical value.
- Backpressure and resource limits work in C++, Rust, Python, and TypeScript/WASM without binding-owned layout logic.
- Direct/staged reporting remains truthful.

## D4. Guaranteed platforms beyond Linux x86-64, macOS arm64, and wasm32

### Current limit

The 0.2.0 support claim and release gates cover Linux x86-64, macOS arm64, and WebAssembly `wasm32`. The fixed-width/little-endian format is designed for portability, but other native targets are not guaranteed until tested.

### Why it is deferred

ABI calling convention, alignment, compiler behavior, sanitizers, text encoding bridges, packaging, and endianness need real target verification. Declaring support from format intent alone would overstate shipped behavior.

### Impact and dependencies

- Users on other targets may experiment from source but receive no compatibility, package, or support guarantee.
- Maintainers must not infer support from a successful compile or from the format's fixed-width intent alone.
- Each new claim depends on real CI hardware/emulation, packaging, ABI checks, and the shared golden corpus.
- The format remains designed for portability; the limitation is verified platform coverage, not permission for platform-dependent bytes.

### Closure criteria

- The target builds the pure-C ABI, C++ RAII facade, and relevant language projections.
- The shared canonical/binary/error/lifetime golden corpus passes.
- ABI symbol and struct-layout checks pass with the target toolchain.
- A sanitizer or equivalent memory-safety job exists where the platform supports it.
- Packaging and CI are added before the support claim changes.

## D5. Native Node.js and Go portable payload projections

### Current limit

TypeScript's official 0.2.0 portable runtime uses the C++ Core compiled to WebAssembly. There is no supported native Node.js portable binding. The repository's existing Go work is not a `fastdb.payload.v1` projection and is not included in the 0.2.0 portable parity claim.

### Why it is deferred

The required initial consumers are C++, Rust, Python, and browser-capable TypeScript/WASM. A Node native addon adds a second TypeScript runtime/lifetime path, while a Go projection adds cgo ownership, finalizer, slice-view, and codegen concerns. Implementing either before the C ABI and initial parity corpus stabilize would duplicate debugging surfaces without closing a current foundation dependency.

### Impact and dependencies

- Node.js users use the official WASM projection; Go users do not yet have a supported portable-payload projection.
- The existing Go code must not be presented as `fastdb.payload.v1` support or grow an independent parser.
- Both projections depend on the frozen C ABI, lifetime/error corpus, and deterministic codegen contract.
- Deferral avoids creating a second TypeScript semantic path or a Go-specific payload meaning while the authority boundary is settling.

### Closure criteria

- The projection wraps the stable C ABI without a schema, digest, layout, or binary parser.
- Checked-view invalidation, explicit release, materialization, nullability, errors, and backing reports match the shared corpus.
- Generated target code compiles and regeneration is deterministic.
- Native Node and WASM TypeScript expose equivalent logical semantics if both are supported.

## Not deferred

The following are part of the 0.2.0 foundation and must not be moved into this issue to shorten implementation:

- strict Core compilation, normalization, RFC 8785 canonicalization, and SHA-256 identity;
- explicit nullability and the complete declared V1 type algebra;
- full `record.v1` behavior;
- ordinary `object_graph.v1` build/open/decode/view/materialize/invalidation, including cycles and shared refs;
- deterministic `fastdb.payload.bin.v1` and hardened open validation;
- stable C ABI, C++ RAII facade, and final-backing lifecycle;
- Rust and Python capability parity;
- official TypeScript/WASM projection;
- C++, Rust, Python, and TypeScript code generation from the same Core;
- clean removal of public call-db/columnar authority surfaces for 0.2.0.
