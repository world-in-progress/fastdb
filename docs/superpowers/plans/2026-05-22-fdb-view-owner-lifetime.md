# FastDB View Owner Lifetime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a FastDB-owned lifetime and aliasing model for backed views so stale row, feature, column, string, and bytes views fail after their owner is invalidated, while owned Python feature objects continue to use normal `__dict__` semantics.

**Architecture:** FastDB, not C-Two, owns the FDB value model. A mapped/backed value carries an owner and generation, checks that owner before every read/write, and becomes invalid when the owner is invalidated. A plain owned feature has no backing owner, stores fields in `__dict__`, and can be kept indefinitely. C-Two will later pass a call-scoped owner into FastDB views and call `fdb.invalidate(...)` when `cc.Held.release()` or server borrowed input cleanup runs.

**Tech Stack:** Python FastDB ORM (`python/fastdb4py`), SWIG/native FastDB table and feature bindings, pytest tests under `tests/python`, current `ColumnEngine`/`ObjectEngine` table mapping paths.

---

## Context

FastDB standalone usage is a trusted scientific-computing scenario. The primary goal is not to defend against malicious in-process code. The required model is lifetime correctness: if a view is backed by an owner that has been released or invalidated, later reuse must raise a deterministic error instead of reading stale pool memory or corrupting a reused shared-memory slot.

C-Two makes this visible because its IPC transport uses reusable shared-memory pools. A stale child view escaped from `cc.hold(...)` or `InputLifetime.BORROWED` can otherwise keep reading or writing after the C-Two lease has ended. Direct IPC pools are directional and relay HTTP copies payloads, so this is not primarily a cross-tenant confidentiality problem. It is still a correctness problem: stale reads and writes must become testable failures.

The current FastDB behavior mixes owned and backed semantics. `Table.__getitem__()` calls `bind_feature(...)`; for new `@feature` classes without `map_from`, `bind_feature(...)` currently falls back to `copy_feature(...)`. That makes row reads materialized in places where the FDB-first model expects direct backed access. Column access currently returns raw NumPy arrays for numeric columns, which bypasses any future owner checks once escaped.

## Design Rules

1. A FastDB feature instance has two modes.
   - Owned mode: no backing state; fields live in `__dict__`; reads and writes are ordinary Python object operations.
   - Mapped mode: has backing state; schema fields are read from and written to the backing table row; every read and write checks owner liveness; writes also check writeability.
2. Logical owners are not reused. A physical memory block may be reused by a pool, but a stale view must keep pointing to its old invalidated owner and fail.
3. `fdb.materialize(value)` and `value.to_owned()` detach values from any backing owner.
4. `fdb.invalidate(value)` invalidates the owner graph for FastDB-managed views. It is idempotent.
5. Safe column access must not hand out raw NumPy arrays when the table is backed by a checked owner. It should return owner-bound column view objects whose read/write methods check liveness.
6. Raw NumPy export is an explicit unsafe escape hatch. It may exist for trusted HPC hot paths, but it cannot be the default path used by C-Two held or borrowed values.
7. Existing trusted standalone workflows can keep raw fast paths for immortal/trusted owners. C-Two borrowed/held buffers must use checked owners.

## Proposed Public API

```python
import fastdb4py as fdb

rows = table[0]                  # mapped feature view if table has a checked/backed owner
rows.x                           # reads backing storage after owner check
rows.x = 1.0                     # writes backing storage after owner/writeable checks

owned = fdb.materialize(rows)    # detached owned feature
owned.x = 2.0                    # ordinary __dict__ write

col = table.column.x             # checked ColumnView for checked owners
col[0] = 3.0                     # owner/writeable checks before write
arr = col.to_numpy()             # copy
raw = col.unsafe_numpy_view()    # explicit escape hatch; no stale-error guarantee after export

fdb.invalidate(table)
rows.x                           # raises FdbViewInvalidatedError
col[0]                           # raises FdbViewInvalidatedError
```

## Proposed Internal APIs

Create `python/fastdb4py/view_owner.py`:

