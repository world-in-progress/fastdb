# FastDB Portable Payload P5 Clean-Cut Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to implement this plan task-by-task. The user
> selected inline execution by the context-owning primary agent; do not
> delegate implementation or review and do not claim independent review.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove every obsolete binding-owned portable authority, rename the
standalone AoS engine to `RecordEngine`, close the native determinism and SWIG
diagnostic debt, and prove FastDB's local clean-cut/package readiness without
changing ABI-117 or publishing a release.

**Architecture:** The frozen C++ Core and stable ABI-117 remain the only
portable specification, identity, binary, runtime, lifetime, and generator
authority. Python and TypeScript retain only mechanical payload projections
plus explicitly standalone storage/serializer APIs. The generic Python CLI
emits an immutable Core `ArtifactSet` into a new destination tree; downstream
contract and project composition remain outside FastDB.

**Tech Stack:** C++17/CMake/CTest, stable C ABI, SWIG, Python 3.10+/pytest/uv,
TypeScript/Wasm/Node, Rust/Cargo, standard-library Python quality gates,
GitHub Actions YAML, Markdown governance records.

**Governing design:**
[2026-07-23-portable-payload-clean-cut-design.md](../specs/2026-07-23-portable-payload-clean-cut-design.md)

**Starting point:** `9d86c171eda1fe107c3519ce040ca2ec417167f9`
on `socu/portable-payload-foundation`.

## Global Constraints

- Preserve exactly 117 sorted public `fdb_payload_v1_*` native and Wasm
  symbols and every P1-P4 semantic meaning.
- Do not add a binding-side parser, canonicalizer, digest, layout, binary,
  graph runtime, profile converter, renderer, or compatibility path.
- Delete obsolete 0.x surfaces cleanly; no alias, fallback module, deprecation
  shim, pickle name, or dual behavior.
- Keep the package versions exactly `fastdb4py==0.1.22` and
  `fastdb4ts==0.0.3`.
- Keep Python syntax compatible with Python 3.10.
- Keep `str` and `wstr` as the native text types; never add `text`.
- Retain `postToBuffer`, `FixedBufferWriteStream`, standalone table/view
  helpers, feature metadata, `ObjectEngine`, and `FastSerializer`.
- Do not modify C-Two or Toodle and do not add CRM, route, relay, transport,
  lease, policy, GIS, or consumer-owned composition behavior.
- Do not push, tag, publish, release, or represent workflow definitions as
  hosted results.
- Every task starts with `.superpowers/sdd/p5-task-<n>-brief.md`, records its
  genuine RED, updates `.superpowers/sdd/progress.md`, ends with
  `.superpowers/sdd/p5-task-<n>-report.md`, and commits only its allowed files.
- After each task commit, freeze the exact predecessor-to-commit diff and run
  two primary-agent passes: specification/authority first, then
  correctness/resource safety/portability/package quality. Fix every material
  finding and rerun affected gates before moving on.
- Build directories and package artifacts are disposable ignored evidence.
  Remove each completed task's unused build output after its report records
  commands and results so the low-space workstation retains only the build
  currently needed by the next gate.

---

## File and Ownership Map

### Portable authority retained unchanged

- `fastcarto/fastdb/include/fastdb_payload.h`
- `fastcarto/fastdb/include/fastdb_payload.hpp`
- `fastcarto/fastdb/src/payload/`
- `bindings/rust/fastdb-sys/`
- `bindings/rust/fastdb/`
- `python/fastdb4py/payload/`
- `ts/fastdb4ts/src/payload/`

These files are test inputs. P5 may update package/gate references around them,
but a required portable semantic edit stops P5 and reopens the owning P1-P4
slice explicitly.

### Python clean cut

- Replace: `python/fastdb4py/cli.py`
- Delete: `python/fastdb4py/codegen/__init__.py`
- Delete: `python/fastdb4py/codegen/ts_gen.py`
- Delete: `python/fastdb4py/call_db.py`
- Delete: `python/fastdb4py/schema.py`
- Delete: `python/fastdb4py/require.py`
- Delete: `python/fastdb4py/allocator.py`
- Modify: `python/fastdb4py/type.py`
- Modify: `python/fastdb4py/__init__.py`
- Rename: `python/fastdb4py/column_engine.py` to
  `python/fastdb4py/record_engine.py`
- Modify package tests/checker:
  `tests/python/test_import_boundary.py`,
  `tools/check_python_package_inventory.py`, and
  `tests/ci/test_check_python_package_inventory.py`

### TypeScript clean cut

- Delete: `ts/fastdb4ts/src/call-db.ts`
- Delete: `tests/ts/test_call_db_runtime.mjs`
- Modify: `ts/fastdb4ts/src/index.ts`
- Modify: `tests/ts/test_public_exports.mjs`
- Modify: `tests/ci/check_ts_payload_package.py`
- Modify: `tests/ci/test_check_ts_payload_package.py`

### Native standalone repair and obsolete wrapper removal

- Modify: `fastcarto/fastdb/include/fastdb.h`
- Modify: `fastcarto/fastdb/src/FastVectorDbBuild_p.h`
- Modify: `fastcarto/fastdb/src/FastVectorDbBuild.cpp`
- Modify: `fastcarto/fastdb/src/FastVectorDbLayerBuild.cpp`
- Modify: `fastcarto/fastdb/swig/fastdb4py.i`
- Create: `tests/cpp/test_legacy_descriptor_determinism.cpp`
- Modify: `tests/cpp/CMakeLists.txt`

### Standalone rename consumers

- Rename: `tests/python/test_column_engine.py` to
  `tests/python/test_record_engine.py`
- Modify:
  `python/fastdb4py/layout.py`,
  `tests/python/test_column_way.py`,
  `tests/python/test_free_threading.py`,
  `tests/python/test_materialize.py`,
  `tests/python/test_string_column.py`,
  `tests/python/test_view_owner_lifetime.py`,
  `tests/python/benchmark_comprehensive.py`,
  `tests/python/benchmark_kostya.py`,
  `tests/python/benchmark_kostya_orm2.py`, and
  `tests/python/benchmark_native_list.py`

### Boundary documentation and executable policy

- Create: `tools/check_p5_clean_cut.py`
- Create: `tests/ci/test_check_p5_clean_cut.py`
- Create: `tests/ci/p5_clean_cut_policy.json`
- Modify: `.github/workflows/tests.yml`
- Modify: `README.md`
- Modify: `python/README.md`
- Modify: `ts/fastdb4ts/README.md`
- Modify: `AGENTS.md`
- Modify: `.github/copilot-instructions.md`
- Modify: `CHANGELOG.md`
- Modify current negative tests:
  `tests/cpp/payload/test_spec_parse.cpp` and
  `tests/cpp/payload/test_codegen.cpp`
- Modify current negative import tests:
  `tests/python/test_public_surface.py` and
  `tests/python/test_import_boundary.py`
- Mark historical/superseded:
  `docs/opt/batch-array-call-db-fast-path-design.md`,
  `docs/vision/neutral-allocator-destructive-update.md`,
  `docs/superpowers/plans/2026-04-21-columnengine-string-column.md`,
  `docs/superpowers/plans/2026-05-18-c-two-call-db-codec.md`,
  `docs/superpowers/plans/2026-05-22-fdb-view-owner-lifetime.md`,
  `docs/superpowers/plans/2026-05-27-require-envelope-neutral-allocator.md`,
  `docs/superpowers/specs/2026-04-20-unified-feature-engine-design.md`,
  `docs/superpowers/specs/2026-04-21-columnengine-string-column-design.md`,
  `docs/superpowers/specs/2026-04-21-truncate-str-fill-unification-design.md`,
  `docs/superpowers/specs/2026-04-22-truncate-string-ingest-optimization-design.md`,
  `ts/README.md`, and
  `ts/analysis/QUALITY_AUDIT.md`
- Update status throughout:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`
  and `docs/issues/0003-legacy-swig-diagnostics.md`

---

### Task 0: Freeze the P5 Design, Inventory, and Executable Plan

**Files:**

- Create:
  `docs/superpowers/specs/2026-07-23-portable-payload-clean-cut-design.md`
- Create:
  `docs/superpowers/plans/2026-07-23-portable-payload-clean-cut.md`
- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`
- Modify: `docs/issues/0003-legacy-swig-diagnostics.md`
- Create/update ignored:
  `.superpowers/sdd/p5-task-0-brief.md`,
  `.superpowers/sdd/p5-task-0-report.md`, and
  `.superpowers/sdd/progress.md`

