# Direct Build (Option C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redesign the Python binding so `Feature()` allocates a row directly in a C++ layer builder, and `feature.x = 10` immediately calls C++ `setField` — eliminating all Python-side dict buffering on the write path.

**Architecture:** A process-level `GlobalORM` singleton holds one `WxLayerTableBuild` per registered `Feature` subclass. `Feature.__new__` allocates the next row (calling `add_feature_begin()`) and stores only `_row_idx`. `Feature.__setattr__` dispatches directly to C++ `set_field` with no Python dict. A `PartialBuild` helper serialises only the schema (tiny) for cross-process sharing.

**Tech Stack:** Python 3.10+, SWIG-bound C++ (`WxDatabaseBuild`, `WxLayerTableBuild`), existing `fastcarto` core (no C++ changes required), `multiprocessing.shared_memory` for meta sharing.

---

## File Map

| File | Role |
|---|---|
| `python/fastdb4py/registry.py` | `FieldDef`, `LayerSchema`, `SchemaRegistry` global |
| `python/fastdb4py/global_orm.py` | `LayerContext`, `GlobalORM` singleton |
| `python/fastdb4py/partial_build.py` | `PartialBuild`: meta export/import + collect |
| `python/fastdb4py/feature/feature_direct.py` | Redesigned `Feature` class (direct build path) |
| `tests/python/test_direct_build.py` | Core build + read tests |
| `tests/python/test_partial_build.py` | PartialBuild meta sharing + collect tests |

---

## Task 1: Create branch and file skeletons

**Files:**
- Create: `python/fastdb4py/registry.py`
- Create: `python/fastdb4py/global_orm.py`
- Create: `python/fastdb4py/partial_build.py`
- Create: `python/fastdb4py/feature/feature_direct.py`
- Create: `tests/python/test_direct_build.py`
- Create: `tests/python/test_partial_build.py`

- [ ] **Step 1: Create branch**

```bash
cd /path/to/fastdb
git checkout -b redesign/direct-build
```

- [ ] **Step 2: Create skeleton files**

```bash
touch python/fastdb4py/registry.py
touch python/fastdb4py/global_orm.py
touch python/fastdb4py/partial_build.py
touch python/fastdb4py/feature/feature_direct.py
touch tests/python/test_direct_build.py
touch tests/python/test_partial_build.py
```

- [ ] **Step 3: Commit skeleton**

```bash
git add python/fastdb4py/registry.py python/fastdb4py/global_orm.py \
        python/fastdb4py/partial_build.py \
        python/fastdb4py/feature/feature_direct.py \
        tests/python/test_direct_build.py tests/python/test_partial_build.py
git commit -m "chore: add skeleton files for redesign/direct-build

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 2: SchemaRegistry — field definitions and type registration

**Files:**
- Modify: `python/fastdb4py/registry.py`

### Design

`FieldDef` holds the static description of one field. `LayerSchema` holds all fields for a Feature type plus O(1) name→field_id lookup. `SchemaRegistry` is a process-level singleton that maps `Type` → `LayerSchema`, populated lazily on first use.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_direct_build.py
from python.fastdb4py.registry import SchemaRegistry, LayerSchema, FieldDef
from python.fastdb4py.type import OriginFieldType

class _Point:
    __annotations__ = {'x': None, 'y': None}  # filled below

def test_schema_registry_registers_type():
    from fastdb4py.type import F64, U32
    class Point:
        x: F64
        y: F64
        label: 'str'  # native str -> OriginFieldType.str

    registry = SchemaRegistry()
    schema = registry.get_or_register(Point)

    assert isinstance(schema, LayerSchema)
    assert schema.layer_name == 'Point'
    assert len(schema.fields) == 3
    assert schema.fields[0].name == 'x'
    assert schema.fields[0].field_type == OriginFieldType.f64
    assert schema.fields[0].field_id == 0
    assert schema.field_id('y') == 1
    assert schema.field_id('label') == 2
```

- [ ] **Step 2: Run test — verify it fails**

```bash
uv run pytest tests/python/test_direct_build.py::test_schema_registry_registers_type -v
```
Expected: `ImportError` or `ModuleNotFoundError` (registry.py is empty)

- [ ] **Step 3: Implement `registry.py` (part 1 — data classes)**

```python
# python/fastdb4py/registry.py
from __future__ import annotations
from dataclasses import dataclass, field
from threading import Lock
from typing import Dict, List, Type, Optional
from .type import OriginFieldType, get_origin_type, FIELD_TYPE_MAP
import inspect


@dataclass(frozen=True)
class FieldDef:
    """Static description of a single Feature field."""
    name: str
    field_type: OriginFieldType   # e.g. OriginFieldType.f64
    field_id: int                  # 0-based column index within the layer
    cpp_type: int                  # raw int for WxLayerTableBuild.add_field()
    ref_target: Optional[Type] = None   # for REF fields: the target Feature class
    list_elem_type: Optional[OriginFieldType] = None  # for LIST fields


@dataclass
class LayerSchema:
    """All field definitions for one Feature type."""
    layer_name: str
    fields: List[FieldDef]
    _name_to_id: Dict[str, int] = field(default_factory=dict, repr=False)

    def __post_init__(self):
        self._name_to_id = {f.name: f.field_id for f in self.fields}

    def field_id(self, name: str) -> int:
        return self._name_to_id[name]

    def get(self, name: str) -> Optional[FieldDef]:
        fid = self._name_to_id.get(name)
        return self.fields[fid] if fid is not None else None
```

