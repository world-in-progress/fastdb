# FastDB Portable Payload P5 Clean-Cut Design

Date: 2026-07-23

Status: accepted P5 delta under the active implementation Goal

Scope: FastDB P5 local clean cut and release readiness only

## 1. Relationship to the accepted foundation

This document is the implementation-level P5 delta for the accepted
[portable payload foundation design](2026-07-16-portable-payload-foundation-design.md)
and [ADR 0001](../../decisions/0001-portable-payload-core-authority.md).
It consumes the locally frozen P1-P4 boundary recorded in
[Issue 0002](../../issues/0002-portable-payload-foundation-implementation-status.md).

P1-P3 Core semantics and the historical ABI-105 runtime boundary are frozen
inputs. P4 is also frozen at exactly 117 sorted `fdb_payload_v1_*` symbols:
four official language projections and one C++ Core-owned four-target
`ArtifactSet` generator already provide the complete replacement for the
legacy portable-authority path.

P5 removes obsolete public authority, makes the surviving standalone storage
APIs truthful, and proves local package readiness. It does not change:

- `fastdb.payload.v1`, either portable profile, or the native type algebra;
- canonicalization, digest, manifest, binary, builder, plan, backing, view,
  materialization, invalidation, error, or codegen meaning;
- `fastdb.payload.bin.v1`;
- C ABI versioning or any of the 117 frozen symbol meanings;
- the FastDB/C-Two ownership boundary; or
- any post-0.2 capability tracked in
  [Issue 0001](../../issues/0001-portable-payload-deferred-capabilities.md).

No new ADR is required. A P5 implementation that discovers a reproducible
defect in a frozen portable layer must reopen that owner explicitly rather
than hiding a semantic change inside cleanup work.

## 2. Outcome

At P5 closure:

1. The only portable specification, canonical identity, binary, runtime,
   lifetime, and codegen authority is the C++ Core reached through ABI-117.
2. Python and TypeScript contain no public call-db runtime, binding,
   descriptor, profile, parser, digest, or generator.
3. Python contains no `fastdb.schema.v1` authority or binding-owned portable
   canonicalization/digest implementation.
4. The public AoS storage engine is `RecordEngine`; neither a
   `ColumnEngine` class, module, alias, import fallback, nor silent profile
   conversion remains.
5. `ObjectEngine`, feature metadata, table/view helpers, and
   `FastSerializer` survive only as explicitly standalone APIs.
6. The generic `fdb codegen` command consumes Core `CompiledSpec` and
   `ArtifactSet` results; it does not discover Python classes or generate
   language semantics itself.
7. Orphaned scratch/final-backing classes added solely for the old call-db
   path are absent from the standalone C++/SWIG surface. Generic portable
   backing remains available through the already-frozen payload API.
8. The legacy standalone database writer initializes every persisted
   descriptor member deterministically, including the non-list
   `element_type` field.
9. A clean Python package build emits zero SWIG warnings, and the package
   gate rejects every warning rather than maintaining an accepted warning
   allowlist.
10. Current exports, package inventories, README files, examples, tests, and
    workflows describe one clean target. Old terms survive only in exact,
    explicitly historical governance or migration records.
11. All locally available release-readiness gates pass without a version
    change, push, tag, publication, or release.

FastDB is then locally frozen for downstream C-Two composition. P5 itself does
not implement an outer contract, RPC method, route, transport, lease, policy,
or destination-tree composer.

## 3. Explicit non-goals

P5 does not:

- redesign or optimize the portable Core;
- add a compatibility parser, deprecated alias, dual digest, or profile
  conversion;
- make `RecordEngine` or `ObjectEngine` a second portable-payload runtime;
- turn feature decorators into another compiled-spec authority;
- make `FastSerializer` a supported portable RPC format;
- make the FastDB CLI a project generator or multi-concern composer;
- remove legitimate field-oriented names such as `table.column`,
  `StringColumn`, or TypeScript `StridedColumn`;
- provide segmented backing, streaming builders, new guaranteed platforms,
  native Node, or Go portable projections;
- update package versions or release metadata;
- execute hosted CI, push, tag, publish, or create a release;
- modify C-Two or Toodle.

## 4. Live starting inventory