**Interfaces:**

- Consumes: accepted foundation design, ADR-0001, frozen ABI-117/P4 evidence,
  and the live source/package/document inventory at `9d86c17`.
- Produces: one complete P5 target contract and this task-by-task execution
  contract; no runtime interface.

- [ ] **Step 1: Retain the genuine document RED**

Run before either document exists:

```bash
test -f docs/superpowers/specs/2026-07-23-portable-payload-clean-cut-design.md \
  && test -f docs/superpowers/plans/2026-07-23-portable-payload-clean-cut.md
```

Expected: exit `1` because both new authority documents were absent.

- [ ] **Step 2: Finish and self-review the design and plan**

Require explicit dispositions for the Python, TypeScript, native/SWIG,
standalone, package, documentation, historical, and downstream-owned
surfaces. Run:

```bash
rg -n 'T[O]DO|T[B]D|F[I]XME|place[h]older|to be deci[d]ed|open quest[i]on|later deci[d]e' \
  docs/superpowers/specs/2026-07-23-portable-payload-clean-cut-design.md \
  docs/superpowers/plans/2026-07-23-portable-payload-clean-cut.md
test $? -eq 1
git diff --check
```

Expected: `rg` exits `1` with no matches; `git diff --check` exits `0`.

- [ ] **Step 3: Record the accurate docs-only issue state**

Append a P5 Task 0 section to Issue 0002 containing:

```text
start = 9d86c171eda1fe107c3519ce040ca2ec417167f9
portable ABI = 117 frozen symbols
implementation = not started
package versions = 0.1.22 / 0.0.3 unchanged
hosted, push, tag, publication, release, C-Two composition = pending
```

Add the design and plan links. In Issue 0003 record the chosen closure:
exclude native-only nested tile declarations from SWIG parsing, make
`utf8_view_t::data` non-settable in SWIG, and change the package gate to reject
every SWIG diagnostic. Keep Issue 0003 `Open`.

- [ ] **Step 4: Run focused and retained quality gates**

```bash
test -f docs/superpowers/specs/2026-07-23-portable-payload-clean-cut-design.md \
  && test -f docs/superpowers/plans/2026-07-23-portable-payload-clean-cut.md
python3 tests/ci/test_check_p4_projection_codegen_quality.py
python3 tests/ci/check_p4_projection_codegen_quality.py --check
ruby tests/ci/test_check_p3_runtime_quality.rb
ruby tests/ci/check_p3_runtime_quality.rb --check-repository
git diff --check
```

Expected: every command exits `0`.

- [ ] **Step 5: Commit and run the two-pass frozen review**

```bash
git add \
  docs/superpowers/specs/2026-07-23-portable-payload-clean-cut-design.md \
  docs/superpowers/plans/2026-07-23-portable-payload-clean-cut.md \
  docs/issues/0002-portable-payload-foundation-implementation-status.md \
  docs/issues/0003-legacy-swig-diagnostics.md
git diff --cached --check
git commit -m "docs: design portable payload clean cut"
```

Freeze `9d86c17..<task-0-commit>` and review design/spec coverage first,
documentation correctness second. Record exact findings and rerun Step 4 after
the last correction.

---

### Task 1: Replace Python-Owned Codegen with the Core Artifact CLI

**Files:**

- Modify: `python/fastdb4py/cli.py`
- Delete: `python/fastdb4py/codegen/__init__.py`
- Delete: `python/fastdb4py/codegen/ts_gen.py`
- Delete: `tests/python/test_codegen.py`
- Create: `tests/python/test_cli_codegen.py`
- Modify: `tests/python/test_import_boundary.py`
- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes:
  `CompiledSpec.compile(bytes)`,
  `CompiledSpec.generate(CodegenTarget)`,
  `Artifact(relative_path, kind, bytes, sha256)`, and
  `ArtifactKind.SOURCE`.
- Produces:
  `fdb codegen SPEC.json --target cpp|rust|python|typescript --output DIR`,
  `_validated_artifacts(ArtifactSet) -> tuple[Artifact, ...]`, and
  `_write_new_tree(Path, tuple[Artifact, ...]) -> None`.

- [ ] **Step 1: Write the CLI replacement tests**

Add tests that compare every emitted file byte-for-byte with the same Core
`ArtifactSet`, reject an existing output root, reject absolute/backslash/
dot-dot/duplicate paths before writing, remove a CLI-created staging tree on a
write failure, preserve Core error fields in stderr, and prove old feature
discovery flags are absent. The end-to-end test begins with:

```python
def test_codegen_cli_emits_the_exact_core_artifact(tmp_path):
    source = (
        b'{"schema":"fastdb.payload.v1","profile":"record.v1",'
        b'"entries":[],"components":[]}'
    )
    spec_path = tmp_path / "spec.json"
    output = tmp_path / "generated"
    spec_path.write_bytes(source)
    completed = subprocess.run(
        [
            sys.executable, "-m", "fastdb4py.cli", "codegen",
            str(spec_path), "--target", "python", "--output", str(output),
        ],
        check=False, capture_output=True, text=True,
    )
    assert completed.returncode == 0, completed.stderr
    with CompiledSpec.compile(source) as spec:
        with spec.generate(CodegenTarget.PYTHON) as generated:
            expected = generated.artifact(0)
    assert (output / expected.relative_path).read_bytes() == expected.bytes
```

- [ ] **Step 2: Run the genuine CLI RED**

```bash
uv run pytest tests/python/test_cli_codegen.py \
  tests/python/test_import_boundary.py -q
```

Expected: the new syntax fails because the old CLI requires `--ts` and
feature-directory discovery.

- [ ] **Step 3: Implement the Core-only facade and delete the old generator**

Use this target map and no feature imports:

```python
TARGETS = {
    "cpp": CodegenTarget.CPP,
    "rust": CodegenTarget.RUST,
    "python": CodegenTarget.PYTHON,
    "typescript": CodegenTarget.TYPESCRIPT,
}
```

Read `SPEC.json` with `Path.read_bytes()`, compile once, generate once, and
copy all artifacts into immutable Python values before filesystem mutation.
For each `relative_path`, require:

```python
path = PurePosixPath(relative_path)
valid = (
    bool(relative_path)
    and "\\" not in relative_path
    and not path.is_absolute()
    and ".." not in path.parts
    and path.as_posix() == relative_path
    and relative_path != "."
)
```

Require `ArtifactKind.SOURCE`, unique paths, and a nonexistent output root.
Create a private sibling with `tempfile.mkdtemp`, write exact bytes beneath
that sibling, and finish with `staging.replace(output)`. On failure,
`shutil.rmtree(staging)` and never remove caller-owned paths. Format
`PayloadError` using its exact `code`, `symbol`, `path`, `message`, and
`details_json`; filesystem errors remain unnumbered CLI errors.

Delete the old `codegen` package and its discovery tests in the same change.

- [ ] **Step 4: Run focused and broad Python gates**

```bash
uv run pytest tests/python/test_cli_codegen.py \
  tests/python/test_import_boundary.py \
  tests/python/payload/test_payload_codegen.py -q
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
uv build --out-dir build/p5-task1-dist
python3 tools/check_payload_abi_symbols.py --build-dir build/p5-task1-native
git diff --check
```

Configure/build `build/p5-task1-native` before the ABI command:

```bash
cmake -S fastcarto -B build/p5-task1-native \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build/p5-task1-native --parallel
```

Expected: tests/build pass and ABI remains exactly 117.

- [ ] **Step 5: Record, commit, freeze, and review**

Record that CLI destination-tree ownership is intentionally narrow and that
project composition remains downstream. Then:

```bash
git add -A -- \
  python/fastdb4py/cli.py \
  python/fastdb4py/codegen/__init__.py \
  python/fastdb4py/codegen/ts_gen.py \
  tests/python/test_codegen.py \
  tests/python/test_cli_codegen.py \
  tests/python/test_import_boundary.py \
  docs/issues/0002-portable-payload-foundation-implementation-status.md
git diff --cached --check
git commit -m "feat(cli): emit Core-owned payload artifacts"
```

Review Core-only delegation/path safety first, failure cleanup/error fidelity
second. Rerun Step 4 after fixes.

---

### Task 2: Remove Python Call-DB, Schema, Requirement, and Allocator Authority

**Files:**

