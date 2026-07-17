# Issue 0002: Portable Payload Foundation Implementation Status

- **Status:** Open
- **Opened:** 2026-07-16
- **Owner:** FastDB
- **Governing design:** [FastDB Portable Payload Foundation Design](../superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
- **Governing decision:** [ADR-0001](../decisions/0001-portable-payload-core-authority.md)
- **First implementation plan:** [Portable Payload Core Contract Implementation Plan](../superpowers/plans/2026-07-16-portable-payload-core-contract.md)
- **P2 implementation plan:** [Portable Payload Record Runtime Implementation Plan](../superpowers/plans/2026-07-17-portable-payload-record-runtime.md)
- **Post-0.2 deferrals:** [Issue 0001](0001-portable-payload-deferred-capabilities.md)

## Purpose

The accepted 0.2.0 design defines a complete portable-payload foundation. The
current repository implements the P1 compiler/query boundary and still ships
the 0.1.x call-db/`ColumnEngine` implementation. P1 Task 9 has completed local
independent implementation review; its first hosted CI execution remains
pending. P2-P5 runtime, language-parity, codegen, clean-cut, and release work
is not implemented. This issue records that temporary gap through the 0.2.0
clean cut.

It is not a mechanism for shrinking the accepted milestone. Capabilities intentionally outside 0.2.0 belong in Issue 0001. Every item below remains non-deferrable for the 0.2.0 foundation and must be removed from this issue by implementation, not reclassified to make a release claim pass.

## Current implemented surface

Current repository state:

- the target architecture, owner boundary, source algebra, identity pipeline, runtime model, C ABI direction, testing obligations, and clean-cut decision are accepted documentation;
- the first executable plan is written for the Core compiler/query contract;
- P1 Tasks 1 and 2 are reviewed and complete: the repository has pinned yyjson, double-conversion, and PicoSHA2 source snapshots, a native CTest seam, Core-owned immutable `JsonValue`, RFC 6901 JSON Pointer construction, RFC 8785 JCS serialization, and SHA-256 identity primitives;
- `JsonValue` lifetime/release and JCS traversal are iterative, with committed 50,000-level array and object tests for serialization, destruction, and failure cleanup, plus high-fan-out shared-child copy/move/release coverage;
- at the start of Task 3, strict source parsing/compiler semantics, the stable public C ABI, binary/runtime behavior, bindings, and code generation were all still absent; Tasks 3-7 have since built only the independently reviewed P1 layers described below;
- P1 Task 3 now ships only the pure-C ABI base: exact-width constants and V1 struct initializers, opaque spec/blob/error declarations, ABI version query, immutable owned blob/error handles, atomic retain/release, stable error field queries, and an exception-to-owned-error boundary;
- P1 Task 4 adds strict duplicate-aware JSON document parsing, iterative source/value/depth enforcement, borrowed source-order cursors, generic audited-document conversion for schema/JCS uses, and a Core-owned repository for the embedded `fastdb.payload.v1` source schema's JCS bytes and SHA-256 digest;
- P1 Task 5 is independently reviewed and complete: Core now has a move-only typed source model and duplicate-safe cursor parser for the complete V1 type algebra, exact object shapes and local ID syntax, finite normalized bounds, explicit `nullable: false` insertion, source-order preservation, and pre-growth entry/component/field count limits; nested-list parsing, normalized emission, and ownership teardown are iterative, and a shared mutable JSON Pointer builder keeps successful deep audit/typed-parse path state linear while materializing immutable RFC 6901 paths only for diagnostics;
- P1 Task 6 is independently reviewed and complete: the Core-internal resolver consumes the move-only Task 5 source model, rejects duplicate entry/component/field IDs in the required source order, sorts components by ASCII ID, assigns stable entry/component/field indexes, retains source IDs while resolving component/ref targets, rejects deterministic by-value cycles and `record.v1` references, and preserves original source JSON Pointers in all diagnostics after sorting;
- Task 6 also derives the exact five semantic facts and their single Core-owned `uint64_t` mask, including transitive by-value component variable width while keeping a bare `ref` fixed-width. Resolver type walks, cycle DFS, fact propagation, and cleanup are iterative; committed regressions cover 20,000 nested lists, a 20,000-component acyclic chain, and a 12,000-component cycle under explicit allocation bounds;
- P1 Task 7 is independently reviewed and complete: the Core-internal compiler/identity stage uses an immutable shared `CompiledSpec` that runs the one strict document -> typed parse/normalization -> resolution -> normalized Core JCS -> SHA-256 -> manifest pipeline, owns deterministic sorted-vector ID indexes, and publishes only const internal reads. Its payload identity is over normalized canonical payload bytes alone;
- Task 7 also adds the exact closed `fastdb.payload.manifest.v1` schema, deterministic raw-schema embedding, Core-JCS/Core-SHA-256 source and manifest schema artifacts, the exact resolved manifest and P1 capability facts, and an explicit ordered 20-case golden corpus. The manifest reports only `compile,query`, no codegen targets, and direct build `not_evaluated/runtime_slice_not_implemented`; it does not claim `eligible:false` or binary/layout support;
- the Task 7 implementation and independent review pass the focused corpus, native Debug/Release/ASan+UBSan suites, Python/package and TypeScript/WASM gates, independent pinned-JCS source-schema and full golden reconstruction, external Draft 2020-12 manifest validation, corpus inventory audit, allocation-failure sweep, and deep/wide resource probes. Tasks 1-7 are now the independently reviewed P1 boundary;
- P1 Task 8 is independently reviewed and complete: the pure-C surface now has exactly 33 `fdb_payload_v1_*` exports, including `fdb_payload_v1_spec_compile_json`, atomic opaque spec lifetime, canonical/manifest/digest/profile/capability queries, stable entry/component/field count-ID-index queries, and source-schema bytes/digest;
- Task 8 projects the reviewed Task 7 `CompiledSpec` and `SchemaRepository` directly. Its 80-byte options and 72-byte capabilities prefixes accept larger future tails without reading or writing them, reject unsupported V1 fields, clear value outputs before ordinary failure, preserve owned error/status equality, validate exact ASCII ID spans, and return independently owned blobs;
- Task 8 also adds the standalone header-only C++17 `fastdb::payload::v1` RAII facade. It performs only C-ABI calls, retain/release ownership, copied-error translation, and typed byte/query ergonomics; it has no parser, canonicalizer, digest, manifest builder, schema model, binary reader, or downstream contract behavior;
- the Task 8 implementation and independent review pass the pure-C link smoke, exact 33-declaration/definition/export audit, Debug/Release/ASan+UBSan native suites, a 16-thread all-ID/index immutable-query test under ThreadSanitizer, allocation-failure publication/retry and live-allocation balance checks, guarded/canary future-prefix tests, C++/C byte-for-byte parity, direct arm64/x86_64/wasm32 C11 header compilation, Python/package regression, and TypeScript/WASM regression. Tasks 1-8 are now the independently reviewed P1 boundary;
- P1 Task 9 is independently reviewed and complete for local P1 freeze: committed adjacent-shape regressions close the deep-object/shared-child observation; an exact sorted 33-symbol allowlist and executable symbol-diff gate freeze the public ABI; the Clang libFuzzer harness queries the complete required compiled-spec surface on success and owned diagnostics on failure; native Debug/Release/sanitizer gates, package inventories, and shipped-state documentation are defined;
- the first independent Task 9 review found two integration defects: fuzzer-only builds instrumented shared dependencies without supplying sanitizer runtimes to ordinary consumers, and the workflow aggregate accepted any skipped job without proving it was out of scope. The reviewed correction makes fuzzer mode a consistent sanitizer configuration and validates each aggregate job against its exact path scope. The final re-review reports zero Critical, Important, or Minor findings after clean fuzzer-only, combined-sanitizer, native, language, package, ABI, and aggregate-matrix gates;
- the GitHub Actions definition now targets the documented standard `ubuntu-24.04` x64 and `macos-15` arm64 runners, asserts the actual architecture, and includes required native and Linux sanitizer/fuzz jobs in the aggregate result. No push is authorized in Task 9, so the first hosted execution remains pending and is not represented as a local pass;
- P2 Task 1 now provides a Core-internal, record-profile logical-value builder over the frozen P1 `ResolvedSpec`: stable source-node runtime type IDs, flat `ValueNode`/index vectors, stable entry roots, copied text/wide-text/opaque storage, explicit iterative expectation frames, schema-driven `many + component` record batches, exact-width fixed runs, strict UTF-8/UTF-16 validation, exact bit-level normalized binary64 finite/range comparison, transactional retryable mutation, deterministic builder limits/accounting, and stable builder errors 2009-2013. Every authoring mutation computes its final logical frame accounting without cloned state and preflights node/storage/total limits before scratch, capacity growth, or input publication; empty container closure uses the same allocation-free cascade accounting as scalar and wide-text leaves. Fixed-run logical limits are likewise preflighted before count-sized scratch or input traversal; native scalar inputs are loaded through exact-width typed objects; input spans are bounded by the Core's `size_t`/`ptrdiff_t` addressability helper before pointer arithmetic; and full-width error facts use lossless decimal strings;
- the Task 1 builder is deliberately internal. No `fastdb.payload.bin.v1`, binary/layout encoder, `BuildPlan`, final backing, payload owner, open, checked view, materialization, object-graph runtime, public builder ABI, portable language projection, or payload code generator is currently shipped;
- current public call-db, `fastdb.schema.v1`, `columnar.v1`, and `ColumnEngine` surfaces remain 0.1.x migration inputs, not the accepted 0.2.0 authority.

The P1 compiler/query slice is implemented, independently reviewed, and frozen
for P2 work. Its first hosted CI execution remains pending. The repository must
not claim that the complete portable payload foundation or FastDB 0.2.0 is
implemented.

## Current limit

Users and downstream repositories can now compile and query
`fastdb.payload.v1` through the independently reviewed C ABI or C++ facade.
The Core now also has the first internal record authoring stage, but no public
caller can create that builder or freeze it into a plan, and there are no
portable binary bytes to build or open. Users still cannot build, open, view,
materialize, invalidate, or generate a portable payload, and no Rust, Python,
or TypeScript/WASM portable projection exists. C-Two may not replace those
missing owner slices or depend on FastDB private headers; recreating FastDB
semantics remains forbidden. Toodle consequently cannot yet treat the full
structured-payload substrate as implemented.

Raw-file/object bytes remain correctly outside FastDB in file/object storage. This implementation gap does not change that owner boundary and is not a reason to route raw files through the existing call-db path.

## Why implementation is sequenced

The five slices have hard dependency order:

1. A strict Core compiler must freeze normalized type/profile/identity/error/query semantics.
2. The record binary and runtime must be designed from that frozen resolved model and must atomically add the exact binary layout document and goldens.
3. Object-graph pools, references, cycles, and validation must reuse the proven binary/backing/lifetime contracts.
4. Language projections and code generation must wrap stable Core behavior rather than speculate or become second authorities.
5. The clean cut and C-Two composition can happen only after replacements exist and parity gates pass.

Collapsing these layers into one speculative change would make review and failure attribution weaker. Reversing the order would force bindings or C-Two to invent semantics that belong in FastDB Core.

## Impact

- FastDB maintainers must describe the accepted design as a target until the relevant slice is verified.
- C-Two work may plan the outer `c-two.contract.v2` composition but must not implement a FastDB parser, digest, binary reader, or privileged Rust-only payload path.
- Python and TypeScript code currently under call-db remains migration source only and must not gain new portable authority.
- No package version may be raised to 0.2.0 and no release note may claim the foundation while any P1-P5 non-deferrable gate remains open.
- Each merged slice must update this issue's current surface, remaining impact, and checklist in the same change.

## Remaining P2-P5 gaps

### P2 record binary, runtime, and lifetime

**Current limit:** P1 compiles and queries a specification, and P2 Task 1 has
the internal flat logical arena and complete record-value authoring state
machine. The builder is not a public ABI and can freeze only to an internal
`LogicalPayload`; it does not produce binary bytes or a repeatable plan. There
is still no `fastdb.payload.bin.v1` byte layout, binary encoder/open reader,
`BuildPlan`, final-backing callback path, direct/staged execution,
`PayloadOwner`, checked view, materialization, or invalidation.

**Reason:** Binary and lifetime behavior must be designed from the frozen P1
resolved model and must land atomically with its normative byte-layout document
and deterministic goldens.

**Impact:** A C or C++ caller can inspect contract facts but cannot author or
consume portable runtime values. Existing 0.1.x database/call-db bytes are not a
substitute for the new portable format.

**Next owner slice:** FastDB P2.

**Closure criteria:** Publish the exact `fastdb.payload.bin.v1` layout and
goldens; implement the complete legal non-`ref` algebra through builder, plan,
backing, build/open, owner, checked view, materialize, and invalidate; pass
deterministic, malformed-input, backing-failure, lifetime, sanitizer, and
cross-platform gates.

### P3 object-graph runtime

**Current limit:** P1 validates `object_graph.v1` specifications at compile
time, but there are no runtime object pools, roots, object IDs, shared
references, cycles, graph build/open, graph views, or graph materialization.

**Reason:** Graph storage and reference validation must reuse P2's normative
binary, backing, owner, and invalidation contracts rather than create a second
runtime.

**Impact:** Graph-shaped contracts can be identified and queried but no graph
payload can be executed or exchanged.

**Next owner slice:** FastDB P3 after P2 closes.

**Closure criteria:** Implement ordinary graph build/open/view/materialize and
invalidation for pools, roots, lists, shared refs, and cycles; reject malformed
IDs/references deterministically; pass graph goldens and sanitizer gates. Only
Issue 0001 D1 may remain outside direct dynamic construction.

### P4 language projections and payload code generation

**Current limit:** The P1 C ABI and query-only C++ facade exist, but no Rust,
Python, or TypeScript/WASM portable-payload projection exists, and the Core
does not generate C++, Rust, Python, or TypeScript payload artifacts. Existing
hand-written 0.1.x call-db layers are migration inputs, not projections of P1.

**Reason:** Safe binding lifetimes, value parity, and generated APIs depend on
the frozen P2/P3 runtime ABI and binary meaning.

**Impact:** Portable compile/query is available only through C and C++; no
supported language binding can yet build/open/view/materialize the shared
golden corpus or consume a Core-owned generated artifact set.

**Next owner slice:** FastDB P4 after P2 and P3 close.

**Closure criteria:** C++/Rust/Python/TypeScript-WASM obtain all semantics from
the same Core ABI and pass canonical, binary, value, error, lifetime, and
direct/staged parity; Core-owned four-target in-memory artifact generation is
deterministic and generated outputs compile or import without downstream
semantics.

### P5 clean cut, release, and downstream composition

**Current limit:** Public call-db, `fastdb.schema.v1`, `columnar.v1`, and
`ColumnEngine` remain in the 0.1.x package surface. Package metadata remains
0.1.x; no 0.2.0 release, tag, publication, or C-Two composition proof exists.

**Reason:** The obsolete surfaces cannot be removed until P1-P4 provide the
complete replacement and parity evidence. Downstream composition belongs in
C-Two only after FastDB freezes its owner boundary.

**Impact:** Users must treat those old APIs as migration inputs and must not
extend them as portable authority. FastDB cannot claim 0.2.0 readiness, and
C-Two cannot fill the missing FastDB slices in its own repository.

**Next owner slice:** FastDB P5, followed by a separate C-Two-owned composition
task.

**Closure criteria:** Remove the obsolete public authority without aliases or
compatibility parsers; rename `ColumnEngine` to `RecordEngine`; pass package,
clean-cut, parity, and release gates at 0.2.0; then prove C-Two delegates the
nested spec and composes artifacts without duplicating FastDB semantics.

## P1 observations that remain open

These are implementation-gate observations, not post-0.2.0 deferrals.

### Pre-existing SWIG diagnostics

**Current limit:** The Python wheel build succeeds with the exact seven legacy
SWIG diagnostics enumerated in [Issue 0003](0003-legacy-swig-diagnostics.md).

**Reason:** These diagnostics come from the legacy 0.1.x SWIG input surface;
Task 9 classifies them but does not redesign that API.

**Impact:** Existing wheels still build and the P1 C ABI is unaffected, but no
new warning may be accepted implicitly. Issue 0003 is the exact owner for the
residual cleanup and package-warning gate.

**Closure criteria:** Close Issue 0003 through the legacy/P5 clean-cut criteria,
or earlier if a listed warning blocks a required package build.

### Hosted native CI evidence

**Current limit:** Task 9 defines the required runner labels, architecture
assertions, native matrix, sanitizer suite, default-schedule fuzz smoke, and
aggregate-result handling, but the branch has not been pushed and no hosted run
exists yet.

**Reason:** Push, tag, publication, and release operations are outside Task 9
authorization.

**Impact:** Local author evidence validates the workflow structure and the
underlying commands, while Linux x86-64 and macOS arm64 hosted outcomes remain
pending. A workflow definition is not a hosted pass.

**Closure criteria:** The first authorized GitHub Actions execution must show
successful `native_tests` matrix legs and `native_sanitizers`; any runner-image
or command failure must be fixed and re-run before P1 receives hosted evidence.

### Local macOS libFuzzer schedule evidence

**Current limit:** On Darwin 25.5.0 arm64, Homebrew LLVM 22.1.6 with matching
libc++, ASan, and libFuzzer aborts inside libFuzzer's own
`InputCorpus::AddRareFeature`, through libc++
`__uninitialized_allocator_relocate`, with an ASan heap-buffer-overflow while
loading the seed corpus under its default entropic power schedule. The stack
has not entered a FastDB input failure. LLVM 21.1.7 instead spins during ASan
shadow initialization on this OS.

**Reason:** This is a local compiler-runtime compatibility limit. Task 9 does
not change Core or harness behavior to mask it.

**Impact:** Each of the four tracked seeds is executed separately through the
ASan+UBSan harness, and the same matching LLVM 22 build completes 10,000
coverage-guided runs with libFuzzer's supported `-entropic=0` schedule. This is
valid local product evidence with an explicit schedule limit, not a
default-schedule pass and not a FastDB defect. The Linux hosted sanitizer job
retains the exact default schedule for its 1,000-run smoke.

**Closure criteria:** Obtain a successful default-schedule run on the hosted
Linux job and retain the local adjusted-schedule evidence; refresh the local
default-schedule run when a compatible macOS LLVM runtime is available.

## P1 observations closed during implementation

### Deep object and shared-child regression coverage

**Closed by Task 9:** `payload.json_depth.object_serialize`,
`payload.json_depth.object_destroy`, and `payload.json_depth.object_failure`
exercise a 50,000-level object chain. `payload.json_depth.shared_child_dag`
copies, moves, and releases 50,000 independent parents sharing one immutable
child in varied order, then proves a retained survivor remains queryable.

**Evidence:** These named CTest cases run in ordinary Debug, Release, and
sanitizer suites and are deep/wide enough to expose recursive traversal or
destruction, double release, use-after-free, and reference-imbalance
regressions.

### Planned schema manifest globs

**Closed by Task 4 and extended by Task 7:** The repository now contains
`schemas/fastdb.payload.v1.schema.json`, its Core-JCS digest pin
`schemas/fastdb.payload.v1.schema.sha256`,
`schemas/fastdb.payload.manifest.v1.schema.json`, and `schemas/README.md`. Both
schemas are embedded as raw bytes by the deterministic standard-library-only
generator.

**Evidence:** `uv build` produced `dist/fastdb4py-0.1.22.tar.gz` and the local
wheel without any no-match warning for `schemas/*.json`, `*.sha256`, or `*.md`.
The current sdist inventory contains all four exact paths. The separately
tracked SWIG 325/451 diagnostics above remain present and open; they were not
normalized as part of this closure.
`python3 tools/generate_embedded_payload_schemas.py --check` passes, and
`payload.json_document` and `payload.compiled_spec` verify that the embedded raw
schemas parse through `JsonDocument`, Core JCS/SHA-256 matches the checked-in
source-schema pin, and both schemas declare their exact draft/id and closed
shapes. The pin has also been rechecked from 2,074 canonical bytes with the
official JCS reference at commit
`19d51d7fe467d4706a3ff08adf8a748f29fc21e0` before comparison with the Core
result.

## Program checklist

| Slice | Status | Required closure evidence |
|---|---|---|
| P1. Core contract compiler/query ABI | Locally complete and frozen; first hosted execution pending | Independent Task 9 review is accepted; obtain first hosted native/sanitizer results without rewriting them as local evidence |
| P2. Record binary/runtime/lifetime | In progress: internal logical arena and complete record builder implemented; binary/plan/backing/lifetime remain open | Exact binary layout document and goldens; builder/plan/backing; deterministic record build/open; checked views/materialize/invalidate for every legal non-`ref` type |
| P3. Object-graph runtime | Blocked on P2 | Object pools, roots, shared refs, cycles, hardened open, checked view/materialize/invalidate; only Issue 0001 D1 remains outside direct construction |
| P4. Language projections and payload codegen | Blocked on P2/P3 | C++/Rust/Python/TypeScript-WASM parity and deterministic C++/Rust/Python/TypeScript in-memory artifact generation from Core |
| P5. Clean cut, release, downstream composition | Blocked on P1-P4 | Public call-db/schema/columnar authority removed, `RecordEngine` rename complete, packages at 0.2.0 pass release gates, then C-Two composes the nested FastDB sub-spec without semantic duplication |

## Non-deferrable 0.2.0 work

The following gaps cannot be moved to Issue 0001 merely to shorten a slice or release:

- strict compilation, normalization, JCS, SHA-256, stable errors, and query ABI;
- complete declared V1 types, nullability, `record.v1`, and ordinary `object_graph.v1` behavior;
- exact deterministic binary format and hardened validation;
- truthful backing, ownership, direct/staged, checked-view, materialization, and invalidation behavior;
- Rust and Python parity plus the official TypeScript/WASM projection;
- four-language payload code generation from the Core;
- clean removal of the old public call-db/columnar authority before 0.2.0.

## Closure criteria

Close this issue only when all of the following are true:

- P1-P5 rows are complete with links to implementation commits and verification evidence;
- every non-deferred success criterion in the governing design passes on its required platforms/languages;
- current public documentation describes shipped behavior without target-state caveats;
- the call-db/`fastdb.schema.v1`/`columnar.v1`/`ColumnEngine` clean-cut scan passes under the design's historical-document allowlist;
- FastDB 0.2.0 packages and C-Two downstream composition consume one Core-owned payload meaning;
- every capability still absent after that point is tracked separately with exact current limit, rationale, impact, and closure criteria.
