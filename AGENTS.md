# AGENTS.md

This file contains repository-specific guidance for Codex and other coding agents working on FastDB. Follow it together with the user's current request.

## Project Overview

FastDB is a compact binary data layer for scientific-computing and RPC payload workflows. The repository has three layers:

- C++ core under `fastcarto/fastdb/`: owns binary layout, table/feature storage, buffer ownership, and native read/write behavior.
- Python binding under `python/fastdb4py/`: owns Python `@feature`, `ColumnEngine`, `ObjectEngine`, `Table`, schema export, materialization, and Python-facing backed view lifetime APIs.
- TypeScript/WASM binding under `ts/fastdb4ts/`: owns browser-side schema/runtime APIs and WASM access to the native core.

FastDB is a generic data/storage project. Do not add C-Two-specific modules, providers, bridge derivation, CRM contracts, route identity, relay behavior, or C-Two codegen surfaces here. C-Two may consume FastDB schemas and buffers, but C-Two-specific RPC planning belongs in the C-Two repository.

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

`ColumnEngine` is the columnar/batch path and does not support REF fields. `ObjectEngine` is the object-graph path and supports references. Shared table behavior belongs in `python/fastdb4py/orm/table.py`.

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

`fastdb.schema.v1` and generic Python-to-TypeScript feature codegen belong in FastDB. C-Two contract helpers, call-db envelopes, route fingerprints, relay integration, and CRM-specific TypeScript helper generation do not belong in FastDB.

The FastDB `fdb` CLI should remain generic. If a feature needs C-Two semantics, put it in C-Two and consume FastDB schema artifacts from there.

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
- Prefer `rg` and `rg --files` for repository searches.
- Use `apply_patch` for manual edits.
- Do not revert unrelated user changes in a dirty worktree.
- Keep Python 3.10 compatibility unless the project explicitly raises the minimum version.
- Keep PRs scoped by layer: Python lifetime/API changes, C++ storage changes, TypeScript/WASM changes, benchmark experiments, and release metadata should be separate when practical.
- When C++ binary layout changes, revalidate Python and TypeScript consumers together.