- [ ] **Step 4: Implement `registry.py` (part 2 — SchemaRegistry)**

Append to `registry.py`:

```python
class SchemaRegistry:
    """Process-level registry: maps Feature type -> LayerSchema."""

    def __init__(self):
        self._schemas: Dict[Type, LayerSchema] = {}
        self._lock = Lock()

    def get_or_register(self, feature_type: Type) -> LayerSchema:
        schema = self._schemas.get(feature_type)
        if schema is not None:
            return schema
        with self._lock:
            schema = self._schemas.get(feature_type)
            if schema is None:
                schema = self._build_schema(feature_type)
                self._schemas[feature_type] = schema
        return schema

    def _build_schema(self, feature_type: Type) -> LayerSchema:
        from .type import get_list_element_type, LIST_ELEM_CPP_TYPE
        import typing

        annotations = {}
        for cls in reversed(feature_type.__mro__):
            annotations.update(getattr(cls, '__annotations__', {}))

        fields: List[FieldDef] = []
        for idx, (name, hint) in enumerate(annotations.items()):
            if name.startswith('_'):
                continue
            origin_type = get_origin_type(hint)
            if origin_type == OriginFieldType.unknown:
                continue

            ref_target = None
            list_elem_type = None
            cpp_type = origin_type.value

            if origin_type == OriginFieldType.ref:
                # Direct Feature subclass reference
                if isinstance(hint, type):
                    ref_target = hint
                cpp_type = OriginFieldType.ref.value  # 11

            elif origin_type == OriginFieldType.list:
                list_elem_type = get_list_element_type(hint)
                cpp_elem_int = LIST_ELEM_CPP_TYPE.get(list_elem_type, 8)
                cpp_type = cpp_elem_int  # stored for add_list_field call

            fields.append(FieldDef(
                name=name,
                field_type=origin_type,
                field_id=idx,
                cpp_type=cpp_type,
                ref_target=ref_target,
                list_elem_type=list_elem_type,
            ))

        return LayerSchema(
            layer_name=feature_type.__name__,
            fields=fields,
        )


# Process-level singleton
_global_schema_registry = SchemaRegistry()


def get_schema(feature_type: Type) -> LayerSchema:
    """Public accessor for the global registry."""
    return _global_schema_registry.get_or_register(feature_type)
```

- [ ] **Step 5: Run test — verify it passes**

```bash
uv run pytest tests/python/test_direct_build.py::test_schema_registry_registers_type -v
```
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add python/fastdb4py/registry.py tests/python/test_direct_build.py
git commit -m "feat(registry): add SchemaRegistry, LayerSchema, FieldDef

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 3: LayerContext — sequential C++ builder per type

**Files:**
- Modify: `python/fastdb4py/global_orm.py`

### Design

One `LayerContext` per Feature type. Wraps a `WxLayerTableBuild`. Maintains `_open_row: int | None` — at most ONE open (uncommitted) feature per layer at a time. `new_feature()` auto-commits the previously open row before starting the next.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_direct_build.py  (append)
from fastdb4py.global_orm import LayerContext
from fastdb4py import core

def _make_layer_build(name: str):
    db = core.WxDatabaseBuild()
    db.begin("")
    lb = db.create_layer_begin(name)
    return lb, db  # must keep db alive

def test_layer_context_new_feature_returns_sequential_ids():
    lb, _db = _make_layer_build("T")
    lb.add_field("x", 8)  # f64=8
    ctx = LayerContext(lb)
    r0 = ctx.new_feature()
    ctx.set_field(r0, 0, 1.0)
    r1 = ctx.new_feature()   # auto-commits r0
    ctx.set_field(r1, 0, 2.0)
    ctx.commit_all()
    assert r0 == 0
    assert r1 == 1

def test_layer_context_disallows_stale_set():
    lb, _db = _make_layer_build("T2")
    lb.add_field("x", 8)
    ctx = LayerContext(lb)
    r0 = ctx.new_feature()
    r1 = ctx.new_feature()   # auto-commits r0
    import pytest
    with pytest.raises(AssertionError):
        ctx.set_field(r0, 0, 99.0)   # r0 no longer open
    ctx.commit_all()
```

- [ ] **Step 2: Run tests — verify they fail**

```bash
uv run pytest tests/python/test_direct_build.py::test_layer_context_new_feature_returns_sequential_ids \
              tests/python/test_direct_build.py::test_layer_context_disallows_stale_set -v