```python
class FdbViewInvalidatedError(RuntimeError):
    pass


class FdbViewWriteError(RuntimeError):
    pass


class FdbViewOwner:
    def __init__(self, *, checked: bool = True, writeable: bool = False, release=None):
        self.checked = checked
        self.writeable = writeable
        self._release = release
        self._alive = True
        self.generation = 0

    @property
    def alive(self) -> bool:
        return self._alive

    def assert_alive(self) -> None:
        if self.checked and not self._alive:
            raise FdbViewInvalidatedError('FastDB backed view owner has been invalidated.')

    def assert_writeable(self) -> None:
        self.assert_alive()
        if self.checked and not self.writeable:
            raise FdbViewWriteError('FastDB backed view is read-only.')

    def invalidate(self) -> None:
        if not self._alive:
            return
        self._alive = False
        release = self._release
        self._release = None
        if release is not None:
            release()
```

Create backing state for mapped feature instances:

```python
class FeatureBacking:
    def __init__(self, *, owner, db, layer, row_index, schema, writeable):
        self.owner = owner
        self.db = db
        self.layer = layer
        self.row_index = row_index
        self.schema = schema
        self.writeable = writeable

    def read_field(self, name: str):
        self.owner.assert_alive()
        # Load field from layer.tryGetFeature(row_index).

    def write_field(self, name: str, value):
        self.owner.assert_writeable()
        if not self.writeable:
            raise FdbViewWriteError('FastDB feature row is read-only.')
        # Write field to backing feature/table row.
```

Owned feature instances must not carry `FeatureBacking`. Mapped feature instances must use backing state for schema fields and must not split reads from backing with writes into `__dict__`.

## File Responsibilities

- `python/fastdb4py/view_owner.py`: owner, invalidate error classes, writeability error classes, helper functions for extracting owners from views.
- `python/fastdb4py/decorator.py`: inject owned/mapped field access behavior into `@feature` classes, preserving normal `__dict__` behavior when no backing is present.
- `python/fastdb4py/reader.py`: replace current copy fallback for backed table row access with mapped feature creation for `@feature` classes; keep `copy_feature(...)` as explicit materialization helper.
- `python/fastdb4py/orm/table.py`: store owner/writeable policy on mapped tables; assert owner in table operations; create mapped feature row views; expose checked column views for checked owners and raw NumPy for trusted owners only if explicitly intended.
- `python/fastdb4py/string_column.py`: make string and bytes columns owner-bound and invalidation-aware.
- `python/fastdb4py/materialize.py`: materialize feature, table, column, string, bytes, tuple, list, and mapping values into owned detached values.
- `python/fastdb4py/__init__.py`: export `invalidate`, `FdbViewInvalidatedError`, and possibly `FdbViewWriteError`.
- `tests/python/test_view_owner_lifetime.py`: owner invalidation tests for table, feature, row, column, string, bytes, materialize, and unsafe export behavior.
- `tests/python/test_reader.py`, `tests/python/test_column_engine.py`, `tests/python/test_string_column.py`: update existing expectations where `table[i]` should now be mapped rather than copied for backed tables.
- `README.md`: document owned vs mapped features, `materialize`, `invalidate`, checked views, and unsafe raw export.

## Implementation Tasks

### Task 1: Add Owner And Invalidation Primitives

**Files:**
- Create: `python/fastdb4py/view_owner.py`
- Modify: `python/fastdb4py/__init__.py`
- Test: `tests/python/test_view_owner_lifetime.py`

- [x] Write tests for idempotent invalidation and error classes.
- [x] Implement `FdbViewOwner`, `FdbViewInvalidatedError`, `FdbViewWriteError`, and `invalidate(value)`.
- [x] Export the new names from `fastdb4py`.
- [x] Run `uv run pytest tests/python/test_view_owner_lifetime.py -q`.

Acceptance criteria:
- `fdb.invalidate(owner_or_view)` is idempotent.
- `owner.assert_alive()` raises after invalidation.
- Existing `fdb.materialize(...)` tests still pass.

### Task 2: Make Table Owners Explicit

**Files:**
- Modify: `python/fastdb4py/orm/table.py`
- Modify: `python/fastdb4py/column_engine.py`
- Modify: `python/fastdb4py/object_engine.py`
- Test: `tests/python/test_view_owner_lifetime.py`

