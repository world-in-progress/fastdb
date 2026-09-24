# FastDB Portable Payload Language Projections and Codegen Implementation Plan

> Execute this plan in the primary task with strict TDD and retained task
> briefs/reports. Do not delegate implementation or review and do not claim an
> independent review.

**Goal:** Complete FastDB P4 by projecting the frozen portable-payload Core
through C++, Rust, Python, and TypeScript/Wasm, then add deterministic
Core-owned C++/Rust/Python/TypeScript in-memory codegen.

**Architecture:** The existing C++ Core remains the sole semantic authority.
The exact ABI-105 runtime meaning is consumed unchanged. Language projections
wrap the C ABI; generated code wraps those projections. The public Core
extension is the reviewed twelve-symbol P4 delta: three Core-owned
specification-provenance guards plus the nine-symbol immutable ArtifactSet/
codegen family, introduced atomically before generated artifacts are
advertised.

**Governing design:**
[2026-07-21-portable-payload-language-projections-codegen-design.md](../specs/2026-07-21-portable-payload-language-projections-codegen-design.md)

**Starting point:** `2f1f19925b942113e812b27b31b857556e7405f7` on
`socu/portable-payload-foundation`, or its reviewed clean successor if Task 0
has already committed.

---

## Execution rules for every task

Before editing production code, create
`.superpowers/sdd/p4-task-<n>-brief.md` with the exact starting commit, allowed
files, consumed/produced interfaces, genuine RED, focused and broad gates,
documentation obligation, non-goals, and commit subject.

For every task:

1. write the failing test first and retain the real failure;
2. implement one complete vertical capability without a fake or binding-side
   semantic fallback;
3. run focused GREEN and every affected broad gate;
4. update Issue 0002, the P4 proof map/quality gate once present, the ignored
   progress ledger, and the ignored task report;
5. inspect the exact tracked diff and commit only the scoped task;
6. review the frozen commit once for spec compliance and once for code quality;
7. fix all material findings in the same primary-agent context; and
8. rerun every affected broad gate after the last fix before closing the task.

Never change package versions, push, tag, publish, modify C-Two/Toodle, or
remove P5 migration surfaces in this plan.

---

## Task 0: Freeze the P4 delta design and plan

**Files**

- Create:
  `docs/superpowers/specs/2026-07-21-portable-payload-language-projections-codegen-design.md`
- Create:
  `docs/superpowers/plans/2026-07-21-portable-payload-language-projections-codegen.md`
- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`
- Create/update ignored:
  `.superpowers/sdd/p4-task-0-{brief,report}.md`,
  `.superpowers/sdd/progress.md`

**Step 1: Reprove the frozen input**

Run:

```bash
python3 tools/check_payload_abi_symbols.py --build-dir build/p3-final-debug
ctest --test-dir build/p3-final-debug --output-on-failure
```

Required baseline: exact ABI-105 and 37/37 Debug tests.

**Step 2: Complete the live delta audit**

Read and classify the public header, C++ facade, Core compiled model/manifest,
Python SWIG/package path, TypeScript/Wasm host, legacy codegen, Rust absence,
goldens, ABI checker, package checker, and workflow gates. Record which files
are P4 inputs and which are P5-only migration removals.

**Step 3: Write and adversarially review the tracked documents**

Require the design to freeze:

- one authority and the projection dependency direction;
- ownership/error/text/backing semantics for every language;
- the exact additive codegen ABI names and prefix/output rules;
- Core ArtifactSet/path/provenance/identifier invariants;
- executable shared parity/codegen/package proof; and
- a strict P4/P5 boundary.

Run relative-link, placeholder, forbidden-type, and `git diff --check` gates.

**Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-07-21-portable-payload-language-projections-codegen-design.md \
  docs/superpowers/plans/2026-07-21-portable-payload-language-projections-codegen.md \
  docs/issues/0002-portable-payload-foundation-implementation-status.md
git commit -m "docs: design portable payload language projections"
```

---

## Task 1: Land one four-language compile/query projection slice

**Purpose:** Create every missing projection only when it has a real end-to-end
Core caller. Freeze the native/Wasm link seams before broader runtime work.

**Expected files**