The inventory below was taken at
`9d86c171eda1fe107c3519ce040ca2ec417167f9`.
It is a migration input, not the target public contract.

### 4.1 Complete replacement already exists

The frozen public replacement consists of:

- `fastcarto/fastdb/include/fastdb_payload.h`;
- `fastcarto/fastdb/include/fastdb_payload.hpp`;
- the C++ payload implementation under `fastcarto/fastdb/src/payload/`;
- `bindings/rust/fastdb-sys` and the safe `bindings/rust/fastdb` crate;
- `python/fastdb4py/payload/`;
- `ts/fastdb4ts/src/payload/`; and
- the Core-owned C++, Rust, Python, and TypeScript generator.

Those surfaces already cover compile/query, author/freeze/plan, direct and
staged execution, copied and external open, record and graph views, complete
V1 values, sharing/cycles, materialization, invalidation, owned errors, and
deterministic artifact generation. P5 does not need a substitute for any
legacy call-db behavior.

### 4.2 Python duplicate authority

The starting Python package exposes:

| Path or surface | Starting role | P5 disposition |
|---|---|---|
| `python/fastdb4py/call_db.py` | 2,566-line call envelope, runtime, view, profile, and binary layer | delete |
| `python/fastdb4py/schema.py` | `fastdb.schema.v1`, `json.dumps(sort_keys=True)`, SHA-256, capability, and codec authority | delete |
| `python/fastdb4py/require.py` | call-envelope allocation and context | delete |
| `Array`, `Batch`, their requirements, `array()`, and `batch()` in `type.py` | call-db/RPC authoring and runtime markers | delete |
| `python/fastdb4py/allocator.py` | call-db scratch/final-backing wrappers | delete |
| `python/fastdb4py/codegen/ts_gen.py` | Python-feature-owned TypeScript generator | delete |
| `python/fastdb4py/cli.py` | entry point for that old generator | replace with a Core ArtifactSet facade |
| top-level `__init__.py` exports | publishes all of the above | replace with the exact clean surface |

Live dependency searches show that `schema.py`, `require.py`, the
call-envelope `Array`/`Batch` types, and `allocator.py` have no independent
production consumers outside the call-db path and its public tests. The
feature registry does not depend on `schema.py` and does not compute a
portable digest.

### 4.3 TypeScript duplicate authority

`ts/fastdb4ts/src/call-db.ts` and its root exports implement the same obsolete
call-envelope descriptor/runtime behavior in TypeScript. Its only required
consumers are the legacy call-db tests and package export test.

The separate files `schema.ts`, `types.ts`, `feature.ts`, `orm.ts`,
`table.ts`, `column.ts`, and `serializer.ts` are standalone engine and
serializer facilities. They do not own portable canonicalization or digest
meaning and can survive after their documentation is made explicit.

### 4.4 Orphaned C++ and SWIG mechanisms

The legacy public C++ header currently contains:

- `ScratchAllocation`, `ScratchAllocator`, and heap implementations;
- `FinalBackingAllocation`, `FinalBackingResource`, and heap implementations;
- `FastVectorDbBuild::postToFinalBacking`; and
- matching SWIG renames, extension methods, ownership annotations, and Python
  wrappers.

These declarations were introduced with the call-db require/final-backing
path. Their only live non-implementation users are legacy call-db code and
tests that explicitly exercise those wrappers. Ordinary standalone table
construction uses the retained `postToBuffer`/fixed-buffer path; portable
payload construction uses the frozen generic backing callback and projection
surfaces.

The same standalone database writer has a real determinism defect:
`FastVectorDbLayerBuild::Impl::addField` leaves
`field_desc_ex_t::element_type` uninitialized for non-list fields and later
serializes the complete descriptor. Deleting call-db removes the known RPC
exposure but would leave nondeterministic standalone database and serializer
bytes. P5 therefore fixes the owner-layer initialization and proves repeated
standalone builds are byte-identical.

### 4.5 Engine naming

`python/fastdb4py/column_engine.py` contains the useful 615-line AoS engine.
It stores records in array-of-structures form and exposes field-oriented
access by stride. The implementation survives; only its false storage claim
and public name do not.

The rename includes:

- source filename and class;
- all imports and runtime type checks;
- tests and test filenames;
- examples and benchmark source;
- current README and contributor instructions; and
- error messages and docstrings that call the engine/profile columnar.