```
Expected: `ImportError`

- [ ] **Step 3: Implement `LayerContext` in `global_orm.py`**

```python
# python/fastdb4py/global_orm.py
from __future__ import annotations
from typing import Optional
from . import core


class LayerContext:
    __slots__ = ('_layer_build', '_open_row', '_row_count')

    def __init__(self, layer_build):
        self._layer_build = layer_build
        self._open_row: Optional[int] = None
        self._row_count: int = 0

    def new_feature(self) -> int:
        if self._open_row is not None:
            self._layer_build.add_feature_end()
            self._open_row = None
        row_idx = self._row_count
        self._row_count += 1
        self._open_row = row_idx
        self._layer_build.add_feature_begin()
        return row_idx

    def set_field(self, row_idx: int, field_id: int, value) -> None:
        assert row_idx == self._open_row, (
            f"set_field: row {row_idx} not open (open={self._open_row}). "
            "Call new_feature() for a different row or finish the current one first."
        )
        self._layer_build.set_field(field_id, value)

    def set_field_cstring(self, row_idx: int, field_id: int, value: str) -> None:
        assert row_idx == self._open_row
        self._layer_build.set_field_cstring(field_id, value)

    def set_field_list_numeric(self, row_idx: int, field_id: int, buf) -> None:
        assert row_idx == self._open_row
        self._layer_build.set_field_list_numeric(field_id, buf, len(buf.tobytes()))

    def set_field_list_refs(self, row_idx: int, field_id: int, refs, count: int) -> None:
        assert row_idx == self._open_row
        self._layer_build.set_field_list_refs(field_id, refs, count)

    def set_field_ref(self, row_idx: int, field_id: int, ref) -> None:
        assert row_idx == self._open_row
        self._layer_build.set_field(field_id, ref)

    def commit(self, row_idx: int) -> None:
        if self._open_row == row_idx:
            self._layer_build.add_feature_end()
            self._open_row = None

    def commit_all(self) -> None:
        if self._open_row is not None:
            self._layer_build.add_feature_end()
            self._open_row = None

    @property
    def row_count(self) -> int:
        return self._row_count
```

- [ ] **Step 4: Run tests — verify they pass**

```bash
uv run pytest tests/python/test_direct_build.py::test_layer_context_new_feature_returns_sequential_ids \
              tests/python/test_direct_build.py::test_layer_context_disallows_stale_set -v
```
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/global_orm.py tests/python/test_direct_build.py
git commit -m "feat(global_orm): add LayerContext sequential C++ builder

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 4: GlobalORM — process-level singleton

**Files:**
- Modify: `python/fastdb4py/global_orm.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_direct_build.py  (append)
from fastdb4py.global_orm import GlobalORM
from fastdb4py.type import F64

def test_global_orm_basic():
    class Pt:
        x: F64
        y: F64

    orm = GlobalORM()
    row0, ctx = orm.new_feature(Pt)
    ctx.set_field(row0, 0, 3.14)
    ctx.set_field(row0, 1, 2.71)
    row1, ctx = orm.new_feature(Pt)
    ctx.set_field(row1, 0, 1.0)
    ctx.set_field(row1, 1, 2.0)

    db = orm.finalize([Pt])
    layer = db.get_layer(0)
    feat = layer.tryGetFeatureAt(0)
    assert abs(feat.get_field_as_float(0) - 3.14) < 1e-9

def test_global_orm_layer_index_stable():
    class A:
        v: F64
    class B:
        v: F64
    orm = GlobalORM()
    orm.new_feature(A)
    orm.new_feature(B)
    assert orm.layer_index_of(A) == 0
    assert orm.layer_index_of(B) == 1
```

- [ ] **Step 2: Run tests — verify they fail**

```bash
uv run pytest tests/python/test_direct_build.py::test_global_orm_basic \
              tests/python/test_direct_build.py::test_global_orm_layer_index_stable -v
```
Expected: `ImportError`

- [ ] **Step 3: Append `GlobalORM` to `global_orm.py`**

```python
# python/fastdb4py/global_orm.py  (append after LayerContext)
import numpy as np
from threading import Lock
from typing import Dict, List, Type, Tuple
from .registry import get_schema, OriginFieldType