- Modify: `fastcarto/fastdb/CMakeLists.txt`
- Modify: `ts/embind/CMakeLists.txt`, `ts/build-wasm.sh`
- Create: `tools/generate_payload_wasm_exports.py`
- Create: `tests/ci/test_generate_payload_wasm_exports.py`
- Create: `bindings/rust/Cargo.toml`
- Create: `bindings/rust/fastdb-sys/{Cargo.toml,build.rs,src/lib.rs}`
- Create: `bindings/rust/fastdb/{Cargo.toml,src/lib.rs}`
- Create: `tests/rust/payload/` compile/query integration coverage
- Create: `python/fastdb4py/payload/{__init__.py,_ffi.py,_error.py,_spec.py}`
- Create: `tests/python/payload/test_compile_query.py`
- Create: `ts/fastdb4ts/src/payload/{abi.ts,error.ts,spec.ts,index.ts}`
- Modify: `ts/fastdb4ts/src/wasm-loader.ts`, `ts/fastdb4ts/package.json`
- Create: `tests/ts/payload_compile_query.mjs`
- Modify: `MANIFEST.in`, `setup.py`, package inventory/checker tests as needed
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

Exact file splits may be simplified during implementation, but no crate or
module may be created without the compile/query test in this commit.

**Interfaces produced**

- A payload-only linkable CMake target backed by the same Core object files.
- Mechanical exact Wasm exports derived from the ABI allowlist.
- `fastdb-sys` raw ABI and a safe Rust `CompiledSpec`/`Blob`/`PayloadError`.
- Python and TypeScript/Wasm `CompiledSpec`, identity, manifest,
  capabilities, profile, and stable index queries.

**Genuine RED**

Add one shared record fixture test in Rust, Python, and TypeScript that compiles
through the Core and asserts the checked canonical bytes/digest/manifest and
stable IDs. Run:

```bash
cargo test --manifest-path bindings/rust/Cargo.toml --workspace --all-features
uv run pytest tests/python/payload/test_compile_query.py -q
npm --prefix ts/fastdb4ts run build:wasm
npm run test:ts
```

Expected RED: Rust workspace and official Python/TypeScript payload modules do
not exist. The failure must reach the missing projection, not malformed test
data.

**GREEN requirements**

- Raw Rust declarations match C sizes/constants and invoke the real library.
- Safe Rust contains no public raw pointer and owns copied errors.
- Python uses the packaged/native `fdb_payload_v1_*` ABI and supports Python
  3.10 syntax.
- TypeScript calls exact Wasm ABI exports with lossless `bigint` for 64-bit
  values and explicit disposal.
- All three compare against the same Core golden source/canonical/digest; none
  canonicalizes locally.
- C++ ABI-105 and existing C++ facade tests remain unchanged and green.

**Focused/broad gates**

```bash
cmake -S fastcarto -B build/p4-task1 -DBUILD_TESTING=ON \
  -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build/p4-task1 --parallel
ctest --test-dir build/p4-task1 --output-on-failure
python3 tools/check_payload_abi_symbols.py --build-dir build/p4-task1
cargo test --manifest-path bindings/rust/Cargo.toml --workspace --all-features
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
uv build --out-dir build/p4-task1-dist
npm --prefix ts/fastdb4ts run build:wasm
npm run test:ts
git diff --check
```

ABI must remain exactly 105 in Task 1.

**Commit:** `feat(bindings): project payload compile and query`

---

## Task 2: Project complete authoring and immutable plan facts

**Purpose:** Give all language users the same typed builder state machine
without adding language-owned validation.

**Expected files**

- Modify safe Rust builder/plan modules and raw declarations as needed
- Create/modify Python payload builder/plan modules
- Create/modify TypeScript payload builder/plan modules
- Create shared Rust/Python/TypeScript builder tests
- Modify Issue 0002 and task evidence

**Interfaces produced**

- builder options and unique builder ownership;
- entry begin, every scalar bit/value, `str`, `wstr`, bytes, component begin,
  recursive list begin, null, fixed run;
- object declare/fill, root/object/ref values;
- immutable plan and complete V2 plan facts; and
- stable builder/type/range/state errors.

**Genuine RED**

Write record-all-types and object-graph authoring tests first. They must fail on
the first absent builder projection while the same fixture compiles through
Core.

```bash
cargo test --manifest-path bindings/rust/Cargo.toml --workspace --all-features \
  builder
uv run pytest tests/python/payload/test_builder.py -q
npm --prefix ts/fastdb4ts run build
node --test tests/ts/payload_builder.mjs
```

**GREEN requirements**

- No binding checks schema kind/range/profile before calling Core.
- Rust/Python/TypeScript preserve floating bits, UTF-8/UTF-16 units, opaque
  bytes, null/empty, object handles, and exact error fields.
