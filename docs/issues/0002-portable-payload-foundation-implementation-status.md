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
current repository implements the P1 compiler/query boundary, the complete P2
non-reference record Core, the Task 9 public C builder/plan/open/owner tranche,
and the local Task 10 checked view/access/materialization C ABI and C++ runtime
facade, while it still ships the 0.1.x call-db/`ColumnEngine` implementation.
P1 Task 9 has completed local
independent implementation review; its first hosted CI execution remains
pending. P2 Tasks 7 and 8 pass their local Debug, Release, Core-wasm,
language/package, and ABI-33 gates; their independent completion reviews each
report zero Critical, Important, or Minor findings. P2 Task 9 also passes its
local Debug, Release, Core-wasm, language/package, cross-target C-header, and
exact ABI-72 gates, and its final independent review reports zero Critical,
Important, or Minor findings. A Task 10 retained-log audit found that the
historical Task 7-9 sanitizer CTest exit-passes contained recoverable
test-harness alignment diagnostics; the corrected cumulative Task 10
halt-on-error sanitizer proof supersedes those earlier cleanliness claims as
recorded below. P2 Task 10 local implementation and gates are complete with the
exact 99-symbol ABI, and its final independent review reports zero Critical,
Important, or Minor findings. Task 11 workflow/package quality,
source-build Emscripten propagation, proof mapping, reviewed binary-open
corpus, and deterministic/fuzz robustness targets are now implemented locally;
its complete fresh local gate is green and its final P2 review remains open.
P3-P5 graph, language-parity, codegen, clean-cut, and release work also remain
open. This issue records that temporary gap through the 0.2.0 clean cut.

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
- P2 Task 1 now provides a Core-internal, record-profile logical-value builder over the frozen P1 `ResolvedSpec`: stable source-node runtime type IDs, flat `ValueNode`/index vectors, stable entry roots, copied text/wide-text/opaque storage, explicit iterative expectation frames, schema-driven `many + component` record batches, exact-width fixed runs, strict UTF-8/UTF-16 validation, exact bit-level normalized binary64 finite/range comparison, transactional retryable mutation, deterministic builder limits/accounting, and stable builder errors 2009-2013. Every authoring mutation computes its final logical frame accounting without cloned state and preflights node/storage/total limits before scratch, capacity growth, or input publication; empty container closure uses the same allocation-free cascade accounting as scalar and wide-text leaves. Fixed-run preflight accounts the consumed value count exactly: a partial run retains its top frame, while a complete multi-value run closes that frame and every completed ancestor before total-byte enforcement. Fixed-run logical limits are likewise preflighted before count-sized scratch or input traversal; native scalar inputs are loaded through exact-width typed objects; input spans are bounded by the Core's `size_t`/`ptrdiff_t` addressability helper before pointer arithmetic; and full-width error facts use lossless decimal strings;
- P2 Task 2 freezes the normative [`fastdb.payload.bin.v1`](../../schemas/fastdb.payload.bin.v1.md) header, seven-kind region matrix, entry directory, canonical offsets/padding, fixed/component slots, list/pool partitions, numeric rules, hardened-open work accounting, limits, and closed binary errors. `RuntimeSchema` is now the sole runtime-ID authority consumed by the Task 1 builder; its iterative reachability and linear component-layout passes preserve all-source IDs, compile/cache only the reachable component subgraph without renumbering, reuse finite component DAG layouts, and reject reachable internal cycles/refs without recursion. Layout planning composes digest-equal independently compiled specs by stable source index rather than pointer identity, and open preflights static spec counts plus known minimum descriptor work before runtime/index construction;
- Task 2 also adds an internal checked `RecordLayout`, ascending bounded `ByteSink` encoder, and strict byte-loading `open_record` for empty and component-free non-normalized fixed scalar records. The two-case ordered corpus pins exact hand-reviewed bytes and independent SHA-256 files, including zero-length boundaries, nullable storage, signed zero, infinities, canonical NaNs, digest embedding, and deterministic repeated builds. `NON_CANONICAL_BINARY` (`3009`) and `INVALID_BINARY_VALUE` (`3010`) are stable without adding any public function; the public ABI remains the frozen 33 symbols;
- P2 Task 3 extends that private Core runtime through every fixed-width scalar, including exact u8n/u16n quantize/dequantize, and through arbitrary finite acyclic inline component reuse under `one` and `many`. The normalized helper decomposes binary64 inputs into bounded integer limbs, applies exact nearest-even code selection, and rounds decoded rationals directly to binary64 bits without `long double`, `fenv`, fast-math, or a binding. Component encoding/open are iterative; immediate validity bytes lead each slot, every field is aligned, empty components use stride one, and every gap, tail, and null component slot is canonical zero;
- Task 3 also replaces the decoded-observation index with immutable entry/field slot metadata. Observation requires the private caller to supply the same still-live immutable byte image accepted by `open_record`; the index validates span shape but owns no payload backing, cannot prove later pointer/content identity, and stores no second decoded value tree. The four-case ordered corpus adds independent numeric-edge and nested/reused-component bytes and hashes; malformed-byte, checked-arithmetic, exact validation-work, allocation-failure, native sanitizer, and Core-only wasm32/Node coverage exercise the new paths. This creates no binding projection or public runtime API, and the frozen public ABI remains exactly 33 symbols;
- P2 Task 4 adds deterministic UTF-8, UTF-16LE, and opaque-byte pools to that same private runtime. Canonical traversal covers root and nested component values without deduplication, every variable slot carries a checked offset/length descriptor, and strict open rejects pool inventory, range, partition, padding, UTF-8, and UTF-16LE violations in deterministic order. The fifth ordered binary golden pins embedded NULs, supplementary UTF-16, repeated equal values, empty present values, nulls, and arbitrary opaque octets; an independent byte receipt and Core-only wasm32/Node coverage pin little-endian text bytes and exact pool offsets;
- Task 4 also staged only a private eager/lazy text-validation switch. Eager open validates all text content and charges UTF-8 bytes plus UTF-16 code units; lazy open validates the complete bounded metadata/partition shape but retains no unchecked text pointer or text accessor. Task 7 now governs that switch through its Core-private `OpenOptions`; this is still not a public API and adds no Task 8 access pin or lifetime guarantee;
- P2 Task 5 completes the private `record.v1` non-reference algebra with recursive lists of every scalar, variable, component, and list item type. One immutable sparse descriptor-fact set and one aggregate per reachable list runtime type are produced by the same iterative canonical DFS that owns list and pool cursors; encoder writes consume those facts rather than recreate traversal state. Strict open validates the complete ordered `LIST_VALIDITY`/`LIST_ITEMS` inventory, checked descriptor partitions, null-zero storage, list-element limits, exact work/depth accounting, and final consumption without recursion. The sixth and seventh ordered goldens cover nested lists and the full root/component/list algebra matrix; independent no-Core directory reconstruction and exact Core-only wasm32 round trips pin both images;
- P2 Task 6 added a private immutable, repeatable `BuildPlan`, exact plan facts, Core heap and validated external backing execution, byte-identical direct/range/staged images, operation-specific closed callback-status classification, explicit reservation ownership, and the interim move-only inline `PendingPayload`. Plan-allocation failure leaves the builder retryable; successful commit transfers one reference, while invalid committed spans release that reference once and never roll back. Core heap reserve checks alignment padding against both platform size arithmetic and the actual byte-vector container limit before allocation, and maps allocation and container-length failures to `ALLOCATION_FAILED`; native and wasm32 regressions prove oversized requests return structurally without adopting backing ownership;
- P2 Task 7 cleanly removes `PendingPayload` and the old private `OpenLimits` spelling. Core now has one safe-default `OpenOptions`, one dependency-neutral `ExecutionReport`, and move-only `RetainedBacking` acquisition that requires retain/release, binds one exact base/size, classifies retain status through the shared callback taxonomy, and transfers exactly one retained reference into an owner without a second retain;
- Task 7 `PayloadOwner::open_copy` preflights null/addressability/total-byte limits before allocation or read, copies into committed Core heap backing, and validates that owned image through the shared reader. `open_external` requires the repeated span to match the retained base/size exactly before reading, then runs the same reader; any validation/publication failure releases the retained reference once. Both paths contain `bad_alloc` and `length_error` as stable `ALLOCATION_FAILED` results;
- the new cheaply copyable, logically immutable `PayloadOwner` shares one `State` containing exactly one committed/retained backing reference, one `CompiledSpec`, one completely validated `PayloadIndex`, and one real optional `ExecutionReport`. Byte-open owners report `std::nullopt`; plan-created owners retain the exact direct/staged report. Owner copies share State without another backing retain, and digest/profile remain independent of caller spec, source bytes, builder, or plan lifetime;
- `BuildPlan::execute` now uses a `.cpp`-local move-only committed-image handoff for direct/staged composition and validates only the final committed image through `open_record` before shared-State publication. Publication options take the maximum of safe defaults and immutable plan/spec facts, including stable entry/component counts. Non-allocation reader failures become `BACKING_CONTRACT` with original reader facts; reader or State allocation failures remain `ALLOCATION_FAILED`; every post-commit failure releases once and never rolls back. A commit-returned readable base may legally differ from the writable base: exact non-null/size/capacity shape is checked, and the shared reader validates the returned image;
- Task 7 focused Debug evidence passes `payload.record_binary`, `payload.payload_backing`, and `payload.payload_open`, including exact physical `/binary/...` diagnostics, retained-span ownership, final-image reader validation, post-commit allocation cleanup, retry/live-byte balance, and legal relocated commit spans. Full Debug and Release each pass 24/24 tests; the ASan+UBSan CTest run exits 24/24, with its historical cleanliness inference superseded by the Task 10 retained-log audit below; the available focused TSan backing/concurrency target passes; the Core wasm32/Node harness passes; the existing ABI remains exactly 33 symbols; Python passes 413 tests, compileall, sdist, and wheel build; TypeScript/WASM passes 76 tests after a clean binding rebuild. Controller pre-commit review passes, and the independent completion review reports zero Critical, Important, or Minor findings after independently rerunning full Debug, the three focused targets, ABI-33, strict JSON/corpus inventory, and diff checks;
- P2 Task 8 extends the one hardened-open `PayloadIndex` rather than adding a second reader: it shares one immutable `RuntimeSchema`, retains only open-validated list-slot facts, and provides one offset-only cursor vocabulary for entry, component, list, scalar, and variable-span derivation. Backed views cache no byte pointer, and existing Task 3 scalar observation seams are thin wrappers over that same cursor arithmetic;
- Task 8 adds one owner-state access barrier with one mutex/condition variable, non-wrapping generation, RAII short/long pins, deterministic invalidated/stale results, and idempotent drain-before-release invalidation. The backing is moved out under the barrier, released outside its mutex, and concurrent invalidators wait through that release before returning;
- Task 8 also adds copyable immutable `View`, move-only typed `Access`, explicit little-endian owned wide-text decoding, selected-span lazy text validation/limits, and iterative one-source-pin materialization into a fresh detached `ValueArena`. Materialization copies exact scalar bits, variable bytes, null/empty distinctions, and child links transactionally before publishing source-independent detached state;
- the focused `payload.checked_view` matrix passes navigation and every non-reference value kind, canonical component identity, text/opaque/wide access, lazy retry/concurrency and exact selected-span work limits, owner/view lifetime, transactional allocation-failure materialization, 100,000 short readers, decisive drain/release/reuse races, two deterministically observed invalidators, distinct-context callback reentry, stale generation, and generation exhaustion. Deep backed materialization and detached rematerialization also prove linear path-state storage through the Core-private metrics seam;
- Task 8 is independently reviewed and complete at its private Core boundary. Full Debug and Release each pass 25/25; a no-competing-load ASan+UBSan CTest run exits 25/25 with the two slow pressure tests completing in 91.63 and 101.11 seconds, with its historical cleanliness inference superseded below; focused TSan repeats pass without a race; Core-only wasm32/Node passes; the public ABI remains exactly 33 symbols; Python passes 413/413 plus compileall, sdist, and wheel; TypeScript/WASM passes 76/76 after a clean rebuild; and the final independent review reports zero Critical, Important, or Minor findings. The successful wheel build emits only the exact seven legacy diagnostics governed by [Issue 0003](0003-legacy-swig-diagnostics.md);
- P2 Task 9 projects the reviewed builder, immutable plan, backing callbacks, build execution, copy/external open, payload metadata/binary-copy, and invalidation through exactly 39 additive C exports. The public allowlist is now exactly 72 symbols. Pointer-free builder/open/plan/report prefixes are fixed at 88/112/104/72 bytes; fixed-run and backing prefixes remain target-sized and explicitly initialize every pointer member to null;
- At the Task 9 boundary, record capabilities truthfully reported only `compile,query,build,open,invalidate` with direct build `eligible/record_layout_exact`. Object-graph remains `compile,query` and `not_evaluated/runtime_slice_not_implemented`. The required manifest `runtime` facts are derived iteratively from the resolved source graph with fixed pool order and cycle-safe component reachability; canonical payload bytes and payload digests remain unchanged;
- a fresh arm64 macOS ASan+UBSan suite under AppleClang `21.0.0.21000101` exposed a CTest scheduling limit: `payload_builder` completed in 117.33 seconds, only 2.67 seconds below its former fixed 120-second timeout, while `record_binary` was killed by that timeout at 121.85 seconds. The same sanitizer-instrumented `record_binary` executable completed directly with `real 159.15`, `user 88.11`, and `sys 12.23` seconds and emitted no diagnostic. These two known heavy tests now use 240-second CTest timeouts to provide scheduler/load margin; their assertions, sanitizer instrumentation, and correctness criteria are unchanged. After reconfiguration, the formal CTest ASan+UBSan suite exits 26/26 in 244.99 seconds: `payload_builder` completes in 106.62 seconds and `record_binary` completes in 119.35 seconds. Its then-recorded suite-wide zero-finding conclusion is superseded by the retained-log audit below;
- Task 9 is independently reviewed and locally complete at its public builder/plan/open/owner boundary. Debug and Release each pass 26/26; focused ThreadSanitizer passes the runtime ABI, backing, and checked-view targets 3/3; Core-only wasm32/Node passes; Python passes 413/413 plus compileall, sdist, and wheel; TypeScript/WASM passes 76/76; the arm64, x86_64, and wasm32 C11 header checks pass; the symbol gate proves exactly 72 exports; and the final independent review reports zero Critical, Important, or Minor findings;
- A Task 10 audit of the retained sanitizer logs corrects the historical interpretation without changing the recorded CTest exit counts: Task 7, Task 8, and Task 9 logs contain respectively 4, 12, and 16 recoverable UBSan `SUMMARY` diagnostics for misaligned construction. The source is the test-only allocation-failure harness: it inserted an allocation header but aligned ordinary `operator new` results only to this platform's 8-byte `alignof(max_align_t)`, below C++'s 16-byte `__STDCPP_DEFAULT_NEW_ALIGNMENT__`. This is not a FastDB Core production allocator defect, but it invalidates the earlier claim that those full sanitizer runs were diagnostically clean. Task 10 cleanly fixes all five affected harnesses (`record_binary`, `payload_backing`, `payload_open`, `checked_view`, and `runtime_abi`) by validating requested power-of-two alignment, preserving at least default-new/header alignment, checking size arithmetic in stages, and using `std::align`. A cumulative ASan+UBSan run with `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` and `ASAN_OPTIONS=halt_on_error=1` now passes 27/27 in 85.03 seconds; an explicit retained-log scan finds zero runtime errors, UBSan/ASan summaries, ASan errors, or LeakSanitizer diagnostics. This current halt-on-error result is the superseding sanitizer proof through Task 10;
- P2 Task 10 locally projects the reviewed Core view/access/materialization model through exactly 27 additive C exports, freezing the sorted public ABI at exactly 99 symbols. View handles retain atomically, access handles own one unique live pin, no view call exposes a backing pointer, payload/UTF-8/opaque spans may alias only while an `Access` lives, and wide text is copied into aligned host-endian units from explicit UTF-16LE loads. The header-only C++17 facade remains a thin C-ABI projection and checks every public `uint64_t` to `size_t` conversion before exposing a span;
- Task 10 record manifests now report `compile,query,build,open,view,materialize,invalidate`; object-graph capabilities remain unchanged, and canonical specification bytes and payload digests remain unchanged. The new C ABI and C++ facade exercise navigation, every non-reference kind, owner/view/access lifetime, stale generations, drain-before-release invalidation, detached materialization, allocation-failure retry, and output clearing;
- Task 10 is independently reviewed and locally complete: Debug and Release each pass 27/27; the cumulative halt-on-error ASan+UBSan run above passes 27/27 with a zero-diagnostic retained-log scan; focused ThreadSanitizer passes runtime ABI/C++ facade/backing/checked-view 4/4; the Core numeric/binary wasm32 harness, pure-C wasm link smoke, and selected C++ facade wasm paths pass; Python passes 413/413 plus compileall/sdist/wheel; TypeScript/WASM passes 76/76; the arm64/x86_64/wasm32 C11 header checks pass; and the exact symbol gate proves 99 unique exports. The package inventories contain the normative binary document, both schemas and the digest pin, vendored provenance, public headers, and native library without build/cache/corpus/generated-image debris. A full wasm runtime-ABI claim is explicitly excluded below. The final independent review reports zero Critical, Important, or Minor findings, and Task 11 remains open;
- Task 10 exposed an Emscripten prerequisite: the exception-based C++ facade needs matching exception catching at Core/consumer compile and final link. Its consumer-only experiment was intentionally not a passing public source-build contract;
- Task 11 now selects the broadly compatible JavaScript-based Emscripten exception model through one source-build CMake interface contract. Core catch sites, real `fastdb` C++ consumers, and final links receive `-fexceptions`; pure-C compilation does not. The real public target passes the Core harness, pure-C smoke, C++ facade, and single-thread injected-failure runtime-ABI proof under Node. This closes the Task 10 exception-propagation prerequisite without claiming pthread coverage;
- Task 11 also defines separate Emscripten and Python package jobs, native and wasm exact ABI-99 checks, exact sdist/wheel inventory validation, the exact seven-diagnostic SWIG baseline owned by Issue 0003, and one executable path-aware aggregate whose 16 scope combinations and every incorrect job result are tested. These are hosted job definitions only until an authorized run exists;
- Task 11 adds `fuzz_payload_open` over borrowed input with bounded options, one fixed matching spec and one deliberate digest-mismatch spec. The same harness is driven deterministically over 10 reviewed named seeds covering valid empty/fixed/text/list payloads and malformed magic/length/offset/validity/text/list shapes. Successful opens traverse every value kind through the public C ABI, exercise scoped spans, materialize before invalidation, and traverse the detached result afterward; failures are opened twice and compared across status, code, symbol, path, message, and canonical details. A machine-checked 14-class map names the hostile binary, backing, allocation, generation, and access-drain proofs;
- Task 11's complete fresh local gate is green: Debug and Release each pass 30/30 with exact native ABI-99; the hard-fail ASan+UBSan suite passes 30/30 with a zero-diagnostic retained-log scan; focused ThreadSanitizer passes 4/4; the Core, pure-C, C++ facade, and single-thread injected-failure Emscripten paths pass with exact wasm ABI-99 and structural flag inspection; arm64/x86-64/wasm32 C11 checks pass; Python passes 413/413 plus compileall and fresh exact sdist/wheel inventory; and TypeScript/WASM passes 76/76. The local libFuzzer and LeakSanitizer toolchain limits remain explicitly bounded below. Final independent P2 review remains open;
- Rust, Python, TypeScript/WASM portable-payload projections and P3+ remain open;
- current public call-db, `fastdb.schema.v1`, `columnar.v1`, and `ColumnEngine` surfaces remain 0.1.x migration inputs, not the accepted 0.2.0 authority.

