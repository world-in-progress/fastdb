# Issue 0002: Portable Payload Foundation Implementation Status

- **Status:** Open
- **Opened:** 2026-07-16
- **Owner:** FastDB
- **Governing design:** [FastDB Portable Payload Foundation Design](../superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
- **Governing decision:** [ADR-0001](../decisions/0001-portable-payload-core-authority.md)
- **First implementation plan:** [Portable Payload Core Contract Implementation Plan](../superpowers/plans/2026-07-16-portable-payload-core-contract.md)
- **P2 implementation plan:** [Portable Payload Record Runtime Implementation Plan](../superpowers/plans/2026-07-17-portable-payload-record-runtime.md)
- **P3 implementation plan:** [Portable Payload Object-Graph Runtime Implementation Plan](../superpowers/plans/2026-07-20-portable-payload-object-graph-runtime.md)
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

P4 Rust/Python/official TypeScript-Wasm portable projections now include the
locally complete compile/query and author/freeze/plan slices, while runtime
execution/lifetime parity and Core-owned four-language code generation remain
open. P5 legacy authority removal,
`RecordEngine` clean rename, package/version release readiness, and later
C-Two-owned composition also remain open. The repository still ships the
0.1.x call-db/`ColumnEngine` migration surface, so Issue 0002 remains open and
no FastDB 0.2.0 release is claimed.

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
  are frozen as P4 delta authority. P4 Tasks 1-2 now add real compile/query and
  author/freeze/plan projection slices over the unchanged ABI-105: a
  payload-only native archive
  built from the same Core object files, raw/safe Rust crates, the
  `fastdb4py.payload` package, and the official TypeScript/Wasm payload
  subpath. A generated Emscripten export inventory is derived mechanically
  from the exact reviewed allowlist; it exposes all 105 C functions without
  changing their declarations, definitions, or meaning. The three language
  projections now author every V1 value kind, fixed runs, nullable/empty lists,
  object identities/references, explicit roots, and immutable Core-owned plan
  facts. Backing, execution, open, view, materialize, invalidate, parity
  closure, and Core-owned codegen remain later P4 tasks. All P5 work remains
  open;
- current public call-db, `fastdb.schema.v1`, `columnar.v1`, and `ColumnEngine` surfaces remain 0.1.x migration inputs, not the accepted 0.2.0 authority.

P1, P2, and P3 are locally implemented and frozen. P1/P2 retain their recorded
independent-review evidence; P3 uses the user-authorized context-owning
primary-agent review and does not claim independence. Their first hosted CI
execution remains pending. The repository must not claim that the complete
portable payload foundation or FastDB 0.2.0 is implemented because P4 and P5
remain non-deferrable.

## Current limit

Users and downstream repositories can compile/query a specification and can
author, build, open, copy, navigate, materialize, and invalidate complete
`record.v1` and ordinary `object_graph.v1` payloads through the public C ABI and
thin C++ facade. Graph sharing, cycles, explicit ref dereference, payload-scoped
identity, direct/staged backing, and the shared checked owner/access lifetime
are executable. P2 is independently reviewed and locally frozen; P3 is locally
frozen at the exact 105-symbol boundary with D1 closed, complete Core Wasm
graph execution, and the explicitly non-independent primary-agent review
described below. Rust, Python, and official TypeScript/WASM now compile/query
the shared `record-all-types` and invalid `bad-kind` fixtures and author/freeze
the same fixed, nested-list, cyclic/shared graph, and disconnected-root
fixtures through the ABI-105 Core. Their projections preserve canonical bytes,
digest, manifest, capabilities, stable indexes, every V1 authoring kind,
Core-owned limit/errors, exact immutable plan facts, retained clones, and
explicit disposal. Backing execution, open, view, materialize, invalidation,
parity closure, and every Core-owned codegen target remain absent. Hosted
outcomes remain pending. Downstream consumers may use
only reviewed public FastDB contracts
and must not depend on private headers or recreate FastDB semantics. The full
structured-payload foundation remains incomplete because P4 and P5 are open.

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
P3 graph runtime exist in the exact ABI-105 C boundary with thin C++ facades
over that same ABI. P4 Tasks 1-2 project compile/query plus complete V1
authoring and immutable plan facts into Rust, `fastdb4py.payload`, and the
official TypeScript/Wasm `./payload` subpath. The Core still does not generate
C++, Rust, Python, or TypeScript payload artifacts, and the three projections
do not yet expose backing execution, open, views/access, materialization, or
invalidation. Existing hand-written 0.1.x call-db layers are migration inputs,
not portable-payload projections.

**Design state:** The complete live delta audit and docs-first design are
recorded in
[the P4 projection/codegen design](../superpowers/specs/2026-07-21-portable-payload-language-projections-codegen-design.md),
with vertical TDD slices in
[the P4 implementation plan](../superpowers/plans/2026-07-21-portable-payload-language-projections-codegen.md).
They preserve ABI-105 runtime meaning, reuse the existing C++ RAII facade,
create Rust/Python/TypeScript projections only with real callers, and reserve
the only additive public Core delta for a nine-symbol immutable ArtifactSet
family. The designed 114-symbol result is not a frozen implementation fact
until native/Wasm scanners and all generated-output gates prove it.

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

**Impact:** Portable compile/query/build/open/view/materialize remains available
to C and C++ through the locally frozen P3 boundary. Rust, Python, and official
TypeScript/Wasm now share compile/query plus complete author/freeze/plan
semantics from that same Core. They still cannot execute or open a plan, retain
backing, navigate a view, materialize, or invalidate through their new payload
projections. No language can yet consume a Core-owned generated artifact set.

**Next owner slice:** P4 Task 3 projects execution, backing ownership, and open
vertically across the same public languages while preserving the frozen Core
meaning and ABI-105.

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
| P4. Language projections and payload codegen | Docs-first design is frozen; Tasks 1-2 compile/query and author/freeze/plan projections are locally complete over unchanged ABI-105 with primary-agent review and no independent/hosted claim; Tasks 3-9 remain open | Continue the reviewed vertical plan through execution/runtime/lifetime parity, then prove deterministic C++/Rust/Python/TypeScript in-memory artifact generation from Core |
| P5. Clean cut, release, downstream composition | Blocked on P4 | Public call-db/schema/columnar authority removed, `RecordEngine` rename complete, packages at 0.2.0 pass release gates, then downstream composition delegates the nested FastDB sub-spec without semantic duplication |

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