- Builders are unique/non-cloneable; plans use Core retain/release.
- Shared tests exercise all V1 values, deep lists within reviewed limits,
  forward refs, missing fields/entries, and invalid builder state.

**Broad gates:** Task 1 gates plus focused existing C++ builder/graph tests and
ASan+UBSan for affected native code if any host adapter changes C++.

**Commit:** `feat(bindings): project payload authoring and plans`

---

## Task 3: Project execution, backing ownership, and open

**Purpose:** Close heap/external backing, direct/staged truthfulness, payload
ownership, binary access, and copy/external open across hosts.

**Expected files**

- Modify Rust safe backing/execution/payload modules
- Modify Python payload backing/execution/payload modules
- Modify TypeScript payload backing/execution/payload modules
- Add a small Wasm-native owner adapter only for Wasm-linear-memory external
  ownership if raw callbacks cannot be represented safely in TypeScript
- Add Rust/Python/TypeScript backing/lifetime tests
- Modify Issue 0002 and evidence

**Interfaces produced**

- internal Core heap execution;
- safe caller-backing adapters in Rust and Python;
- Wasm-owned external storage adapter and accurately named JS copies;
- `REQUIRE_DIRECT`/`ALLOW_STAGING`, plan/payload execution reports;
- payload binary blob/acquire, copied open, and external open; and
- deterministic commit/rollback/retain/release failure behavior.

**Genuine RED**

Tests first require:

- a direct internal-heap build;
- a backing that rejects direct and accepts staged;
- direct-required exact failure;
- external owner retention after the initiating host object is dropped;
- rollback after injected write/commit failure; and
- copied bytes remaining valid independently.

Expected RED is the absent backing/execution projection, not a changed Core
plan outcome.

**GREEN requirements**

- Binding callbacks never throw/unwind across C.
- Callback objects and readable buffers remain pinned until Core release.
- TypeScript never calls a JavaScript-to-Wasm copy “external zero-copy”.
- Reports exactly match Core mode/reason/byte/region/capacity fields.
- No second complete image is introduced in a binding-side “direct” path.

**Broad gates:** all projection tests, existing backing/direct proofs, focused
TSan where available, native/Wasm exact ABI-105, package builds, and sanitizer
coverage for callback failure cleanup.

**Commit:** `feat(bindings): project payload backing and execution`

---

## Task 4: Project complete record views, access, materialization, and invalidation

**Purpose:** Close the complete non-ref algebra and checked lifetime in every
projection before graph-specific ergonomics.

**Expected files**

- Modify Rust safe view/access modules
- Modify Python payload view/access modules
- Modify TypeScript payload view/access modules
- Add shared record parity/lifetime tests and package exports
- Modify Issue 0002 and evidence

**Genuine RED**

Use the checked-in record fixtures to require sequence/list navigation,
component fields, every scalar, normalized endpoints, `str`, `wstr`, bytes,
null/empty, nested lists/components, materialize, invalidate, and stale access.
Expected RED is the absent view/access method.

**GREEN requirements**

- C++/Rust borrows cannot outlive `Access`; Python/TypeScript safe APIs copy.
- Wide text uses ABI-returned aligned units and one reviewed Unicode conversion
  per host, not a binary parser.
- A source view fails after Core invalidation with exact 4001 fields.
- A Core-materialized view survives source invalidation and release.
- Read-only behavior and exact kind/type mismatch errors are preserved.

**Broad gates:** all Task 3 gates plus complete record goldens and lifetime
stress/drop/dispose tests.

**Commit:** `feat(bindings): project checked record views`

---

## Task 5: Project object graphs, refs, identity, sharing, and cycles

**Purpose:** Complete P3 semantics in every binding without a binding-owned
graph algorithm.

**Expected files**

- Modify only projection view/builder ergonomics and tests
- Add shared object-graph parity tests in Rust/Python/TypeScript
- Modify Issue 0002 and evidence

**Genuine RED**

Require disconnected roots, forward refs, shared targets, self and mutual
cycles, null refs, graph identity, ref targets, complete reachable-closure
materialization, invalid refs, and generation invalidation. Expected RED is the
missing graph projection method, while the C++ Core golden remains green.

**GREEN requirements**

- Bindings follow explicit ref views; they do not recursively decode graphs.
- Identity is the Core `(component_index, object_id)` pair.
- Sharing/cycles are asserted by identity, never host pointer equality.
- Materialization remains one Core call and retains reachable closure.
- Rust has no graph capability absent from Python/TypeScript.