- Delete: `python/fastdb4py/call_db.py`
- Delete: `python/fastdb4py/schema.py`
- Delete: `python/fastdb4py/require.py`
- Delete: `python/fastdb4py/allocator.py`
- Modify: `python/fastdb4py/type.py`
- Modify: `python/fastdb4py/__init__.py`
- Delete: `tests/python/test_call_db_runtime.py`
- Delete: `tests/python/test_require_envelope.py`
- Delete: `tests/python/test_schema_unified.py`
- Create: `tests/python/test_public_surface.py`
- Modify: `tests/python/test_import_boundary.py`
- Modify: `tools/check_python_package_inventory.py`
- Modify: `tests/ci/test_check_python_package_inventory.py`
- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: the frozen `fastdb4py.payload` projection and existing standalone
  registry/layout/object/table/view/serializer modules.
- Produces: an exact standalone top-level `__all__`; at this ordered
  intermediate commit the existing AoS engine is the sole scheduled old name,
  and Task 5 replaces that class/module atomically with `RecordEngine`.
  Removed authority modules fail with `ModuleNotFoundError`.

- [ ] **Step 1: Write exact public-surface and package-inventory tests**

Use this exact Task 2 top-level set:

```python
EXPECTED_TASK2 = {
    "feature", "is_feature", "get_schema", "lookup_class",
    "Layout", "ColumnEngine", "ObjectEngine", "Table",
    "StringColumn", "pack_utf8_column",
    "FdbViewOwner", "FdbViewInvalidatedError", "FdbViewWriteError",
    "invalidate", "materialize", "FastSerializer",
    "BOOL", "U8", "U16", "U32", "I32", "U8N", "U16N",
    "F32", "F64", "STR", "WSTR", "REF", "BYTES",
}
```

Task 5 changes this one expected name from `ColumnEngine` to `RecordEngine`
and proves the old module is absent. Independently assert that importing each
of:

```python
REMOVED_MODULES = (
    "fastdb4py.call_db",
    "fastdb4py.schema",
    "fastdb4py.require",
    "fastdb4py.allocator",
    "fastdb4py.codegen",
)
```

fails, and that the sdist/wheel forbidden inventory includes those exact
module paths.

- [ ] **Step 2: Run the genuine removal RED**

```bash
uv run pytest tests/python/test_public_surface.py \
  tests/ci/test_check_python_package_inventory.py -q
```

Expected: removed modules and old top-level attributes are still present.

- [ ] **Step 3: Delete duplicate authority and narrow `type.py`**

Delete the four modules and their authority-only tests. Remove
`BatchRequirement`, `ArrayRequirement`, `batch`, `array`, `_validate_requirement_rows`,
`Array`, and `Batch` from `type.py`; retain `OriginFieldType`,
`OriginFieldDefinition`, native aliases, and the type-introspection functions
used by standalone feature/storage code.

Replace top-level imports/`__all__` with only retained standalone names. Do not
import `fastdb4py.payload` at top level and do not expose a second portable
facade.

Change the package checker from implicit recursive acceptance to explicit
forbidden members for:

```text
fastdb4py/call_db.py
fastdb4py/schema.py
fastdb4py/require.py
fastdb4py/allocator.py
fastdb4py/codegen/__init__.py
fastdb4py/codegen/ts_gen.py
```

Make the sdist equivalents under `python/` forbidden too.

- [ ] **Step 4: Prove replacement and retained standalone behavior**

```bash
uv run pytest tests/python/test_public_surface.py \
  tests/python/test_import_boundary.py \
  tests/python/payload \
  tests/python/test_decorator.py \
  tests/python/test_object_engine.py \
  tests/python/test_view_owner_lifetime.py \
  tests/python/test_materialize.py \
  tests/python/test_fast_serializer.py -q
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
python3 tests/ci/test_check_python_package_inventory.py
uv build --out-dir build/p5-task2-dist 2>&1 | tee build/p5-task2-build.log
python3 tools/check_python_package_inventory.py \
  --dist-dir build/p5-task2-dist \
  --build-log build/p5-task2-build.log
git diff --check
```

Expected: retained behaviors pass, removed imports fail, package inventories
exclude every removed module, and the package version remains 0.1.22. The
seven SWIG warnings remain governed until Task 4.

- [ ] **Step 5: Record, commit, freeze, and review**

```bash
git add -A -- \
  python/fastdb4py/call_db.py \
  python/fastdb4py/schema.py \
  python/fastdb4py/require.py \
  python/fastdb4py/allocator.py \
  python/fastdb4py/type.py \
  python/fastdb4py/__init__.py \
  tests/python/test_call_db_runtime.py \
  tests/python/test_require_envelope.py \
  tests/python/test_schema_unified.py \
  tests/python/test_public_surface.py \
  tests/python/test_import_boundary.py \
  tools/check_python_package_inventory.py \
  tests/ci/test_check_python_package_inventory.py \
  docs/issues/0002-portable-payload-foundation-implementation-status.md
git diff --cached --check
git commit -m "refactor(python): remove duplicate payload authority"
```

Review for clean-cut completeness and preserved standalone exports first,
then import/package/Python-3.10 correctness. Rerun Step 4 after fixes.

---

### Task 3: Remove the TypeScript Call-DB Runtime and Package Surface

**Files:**

- Delete: `ts/fastdb4ts/src/call-db.ts`
- Delete: `tests/ts/test_call_db_runtime.mjs`
- Modify: `ts/fastdb4ts/src/index.ts`
- Modify: `tests/ts/test_public_exports.mjs`
- Modify: `tests/ci/check_ts_payload_package.py`
- Modify: `tests/ci/test_check_ts_payload_package.py`
- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: the retained root feature/ORM/table/serializer API and official
  `fastdb4ts/payload` Wasm projection.
- Produces: a root package with no call-db value/type export or generated
  `dist/call-db.*` member.

- [ ] **Step 1: Invert the public and packed-package tests**

Replace the old positive export assertions with:

```javascript
test('fastdb4ts root contains no removed call-db authority', () => {
  for (const name of [
    'encodeFastdbCallDb',
    'decodeFastdbCallDb',
    'viewFastdbCallDb',
    'encodeFastdbFeature',
    'decodeFastdbFeature',
  ]) {
    assert.equal(name in fastdb, false);
  }
  assert.equal(typeof fastdb.Feature, 'function');
  assert.equal(typeof fastdb.ORM, 'function');
  assert.equal(typeof fastdb.FastSerializer, 'function');
});
```

Add `dist/call-db.js` and `dist/call-db.d.ts` to the packed-package forbidden
set while retaining all `dist/payload/*` requirements.

- [ ] **Step 2: Run the genuine TypeScript RED**

```bash
npm --prefix ts/fastdb4ts run build
node --test tests/ts/test_public_exports.mjs
python3 tests/ci/test_check_ts_payload_package.py
```

Expected: call-db exports and packed files still exist.

- [ ] **Step 3: Delete the runtime and root exports**

Delete `call-db.ts` and its runtime test. Remove every value and type re-export
from `src/index.ts`. Do not move call-db logic into `schema.ts`, `feature.ts`,
`orm.ts`, or `serializer.ts`; those remain standalone-only.

- [ ] **Step 4: Run TypeScript/Wasm and package gates**

```bash
npm --prefix ts/fastdb4ts run build:wasm
npm run test:ts
python3 tests/ci/test_check_ts_payload_package.py
rm -rf build/p5-task3-package
cmake -E make_directory build/p5-task3-package
npm pack ./ts/fastdb4ts --pack-destination build/p5-task3-package
python3 tests/ci/check_ts_payload_package.py \
  --package-dir build/p5-task3-package
python3 tools/check_payload_abi_symbols.py \
  --wasm-build-dir ts/build-wasm
git diff --check
```

Expected: root standalone tests and all portable Wasm tests pass, packed
call-db files are absent, and Wasm ABI remains exactly 117.

- [ ] **Step 5: Record, commit, freeze, and review**

```bash
git add -A -- \
  ts/fastdb4ts/src/call-db.ts \
  ts/fastdb4ts/src/index.ts \
  tests/ts/test_call_db_runtime.mjs \
  tests/ts/test_public_exports.mjs \
  tests/ci/check_ts_payload_package.py \
  tests/ci/test_check_ts_payload_package.py \
  docs/issues/0002-portable-payload-foundation-implementation-status.md
git diff --cached --check
git commit -m "refactor(ts): remove duplicate payload authority"
```

