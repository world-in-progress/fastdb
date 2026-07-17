# Issue 0002: Portable Payload Foundation Implementation Status

- **Status:** Open
- **Opened:** 2026-07-16
- **Owner:** FastDB
- **Governing design:** [FastDB Portable Payload Foundation Design](../superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
- **Governing decision:** [ADR-0001](../decisions/0001-portable-payload-core-authority.md)
- **First implementation plan:** [Portable Payload Core Contract Implementation Plan](../superpowers/plans/2026-07-16-portable-payload-core-contract.md)
- **Post-0.2 deferrals:** [Issue 0001](0001-portable-payload-deferred-capabilities.md)

## Purpose

The accepted 0.2.0 design defines a complete portable-payload foundation, while the current repository still ships the 0.1.x call-db/`ColumnEngine` implementation and has not implemented `fastdb.payload.v1`. This issue records that temporary implementation gap from the first plan through the 0.2.0 clean cut.

It is not a mechanism for shrinking the accepted milestone. Capabilities intentionally outside 0.2.0 belong in Issue 0001. Every item below remains non-deferrable for the 0.2.0 foundation and must be removed from this issue by implementation, not reclassified to make a release claim pass.

## Current implemented surface

Current repository state:

- the target architecture, owner boundary, source algebra, identity pipeline, runtime model, C ABI direction, testing obligations, and clean-cut decision are accepted documentation;
- the first executable plan is written for the Core compiler/query contract;
- P1 Tasks 1 and 2 are reviewed and complete: the repository has pinned yyjson, double-conversion, and PicoSHA2 source snapshots, a native CTest seam, Core-owned immutable `JsonValue`, RFC 6901 JSON Pointer construction, RFC 8785 JCS serialization, and SHA-256 identity primitives;
- `JsonValue` lifetime/release and JCS traversal are iterative, with committed array-focused tests at 50,000 levels for serialization, destruction, failure cleanup, and copy/move lifetime behavior;
- at the start of Task 3, strict source parsing/compiler semantics, the stable public C ABI, binary/runtime behavior, bindings, and code generation were all still absent;
- P1 Task 3 now ships only the pure-C ABI base: exact-width constants and V1 struct initializers, opaque spec/blob/error declarations, ABI version query, immutable owned blob/error handles, atomic retain/release, stable error field queries, and an exception-to-owned-error boundary;
- P1 Task 4 adds strict duplicate-aware JSON document parsing, iterative source/value/depth enforcement, borrowed source-order cursors, generic audited-document conversion for schema/JCS uses, and a Core-owned repository for the embedded `fastdb.payload.v1` source schema's JCS bytes and SHA-256 digest;
- the Task 3 ABI base does **not** expose `fdb_payload_v1_spec_compile_json` or any spec query family. Typed source parsing, normalization, resolved indexes/manifest/capabilities, and the complete public spec compile/query ABI remain absent until later P1 tasks;
- no `fastdb.payload.bin.v1`, builder/plan/backing, payload owner, checked view, materialization, object-graph runtime, portable language projection, or payload code generator is currently shipped;
- current public call-db, `fastdb.schema.v1`, `columnar.v1`, and `ColumnEngine` surfaces remain 0.1.x migration inputs, not the accepted 0.2.0 authority.

The repository therefore must not claim that the portable payload foundation or FastDB 0.2.0 is implemented.

## Current limit

Users and downstream repositories cannot yet compile or consume `fastdb.payload.v1` through a stable library boundary. C-Two cannot safely wrap the planned FastDB sub-spec without either depending on unimplemented interfaces or recreating FastDB semantics, which is forbidden. Toodle consequently cannot treat this target design as a working structured-payload substrate yet.

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

## P1 observations that remain open

These are implementation-gate observations, not post-0.2.0 deferrals.

### Deep object and shared-child regression coverage

**Current limit:** The committed 50,000-level regression matrix is array-focused. During Task 2 review, deep-object traversal and shared-child DAG lifetime behavior passed reviewer-only probes, but those probes are not committed tests.

**Reason:** Task 2 closed the concrete recursive destruction and JCS traversal failures with the smallest durable array-chain matrix. The reviewer then checked adjacent object and shared-node shapes without expanding that reviewed task after its implementation gate.

**Impact:** The iterative algorithms have direct review evidence for those adjacent shapes, but the repository would not automatically catch a future regression that affects only deep objects or repeated references to one immutable child.

**Closure criteria:** Before the P1 final gate, commit regression tests that exercise deep object serialization/destruction and shared-child DAG copy/move/release behavior at a depth and fan-out sufficient to fail a recursive or double-release implementation. Run them in Debug, Release, and the native sanitizer job.

### Pre-existing SWIG diagnostics

**Current limit:** The Python wheel build succeeds while SWIG reports `Warning 325` for nested `TileBox` (line 582), `HandleTileAction` (588), `TakeResult` (595), `TileDataHandle` (622), `TileDbBox` (631), and the second `TakeResult` (637) in `fastcarto/fastdb/include/fastdb.h`; it also reports `Warning 451` for the settable `const char *` member at line 206.

**Reason:** These diagnostics come from the legacy 0.1.x SWIG input surface: SWIG does not project those nested declarations, and its generated setter cannot prove ownership for the character pointer. Task 3 does not add `fastdb_payload.h` to that SWIG surface.

**Impact:** Existing wheels still build, and the new payload C ABI is unaffected, but ignored nested declarations and an ownership warning remain noisy enough to conceal a new binding regression.

**Closure criteria:** Before the P1 final gate, either fix the legacy SWIG declarations/typemaps and require a quiet build, or open an exact owner issue that enumerates each accepted residual diagnostic with its rationale, impact, and closure gate. No new SWIG warning may be normalized into this list implicitly.

## P1 observations closed during implementation

### Planned schema manifest globs

**Closed by Task 4:** The repository now contains
`schemas/fastdb.payload.v1.schema.json`, its Core-JCS digest pin
`schemas/fastdb.payload.v1.schema.sha256`, and `schemas/README.md`. The source
schema is also embedded as raw bytes by the deterministic standard-library-only
generator.

**Evidence:** `uv build` produced `dist/fastdb4py-0.1.22.tar.gz` and the local
wheel without any no-match warning for `schemas/*.json`, `*.sha256`, or `*.md`.
The sdist inventory contains all three exact paths. The separately tracked SWIG
325/451 diagnostics above remain present and open; they were not normalized as
part of this closure.
`python3 tools/generate_embedded_payload_schemas.py --check` passes, and
`payload.json_document` verifies that the embedded raw
schema parses through `JsonDocument`, Core JCS/SHA-256 matches the checked-in
pin, and the schema declares draft 2020-12 with a closed root shape. The pin was
independently produced from 2,074 canonical bytes by the verification-only
Python `rfc8785` 0.1.4 implementation before comparison with the Core result.

## Program checklist

| Slice | Status | Required closure evidence |
|---|---|---|
| P1. Core contract compiler/query ABI | In progress: Tasks 1-3 reviewed; Task 4 strict JSON/schema repository implemented | Complete typed source algebra; normalization; resolved indexes/manifest/capabilities; complete spec compile/query C ABI; C++ facade; committed deep-object/DAG regressions; golden/fuzz/native CI |
| P2. Record binary/runtime/lifetime | Blocked on P1 | Exact binary layout document and goldens; builder/plan/backing; deterministic record build/open; checked views/materialize/invalidate for every legal non-`ref` type |
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