**Broad gates:** all Task 4 gates, all graph C++ tests, the 16-seed corpus, full
Core Wasm graph harness, projection concurrency/disposal tests, and exact
ABI-105.

**Commit:** `feat(bindings): project portable object graphs`

---

## Task 6: Freeze shared cross-language runtime parity and package boundaries

**Purpose:** Convert per-language coverage into one executable proof matrix and
close packaging/lifetime gaps before codegen targets depend on the APIs.

**Expected files**

- Create: `tests/ci/p4_projection_codegen_map.json` with runtime rows initially
- Create: `tests/ci/check_p4_projection_codegen_quality.py` or `.rb`
- Create: tests for that checker
- Add shared fixture drivers/receipts under `tests/golden/payload/v1/parity/`
  only if existing fixtures cannot encode the cross-language observations
- Modify workflows to define Rust and projection parity jobs
- Modify package inventories and language package docs
- Modify Issue 0002 and evidence

**Genuine RED**

The new quality checker must first reject the repository because the exact
runtime projection traceability/package/workflow inventory is absent. Unit
tests must prove it rejects missing/reordered/duplicated evidence and false
hosted claims.

**GREEN requirements**

- One ordered map links each common runtime requirement to C++, Rust, Python,
  and TypeScript proof.
- Independent receipts compare canonical, binary, logical value, error,
  lifetime, and direct/staged results.
- Python 3.10 syntax/import/package, Rust source/system link seams, and
  TypeScript browser-capable Wasm package exports are executable.
- Workflow definitions are labeled definitions until hosted runs exist.
- All codegen rows remain explicitly open.

**Broad gates:** Debug/Release, ASan+UBSan, focused TSan, corpus/fuzzer smoke,
Core Wasm, exact ABI-105, all bindings, generated package inventories, docs,
and quality-checker tests.

**Commit:** `test(bindings): freeze payload projection parity`

---

## Task 7: Implement private Core ArtifactSet and all four generators

**Purpose:** Build codegen once in Core against the now-frozen projection APIs,
without advertising or exposing partial targets.

**Expected files**

- Create: `fastcarto/fastdb/src/payload/codegen/Artifact.hpp`
- Create: `fastcarto/fastdb/src/payload/codegen/Artifact.cpp`
- Create: `fastcarto/fastdb/src/payload/codegen/Identifier.hpp`
- Create: `fastcarto/fastdb/src/payload/codegen/Identifier.cpp`
- Create: `fastcarto/fastdb/src/payload/codegen/Generator.hpp`
- Create: `fastcarto/fastdb/src/payload/codegen/Generator.cpp`
- Create target-specific renderer files only where a real implementation/test
  needs separation
- Create: `tests/cpp/payload/test_codegen.cpp`
- Create generated-code fixtures/receipts under
  `tests/golden/payload/v1/codegen/`
- Modify: `tests/cpp/CMakeLists.txt`, package inventory, Issue 0002, evidence

**Interfaces consumed**

`CompiledSpec`, `ResolvedSpec`, canonical bytes/digest, stable indexes, public
projection API shapes, Core SHA-256, and existing error/result primitives.

**Interfaces produced**

A private immutable `ArtifactSet` and one generator supporting `cpp`, `rust`,
`python`, and `typescript`. No public ABI or manifest capability changes yet.

**Genuine RED**

Add C++ tests requiring deterministic generation for:

- record-all-types;
- nullable recursive lists/components;
- source IDs shaped like language keywords and escape prefixes; and
- a graph with sharing/self/mutual cycles.

Require exact path, kind, provenance, original-ID map, bytes, hashes, order,
limits, repeated-run equality, and forbidden-content absence. Expected RED:
Core has no ArtifactSet/generator.

**GREEN requirements**

- Generator consumes `ResolvedSpec` directly and never reparses JSON/manifest.
- Identifier encoding is byte-total, collision-free, reversible, and covered
  by adversarial IDs.
- Output/path/resource invariants fail closed with stable codegen errors.
- Every target calls only its official runtime and contains no binary/layout
  constants or downstream semantics.
- Manifest target list and public C ABI remain unchanged/empty/105 in Task 7.

**Broad gates:** complete native Debug/Release, sanitizers, resource/allocation
failure tests, repeated deterministic generation under varied environment,
package inventory, schema checks, projection regressions, and exact ABI-105.

**Commit:** `feat(core): generate portable payload projections`

---

## Task 8: Publish the ArtifactSet ABI, C++ RAII, generated-output gates, and manifests