It does not rename physical column accessors, `StringColumn`, `BytesColumn`,
or `StridedColumn`.

### 4.6 Standalone survivors

The following remain useful independently of portable payload:

- `RecordEngine`, `ObjectEngine`, `Layout`, `Table`, and field/column views;
- `@feature`, `LayerSchema`, registry lookup, and native type annotations;
- trusted or checked standalone table-view ownership;
- recursive materialization of standalone table/feature values;
- `FastSerializer` in Python and TypeScript; and
- the legacy standalone C++ database/storage classes.

They must be explicitly classified rather than silently treated as portable
projections.

### 4.7 Package diagnostics and documentation

The current package gate accepts exactly seven legacy SWIG diagnostics:
six unsupported nested-declaration warnings for C++ tile types that are
already excluded from Python, and one writable-setter warning for
`utf8_view_t::data`.

Current public README files also document the migration surfaces as if they
were still callable current APIs. Several older optimization, vision, audit,
and implementation-plan files remain useful history but look active unless
they carry an explicit superseded/historical status.

## 5. Target ownership and dependency direction

The final dependency direction is:

```text
portable specification bytes
        |
        v
C++ Core -> stable ABI-117 -> C++ / Rust / Python / TypeScript-Wasm
        |
        +-> Core-owned ArtifactSet
                         |
                         +-> generic fdb diagnostic output
                         +-> downstream owner composition

standalone feature metadata -> RecordEngine / ObjectEngine / FastSerializer
```

There is no edge from standalone feature metadata, serializer code, Python,
or TypeScript back into portable compile, identity, binary, runtime, or
codegen semantics.

## 6. Python public surface

### 6.1 Portable package

`fastdb4py.payload` remains the only Python portable projection. It continues
to expose the Core-backed:

- `CompiledSpec`, `Capabilities`, and `Profile`;
- `Builder`, `BuildPlan`, builder options, object handles, and plan facts;
- `MemoryBacking`, `ExternalBytes`, build/open options, reports, and payload
  ownership;
- `View`, `Access`, graph identity, materialization, and invalidation;
- `ArtifactSet`, artifact metadata, codegen options, and codegen targets; and
- exact Core error fields.

No P5 source may add JSON parsing, canonicalization, SHA-256 identity, layout,
binary, graph, or generator logic to this package.

### 6.2 Top-level standalone package

The top-level package exports exactly the retained standalone concepts:

- `feature`, `is_feature`, `get_schema`, and `lookup_class`;
- `Layout`, `RecordEngine`, `ObjectEngine`, and `Table`;
- `StringColumn` and `pack_utf8_column`;
- `FdbViewOwner`, its two errors, `invalidate`, and `materialize`;
- `FastSerializer`; and
- the existing native aliases `BOOL`, `U8`, `U16`, `U32`, `I32`, `U8N`,
  `U16N`, `F32`, `F64`, `STR`, `WSTR`, `REF`, and `BYTES`.

`get_schema` returns process-local `LayerSchema` metadata for standalone
feature/storage authoring. It does not return a portable source document,
canonical bytes, a digest, a capability declaration, or a codec reference.

`FdbViewOwner`, top-level `invalidate`, and top-level `materialize` are
documented as standalone table/view helpers. Portable payload callers use
the Core-backed `Payload` and `View` methods in `fastdb4py.payload`. The
standalone helpers may delegate to an object's existing safe method, but
cannot recreate portable lifetime or materialization semantics.

### 6.3 Removed import surface

The following modules and names must fail to import or be absent:

- `fastdb4py.call_db`;
- `fastdb4py.schema`;
- `fastdb4py.require`;
- `fastdb4py.allocator`;
- `fastdb4py.codegen` and its old TypeScript generator;
- every call-db descriptor, view, codec, profile, context, or encoder name;
- `SCHEMA_VERSION`, schema canonicalization/digest/capability helpers;
- `Array`, `Batch`, their requirement types, `array`, `batch`, and `require`;
- the old scratch/final-backing wrapper names; and
- `ColumnEngine` and `fastdb4py.column_engine`.

There is no deprecation warning or fallback import. Import failure is the
intentional 0.x clean cut.

## 7. TypeScript public surface

### 7.1 Portable subpath