class GlobalORM:
    """One WxDatabaseBuild + one LayerContext per Feature type."""

    def __init__(self):
        self._db_build = core.WxDatabaseBuild()
        self._db_build.begin("")
        self._contexts: Dict[Type, LayerContext] = {}
        self._layer_idx: Dict[Type, int] = {}
        self._next_idx: int = 0
        self._lock = Lock()

    def new_feature(self, feature_type: Type) -> Tuple[int, LayerContext]:
        ctx = self._contexts.get(feature_type)
        if ctx is None:
            ctx = self._create_layer(feature_type)
        return ctx.new_feature(), ctx

    def layer_index_of(self, feature_type: Type) -> int:
        return self._layer_idx[feature_type]

    def finalize(self, feature_types: List[Type]) -> 'core.WxDatabase':
        for ft in feature_types:
            ctx = self._contexts.get(ft)
            if ctx:
                ctx.commit_all()
        stream = core.WxMemoryStream()
        self._db_build.post(stream)
        buf = stream.data().as_array(np.uint8).tobytes()
        db = core.WxDatabase.load_xbuffer(buf)
        db._buffer = buf
        return db

    def reset(self) -> None:
        self._db_build = core.WxDatabaseBuild()
        self._db_build.begin("")
        self._contexts.clear()
        self._layer_idx.clear()
        self._next_idx = 0

    def _create_layer(self, feature_type: Type) -> LayerContext:
        with self._lock:
            if feature_type in self._contexts:
                return self._contexts[feature_type]
            schema = get_schema(feature_type)
            lb = self._db_build.create_layer_begin(schema.layer_name)
            for fd in schema.fields:
                if fd.field_type == OriginFieldType.list:
                    lb.add_list_field(fd.name, fd.cpp_type)
                else:
                    lb.add_field(fd.name, fd.cpp_type)
            ctx = LayerContext(lb)
            self._contexts[feature_type] = ctx
            self._layer_idx[feature_type] = self._next_idx
            self._next_idx += 1
            return ctx


_global_orm = GlobalORM()

def get_global_orm() -> GlobalORM:
    return _global_orm
```

- [ ] **Step 4: Run tests — verify they pass**

```bash
uv run pytest tests/python/test_direct_build.py::test_global_orm_basic \
              tests/python/test_direct_build.py::test_global_orm_layer_index_stable -v
```
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/global_orm.py tests/python/test_direct_build.py
git commit -m "feat(global_orm): add GlobalORM singleton

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 5: Feature.__new__ and scalar __setattr__

**Files:**
- Modify: `python/fastdb4py/feature/feature_direct.py`

### Design

`Feature.__new__` calls `get_global_orm().new_feature(cls)` and stores `_row_idx` + `_layer_ctx` in slots. `__setattr__` for non-private names looks up the schema and calls the appropriate `layer_ctx` method directly. Private names (`_*`) fall through to `object.__setattr__`.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_direct_build.py  (append)
from fastdb4py.feature.feature_direct import Feature
from fastdb4py.global_orm import GlobalORM, get_global_orm
from fastdb4py.type import F64, U32, STR

def test_feature_new_allocates_row():
    class Point(Feature):
        x: F64
        y: F64

    orm = GlobalORM()
    # patch global orm for isolation
    import fastdb4py.feature.feature_direct as fd
    old = fd._get_orm
    fd._get_orm = lambda: orm

    p = Point()
    assert p._row_idx == 0

    p2 = Point()
    assert p2._row_idx == 1

    fd._get_orm = old  # restore

def test_feature_scalar_setattr_direct():
    class Sample(Feature):
        val: F64
        count: U32

    orm = GlobalORM()
    import fastdb4py.feature.feature_direct as fd
    old = fd._get_orm
    fd._get_orm = lambda: orm

    s = Sample()
    s.val = 42.0
    s.count = 7

    db = orm.finalize([Sample])
    layer = db.get_layer(0)
    feat = layer.tryGetFeatureAt(0)
    assert abs(feat.get_field_as_float(0) - 42.0) < 1e-9
    assert feat.get_field_as_int(1) == 7

    fd._get_orm = old
```

- [ ] **Step 2: Run tests — verify they fail**

```bash
uv run pytest tests/python/test_direct_build.py::test_feature_new_allocates_row \
              tests/python/test_direct_build.py::test_feature_scalar_setattr_direct -v
```
Expected: `ImportError`

- [ ] **Step 3: Implement `feature_direct.py`**

```python
# python/fastdb4py/feature/feature_direct.py
from __future__ import annotations
from typing import Type
from ..registry import get_schema, OriginFieldType, FieldDef


def _get_orm():
    from ..global_orm import get_global_orm
    return get_global_orm()


_NUMERIC_TYPES = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.u8n, OriginFieldType.u16n,
    OriginFieldType.f32, OriginFieldType.f64,
))


class Feature:
    """
    Direct-build Feature. __new__ allocates a C++ row immediately.
    __setattr__ dispatches directly to WxLayerTableBuild.set_field — no Python dict.
    """
    __slots__ = ('_row_idx', '_layer_ctx', '_feature_type')

    def __new__(cls):
        obj = object.__new__(cls)
        orm = _get_orm()
        row_idx, layer_ctx = orm.new_feature(cls)
        object.__setattr__(obj, '_row_idx', row_idx)
        object.__setattr__(obj, '_layer_ctx', layer_ctx)
        object.__setattr__(obj, '_feature_type', cls)
        return obj

    def __setattr__(self, name: str, value):
        if name.startswith('_'):
            object.__setattr__(self, name, value)
            return

        schema = get_schema(type(self))
        fd: FieldDef | None = schema.get(name)
        if fd is None:
            raise AttributeError(f"'{type(self).__name__}' has no field '{name}'")

        ctx = object.__getattribute__(self, '_layer_ctx')
        row = object.__getattribute__(self, '_row_idx')

        if fd.field_type in _NUMERIC_TYPES:
            ctx.set_field(row, fd.field_id, value)
        elif fd.field_type == OriginFieldType.str:
            ctx.set_field_cstring(row, fd.field_id, value)
        elif fd.field_type == OriginFieldType.wstr:
            ctx.set_field_cstring(row, fd.field_id, value)
        else:
            # REF and LIST handled in later tasks; raise for now
            raise NotImplementedError(
                f"Field '{name}' type {fd.field_type} not yet supported in direct setattr"
            )

    def commit(self) -> None:
        """Explicitly commit this feature's open row."""
        ctx = object.__getattribute__(self, '_layer_ctx')
        row = object.__getattribute__(self, '_row_idx')
        ctx.commit(row)
```