The P1 compiler/query slice is implemented, independently reviewed, and frozen
for P2 work. Its first hosted CI execution remains pending. The repository must
not claim that the complete portable payload foundation or FastDB 0.2.0 is
implemented.

## Current limit

Users and downstream repositories can compile/query a specification and can
now author, build, open, copy, inspect metadata for, and invalidate a complete
non-reference `record.v1` payload through the public C ABI, navigate checked
views, hold scoped borrowed access, materialize detached values, and use the
thin C++ runtime facade. No Rust, Python, or TypeScript/WASM portable projection
exists. Task 11 now defines the workflow/package gates, public build guidance,
proof mapping, binary-opening robustness, and single-thread wasm
failure-containment proof. P2 is not closed because the complete fresh local
gate is green but final independent review remains open; hosted outcomes are
also pending. C-Two may consume only
reviewed public FastDB contracts and must not depend on private headers or
recreate FastDB semantics. Toodle consequently cannot yet treat the full
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

## P2 requirement-to-test traceability

This table maps the accepted P2 contract to the current Core authority and
named local evidence. It is a proof index, not a claim that later phases or
hosted jobs have completed. Task 11's complete fresh local gate is green;
independent P2 review remains open at this point.

| Accepted requirement | Implementation authority | Exact proof |
|---|---|---|
| Design Section 7: one immutable compiled specification owns canonical bytes, digest, indexes, manifest, and capabilities | `CompiledSpec`, `SchemaRepository`, and `Manifest.cpp` in `fastcarto/fastdb/src/payload/spec/` | `payload.compiled_spec`, `payload.spec_abi`, ordered specification and binary golden indexes |
| Section 7: record runtime facts are Core-derived without changing specification identity | `Manifest.cpp`, `RuntimeSchema`, `fdb_payload_v1_spec_manifest_json` | `test_manifest_indexes_facts_and_capabilities`, `payload.runtime_abi`; canonical specification bytes/digests remain pinned by the P1 corpus |
| Section 7/10: every all-source runtime ID is stable and the builder consumes the same resolved identity | `RuntimeSchema`, `RecordLayout`, `PayloadBuilder` | `test_runtime_ids_reachability_and_component_layout`, `test_builder_consumes_the_same_runtime_schema_ids` in `payload.record_layout` |
| Section 8.1: schema-driven record authoring, including the record-batch frame and exact-width fixed runs | `PayloadBuilder`, `fdb_payload_v1_builder_entry_begin`, `fdb_payload_v1_builder_value_fixed_run` | `test_record_batch_components_and_lists`, `test_fixed_runs_and_transactional_failures` in `payload.payload_builder`; C parity in `payload.runtime_abi` |
| Section 8.1: all non-`ref` scalar, component, text/bytes, and recursive-list values with missing/null distinctions | `ValueArena`, `PayloadBuilder`, `RuntimeSchema` | `payload.payload_builder`, `payload.record_binary`, `payload.runtime_abi`; `fixed-scalars`, `text-bytes`, `nested-components`, `nested-lists`, and `component-list-composition` goldens |
| Section 8.2: freeze seals the builder and publishes an immutable repeatable plan | `PayloadBuilder::freeze`, `BuildPlan` | `payload.payload_builder`, `payload.payload_backing`, `payload.runtime_abi` |
| Section 8.3: one immutable owner retains the validated backing/spec/index/report | `PayloadOwner::State`, `PayloadOwner::open_copy`, `PayloadOwner::open_external` | `payload.payload_open`, `payload.checked_view`, `payload.runtime_abi` |
| Section 8.4: checked views capture generation and detached materialization survives source invalidation | `View`, `Access`, `materialize`, C ABI view/access/materialize families | `payload.checked_view`, `payload.runtime_abi`, `payload.runtime_cpp_facade` |
| Section 8.5: immutable reads are concurrent and invalidation drains active access before release | `AccessBarrier`, `PayloadOwner::invalidate` | `payload.checked_view`, `payload.runtime_abi`; focused native ThreadSanitizer is the concurrency authority |
| Section 8.5/9: callbacks run outside unrelated Core locks; distinct-context reentry works while same-context/token reentry remains an explicit V1 precondition | `BuildPlan::execute`, `Backing`, `AccessBarrier` | `test_repeatable_concurrent_and_reentrant_execution`, `payload.runtime_abi`; the same-context/token limitation and closure criterion remain recorded below |
| Section 9: Core heap and caller external backing have explicit reserve/write/commit/rollback/retain/release ownership | `Backing`, `HeapBacking`, `BuildPlan::execute`, public `fdb_payload_v1_backing_v1_t` | `payload.payload_backing`, `payload.payload_open`, `payload.runtime_abi` |
| Section 9: direct/staged truth and `REQUIRE_DIRECT` never silently fall back | `ExecutionReport`, `BuildPlan::execute` | `payload.payload_backing`, `payload.runtime_abi` |
| Section 9: callback status mapping, relocated committed spans, rollback, and post-commit cleanup | `Backing.cpp`, `RetainedBacking`, `BuildPlan.cpp` | `payload.payload_backing`, `payload.payload_open`, `payload.runtime_abi` |
| Section 10: exact 128-byte header, 56-byte region descriptor, 40-byte entry descriptor, seven region kinds, each count unit, relative variable descriptors, order/alignment/zero rules | `BinaryFormat.hpp`, `RecordLayout`, `RecordEncoder`, `open_record`; normative `schemas/fastdb.payload.bin.v1.md` | `test_region_matrix_zero_boundaries_and_partition_rules`, `test_task4_malformed_pools_and_descriptors_have_exact_errors`, `test_task5_list_region_descriptor_and_partition_failures`; seven ordered binary goldens |
| Section 10: numeric bits, canonical NaNs, normalized endpoints/ties, UTF-8/UTF-16LE, pool/list partitions | `NormalizedInteger`, `TextEncoding`, `RecordEncoder`, shared `open_record` validation | `payload.record_binary`, Core `payload_runtime_harness.js`; `numeric-edges`, `text-bytes`, `nested-lists`, and `component-list-composition` goldens |
| Section 10: bounded open validates before publishing a view and charges static plus selected-span work exactly | `OpenOptions`, `open_record`, `PayloadOwner::open_copy/open_external` | `test_open_preflights_static_spec_limits_and_known_work`, `test_lazy_selected_span_work_boundary`, `payload.runtime_abi` |
| Section 11: fixed-width pure-C ABI, opaque handles, explicit ownership, future-tail structs | `fastdb_payload.h`, `fastdb_payload.cpp` | `payload.c_header_smoke`, `payload.spec_abi`, `payload.runtime_abi`, future-tail canaries |
| Section 11: exact public ABI is frozen at 99 symbols on native and applicable wasm objects | `tests/abi/fastdb_payload_v1_symbols.txt` | `tools/check_payload_abi_symbols.py --build-dir ...` and `--wasm-build-dir ...` |
| Section 11: C++17 is a thin RAII projection over the C ABI | `fastdb_payload.hpp` | `payload.cpp_facade`, `payload.runtime_cpp_facade`; C/C++ parity assertions |
| Section 12: stable code/path/message/canonical-details errors and no exception crosses the C boundary | `Error`, `Result`, ABI catch/owned-error publication in `fastdb_payload.cpp` | `payload.error`, `payload.spec_abi`, `payload.runtime_abi` |
| Section 18.2: build/open/value matrix for every P2 type and entry/component/list context | shared builder/layout/open/view paths | `payload.payload_builder`, `payload.record_binary`, `payload.checked_view`, `payload.runtime_abi`; ordered binary corpus |
| Section 18.2: null versus empty, signed zero/infinities/NaNs, normalized ties, invalid text, counts/limits/work | shared Core layout/open validation | `payload.record_binary`, `payload.payload_builder`, Core wasm runtime harness |
| Section 18.4: heap/external, direct/range/staged, injected callback failures, retain/release balance | `BuildPlan`, `Backing`, `PayloadOwner` | `payload.payload_backing`, `payload.payload_open`, `payload.runtime_abi` |
| Section 18.4: checked owner/view retention, stale generation, active-access drain, and detached lifetime | `AccessBarrier`, `View`, `Materialize` | `payload.checked_view`, `payload.runtime_abi`, `payload.runtime_cpp_facade` |
| Section 18.4: `wstr` access is copied, aligned, host-endian, and valid only for the scoped access lifetime | `Access::wstr`, detached-arena projection, C ABI access family | `test_checked_view_access_materialize_and_barrier_abi`, `payload.checked_view`; assertions prove aligned values and storage disjoint from UTF-16LE payload bytes |
| Section 18.5: pure-C compile, exact layouts, malformed-format classes, resource limits, and deterministic diagnostics | C header plus the one Core binary reader | `payload.c_header_smoke`, `payload.record_binary`, `payload.payload_open`, `payload.binary_open_corpus`, `fuzz_payload_open`, `tools/check_payload_binary_corpus.py`, and `tests/ci/p2_malformed_class_map.json` |
| Section 18.5: memory/undefined-behavior instrumentation | shared CMake sanitizer configuration | complete ASan+UBSan CTest with halt/abort-on-error and retained-log zero-diagnostic scan; local macOS uses `detect_leaks=0` because Apple ASan rejects LeakSanitizer, while hosted Linux keeps `detect_leaks=1`; historical Task 7-9 cleanliness wording remains superseded |
| Section 18.7: macOS arm64 and wasm32 local compatibility | native CTest/C11/ABI gates; Core/C/C++ Emscripten targets | current local arm64 native gates; `payload_runtime_harness.js` exercises Core numeric/binary wasm32, and the public target also proves C/C++/single-thread runtime status paths |
| Section 18.7: Linux x86-64 and macOS arm64 hosted compatibility | `.github/workflows/tests.yml` native matrix and path-aware aggregate | workflow definition only. Hosted results remain pending until an authorized push/run |
| Goal P2: deterministic byte-identical direct/staged record payloads | one `RecordLayout`/`RecordEncoder`/`open_record` authority | `payload.record_binary`, `payload.payload_backing`, binary SHA-256 receipts |
| Goal P2: open, traverse, scoped borrow, materialize, invalidate, and release through C/C++ | 99-symbol C ABI and `fastdb_payload.hpp` | `payload.runtime_abi`, `payload.runtime_cpp_facade`, `payload.checked_view` |
| Goal P2: source-build WebAssembly contains Core failures as stable C statuses | `fastdb_payload_emscripten_exception_model`, public `fastdb` target, ABI catch sites | `fastdb_payload_wasm_runtime_abi_single_thread --single-thread-injected-failure`, `tools/check_emscripten_exception_flags.py` |
| Goal P2: package surfaces contain the normative contract, Python extension, and native libraries without build debris or unreviewed SWIG diagnostics | `MANIFEST.in`, Python build configuration, Issue 0003 baseline | `tests/ci/test_check_python_package_inventory.py` and `tools/check_python_package_inventory.py` over the retained build log, one sdist, and one wheel |
| P3 boundary: graph specifications compile, but no partial graph runtime exists | stable `RUNTIME_UNAVAILABLE` projection | `payload.runtime_abi`; Object-graph runtime remains P3 |