`fastdb4ts/payload` remains the official browser-capable portable projection.
It continues to use the mechanical Wasm ABI export and explicit disposal.
P5 does not add a native Node path or a TypeScript parser.

### 7.2 Standalone root

The root package may retain:

- Wasm initialization;
- standalone `Feature`, `ORM`, `Table`, and `StridedColumn`;
- standalone schema/type metadata needed by those classes;
- standalone database-buffer ownership;
- `FastSerializer`; and
- standalone runtime/schema/usage errors.

`FastSerializer` receives a module comment and public documentation that call
it a legacy standalone serializer. Its format and schema metadata are not
advertised as `fastdb.payload.v1`.

### 7.3 Removed root surface

`call-db.ts`, all call-db functions and types, and their tests disappear.
The packed npm root must not export them, while the `./payload` subpath and
its Wasm runtime remain unchanged.

## 8. Standalone engine contract

### 8.1 `RecordEngine`

`RecordEngine` is a clean rename of the existing AoS storage engine. Its
storage and behavior are not reimplemented. It retains:

- mutable `create`;
- fixed-capacity `truncate`;
- load/save and shared-memory behavior;
- named standalone `Layout` and `Table` access;
- numeric, string, bytes, and list fields already supported;
- trusted raw NumPy field views for standalone performance;
- checked/read-only owner behavior when explicitly requested; and
- existing materialization helpers.

No `ColumnEngine` alias, module, subclass, re-export, `__getattr__` fallback,
pickle compatibility name, or string-based profile conversion is provided.

### 8.2 `ObjectEngine`

`ObjectEngine` remains the name of the standalone mutable object-graph
database engine. It is not the implementation or authority for portable
`object_graph.v1`; the portable graph runtime remains exclusively in the
payload Core.

### 8.3 Feature metadata

Python `LayerSchema` and TypeScript class schema objects remain local
authoring/storage metadata. They may describe standalone fields and dispatch
storage operations. They may not:

- use a version string that claims portable authority;
- calculate portable canonical bytes or digest;
- choose a portable profile;
- publish codec IDs or RPC capabilities; or
- accept/reject a portable source document independently of Core.

No new feature-to-payload converter is required by P5. A future convenience
may be added only if it constructs authoring JSON and immediately delegates
the final bytes to Core without becoming another validator.

### 8.4 `FastSerializer`

Python and TypeScript `FastSerializer` remain available for existing
standalone object serialization and their existing interoperability tests.
Every current public description must state:

- it is a legacy standalone serializer;
- it is not `fastdb.payload.v1`;
- it is not the external RPC foundation; and
- new portable integrations should use the payload projection.

Its internal format can continue to use standalone feature metadata. It cannot
be called a portable profile or share a portable digest.

## 9. Generic backing and legacy C++ removal

The portable generic backing contract is already complete:

- `BuildPlan` owns exact immutable build facts;
- heap execution and `MemoryBacking` provide Core-owned memory;
- external callback backing and `ExternalBytes` provide caller-owned storage;
- direct/staged reports remain truthful;
- `Payload` owns or retains committed backing;
- checked views drain before invalidation returns; and
- materialized values survive source release.

P5 therefore removes the old call-db-only C++ scratch/final-backing classes,
implementations, methods, SWIG directives, and Python wrappers. It retains
`FixedBufferWriteStream` and `postToBuffer` because the standalone engine has
real callers for writing a known buffer and they do not claim portable
final-backing semantics.

Removing these C++ classes does not change ABI-117 because they are not
`fdb_payload_v1_*` symbols. It is an intentional breaking cleanup of the
legacy C++ class surface under the 0.x clean-cut decision.

## 10. Generic `fdb codegen` facade

### 10.1 Command

The installed Python entry point remains `fdb`. Its codegen command becomes:

```text
fdb codegen SPEC.json --target cpp|rust|python|typescript --output DIR
```

`SPEC.json` is read as exact bytes. The CLI calls
`CompiledSpec.compile(source_bytes)` and then Core codegen for the selected
target. It never imports user Python modules or discovers feature classes.

### 10.2 Artifact and filesystem rules

The CLI consumes artifacts in Core order and does not modify their bytes,
paths, kinds, or hashes. Before touching the filesystem it:

1. compiles and generates the complete artifact set;
2. copies every artifact and its Core-provided hash into Python-owned values;
3. validates that each path is non-empty, normalized, relative, contains no
   `..`, and is unique;
4. rejects non-source artifact kinds it does not know how to emit; and
5. requires the output root not to exist.

It writes a private sibling staging directory, creates only normalized
artifact parents, writes exact bytes, and renames the complete staging tree to
the requested output root. Any failure removes only the CLI-created staging
tree. It never merges into or overwrites an existing project tree.

This narrow output rule is intentional, not a missing project-composition
feature. The FastDB CLI is a diagnostic/explicit artifact emitter; downstream
owners such as `c3` perform conflict-aware multi-concern composition.

### 10.3 Diagnostics

Core compile/codegen errors preserve their code, symbol, path, message, and
details in the CLI diagnostic. Filesystem and usage errors remain CLI errors
and do not invent FastDB Core error codes.

The CLI may display Core-returned payload and artifact hashes, but it does not
calculate a competing payload identity or treat its output format as another
manifest.

## 11. Deterministic standalone descriptor repair

P5 initializes the complete `field_desc_ex_t` object before assigning any
field. Non-list fields persist `element_type == 0`; list fields overwrite it
with their explicit native element type.

The repair must have a native regression that:

- builds the same ordinary non-list standalone database repeatedly from
  perturbed stack state;
- compares exact output bytes;
- opens the result and confirms logical values;
- inspects the persisted descriptor to prove the reserved element field is
  zero; and
- runs in Debug, Release, ASan+UBSan, and available TSan configurations.

The test is standalone database evidence, not a replacement portable golden.
Portable binary bytes remain governed by the existing payload corpus.

## 12. SWIG zero-warning contract

The six nested tile declarations are already absent from the Python API.
P5 excludes them from SWIG parsing explicitly while leaving the C++ tile APIs
available to native callers. The `utf8_view_t` bridge is used internally by a
custom sequence wrapper; SWIG must not generate a writable owner-ambiguous
setter for its `const char *` member.

The implementation may use verified SWIG exclusion/immutability directives or
a `SWIG` preprocessing guard, but it may not:

- delete a live C++ tile API merely to silence the Python generator;
- suppress all warning output globally;
- add a broad warning allowlist; or
- hide a new warning in build-log filtering.

`check_swig_diagnostics` changes from “accept these exact seven warnings” to
“accept zero SWIG warnings.” Its unit tests first prove that the old retained
warning set is rejected, then a clean wheel build proves no warning is
emitted. Issue 0003 closes only after Python tests, compileall, sdist/wheel,
and exact package inventory also pass.

## 13. Historical records and forbidden-term policy

### 13.1 Current surfaces

Production source, current tests, examples, active README files, package
exports, generated package inventories, and user-facing CLI help must contain
none of the removed authority names or IDs.

Negative tests that prove an obsolete profile is rejected construct the input
from reviewed fragments so the repository-wide current-surface scan remains
literal and fail-closed.

### 13.2 Historical allowlist

Old terms may remain only in exact files classified as:

- accepted foundation/phase design history;
- ADR history;
- Issues 0001-0003 and their eventual closure evidence;
- explicit migration or implementation-plan records;
- the P5 design/plan/report;
- changelog explanations; or
- a separately marked historical audit/benchmark record.

The P5 checker uses an exact file allowlist, not a broad `docs/**` exclusion.
Older active-looking optimization, vision, or TypeScript plan/audit files that
contain removed terms receive a visible “historical/superseded” header or are
removed when they have no continuing evidentiary value.

The versioned P5 policy JSON is the sole non-historical literal carrier because
it must name the strings it rejects. The checker excludes that one exact file
from its content scan while validating the policy's own schema, duplicate
entries, paths, and absence of wildcards. Negative tests and other quality
checkers construct obsolete names from reviewed fragments; they do not join an
expanding exception list.

### 13.3 Domain ownership scan

FastDB production source, tests, examples, generated outputs, and package
README files contain no CRM, route, relay, transport, lease, policy, Toodle,
GIS, or consumer-specific codegen behavior. Accepted architecture,
governance, and Issue records may explain the owner boundary without becoming
product behavior.

## 14. Package and export proof

### 14.1 Python