Review clean package boundaries first, then Wasm initialization/disposal and
standalone-root regressions. Rerun Step 4 after fixes.

---

### Task 4: Remove Orphaned Native Allocators, Repair Descriptor Bytes, and Close SWIG Warnings

**Files:**

- Modify: `fastcarto/fastdb/include/fastdb.h`
- Modify: `fastcarto/fastdb/src/FastVectorDbBuild_p.h`
- Modify: `fastcarto/fastdb/src/FastVectorDbBuild.cpp`
- Modify: `fastcarto/fastdb/src/FastVectorDbLayerBuild.cpp`
- Modify: `fastcarto/fastdb/swig/fastdb4py.i`
- Create: `tests/cpp/test_legacy_descriptor_determinism.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `tests/python/test_column_engine.py`
- Modify: `tools/check_python_package_inventory.py`
- Modify: `tests/ci/test_check_python_package_inventory.py`
- Modify: `docs/issues/0003-legacy-swig-diagnostics.md`
- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: legacy standalone `FastVectorDbBuild::postToBuffer` and the frozen
  portable generic backing API.
- Produces: deterministic zero-initialized standalone descriptors and a
  warning-free SWIG/package contract; no new public API.

- [ ] **Step 1: Add deterministic descriptor and zero-warning tests**

The C++ regression builds one non-list `ftU8` layer repeatedly, asserts exact
byte equality, loads the result and reads value `7`, and reads the persisted
descriptor with `memcpy` to avoid alignment UB:

```cpp
wx::field_desc_ex_t descriptor{};
std::memcpy(&descriptor, bytes.data() + 20 + sizeof(wx::layer_header_t),
            sizeof(descriptor));
require(descriptor.type == wx::ftU8);
require(descriptor.element_type == 0);
```

The test build helper uses:

```cpp
wx::FastVectorDbBuild build;
build.begin("{}");
build.createLayerBegin("values");
build.addField("value", wx::ftU8);
build.addFeatureBegin();
build.setField(0, 7);
build.addFeatureEnd();
build.createLayerEnd();
std::vector<unsigned char> bytes(build.byteLength());
require(build.postToBuffer(bytes.data(), bytes.size()) == bytes.size());
```

Register the owner-layer test explicitly:

```cmake
add_executable(
    fastdb_legacy_descriptor_determinism
    test_legacy_descriptor_determinism.cpp
)
target_include_directories(
    fastdb_legacy_descriptor_determinism
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
        ${CMAKE_CURRENT_SOURCE_DIR}/payload
)
target_link_libraries(fastdb_legacy_descriptor_determinism PRIVATE fastdb)
fastdb_apply_sanitizers(fastdb_legacy_descriptor_determinism)
add_test(
    NAME legacy.descriptor_determinism
    COMMAND fastdb_legacy_descriptor_determinism
)
```

Change `check_swig_diagnostics("")` to pass and make any matched SWIG warning,
including each of the former seven lines, raise `CheckError`.

- [ ] **Step 2: Run deterministic and package genuine REDs**

Configure the owner library with deterministic uninitialized-byte poisoning:

```bash
cmake -S fastcarto -B build/p5-task4-red \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS=-ftrivial-auto-var-init=pattern
cmake --build build/p5-task4-red --parallel
ctest --test-dir build/p5-task4-red \
  -R legacy.descriptor_determinism --output-on-failure
```

Expected before the fix: the persisted non-list `element_type` contains the
compiler poison rather than zero.

Then run:

```bash
python3 tests/ci/test_check_python_package_inventory.py
uv build --out-dir build/p5-task4-red-dist \
  2>&1 | tee build/p5-task4-red-build.log
python3 tools/check_python_package_inventory.py \
  --dist-dir build/p5-task4-red-dist \
  --build-log build/p5-task4-red-build.log
```

Expected: the package checker fails on the existing seven SWIG warnings.

- [ ] **Step 3: Repair the owner and remove call-db-only native classes**

Change the descriptor declaration to:

```cpp
field_desc_ex_t fd{};
```

Delete `ScratchAllocation`, `ScratchAllocator`, heap scratch implementations,
`FinalBackingAllocation`, `FinalBackingResource`, heap final-backing
implementations, and `FastVectorDbBuild::postToFinalBacking` from the public
header, private header, C++ implementation, SWIG renames/ownership/extensions,
and Python tests. Retain `postToBuffer` and `FixedBufferWriteStream`.

Wrap the native-only `TileBoxTake` and `FastVectorTileDb` declarations from
SWIG parsing with `#ifndef SWIG` without deleting their C++ definitions. Add:

```swig
%ignore wx::utf8_view_t::data;
```

before `%include "fastdb.h"` so the custom sequence bridge can construct
`utf8_view_t` internally but Python receives no owner-ambiguous setter.

- [ ] **Step 4: Run focused native, sanitizer, Python, and package gates**

```bash
cmake -S fastcarto -B build/p5-task4-debug \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build/p5-task4-debug --parallel
ctest --test-dir build/p5-task4-debug --output-on-failure
python3 tools/check_payload_abi_symbols.py --build-dir build/p5-task4-debug

cmake -S fastcarto -B build/p5-task4-sanitize \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_ENABLE_SANITIZERS=ON
cmake --build build/p5-task4-sanitize --parallel
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/p5-task4-sanitize --output-on-failure

uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
python3 tests/ci/test_check_python_package_inventory.py
uv build --out-dir build/p5-task4-dist \
  2>&1 | tee build/p5-task4-build.log
python3 tools/check_python_package_inventory.py \
  --dist-dir build/p5-task4-dist \
  --build-log build/p5-task4-build.log
git diff --check
```

Expected: native/Python/package gates pass, ABI is exactly 117, build log has
zero SWIG warnings, and the wheel still contains exactly one extension, one
FastDB library, and one binding library.

- [ ] **Step 5: Close Issue 0003, commit, freeze, and review**

Set Issue 0003 to `Closed` only after Step 4 records the warning-free clean
build. Record that native tile APIs remain available and that
`utf8_view_t::data` is intentionally not settable from Python.

```bash
git add -A -- \
  fastcarto/fastdb/include/fastdb.h \
  fastcarto/fastdb/src/FastVectorDbBuild_p.h \
  fastcarto/fastdb/src/FastVectorDbBuild.cpp \
  fastcarto/fastdb/src/FastVectorDbLayerBuild.cpp \
  fastcarto/fastdb/swig/fastdb4py.i \
  tests/cpp/test_legacy_descriptor_determinism.cpp \
  tests/cpp/CMakeLists.txt \
  tests/python/test_column_engine.py \
  tools/check_python_package_inventory.py \
  tests/ci/test_check_python_package_inventory.py \
  docs/issues/0002-portable-payload-foundation-implementation-status.md \
  docs/issues/0003-legacy-swig-diagnostics.md
git diff --cached --check
git commit -m "fix(native): clean legacy backing and descriptor state"
```

Review ABI/backing ownership/deterministic wire state first, then
memory/resource/SWIG/package portability. Rerun Step 4 after fixes.

---

### Task 5: Rename the Standalone AoS Engine to `RecordEngine`

**Files:**

- Rename: `python/fastdb4py/column_engine.py` to
  `python/fastdb4py/record_engine.py`
- Rename: `tests/python/test_column_engine.py` to
  `tests/python/test_record_engine.py`
- Modify: `python/fastdb4py/__init__.py`
- Modify: `python/fastdb4py/layout.py`
- Modify: `tests/python/test_public_surface.py`
- Modify: `tests/python/test_column_way.py`
- Modify: `tests/python/test_free_threading.py`
- Modify: `tests/python/test_materialize.py`
- Modify: `tests/python/test_string_column.py`
- Modify: `tests/python/test_view_owner_lifetime.py`
- Modify: `tests/python/benchmark_comprehensive.py`
- Modify: `tests/python/benchmark_kostya.py`
- Modify: `tests/python/benchmark_kostya_orm2.py`
- Modify: `tests/python/benchmark_native_list.py`
- Modify: `README.md`
- Modify: `python/README.md`
- Modify: `AGENTS.md`
- Modify: `.github/copilot-instructions.md`
- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: the existing AoS engine implementation and standalone tests.
- Produces: `fastdb4py.RecordEngine` and
  `fastdb4py.record_engine.RecordEngine`; the old name/module are absent.

- [ ] **Step 1: Change the public test to require the final name**

Require:

```python
assert fastdb4py.RecordEngine.__module__ == "fastdb4py.record_engine"
assert "RecordEngine" in fastdb4py.__all__
assert not hasattr(fastdb4py, "ColumnEngine")
with pytest.raises(ModuleNotFoundError):
    importlib.import_module("fastdb4py.column_engine")
```

Retain create/truncate/load/save/shared-memory/string/list/view-owner behavior
tests; only rename their engine reference.

- [ ] **Step 2: Run the genuine rename RED**

```bash
uv run pytest tests/python/test_public_surface.py -q
```

Expected: `RecordEngine` is absent and the old module imports.

- [ ] **Step 3: Perform the clean file/class/import rename**

Use `git mv` for both source and primary test. Rename the class, imports,
annotations, runtime type checks, messages, docstrings, tests, examples, and
benchmarks. Do not rename legitimate `column`, `StringColumn`, `BytesColumn`,
`StridedColumn`, column accessor, or field-oriented operations.

Do not add:

```text
ColumnEngine = RecordEngine
sys.modules["fastdb4py.column_engine"]
__getattr__("ColumnEngine")
pickle compatibility registration
```

- [ ] **Step 4: Run the complete standalone and package proof**

```bash
uv run pytest tests/python/test_public_surface.py \
  tests/python/test_record_engine.py \
  tests/python/test_column_way.py \
  tests/python/test_string_column.py \
  tests/python/test_materialize.py \
  tests/python/test_view_owner_lifetime.py \
  tests/python/test_shared_memory.py \
  tests/python/test_truncate_block.py -q
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
uv build --out-dir build/p5-task5-dist \
  2>&1 | tee build/p5-task5-build.log
python3 tools/check_python_package_inventory.py \
  --dist-dir build/p5-task5-dist \
  --build-log build/p5-task5-build.log
rg -n '\bColumnEngine\b|fastdb4py\.column_engine|column_engine\.py' \
  python/fastdb4py tests/python README.md python/README.md \
  AGENTS.md .github/copilot-instructions.md
test $? -eq 1
git diff --check
```

Expected: all tests/package checks pass; `rg` exits `1` with no match.

- [ ] **Step 5: Record, commit, freeze, and review**

```bash
git add -A -- \
  python/fastdb4py/column_engine.py \
  python/fastdb4py/record_engine.py \
  python/fastdb4py/__init__.py \
  python/fastdb4py/layout.py \
  tests/python/test_column_engine.py \
  tests/python/test_record_engine.py \
  tests/python/test_public_surface.py \
  tests/python/test_column_way.py \
  tests/python/test_free_threading.py \
  tests/python/test_materialize.py \
  tests/python/test_string_column.py \
  tests/python/test_view_owner_lifetime.py \
  tests/python/benchmark_comprehensive.py \
  tests/python/benchmark_kostya.py \
  tests/python/benchmark_kostya_orm2.py \
  tests/python/benchmark_native_list.py \
  README.md python/README.md AGENTS.md \
  .github/copilot-instructions.md \
  docs/issues/0002-portable-payload-foundation-implementation-status.md
git diff --cached --check
git commit -m "refactor(python): rename AoS engine to RecordEngine"
```

Review no-alias clean cut and naming truth first, then preserved behavior,
pickle/import/package regressions, and documentation accuracy. Rerun Step 4
after fixes.

---

### Task 6: Make Standalone Boundaries and P5 Quality Policy Executable

**Files:**

- Create: `tools/check_p5_clean_cut.py`
- Create: `tests/ci/test_check_p5_clean_cut.py`
- Create: `tests/ci/p5_clean_cut_policy.json`
- Modify: `.github/workflows/tests.yml`
- Modify: `python/fastdb4py/object_engine.py`
- Modify: `python/fastdb4py/registry.py`
- Modify: `python/fastdb4py/serializer.py`
- Modify: `python/fastdb4py/materialize.py`
- Modify: `python/fastdb4py/view_owner.py`
- Modify: `ts/fastdb4ts/src/schema.ts`
- Modify: `ts/fastdb4ts/src/feature.ts`
- Modify: `ts/fastdb4ts/src/serializer.ts`
- Modify: `README.md`
- Modify: `python/README.md`
- Modify: `ts/fastdb4ts/README.md`
- Modify: `AGENTS.md`
- Modify: `.github/copilot-instructions.md`
- Modify: `CHANGELOG.md`
- Modify: `tests/cpp/payload/test_spec_parse.cpp`
- Modify: `tests/cpp/payload/test_codegen.cpp`
- Modify: `tests/python/test_public_surface.py`
- Modify: `tests/python/test_import_boundary.py`
- Modify: `tools/check_python_package_inventory.py`
- Modify: `tests/ci/test_check_python_package_inventory.py`
- Modify: `tests/ci/check_ts_payload_package.py`
- Modify: `tests/ci/test_check_ts_payload_package.py`
- Modify the exact historical files listed in the file map
- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: the clean source/package surface from Tasks 1-5.
- Produces:
  `check_repository(root: Path, policy: dict[str, object]) -> list[str]`,
  a versioned exact policy JSON, and one hosted P5 quality step.

- [ ] **Step 1: Write adversarial checker tests and policy**

The policy document is populated with exact paths and literals:

```json
{
  "schema": "fastdb.p5-clean-cut-policy.v1",
  "native_abi_symbol_count": 117,
  "python_version": "0.1.22",
  "typescript_version": "0.0.3",
  "removed_paths": [
    "python/fastdb4py/allocator.py",
    "python/fastdb4py/call_db.py",
    "python/fastdb4py/codegen/__init__.py",
    "python/fastdb4py/codegen/ts_gen.py",
    "python/fastdb4py/column_engine.py",
    "python/fastdb4py/require.py",
    "python/fastdb4py/schema.py",
    "tests/python/test_call_db_runtime.py",
    "tests/python/test_codegen.py",
    "tests/python/test_column_engine.py",
    "tests/python/test_require_envelope.py",
    "tests/python/test_schema_unified.py",
    "tests/ts/test_call_db_runtime.mjs",
    "ts/fastdb4ts/src/call-db.ts"
  ],
  "required_paths": [
    "fastcarto/fastdb/include/fastdb_payload.h",
    "fastcarto/fastdb/include/fastdb_payload.hpp",
    "bindings/rust/fastdb-sys/src/lib.rs",
    "bindings/rust/fastdb/src/lib.rs",
    "python/fastdb4py/payload/__init__.py",
    "python/fastdb4py/record_engine.py",
    "tests/python/test_record_engine.py",
    "ts/fastdb4ts/src/payload/index.ts",
    "tools/check_p5_clean_cut.py",
    "tests/ci/test_check_p5_clean_cut.py"
  ],
  "historical_allowlist": [
    "CHANGELOG.md",
    "docs/decisions/0001-portable-payload-core-authority.md",
    "docs/decisions/README.md",
    "docs/issues/0001-portable-payload-deferred-capabilities.md",
    "docs/issues/0002-portable-payload-foundation-implementation-status.md",
    "docs/issues/0003-legacy-swig-diagnostics.md",
    "docs/opt/batch-array-call-db-fast-path-design.md",
    "docs/superpowers/plans/2026-04-21-columnengine-string-column.md",
    "docs/superpowers/plans/2026-05-18-c-two-call-db-codec.md",
    "docs/superpowers/plans/2026-05-22-fdb-view-owner-lifetime.md",
    "docs/superpowers/plans/2026-05-27-require-envelope-neutral-allocator.md",
    "docs/superpowers/plans/2026-07-16-portable-payload-core-contract.md",
    "docs/superpowers/plans/2026-07-17-portable-payload-record-runtime.md",
    "docs/superpowers/plans/2026-07-20-portable-payload-object-graph-runtime.md",
    "docs/superpowers/plans/2026-07-23-portable-payload-clean-cut.md",
    "docs/superpowers/specs/2026-04-20-unified-feature-engine-design.md",
    "docs/superpowers/specs/2026-04-21-columnengine-string-column-design.md",
    "docs/superpowers/specs/2026-04-21-truncate-str-fill-unification-design.md",
    "docs/superpowers/specs/2026-04-22-truncate-string-ingest-optimization-design.md",
    "docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md",
    "docs/superpowers/specs/2026-07-21-portable-payload-language-projections-codegen-design.md",
    "docs/superpowers/specs/2026-07-23-portable-payload-clean-cut-design.md",
    "docs/vision/neutral-allocator-destructive-update.md",
    "ts/README.md",
    "ts/analysis/QUALITY_AUDIT.md"
  ],
  "literal_carrier_allowlist": [
    "tests/ci/p5_clean_cut_policy.json"
  ],
  "superseded_documents": [
    "docs/opt/batch-array-call-db-fast-path-design.md",
    "docs/vision/neutral-allocator-destructive-update.md",
    "docs/superpowers/plans/2026-04-21-columnengine-string-column.md",
    "docs/superpowers/plans/2026-05-18-c-two-call-db-codec.md",
    "docs/superpowers/plans/2026-05-22-fdb-view-owner-lifetime.md",
    "docs/superpowers/plans/2026-05-27-require-envelope-neutral-allocator.md",
    "docs/superpowers/specs/2026-04-20-unified-feature-engine-design.md",
    "docs/superpowers/specs/2026-04-21-columnengine-string-column-design.md",
    "docs/superpowers/specs/2026-04-21-truncate-str-fill-unification-design.md",
    "docs/superpowers/specs/2026-04-22-truncate-string-ingest-optimization-design.md",
    "ts/README.md",
    "ts/analysis/QUALITY_AUDIT.md"
  ],
  "authority_scan_roots": ["."],
  "domain_scan_roots": [
    "fastcarto/fastdb/include",
    "fastcarto/fastdb/src",
    "fastcarto/fastdb/swig",
    "python/fastdb4py",
    "ts/fastdb4ts/src",
    "tests/cpp",
    "tests/python",
    "tests/ts",
    "README.md",
    "python/README.md",
    "ts/fastdb4ts/README.md"
  ],
  "forbidden_authority_literals": [
    "ColumnEngine",
    "column_engine",
    "fastdb.schema.v1",
    "columnar.v1",
    "fastdb.call.columnar.v1",
    "fastdb.call.object-graph.v1",
    "call-db",
    "org.fastdb.call-db",
    "CALL_DB_",
    "FastdbCallDb",
    "call_db",
    "ArrayRequirement",
    "BatchRequirement",
    "build_call_db",
    "BytearrayAllocation",
    "BytearrayAllocator",
    "ScratchAllocation",
    "ScratchAllocator",
    "HeapScratchAllocation",
    "HeapScratchAllocator",
    "FinalBackingAllocation",
    "FinalBackingResource",
    "HeapFinalBackingAllocation",
    "HeapFinalBackingResource",
    "postToFinalBacking",
    "post_to_final_backing"
  ],
  "forbidden_domain_literals": [
    "c-two",
    "C-Two",
    "c3 relay",
    "CRM",
    "Toodle",
    "GIS",
    "route fingerprint",
    "transport lease",
    "lease policy"
  ],
  "standalone_markers": {
    "python/fastdb4py/object_engine.py": "Standalone object-graph storage engine; not fastdb.payload.v1 authority.",
    "python/fastdb4py/registry.py": "Process-local standalone feature metadata; not portable canonical identity.",
    "python/fastdb4py/serializer.py": "Legacy standalone serializer; not fastdb.payload.v1 or an external RPC format.",
    "python/fastdb4py/materialize.py": "Standalone table/view detachment helper; portable callers use fastdb4py.payload.",
    "python/fastdb4py/view_owner.py": "Standalone table/view lifetime helper; portable callers use fastdb4py.payload.",
    "ts/fastdb4ts/src/schema.ts": "Standalone schema metadata; not fastdb.payload.v1 authority.",
    "ts/fastdb4ts/src/feature.ts": "Standalone feature metadata; not fastdb.payload.v1 authority.",
    "ts/fastdb4ts/src/serializer.ts": "Legacy standalone serializer; not fastdb.payload.v1 or an external RPC format."
  }
}
```

The checker rejects any unknown key, duplicate entry, nonexistent allowlisted
path, wildcard, or directory entry in either allowlist. The policy JSON is the
only non-historical literal carrier because it must name what it rejects; all
negative tests and other checkers construct obsolete names from reviewed
fragments. Unit tests must independently mutate a temporary fixture to prove
rejection of:

1. each removed source path;
2. a `ColumnEngine` alias or old module;
3. a binding-side `fastdb.schema.v1`, digest, parser, profile, or renderer;
4. old scratch/final-backing C++ or SWIG declarations;
5. a new SWIG warning allowance;
6. a broad historical allowlist;
7. a superseded document without an explicit historical/superseded status and
   accepted-design link;
8. a current forbidden literal;
9. missing standalone-boundary labels;
10. a non-Core CLI generator/import or unsafe destination rule;
11. package version drift;
12. native/Wasm ABI drift;
13. CRM/route/relay/transport/lease/policy/Toodle/GIS behavior in current
    source/tests/examples/package docs; and
14. an inaccurate Issue 0002/0003 state; and
15. a missing relative Markdown link target or local heading anchor in any
    governed current, historical, design, plan, ADR, or Issue document.

- [ ] **Step 2: Run the genuine repository-policy RED**

```bash
python3 tests/ci/test_check_p5_clean_cut.py
python3 tools/check_p5_clean_cut.py --check
```

Expected: unit tests initially fail because the checker is absent; after the
checker unit becomes green, the repository check still fails on unlabeled
standalone/current documentation and unclassified historical literals.

- [ ] **Step 3: Implement the fail-closed standard-library checker**

Parse JSON with duplicate-key rejection, reject unknown policy keys, resolve
every policy path beneath the repository root, and return stable sorted
violations. Obtain the scan inventory with
`git ls-files -z --cached --others --exclude-standard` so an untracked,
non-ignored resurrection cannot evade the gate while ignored build/venv
outputs do not enter it. Search forbidden ASCII literals in raw file bytes;
decode UTF-8 only for the documents whose markers or links are parsed. Derive
exact public ABI symbols from the checked allowlist/generator inputs already
used by `tools/check_payload_abi_symbols.py`; do not maintain a second symbol
list.

Require all removed paths absent, all required paths present, versions exact,
no broad allowlist entry, and current literal scans clean. Accept Issue 0003
only in one of two truthful forms:

```text
Open   + explicit pending clean-package evidence
Closed + exact local warning-free package evidence
```

Issue 0002 remains open for hosted/release/downstream facts while its P5 local
subsection may become complete.

- [ ] **Step 4: Make every retained and historical surface truthful**

Add explicit module/public wording:

```text
RecordEngine/ObjectEngine/feature metadata/view-owner/materialize are
standalone storage helpers, not fastdb.payload.v1 authority.

FastSerializer is a legacy standalone serializer, not fastdb.payload.v1 and
not the external RPC foundation.
```

Rewrite current README/instructions around `fastdb4py.payload`, the standalone
APIs, and Core-only CLI. Mark each listed older design/audit file
`Status: historical/superseded` with a link to the accepted foundation/P5
design. Preserve accepted ADR/design/Issue history through the exact policy
allowlist.

In negative C++ tests, construct obsolete values from adjacent fragments, for
example:

```cpp
const std::string obsolete_profile =
    std::string("colum") + "nar.v1";
```

The runtime rejection assertion remains; only the repository literal is
removed.

Apply the same fragment rule to removed Python import assertions in
`tests/python/test_public_surface.py` and
`tests/python/test_import_boundary.py`, for example
`"fastdb4py." + "call" + "_db"`. This keeps the negative proof while making
the current-surface literal scan fail-closed.

Use reviewed fragments for forbidden package member assertions in
`tools/check_python_package_inventory.py`,
`tests/ci/test_check_python_package_inventory.py`,
`tests/ci/check_ts_payload_package.py`, and
`tests/ci/test_check_ts_payload_package.py` as well. Do not add those checker
files to either allowlist.

- [ ] **Step 5: Integrate and run focused/broad gates**

Add one `.github/workflows/tests.yml` step that runs:

```bash
python3 tests/ci/test_check_p5_clean_cut.py
python3 tools/check_p5_clean_cut.py --check
```

Then run locally:

```bash
python3 tests/ci/test_check_p5_clean_cut.py
python3 tools/check_p5_clean_cut.py --check
python3 tests/ci/test_check_p4_projection_codegen_quality.py
python3 tests/ci/check_p4_projection_codegen_quality.py --check
ruby tests/ci/test_check_p3_runtime_quality.rb
ruby tests/ci/check_p3_runtime_quality.rb --check-repository
python3 tests/ci/test_check_python_package_inventory.py
python3 tests/ci/test_check_ts_payload_package.py
uv run pytest tests/python -q
npm run test:ts
cmake -S fastcarto -B build/p5-task6 \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build/p5-task6 --parallel
ctest --test-dir build/p5-task6 --output-on-failure
python3 tools/check_payload_abi_symbols.py --build-dir build/p5-task6
git diff --check
```