- [ ] **Step 4: Run tests — verify they pass**

```bash
uv run pytest tests/python/test_direct_build.py::test_feature_new_allocates_row \
              tests/python/test_direct_build.py::test_feature_scalar_setattr_direct -v
```
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/feature/feature_direct.py tests/python/test_direct_build.py
git commit -m "feat(feature_direct): Feature.__new__ allocates C++ row, scalar __setattr__ direct

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 6: REF field — feature-to-feature assignment

**Files:**
- Modify: `python/fastdb4py/feature/feature_direct.py`

### Design

When `feature_a.vendor = feature_b`, the `__setattr__` code:
1. Commits `feature_b`'s open row (calls `ctx_b.commit(row_b)`) so its data is fully written
2. Gets `feature_b`'s layer index from GlobalORM
3. Builds a `WxFeatureRef` via `layer_ctx_a._layer_build.create_feature_ref(row_b)`... actually via `WxFeatureRef.make(layer_idx_b, row_b)`
4. Calls `ctx_a.set_field_ref(row_a, field_id, ref)`

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_direct_build.py  (append)
def test_feature_ref_assignment():
    class Vendor(Feature):
        name: STR

    class Device(Feature):
        vendor: Vendor   # REF field

    orm = GlobalORM()
    import fastdb4py.feature.feature_direct as fd
    old = fd._get_orm
    fd._get_orm = lambda: orm

    v = Vendor()
    v.name = "Intel"

    d = Device()
    d.vendor = v   # should auto-commit v and store ref

    db = orm.finalize([Vendor, Device])

    # Vendor layer = index 0, Device layer = index 1
    vendor_layer = db.get_layer(0)
    device_layer = db.get_layer(1)

    device_feat = device_layer.tryGetFeatureAt(0)
    ref = device_feat.get_field_as_ref(0)
    vendor_feat = db.tryGetFeature(ref)
    assert vendor_feat.get_field_as_string(0) == "Intel"

    fd._get_orm = old
```

- [ ] **Step 2: Run test — verify it fails**

```bash
uv run pytest tests/python/test_direct_build.py::test_feature_ref_assignment -v
```
Expected: `NotImplementedError`

- [ ] **Step 3: Extend `__setattr__` with REF support in `feature_direct.py`**

Replace the `else: raise NotImplementedError` block with:

```python
        elif fd.field_type == OriginFieldType.ref:
            if not isinstance(value, Feature):
                raise TypeError(
                    f"Field '{name}' expects a Feature instance, got {type(value)}"
                )
            # Commit the referenced feature so its row is fully written
            ref_ctx = object.__getattribute__(value, '_layer_ctx')
            ref_row = object.__getattribute__(value, '_row_idx')
            ref_ctx.commit(ref_row)
            # Build WxFeatureRef: (layer_index_of_ref_type, row_idx)
            from ..global_orm import get_global_orm
            from .. import core
            ref_type = object.__getattribute__(value, '_feature_type')
            layer_idx = get_global_orm().layer_index_of(ref_type)
            cpp_ref = core.WxFeatureRef.make(layer_idx, ref_row)
            ctx.set_field_ref(row, fd.field_id, cpp_ref)
        else:
            raise NotImplementedError(
                f"Field '{name}' type {fd.field_type} not yet supported in direct setattr"
            )
```

- [ ] **Step 4: Run test — verify it passes**

```bash
uv run pytest tests/python/test_direct_build.py::test_feature_ref_assignment -v
```
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/feature/feature_direct.py tests/python/test_direct_build.py
git commit -m "feat(feature_direct): support REF field assignment with auto-commit

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 7: LIST field — numeric array assignment

**Files:**
- Modify: `python/fastdb4py/feature/feature_direct.py`

### Design

When `device.temps = [35.5, 36.1, 37.0]` or a numpy array is assigned to a `List[F64]` field, convert to the correct numpy dtype and call `ctx.set_field_list_numeric(row, field_id, buf)`.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_direct_build.py  (append)
import numpy as np
from fastdb4py.type import F64
from typing import List

def test_feature_list_numeric_assignment():
    class Device(Feature):
        temps: List[F64]

    orm = GlobalORM()
    import fastdb4py.feature.feature_direct as fd
    old = fd._get_orm
    fd._get_orm = lambda: orm

    d = Device()
    d.temps = [35.5, 36.1, 37.0]

    db = orm.finalize([Device])
    layer = db.get_layer(0)
    feat = layer.tryGetFeatureAt(0)
    arr = feat.get_field_as_list_view(0).as_array(np.float64)
    assert list(arr) == [35.5, 36.1, 37.0]

    fd._get_orm = old
```

