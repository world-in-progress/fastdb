# AGENTS.md

This file contains repository-specific guidance for Codex and other coding agents working on FastDB. Follow it together with the user's current request.

## Project Overview

FastDB is a compact binary data layer for scientific-computing and RPC payload workflows. The accepted 0.2.0 target is defined by:

1. `docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md`
2. `docs/decisions/0001-portable-payload-core-authority.md`
3. `docs/issues/0001-portable-payload-deferred-capabilities.md`

Read those documents before portable-payload architecture or implementation
work. They remain normative while the remaining 0.1.x documentation and
standalone-policy surfaces await the rest of the clean-cut migration.

The target repository layers are:

- C++ Core under `fastcarto/fastdb/`: sole authority for `fastdb.payload.v1`, canonical identity, binary layout, build/open, backing, lifetime, errors, and codegen.
- Stable C ABI under `fastcarto/fastdb/include/fastdb_payload.h`: versioned opaque-handle boundary for every native and WASM projection.
- C++ RAII facade, Rust raw/safe crates, Python binding, and TypeScript/WASM binding: ergonomic projections over the Core, never separate semantic implementations.

FastDB is a generic data/storage project. Do not add C-Two-specific modules, CRM methods/bindings, providers, bridge derivation, contract assembly, route identity, relay behavior, transport/lease policy, or C-Two codegen surfaces here. C-Two owns the `c-two.contract.v2` super-schema and delegates its nested FastDB value to this library.

## Build, Test, And Run

Use `uv` for Python workflows.

```bash
# Python tests
uv run pytest tests/python -q

# Focused Python tests
uv run pytest tests/python/test_view_owner_lifetime.py -q
uv run pytest tests/python/test_materialize.py tests/python/test_reader.py -q

# Python syntax/import sanity
uv run python -m compileall -q python/fastdb4py tests/python

# Build Python sdist and local wheel
uv build

# Initial environment setup helper (not a forced native rebuild)
./py_utils.sh --setup

# Force a fresh editable native binding, then restore locked dependencies
uv pip install --reinstall -e .
uv sync
./py_utils.sh --test

# TypeScript/WASM
bash ts/build-wasm.sh
npm --prefix ts/fastdb4ts run build
npm run test:ts
```

When a change touches only Python code and docs, run the Python test suite, compileall, and `uv build`. When a change touches C++ wire/storage layout or TypeScript/WASM code, also run the TypeScript/WASM build and tests.

## Python Binding Conventions

Import the package as `fastdb4py` or `import fastdb4py as fdb` in examples and tests.

`@feature` classes are plain Python classes with annotations. Owned instances use normal `__dict__` semantics. Backed table rows are view objects that read and write through native storage when writeable.

`python/fastdb4py/core/` is generated/native binding output. Prefer changes in `python/fastdb4py/` unless the SWIG bridge or C++ API itself must change.

`RecordEngine` is the standalone AoS record path with strided field access,
not true columnar storage. It is exposed under that final name without a
compatibility alias for the pre-0.2 name. `ObjectEngine` remains the
object-graph engine name. Do not restore a removed pre-clean-cut profile.
Shared standalone table behavior currently belongs in
`python/fastdb4py/orm/table.py`.

## Backed View Lifetime Model

FastDB owns the generic value lifetime model:

- `FdbViewOwner(checked=True, writeable=...)` represents a call-scoped or lease-scoped owner.
- `fdb.invalidate(owner_or_view)` must make checked table, row, string-column, bytes-column, and numeric-column views fail on later read/write.
- `fdb.materialize(value)` and `value.to_owned()` detach FastDB-managed views before the caller retains data beyond a backing-buffer lifetime.
- Standalone FastDB remains trusted by default: `db.table(Point)` may return raw NumPy column views for performance.
- `table(..., writeable=False)` must enforce read-only behavior even if no checked owner is provided.
- `unsafe_numpy_view()` is an explicit trusted escape hatch. It cannot be revoked after returning a raw NumPy view and must not be used as the default integration path for reusable memory leases.

Do not replace these FastDB-owned semantics with C-Two-owned guard wrappers. Downstream systems should pass owners into FastDB views and call `fdb.invalidate(...)` when their transport lease ends.

## Schema And Codegen Boundary

`fastdb.payload.v1`, its two profiles, RFC 8785 canonicalization, SHA-256 digest, `fastdb.payload.bin.v1`, generic payload lifetime/backing, and payload-only C++/Rust/Python/TypeScript artifact generation belong in FastDB Core.

Bindings may offer authoring conveniences, but the final JSON is compiled by Core. They must not parse, normalize, digest, lay out, or decode the portable format independently. All public FastDB type names come from the native algebra (`str` and `wstr`, never a binding-owned `text` type).

C-Two owns the outer `c-two.contract.v2`, CRM binding derivation, route fingerprints, relay integration, lease/lifecycle policy, and final multi-concern code generation through `c3`. FastDB returns an in-memory payload artifact set; it does not write C-Two's destination tree.