Expected: all commands pass, ABI remains exactly 117, current surfaces contain
no forbidden terms, and historical exceptions are exact and visibly labeled.

- [ ] **Step 6: Record, commit, freeze, and review**

```bash
git add -A -- \
  tools/check_p5_clean_cut.py \
  tests/ci/p5_clean_cut_policy.json \
  tests/ci/test_check_p5_clean_cut.py \
  tools/check_python_package_inventory.py \
  tests/ci/test_check_python_package_inventory.py \
  tests/ci/check_ts_payload_package.py \
  tests/ci/test_check_ts_payload_package.py \
  .github/workflows/tests.yml \
  python/fastdb4py/object_engine.py \
  python/fastdb4py/registry.py \
  python/fastdb4py/serializer.py \
  python/fastdb4py/materialize.py \
  python/fastdb4py/view_owner.py \
  ts/fastdb4ts/src/schema.ts \
  ts/fastdb4ts/src/feature.ts \
  ts/fastdb4ts/src/serializer.ts \
  tests/python/test_public_surface.py \
  tests/python/test_import_boundary.py \
  tests/cpp/payload/test_spec_parse.cpp \
  tests/cpp/payload/test_codegen.cpp \
  README.md python/README.md ts/fastdb4ts/README.md \
  AGENTS.md .github/copilot-instructions.md CHANGELOG.md \
  docs/opt/batch-array-call-db-fast-path-design.md \
  docs/vision/neutral-allocator-destructive-update.md \
  docs/superpowers/plans/2026-04-21-columnengine-string-column.md \
  docs/superpowers/plans/2026-05-18-c-two-call-db-codec.md \
  docs/superpowers/plans/2026-05-22-fdb-view-owner-lifetime.md \
  docs/superpowers/plans/2026-05-27-require-envelope-neutral-allocator.md \
  docs/superpowers/specs/2026-04-20-unified-feature-engine-design.md \
  docs/superpowers/specs/2026-04-21-columnengine-string-column-design.md \
  docs/superpowers/specs/2026-04-21-truncate-str-fill-unification-design.md \
  docs/superpowers/specs/2026-04-22-truncate-string-ingest-optimization-design.md \
  ts/README.md ts/analysis/QUALITY_AUDIT.md \
  docs/issues/0002-portable-payload-foundation-implementation-status.md
git diff --cached --check
git commit -m "test: enforce portable payload clean cut"
```

Review policy coverage/authority/history classification first, then checker
security/path handling/YAML/docs maintainability. Rerun Step 5 after fixes.

---

### Task 7: Run Fresh Local Release-Readiness Gates and Freeze P5

**Files:**

- Modify:
  `docs/issues/0002-portable-payload-foundation-implementation-status.md`
- Modify: `docs/issues/0003-legacy-swig-diagnostics.md` only if final evidence
  corrects its Task 4 receipt
- Modify: `CHANGELOG.md`
- Create/update ignored:
  `.superpowers/sdd/p5-task-7-brief.md`,
  `.superpowers/sdd/p5-task-7-report.md`,
  `.superpowers/sdd/p5-final-review.diff`, and
  `.superpowers/sdd/progress.md`

**Interfaces:**

- Consumes: Tasks 0-6 and every frozen P1-P4 gate.
- Produces: one locally frozen FastDB owner boundary ready for C-Two
  composition; no published artifact or release.

- [ ] **Step 1: Retain a genuine closure-record RED**

Before any final evidence is recorded, run:

```bash
rg -n '^\*\*P5 local clean cut:\*\* Complete$' \
  docs/issues/0002-portable-payload-foundation-implementation-status.md
```

Expected: exit `1` because the Issue truthfully says Task 0 is frozen and P5
implementation has not started. Step 7 adds this exact marker only after all
fresh gates pass.

- [ ] **Step 2: Build fresh Debug, Release, and ASan+UBSan trees**

```bash
rm -rf build/p5-final-debug build/p5-final-release build/p5-final-sanitize

cmake -S fastcarto -B build/p5-final-debug \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build/p5-final-debug --parallel
ctest --test-dir build/p5-final-debug --output-on-failure

cmake -S fastcarto -B build/p5-final-release \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/p5-final-release --parallel
ctest --test-dir build/p5-final-release --output-on-failure

cmake -S fastcarto -B build/p5-final-sanitize \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_ENABLE_SANITIZERS=ON
cmake --build build/p5-final-sanitize --parallel
ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/p5-final-sanitize --output-on-failure
```

Expected: complete suites pass. Record `detect_leaks=0` as no LeakSanitizer
claim.

- [ ] **Step 3: Run available TSan, corpus/fuzz, ABI, schema, and dependency gates**

Build the no-competing-load TSan tree explicitly:

```bash
rm -rf build/p5-final-tsan
cmake -S fastcarto -B build/p5-final-tsan \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/p5-final-tsan --parallel
TSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
ctest --test-dir build/p5-final-tsan --output-on-failure \
  -R '^payload\.(compiled_spec|codegen|runtime_abi|runtime_cpp_facade|payload_builder|record_binary|graph_binary|payload_backing|graph_backing|payload_open|checked_view|graph_view|graph_materialize)$|^legacy\.descriptor_determinism$'
```

If configuration, link, or process start proves the local compiler/runtime
unavailable, retain that exact command/error and make no TSan claim. A test
failure after the runtime starts is a product failure, not an unavailable
gate.

```bash
python3 tools/check_payload_abi_symbols.py --build-dir build/p5-final-debug
python3 tests/ci/test_check_payload_binary_corpus.py
python3 tools/check_payload_binary_corpus.py --check
python3 tools/generate_embedded_payload_schemas.py --check
tools/vendor_portable_payload_deps.sh --check
python3 tests/ci/test_generate_payload_wasm_exports.py
ruby tests/ci/check_p2_task11_quality.rb --check-repository
ruby tests/ci/check_p3_runtime_quality.rb --check-repository
python3 tests/ci/check_p4_projection_codegen_quality.py --check
python3 tools/check_p5_clean_cut.py --check
```

Compile the pure C11 public header for each reviewed target:

```bash
clang -std=c11 -Wall -Wextra -Werror -arch arm64 \
  -Ifastcarto/fastdb/include -c \
  tests/cpp/payload/test_c_header_smoke.c \
  -o build/p5-final-debug/c-header-arm64.o
clang -std=c11 -Wall -Wextra -Werror -arch x86_64 \
  -Ifastcarto/fastdb/include -c \
  tests/cpp/payload/test_c_header_smoke.c \
  -o build/p5-final-debug/c-header-x86_64.o
emcc -std=c11 -Wall -Wextra -Werror \
  -Ifastcarto/fastdb/include -c \
  tests/cpp/payload/test_c_header_smoke.c \
  -o build/p5-final-debug/c-header-wasm32.o
```

If local libFuzzer linkage remains unavailable, run the deterministic reviewed
corpus under ASan+UBSan and retain the exact toolchain limitation; do not claim
coverage-guided fuzzing.

- [ ] **Step 4: Run Rust, Python, package, and generated-output gates**

