# Issue 0002: Portable Payload Foundation Implementation Status

- **Status:** Open
- **Opened:** 2026-07-16
- **Owner:** FastDB
- **Governing design:** [FastDB Portable Payload Foundation Design](../superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
- **Governing decision:** [ADR-0001](../decisions/0001-portable-payload-core-authority.md)
- **First implementation plan:** [Portable Payload Core Contract Implementation Plan](../superpowers/plans/2026-07-16-portable-payload-core-contract.md)
- **P2 implementation plan:** [Portable Payload Record Runtime Implementation Plan](../superpowers/plans/2026-07-17-portable-payload-record-runtime.md)
- **P3 implementation plan:** [Portable Payload Object-Graph Runtime Implementation Plan](../superpowers/plans/2026-07-20-portable-payload-object-graph-runtime.md)
- **P4 implementation plan:** [Portable Payload Language Projections and Codegen Implementation Plan](../superpowers/plans/2026-07-21-portable-payload-language-projections-codegen.md)
- **P5 design:** [Portable Payload P5 Clean-Cut Design](../superpowers/specs/2026-07-23-portable-payload-clean-cut-design.md)
- **P5 implementation plan:** [Portable Payload P5 Clean-Cut Implementation Plan](../superpowers/plans/2026-07-23-portable-payload-clean-cut.md)
- **Post-0.2 deferrals:** [Issue 0001](0001-portable-payload-deferred-capabilities.md)

## Purpose

The accepted 0.2.0 design defines a complete portable-payload foundation. The
current repository now has locally frozen P1 compiler/query, P2
`record.v1`, and P3 ordinary `object_graph.v1` Core/runtime slices. P3 Tasks
1-9 implement one graph topology and authoring model, exact profile-2 bytes and
hardened open, truthful direct/staged backing, checked views and invalidation,
reachable-closure materialization, the exact 105-symbol C ABI, a thin C++17
facade, the reviewed 16-seed hostile corpus/fuzz target, and full Core Wasm
graph proof. Task 10 formally closes D1 and freezes that local evidence without
changing production code, ABI, schema semantics, package version, or release
metadata.

The final P3 read-only review is performed by the context-owning primary agent
because the user explicitly prohibited delegation for this work. It reports no
unresolved Critical, Important, or material Minor finding, but it is not an
independent/subagent review and this issue does not claim otherwise. Local
macOS sanitizer/fuzzer constraints and all exact Task 10 gate results are
recorded below. Hosted CI remains pending because no push was authorized.

P4 is locally complete through Tasks 1-9. Rust, Python, and official
TypeScript/Wasm project compile/query, author/freeze/plan,
execution/backing/open, checked record and graph views, Core-owned
materialization/invalidation, and the immutable four-target ArtifactSet from
the same C++ Core. Task 8 published exact ABI-117 and executed simple generated
outputs; Task 9 closes exact output ceilings, concurrent determinism, the
four-shape hostile compiler/import matrix, package/workflow evidence, and the
same-agent primary review. P5 Task 1 now removes the Python-owned
feature-discovery generator and makes `fdb codegen` a creation-only filesystem
facade over the same Core ArtifactSet. The Python and TypeScript duplicate
authority, native/SWIG debris, no-alias `RecordEngine` rename, and executable
clean-cut policy are locally frozen through P5 Task 6. P5 Task 7 now completes
the fresh local native, sanitizer, language, generated-output, installed
package, and Wasm release-readiness matrix; it also closes a real Python 3.10
Windows-artifact-path fallback defect found by the minimum-version installed
wheel. The **P5 local clean cut is complete** at exact ABI-117. Hosted results,
versioning, push/tag/publication/release, and C-Two-owned composition remain
open. Package metadata is still 0.1.x, so Issue 0002 remains open and no
FastDB 0.2.0 release is claimed.

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
- `BuildPlan::execute` now uses a `.cpp`-local move-only committed-image handoff for direct/staged composition and validates only the final committed image through `open_payload` and its focused profile reader before shared-State publication. Publication options take the maximum of safe defaults and immutable plan/spec facts, including stable entry/component and graph-object counts. Non-allocation reader failures become `BACKING_CONTRACT` with original reader facts; reader or State allocation failures remain `ALLOCATION_FAILED`; every post-commit failure releases once and never rolls back. A commit-returned readable base may legally differ from the writable base: exact non-null/size/capacity shape is checked, and the shared reader validates the returned image;
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
- Task 11 also defines separate Emscripten and Python package jobs, native and wasm exact ABI-99 checks, exact sdist/wheel inventory validation, the exact seven-diagnostic SWIG baseline owned by Issue 0003, and one executable path-aware aggregate whose 16 scope combinations and every incorrect job result are tested. The package job now has exact Python 3.10/3.12 matrix legs. Its standard-library-only checker derives and cross-checks distribution identity from sdist/wheel filenames and their `PKG-INFO`/`METADATA`, so it does not import a Python-3.11-only TOML module. These are hosted job definitions only until an authorized run exists;
- Task 11 adds `fuzz_payload_open` over borrowed input with bounded options, one fixed comprehensive matching spec and one deliberate digest-mismatch spec. All 10 reviewed seeds now embed the comprehensive spec digest: the Core-generated `component-list-empty` golden proves the empty form; reviewed scalar/text mutations provide distinct valid fixed/text forms; the full composition is the list form; and every malformed seed derives from one of those matching valid images. The deterministic runner asserts each seed's expected success or exact status/path before invoking the shared fuzz harness, so malformed offset/validity/text/list coverage cannot silently collapse to `DIGEST_MISMATCH`. Successful opens traverse every value kind through the public C ABI, exercise scoped spans, materialize before invalidation, and traverse the detached result afterward; fuzz failures are opened twice and compared across status, code, symbol, path, message, and canonical details. A machine-checked 14-class map names the hostile binary, backing, allocation, generation, and access-drain proofs;
- Task 11's complete fresh post-review-fix local gate is green: Debug passes 30/30 in 22.46 seconds and Release passes 30/30 in 8.70 seconds, each with exact native ABI-99; the hard-fail ASan+UBSan suite passes 30/30 in 95.78 seconds with a zero-diagnostic retained-log scan; focused ThreadSanitizer passes 4/4; the Core, pure-C, C++ facade, and single-thread injected-failure Emscripten paths pass with exact wasm ABI-99 and structural flag inspection; the unchanged arm64/x86-64/wasm32 C11 checks remain green; Python passes 413/413 plus compileall; a fresh Python 3.10 sdist/wheel build passes the exact inventory checker under both Python 3.10 and the current interpreter; and a clean paired TypeScript/WASM rebuild passes 76/76. The local libFuzzer and LeakSanitizer toolchain limits remain explicitly bounded below. The first review's two Important findings are corrected, and the same reviewer confirms both closed with zero Critical, Important, or Minor findings;
- P3 Tasks 1-8 provide one Core-owned graph topology, logical authoring,
  complete normative profile-2 layout/encoding and hardened open, iterative
  reachability, typed index-cursor facts, immutable graph BuildPlan,
  direct/staged backing execution, strict final-image validation, checked
  backed navigation, source-independent reachable-closure materialization, and
  the shared P2 owner/access/invalidation lifecycle. Task 8 projects that one
  implementation through exactly six additive C exports and a thin C++17
  facade; public graph build/open/view/materialize/invalidate is now available;
- Task 8 appends compatible `max_graph_objects` and `graph_object_count` tails,
  keeps the V1 prefixes exactly 88/104 bytes, and freezes the public ABI at
  exactly 105 symbols while preserving the old 99 declarations and record
  behavior. Guard-page and canary tests prove old and partial prefixes do not
  read or write the new tails;
- graph manifests truthfully report runtime `available`, layout
  `object_pool_aos`, operations
  `compile,query,build,open,view,materialize,invalidate`, direct build
  `eligible/graph_layout_exact_after_freeze`, and no codegen targets. Task 9
  adds the 16-seed graph-aware hostile corpus, fuzz traversal/equality, full
  Core Wasm graph runtime, ABI/package/workflow hardening, complete fresh local
  gates, and a zero-finding primary-agent review. Task 10 closes D1 and freezes
  P3 locally at ABI-105. The P4 live delta audit is now complete and its
  [language-projection/codegen design](../superpowers/specs/2026-07-21-portable-payload-language-projections-codegen-design.md)
  plus [executable implementation plan](../superpowers/plans/2026-07-21-portable-payload-language-projections-codegen.md)
  are frozen as P4 delta authority. P4 Tasks 1-9 add real compile/query,
  author/freeze/plan, execution/backing/open, and checked-record-view
  projection slices over the unchanged ABI-105: a
  payload-only native archive
  built from the same Core object files, raw/safe Rust crates, the
  `fastdb4py.payload` package, and the official TypeScript/Wasm payload
  subpath. A generated Emscripten export inventory is derived mechanically
  from the exact reviewed allowlist; it exposes all 105 C functions without
  changing their declarations, definitions, or meaning. The three language
  projections now author every V1 value kind, fixed runs, nullable/empty lists,
  object identities/references, explicit roots, and immutable Core-owned plan
  facts; execute through truthful Core backing modes; copy/open owned images;
  and navigate every non-ref record value through generation-checked views.
  Task 4 adds scoped access, exact scalar bits, `str`/`wstr`/bytes, detached
  Core materialization, and synchronous drain-before-release invalidation;
  Task 5 adds safe graph ref/identity observation; Task 6 freezes ordered
  parity and package/link proof; Task 7 adds one private Core-owned ArtifactSet
  and four deterministic renderers; Task 8 publishes exact ABI-117 and all
  generated targets; and Task 9 closes hostile generation, exact ceilings,
  concurrent determinism, packages/workflow, documentation, and local review.
  P5 Task 1 then removes `fastdb4py.codegen` and its Python feature discovery,
  while the replacement CLI reads exact specification bytes and writes exact
  Core artifacts for C++, Rust, Python, or TypeScript into a new tree. All
  remaining P5 work remains open;
- current public call-db, `fastdb.schema.v1`, `columnar.v1`, and `ColumnEngine` surfaces remain 0.1.x migration inputs, not the accepted 0.2.0 authority.

P1, P2, P3, and P4 are locally implemented and frozen. P1/P2 retain their recorded
independent-review evidence; P3 uses the user-authorized context-owning
primary-agent review and does not claim independence; P4 uses the same explicit
review classification. Their first hosted CI execution remains pending. The
repository must not claim FastDB 0.2.0 readiness or release because P5 remains
non-deferrable.

## Current limit

Users and downstream repositories can compile/query a specification and can
author, build, open, copy, navigate, materialize, and invalidate complete
`record.v1` and ordinary `object_graph.v1` payloads through the public C ABI and
thin C++ facade. Graph sharing, cycles, explicit ref dereference, payload-scoped
identity, direct/staged backing, and the shared checked owner/access lifetime
are executable. P2 is independently reviewed and locally frozen; P3 is locally
frozen at the exact 105-symbol boundary with D1 closed, complete Core Wasm
graph execution, and the explicitly non-independent primary-agent review
described below. Rust, Python, and official TypeScript/WASM compile/query,
author/freeze/plan, execute/open, navigate, materialize, invalidate, and
generate through exact ABI-117. Their projections preserve canonical bytes,
digest, manifest, capabilities, stable indexes, every V1 authoring kind,
Core-owned limits/errors, exact immutable plan facts, retained clones, and
explicit disposal without private semantic paths.

The public Core-owned ArtifactSet renders deterministic payload-only C++, Rust,
Python, and TypeScript source. Simple generated outputs prove successful value
roundtrips and cross-spec rejection; the closing hostile matrix compiles,
imports, or type-checks all-values records, recursive lists,
keyword/generated-prefix collisions, and shared cyclic graphs through the
official runtimes. Native and ThreadSanitizer receipts additionally pin
per-target exact byte ceilings and concurrent path/bytes/SHA-256 equality. The
versioned proof map closes every runtime/codegen/closure row while explicitly
recording same-agent review, hosted-pending status, and P5-open status. Hosted
outcomes remain pending.
Downstream consumers may use
only reviewed public FastDB contracts
and must not depend on private headers or recreate FastDB semantics. The full
structured-payload foundation remains incomplete because P5 is open.

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
the final same-reviewer result is zero Critical, Important, or Minor findings,
so this P2 proof index is locally frozen.

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
| P3 Sections 9 and 14-16: graph specifications have one Core topology/logical authoring/runtime authority, complete profile-2 layout/encoding/hardened-open semantics, the shared plan/backing/owner lifecycle, checked navigation/materialization for every V1 value, and one additive public projection | `RuntimeTopology`, `RuntimeSchema`, `GraphAuthoring`, `GraphLayout`, `GraphEncoder`, `ProfileLayout`, `BuildPlan`, `open_payload`/`open_graph`, `PayloadOwner`, explicit inline/object/ref cursor variants, `GraphView`; six `fdb_payload_v1_*` graph exports and thin C++ `ObjectHandle`/`GraphIdentity` facade | `payload.graph_runtime_schema`, `payload.graph_builder`, `payload.graph_binary`, `payload.graph_backing`, `payload.payload_open`, `payload.graph_view`, `payload.graph_materialize`, `payload.runtime_abi`, `payload.runtime_cpp_facade`, guard-page/canary V1/V2 prefix tests, seven annotated graph goldens, seven exact invalid receipts, allocation sweeps, the no-full-image direct proof, a 50,000-object cycle, 32-thread immutable traversal, generation/drain/release proofs, record regressions, the 16-seed graph-aware corpus/fuzz harness, full Core Wasm graph proof, and exact public ABI-105 |

## P3 requirement-to-proof traceability

The machine-readable
[`p3_malformed_class_map.json`](../../tests/ci/p3_malformed_class_map.json)
now contains two complementary proof indexes:

- the original 17 exact hostile-input/runtime proof classes, whose entries are
  checked for definition and execution by `check_p3_runtime_quality.rb`;
- an 18-row mapping for every P3 design Section 4-21 and a 14-row mapping for
  every active-goal Stage B requirement. Each row names the Core implementation
  file/symbol and an exact test, golden, corpus, or repository gate.

Together those rows explicitly cover all V1 values and nullability, roots and
refs, shared/self/mutual cycles, immutable repeatable plans, direct/staged
truth, malformed object/root/ref IDs and inventory, work/resource limits,
checked views and drain-before-release lifetime, dense reachable-closure
materialization, manifest truth, old/new ABI prefixes, exact ABI-105, full Core
Wasm execution, fuzz/corpus evidence, and frozen record non-regression. The map
sets D1 to `closed-task-10`; the repository quality gate accepts that state
only when this issue contains the matching closure marker below.

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
workflow definition is not a hosted pass. Task 11's complete fresh local gates
are green and its same-reviewer final review reports zero Critical, Important,
or Minor findings. P2 is locally frozen without converting any hosted
expectation into a pass.

## Frozen P2 closure and remaining P3-P5 gaps

### P2 record binary, runtime, and lifetime

**Frozen local status:** P1 compiles/queries a specification, P2 Task 1 has the
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
locally, its complete fresh local gate is green, and the same reviewer reports
zero Critical, Important, or Minor findings after confirming both initial
Important findings closed. P2 is locally complete and frozen. Hosted execution
remains pending.

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
deterministic and coverage-guided robustness entry points. This surface is the
locally completed and frozen P2 contract. It is not the complete 0.2.0
foundation: P3-P5, hosted evidence, language parity, codegen, clean cut, and
release remain open. No binding may bypass
the public access/lifetime rules or replace them
with private Core access.
Same-context/token callback reentry remains an adapter precondition;
distinct-context execution and nested execution are supported. Existing 0.1.x
database/call-db bytes are not a substitute. On wasm32, the named
single-thread C-ABI allocation-failure proof is supported; concurrency remains
outside that proof.

**Next owner slice:** FastDB P3 object-graph runtime.

**Closure criteria:** Task 9 exposes builder/plan/backing/build/open/owner
through the public C ABI. Task 10 locally exposes the already reviewed checked
view/access/materialization behavior and C++ runtime facade without changing
the Task 9 contracts; its final independent review reports zero unresolved
findings. Task 11 has now closed workflow/package structure, consumer build
guidance, proof mapping, binary-opening quality, and the single-thread wasm
exception-propagation requirement, and its complete fresh local gate is green.
The same reviewer reports zero Critical, Important, or Minor findings after
confirming both initial Important findings closed, so the local P2 closure
criteria are satisfied.
A future same-context/token reentrant callback contract requires
explicit adapter ownership/serialization semantics and invalid nested-callback
tests; until then only distinct contexts may re-enter. Pass deterministic,
malformed-input, backing-failure, lifetime, sanitizer, wasm, ABI, and
cross-platform gates before calling P2 public.

### P3 object-graph runtime

**Current limit:** P3 Task 1 adds one Core-owned, iterative source/runtime
topology derivation and makes both manifest facts and `RuntimeSchema` consume
it. The derivation preserves the frozen all-source runtime-ID order, records
entry-value versus component-field context, assigns ordinary/list/root/inline
component/reference storage roles, identifies reachable and identity-bearing
components, and terminates over shared, self, mutual, and 20,000-component
reference cycles. Graph roots and refs have internal eight-byte aligned ID
slots while by-value component fields retain their existing inline AoS layout;
record runtime IDs, component layouts, list inventory, manifest bytes, and
binary behavior remain frozen.

P3 Task 2 now adds a Core-private logical graph authoring state machine on that
topology. A builder may declare identity-bearing objects in interleaved
component-local pools, receive process-unique non-zero temporary handles, fill
each declaration exactly once in any order, and author typed object roots and
references including forward, shared, self, and mutual references. Temporary
handles are resolved immediately to dense `(component_index, object_id)`
coordinates and are never stored in `ValueNode`, `LogicalPayload`, diagnostics,
or proof records. Successful logical freeze destroys the token registry and
publishes immutable per-component object pools; failed freeze leaves the
builder retryable. Fill and reachability validation are iterative and scan
components/declarations deterministically. Orphan declarations fail with
`UNREACHABLE_OBJECT` (`2015`), invalid/foreign/stale handles fail with
`INVALID_OBJECT_HANDLE` (`2014`), and stable object paths use
`/objects/<component-id>/<declaration-index>` without exposing a token. Each
declaration consumes one graph object, one value node, and the existing exact
64-byte logical node charge; the default graph-object limit is 10,000,000.

Task 2 local evidence passes the complete Debug suite `32/32`, the complete
hard-fail ASan+UBSan suite `32/32`, and the available focused ThreadSanitizer
graph-builder test `1/1`; the graph test includes allocation-failure retry,
32 concurrent builders, and a 50,000-object cycle. Native exports remain the
exact reviewed 99-symbol allowlist and the existing binary-open corpus remains
`10/10`. Unchanged consumers pass Python `413/413`, compileall, sdist/wheel,
and TypeScript/WASM `76/76`. The package build emits only the seven existing
SWIG diagnostics owned by Issue 0003. Local Apple AddressSanitizer requires
`detect_leaks=0`, so this result is not claimed as LeakSanitizer evidence.
These are local results only; no hosted outcome, release, push, or publication
is implied.

P3 Task 3 freezes the complete normative profile-2 binary contract without
changing profile 1: the 128-byte header uses profile value `2`, region kind
`OBJECT_VALUES = 8` stores one dense AoS pool per identity-bearing component,
and every object root/reference is a schema-typed 8-byte object-ID slot with
no duplicate physical root/ref table. Shared container/layout/sink helpers are
narrow extractions; record traversal, bytes, diagnostics, work accounting, and
the exact 99-symbol ABI remain unchanged. Core-private `GraphLayout`,
`GraphEncoder`, and `open_graph` execute the initial list-free fixed graph
slice. Three hand-audited source/hex/SHA/layout receipts cover a zero-count
identity pool, present object ID zero versus null, and shared/self-cyclic
objects whose declaration order fixes IDs while fill order does not.

Task 3 local evidence passes the complete Debug and Release suites `33/33` in
each configuration. The complete hard-fail ASan+UBSan suite also passes
`33/33`; after the final review made encoding consume frozen layout facts
linearly and corrected structural component-depth accounting, the focused
sanitizer graph-binary test passes again `1/1`. Local Apple AddressSanitizer
uses `detect_leaks=0`, so none of these results is claimed as LeakSanitizer
evidence. Native exports remain the exact reviewed 99-symbol allowlist, with
no public graph function, and the reviewed binary-open corpus remains `10/10`.
Schema embedding, JSON, Markdown relative links, and all `17` CI helper tests
pass. Unchanged consumers pass Python `413/413` and TypeScript/WASM `76/76`
after a fresh WASM rebuild. The three independently decoded graph images match
their checked-in SHA-256 receipts. These are local results only; no hosted
outcome, release, push, or publication is implied.

P3 Task 4 completes that Core-private byte runtime for every existing V1 kind:
fixed and normalized scalars, `str`, `wstr`, `bytes`, recursive lists, inline
components, identity roots, and refs. One canonical physical traversal now
produces descriptor facts, list aggregates, variable-pool partitions, object
regions, exact validation work, and monotonic encoded bytes without following
root/ref targets in place. Hardened open consumes the exact entry/object/list/
pool inventory, validates every object structurally before iterative
reachability, enforces typed ID bounds and exact partitions, and rejects the
first unmarked object in component/object-ID order. `max_graph_objects` and
native marker/queue capacities are checked before marker or queue allocation.

The private index now distinguishes `InlineValueCursor`,
`IdentityObjectCursor`, and `RefCursor`; identity cursors retain component/ID
coordinates without inventing a runtime type ID. Object-pool, list, variable,
root-count, object-count, region-count, and validation-work facts are published
only after the complete byte image passes. The generic record view and
materialization internals were adapted to the explicit cursor variant so
profile 1 remains exact; non-inline graph materialization remains deliberately
closed until its graph-closure task.

Task 4 adds four complete all-value/nested-list/disconnected-root/null-empty
source/hex/SHA/layout receipts to the three fixed Task 3 graph images and seven
checked invalid hex/error receipts. The malformed matrix covers exact object
inventory and descriptor fields, profile isolation, padding and validity
tails, typed root/ref bounds (including an empty target pool), list/pool
partitions, text, Boolean/NaN values, reachability, and resource limits.
Allocation-failure sweeps exercise the complete list/variable/multi-pool graph
layout and open paths with successful retry, and a real 50,000-object cycle
passes builder, freeze, layout, encode, open, and iterative reachability.

The current Task 4 native gate passes Debug `33/33`, Release `33/33`, and the
hard-fail ASan+UBSan suite `33/33`; the retained sanitizer log contains no
runtime-error or sanitizer diagnostic. Local Apple AddressSanitizer uses
`detect_leaks=0`, so this is not LeakSanitizer evidence. Native exports remain
the exact reviewed 99-symbol allowlist. A fresh wasm32 build caught and closed
one `uint64_t`-to-`size_t` component-index allocation conversion; the rebuilt
TypeScript/WASM consumer then passes `76/76`. Python remains `413/413` with
compileall, the reviewed binary-open corpus remains `10/10`, all `17` CI
helper tests pass, and all seven graph image SHA-256 receipts match. These are
local Core-private results only; no hosted outcome, release, push, or
publication is implied.

P3 Task 5 integrates that byte runtime into the existing P2 lifecycle without
adding a graph-specific planner, callback family, owner, or codec. `BuildPlan`
owns exactly one `ProfileLayout` variant (`RecordLayout | GraphLayout`), derives
the unchanged plan facts plus a Core-private tail `graph_object_count`, and
visits that variant to select exactly one monotonic encoder. `open_payload`
dispatches only to the focused `open_record` or `open_graph` validator before
the same `PayloadOwner::State` can be published. Record plans retain an exact
zero graph-object fact and their existing bytes, reports, callbacks, views,
and public ABI behavior.

Graph heap, stable-span, and range-write-only direct execution now produce the
same checked `graph-all-values` image, including cycles, sharing, lists,
`str`, `wstr`, and bytes. A declined direct reservation falls back only for
`ALLOW_STAGING`, with exact
`STAGED/BACKING_DECLINED_DIRECT/staging_bytes=total_bytes`; `REQUIRE_DIRECT`
returns before any staged reserve. Direct write/commit failures roll back once
without retry, while invalid committed bytes and post-commit publication
allocation failures release once and never roll back. Plan-construction
allocation failures restore the value arena, entry roots, and graph object
pools atomically, so the same builder can retry successfully; a successful
logical/plan freeze still destroys temporary handle authority.

The D1 direct proof uses a variable-width graph larger than four MiB, a
range-write-only final backing, and an allocation guard rejecting any single
allocation at least half the final image. Direct execution succeeds while a
deliberate full-image allocation fails under the same guard. A
`BUILD_TESTING`-only private heap-reserve observer records zero Core heap or
staged reserves on that path; source audit confirms `GraphEncoder` owns only
bounded traversal metadata, not a complete-image byte buffer. The final
reservation receives one monotonic, non-overlapping, complete write stream,
and the execution report is derived from the reservation path actually taken.

The final post-review Task 5 local rerun passes Debug `34/34` in 50.05
seconds, Release `34/34` in 30.88 seconds, and hard-fail ASan+UBSan `34/34`
in 195.17 seconds. The retained sanitizer log contains no ASan/UBSan
diagnostic; local Apple ASan uses `detect_leaks=0`, so this is not
LeakSanitizer evidence. Available ThreadSanitizer passes the focused backing,
graph-backing, and checked-view set `3/3` in 25.63 seconds. Native Debug and
Release and a fresh Core Emscripten build each retain exactly 99 public ABI
symbols; both Core WASM runtime harnesses pass. Python remains `413/413` plus
compileall, and a clean TypeScript/WASM rebuild remains `76/76`.

Independent repository checks pass the pinned dependency and embedded-schema
checks, the reviewed binary-open corpus `10/10`, all 17 CI helper tests, all
28 binary JSON documents under duplicate-key rejection, all seven graph
SHA-256 receipts, nine changed-document relative links, and `git diff
--check`. A production `BUILD_TESTING=OFF` build succeeds and contains no
heap-observer symbols. The repository formatter remains unavailable because
the local Homebrew LLVM executable links an absent `libz3.4.15.dylib`; no
system dependency was mutated. Compiler `-Werror`, Emscripten narrowing
warnings, manual style review, and the diff check remain the available local
format/compile evidence. No hosted result, push, tag, version change, release,
or publication is implied.

P3 Task 6 removes only the Core-private record-profile gate from
`PayloadOwner::entry_view`; all public graph build/open guards remain exact.
The existing explicit `IdentityObjectCursor` and `RefCursor` variants now flow
through the ordinary checked `View` navigation path. Identity objects expose
component fields without a synthetic runtime type ID, inline components have
no graph identity, `field` never dereferences a ref, and `ref_target` is the
only dereference operation. `graph_identity` returns the exact
payload-scoped `(component_index, object_id)` coordinate for a present
identity object or ref, including object ID zero. Null applicable values fail
with `UNEXPECTED_NULL`; inapplicable identity or dereference operations fail
with `TYPE_MISMATCH`.

Every backed graph operation acquires a short pin at the view's captured
generation before it observes kind, presence, coordinates, fields, or bytes.
Published children retain the same owner and generation but no pin or cached
backing pointer. Scalar, normalized, canonical-NaN, string, wide-string,
bytes, nested-list, inline-component, root, ref, shared-target, and cycle
navigation all reuse the P2 reader, owner, access barrier, and `Access` types;
there is no graph lock, object-node owner, cache, or unpinned span. A
32-thread immutable traversal succeeds, an active Access blocks invalidation,
new graph operations are rejected while invalidation drains, the generation
advances from 1 to 2, and external backing releases exactly once. A child view
also keeps external backing alive after the original owner handle is gone.
Injected child-view and Access allocation failures remain retryable.

The final Task 6 local gate passes Debug `35/35` in 50.94 seconds, Release `35/35`
in 41.27 seconds, and hard-fail ASan+UBSan `35/35` in 200.83 seconds. The
retained sanitizer log contains no ASan or UBSan diagnostic; local Apple ASan
uses `detect_leaks=0`, so this is not LeakSanitizer evidence. Available
ThreadSanitizer passes graph-backing, checked-view, and graph-view `3/3` in
23.32 seconds. Native Debug, native Release, and a fresh Core Emscripten build
each retain exactly 99 public ABI symbols; both Core WASM runtime harnesses
pass. Python remains `413/413` plus compileall, and a clean TypeScript/WASM
rebuild remains `76/76`. At that frozen Task 6 boundary graph materialization
still failed with `graph_materialization_not_available`; Task 7 below closes
only that Core-private gap and does not change the public graph boundary.

P3 Task 7 extends the existing detached `ValueArena` state with per-component
object-record pools and one graph materialization transaction; it adds no
second owner, backing, reader, cache, graph lock, or object-node lifetime
model. A backed source acquires exactly one access pin before the first
materialization allocation and keeps it through iterative discovery, copy,
validation, and final publication. A detached source uses the same transaction
without a backing. The published state retains only the immutable runtime
schema, copied nodes/bytes, and dense object pools; it has no source owner, pin,
byte pointer, or backing dependency. Existing record materialization still
publishes literally empty object pools.

Discovery walks the selected root and only its reachable ref closure without
native recursion. Each component's discovered source IDs are sorted ascending
and independently remapped to dense target IDs before the root and object
records are copied. Identity occurrences, refs, root kind, lists, inline
components, every fixed/normalized/variable V1 value, null versus empty,
sharing, self/mutual/multi-component cycles, and scalar bit patterns survive
the copy. Final validation checks root and child ranges, pool inventory and
record coordinates, every reachable ref/root target, byte ranges, and complete
node/object reachability before publishing the immutable state. A selected
object excludes disconnected roots; a selected ref remains a ref; materializing
an already detached graph produces an independent second state.

Task 7's focused graph-materialize/view/record set passes `3/3`. The complete
Debug and Release suites each pass `36/36` in 24.82 and 21.42 seconds. The
hard-fail ASan+UBSan suite passes `36/36` in 78.88 seconds and its retained log
contains no sanitizer diagnostic; local Apple ASan uses `detect_leaks=0`, so
this is not LeakSanitizer evidence. Available ThreadSanitizer passes the
focused graph-backing/checked-view/graph-view/graph-materialize set `4/4` in
21.10 seconds. A 12,000-object cycle with a wide root proves iterative
closure/copy, and the exhaustive allocation-failure sweep proves no partial
publication, balanced pins, retryable source use, and eventual success. An
external-backing race proves invalidation waits for the one full source pin,
releases backing exactly once, and leaves the detached result usable.

At the Task 7 boundary, native Debug, Release, sanitizer, and a fresh Core
Emscripten build still retained exactly 99 public ABI symbols. Both then-current
Core WASM runtime harnesses, including the single-thread injected-failure path,
passed. That evidence remains the historical private-runtime boundary; Task 8
supersedes its public-unavailable and ABI-count claims.

P3 Task 8 now projects the complete ordinary graph runtime through the existing
public builder, plan, payload, owner, view, access, materialization, and
invalidation handles. `builder_create`, `open_copy`, and `open_external` accept
both supported profiles and dispatch to the same C++ Core path. Exactly six
new exports add builder-local object declaration/fill/object/ref authoring and
explicit ref-target/graph-identity observation. The object handle is a
temporary, non-retained builder token; it is not a wire object ID. The C++17
`ObjectHandle` and `GraphIdentity` facade types only make one C call per method
and contain no graph topology, layout, reachability, binary, or materialization
authority.

The builder options and plan-info structs are now 96 and 112 bytes. Their V1
prefix constants remain exactly 88 and 104 bytes. A null options pointer uses
the 10,000,000-object default; sizes 88 and 89-95 never read the new tail; size
96 or larger reads `max_graph_objects`, with zero selecting the default. Plan
info sizes 104 and 105-111 write only the original 104-byte prefix; size 112 or
larger also writes `graph_object_count`. Unknown larger tails are ignored and
preserved. Native guard-page tests place the exact old allocation immediately
before `PROT_NONE`, proving no byte-88+ read and no byte-104+ write, while
portable canary tests cover partial and future prefixes.

This prefix contract is not an unconditional binary-compatibility promise for
an already compiled 0.1 caller that invokes the updated initializer with only
an 88- or 104-byte allocation. The unchanged initializer symbols now initialize
the current 96- and 112-byte structs, as required by the frozen Task 8
contract, and therefore require storage compiled from the current header. A
pre-V2 source caller must rebuild, pass null options, or populate and declare
the old prefix without calling the current initializer. FastDB is still 0.x,
so Task 8 takes this clean cut instead of adding a seventh versioned export or
hidden size inference. P5 owns release/migration wording; any later stable
binary-compatibility guarantee for initializer evolution requires an Accepted
ABI-versioning decision and an executable old-binary fixture.

The graph manifest now reports runtime `available`, layout
`object_pool_aos`, the shared-topology-derived pools, operations
`compile,query,build,open,view,materialize,invalidate`, direct build
`eligible/graph_layout_exact_after_freeze`, and an empty codegen target list.
The public ABI is exactly 105 sorted unique symbols: the old 99 declarations
and signatures plus the six approved graph exports. Apart from the two
documented initializer revisions, canonical payload JSON/digests, the binary
format, and record manifest/runtime behavior remain unchanged.

Task 8's final local evidence includes complete Debug and Release suites at
`36/36` in 57.94 and 36.10 seconds, a hard-fail ASan+UBSan suite at `36/36` in
229.66 seconds with zero retained sanitizer diagnostics and the local Apple
`detect_leaks=0` limitation, and a focused ThreadSanitizer
runtime/facade/backing/view/materialization set at `6/6` in 45.95 seconds.
The fresh Emscripten Core harness, pure-C link smoke, C++ facade, and
single-thread injected-failure path pass with exact wasm ABI-105; complete wasm
graph execution remains deliberately assigned to Task 9 and is not claimed
here. Python remains `413/413` plus compileall and exact fresh sdist/wheel
inventory; a clean TypeScript/WASM rebuild remains `76/76`. Dependency,
embedded-schema, reviewed 10-seed corpus, 17 CI-helper, 28 duplicate-safe JSON,
seven graph SHA-256, direct C11 arm64/x86_64/wasm32 compile/link, and document
checks pass. These are local results only.

#### P3 Task 9 local hardening evidence

This section records the completed local Task 9 hardening evidence, not P3
closure.
The reviewed binary-open corpus is now exactly **16/16 reviewed binary-open seeds**:
the frozen ten record seeds plus valid graph cycle/variable/null
images and malformed object-region/reference/unreachable images. Every seed
has one checked source recipe, SHA-256, matching Core specification, and exact
success or status/path result. The deterministic runner verifies all sixteen
before passing the same bytes to the graph-aware fuzz entrypoint. Repeated
opens compare status, code, symbol, path, message, and canonical details;
successful graph opens traverse explicit refs with a bounded visited
`(component_index, object_id)` set, acquire variable spans, materialize,
invalidate, and traverse the detached closure without parsing wire bytes in
the harness.

The executable 17-class P3 proof map covers profile/region inventory, object
descriptors, padding/null slots, typed ID bounds, reachability, canonical
values/lists/text, limits/work, builder handles, allocation and backing
failures, direct no-full-image behavior, view generation/drain,
materialization closure, ABI prefixes/output/exception containment, and Wasm.
Its tested quality gate rejects missing, duplicate, misordered, absent, or
unexecuted proofs; non-exact ABI-105/corpus inventories; stale graph manifest
truth; incomplete D1 facts; forbidden downstream ownership/type terms; and
missing workflow/package gates.

The **full graph wasm runtime** proof now builds graph-all-values through Core,
executes direct range writes, matches the exact golden bytes, traverses scalar
and variable values plus self/cross-component cycles, injects a transactional
allocation-class callback failure, materializes the root, invalidates the
source, and re-traverses the detached cycle under wasm32/Node. The separate
public single-thread ABI mode executes the exact six graph functions, old/new
struct prefixes, allocation boundaries, and exception-to-status containment;
the Wasm symbol gate remains exactly 105.

The complete fresh local gate is green. Debug passes 37/37 in 70.83 seconds
and Release passes 37/37 in 47.28 seconds, each with exact native ABI-105. The
hard-fail ASan+UBSan suite passes 37/37 in 287.68 seconds with a zero-diagnostic
retained-log scan; local Apple ASan uses `detect_leaks=0`, so this is not
LeakSanitizer evidence. Focused ThreadSanitizer passes 6/6 in 59.58 seconds
with no diagnostic. Core Wasm, pure-C Wasm, the C++ facade, single-thread
failure injection, the complete graph ABI mode, exception propagation, and
exact Wasm ABI-105 pass. Python remains 413/413 plus compileall; package-helper
tests pass 13/13 and a fresh sdist/wheel passes exact inventory checks;
TypeScript/Wasm remains 76/76 after a paired rebuild. Direct warning-clean C11
compilation passes for arm64, x86_64, and wasm32, and the fresh x86_64 FastDB
library plus pure-C smoke link and run. Dependency, embedded-schema, 16-seed
corpus, P2/P3 quality, duplicate-key JSON/YAML, relative-link, and diff checks
pass.

The default AppleClang installation still lacks its own libFuzzer archive.
Without mutating Homebrew, Xcode, or system dylinks, the local 1,000-run smoke
used AppleClang 21 with its matching Xcode ASan/UBSan resources and a
build-local resource overlay exposing only Homebrew LLVM 21's arm64
`libclang_rt.fuzzer_osx.a`. The default entropic schedule completed all 1,000
runs over the 16 seeds in six seconds with no sanitizer failure and no
artifact. This is explicit adjusted local linkage evidence, not a claim that
the default Apple toolchain contains libFuzzer; the standard hosted Linux
definition remains the authoritative unadjusted sanitizer/fuzz job.

**D1 remains open until Task 10.** Task 9 preserves the direct-path proof
contract and its `mode`, `fallback_reason`, and `staging_bytes` report fields,
but does not change D1 status before Task 10's formal closure review. The
context-owning **primary-agent review** inspected authority, resource bounds,
error equality, handle/access release, cycle termination, Wasm/Core ownership,
ABI, package/workflow truth, and remaining-scope claims. It found and closed
one test-harness robustness issue by rejecting null-plus-nonzero Wasm spans
before pointer arithmetic; no Critical, Important, or material Minor finding
remains. Per explicit user direction this is not an independent/subagent
review, and no such review is claimed. **Hosted results remain pending**:
workflow definitions are not hosted passes, and no push, tag, version,
release, or publication is authorized.

Object-graph runtime remains P3 and is not closed by Task 9 alone. Task 9 now
owns a locally green 16-seed graph-aware hostile corpus, fuzz
traversal/equality, malformed-class proof map, full Core Wasm graph execution,
ABI/package/workflow hardening, complete fresh gates, and the context-owning
primary-agent zero-finding review above. Task 10 still owns formal D1/P3
closure, truthful public documentation, complete evidence mapping, and final
primary-agent P3 review; the absence of independent delegation must remain
explicit. Rust, Python, and official TypeScript/WASM projections and codegen
remain P4; legacy clean cut, versioning, release, and C-Two composition remain
P5. No hosted outcome, D1 closure, release, push, tag, or publication is
claimed.

#### P3 Task 10 closure evidence

D1 closure state: `closed-task-10`

Task 10 closes the historical direct-graph deferral and freezes ordinary P3
locally. The implementation range is:

- `7ed553e` shared runtime topology;
- `996163e` graph authoring and temporary handles;
- `34d4b44` profile-2 binary freeze;
- `3dd7336` complete graph binary/open runtime;
- `5ee2eb8` exact immutable plans and truthful direct/staged execution;
- `a7cb69e` checked graph views and shared lifetime barrier;
- `4e74350` detached reachable-closure materialization;
- `2f06e52` six-function public graph projection and exact ABI-105;
- `2730e9e` graph-aware robustness, fuzz, full Wasm, package/workflow, and
  proof-map hardening;
- `5d5939b` strict proof-gate unit coverage for the open-to-closed D1
  transition, including positive class-inventory validation in either state.
- `1f0be6c` fail-closed validation for all design Sections 4-21 and all 14
  active Stage B trace rows, including real implementation file/symbol checks.
- `5807f72` preservation of the frozen P2 status gate without hard-coding P3
  as pending after P3's real closure.

D1 is closed by
`test_direct_graph_has_no_full_image_allocation`, the paired
`test_graph_staging_policy_and_cleanup`, and the source audit of
`GraphEncoder::encode_graph`, `AscendingWriter`, `ByteSink`,
`RangeCallbackSink`, `BuildPlan::execute`, the test-only heap-reserve observer,
and `ExecutionReport`. Together they prove one direct reserve, no staged or
Core heap-image reserve, exact monotonic full-length range coverage, success
under a threshold that rejects a complete image, truthful
`mode`/`fallback_reason`/`staging_bytes`, byte-identical staged execution, and
cycles/sharing/lists/text/bytes plus injected backing failures.

The tracked P3 proof map now maps all design Sections 4-21 and all 14 active
Stage B requirements to exact implementation symbols and named tests, goldens,
corpus entries, or repository gates. Its original 17-class executable matrix,
exact 16-seed corpus, exact ABI-105, graph manifest truth, D1 fact inventory,
package/workflow gates, and forbidden ownership/type checks remain enforced.

The context-owning primary agent performed the final read-only review of the
complete `f0aff71..5807f72` P3 design/implementation/quality range plus the
Task 10 documentation and proof-map delta. It reviewed Core authority,
topology/runtime agreement,
binary/ID/canonicality, direct/staged truth, ownership and invalidation,
materialized closure, stable errors/resource bounds, C ABI prefixes/symbols,
Wasm/fuzz/package/workflow evidence, and all remaining-scope claims. No
Critical, Important, or material Minor finding remains. The user explicitly
required this primary-context review to avoid delegation drift; it is not an
independent/subagent review and no such evidence is claimed.

The fresh Task 10 native builds pass 37/37 Debug tests in 54.38 seconds and
37/37 Release tests in 32.87 seconds with the exact 105-symbol ABI in both
configurations. A halt-on-error ASan+UBSan build passes 37/37 in 223.87
seconds with no retained diagnostic; `detect_leaks=0` is explicit, so this is
not a LeakSanitizer claim. Focused TSan passes the runtime ABI, C++ facade,
backing, checked-view, graph-view, and graph-materialization targets 6/6 in
48.77 seconds without a retained diagnostic. The adjusted local AppleClang
libFuzzer run consumes the exact 16-seed corpus for 1,000 runs in six seconds
without a sanitizer diagnostic or crash artifact; hosted Linux remains the
portable default-fuzzer authority.

The Emscripten build passes the Core graph harness, pure-C link smoke, C++
facade, single-thread injected-failure runtime proof, graph-runtime proof, and
exception helper, while retaining ABI-105. Python passes 413/413 plus
compileall; the package helper passes 13/13 after a fresh `uv build`, producing
only the expected 0.1.22 cp314t arm64 wheel and source distribution inventory.
The paired TypeScript/Wasm build passes 76/76. Strict C11 header compilation
passes arm64, x86_64, and wasm32; the x86_64 native build, pure-C smoke under
Rosetta, and ABI-105 check pass. A production `BUILD_TESTING=OFF` build passes
and contains no test-only heap-reserve-observer symbol.

The final tracked-tree replay after this evidence edit reproduces those test
counts and passes pinned-dependency, embedded-schema, exact 16-seed corpus,
P2/P3 repository-quality, duplicate-key JSON/proof-map, Markdown JSON/link,
placeholder, exact-scope, and `git diff --check` gates. It ran on macOS 26.5.2
arm64 with AppleClang 21.0.0, CMake 4.3.2, CMake's system Python 3.14.5, uv's
Python 3.14.3 environment, Node 25.8.1, and Emscripten 5.0.2. Hosted
Linux/macOS jobs remain definitions rather than passes. No version, push, tag,
release, or publication operation is authorized.

**Reason:** Tasks 1-7 established one Core graph meaning, deterministic
identity and reachability, exact bytes, hardened open, checked lifetime, and
detached closure semantics. Task 8 publishes that completed ordinary runtime
directly instead of creating a second public graph implementation. Task 9
separately hardened robustness and cross-toolchain proof, and Task 10 closes
P3 documentation only after that evidence existed.

**Impact:** C and C++ callers can now author, freeze, execute, open, navigate,
materialize, invalidate, and exchange ordinary `object_graph.v1` payloads with
all V1 values, sharing, and cycles. Other language SDKs still lack this
portable-payload projection; the existing 0.1 call-db surface is not a
substitute and does not own graph meaning. P3 closure does not imply P4/P5,
hosted-CI, package-version, or 0.2.0 release readiness.

**Owner and dependencies:** FastDB owns graph semantics in its C++ Core and
projects them through the stable C ABI. Task 9 consumes Task 8's exact ABI-105
surface and adds robustness, wasm, CI/package proof, and review. Task 10
consumes that hardened Task 9 boundary to close D1/P3 truthfully. P4 is now the
next FastDB owner slice for language projections/codegen; downstream
composition remains outside FastDB and does not own FastDB graph meaning.

**Closure result:** P3 direct/staged, lifetime, 16-seed corpus/fuzz, sanitizer,
full Wasm graph, package, workflow-definition, proof-map, ABI-105, and complete
fresh local-gate requirements pass. D1 and P3 are closed locally with exact
evidence. The accepted independent-review step is explicitly replaced for this
freeze by the user's primary-context review direction and is not misreported as
independent. No ordinary P3 semantic was deferred to shorten implementation.

### P4 language projections and payload code generation

**Current limit:** The P1 compile/query, P2 record runtime, and locally frozen
P3 graph runtime retain their exact historical ABI-105 C boundary and meaning.
P4 Tasks 1-6 project compile/query, complete V1 authoring,
immutable plan facts, backing execution, payload ownership, copied/external
open, complete checked `record.v1` views, graph ref/identity/sharing/cycle
observation, and ordered runtime/package parity into Rust,
`fastdb4py.payload`, and the official TypeScript/Wasm `./payload` subpath.
Task 7 adds deterministic payload-only C++, Rust, Python, and TypeScript source
generation behind one private Core-owned immutable ArtifactSet. Task 8 adds
exactly twelve public exports, projects that ArtifactSet equally through C++,
Rust, Python, and TypeScript/Wasm, executes every generated target, and changes
the manifest operation/target facts atomically. The current public boundary is
therefore exact ABI-117. Task 9 closes hostile-codegen robustness, exact output
ceilings, concurrent determinism, package/workflow/documentation evidence,
fresh local gates, and the same-agent primary review. P4 is locally complete;
existing hand-written 0.1.x call-db layers remain P5 migration inputs, not
portable-payload projections.

**Design state:** The complete live delta audit and docs-first design are
recorded in
[the P4 projection/codegen design](../superpowers/specs/2026-07-21-portable-payload-language-projections-codegen-design.md),
with vertical TDD slices in
[the P4 implementation plan](../superpowers/plans/2026-07-21-portable-payload-language-projections-codegen.md).
They preserve ABI-105 runtime meaning, reuse the existing C++ RAII facade,
create Rust/Python/TypeScript projections only with real callers, and reserve
an exact twelve-symbol additive P4 Core delta: three specification-provenance
guards plus the nine-symbol immutable ArtifactSet family. Task 8 implements
that exact delta; native/Wasm scanners and all four generated-output gates now
make ABI-117 a local implementation fact without changing ABI version 1.

**Reason:** Safe binding lifetimes, value parity, and generated APIs depend on
the frozen P2/P3 runtime ABI and binary meaning.

**Task 1 local evidence:** Rust builds the same payload Core translation units
through `fastdb-sys`, keeps raw pointers private in the safe crate, and passes
2 compile/query integration tests plus its raw ABI/layout test. Python passes
the shared compile/error tests and a 64-query free-threaded retain/close
lifetime regression; the full Python suite passes 416 tests. TypeScript/Wasm
uses `bigint` for every exercised `uint64_t`, explicit disposal plus finalizer
fallback, and passes the two shared payload tests inside the 78-test full
suite. Native Debug passes 37/37, native and Wasm object scanners each prove
the unchanged exact 105-symbol ABI, the deterministic export generator passes
2 tests, the Python package checker passes 14 tests, and the P3 repository
quality gates remain green. The source distribution and wheel build include
the new Python package. These are local results on AppleClang 21, Rust 1.91,
Python 3.14t, Node 25.8.1, and Emscripten 5.0.2; no hosted result is claimed.

The context-owning primary-agent review found and fixed an unaligned Wasm
`uint64_t` output slot and a free-threaded Python close/query lifetime race,
then tightened Rust blob contract failures and incremental export regeneration.
Its post-fix result has zero unresolved Critical, Important, or material Minor
findings. This is explicitly a same-agent review, not independent/subagent
evidence.

**Task 2 local evidence:** Rust passes 6 author/freeze integration tests, the 2
Task 1 compile/query tests, and the raw ABI/layout test, with formatting and
warning-denying clippy clean. Python passes 424 tests plus compileall, including
free-threaded mutation/close and immutable-plan close/query races. The official
TypeScript/Wasm package passes 86 tests. All three languages author the shared
fixed-scalar, nested-list, forward/self/mutual-reference, and disconnected-root
fixtures; assert exact Core plan facts; preserve retry after Core type error
`2004`; and receive the same Core-owned builder-limit error `2013` at
`/objects/Node/1`. Native focused runtime/facade/builder/graph tests pass 4/4;
native and Wasm scanners retain exactly 105 symbols; the package inventory
checker passes 15 tests; and schema, corpus, P3 quality, and Wasm export
generation checks remain green.

Fresh Python 3.14t and Python 3.10 source/wheel builds both pass inventory and
installed author/freeze/handle-integrity smokes. The first Python 3.10
`--no-deps` smoke correctly failed because NumPy was absent from that isolated
target; installing the declared dependency made the same built wheel pass. The
3.10 package build retained its existing SWIG diagnostics, one NumPy generated
conversion warning, and the local macOS deployment-target warning; no
warning-free package claim is made. These are local Darwin arm64 results on
AppleClang 21, Rust 1.91, Python 3.14t/3.10, Node 25.8.1, Emscripten 5.0.2, and
SWIG 4.4.1. Package version remains 0.1.22 and no hosted result is claimed.

The Task 2 primary-agent review added explicit builder-option propagation and
Core-limit coverage, then found and closed runtime-forgeable Python/JavaScript
object, builder, plan, and compiled-spec handles. Module-private creation
tokens close direct construction; ES2022 `#private` runtime brands, direct
prototype-brand checks, and token-gated internal factories also close emitted
JavaScript prototype/reflection forgery. It also closed formatting/clippy
findings. The post-fix result has zero unresolved Critical, Important, or
material Minor findings; it remains a same-agent, non-independent review.

**Task 3 local evidence:** Rust, Python, and official TypeScript/Wasm now
execute immutable plans through the frozen Core, project exact direct/staged
reports and fallback reasons, retain caller backing through payload release,
and open copied or explicitly host/Wasm-owned external images. Rust and Python
safe backings serialize complete executions sharing one context as required by
the stable C callback contract. Callback panic/exception and write/commit
failure paths stop at C, return the closed Core error taxonomy, and roll back
exactly once. The Wasm API names the JavaScript-to-Wasm copy explicitly and
does not claim JavaScript `ArrayBuffer` zero-copy.

Final local counts are 14 Rust non-doc tests with fmt and warning-denying
clippy clean, 433 Python tests plus compileall, and 91 TypeScript/Wasm tests.
Native, ASan+UBSan, and available focused TSan backing/runtime/open sets each
pass 4/4. Native and Wasm scanners remain exactly 105 symbols; the Wasm export
generator passes 2 tests, package inventory passes 16 tests, schema and
16-seed corpus checks remain green, and the P3 quality gate remains 12 runs /
48 assertions plus its repository check. `detect_leaks=0` was used for the
ASan+UBSan run, so no LeakSanitizer result is claimed.

Fresh Python 3.14t and 3.10 source/wheel builds pass exact inventory and
installed compile/freeze/backing/build/copy-open/external-open smokes. Both
installed wheels produce the same 128-byte image and Core digest. The local
3.10 build retains the already-recorded SWIG, generated NumPy, and deployment
target diagnostics; no warning-free package claim is made. These are local
macOS 26.5.2 arm64 results on AppleClang 21, Rust 1.91, Python 3.14t/3.10,
Node 25.8.1, Emscripten 5.0.2, and SWIG 4.4.1. Package version remains 0.1.22,
and no hosted result is claimed.

The context-owning primary-agent review found and closed missing complete-call
serialization for shared Rust/Python backing contexts, incorrect Python
allocation-failure mapping, insufficient Rust callback-panic coverage,
forgeable runtime-handle regressions, and native Wasm owned-byte constructor
exception safety. The post-fix result has zero unresolved Critical, Important,
or material Minor findings. This is explicitly a same-agent review, not
independent/subagent evidence.

**Task 4 local evidence:** Rust, Python, and official TypeScript/Wasm consume
the same checked-in `record-all-types` specification and use only the existing
ABI-105 view/access family. Each projection now covers sequence/list/component
navigation, null versus empty, every non-ref scalar and exact floating/
normalized bits, embedded-NUL `str`, aligned host-unit `wstr`, bytes, immutable
Core materialization, exact kind/type failures, and exact five-field
`VIEW_INVALIDATED` (`4001`) failures. Rust borrows are tied to the unique
`Access`; Python and TypeScript return host-owned copies. No binding parses a
binary, walks a graph recursively, computes layout/digest, or implements a
second materializer.

Final local counts are 16 Rust non-doc tests plus one compile-fail borrow
doctest, with formatting and warning-denying clippy clean; 437 Python tests
plus compileall; and 93 TypeScript/Wasm tests. Native Debug and Release remain
37/37. The focused runtime/facade/record/backing/open/checked-view set passes
7/7 under hard-fail ASan+UBSan and 7/7 under a fresh available ThreadSanitizer
build. `detect_leaks=0` was used for Apple ASan, so this is not LeakSanitizer
evidence. Native, official Wasm, and a fresh Core wasm32 object each remain
exactly 105 symbols. Both Core Node harnesses pass; the Wasm export generator
passes 2 tests; package inventory passes 16 tests; schema generation, pinned
dependencies, the 4-test corpus checker, all 16 retained seeds, and the P3
quality gate at 12 runs / 48 assertions plus repository check remain green.

Fresh Python 3.14t and 3.10 source/wheel builds pass exact inventory, and an
isolated install of each wheel passes all 4 Task 4 record-view tests against
its packaged Core. Their SHA-256 values are:

- 3.14t sdist `26737be66b38ab349bb3b6f849b13633024714d7828d1adcb0febd2788fc1758`;
- 3.14t wheel `3530882aa2daacb9fa2bc39a6386105d9c0069aa0bc2b6b8c163904146c1d938`;
- 3.10 sdist `5b31438c6d89a3b15cee188cccb306ddd6fcdb592fbc44de1c9c51c70993b31c`;
- 3.10 wheel `021527e6228ee18bd93fd18d8f10c2372fa3d0e8b2b18d61459b5a96bc04b2ad`.

The local 3.10 build retains the already-recorded SWIG 325/451, generated
NumPy null-conversion, and deployment-target diagnostics; no warning-free
package claim is made. Package version remains 0.1.22, no hosted result is
claimed, and no Core source/header/schema/manifest/ABI meaning changed.

The context-owning primary-agent review found that Python and TypeScript copied
Core `wstr` bytes safely but did not defensively verify the ABI-promised
`uint16_t` alignment as Rust did. It also reproduced a Python ownership defect:
the default `copy.copy(Access)` produced two Python objects for one unique
native handle and could therefore release it twice. Both projections now
reject non-empty unaligned spans before copying. Retainable Python owners map
both copy protocols to a real Core retain, unique/mutable callback owners reject
copying, immutable external bytes share safely, and every Python/TypeScript
owned-handle adoption path releases the native handle if wrapper construction
fails. The affected focused, full, and package gates are replayed.

The review also records the synchronous drain rule: Rust/Python must not wait
in `invalidate` on an `Access` retained by that same thread, and the official
single-thread Wasm caller must dispose every access before calling
`invalidate`. Native Core, Rust, Python, and ThreadSanitizer prove active-pin
drain; Task 4 does not invent an asynchronous invalidation API or claim Wasm
pthread coverage. This is an explicit host-use constraint, not permission to
weaken Core invalidation. The review is same-agent, not independent/subagent
evidence.

**Task 5 local evidence:** Rust, Python, and official TypeScript/Wasm now
project the existing ABI-105 `view_ref_target` and `view_graph_identity`
operations as thin safe APIs. `GraphIdentity` is exactly the Core pair
`(component_index, object_id)` and is meaningful only within its owning
payload; equal numeric pairs from separate payloads do not assert shared
identity. Field navigation never dereferences a ref implicitly, and no binding
recursively decodes, caches, enumerates, or materializes a graph.

The shared callers author the checked-in all-values graph through Core before
filling the forward target, then observe a Node self-cycle, the Node/Asset
mutual cycle, repeated refs to one shared Node, a null ref, an inline component
without identity, and two disconnected roots. They compare Core coordinates,
not host pointers. One Core materialization call preserves the selected
reachable cycle after source invalidation/release; source identity and target
calls fail with exact five-field `VIEW_INVALIDATED(4001)`, while null target and
identity calls preserve exact `UNEXPECTED_NULL(2003)`. Rust and free-threaded
Python also exercise cloned handles concurrently; TypeScript exercises 1,000
retain/dispose traversals in the official single-thread Wasm host.

Final local counts are 18 Rust non-doc tests plus the Task 4 compile-fail
borrow doctest, with formatting and warning-denying clippy clean; 439 Python
tests plus compileall; and 95 TypeScript/Wasm tests. Native Debug and Release
pass 37/37 in 70.46 and 46.56 seconds. The 12-test runtime/record/graph/
backing/open/view/materialization set passes hard-fail ASan+UBSan in 202.61
seconds and a fully built available ThreadSanitizer configuration in 358.95
seconds. The first expanded TSan invocation found that the retained Task 4
build had registered but not built three graph executables; that incomplete
run was stopped and is not evidence. All 12 exact targets were then built and
the clean 12/12 run was started from the beginning. Apple ASan still uses
`detect_leaks=0`, so no LeakSanitizer result is claimed.

The Core graph Wasm harness and single-thread injected-failure path pass.
Native, official-Wasm-build, and fresh Core wasm32 symbol scans remain exactly
105. Export generation passes 2 tests; package inventory passes 16; schema,
pinned dependencies, the 4-test corpus helper, all 16 reviewed seeds, and the
P3 quality gate at 12 runs / 48 assertions plus repository check remain green.
The unchanged official artifacts remain
`d763262fb5bc87196fcbc56781561e128595af55ae85546fff67ca6f81192e22`
for JavaScript and
`511b3cbdd5cd1e09c497ca85cc5ef2981d3ec85753cc18b257575d2ac1b10fae`
for Wasm.

Fresh Python 3.14t and 3.10 source/wheel builds pass exact inventory. An
isolated install of each wheel passes all six Task 4-5 record/graph tests
against its packaged Core. SHA-256 values are:

- 3.14t sdist `315610e70525518fe9aa5b6562a714c69d2e194dd53276331d491e796e8a708e`;
- 3.14t wheel `43174c8a1f8da628b474732c5a507d7471bb159b6941aa6722187e8d87bf315e`;
- 3.10 sdist `28025839fef9596a7329a1697670249d4f3b6ba32350390ff2a92c5ad1fe0229`;
- 3.10 wheel `97bcdba24e3e592ea526e8f86a3798439dd7c232d6bd3a61fd8c0db4f8fbdd08`.

The 3.10 build retains the diagnostics already governed by Issue 0003,
including SWIG 325/451 and local deployment-target warnings. Package version
remains 0.1.22, no Core/header/schema/manifest/ABI meaning changed, and no
hosted result is claimed. The primary-agent review is same-agent rather than
independent/subagent evidence.

#### P4 Task 6 local evidence

Task 6 adds one fail-closed executable map with exactly six ordered runtime
receipts: canonical, binary, logical value, error, lifetime, and direct/staged.
Every receipt names non-empty C++, Rust, Python, and TypeScript/Wasm callers in
that order and pins their existing Core-owned fixtures or observations. The
checker rejects missing, duplicated, reordered, or nonexistent proof tests and
markers. It also freezes ABI version 1 and the exact 105-symbol allowlist. The
four codegen rows are exactly `cpp`, `rust`, `python`, and `typescript`, remain
`open`, and contain no fabricated proof.

The record and graph projection tests now independently compare the binary
bytes returned by Core with the checked-in `fixed-scalars` and
`graph-all-values` hex goldens. No binding computes an expected layout or
binary digest. Existing callers supply the rest of the receipt: canonical
JSON/manifest/SHA-256/index facts; every V1 logical value; exact five-field
compile/builder/type/backing/invalidation errors; clone/retain/release,
copy/external ownership, checked invalidation, detached materialization, and
failure cleanup; and exact internal/direct/staged/direct-required reports.
The existing fixtures were sufficient, so Task 6 creates no duplicate parity
golden or binding-side expectation generator.

Rust keeps repository `source` linking as its default and adds an explicit,
fail-closed `FASTDB_PAYLOAD_LINK_MODE=system` boundary. System mode requires an
absolute `FASTDB_PAYLOAD_SYSTEM_LIB_DIR` containing the platform shared
`libfastdb`; it never reaches into a source tree. A relocated consumer copied
outside the repository links that library, calls the safe API, and validates
the real Core ABI version. Cargo source inventories include both crate READMEs
and contain no Core copy, test tree, or build debris. The embedding process
still owns its ordinary dynamic-loader search path; Task 6 does not hide or
duplicate a native library inside the Rust crate.

The Python 3.10 installed wheel runs all 27 portable-payload tests locally.
The npm tarball contains the `./payload` export, compiled projection modules,
Wasm loader, and reviewed Wasm artifact; an extracted clean package resolves
`fastdb4ts/payload`, initializes Wasm, compiles and queries a real Core spec,
and disposes the handle. The package check rejects missing payload members,
debris, links, an altered subpath, or a non-ES-module package. These package
proofs add no native Node projection and do not broaden the browser/worker/Wasm
claim.

The quality checker has 13 unit tests, the Rust package checker has 3, and the
TypeScript package checker has 5. Focused Rust binary parity, Python binary
parity, TypeScript binary parity, the relocated Rust system consumer, and the
packed TypeScript/Wasm smoke pass locally. The Rust and projection-parity
GitHub Actions jobs plus Python 3.10 installed-wheel and npm packed-package
steps are workflow definitions. Hosted Task 6 execution remains pending; no
definition is rewritten as a hosted result. Final full-gate counts and the
frozen commit are recorded in the retained Task 6 report after the scoped
commit and exact-range review.

Task 6 changes no C++ Core source, public header/facade, schema, manifest,
binary meaning, ABI symbol, package version, P5 legacy surface, C-Two, or
Toodle file. The context-owning primary agent performs the required spec and
code-quality review; it is same-agent evidence, not independent/subagent
review.

#### P4 Task 7 local evidence

Task 7 adds one Core-private `Target`, immutable `ArtifactSet`, validated
artifact factory, byte-total identifier projection, and four renderers under
`src/payload/codegen/`. One call accepts one compiled specification and one
target and returns exactly one in-memory payload-only source artifact. The
generator consumes the existing `CompiledSpec`, `ResolvedSpec`, canonical
bytes/digest, stable indexes, and Core-derived `RuntimeTopology`; it does not
reparse JSON, read a manifest into a second model, write a destination tree, or
reimplement schema, canonicalization, digest, layout, binary, graph, backing,
or materialization semantics.

Artifact paths are digest-derived, relative, UTF-8, slash-normalized, unique,
and unsigned-byte sorted. Kinds, exact byte totals, literal output limits, and
SHA-256 receipts are checked before the immutable result is published.
Unknown targets, invalid paths/kinds, duplicate paths, literal count/byte
limits, allocation failure, renderer exception, and topology failure have
stable Core codegen errors and publish no partial set. Allocation injection
also covers error construction for an invalid target, so `std::bad_alloc`
does not escape merely because target validation failed before rendering.

Each artifact embeds exact environment-free provenance, the Core canonical
source/digest, original-ID metadata, stable indexes, entry wrappers, checked
component wrappers, official-runtime scalar conveniences, generic recursive
view escape, and builder selection helpers. Ref-target helpers exist only for
actual refs. Graph identity and object declaration exist only where the
Core-derived topology assigns identity. A schema-specific component wrapper
cannot be constructed around an arbitrary valid `View`: C++ and TypeScript
use private construction, Rust exposes a checked `Result<Option<_>>`, and
Python uses a module-private creation token; all four compare the Core-reported
component index before publication.

The exact empty-record C++/Rust/Python/TypeScript artifacts and a receipt that
binds target, path, kind, fixture, bytes, SHA-256, and order are checked in.
Rich tests cover record-all-types, recursive lists, keyword/escape-shaped IDs,
and shared/cyclic graphs; repeat generation under distinct HOME/TMP/locale/
timezone values is byte-identical and environment strings are absent. The
identifier test covers all 256 byte values even though V1 source IDs remain
the narrower accepted ASCII grammar. The allocation sweep reaches success
after induced failures and every observed failure returns the exact
`allocation_failed` result without a partial ArtifactSet.

The context-owning primary review found and closed twelve material issues before
freeze: record output incorrectly exposed graph helpers; component wrappers
could be forged around the wrong schema component; limit details represented
exact integers as JSON numbers; generated C++ named the non-existent
`fastdb::payload` facade instead of `fastdb::payload::v1`; invalid-target error
allocation could escape the generator boundary; the four renderers duplicated
the generator and Core ABI provenance instead of consuming one Core constant;
empty or topology-reduced TypeScript output imported runtime types it did not
use and therefore failed strict `noUnusedLocals`; and drive-prefix recognition
depended on the process C locale rather than explicit ASCII semantics. The
ninth finding was a cross-language lifetime mismatch: successful Python/
TypeScript component construction retained the caller's same View object, so
closing that alias invalidated the generated wrapper, while their entry
constructors allowed the same ambiguous ownership. Focused REDs reproduce each
behavior before the corrections. Successful component casts now clone the
official runtime handle, and entry construction requires a module-owned token
so factories transfer the sole new entry View into the wrapper. The post-fix
C++ rich artifact passes Clang C++17 syntax compilation with warnings denied,
Rust passes a relocated Cargo check against the safe crate, Python passes syntax
plus real import/Core compile, and both empty and rich TypeScript artifacts
pass strict type-checking with unused locals denied against the official
payload source. These build-tree diagnostics validate Task 7 output shape but
do not replace or close Task 8's tracked generated-output runtime harnesses.

The tenth finding was the remaining C++/Rust entry-construction escape: both
targets publicly accepted an arbitrary View even though only the generated
payload factory can establish the entry identity. C++ now keeps its View
constructor private and exposes only `from_payload`; Rust keeps `new` module
private. Focused output checks and real C++/Rust compilation prove those
surfaces after the correction.

The eleventh finding was a latent target-routing mismatch: the private Core
target enum used ordinal `1/2/3/4` values while the designed Task 8 public
target flags are `1/2/4/8`. The private values now use those exact bits, and a
focused regression fixes all four mappings before Task 8 introduces the C ABI.

The twelfth finding was mutable TypeScript provenance: the generated module
exported one shared `Uint8Array`, so a caller could modify the bytes later used
by `compileSpec()`. TypeScript now keeps the canonical text as the immutable
source and returns a fresh encoding from `canonicalSource()` for every caller
and compile. The exact golden and strict compiler matrix cover the corrected
surface.

Task 7 also makes one private-boundary limitation explicit: its component cast
can prove only the Core-reported component index, not that the View came from
the generated `CompiledSpec`; entry and builder helpers likewise assume a
matching specification. Indexes are spec-scoped, so a different schema can
legitimately reuse index zero. ABI-105 has no Builder/View provenance guard,
and binding-owned digest comparison would create a second error authority.
Because no Task 7 artifact is public or advertised, Task 8 owns the clean
closure: add the three Core `require_spec_sha256` guards, project them equally,
and prove that same-index cross-spec Builder/Payload/View inputs fail with
identical Core `DIGEST_MISMATCH` facts before any generated wrapper publication
or builder mutation. Until that proof passes, manifest targets and codegen
proof rows remain empty.

Task 7 has a second private-boundary limit: the known one-artifact inventory is
rendered before `max_artifacts` is checked, and `max_total_bytes` rejects an
oversized completed draft before ArtifactSet publication only after that draft
has been constructed in memory. These are publication limits, not yet early
work/allocation ceilings. Task 8 must reject an impossible artifact count
before rendering and enforce the byte limit through a checked output sink or
equivalent exact preflight before the public options ABI and manifest targets
are exposed.

The final local rerun passes native Debug `38/38` in 59.62 seconds, Release
`38/38` in 36.71 seconds, and hard-fail ASan+UBSan `38/38` in 262.29 seconds;
the available focused ThreadSanitizer codegen target passes `1/1` in 1.23
seconds. Apple ASan uses `detect_leaks=0`, so this is not LeakSanitizer
evidence. Rust passes formatting, warning-denying clippy, 19 non-doc tests,
and one compile-fail doctest; Python passes 440 tests plus compileall; the
fresh official TypeScript/Wasm rebuild passes 96 tests. Native and fresh
official Wasm scanners each remain exactly ABI-105. The embedded-schema check,
16-seed binary corpus, vendored dependency transaction suite, retained P2/P3
quality gates, 13-test P4 quality checker, 16-test Python package checker,
Rust source/system relocation check, and five-test packed TypeScript package
checker are green.

A fresh Python 3.14t sdist/wheel build contains every Task 7 Core source,
passes exact inventory, and its isolated installed wheel passes all 27 payload
tests. The build emits the same seven SWIG 325/451 diagnostics governed by
Issue 0003; no warning-free package claim is made. The final sdist and wheel
SHA-256 values are respectively
`3a3a692ef657aadea3d4302c57f6770c1696000343f60fe855e11039088168c6`
and
`dfab93e2302620b5ae83c5e2e74946fad4f31af73d79e0f4fe5907c851309cbf`.
The packed TypeScript artifact and official JavaScript/Wasm hashes remain
`fdf3f6fe2d7a663b2db8b77af92c2528f972321c24784f701afa5fc05af471b4`,
`d763262fb5bc87196fcbc56781561e128595af55ae85546fff67ca6f81192e22`,
and `511b3cbdd5cd1e09c497ca85cc5ef2981d3ec85753cc18b257575d2ac1b10fae`.

Task 7 changes no public C header/facade, ABI allowlist, schema, manifest,
projection API, proof-map codegen status, binary meaning, package version, P5
legacy surface, C-Two, or Toodle file. The public native and official Wasm
boundaries remain ABI-105 and advertise an empty codegen target list. Hosted
execution remains pending, and the required review is same-agent primary
review rather than independent/subagent evidence.

#### P4 Task 8 local evidence

Task 8 publishes the complete generator atomically. The stable C ABI adds
exactly the three reviewed Builder/Payload/View `require_spec_sha256` guards
and nine immutable ArtifactSet/codegen functions, with no other export. The
native and official Wasm scanners both observe exactly 117 sorted
`fdb_payload_v1_*` symbols while ABI version remains 1. The historical P2/P3
sub-boundaries remain 99/105 and retain their prior meaning.

Pure-C tests cover options-prefix validation, target routing, output clearing,
retain/release, artifact count/path/kind/bytes/SHA-256 queries, invalid indexes,
literal zero limits, and failure without a partial result. The thin C++ RAII,
Rust, Python, and TypeScript/Wasm projections expose the same targets,
independently owned artifact fields, limits, and Core errors. None parses a
specification, renders a template, computes FastDB identity, or interprets
payload layout.

The context-owning Task 8 review found and closed six publication-quality
defects. Generated Python scalar/ref helpers and TypeScript scalar/ref helpers
originally left temporary Views to finalization; focused injected-success and
exception REDs now require context-manager or `try/finally` release. The new C
functions originally cleared result/query outputs inside the ordinary error
guard, so a null required `out_error` returned before clearing them; pure-C
REDs now cover every result/blob/count/kind/digest output and clearing occurs
before that guard without changing older ABI families. Python also treated
falsey non-`CodegenOptions` values as defaults, and Python/TypeScript accepted
post-construction negative or out-of-range option mutations through unsigned
FFI conversion; both projections now reject the wrong object type and
revalidate every dynamic option value at the call boundary. Finally, the
generated-output harness itself chained owned Python/TypeScript parent Views
and therefore relied on finalizers even though generated helpers did not; its
parents are now explicitly closed on success and failure. Each correction had
a failing regression before GREEN.

The complete-stack rerun then exposed two test-integration defects. The new
Python payload codegen test originally shared the legacy
`tests/python/test_codegen.py` module name, so default pytest collection
rejected the full suite; it now has a unique module name and the proof map and
focused command name that exact file. The ordinary Core Wasm C++ facade receipt
also tried to create native `std::thread` workers even though that build is
deliberately single-threaded. The shared ArtifactSet stress now runs in native
and pthread-enabled builds, while ordinary Wasm still executes all RAII,
query, error, and limit assertions. Native ThreadSanitizer remains the
concurrency authority. Both defects were retained as RED before their focused
and broad GREEN reruns.

The three provenance guards compare the Core-owned specification digest
already attached to each handle. Direct C, C++ RAII, Rust, Python, and
TypeScript tests use two specifications whose valid entries/components share
index zero and require the same exact `DIGEST_MISMATCH` code, symbol, path,
message, and details for Builder, Payload, backed View, and detached View.
Generated entry factories, builder helpers, and component constructors invoke
those Core guards before selecting an index, publishing a wrapper, or mutating
a builder.

One clean temporary-tree harness asks the public Python projection to invoke
Core for all four artifacts, validates each relative path/kind/bytes/SHA-256,
and then uses only official runtimes. It compiles, links, and executes generated
C++17; builds and runs a relocated Rust binary against the safe `fastdb` crate
and system Core; imports and executes generated Python; and strictly
type-checks then executes generated TypeScript against `fastdb4ts/payload` and
the official Wasm Core. Every generated target proves both the cross-spec
rejection above and a successful value roundtrip. The generated files exist
only in the temporary test tree; FastDB still does not write a consumer's
destination tree.

Only after those four executions passed did the manifest append `codegen`, set
all four target bits, and expose exactly
`["cpp","rust","python","typescript"]`. The manifest schema, embedded schema
bytes, nine manifest goldens, C/C++/Rust/Python/TypeScript capability tests,
ABI allowlist, Wasm exports, deterministic codegen goldens, and executable P4
proof map changed with that same switch. The proof map now requires, for every
target, a Core determinism/hash receipt, a public-language projection receipt,
and the invoked generated-output compiler/import/runtime function.

The complete post-review rerun passes native Debug `39/39` in 58.36 seconds,
Release `39/39` in 36.93 seconds, and hard-fail ASan+UBSan `39/39` in 245.92
seconds. Apple ASan uses `detect_leaks=0`, so this is not LeakSanitizer
evidence. The available no-competing-load ThreadSanitizer set passes `13/13`
in 444.73 seconds and includes codegen plus the runtime, builder, binary,
backing, open, view, and materialization paths.

Rust passes format, warning-denying clippy, 21 non-documentation tests, and one
compile-fail doctest. Python passes `447/447` plus compileall; installed current
and Python 3.10 wheels each pass the complete 34-test payload suite.
TypeScript/Wasm passes `98/98`, and its clean packed-package smoke passes.
Native, official TypeScript/Wasm, and independently built Core Wasm scanners
all observe exact ABI-117. The six Core Wasm/Node runtime, pure-C, C++ facade,
and injected-failure receipts pass; strict C11 arm64, x86-64, and wasm32
compilation passes; and the Emscripten exception-option contract is verified.
The generated-output harness, package inventories, schema regeneration,
16-seed corpus, vendored dependency transaction, P4 quality checker, and
retained P2/P3 quality gates all pass locally.

This is same-agent primary evidence, not independent/subagent review, and no
hosted result is claimed. At the Task 8 boundary, Task 9 still owned final
hostile-codegen, workflow/documentation, and full-range closure.

**Recorded platform limit:** Windows generated-C++ linking is not implemented
or locally tested by the Task 8 harness. A reliable Windows receipt needs an
explicit compiler/import-library/runtime-DLL recipe rather than translating the
current Unix `-L/-l/-rpath` command heuristically. This does not change Core
codegen or the generated header, and Linux/macOS remain covered by the generic
C++17 path; the missing Windows harness recipe must be added and proven before
FastDB claims Windows generated-C++ package support. It is not being hidden as
a later compatibility alias or binding-owned workaround.

Task 8 changes no package version, legacy 0.1.x authority surface, P5 clean-cut
decision, C-Two file, Toodle file, tag, publication, or hosted status.

#### P4 Task 9 local closure evidence

Task 9 starts from exact Task 8 commit
`38d40188d041b9dd0b8ffba04900626c98fb1a25`
(`feat(core): publish portable payload artifact codegen`) and records its
closure under the scoped commit subject
`docs: record portable payload P4 closure`. Task 7's frozen predecessor is
`dbcca51347f9bc16cc80d57d9a7acc7bcb961c2d`. The versioned
`fastdb.payload.p4-projection-codegen-map.v2` proof map is committed with the
closure and freezes exact ABI-117, all four `closed-p4` targets, eight ordered
closure requirements, same-agent review, hosted-pending status, and
`open-clean-cut` P5 handoff. The exact Task 9 Git object ID is recorded after
commit in the local Task 9 report because a commit cannot contain its own
hash.

The genuine Task 9 REDs first proved that the prior map had no closure object
and the generated-output runner had no hostile matrix. The new four-shape hostile
matrix invokes
Core through the public Python projection, then compiles, imports, type-checks,
and executes generated artifacts for four ordered accepted shapes:

1. all declared V1 values;
2. recursive lists;
3. keyword plus generated-prefix identifier collisions; and
4. a shared cyclic object graph.

Each shape runs through generated C++17, relocated safe Rust plus system Core,
Python, and the official TypeScript/Wasm projection. The matrix uses only
official runtimes; it neither reparses FastDB semantics nor writes a consumer
tree.

The exact all-target limit RED exposed one Core owner-layer defect. Rust,
Python, and TypeScript renderers constructed a second terminal newline, checked
that temporary byte against `max_total_bytes`, and removed it afterward.
Consequently, a ceiling equal to the published artifact size failed by one
byte even though the returned artifact would fit. `CheckedOutput` now defers at
most one trailing newline and discards it only when reproducing the prior
double-newline trim. Published bytes and ABI-117 are unchanged, while exact
final-size ceilings pass and one-byte-short limits fail with the exact
Core-owned `actual`, `limit`, and reason. Eight native threads repeatedly
compare path, bytes, and SHA-256 for every target; ordinary single-thread
Emscripten keeps the non-concurrent assertions, and native ThreadSanitizer is
the concurrency authority.

The frozen primary review then found that an empty `CheckedOutput` append
still flushed the pending newline. At an exact ceiling, a true no-op could
therefore report a false overflow and could alter final blank-line trimming.
The retained RED fails on that empty append before the fix. `append()` now
returns immediately for empty input, and the same exact-limit case passes
under Debug, Release, ASan+UBSan, and ThreadSanitizer. All Core/Wasm and
package artifacts were rebuilt from that corrected source before the final
hashes below were recorded.

The first combined TypeScript hostile run also exposed a harness defect:
generated `compileSpec()` calls ran before the official Wasm projection was
initialized. The smoke now awaits `initPayload()` before executing any
generated function. This is a test-harness repair, not a second runtime path.
A stale ignored `python/fastdb4py/core/libfastdb.dylib` with only ABI-105 also
caused the first local source-suite invocation to fail uniformly on the new
codegen symbol. Binding that suite to the freshly scanned ABI-117 Core passes
all tests; both final wheels independently rebuild and package their own
ABI-117 Core, so no stale binary is part of release evidence.

The complete fresh local gate on AppleClang 21.0.0, Emscripten 5.0.2,
Node 25.8.1, Rust 1.91.0, uv 0.10.12, free-threaded Python 3.14.3, and
Python 3.10.17 is:

```text
Native Debug: 39/39 in 97.37s
Native Release: 39/39 in 74.08s
ASan+UBSan hard-fail: 39/39 in 301.04s
Focused ThreadSanitizer runtime/codegen set: 13/13 in 499.98s
Native ABI: exactly 117
Official TypeScript/Wasm ABI: exactly 117
Independent Core Wasm ABI: exactly 117
Rust: fmt and warning-denying clippy pass; 21 non-doc tests + 1 doctest
Python source: 447/447 plus compileall
Installed free-threaded 3.14 and Python 3.10 wheels: 34/34 each
TypeScript/Wasm: 98/98
TypeScript package checker: 5/5 plus packed-package Wasm smoke
Generated harness checker: 6/6 plus simple and four-shape runtime matrix
P4 closure checker: 17/17 plus repository check
P3 retained quality: 12 runs / 48 assertions plus repository check
Binary corpus: 4/4 plus exactly 16 reviewed seeds
Strict C11 header objects: arm64, x86_64, and wasm32 pass
```

The six independent Core Wasm artifacts pass their Node receipts, with the
runtime-ABI artifact executing both injected-failure and graph-runtime modes.
The Emscripten exception contract, generated export inventory, embedded
schemas, vendored dependencies, Rust relocated system consumer, Python package
inventories, and P2 retained quality checker also pass.

The Task 9 package artifact SHA-256 values are:

```text
free-threaded 3.14 wheel
  4ce9d9e442a564c286e56dec51239b8842208bd67ee84bf22f3e937e6c66b5ba
free-threaded 3.14 sdist
  11aa69ee143635a8d211aa4abd3cc0de2c5615da9dbf511f921d00fe894f51f9
Python 3.10 wheel
  b9fa3ebdf373206d860d1b9bea2ccfe6a91e7fe3bbd985ba10b41ef07a2897bb
Python 3.10 sdist
  2e8a0b2b0513f22c5e6637cb45c640cdec68b51050769ecdce9669ab0e00dd35
fastdb4ts-0.0.3.tgz
  f1f7564954c096975956008596b3ee1e2e38668e3936b5f2d13cda88d03d6f6d
official fastdb4ts.js
  ed28d53b23c336abc5dd4c94bc4472a7c323c9fa8029d049758bdafc33a7dc54
official fastdb4ts.wasm
  d2dbbbc4588d618ec4e3f5da5ab4063da28e0249f0c3cfaef272b325b2c5aba7
```

The context-owning agent performed the Task 9 specification/authority and code
quality review personally, as required. The recorded classification is
**same-agent primary review**, not independent or subagent evidence. P4 is
locally complete at exact ABI-117 with no package version change. Hosted P4 execution remains pending;
committed workflow definitions are not hosted results.
P5 clean cut remains open.

**Impact:** Portable compile/query/build/open/view/materialize remains available
to C and C++ through the locally frozen P3 boundary. Rust, Python, and official
TypeScript/Wasm now share compile/query, complete author/freeze/plan semantics,
truthful execution/backing, payload ownership, open, complete checked record
navigation/access, graph ref/identity/sharing/cycle observation, detached
materialization, and invalidation from that same Core. Per-language tests are
now indexed by the ordered executable four-language parity map, and the
source/system/wheel/npm boundaries are locally executable. All four languages
can now consume the same Core-owned generated artifact set without gaining
binding-owned semantics.

**Next owner slice:** P5 removes legacy authority, performs the clean
`RecordEngine` rename, and establishes local release readiness without
reopening the now-proven ABI-117 or creating binding-side semantics.

**Closure criteria:** C++/Rust/Python/TypeScript-WASM obtain all semantics from
the same Core ABI and pass canonical, binary, value, error, lifetime, and
direct/staged parity; Core-owned four-target in-memory artifact generation is
deterministic and generated outputs compile or import without downstream
semantics.

### P5 clean cut and local release-readiness handoff

**Current limit:** The duplicate Python/TypeScript authority, orphaned
native/SWIG backing surfaces, old engine name, stale current instructions, and
unclassified current/historical literals are removed or governed through P5
Task 6. Task 7 completes the fresh local source/package/platform-applicable
readiness matrix. Package metadata remains `fastdb4py==0.1.22` and
`fastdb4ts==0.0.3`; hosted Linux/Windows evidence, a 0.2.0 version change,
push, tag, publication, and release do not exist. A downstream C-Two local
composition proof now exists at exact implementation commits and hashes, but
it is not an official package release or hosted result.

**Reason:** P1-P4 now provide the complete replacement and parity evidence, so
the P5 clean cut can proceed without a compatibility parser or binding-owned
fallback. Downstream composition still belongs in C-Two only after FastDB
freezes its owner boundary.

**Impact:** The FastDB owner boundary is locally release-ready and has been
consumed by the separate C-Two owner slice without moving payload meaning into
C-Two. That local result is not a hosted platform matrix, a versioned package
release, or a published 0.2.0 artifact. C-Two still cannot fill a missing
generic FastDB slice in its own repository.

**P5 Task 0 handoff:** The design and executable plan are frozen from exact
start `9d86c171eda1fe107c3519ce040ca2ec417167f9`. The portable ABI remains
exactly 117 sorted symbols.

**P5 Task 1 local implementation evidence:** From exact start
`c94aa85fa39566e8e48f5a6c711fe099a47cd848`, the old
`fastdb4py.codegen` package, Python feature-module discovery/rendering, and
`fdb codegen --ts INPUT_DIR OUTPUT_DIR` contract are removed. The replacement
command is exactly
`fdb codegen SPEC.json --target cpp|rust|python|typescript --output DIR`.
It reads the source as bytes, compiles and generates once through
`fastdb4py.payload`, copies the complete immutable Core ArtifactSet before
filesystem mutation, rejects unsafe/duplicate/non-source paths, and publishes
exact artifact bytes into a new tree.

The genuine RED was 23 replacement failures against the old CLI with three
unrelated boundary tests passing. The corrected focused gate passes 46/46;
the complete Python suite passes 396/396; compileall succeeds; and a fresh
0.1.22 sdist/wheel pair builds. Neither distribution contains a
`fastdb4py/codegen` package. A fresh native build retains exactly 117 sorted
payload ABI symbols. The package build still reports the seven governed
legacy SWIG diagnostics; Issue 0003 and P5 Task 4 own their removal, so this
Task 1 evidence does not claim warning-free packaging.

The first mechanically frozen same-agent specification pass found one
Important cross-platform containment defect: `C:/...` was a relative
`PurePosixPath` but an absolute Windows drive path when joined to staging. It
also found the adjacent NUL/file-directory conflict validation gap. The code
quality pass found one material Minor filesystem-alias defect: `"wb"` could
silently collapse two byte-distinct artifact paths on a case-folding or
normalizing filesystem. Retained REDs reproduce both classes. The correction
rejects Windows drives, NUL, and file/directory prefix conflicts before
filesystem mutation and creates every staged file with exclusive `"xb"`
semantics. The continued security pass found and closed a second Important
Windows fidelity defect: device names, trailing-dot/space aliases, forbidden
characters, and NTFS alternate-stream paths could succeed without creating
the requested ordinary artifact. Runtime validation uses the supported
Windows reserved-path predicate while retaining Core-valid colon paths on
POSIX. The initial Python 3.10-3.12 fallback delegated to
`PureWindowsPath.is_reserved()` and was later proven incomplete by the P5 Task
7 minimum-version installed-wheel gate; Task 7 records the retained regression
and corrected fallback rather than preserving the earlier overclaim.

The final mechanically frozen same-agent re-review covers exact range
`c94aa85fa39566e8e48f5a6c711fe099a47cd848..d6d284c`. Its separate
specification/authority and five-axis code-quality passes report 0 Critical,
0 Important, and 0 unresolved material Minor findings after the corrections.
This is primary-agent review, not independent or subagent evidence. The
exclusive macOS path and platform-independent Windows-name policy are locally
executed; Linux `renameat2` and Windows `rename` runtime branches remain
pending their platform package gates and are not represented as local passes.

**Task 1 intentional destination-tree limit:** The FastDB CLI accepts only a
nonexistent output root. It never merges, updates, or overwrites a project
tree. It stages privately and uses an exclusive final rename:
`renamex_np(RENAME_EXCL)` on macOS,
`renameat2(RENAME_NOREPLACE)` on Linux, and non-replacing `rename` on Windows.
An unsupported platform or Linux libc without the exclusive primitive fails
closed with `ENOTSUP`; it does not fall back to POSIX replacement semantics.
Every staged file is also created exclusively, so case folding, Unicode
normalization, or another filesystem alias fails and cleans the private tree
instead of collapsing two Core artifact paths.
Windows device, reserved-character, trailing-dot/space, and alternate-stream
paths are rejected before staging; POSIX retains Core-valid names that do not
have those filesystem meanings.

**Reason:** Project conflict policy and multi-owner artifact composition are
not FastDB semantics. Standard POSIX replacement can also overwrite an empty
directory created after an existence check, so a check-then-`replace` fallback
would violate caller ownership under a race.

**Impact:** A caller must choose a new destination and explicitly dispose of a
previous caller-owned tree before regeneration. C-Two must compose its own and
FastDB artifacts at the downstream boundary instead of asking this CLI to
merge them. A currently unsupported operating system cannot use the
filesystem facade until it supplies an exclusive directory-rename proof, but
can still consume the in-memory Core ArtifactSet through a supported
projection.

**Dependencies and closure criteria:** FastDB does not plan a merge mode.
Linux, macOS, and Windows package gates must retain a race regression proving
an existing empty directory is not replaced. Support for another platform
requires its own no-replace primitive plus the same regression; it must not
weaken to check-then-replace. C-Two's later composition task closes the
downstream multi-artifact concern without moving conflict rules into FastDB.
These owner boundaries are intentional and do not close the remaining P5
tasks.

**P5 Task 2 local implementation evidence:** From exact start
`4bb68dcc1b215a8a2c7ee7da3998f386d2e4a9e`, the Python-owned
`call_db.py`, `schema.py`, `require.py`, and `allocator.py` modules and their
authority-only tests are deleted without aliases or deprecation shims.
`BatchRequirement`, `ArrayRequirement`, `batch`, `array`, `Array`, and
`Batch` are also removed from `type.py`. The retained package-root `__all__`
is the exact standalone decorator, registry, layout, engine, table, string,
view-owner, materialization, serializer, and native-type-alias set. A root
`import fastdb4py` neither imports `fastdb4py.payload` nor creates a second
portable facade; consumers import the frozen projection explicitly.

The package inventory gate now rejects the exact six historical module paths
in both sdist and wheel layouts, including the already removed
`fastdb4py.codegen` files. This is an explicit forbidden-member assertion, not
an inference from a recursive build. The genuine RED produced seven failures:
the old top-level surface and four authority modules remained importable, the
requirement value classes remained in `type.py`, and the package checker had
no forbidden inventory. The corrected replacement gate passes 25 tests plus
seven subtests. After migrating retained standalone cases, the focused gate
passes 126/126 and the complete Python source suite passes 341/341.

The first complete-suite run exposed two native allocation tests that still
used aliases from the deleted Python allocator facade. Their behavior remains
valid and now names the existing `fastdb4py.core.Wx*` types directly. No new
allocator facade or Python-owned allocation semantics were introduced.
Nine retained `LayerSchema`/registry cases and three generic `Layout.name`
cases were also migrated out of the deleted authority-named test files, so
the clean cut does not discard standalone storage coverage.
Compileall passes, the package-checker unit suite passes 18 tests, and a fresh
`fastdb4py==0.1.22` sdist/wheel pair passes the real inventory gate. The build
continues to emit exactly the seven governed SWIG diagnostics; Issue 0003 and
P5 Task 4 still own their removal.

**Task 2 ordered intermediate limit:** The standalone AoS implementation is
still exported as `ColumnEngine`, and current README examples still describe
the now-removed Python call-db surface.

**Reason:** P5 Task 5 performs the class/module/export rename atomically, while
P5 Task 6 rewrites all public instructions only after Python, TypeScript, and
native clean cuts have settled. Mixing either concern into Task 2 would make
its package-authority proof ambiguous.

**Impact:** The source and built Python package have a clean authority boundary,
but this branch is not release-ready: users following the current README can
encounter removed names, and the old engine name can still be mistaken for a
true columnar-storage engine.

**Dependencies and closure criteria:** P5 Task 3 must remove the TypeScript
call-db surface, Task 4 must close native/SWIG debris, Task 5 must replace
`ColumnEngine` with `RecordEngine` without an alias, and Task 6 must make
standalone boundaries and documentation policy executable. Until the fresh
Task 7 readiness run passes, package versions remain unchanged and no
publication is authorized.

**Task 2 frozen review:** The implementation commit is `97c4d39`
(`refactor(python): remove duplicate payload authority`). The context-owning
primary agent reviewed exact range
`4bb68dcc1b215a8a2c7ee7da3998f386d2e4a9e..97c4d39` in separate
specification/authority and code-quality passes. The final classification is
0 Critical, 0 Important, and 0 unresolved material Minor findings. This is a
same-agent review, not independent or subagent evidence.

The local installed-package smoke is CPython 3.14t on macOS arm64. Python 3.10
compileall and checker startup pass, while the fresh installed Python 3.10,
Linux, and Windows package matrices remain explicitly pending P5 Task 7 and
hosted execution. No cross-platform or hosted result is inferred from the
local archive inventory.

**P5 Task 3 local implementation evidence:** From exact start `0ff6640`, the
TypeScript `call-db.ts` runtime and its authority test are deleted. The root
entry removes all value and type re-exports for
`encodeFastdbCallDb`, `decodeFastdbCallDb`, `viewFastdbCallDb`,
`encodeFastdbFeature`, `decodeFastdbFeature`, and the associated call-db
types. No call-db logic moved into the retained feature, schema, ORM, table,
serializer, or database-buffer modules. `Feature`, `ORM`, and
`FastSerializer` remain root exports, while `fastdb4ts/payload` remains the
official Wasm projection.

The genuine RED built the old source successfully, then failed the inverted
root export test and package-checker test because the five values and two
generated members still existed. The corrected focused gate passes two root
export tests and seven package-checker unit tests. One standalone bulk table
fill case and four Wasm-owned database-buffer lifetime cases were migrated
out of the deleted authority test; the complete TypeScript/Wasm suite
therefore passes 57/57 without discarding retained storage coverage.

A fresh `fastdb4ts==0.0.3` tarball contains 41 files, retains every required
`dist/payload/*` JavaScript/declaration artifact and the official Wasm files,
contains neither `dist/call-db.js` nor `dist/call-db.d.ts`, and passes the
isolated packed-package root and payload smoke. The package gate requires the
root JavaScript/declaration indexes and exact root/payload export map in
addition to forbidding the removed artifacts. The rebuilt official Wasm object
retains exactly 117 sorted payload ABI symbols. Current artifact SHA-256
values are:

```text
fastdb4ts-0.0.3.tgz
  d3f695b65555ff47c9211f2a1dd4a8094274341523e2ec94c39c4b2a5cf9805f
official fastdb4ts.js
  ed28d53b23c336abc5dd4c94bc4472a7c323c9fa8029d049758bdafc33a7dc54
official fastdb4ts.wasm
  d2dbbbc4588d618ec4e3f5da5ab4063da28e0249f0c3cfaef272b325b2c5aba7
```

Execution also corrected the P5 plan's stale `ts/build-wasm` ABI-check and
cleanup paths to the real repository-root `build-wasm` directory produced by
`ts/build-wasm.sh`. No duplicate build tree or compatibility path was added.

The frozen specification pass found that runtime smoke and forbidden package
members proved value/runtime removal but did not inspect the packed root
declaration file for the ten removed call-db types. The retained review RED
failed one of seven checker tests. The package gate now reads the actual
`dist/index.d.ts` member and rejects all five removed values, all ten removed
types, and a `call-db` re-export marker.

**Task 3 frozen review:** The implementation commit is `c0bcb69`
(`refactor(ts): remove duplicate payload authority`) and the retained
declaration-proof correction is `6473159`
(`fix(ci): verify removed TypeScript declarations`). The context-owning
primary agent reviewed exact range
`0ff6640dc40b73abe2e0b14353d8aa247de34c5e..6473159` in separate
specification/authority and code-quality passes. The first pass found the one
material Minor package-proof gap above; after its retained RED, correction,
and complete affected-gate rerun, the final classification is 0 Critical,
0 Important, and 0 unresolved material Minor findings. This is same-agent
primary review, not independent or subagent evidence.

**Task 3 ordered intermediate limit:** Legacy native allocator/final-backing
and SWIG debris remain until P5 Task 4, `ColumnEngine` remains until Task 5,
and the root/Python historical README sections still describe removed 0.1.x
surfaces until Task 6.

**Reason:** Task 3 proves one clean TypeScript source/package boundary. Native
header/SWIG changes require their own deterministic byte and package-warning
evidence; the engine rename must remain atomic; and public documentation is
rewritten only after those final names settle.

**Impact:** The built TypeScript package no longer exposes duplicate payload
authority, but the repository as a whole is not yet locally release-ready and
current historical documentation can still lead a reader to removed APIs.

**Dependencies and closure criteria:** Task 4 removes orphaned native
allocators, fixes descriptor determinism, and closes all governed SWIG
warnings. Task 5 performs the no-alias `RecordEngine` rename. Task 6 rewrites
documentation and installs the exact clean-cut policy. Task 7 must then pass
fresh local and installed-package matrices before any version, tag,
publication, release, or C-Two composition claim.

**Task 3 next owner slice:** P5 Task 4 removes orphaned native
allocator/final-backing surfaces, repairs deterministic descriptor
initialization, and closes the
seven governed SWIG warnings. After all FastDB P5 tasks, stop design work at
the frozen owner boundary and begin the separate C-Two-owned composition
task.

**P5 Task 4 local implementation evidence:** From exact start `74ab723`, the
legacy owner now value-initializes every `field_desc_ex_t`, so a non-list
descriptor persists `element_type == 0`. The genuine
`-ftrivial-auto-var-init=pattern` RED passed repeated byte equality first and
then failed the exact persisted member assertion. The corrected focused test
passes and opens the same bytes through the native owner, reading `ftU8` value
`7` and `ftF64` value `7.25`.

The first required ASan+UBSan suite exposed a second real owner-layer defect:
the fixed 20-byte legacy database header places the first `layer_header_t` at
an address that is not naturally aligned for its 64-bit members. That run
passed 39/40 tests and then aborted with a UBSan misaligned-member access in
`FastVectorDb.cpp`; it is not counted as GREEN. Task 4 was explicitly expanded
to fix that finding rather than shifting the test bytes or redesigning the
portable Core. Fixed legacy metadata is copied into aligned owner storage,
and legacy scalar/geometry/list/string-offset reads use `memcpy`-based
unaligned loads. The serialized header, descriptors, table layout, and
zero-copy data spans remain byte-compatible.

The following orphaned call-db-only public/native/SWIG/Python surface is
deleted with no alias or deprecated wrapper:

- `ScratchAllocation`, `ScratchAllocator`, `HeapScratchAllocation`, and
  `HeapScratchAllocator`;
- `FinalBackingAllocation`, `FinalBackingResource`,
  `HeapFinalBackingAllocation`, and `HeapFinalBackingResource`; and
- `FastVectorDbBuild::postToFinalBacking` /
  Python `post_to_final_backing`.

`postToBuffer` and `FixedBufferWriteStream` remain. `TileBoxTake` and
`FastVectorTileDb` remain compiled C++ APIs but are excluded from SWIG parsing.
The internal `utf8_view_t` sequence bridge remains, while its borrowed
`data` member intentionally has no Python setter.

The first real package run under the inverted zero-warning gate emitted the
historical seven diagnostics and failed inventory exactly as required. The
fresh corrected build emits zero matched SWIG diagnostics, passes the real
inventory gate, and contains exactly one Python extension, one FastDB
library, and one binding library. Its local hashes are:

```text
d60df86e14f9514f9e6aaae666d6bac072fbbe459dfcedef026c04397a844f9a  fastdb4py-0.1.22-cp314-cp314t-macosx_26_0_arm64.whl
de2a8f95376719017d18e75edf52adef7ab4c7c42a8902baf06c24fb3054e424  fastdb4py-0.1.22.tar.gz
```

The final native Debug suite passes 40/40 in 71.22 seconds and retains the
exact ABI-117 symbol set. Its `libfastdb.dylib` hash is
`ffaa73c59323d2d797521396fdf21589fe66878c0b2136d16c2c016a5b7c5bcd`.
The hard-fail ASan+UBSan suite passes 40/40 in 230.99 seconds after the
alignment repair, with no retained ASan/UBSan runtime diagnostic. Apple ASan
uses `detect_leaks=0`, so this is explicitly not LeakSanitizer proof. The
Python source suite passes 338 tests plus compileall; the package checker
passes 16 tests; the removed-surface focused test passes; and Issue 0003 is
closed by the warning-free build. The repository-required downstream rebuild
also passes all 57 TypeScript/Wasm tests, and the fresh Wasm object retains
exact ABI-117.

This task changes no portable payload Core source, public payload ABI,
schema, manifest, binary, backing, lifetime, codegen, package version,
workflow, C-Two, or Toodle file. The next owner slice is P5 Task 5: replace
`ColumnEngine`/`column_engine.py` with `RecordEngine`/`record_engine.py`
atomically and without an alias. Public-documentation and clean-cut policy
work remains Task 6; fresh local and installed-package release-readiness
matrices remain Task 7. Hosted execution, package-version changes, tag,
publication, release, and C-Two composition remain pending.

**Task 4 frozen review:** The scoped implementation commit is `ad3e4d6`
(`fix(native): clean legacy backing and descriptor state`). The
context-owning primary agent reviewed exact range `74ab723..ad3e4d6` first for
authority/scope, wire/ABI/backing ownership, and clean-cut completeness, then
separately for correctness, safety, resource behavior, maintainability, and
package portability.

The first pass found one material Minor proof gap: the descriptor regression
asserted the formerly uninitialized `element_type`, but did not independently
pin the alignment padding that is serialized with the complete native
descriptor object. Correction `099a890`
(`test(native): pin serialized descriptor padding`) requires every byte
between `element_type` and `vmin` to be zero. The poison-focused target passes
1/1 in 1.20 seconds. The final exact-range Debug rerun passes 40/40 in 71.76
seconds and remains ABI-117. No production code changed after the complete
sanitizer, Python, package, and TypeScript/Wasm gates recorded above.

The final re-review of `74ab723..099a890` reports 0 Critical, 0 Important, and
0 unresolved material Minor findings. The review confirms that no portable
Core/ABI file changed, the legacy wire bytes and zero-copy data spans remain
in place, removed backing names have no compatibility surface, native tile
implementations still compile, and the zero-warning package gate fails closed.
This is same-agent primary review by explicit user direction, not independent
or subagent evidence.

**P5 Task 5 local implementation evidence:** From exact start `2e99f30`, the
standalone Python AoS record implementation is exposed only as
`fastdb4py.RecordEngine` and
`fastdb4py.record_engine.RecordEngine`. The source module and primary test
module are renamed to `record_engine.py` and `test_record_engine.py`.
`RecordEngine.__module__` is exactly `fastdb4py.record_engine`; the old root
name and module are absent. There is no class alias, deprecated wrapper,
`sys.modules` entry, dynamic `__getattr__`, pickle registration, compatibility
parser, or silent profile conversion. `ObjectEngine` remains the independent
standalone object-graph engine, while legitimate `Table.column`,
`StringColumn`, `BytesColumn`, `StridedColumn`, and field-oriented column
operations retain their names.

The genuine public-surface RED changed only the exact package-surface test and
failed `3` tests while `15` passed: the root still exported the old name, the
new name was absent, and the old module remained importable. The corrected
focused engine/string/materialize/shared-memory/lifetime suite passes
`111` tests in `0.38s`; the complete Python suite passes `340` tests in
`2.77s`; compileall passes; and all `16` package-checker unit tests pass in
`0.043s`.

The final fresh CPython 3.14t package build contains `record_engine.py` in both
sdist and wheel and contains no old engine module. The real package inventory
gate passes with zero governed SWIG diagnostics and exactly
`_fastdb4py.so`, `libfastdb.dylib`, and `libfastdb4py.dylib`. The local
artifacts are:

```text
f4f0b64c9c1873bc070ce92809f5265db5112a534812cf5dbc3c716c5e65c5b8  fastdb4py-0.1.22-cp314-cp314t-macosx_26_0_arm64.whl
bbca84d66fff8eda24fd5d247ebadbde827ba4500c71f75f81230b14c3fdc665  fastdb4py-0.1.22.tar.gz
```

An isolated environment installed that wheel and proved the exact final
export/module boundary plus a two-row
`@feature`/`RecordEngine.truncate`/`Table.fill` round trip. The required
active-source/instruction scan for the old class/module/file name exits `1`
with no match, as does the broader case-insensitive
`column[_ -]?engine` scan. This evidence is local macOS `26.5.2` arm64 with
AppleClang `21.0.0`, CMake `4.3.2`, SWIG `4.4.1`, uv `0.10.12`,
free-threaded CPython `3.14.3`, NumPy `2.3.5`, and pytest `9.0.1`.

**Task 5 current documentation limit:** The root and Python READMEs still
contain historical call-db instructions for APIs already removed by Tasks 1-3,
and current contributor instructions still advertise
`./py_utils.sh --build` even though the helper implements `--setup`, not
`--build`. The latter was reproduced after intentional artifact cleanup:
`uv sync` restores the editable dependency environment but does not run the
CMake/SWIG extension build; `uv pip install --reinstall -e .` regenerates the
native binding, after which `uv sync` restores locked dependencies.

**Reason:** Task 5 is the atomic engine class/module/export rename. Task 6 is
the separately frozen owner for rewriting current public instructions and
installing an executable historical-document allowlist rather than mixing
policy decisions into the rename.

**Impact:** The package surface and archives are correct, but a source-tree
reader can still be directed to removed call-db APIs or a nonexistent helper
mode. FastDB is therefore not yet documentation-clean or locally
release-ready, and downstream composition must not start from those
instructions.

**Owner, dependencies, and closure criteria:** FastDB P5 Task 6 must rewrite
current README/AGENTS/copilot/examples/schema-index/CHANGELOG surfaces, make
the actual native-binding rebuild command truthful, and add exact forbidden
term/package policy that permits only named historical ADR/Issue/migration
records. Task 7 must then execute the fresh source and installed-package
release-readiness matrix. Hosted runs, version changes, push, tag,
publication, release, and C-Two composition remain pending and unauthorized.

**Task 5 frozen review:** The scoped implementation commit is `99e7916`
(`refactor(python): rename AoS engine to RecordEngine`). The context-owning
primary agent first reviewed exact range `2e99f30..99e7916` for brief scope,
authority ownership, exact public surface, no-alias clean cut, retained
standalone behavior, package contents, and documented next-owner boundaries.
All `19` changed paths are within the brief; old tree paths and active old-name
spellings are absent; and Core, ABI-117, Rust, TypeScript/Wasm, schema,
manifest, workflow, versions, C-Two, and Toodle are unchanged.

A separate code-quality pass normalized the file/class/test rename and
compared the frozen source with its predecessor. The engine's executable
implementation is unchanged; only its formerly misleading columnar
description becomes the accurate AoS-record/strided-field description. The
normalized primary test file is byte-identical. Frozen-commit runtime
import/absence checks, `73` public/engine tests, compileall, and diff checks
pass.

Both passes report 0 Critical, 0 Important, and 0 unresolved material Minor
findings. The documented stale public call-db/helper instructions remain the
explicit Task 6 input rather than an unrecorded limitation. This is same-agent
primary review by explicit user direction, not independent or subagent
evidence.

#### P5 Task 6 local evidence

P5 Task 6 installs the exact
`fastdb.p5-clean-cut-policy.v1` contract and a standard-library-only,
fail-closed repository gate. The policy freezes ABI-117, Python `0.1.22`,
TypeScript `0.0.3`, 14 removed paths, 10 required paths, 27 exact historical
files, one non-historical policy-literal carrier, 14 visibly superseded
documents, 26 obsolete-authority literals, nine downstream-domain literals,
and eight standalone-boundary markers. It contains no wildcard or directory
allowlist.

The governed inventory is the NUL-delimited union returned by
`git ls-files --cached --others --exclude-standard`: untracked,
non-ignored files therefore cannot resurrect a removed surface, while ignored
build environments and caches remain outside the scan. Policy paths are
normalized, resolved beneath the repository, required to be regular
non-symlink files where applicable, and rejected on missing, wildcard,
directory, duplicate, escape, or unknown-key input. Current authority and
domain literals are searched in raw bytes; UTF-8 decoding is reserved for
documents whose headings, links, versions, or markers are interpreted.

Current root/Python/TypeScript package instructions now describe the official
portable projections, retained standalone storage helpers, and the Core-only
`fdb codegen` ArtifactSet facade. The eight retained modules state that they
are standalone metadata, storage, serializer, detachment, or lifetime helpers
rather than portable authority. All 14 older design/plan/audit files have a
visible historical/superseded status and resolving links to the accepted
foundation and P5 design. Negative source/package tests keep their absence
proofs while constructing obsolete names from reviewed fragments.

The genuine checker-missing RED exited `1` with `FileNotFoundError` before
test discovery. After the checker unit became green, the first real repository
run exited `1` with exactly 101 stable violations over the expected workflow,
current-documentation, standalone-marker, package-checker, negative-test,
historical-label/link, Issue-state, and current-domain surfaces. It also found
one Markdown false positive from inline code; the corrected parser now blanks
fenced and inline code spans, with a retained regression.

The pre-commit plan audit found that the initial 23-test suite mutated only a
fixture deletion and fixture literals rather than every production policy
entry, and that the checker accepted only the current Closed form of Issue
0003 even though the plan permits a truthful Open form with explicit pending
clean-package evidence. The correction now independently mutates all 14
removed paths, all 26 authority literals, and all nine downstream-domain
literals; rejects intermediate symlink components; and accepts exactly one
truthful Open or Closed Issue 0003 state. Its correction RED was one failure
in 27 tests for the missing truthful-Open behavior, followed by 27/27 green.
The same review removed remaining invalid developer commands and stale
Python-class codegen instructions from the current root, Python, TypeScript,
and contributor guides.

The final pre-commit local evidence is:

- clean-cut checker `27/27` plus the real repository gate: pass;
- P4 projection/codegen checker `17/17` plus repository validation: pass;
- P3 runtime checker `12` runs / `48` assertions plus repository validation:
  pass;
- Python package-checker unit suite `16/16`: pass;
- TypeScript package-checker unit suite `7/7`: pass;
- Python source suite `340/340` in `5.09s`, followed by compileall: pass;
- TypeScript/Wasm source suite `57/57`: pass;
- fresh 41-file `fastdb4ts==0.0.3` tarball inventory/isolated smoke and Wasm
  ABI-117: pass;
- fresh native Debug CTest `40/40` in `54.34s`, followed by the actual shared
  library export check at exact ABI-117: pass; and
- `git diff --check`: pass.

The first P4 repository rerun correctly rejected a broad `docs/**` filter
without the historically frozen exact Issue path, then rejected README text
that no longer contained the truthful P4 handoff marker. The workflow now
retains both the broad docs scope and exact path, and README states that P5
remains open specifically at Task 7; the corrected P4 gate passes.

The native library SHA-256 is
`a4fde51deb44d059449f8fa4aad545515a2acd0e5e6f3fdacbdf9954dc6f2a51`.
The packed TypeScript artifact SHA-256 is
`fb2eb42f1f23d1c1df2d683b7244751b668cfffae6785bd2dc71a256bf4ba884`;
its generated JavaScript and Wasm hashes are respectively
`2e551575ffb880ee8863b6b29b09e5c685a59d0ddca9824a9df263e50dfaad53`
and
`a4c85a21d18aa40862a22013098e3441ea66d7f8ddd301564a352bdb2da59093`.
The test host is macOS `26.5.2` / Darwin `25.5.0` arm64 with AppleClang
`21.0.0`, CMake `4.3.2`, SWIG `4.4.1`, uv `0.10.12`, free-threaded CPython
`3.14.3`, NumPy `2.3.5`, pytest `9.0.1`, Node `25.8.1`, npm `11.11.0`, and
Emscripten `5.0.2`.

All Python, Node, Wasm, native build, package, and cache output was deleted
after evidence; the governed source tree remains approximately `48 MiB`.
The same policy commands are present in one unconditional standalone hosted
workflow job. **Hosted Task 6 execution remains pending** because no push or
hosted run is authorized.

The scoped implementation is `928baa8` (`test: enforce portable payload clean
cut`). The frozen primary-agent spec-compliance review found three Important
gaps:

1. downstream-domain and binding-duplicate scans did not cover the Rust
   bindings and several other current implementation surfaces;
2. Markdown target/anchor validation did not cover all first-party Markdown,
   leaving two stale standalone optimization plans and two broken links
   outside the governed set; and
3. the clean-cut commands lived inside a path-conditional job, so changes
   outside its filter could skip the hosted gate.

The code-quality pass also found three material Minor fail-closed defects: the
SWIG scan hard-coded the fixture/production carrier path instead of consuming
the validated policy; an Open Issue 0003 could pair a completed clean-package
claim with an unrelated hosted `pending` marker; and workflow command checks
accepted `|| true` or `continue-on-error`. The correction RED sequence was
`3/30`, `1/31`, `1/32`, `1/33`, `1/34`, and `1/35` failures for those missing
behaviors and the policy-driven carrier case. Review-fix commit `3cd064a`
closes every finding by:

- scanning the exact Rust raw/safe module inventories for parser, digest, and
  renderer dependencies or definitions;
- expanding downstream-domain roots across all current owned implementation,
  binding, example, package, schema, test, and tool surfaces while keeping
  vendored `fastcarto/lib` outside first-party documentation governance;
- validating links/anchors in every first-party Markdown file and classifying
  `plan.md` plus `optimize/optimize_plan.md` as the 13th and 14th visibly
  superseded records;
- installing an unconditional standalone `clean_cut` workflow job with exact,
  non-failing commands; and
- making Issue-state and literal-carrier behavior policy-driven and
  relationship-aware.

After the last fix, the affected complete gate set passes: clean-cut `35/35`
plus the real repository, P4 `17/17` plus repository validation, P3 `12`
runs / `48` assertions plus repository validation, Python package checker
`16/16`, TypeScript package checker `7/7`, and `git diff --check`. The review
fix changes no runtime, binding implementation, schema, ABI, or package
metadata, so the fresh Python 340, TypeScript/Wasm 57, native Debug 40/40,
packed TypeScript, and ABI-117 evidence above remains the complete affected
runtime gate result.

The final frozen re-review of exact range `d353a1d..3cd064a` is APPROVE with
zero Critical, zero Important, and zero material Minor findings. The
spec-compliance and code-quality passes were performed personally by the
context-owning primary agent as required by the user; this is explicitly not
independent or subagent evidence.

**Task 6 developer-helper limit:** `./py_utils.sh --setup` is an initial
environment setup helper, not a forced native-binding rebuild. After generated
binding output is deleted, `uv sync` restores the editable environment but
does not run CMake/SWIG; `uv pip install --reinstall -e .` regenerates the
binding, and a following `uv sync` restores locked dependencies. Current
instructions now state this exact sequence instead of advertising a
nonexistent helper mode.

**Reason and impact:** Changing the setup helper's repeat-install semantics is
not required to define the package or portable authority and is not hidden
behind a misleading command. A contributor who only runs `uv sync` after
deleting native output will still lack the generated extension until using
the documented forced reinstall.

**Owner, dependencies, and closure criteria:** FastDB packaging owns the
helper. A later change may make its setup mode idempotently force the editable
native rebuild, but must retain locked dependency parity and a regression that
starts without generated output. Until then, the documented direct rebuild is
the supported clean-tree command. P5 Task 7 proves the complete fresh local
readiness matrix without changing this helper. Hosted execution, version
change, push, tag, publication, release, and C-Two composition remain pending
and unauthorized.

#### P5 Task 7 local evidence

**P5 local clean cut:** Complete

```text
portable ABI = exactly 117
package versions = 0.1.22 / 0.0.3 unchanged
primary review = same-agent, not independent
hosted CI = pending
push/tag/publication/release = not performed
C-Two composition = next downstream owner slice
```

Task 7 started from exact commit
`47ab2eeb0ba161563942e8b4098fbc828aa1be98`. Its first closure-marker check
was a genuine RED: the exact
`**P5 local clean cut:** Complete` line was absent and `rg` exited `1`. No
release record was written before the complete readiness matrix passed.

The final post-review native results on macOS `26.5.2` / Darwin `25.5.0`
arm64 with AppleClang `21.0.0` and CMake `4.3.2` are:

| Configuration | Result | Portable static SHA-256 | `libfastdb` SHA-256 |
|---|---:|---|---|
| Debug | 40/40 in 54.17s | `030a9afe148ec2a95db062d6273cb41e513804155e88035f38bc87c6be31e847` | `dadb3553d20de55dd0033e2a58b60c5397bfd22ce5ac368d39518dcf43bda0d1` |
| Release | 40/40 in 48.08s | `dc34a9bcd3e430a4b81dd50dfcaaf2544237a26bdfa2fc9a8b1b2000dac7d6a8` | `b9e96d45748695aeacf6d4dd8675d672ad3a602c05a28a3f14bfb4fd81212d34` |
| ASan+UBSan Debug | 40/40 in 231.83s | `4759ddb10ea81ffecb122af22f8742f444cda99da2ab226e4e93394f75ce6efe` | `168d0a5005f112ce0d3f528e201a60c87ffd842ed0bb00eb6f7551d27db95c9c` |
| focused TSan Debug | 14/14 in 349.32s | `e541757dfb6fd5b51ce871237a2784b900fc647611f77d19848311e866e54af8` | `21266ac069fd27d49284b8aef68c21ca19da5899cd8a095ff88bfe6066c3dab9` |

ASan+UBSan ran with
`ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. This is explicitly not a
LeakSanitizer claim. The deterministic binary-open robustness runner passed
the exact 16-seed repository corpus again under those sanitizers. The local
AppleClang installation advertised a bare libFuzzer runtime path but the
archive did not exist, so no local coverage-guided fuzz result is claimed;
the deterministic sanitizer corpus is the available local proof and hosted
Linux remains the owner of the standard libFuzzer run.

The exact ABI, schema, dependency, package, and portability gates also pass:

- native and both Wasm inventories contain exactly 117 sorted
  `fdb_payload_v1_*` symbols;
- the 16-seed corpus checker, embedded-schema regeneration, vendored
  dependency check, P2/P3/P4/P5 repository gates, and Wasm-export generator
  checks pass;
- the public C11 header compiles warning-clean for arm64, x86_64, and wasm32;
  the object hashes are respectively
  `78ca66e27ff3fd67e5c4c5a304f717f31e26ea75be6113f5f71b38bb5a044ba8`,
  `79a5e766ac7f8da29a6c7fab8644d1e5a349b2011dd343450d94036e076a5023`,
  and
  `5a76e9415c473353ba05939be9f92281d6ce16cb21765edb63270e5e431ca0b7`;
- Rust `fmt` and all-feature `clippy -D warnings` pass; the safe/raw crates
  pass 21 unit/integration tests plus the compile-fail lifetime doctest, and
  the relocated system-link package proof passes under Rust/Cargo `1.91.0`;
  and
- Core-generated C++, Rust, Python, and TypeScript artifacts compile/import
  and execute through the same ABI, including the four-shape hostile matrix.

The first Python run after disk cleanup stopped during collection with 24
missing-`fastdb4py.core` imports because generated native binding output had
been deliberately removed. The documented
`uv pip install --reinstall -e .` followed by `uv sync` regenerated the
binding and restored locked dependencies; no source defect was hidden by that
environment recovery.

The first independently built Python 3.10 wheel then exposed a real product
RED: 187 installed-wheel tests passed and four Windows artifact paths
(`trailing.`, `trailing `, `file:stream`, and `bad"name.py`) were incorrectly
accepted. Python 3.13+ has `ntpath.isreserved`; Python 3.10 does not, and the
old `PureWindowsPath.is_reserved()` fallback covered device names but not the
remaining Windows filename rules. Commit `9bef0f1` retains a regression that
removes `ntpath.isreserved`, then adds the conservative standard-library rules
for ASCII controls/forbidden characters, trailing dot/space, and DOS device
names including superscript aliases. It changes only the CLI destination-path
safety boundary, not Core parsing, canonicalization, digest, layout, binary,
codegen, or artifact identity.

The final rerun passes the source suite 344/344 on free-threaded CPython
`3.14.3`, source compileall under both CPython `3.14.3` and minimum supported
CPython `3.10.17`, and the full independently installed Python 3.10 wheel
suite 344/344. Both fresh package inventories pass, and both isolated
no-project selected installed-wheel suites pass 195/195:

| Python artifact | Bytes | SHA-256 |
|---|---:|---|
| `fastdb4py-0.1.22-cp314-cp314t-macosx_26_0_arm64.whl` | 1,135,772 | `6c87edc721141b295b511e0c76f22da5e37c2fb8584513c6fee77d9f59591866` |
| current-interpreter `fastdb4py-0.1.22.tar.gz` | 1,068,012 | `c6fb7e11ee6a02b61b2d7c02bc4d93d66555fefa663faf8f2bb79c228217517a` |
| `fastdb4py-0.1.22-cp310-cp310-macosx_26_0_arm64.whl` | 1,132,548 | `b8bdb707031207d24bed70eb37aa1fd1d0f5e0ee23d17dd725b5f1c2afea19cd` |
| Python-3.10 `fastdb4py-0.1.22.tar.gz` | 1,067,979 | `1e4f29bc39df1c643718f8b9497e1bc5e551dcea7fa3f6e7af9bbdc682de5ac1` |

Both final logs contain zero matched SWIG `Warning NNN` diagnostics, so Issue
0003 remains closed. The Python 3.10 log does contain one C++ compiler warning
from NumPy's `import_array()` macro returning `NULL` from an `int` wrapper and
wheel-tag warnings because the local interpreter targets macOS 11 while the
built native libraries inherit the macOS 26 host target. Those are not SWIG
parser diagnostics, are not concealed as a warning-free whole log, and remain
local packaging/toolchain observations. Hosted lower-macOS, Linux, and Windows
package execution is still pending.

The official Emscripten `5.0.2` / Node `25.8.1` TypeScript/Wasm path passes
57/57 source tests, seven package-checker tests, the exact 41-file packed
inventory, package smoke, and ABI-117. Its artifacts are:

| TypeScript artifact | Bytes | SHA-256 |
|---|---:|---|
| `fastdb4ts-0.0.3.tgz` | 639,738 | `219fcda8ae71ff97a8ddc0cf11a1edf6c2d7299b5371c32195f5bbd781080a9a` |
| `dist/wasm/fastdb4ts.wasm` | 1,976,272 | `67320567f90fb0c49615e731a58adfd0e639c282c2b9c0eb01d6a3836f3cd0cd` |
| `dist/wasm/fastdb4ts.js` | 122,622 | `ed28d53b23c336abc5dd4c94bc4472a7c323c9fa8029d049758bdafc33a7dc54` |

A separate fresh Core Wasm tree also exposes exact ABI-117 and passes all
seven explicit Node receipts: runtime harness, pure-C header, generated C ABI,
C++ facade, runtime C++ facade, injected-failure rollback, and graph-runtime
proof. The generated projection checker passes 6/6 before the complete
four-language execution harness.

The final pre-record documentation/scope gate passes P5 clean-cut 41/41 plus
the real repository check, P4 projection/codegen 18/18 plus its repository
check, Python package inventory 17/17, `git diff --check`,
schema/JSON/package inventories, first-party Markdown link/anchor checks, and
an added-line credentials/private-endpoint scan. The plan's
whole-changed-file drafting-token scan reports two baseline facts: a 2026-04
plan sentence that literally spells the token it says is absent and a 2025
native geometry-size comment. `git blame` at the frozen P5 start proves both
predate P5; neither is a new production placeholder or Task 7 limitation,
and Task 7 does not expand into the unrelated legacy geometry algorithm
merely to force an unqualified text scan to exit `1`.

All disposable Task 7 native, sanitizer, Rust target, Python distribution,
TypeScript distribution/package, and Wasm build trees were deleted after
their hashes and results were recorded. Source virtual environments are
retained as required by the plan. The primary-agent closure review is
performed personally in separate specification-compliance and code-quality
passes after freezing the closure commit; by user direction it is same-agent
evidence and is not represented as independent or subagent review.

##### P5 final frozen primary-agent review

The mechanically frozen review starts at P4 closure
`9d86c171eda1fe107c3519ce040ca2ec417167f9` and includes every P5
implementation, deletion, review fix, and closure record. The final review
found and closed these post-closure issues:

- `026b17b` makes the repository clean-state gate include nonignored
  untracked files, makes the workflow and Python package inventories exact,
  and copies standalone serialized `wstr` values into aligned owned storage
  before exposing a wide-character pointer. The alignment correction belongs
  only to the retained standalone legacy reader; it does not change portable
  Core layout, binary meaning, ABI, or any language projection.
- `a599e40`, `c5e4bce`, `9e013c3`, `216a3a7`, `b8ec185`, and `2203180`
  make workflow shell behavior, Issue-index state, and current guide markers
  executable and exact without introducing downstream-domain semantics into
  the FastDB policy checker.
- `1c0c88f` and `1b508a1` keep historical P2/P4 facts frozen while allowing
  their repository gates to accept the later truthful P5 phase. Their genuine
  REDs were the current Issue-index row rejected by the old P2 checker and
  the current P5-complete README rejected by the old P4 checker.

The final specification-compliance pass covers the sole C++ Core authority,
exact ABI-117, complete removal policy, `RecordEngine` clean rename,
standalone truth, four-language projection/codegen parity, package contents,
and external-fact limits. The separate code-quality pass covers native
initialization/alignment and ownership, C ABI output clearing, Python 3.10
path safety, TypeScript/Wasm disposal, checker fail-closure, workflow shell
semantics, determinism, portability, credentials, debris, and maintainability.
Both passes end with 0 Critical, 0 Important, and 0 unresolved material Minor
findings. This is same-agent primary review by explicit user direction, not
independent or subagent evidence.

**Closure criteria:** Remove the obsolete public authority without aliases or
compatibility parsers; rename `ColumnEngine` to `RecordEngine`; pass package,
clean-cut, parity, warning-free package, and local release-readiness gates
without changing the current package versions or publishing. Record hosted,
version-bump, tag, publication, and release facts as pending. The now-complete
local C-Two proof delegates the nested spec and composes artifacts without
duplicating FastDB semantics, but remains downstream evidence rather than part
of FastDB P5 or an official release.

#### C-Two downstream local-candidate handoff evidence

FastDB implementation commit
`7eb74734926bd8fe911229eee9744a6dd8172487` packages the unchanged P5/Core
boundary into seven FastDB-owned artifacts. The canonical retained manifest is
[`evidence/fastdb-local-candidate-manifest.v1.json`](evidence/fastdb-local-candidate-manifest.v1.json),
has SHA-256
`9a1c7c83dca16237257dcc50d5917f10d032e02ffceae1278ae331d101b69d32`,
and records exact ABI-117 plus the Core bundle, CPython 3.10/current wheels,
sdist, `fastdb`/`fastdb-sys` crates, and `fastdb4ts` tarball.

C-Two implementation commit
`bf6f5c950959bcd2723cf3c7bfe772c9ee91dc02` embeds that exact FastDB
manifest and artifact set in its 42-entry candidate manifest with SHA-256
`73d723dbec919d20c229b6fa441750571efcb0937bd2b3c9f84260807c11e0f1`.
Isolated consumers pass for version-only Rust packages, no-index CPython 3.10
and current wheels, and tarball-only Node packages. Candidate-stage runtime
receipts contain exactly 18/18 passing Rust/Python direct/relay rows and 12/12
passing generated TypeScript Node rows. No row is skipped or xfailed.

The downstream matrix covers no-payload, `record.v1`, and
`object_graph.v1`; Rust client to Rust host, Rust client to Python host, and
Python client to Rust host; direct IPC and explicit relay; plus generated
TypeScript direct IPC, explicit relay, relay-aware verified local IPC, and
relay-aware HTTP. The TypeScript result is Node-only on the recorded
darwin-arm64 platform; browser runtime remains unverified.

The proven Rust and Python receive adapters are copy-backed. Held response and
borrowed request lifetimes invalidate checked FastDB owners before C-Two lease
release, but this does not prove direct construction into final response shared
memory or revocation of deliberately leaked raw buffers. The local artifact
versions remain `0.1.22`/`0.0.3`; commit and SHA-256 provenance disambiguate
them from previously published packages with the same metadata.

This handoff changes no FastDB payload semantics, C ABI symbol, profile, binary
layout, lifetime rule, package version, or owner boundary. The source of every
artifact is the implementation commit above; the documentation-only commit
containing this record follows it and is not an artifact source. Hosted
Linux/Windows execution, version bump, push, tag, publication, official
release, browser proof, and official downstream pinning remain pending.

## P1 implementation-gate observations

These are implementation-gate observations, not post-0.2.0 deferrals. Closed
entries retain their historical cause and exact closure evidence.

### Legacy call-db uninitialized descriptor-member nondeterminism

**Closed by P5 Task 4:** Before P5 Task 2, two independent legacy
`encode_call_db` / `encode_call_db_into` builds of the same scalar-array value
could differ at byte 182 on CPython 3.13. The deleted byte-for-byte call-db
test reproduces this at the pre-P2-Task-5 starting commit
`0f08e9feeed09a7acba8d3639e0f0e3d2091a756`. P5 Task 2 removes that obsolete
Python entry point and test, but the underlying native descriptor write
remained open until the P5 Task 4 cleanup.

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

**Closure evidence:** P5 Task 4 value-initializes the complete descriptor
representation, pins a zero non-list `element_type` under compiler poison,
requires repeated byte equality, and opens/reads the resulting legacy image.
The repair remains in the standalone legacy owner and does not change the
portable-record Core.

### Pre-existing SWIG diagnostics

**Closed by P5 Task 4:** Earlier Python wheel builds succeeded with the exact
seven legacy SWIG diagnostics enumerated in
[Issue 0003](0003-legacy-swig-diagnostics.md).

**Reason:** These diagnostics come from the legacy 0.1.x SWIG input surface;
Task 9 classifies them but does not redesign that API.

**Impact:** Existing wheels still build and the P1 C ABI is unaffected, but no
new warning may be accepted implicitly. Issue 0003 is the exact owner for the
residual cleanup and package-warning gate.

**Closure evidence:** Issue 0003 records the fresh zero-warning sdist/wheel
build, exact package inventory, negative any-warning checker, and local
artifact hashes. Native tile APIs remain C++-available while intentionally
outside SWIG, and the borrowed UTF-8 data pointer has no Python setter.

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
and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`: the fresh post-review-fix
run passed all 30 CTest targets in 95.78 seconds and the retained log contained no runtime, ASan, UBSan,
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
| P2. Record binary/runtime/lifetime | Locally complete and frozen at exactly 99 symbols; Task 11 complete fresh local gates are green and the same-reviewer final result is 0 Critical / 0 Important / 0 Minor | Keep hosted outcomes pending until an authorized run exists; do not reopen P2 semantics from a downstream binding |
| P3. Object-graph runtime | Locally complete and frozen at exactly 105 symbols; D1 closed; complete local gates and the user-authorized primary-agent review are green; hosted execution pending | Preserve the frozen P3 Core/ABI meaning through P4/P5; do not convert the explicitly non-independent review or workflow definitions into independent/hosted evidence |
| P4. Language projections and payload codegen | Locally complete and frozen at exact ABI-117: equal projections, Core-owned deterministic four-target generation, truthful manifests, simple and four-shape hostile generated-output execution, packages/workflow, fresh local gates, and the same-agent primary review are green; hosted execution remains pending | Preserve ABI-105 runtime meaning and ABI-117 public truth through P5; do not convert the explicitly non-independent review or workflow definitions into independent/hosted evidence |
| P5. Clean cut and local release-readiness handoff | Locally complete at exact ABI-117: Tasks 0-6 are frozen and Task 7 passes fresh native Debug/Release, sanitizer/TSan, Rust, Python 3.10/current installed-package, generated-output, TypeScript/Wasm, package, documentation, and scope gates; package versions remain 0.1.22/0.0.3 | Keep hosted platform results, version change, push, tag, publication, and release pending; hand only the frozen FastDB boundary to the C-Two-owned composition slice |

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