The FastDB `fdb` CLI remains a generic diagnostic/payload tool. If a feature needs C-Two semantics, put it in C-Two and consume FastDB through the stable library boundary.

The pre-clean-cut public schemas/modules and binding-side semantic
duplication are removed. Do not restore them through compatibility aliases,
fallbacks, or new users; ADR-0001 requires the single Core authority.

## Release Process

Python, Rust, TypeScript and the Core bundle share one version and source
commit. Update all package metadata and lockfiles together, and run the
relevant local checks before opening the release PR. The exact merged source
must pass both `tests.yml` and `release-artifacts.yml` on `main`.

Python publication uses `pypi_publish.yml`. A `v<version>` tag push, or manual
`operation=publish` from `main`, downloads the exact tag source's unexpired
`release-part-python` artifact from a complete successful main CI run. It
checks the source-bound fragment hashes and the complete 35-wheel-plus-sdist
inventory before upload. Publication never rebuilds missing or expired CI
artifacts. The publishing job uses GitHub OIDC with no environment declared.

Release Artifacts builds candidates on `main` when package inputs change;
PRs and changes limited to release tooling or documentation run lightweight
checks. The runtime CI workflow retains its required validation gates. A
release source must still have both complete CI proofs and its own candidate;
a metadata-only run cannot serve as release evidence.

Manual `operation=backfill` from `main` takes an existing `tag` and uses the
current workflow with that tag's exact source and CI proofs. Before allocating
builders, it compares the published sdist source, verifies reusable PyPI
files by size and SHA-256, and selects only missing wheel slots. It reuses the
sdist, checks the combined complete inventory, then uploads only missing
filenames with `skip-existing`. A complete release allocates no builders or
upload job. PyPI's 14-day upload window is checked before building missing
files and again before staging; expired partial releases need a new version.
Do not move old tags. See [publishing and local validation](docs/releases/publishing.md)
for the matrix, guards and dispatch inputs.

The 0.2.3 Windows ARM64 matrix uses `windows-11-arm`, five Python ABIs
(`cp311`, `cp312`, `cp313`, `cp314`, `cp314t`) and an
`aarch64-pc-windows-msvc` Core bundle. NumPy's Windows ARM64 binary packages
require Python 3.11+, so the existing five platforms retain Python 3.10.
Native C++/ABI/Rust and installed-wheel tests passed for the exact 0.2.3
source; see the [release record](docs/releases/0.2.3.md). Those tests remain
required for future releases. The original `v0.2.2` source forces CMake `-A x64`;
its ARM64 backfill is rejected before building. Do not alter that tag or
build a patched source under its version. The architecture fix requires a
new release; the user selected 0.2.3 for all ecosystems. See
[Windows ARM64 preparation](docs/releases/windows-arm64.md).

Rust and npm publication is a manual workflow dispatch from that source on
`main`, or from its matching `v<version>` tag after `main` advances. Supply the
successful Release Artifacts run ID and full source SHA. The publishing
helpers validate the proof run, source identity, manifest and artifact hashes;
they upload the tested archives and only accept existing registry versions
whose hashes match. Do not rebuild packages inside upload jobs or move
an existing release tag. See the [0.2.0 release record](docs/releases/0.2.0.md)
for the complete package inventory and verified evidence.

Rust publication uses GitHub OIDC in `crates_publish.yml`, operation `publish`.
Both crates must have a Trusted Publisher for that workflow with no environment
configured. Operation `verify-oidc`, dispatched from `main`, exchanges and
revokes a temporary token without uploading packages. Publication has no
long-lived `CARGO_REGISTRY_TOKEN` secret fallback.

These operations apply to `main` after the OIDC change and later tags containing
that workflow. The immutable `v0.2.0` tag retains its original workflow with no
`operation` input and with token-secret authentication; do not move that tag to
retrofit OIDC. Its complete publication is already verified.

## Working Rules

- Read existing code before changing it.
- For portable payload work, read the accepted design, ADR, and owner issue before writing a plan or code.
- Prefer `rg` and `rg --files` for repository searches.
- Use `apply_patch` for manual edits.
- Do not revert unrelated user changes in a dirty worktree.
- FastDB is 0.x: implement the clean target and remove the wrong abstraction rather than preserving zombie compatibility, unless the user explicitly requests a migration window.
- Record every intentionally limited payload behavior in `docs/issues/` with rationale and closure criteria before merge.
- Keep the C++ Core as the only parser/canonicalizer/digest/layout/binary authority; language parity is a release gate.
- Keep Python 3.10 compatibility unless the project explicitly raises the minimum version.
- Keep PRs scoped by layer: Python lifetime/API changes, C++ storage changes, TypeScript/WASM changes, benchmark experiments, and release metadata should be separate when practical.
- When C++ portable binary or ABI behavior changes, revalidate C++, Rust, Python, and TypeScript/WASM consumers together.