The package inventory gate must require every portable projection and retained
standalone module, and must explicitly forbid the removed Python modules.
The wheel still contains exactly:

- one Python extension;
- one native FastDB library; and
- one native Python binding library.

Both source and installed-wheel tests prove:

- clean top-level exports;
- `fastdb4py.payload` import and execution;
- removed module import failure;
- `RecordEngine` behavior and absence of the old name;
- standalone `ObjectEngine`, view-owner/materialization, and serializer
  behavior; and
- CLI Core codegen from the installed artifact.

The supported floor remains Python 3.10.

### 14.2 TypeScript/Wasm

The npm inventory and packed-package tests prove:

- the root has no call-db exports or file;
- `./payload` remains exported with its Wasm runtime;
- portable disposal/lifetime and generated code still execute; and
- standalone feature/ORM/table/serializer exports remain available only under
  their documented standalone meaning.

### 14.3 Native and ABI

Native and Wasm exact symbol gates remain exactly 117. P5 may remove legacy
C++ class symbols, but it cannot add, remove, or change an
`fdb_payload_v1_*` symbol.

## 15. Executable P5 quality contract

P5 adds a repository-owned standard-library checker and focused unit tests.
The checker fail-closes at least:

1. exact native and Wasm ABI-117 references;
2. absence of removed source files and package exports;
3. presence of `record_engine.py` and absence of the old module/name;
4. no binding-owned portable parser/digest/layout/binary/codegen source;
5. no old C++ scratch/final-backing declarations or SWIG wrappers;
6. zero-warning Python package policy;
7. exact current-surface forbidden-term results with an exact historical
   allowlist;
8. standalone boundary labels for `ObjectEngine`, feature metadata,
   view-owner/materialization, and `FastSerializer`;
9. Core-only CLI delegation and safe destination rules;
10. current README, schema index, package exports, examples, and workflow
    integration;
11. unchanged package versions and no release claim;
12. accurate Issue 0002/0003 status and hosted-pending wording; and
13. a clean tracked worktree/index at closure.

The checker is evidence only after its own adversarial unit tests prove that
each required omission, forbidden term, stale warning, broad allowlist,
version drift, or false closure state is rejected.

## 16. Implementation slices

The executable sequence is defined in the companion
[implementation plan](../plans/2026-07-23-portable-payload-clean-cut.md).
It proceeds as independent owner slices:

1. freeze this P5 inventory, design, plan, and Issue handoff;
2. replace Python-owned codegen with the Core-only CLI facade;
3. remove Python call-db/schema/require/allocator authority and public tests;
4. remove TypeScript call-db authority and package exports;
5. remove orphaned native allocator/final-backing classes, repair standalone
   descriptor determinism, and close SWIG warnings;
6. perform the clean `RecordEngine` file/class/docs/test/benchmark rename;
7. make retained standalone boundaries and exact package/forbidden-term gates
   executable; and
8. run complete fresh release-readiness gates, fix every material review
   finding, and freeze the P5 handoff.

Every implementation task starts with an ignored brief and a genuine failing
test. No task uses an absent legacy test as proof that the replacement still
works.

## 17. Review and closure

Each task receives two mechanically frozen primary-agent review passes:

1. specification, authority, ABI, clean-cut, parity, and documentation
   compliance; and
2. correctness, lifetime/resource safety, portability, security, package
   quality, and maintainability.

This is intentionally the context-owning primary agent's review and is not
represented as independent or subagent evidence.

P5 closes locally only when:

- every planned removal and rename is executable and package-visible;
- all retained standalone boundaries are accurate;
- Issue 0003 is closed by a warning-free package build;
- the legacy descriptor nondeterminism is closed by owner-layer tests;
- full Debug, Release, ASan+UBSan, available TSan, corpus/fuzz, Wasm, Rust,
  Python, Python 3.10, TypeScript, generated-output, package, schema,
  documentation, ABI, and clean-worktree gates pass after the final fix;
- the final frozen review has no unresolved Critical, Important, or material
  Minor finding; and
- Issue 0002 records P5 commits and exact local evidence while keeping hosted,
  version, tag, publication, release, and downstream composition facts open.

The package version remains the current 0.1.x value during this Goal. P5
closure means “local clean target and release-ready owner boundary,” not
“FastDB 0.2.0 published.”