- [x] Add `_fdb_owner`, `_fdb_checked`, and `_fdb_writeable` fields to `Table`.
- [x] Extend `Table.map_from(...)` to accept `owner: FdbViewOwner | None = None` and `writeable: bool = False`.
- [x] If no owner is provided, create an immortal/trusted owner for standalone existing behavior.
- [x] Assert owner liveness in `Table.__len__`, `Table.__getitem__`, `Table.__iter__`, `Table.column`, and `Table.fill`.
- [x] Run table lifetime tests.

Acceptance criteria:
- A table with a checked owner raises after `owner.invalidate()`.
- Existing `ColumnEngine` and `ObjectEngine` tests pass without requiring callers to pass owners.

### Task 3: Add Mapped Feature Semantics For `@feature`

**Files:**
- Modify: `python/fastdb4py/decorator.py`
- Modify: `python/fastdb4py/reader.py`
- Test: `tests/python/test_view_owner_lifetime.py`
- Update: `tests/python/test_reader.py`

- [x] Inject feature field access behavior into `@feature` classes while preserving ordinary `__dict__` storage for owned instances.
- [x] Add a `FeatureBacking` helper that stores owner, layer, row index, schema, and writeability.
- [x] Make `bind_feature(...)` return mapped feature instances for `@feature` classes when called from backed tables.
- [x] Keep `copy_feature(...)` as explicit materialization and use it from `fdb.materialize(...)`.
- [x] Add tests where `row = table[0]`; `row.x` reads from backing storage; `fdb.invalidate(table)` makes `row.x` raise.
- [x] Add tests where `owned = fdb.materialize(row)` remains readable and writable after invalidation.

Acceptance criteria:
- Owned `@feature` instances use `__dict__` and do not require an owner.
- Mapped `@feature` instances check owner on read and write.
- Mapped writes do not silently write only to `__dict__`.

### Task 4: Add Checked Column Views

**Files:**
- Modify: `python/fastdb4py/orm/table.py`
- Modify: `python/fastdb4py/string_column.py`
- Create if useful: `python/fastdb4py/column_view.py`
- Test: `tests/python/test_view_owner_lifetime.py`
- Update: `tests/python/test_string_column.py`

- [x] Implement `NumericColumnView` with `__len__`, `__getitem__`, `__setitem__`, `__iter__`, `to_numpy()`, `to_owned()`, and `unsafe_numpy_view()`.
- [x] Implement or adapt `StringColumn` and `BytesColumn` so reads, iteration, and `to_owned()` check owner liveness for checked owners.
- [x] Make checked table column access return checked column view wrappers instead of bare NumPy arrays.
- [x] Decide whether immortal/trusted standalone tables keep returning raw NumPy arrays or also return wrappers with cheap pass-through behavior. Prefer the smallest change that keeps existing trusted performance tests meaningful.
- [x] Add tests showing `col = table.column.x`; `fdb.invalidate(table)` makes `col[0]` and `list(col)` raise.
- [x] Add tests showing `col.to_numpy()` is a detached copy and remains valid after invalidation.

Acceptance criteria:
- Safe checked column views cannot be used after owner invalidation.
- Raw NumPy export is explicit and documented as unsafe.
- Existing column write tests are updated to use the intended API for trusted or checked owners.

### Task 5: Deep Materialization And Invalidation Coverage

**Files:**
- Modify: `python/fastdb4py/materialize.py`
- Modify: `python/fastdb4py/view_owner.py`
- Test: `tests/python/test_materialize.py`
- Test: `tests/python/test_view_owner_lifetime.py`

- [x] Ensure `fdb.materialize(...)` detaches mapped feature views, tables, numeric columns, string columns, bytes columns, arrays, tuples, lists, and mappings.
- [x] Ensure `to_owned()` methods call the same materialization path.
- [x] Ensure `fdb.invalidate(...)` recurses through tuple/list/mapping containers of views when appropriate.
- [x] Add tests for nested containers containing table, feature, and column views.

Acceptance criteria:
- Materialized values remain accessible and mutable after owner invalidation.
- Invalidated FastDB-managed views raise deterministic FastDB errors.

### Task 6: Writeability And Direct Write Semantics