**Purpose:** Make the complete four-target generator public atomically and
freeze the real post-P4 ABI only after generated code executes.

**Expected files**

- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Modify: `fastcarto/fastdb/include/fastdb_payload.hpp`
- Modify: `fastcarto/fastdb/src/payload/abi/Handles.hpp`
- Modify: `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp`
- Modify: `fastcarto/fastdb/src/payload/build/PayloadBuilder.{hpp,cpp}`
- Modify: `fastcarto/fastdb/src/payload/view/View.{hpp,cpp}`
- Modify: `fastcarto/fastdb/src/payload/view/{Materialize,GraphMaterialize}.cpp`
- Modify: `fastcarto/fastdb/src/payload/spec/Manifest.{hpp,cpp}`
- Modify: `schemas/fastdb.payload.manifest.v1.schema.json`
- Regenerate: `fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc`
- Modify: ABI allowlist and ABI checker/tests
- Modify all three non-C++ projections for ArtifactSet/codegen
- Add generated C++/Rust/Python/TypeScript compile/import/type-check/runtime
  harnesses
- Modify parity proof map, workflows, package inventories, README/schema docs,
  Issue 0002, and evidence

**Genuine RED**

Write first:

- pure C output-clearing, prefix, ownership, target/index/error tests;
- pure C and four-language same-index/cross-spec provenance rejection for
  Builder, Payload, and View;
- C++ RAII ArtifactSet tests;
- Rust/Python/TypeScript codegen query tests;
- manifest operation/target capability tests; and
- a harness that asks Core for all four artifacts, writes only to a temporary
  test tree, then compiles/imports/type-checks and executes them.

Expected RED is the missing reviewed twelve-symbol P4 ABI delta and empty
manifest target list.

**GREEN requirements**

- Add exactly the three reviewed Core provenance guards and nine ArtifactSet/
  codegen functions, with no unrelated export.
- Generated entry factories, builder helpers, and component constructors
  reject a same-index handle from a different specification through the same
  Core `DIGEST_MISMATCH` code/path/details in every projection.
- All failures clear every output before validation and publish no partial
  result.
- the known one-artifact inventory is rejected before rendering when
  `max_artifacts < 1`, and `max_total_bytes` is enforced while constructing
  renderer output through a checked sink or equivalent exact preflight;
  post-render rejection alone is insufficient for the public API.
- Result/blob/error ownership survives arbitrary valid retain/release order.
- Generated outputs compile/import/type-check and call the real official
  projection for a runtime smoke.
- Manifest operations/targets, capability bits, schema, embedded bytes, and
  goldens change together only after all four target gates pass.
- Native and Wasm scanners freeze the actual exact allowlist. If it is the
  designed 105+12 set, record 117; otherwise stop and reconcile the design
  before accepting drift.

**Focused gates**

```bash
ctest --test-dir build/p4-task8 -R \
  '^payload\.(codegen|spec_abi|cpp_facade|runtime_cpp_facade|abi_symbols)$' \
  --output-on-failure
cargo test --manifest-path bindings/rust/Cargo.toml --workspace --all-features \
  codegen
uv run pytest tests/python/payload/test_payload_codegen.py -q
npm --prefix ts/fastdb4ts run build
node --test tests/ts/payload_codegen.mjs
python3 tools/check_payload_abi_symbols.py --build-dir build/p4-task8
python3 tools/check_payload_abi_symbols.py --wasm-build-dir build/p4-task8-wasm
```

Then run the entire Task 6 gate stack from fresh build directories.

**Commit:** `feat(core): publish portable payload artifact codegen`

---

## Task 9: P4 robustness, workflow, documentation, and closure freeze

**Purpose:** Close every P4 requirement after the implemented public boundary
exists, repair findings, and leave an auditable clean handoff to P5.

**Expected files**

- Extend parser/codegen robustness corpus only for codegen-owned hostile cases
- Complete the P4 proof map and executable quality checker
- Complete native/Wasm/Rust/Python/TypeScript/package workflow definitions
- Update README, schemas README, Issue 0002, and language package docs
- Create/update ignored P4 closure report and review packages
- No version metadata change

**Genuine RED**

The completed P4 quality checker must initially reject any missing:

- projection language/requirement row;
- generated target or compiler/import proof;
- exact ABI count/allowlist;
- artifact determinism/provenance/hash receipt;
- package/workflow definition;
- local-versus-hosted wording; or
- P5-open limitation.