```bash
cargo fmt --manifest-path bindings/rust/Cargo.toml --all -- --check
cargo clippy --manifest-path bindings/rust/Cargo.toml \
  --workspace --all-targets --all-features -- -D warnings
cargo test --manifest-path bindings/rust/Cargo.toml \
  --workspace --all-features
python3 tests/ci/test_check_rust_payload_package.py
python3 tests/ci/check_rust_payload_package.py \
  --build-dir build/p5-final-debug

uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
python3 tests/ci/test_check_python_package_inventory.py
rm -rf build/p5-final-dist-current build/p5-final-dist-py310
current_python="$(uv run python -c 'import sys; print(sys.executable)')"
export current_python
uv build --python "$current_python" \
  --out-dir build/p5-final-dist-current \
  2>&1 | tee build/p5-final-package-current.log
python3 tools/check_python_package_inventory.py \
  --dist-dir build/p5-final-dist-current \
  --build-log build/p5-final-package-current.log
uv build --python 3.10 \
  --out-dir build/p5-final-dist-py310 \
  2>&1 | tee build/p5-final-package-py310.log
python3 tools/check_python_package_inventory.py \
  --dist-dir build/p5-final-dist-py310 \
  --build-log build/p5-final-package-py310.log

bash -euo pipefail <<'BASH'
repo="$PWD"
current_wheels=("$repo"/build/p5-final-dist-current/*.whl)
python310_wheels=("$repo"/build/p5-final-dist-py310/*.whl)
test "${#current_wheels[@]}" -eq 1
test "${#python310_wheels[@]}" -eq 1

run_installed_tests() {
  label="$1"
  python="$2"
  wheel="$3"
  temporary="$(mktemp -d "${TMPDIR:-/tmp}/fastdb-p5-${label}.XXXXXX")"
  if (
    cd "$temporary"
    uv run --isolated --no-project --python "$python" \
      --with "$wheel" --with pytest \
      python -m pytest --rootdir="$temporary" -c /dev/null \
      "$repo/tests/python/payload" \
      "$repo/tests/python/test_cli_codegen.py" \
      "$repo/tests/python/test_public_surface.py" \
      "$repo/tests/python/test_record_engine.py" \
      "$repo/tests/python/test_object_engine.py" \
      "$repo/tests/python/test_view_owner_lifetime.py" \
      "$repo/tests/python/test_materialize.py" \
      "$repo/tests/python/test_fast_serializer.py" -q
  ); then
    status=0
  else
    status=$?
  fi
  rm -rf "$temporary"
  return "$status"
}

run_installed_tests current "$current_python" "${current_wheels[0]}"
run_installed_tests python310 3.10 "${python310_wheels[0]}"
BASH

shasum -a 256 \
  build/p5-final-dist-current/* \
  build/p5-final-dist-py310/*

uv run python tests/ci/test_run_generated_payload_projections.py
uv run python tests/ci/run_generated_payload_projections.py \
  --build-dir build/p5-final-debug
```

Expected: both independently built wheel/sdist pairs pass exact inventory;
the isolated current/Python-3.10 runs cannot import the source tree and pass
payload, CLI, public-surface, and retained standalone behavior. Record the
exact interpreter versions and printed package SHA-256 values.

- [ ] **Step 5: Run TypeScript, package, and independent Core Wasm gates**

```bash
npm --prefix ts/fastdb4ts run build:wasm
npm run test:ts
python3 tests/ci/test_check_ts_payload_package.py
rm -rf build/p5-final-ts-package
cmake -E make_directory build/p5-final-ts-package
npm pack ./ts/fastdb4ts --pack-destination build/p5-final-ts-package
python3 tests/ci/check_ts_payload_package.py \
  --package-dir build/p5-final-ts-package
python3 tools/check_payload_abi_symbols.py \
  --wasm-build-dir ts/build-wasm

rm -rf build/p5-final-wasm
emcmake cmake -S fastcarto -B build/p5-final-wasm \
  -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/p5-final-wasm --parallel
python3 tools/check_payload_abi_symbols.py \
  --wasm-build-dir build/p5-final-wasm
node build/p5-final-wasm/wasm-tests/payload_runtime_harness.js
node build/p5-final-wasm/tests/cpp/fastdb_payload_test_c_header_smoke.js
node build/p5-final-wasm/tests/cpp/fastdb_payload_test_codegen_c_abi.js
node build/p5-final-wasm/tests/cpp/fastdb_payload_test_cpp_facade.js
node build/p5-final-wasm/tests/cpp/fastdb_payload_test_runtime_cpp_facade.js
node build/p5-final-wasm/wasm-tests/payload_runtime_abi_single_thread.js \
  --single-thread-injected-failure
node build/p5-final-wasm/wasm-tests/payload_runtime_abi_single_thread.js \
  --graph-runtime-proof
```

Expected: both Wasm surfaces expose exactly 117 symbols and all Node receipts,
including both single-thread modes, pass.

- [ ] **Step 6: Audit documentation, scope, credentials, and clean state**

```bash
python3 tests/ci/test_check_p5_clean_cut.py
python3 tools/check_p5_clean_cut.py --check
git diff --check
git status --short
git diff --name-only 9d86c171eda1fe107c3519ce040ca2ec417167f9
rg -n 'T[O]DO|T[B]D|F[I]XME|pan[i]c!|to[d]o!|unimplemente[d]!' \
  $(git diff --name-only 9d86c171eda1fe107c3519ce040ca2ec417167f9)
test $? -eq 1
```

Inspect changed files for credentials, signed URLs, private endpoints, build
artifacts, and consumer-specific behavior. Expected: no secret/debris finding,
no unresolved drafting marker in changed production/current docs, and only
planned paths changed.

- [ ] **Step 7: Record local closure without a release claim**

Issue 0002 must record:

```text
**P5 local clean cut:** Complete
portable ABI = exactly 117
package versions = 0.1.22 / 0.0.3 unchanged
primary review = same-agent, not independent
hosted CI = pending
push/tag/publication/release = not performed
C-Two composition = next downstream owner slice
```

Record exact test counts, commands, platforms, warnings, unavailable gates,
package hashes, commits, and all intentionally retained limitations. Issue
0002 stays open for hosted/release/downstream facts. Issue 0003 stays closed
only if the final clean package log still has zero SWIG warnings.

```bash
python3 tests/ci/test_check_p5_clean_cut.py
python3 tools/check_p5_clean_cut.py --check
git diff --check
```

Expected: the checker accepts the exact locally-complete/externally-pending
state and all three commands pass.

- [ ] **Step 8: Commit closure and run the frozen two-pass review**

```bash
git add \
  docs/issues/0002-portable-payload-foundation-implementation-status.md \
  docs/issues/0003-legacy-swig-diagnostics.md \
  CHANGELOG.md
git diff --cached --check
git commit -m "docs: record portable payload P5 closure"
git diff --binary \
  --output=.superpowers/sdd/p5-final-review.diff \
  9d86c171eda1fe107c3519ce040ca2ec417167f9..HEAD
```

Pass 1 covers every design requirement, sole Core authority, ABI-117,
clean-cut completeness, standalone truth, package readiness, and accurate
limits. Pass 2 covers C++ initialization/resource safety, SWIG ownership,
Python/TypeScript lifetime and path safety, portability, checker fail-closure,
workflow truth, documentation, and maintainability.

For each material finding: retain a failing regression, fix it in a scoped
commit, rerun every affected focused/broad gate, then rerun the complete final
gate set. P5 is frozen only with zero unresolved Critical, Important, or
material Minor findings.

- [ ] **Step 9: Remove disposable outputs after evidence is immutable**

After the report records all hashes and results, delete only ignored build
products created by this plan:

```bash
rm -rf \
  build/p5-task1-native build/p5-task1-dist \
  build/p5-task2-dist build/p5-task3-package \
  build/p5-task4-red build/p5-task4-red-dist \
  build/p5-task4-debug build/p5-task4-sanitize build/p5-task4-dist \
  build/p5-task5-dist build/p5-task6 \
  build/p5-task2-build.log build/p5-task4-red-build.log \
  build/p5-task4-build.log build/p5-task5-build.log \
  build/p5-final-debug build/p5-final-release build/p5-final-sanitize \
  build/p5-final-tsan build/p5-final-dist-current \
  build/p5-final-dist-py310 build/p5-final-ts-package \
  build/p5-final-wasm build/p5-final-package-current.log \
  build/p5-final-package-py310.log \
  ts/build-wasm ts/fastdb4ts/dist bindings/rust/target
git status --short
```

Expected: no tracked change from cleanup. Keep source virtual environments and
all tracked evidence; do not delete user-owned caches outside this repository.

---

## P5-to-C-Two Handoff Boundary

After Task 7, stop FastDB design work. The next repository owner is C-Two,
which consumes FastDB as follows:

```text
c-two.contract.v2 JSON bytes
  -> C-Two parses and validates the outer contract
  -> C-Two extracts the nested FastDB source bytes
  -> FastDB Core compiles the nested value
  -> FastDB Core returns its immutable ArtifactSet
  -> C-Two/c3 composes FastDB artifacts with C-Two-owned artifacts
  -> Rust and Python C-Two SDKs expose equal downstream behavior
```

FastDB does not learn the outer schema, CRM, routes, transport, policy,
destination-tree conflict rules, or SDK composition. Any missing generic
FastDB capability discovered by the downstream proof must be reported as a
new owner issue and reviewed before reopening this frozen P5 boundary.