### Emscripten exception choice and current closure

The selected source-build model is JavaScript-based `-fexceptions`. The
[official Emscripten exception
documentation](https://emscripten.org/docs/porting/exceptions.html) states
that exception catching is disabled by default, that `-fexceptions` must be
used at compile and link time, and that this model works in all JavaScript
engines with WebAssembly support at higher overhead. Native WebAssembly
exceptions can reduce overhead but are not supported by every engine.

The CMake contract therefore applies the C++ compile option to Core catch
sites, propagates it to C++ source consumers of the real `fastdb` target, and
propagates the final-link option to C and C++ consumers. Pure-C compilation is
left unchanged. The single-thread WebAssembly runtime-ABI mode exercises
injected allocation failures without executing any worker phase and requires
stable C status/error mapping. It does not claim pthread support; native plus
ThreadSanitizer remains the concurrency evidence.

The workflow defines Linux/macOS native, Linux sanitizer, Emscripten runtime,
Python package, Python regression, and TypeScript regression jobs with an
exhaustively tested path/result aggregate. Hosted results remain pending; a
workflow definition is not a hosted pass. Task 11 now requires only its
independent final review before P2 can be called locally frozen; its complete
fresh local gates are green.

## Remaining P2-P5 gaps

### P2 record binary, runtime, and lifetime

**Current limit:** P1 compiles/queries a specification, P2 Task 1 has the
internal logical arena and record authoring state machine, P2 Task 2 has the
exact binary contract, and P2 Tasks 3-5 have the private deterministic
encoder/open for all fixed-width scalars, arbitrary finite acyclic inline AoS
components, canonical UTF-8, UTF-16LE, and opaque-byte pools, and recursive
lists across the complete non-reference algebra. P2 Task 6 has the private
repeatable plan and truthful final-backing execution. Task 7 now adds
Core-private safe-default `OpenOptions`, hardened copy/external open,
exact-span `RetainedBacking`, and one shared immutable `PayloadOwner` whose
State owns the backing, compiled spec, validated offset-only index, and a real
optional execution report. Plan execution validates only its final direct or
staged committed image through the same reader before publication; post-commit
reader/publication failure releases once and never rolls back. Commit
relocation remains legal: a successful commit may return a different readable
base from its writable base when non-null/size/capacity shape and reader
validation succeed. Task 8 now adds the independently reviewed Core-private
single access barrier, generation, checked `View`/`Access`, transactional
detached materialization, and drain-before-release invalidation over that same
owner and index. Its focused, full-suite, sanitizer, wasm, language/package,
ABI-33, and zero-finding independent-review gates are closed.

Task 9 now provides the public builder, plan, backing, build/open/owner C ABI,
payload metadata, independent binary copy, and invalidation. Task 10 now
locally exposes checked view/access/materialization and the thin C++ runtime
projection through the complete exact 99-symbol C ABI. Its local native,
sanitizer, Core numeric/binary wasm, pure-C wasm smoke, selected C++ facade
wasm, language/package, cross-target C11, and symbol gates pass, and its
independent review is accepted with zero findings. Task 11 workflow/package
structure, public consumer guidance, proof mapping, reviewed binary-open
corpus/targets, and the single-thread wasm status proof are implemented
locally, and its complete fresh local gate is green. Hosted execution and final
independent review remain open.

Rust, Python,
TypeScript/WASM portable-payload projections, P3 object graphs, and later
slices remain absent. Task 6/7
callbacks may perform distinct-context nested execution because Core holds no
unrelated lock, but a callback must not synchronously re-enter execution
through the same backing context/token.

Task 11 closes the concrete wasm32 source-build prerequisite exposed by Task
10. The public `fastdb` target now propagates JavaScript-based `-fexceptions`
to Core/C++ compile and final link steps while leaving pure-C compilation
unchanged. The Core numeric/binary harness, pure-C header/link smoke, C++
runtime facade, and a deliberately single-threaded runtime-ABI allocation-
failure mode all execute under Node through the real public target. Structural
inspection pins the option at each required phase and proves that the
single-thread target does not enable pthreads.

The wasm proof remains deliberately narrower than the native runtime ABI
suite. It does not execute the worker phases of `payload.runtime_abi` and does
not claim generic WebAssembly pthread support. Native plus ThreadSanitizer
remains the concurrency authority.

**Reason:** Checked access and invalidation reuse the single Task 7 owner and
offset-only index. The public ABI must project that reviewed lifetime model
rather than expose private headers or invent a second reader. Callback
execution is synchronous within one execution, while an external adapter may
serialize or mutate one owner context/token; Core cannot safely infer a
same-context/token reentrant ownership contract. Relocated commit spans are an
accepted backing capability, so pointer identity cannot replace reader
validation. Emscripten disables C++ exception catching by default, while the
Core uses exception containment internally to project stable C statuses; that
compile/link requirement and the separation of single-thread failure proof
from optional pthread concurrency require the explicit public build contract
and separate proof modes now supplied by Task 11.

**Impact:** Supported C and C++ callers can author and own portable record
bytes, navigate typed values, hold scoped borrowed spans, and materialize a
detached value through public handles. The binary reader now also has reviewed
deterministic and coverage-guided robustness entry points. This local surface
cannot yet be called the completed P2 contract because the Task 11 final review
remains open. No binding may bypass
the public access/lifetime rules or replace them
with private Core access.
Same-context/token callback reentry remains an adapter precondition;
distinct-context execution and nested execution are supported. Existing 0.1.x
database/call-db bytes are not a substitute. On wasm32, the named
single-thread C-ABI allocation-failure proof is supported; concurrency remains
outside that proof.

**Next owner slice:** Finish FastDB P2 Task 11 review, then begin FastDB P3.

**Closure criteria:** Task 9 exposes builder/plan/backing/build/open/owner
through the public C ABI. Task 10 locally exposes the already reviewed checked
view/access/materialization behavior and C++ runtime facade without changing
the Task 9 contracts; its final independent review reports zero unresolved
findings. Task 11 has now closed workflow/package structure, consumer build
guidance, proof mapping, binary-opening quality, and the single-thread wasm
exception-propagation requirement, and its complete fresh local gate is green.
It must still obtain final independent P2 review before P2 is called complete.
A future same-context/token reentrant callback contract requires
explicit adapter ownership/serialization semantics and invalid nested-callback
tests; until then only distinct contexts may re-enter. Pass deterministic,
malformed-input, backing-failure, lifetime, sanitizer, wasm, ABI, and
cross-platform gates before calling P2 public.

### P3 object-graph runtime

**Current limit:** P1 validates `object_graph.v1` specifications at compile
time. For otherwise valid ABI calls, Task 9 `builder_create`, `open_copy`, and
`open_external` return stable `RUNTIME_UNAVAILABLE` (`2009`) with canonical
details
`{"profile":"object_graph.v1","reason":"runtime_slice_not_implemented"}`.
Following the frozen acquisition order, `open_external` retains before Core
runtime validation and releases that successful retain exactly once (`1/1`).
There are no runtime object pools, roots, object IDs, shared references,
cycles, graph build/open, graph views, or graph materialization.

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

**Current limit:** The P1 compile/query C ABI and C++ facade exist, Task 9
publicly exposes C build/open/owner operations, and Task 10 locally adds the
C++ runtime facade over the same C ABI. There is still no Rust, Python, or
TypeScript/WASM portable-payload projection, and the Core does not generate
C++, Rust, Python, or TypeScript payload artifacts. Existing hand-written 0.1.x
call-db layers are migration inputs, not portable-payload projections.

**Reason:** Safe binding lifetimes, value parity, and generated APIs depend on
the frozen P2/P3 runtime ABI and binary meaning.

**Impact:** Portable compile/query/build/open/view/materialize is available to
C and C++ callers at the locally implemented Task 10 boundary. No Rust, Python,
or TypeScript/WASM binding can yet exercise the shared golden corpus, and no
language can consume a Core-owned generated artifact set.

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

### Legacy call-db uninitialized descriptor-member nondeterminism

**Current limit:** The supported Python range is `>=3.10`, but on CPython 3.13
two independent legacy `encode_call_db` / `encode_call_db_into` builds of the
same scalar-array value can differ at byte 182. The existing byte-for-byte
test reproduces this at the pre-P2-Task-5 starting commit
`0f08e9feeed09a7acba8d3639e0f0e3d2091a756`; the official CPython 3.14t gate
passes and Task 5 does not change the affected legacy code.

**Reason:** `FastVectorDbLayerBuild::Impl::addField` declares
`field_desc_ex_t fd` and does not initialize `element_type` for non-list
fields, while `FastVectorDbLayerBuild::Impl::write` serializes the complete
descriptor object representation. In the reproduced scalar-array image,
binary offset 182 maps to that uninitialized `element_type` member; its value
changes across otherwise identical encodes.

**Impact:** Logical decode still succeeds, but legacy call-db bytes are not
deterministic across repeated supported-runtime builds. Serializing an
uninitialized member can also disclose process-local residual bytes. This is
a legacy 0.1.x/P5 owner defect, not portable-record Task 5 behavior, and no
new portable authority may depend on those bytes.

**Closure criteria:** P5 removes the obsolete call-db surface, or the legacy
owner initializes the complete descriptor representation (including a
defined non-list `element_type`) and adds deterministic repeated-encode tests
across the supported Python matrix before P5. The repair must be separately
scoped and must not be folded into a portable-record slice.

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

### Hosted workflow evidence

**Current limit:** Task 11 extends the Task 9 runner labels, architecture
assertions, native matrix, and sanitizer suite with Emscripten runtime, exact
native/wasm ABI, Python package, and executable path-aware aggregate gates, but
the branch has not been pushed and no hosted run exists yet.

**Reason:** Push, tag, publication, and release operations are outside Task 11
authorization.

**Impact:** Local author evidence validates the workflow structure and the
underlying commands, while Linux x86-64, macOS arm64, Emscripten, and package
hosted outcomes remain pending. A workflow definition is not a hosted pass.

**Closure criteria:** The first authorized GitHub Actions execution must show
successful `native_tests` matrix legs, `native_sanitizers`, `wasm_core`,
`package_tests`, language jobs, and the final aggregate; any runner-image or
command failure must be fixed and re-run before hosted evidence is recorded.

### Local macOS libFuzzer evidence and toolchain limits

**Historical P1 compiler-harness evidence:** On Darwin 25.5.0 arm64, Homebrew
LLVM 22.1.6 with matching libc++, ASan, and libFuzzer aborts inside libFuzzer's own
`InputCorpus::AddRareFeature`, through libc++
`__uninitialized_allocator_relocate`, with an ASan heap-buffer-overflow while
loading the seed corpus under its default entropic power schedule. The stack
has not entered a FastDB input failure. LLVM 21.1.7 instead spins during ASan
shadow initialization on this OS.

**Reason:** This is a local compiler-runtime compatibility limit. Task 9 did
not change Core or harness behavior to mask it.

**Historical impact:** Each of the four tracked specification seeds was
executed separately through the ASan+UBSan harness, and the same matching LLVM
22 build completes 10,000
coverage-guided runs with libFuzzer's supported `-entropic=0` schedule. This is
valid local product evidence with an explicit schedule limit, not a
default-schedule pass and not a FastDB defect.

**Current Task 11 binary-open limit:** AppleClang 21 compiles
`fuzz_payload_open.cpp` with the requested sanitizer instrumentation, but its
installed Xcode toolchain has no `libclang_rt.fuzzer_osx.a`, so the executable
cannot link. The previously used Homebrew LLVM 22 binary currently cannot
start: it is linked to `libz3.4.15.dylib`, while the installed Z3 package
provides 4.16. No system package, dylib link, or compiler installation is
mutated to manufacture a local pass.

**Current Task 11 local leak-check limit:** AppleClang 21's AddressSanitizer
rejects `detect_leaks=1` before any test starts because LeakSanitizer is not
supported on this macOS platform. The fresh fully instrumented Task 11 build
therefore ran with `ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0`
and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`: all 30 CTest targets
passed in 81.12 seconds and the retained logs contained no runtime, ASan, UBSan,
or leak diagnostic. This is memory- and undefined-behavior evidence, not a
local LeakSanitizer pass. The hosted Linux job deliberately retains
`detect_leaks=1`; its result remains pending until an authorized run exists.

**Current Task 11 impact:** A local 10,000-iteration coverage-guided
`fuzz_payload_open` result is therefore not claimed. The exact harness source
compiles, its reviewed 10-seed corpus is reproduced and hash-checked, the same
harness executes deterministically as `payload.binary_open_corpus`, and the
complete hard-fail ASan+UBSan CTest suite supplies local product evidence. The
Linux hosted sanitizer definition retains default 1,000-run smokes for both
the specification compiler and binary opener, but remains an expected job
definition until an authorized run exists.

**Closure criteria:** Obtain successful default-schedule runs for both fuzz
targets on the hosted Linux job and retain the historical local adjusted-
schedule evidence; run the Task 11 binary opener for 10,000 local iterations
when a matching launchable macOS Clang/libFuzzer toolchain is available.

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
| P2. Record binary/runtime/lifetime | In progress: Core behavior is independently reviewed through Task 10 at exactly 99 symbols. Task 11 workflow/package structure, public consumer guidance, proof mapping, binary-opening robustness, single-thread wasm status mapping, and complete fresh local gates are green; final review remains open | Obtain final independent P2 review; keep hosted outcomes pending until an authorized run exists |
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