**Complete local gate**

Use fresh final directories and run at least:

```bash
cmake -S fastcarto -B build/p4-final-debug \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build/p4-final-debug --parallel
ctest --test-dir build/p4-final-debug --output-on-failure

cmake -S fastcarto -B build/p4-final-release \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/p4-final-release --parallel
ctest --test-dir build/p4-final-release --output-on-failure

cmake -S fastcarto -B build/p4-final-sanitize \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF \
  -DFASTDB_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/p4-final-sanitize --parallel
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/p4-final-sanitize --output-on-failure

cargo test --manifest-path bindings/rust/Cargo.toml \
  --workspace --all-features
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
set -o pipefail
uv build --out-dir build/p4-final-dist \
  2>&1 | tee build/p4-final-package.log
npm --prefix ts/fastdb4ts run build:wasm
npm run test:ts

emcmake cmake -S fastcarto -B build/p4-final-wasm \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/p4-final-wasm --parallel --target \
  fastdb_payload_wasm_runtime_harness \
  fastdb_payload_test_c_header_smoke \
  fastdb_payload_test_runtime_cpp_facade \
  fastdb_payload_wasm_runtime_abi_single_thread
node build/p4-final-wasm/wasm-tests/payload_runtime_harness.js
node build/p4-final-wasm/tests/cpp/fastdb_payload_test_c_header_smoke.js
node build/p4-final-wasm/tests/cpp/fastdb_payload_test_runtime_cpp_facade.js

python3 tools/generate_embedded_payload_schemas.py --check
python3 tests/ci/test_check_payload_binary_corpus.py
python3 tools/check_payload_binary_corpus.py --check
python3 tests/ci/test_check_python_package_inventory.py
python3 tools/check_python_package_inventory.py \
  --dist-dir build/p4-final-dist --build-log build/p4-final-package.log
python3 tools/check_payload_abi_symbols.py --build-dir build/p4-final-debug
python3 tools/check_payload_abi_symbols.py --wasm-build-dir build/p4-final-wasm
git diff --check
```

Also run, where the local toolchain supports them:

- focused ThreadSanitizer for runtime and binding-safe native paths;
- repository libFuzzer/corpus smoke with retained log scan;
- pure-C arm64/x86_64/wasm32 header and link smokes;
- full Core wasm32/Node runtime and codegen harness;
- generated C++/Rust/Python/TypeScript execution from a clean temp tree;
- Python 3.10 syntax/import/package checks;
- Markdown relative-link/anchor and JSON/YAML parse checks;
- forbidden Core-duplication/downstream-semantic/type scans; and
- exact clean tracked worktree/index checks after the closure commit.

Record Apple `detect_leaks=0`, adjusted fuzzer linkage, unavailable TSan, or
other tool limits exactly; never upgrade them to stronger evidence.

**Primary-agent closure review**

Review the entire Task 1-9 range separately for:

1. authority/spec/ABI/lifetime/determinism/parity compliance; and
2. correctness, portability, resource safety, cleanup, package quality, and
   maintainability.

Fix every material finding, rerun every affected full gate, and record that the
review is not independent.

**Commit:** `docs: record portable payload P4 closure`

After this commit, stop P4. Verify a clean worktree and begin a separate P5
docs-first inventory/removal plan. Do not combine P5 removals into the P4
closure commit.

---

## P4 completion checklist

- [ ] Frozen P1-P3 semantics and original 105 exports retain their meaning.
- [ ] C++ existing RAII is reused and only codegen delta is added.
- [ ] Rust raw/safe crates have real callers and no binding-owned semantics.
- [ ] Python 3.10 `fastdb4py.payload` calls the native C ABI only.
- [ ] TypeScript/Wasm calls the same Core C ABI with explicit disposal.
- [ ] All projections cover complete record and ordinary graph behavior.
- [ ] Shared canonical/binary/value/error/lifetime/direct-staged parity passes.
- [ ] Core ArtifactSet and all four renderers are deterministic.
- [ ] Generated C++/Rust/Python/TypeScript outputs really execute.
- [ ] Manifest capabilities are truthful.
- [ ] Exact post-P4 native/Wasm ABI is frozen and documented.
- [ ] Package/workflow/docs/robustness gates pass locally or have exact limits.
- [ ] Primary-agent review is clean and accurately labeled non-independent.
- [ ] Issue 0002 records P4 evidence and keeps P5/version/hosted/release open.
- [ ] No version, push, tag, publication, C-Two, or Toodle change occurred.