- [ ] **Step 2: Run test — verify it fails**

```bash
uv run pytest tests/python/test_direct_build.py::test_feature_list_numeric_assignment -v
```
Expected: `NotImplementedError`

- [ ] **Step 3: Extend `__setattr__` with LIST numeric support**

Replace `else: raise NotImplementedError` with:

```python
        elif fd.field_type == OriginFieldType.list:
            import numpy as np
            from ..type import LIST_ELEM_DTYPE
            elem_type = fd.list_elem_type
            dtype_str = LIST_ELEM_DTYPE.get(elem_type, 'float64')
            if not isinstance(value, np.ndarray):
                value = np.asarray(value, dtype=dtype_str)
            elif value.dtype.name != dtype_str:
                value = value.astype(dtype_str)
            buf = np.ascontiguousarray(value)
            ctx.set_field_list_numeric(row, fd.field_id, buf)
        else:
            raise NotImplementedError(
                f"Field '{name}' type {fd.field_type} not yet supported in direct setattr"
            )
```

- [ ] **Step 4: Run test — verify it passes**

```bash
uv run pytest tests/python/test_direct_build.py::test_feature_list_numeric_assignment -v
```
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/feature/feature_direct.py tests/python/test_direct_build.py
git commit -m "feat(feature_direct): support LIST[numeric] assignment

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 8: Feature.__getattr__ — direct C++ read after collect

**Files:**
- Modify: `python/fastdb4py/feature/feature_direct.py`

### Design

After `orm.finalize()`, read-back is done via `WxFeature` (existing `WxFeature.get_field_as_float` etc.). The `Feature` class gains a `_origin` slot for the `WxFeature` pointer and a class method `map_from(origin, db)` to create a read-only view. `__getattr__` dispatches to `_origin` when set, else raises.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_direct_build.py  (append)
def test_feature_read_after_finalize():
    class Point(Feature):
        x: F64
        y: F64

    orm = GlobalORM()
    import fastdb4py.feature.feature_direct as fd
    old = fd._get_orm
    fd._get_orm = lambda: orm

    p = Point()
    p.x = 1.5
    p.y = 2.5

    db = orm.finalize([Point])
    layer = db.get_layer(0)

    # map_from creates a read-only Feature view
    p_read = Point.map_from(layer.tryGetFeatureAt(0), db)
    assert abs(p_read.x - 1.5) < 1e-9
    assert abs(p_read.y - 2.5) < 1e-9

    fd._get_orm = old
```

- [ ] **Step 2: Run test — verify it fails**

```bash
uv run pytest tests/python/test_direct_build.py::test_feature_read_after_finalize -v
```
Expected: `AttributeError` or `NotImplementedError`

- [ ] **Step 3: Add `_origin` slot + `map_from` + `__getattr__` to `feature_direct.py`**

In the `Feature` class, change `__slots__` and add methods:

```python
    __slots__ = ('_row_idx', '_layer_ctx', '_feature_type', '_origin', '_db')

    # (existing __new__ and __setattr__ unchanged)

    @classmethod
    def map_from(cls, origin, db) -> 'Feature':
        """Create a read-only view from a WxFeature + WxDatabase."""
        obj = object.__new__(cls)
        object.__setattr__(obj, '_row_idx', -1)
        object.__setattr__(obj, '_layer_ctx', None)
        object.__setattr__(obj, '_feature_type', cls)
        object.__setattr__(obj, '_origin', origin)
        object.__setattr__(obj, '_db', db)
        return obj

    def __getattr__(self, name: str):
        if name.startswith('_'):
            raise AttributeError(name)

        origin = object.__getattribute__(self, '_origin')
        if origin is None:
            raise AttributeError(
                f"Feature is in write mode; read fields only after map_from()"
            )

        schema = get_schema(type(self))
        fd = schema.get(name)
        if fd is None:
            raise AttributeError(f"'{type(self).__name__}' has no field '{name}'")

        ft = fd.field_type
        if ft in _NUMERIC_TYPES:
            if ft in (OriginFieldType.f32, OriginFieldType.f64):
                return origin.get_field_as_float(fd.field_id)
            return origin.get_field_as_int(fd.field_id)
        if ft == OriginFieldType.str:
            return origin.get_field_as_string(fd.field_id)
        if ft == OriginFieldType.ref:
            db = object.__getattribute__(self, '_db')
            ref = origin.get_field_as_ref(fd.field_id)
            target_origin = db.tryGetFeature(ref)
            if target_origin is None:
                return None
            target_cls = fd.ref_target or Feature
            return target_cls.map_from(target_origin, db)
        if ft == OriginFieldType.list:
            import numpy as np
            from ..type import LIST_ELEM_DTYPE
            dtype_str = LIST_ELEM_DTYPE.get(fd.list_elem_type, 'float64')
            chunk = origin.get_field_as_list_view(fd.field_id)
            return chunk.as_array(np.dtype(dtype_str))
        return None