**Files:**
- Modify: `python/fastdb4py/reader.py`
- Modify: `python/fastdb4py/orm/table.py`
- Modify native/SWIG bindings only if current row-level field setters are insufficient.
- Test: `tests/python/test_view_owner_lifetime.py`

- [x] Add tests for mapped feature direct writes: `row.x = 1.0` writes through to backing storage when owner is writeable.
- [x] Add tests for read-only mapped feature writes raising `FdbViewWriteError`.
- [x] Add tests for stale mapped feature writes raising `FdbViewInvalidatedError`.
- [x] Implement row-level write-through using existing native APIs if available.
- [x] Confirm row-level native scalar setters are exposed; no native/SWIG gap remains for scalar direct writes in this phase.

Acceptance criteria:
- Owned feature writes remain ordinary `__dict__` writes.
- Mapped feature writes never silently update only Python object state.
- Stale writes fail before touching backing memory.

### Task 7: C-Two Integration Hook Preparation

**Files:**
- Modify: `python/fastdb4py/column_engine.py`
- Modify: `python/fastdb4py/object_engine.py`
- Test: `tests/python/test_view_owner_lifetime.py`

- [x] Add a documented path for creating a `ColumnEngine` or table from a caller-supplied owner and release callback.
- [x] Ensure the owner can call a release hook exactly once when invalidated.
- [x] Add tests with a release callback counter to prove invalidation releases once.
- [x] Do not add C-Two imports or C-Two-specific APIs to FastDB.

Acceptance criteria:
- FastDB stays provider-agnostic.
- C-Two can later pass a `FdbViewOwner(release=...)` without FastDB importing C-Two.

### Task 8: Documentation And Migration Notes

**Files:**
- Modify: `README.md`
- Optionally create: `docs/view-owner-lifetime.md`

- [x] Document owned vs mapped feature behavior.
- [x] Document `fdb.materialize(...)`, `value.to_owned()`, and `fdb.invalidate(...)`.
- [x] Document checked column views and unsafe raw NumPy export.
- [x] Document direct write behavior and writeability checks.
- [x] Document that FastDB standalone is trusted by default, while C-Two borrowed/held integration should use checked owners.

Acceptance criteria:
- Docs make clear that stale FastDB-managed views raise errors.
- Docs make clear that raw unsafe NumPy export bypasses stale-error guarantees.

## Verification

Run after each relevant phase:

```bash
uv run pytest tests/python/test_view_owner_lifetime.py -q
uv run pytest tests/python/test_materialize.py -q
uv run pytest tests/python/test_reader.py tests/python/test_column_engine.py tests/python/test_string_column.py -q
```

Run before completion:

```bash
uv run pytest tests/python -q
uv run python -m compileall -q python/fastdb4py tests/python
```

If native/SWIG bindings are changed, also run the repository's native build and any TypeScript tests that consume the affected binary artifacts.

## Non-Goals

- Do not add C-Two-specific modules or provider hooks to FastDB.
- Do not make FastDB a hostile multi-tenant sandbox.
- Do not guarantee that already-exported raw NumPy arrays can be revoked.
- Do not implement C-Two stub-supported RPC in this FastDB phase.
- Do not rewrite benchmarks except where tests/docs must stop claiming stale behavior is safe.

## Open Risks

- Existing tests and README currently describe `table.column.x` as a raw zero-copy NumPy view. A checked owner may require wrappers instead. Decide whether this is a 0.x clean break or a policy-dependent behavior.
- Row-level write-through may require native setter support that is not currently exposed through SWIG. If missing, implement column-level checked writes first and document the native gap.
- A wrapper that implements `__array__` can still allow raw ndarray escape through `np.asarray(view)`. Treat this as unsafe export or return a copy.
- Free-threading Python means owner state must avoid unsynchronized mutable races. If owner invalidation can happen across threads, guard owner state with a lock or native atomic.

## Handoff To C-Two After FastDB

After this plan is implemented in FastDB, C-Two should stop building C-Two-owned row/column guard wrappers. It should create FastDB checked owners for `cc.hold(...)` responses and `InputLifetime.BORROWED` inputs, then call `fdb.invalidate(...)` before releasing the native buffer lease. C-Two remains responsible for RPC lifetime and transport leases; FastDB remains responsible for value views, materialization, invalidation, and direct read/write behavior.
