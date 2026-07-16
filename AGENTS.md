# AGENTS.md

This file contains repository-specific guidance for Codex and other coding agents working on FastDB. Follow it together with the user's current request.

## Project Overview

FastDB is a compact binary data layer for scientific-computing and RPC payload workflows. The accepted 0.2.0 target is defined by:

1. `docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md`
2. `docs/decisions/0001-portable-payload-core-authority.md`
3. `docs/issues/0001-portable-payload-deferred-capabilities.md`

Read those documents before portable-payload architecture or implementation work. They are normative even while the worktree still contains 0.1.x call-db and `ColumnEngine` code awaiting the clean-cut migration.

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

# Existing helper script path
./py_utils.sh --build
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

The existing `ColumnEngine` is a 0.1.x AoS record path with strided field access, not true columnar storage. The accepted 0.2.0 public name is `RecordEngine`, with no compatibility alias. `ObjectEngine` remains the object-graph engine name. Do not extend `ColumnEngine` or add new `columnar.v1` surfaces while migrating. Shared standalone table behavior currently belongs in `python/fastdb4py/orm/table.py`.

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

The existing public call-db schemas/modules and Python/TypeScript semantic duplication are migration sources only. Do not add compatibility aliases or new users; remove them before 0.2.0 as required by ADR-0001.

## Release Process

For a Python package release:

1. Bump `[project].version` in `pyproject.toml`.
2. Refresh `uv.lock` so the editable `fastdb4py` package version matches.
3. Update the `fastdb4py` section in `CHANGELOG.md`.
4. Verify the tag `py/v<version>` does not already exist.
5. Run `uv run pytest tests/python -q`, `uv run python -m compileall -q python/fastdb4py tests/python`, `git diff --check`, and `uv build`.

The PyPI workflow publishes only when `pyproject.toml` changes and the target `py/v<version>` tag is absent. Avoid mixing unrelated benchmark or exploratory changes into a release PR unless the user explicitly wants them included.

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