```

- [ ] **Step 4: Run test — verify it passes**

```bash
uv run pytest tests/python/test_direct_build.py::test_feature_read_after_finalize -v
```
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/feature/feature_direct.py tests/python/test_direct_build.py
git commit -m "feat(feature_direct): add map_from + __getattr__ for C++ read path

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 9: PartialBuild — meta sharing and finalize

**Files:**
- Modify: `python/fastdb4py/partial_build.py`

### Design

`PartialBuild` wraps a `GlobalORM`. `export_meta()` returns a plain dict `{class_name: [(field_name, field_type_int, cpp_type, ref_target_name, list_elem_type_int), ...]}` — fully picklable with no C++ objects. `import_meta(meta_dict)` pre-populates the `SchemaRegistry` from that dict so the subprocess knows all schemas before building. `collect(types)` delegates to `GlobalORM.finalize`.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_partial_build.py
import pickle
from fastdb4py.partial_build import PartialBuild
from fastdb4py.global_orm import GlobalORM
from fastdb4py.type import F64, STR

def test_export_meta_is_picklable():
    class Sensor:
        temp: F64
        label: STR

    orm = GlobalORM()
    pb = PartialBuild(orm)
    # Trigger schema registration by registering type
    pb.register(Sensor)
    meta = pb.export_meta()
    # Must round-trip through pickle (no C++ objects)
    meta2 = pickle.loads(pickle.dumps(meta))
    assert 'Sensor' in meta2
    fields = meta2['Sensor']
    assert fields[0][0] == 'temp'   # name
    assert fields[1][0] == 'label'

def test_import_meta_registers_schema():
    class Widget:
        x: F64

    pb_src = PartialBuild(GlobalORM())
    pb_src.register(Widget)
    meta = pb_src.export_meta()

    # Import into a fresh registry (simulate subprocess)
    from fastdb4py.registry import SchemaRegistry
    fresh_registry = SchemaRegistry()
    PartialBuild.import_meta_into(meta, fresh_registry)
    schema = fresh_registry._schemas.get('Widget')
    assert schema is not None
    assert schema.fields[0].name == 'x'
```

- [ ] **Step 2: Run tests — verify they fail**

```bash
uv run pytest tests/python/test_partial_build.py -v
```
Expected: `ImportError`

- [ ] **Step 3: Implement `partial_build.py`**

```python
# python/fastdb4py/partial_build.py
from __future__ import annotations
from typing import Dict, List, Any, Type, TYPE_CHECKING

if TYPE_CHECKING:
    from .global_orm import GlobalORM
    from .registry import SchemaRegistry


class PartialBuild:
    """
    Wraps a GlobalORM.  Provides:
      - register(cls)       : ensure schema is known
      - export_meta()       : picklable dict of all schemas
      - import_meta_into()  : class method; populate a SchemaRegistry from a dict
      - collect(types)      : finalize → WxDatabase
    """

    def __init__(self, orm: 'GlobalORM'):
        self._orm = orm

    def register(self, feature_type: Type) -> None:
        """Pre-register a type so its schema appears in export_meta."""
        from .registry import get_schema
        get_schema(feature_type)  # side-effect: populates global registry

    def export_meta(self) -> Dict[str, Any]:
        """Return a picklable snapshot of all currently registered schemas."""
        from .registry import _global_schema_registry
        result: Dict[str, Any] = {}
        for cls, schema in _global_schema_registry._schemas.items():
            fields_serial = []
            for fd in schema.fields:
                ref_name = fd.ref_target.__name__ if fd.ref_target else None
                list_elem = fd.list_elem_type.value if fd.list_elem_type else None
                fields_serial.append((
                    fd.name,
                    fd.field_type.value,
                    fd.cpp_type,
                    ref_name,
                    list_elem,
                ))
            result[schema.layer_name] = fields_serial
        return result

    @staticmethod
    def import_meta_into(meta: Dict[str, Any],
                         registry: 'SchemaRegistry') -> None:
        """Populate a SchemaRegistry from an export_meta() dict."""
        from .registry import LayerSchema, FieldDef, OriginFieldType

        for layer_name, fields_serial in meta.items():
            fields = []
            for idx, (name, ft_val, cpp_type, ref_name, list_elem_val) in \
                    enumerate(fields_serial):
                list_elem = OriginFieldType(list_elem_val) \
                    if list_elem_val is not None else None
                fd = FieldDef(
                    name=name,
                    field_type=OriginFieldType(ft_val),
                    field_id=idx,
                    cpp_type=cpp_type,
                    ref_target=None,   # class objects not available cross-process
                    list_elem_type=list_elem,
                )
                fields.append(fd)
            schema = LayerSchema(layer_name=layer_name, fields=fields)
            # Store under a string key for cross-process lookup
            registry._schemas[layer_name] = schema  # type: ignore[index]

    def collect(self, feature_types: List[Type]) -> Any:
        """Finalize all open rows and return a WxDatabase."""
        return self._orm.finalize(feature_types)
```

- [ ] **Step 4: Run tests — verify they pass**

```bash
uv run pytest tests/python/test_partial_build.py -v
```
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/partial_build.py tests/python/test_partial_build.py
git commit -m "feat(partial_build): meta export/import + collect

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 10: Integration test — full Room→Device→Vendor pipeline

**Files:**
- Modify: `tests/python/test_direct_build.py`

This test exercises the full end-to-end scenario from `more_direct.md`: nested types, REF fields, LIST fields, read-back after finalize.

- [ ] **Step 1: Write the integration test**

```python
# tests/python/test_direct_build.py  (append)
import numpy as np
from typing import List
from fastdb4py.feature.feature_direct import Feature
from fastdb4py.global_orm import GlobalORM
from fastdb4py.type import F64, I32, STR

def test_integration_room_device_vendor():
    class Vendor(Feature):
        name: STR

    class Device(Feature):
        vendor: Vendor
        temps: List[F64]

    class Room(Feature):
        room_id: I32
        devices: List[Device]   # LIST of REF -- not yet supported; use single ref for now

    orm = GlobalORM()
    import fastdb4py.feature.feature_direct as fd
    old = fd._get_orm
    fd._get_orm = lambda: orm

    # Build vendor
    intel = Vendor()
    intel.name = "Intel"
    amd = Vendor()
    amd.name = "AMD"

    # Build devices
    dev_a = Device()
    dev_a.vendor = intel
    dev_a.temps = [35.5, 36.1, 37.0]

    dev_b = Device()
    dev_b.vendor = amd
    dev_b.temps = [40.2, 41.5]

    db = orm.finalize([Vendor, Device])

    vendor_layer = db.get_layer(0)
    device_layer = db.get_layer(1)

    # Verify device_a vendor ref resolves to Intel
    d0 = device_layer.tryGetFeatureAt(0)
    ref = d0.get_field_as_ref(0)
    v = db.tryGetFeature(ref)
    assert v.get_field_as_string(0) == "Intel"

    # Verify device_a temps
    arr = d0.get_field_as_list_view(1).as_array(np.float64)
    assert list(arr) == [35.5, 36.1, 37.0]

    # Verify device_b temps
    d1 = device_layer.tryGetFeatureAt(1)
    arr2 = d1.get_field_as_list_view(1).as_array(np.float64)
    assert list(arr2) == [40.2, 41.5]

    fd._get_orm = old
```

- [ ] **Step 2: Run integration test**

```bash
uv run pytest tests/python/test_direct_build.py::test_integration_room_device_vendor -v
```
Expected: PASS

- [ ] **Step 3: Run all direct build tests**

```bash
uv run pytest tests/python/test_direct_build.py tests/python/test_partial_build.py -v
```
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
git add tests/python/test_direct_build.py
git commit -m "test(direct_build): add integration test Room->Device->Vendor

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Self-Review Checklist

### Spec coverage

| Requirement | Task |
|---|---|
| `Feature()` allocates C++ row immediately | Task 5 |
| `feature.x = 10` calls C++ `setField` directly | Task 5 |
| REF assignment auto-commits ref target | Task 6 |
| LIST numeric assignment | Task 7 |
| Read-back via `map_from` after finalize | Task 8 |
| `PartialBuild.export_meta()` picklable | Task 9 |
| Cross-process schema import | Task 9 |
| `GlobalORM.finalize()` returns `WxDatabase` | Task 4 |
| Integration test | Task 10 |

### Known limitations (acceptable for this iteration)

- `LIST[REF]` (list of feature references) is NOT implemented. The integration test uses scalar REF only. This is a follow-up task.
- `POLY_REF` is intentionally omitted (marked Future Work in `more_direct.md`).
- `Feature.__getattr__` for LIST[REF] is not implemented.
- `GlobalORM` is process-level; no thread-safety for `new_feature()` when called from multiple threads concurrently (each thread should use its own `GlobalORM` instance or a lock wrapper).
- `partial_build.import_meta_into` stores schemas under string key (`layer_name`), not under the class object — cross-process type lookup works by name, not identity.

### Placeholder scan

No TBD/TODO in task bodies. All code blocks are complete.

### Type consistency

- `LayerContext.set_field_list_numeric` signature uses `buf` (numpy array) consistently in Tasks 3, 7.
- `FieldDef.cpp_type` is used in Task 2 (SchemaRegistry build) and Task 4 (GlobalORM `_create_layer`).
- `Feature.__slots__` updated in Task 8 to add `_origin`, `_db` — consistent with `map_from` usage.
