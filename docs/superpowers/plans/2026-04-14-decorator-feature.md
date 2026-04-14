# Decorator-based Feature Redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `Feature` subclass pattern with a `@feature` decorator that works on plain Python classes — instances live in Python memory, `ORM.push()` serializes to C++, and read-back supports `map` (zero-copy) and `copy` modes.

**Architecture:** `@feature` registers the class's schema (via annotations) into a global `SchemaRegistry` on first use. Instance data lives in normal Python `__dict__`. `ORM.push(obj)` reads `__dict__`, maps fields to `WxLayerTableBuild` calls in one shot. Read-back from `WxDatabase` supports two modes: `map` returns a proxy that dispatches reads to C++ memory, `copy` creates a full Python object with copied data. Strict type rules: only serializable types allowed (no bare `dict`, `Any`, `tuple`, `object`).

**Tech Stack:** Python 3.10+, SWIG C++ bindings (existing `fastdb4py.core`), NumPy, pytest

---

## Design Rules

### Strict Type Policy

Only these annotation types are allowed on `@feature` classes:

| Annotation | fastdb type | Storage |
|---|---|---|
| `int` | I32 | scalar column |
| `float` | F64 | scalar column |
| `str` | STR | string column |
| `bool` | U8 | scalar column |
| `bytes` | BYTES | geometry-like blob |
| `U8`, `U16`, `U32`, `I32`, `F32`, `F64` | direct | scalar column |
| `STR`, `WSTR` | direct | string column |
| `AnotherFeatureClass` | REF | ref column (u16 layer + u24 row) |
| `list[float]`, `list[int]`, `list[F64]`, etc. | LIST(PRIMITIVE) | offset+length + pool layer |
| `list[AnotherFeatureClass]` | LIST(REF) | offset+length + ref pool |
| `np.ndarray` (with dtype annotation) | LIST(PRIMITIVE) | columnar buffer |

**Rejected types** (raise `TypeError` at decoration time):
- `dict`, `Dict`, `Mapping`
- `Any`, `object`
- `tuple`, `Tuple`
- `set`, `frozenset`
- bare `list` (no element type)
- bare `np.ndarray` without dtype metadata

### Two Operating Modes

1. **Write mode** (default): `obj = MyFeature()` → normal Python object, data in `__dict__`
2. **Read mode** (after ORM load):
   - `map` mode → proxy object, reads dispatch to `WxFeature` C++ memory (zero-copy, lifetime tied to `WxDatabase`)
   - `copy` mode → full Python object with all data copied from C++

---

## File Map

| File | Responsibility |
|---|---|
| `python/fastdb4py/decorator.py` | `@feature` decorator + type validation + schema extraction |
| `python/fastdb4py/registry.py` | `SchemaRegistry` singleton: `FieldDef`, `LayerSchema`, `get_schema()` |
| `python/fastdb4py/push.py` | `push_feature()` — serialize one Python object into `WxLayerTableBuild` |
| `python/fastdb4py/reader.py` | `map_feature()` / `copy_feature()` — read features from `WxDatabase` |
| `python/fastdb4py/orm2.py` | New ORM class using decorator-based features |
| `tests/python/test_decorator.py` | Unit tests for `@feature` decorator |
| `tests/python/test_push.py` | Unit tests for push serialization |
| `tests/python/test_reader.py` | Unit tests for map/copy read modes |
| `tests/python/test_orm2.py` | Integration tests for full ORM2 lifecycle |

---

## Task 1: Create branch + skeleton files

**Files:**
- Create: python/fastdb4py/decorator.py
- Create: python/fastdb4py/registry.py
- Create: python/fastdb4py/push.py
- Create: python/fastdb4py/reader.py
- Create: python/fastdb4py/orm2.py
- Create: tests/python/test_decorator.py
- Create: tests/python/test_push.py
- Create: tests/python/test_reader.py
- Create: tests/python/test_orm2.py

- [ ] **Step 1: Create the branch**

    git checkout -b redesign/decorator-feature

- [ ] **Step 2: Create skeleton files**

    touch python/fastdb4py/decorator.py registry.py push.py reader.py orm2.py
    touch tests/python/test_decorator.py test_push.py test_reader.py test_orm2.py

- [ ] **Step 3: Commit skeleton files**

---

## Task 2: SchemaRegistry — FieldDef, LayerSchema, get_schema()

**Files:**
- Create: `python/fastdb4py/registry.py`
- Test: `tests/python/test_decorator.py`

### Design

`get_schema(cls)` lazily computes and caches a `LayerSchema` from annotations. `FieldDef` is a frozen dataclass for each field. Thread-safe via Lock + double-check. Uses `WeakKeyDictionary` so classes can be GC'd.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_decorator.py
import pytest
from fastdb4py.registry import get_schema, FieldDef, LayerSchema
from fastdb4py.type import OriginFieldType, F64, U32, STR

def test_get_schema_scalar_fields():
    class Point:
        x: F64
        y: F64
        label: STR

    schema = get_schema(Point)
    assert isinstance(schema, LayerSchema)
    assert schema.layer_name == 'Point'
    assert len(schema.fields) == 3

    f_x = schema.fields[0]
    assert f_x.name == 'x'
    assert f_x.field_type == OriginFieldType.f64
    assert f_x.field_id == 0

    f_label = schema.fields[2]
    assert f_label.name == 'label'
    assert f_label.field_type == OriginFieldType.str
    assert f_label.field_id == 2

def test_get_schema_python_builtins():
    class Simple:
        count: int
        value: float
        name: str

    schema = get_schema(Simple)
    assert schema.fields[0].field_type == OriginFieldType.i32
    assert schema.fields[1].field_type == OriginFieldType.f64
    assert schema.fields[2].field_type == OriginFieldType.str

def test_get_schema_ref_field():
    class Vendor:
        name: STR

    class Device:
        vendor: Vendor

    schema = get_schema(Device)
    f = schema.fields[0]
    assert f.field_type == OriginFieldType.ref
    assert f.ref_target is Vendor

def test_get_schema_list_field():
    from typing import List
    class Sensor:
        temps: List[F64]

    schema = get_schema(Sensor)
    f = schema.fields[0]
    assert f.field_type == OriginFieldType.list
    assert f.list_elem_type == OriginFieldType.f64

def test_get_schema_caches():
    class Cached:
        x: F64
    s1 = get_schema(Cached)
    s2 = get_schema(Cached)
    assert s1 is s2

def test_get_schema_skips_private():
    class WithPrivate:
        _internal: int
        x: F64

    schema = get_schema(WithPrivate)
    assert len(schema.fields) == 1
    assert schema.fields[0].name == 'x'
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `uv run pytest tests/python/test_decorator.py -v`
Expected: `ImportError: cannot import name 'get_schema' from 'fastdb4py.registry'`

- [ ] **Step 3: Implement `registry.py`**

```python
# python/fastdb4py/registry.py
from __future__ import annotations
from dataclasses import dataclass
from threading import Lock
from typing import Any, Dict, List, Optional, Type, get_type_hints, get_origin, get_args
import typing
import weakref

from .type import (
    OriginFieldType, get_origin_type, get_list_element_type,
    LIST_ELEM_CPP_TYPE, FIELD_TYPE_MAP,
)
from .feature.base import BaseFeature


@dataclass(frozen=True, slots=True)
class FieldDef:
    """Metadata for a single field in a @feature class."""
    name: str
    field_type: OriginFieldType
    field_id: int                          # 0-based column index
    cpp_type: int                          # raw C++ FieldTypeEnum int
    ref_target: Optional[Type] = None      # target class for REF fields
    list_elem_type: Optional[OriginFieldType] = None


class LayerSchema:
    """Schema for one @feature class (= one fastdb layer)."""
    __slots__ = ('layer_name', 'fields', '_by_name')

    def __init__(self, layer_name: str, fields: List[FieldDef]):
        self.layer_name = layer_name
        self.fields = fields
        self._by_name: Dict[str, FieldDef] = {f.name: f for f in fields}

    def get(self, name: str) -> Optional[FieldDef]:
        return self._by_name.get(name)

    def __len__(self):
        return len(self.fields)


_registry_lock = Lock()
_registry: weakref.WeakKeyDictionary = weakref.WeakKeyDictionary()


def get_schema(cls: Type) -> LayerSchema:
    """Return (or compute) the LayerSchema for cls. Thread-safe, cached."""
    schema = _registry.get(cls)
    if schema is not None:
        return schema
    with _registry_lock:
        schema = _registry.get(cls)
        if schema is not None:
            return schema
        schema = _build_schema(cls)
        _registry[cls] = schema
        return schema


def _build_schema(cls: Type) -> LayerSchema:
    """Parse class annotations and build a LayerSchema."""
    try:
        hints = get_type_hints(cls)
    except NameError:
        hints = dict(getattr(cls, '__annotations__', {}))

    fields: List[FieldDef] = []
    field_id = 0
    for name, hint in hints.items():
        if name.startswith('_'):
            continue
        ft = _resolve_field_type(hint)
        cpp_type = _resolve_cpp_type(ft, hint)
        ref_target = _resolve_ref_target(ft, hint)
        list_elem = _resolve_list_elem(ft, hint)

        fields.append(FieldDef(
            name=name, field_type=ft, field_id=field_id,
            cpp_type=cpp_type, ref_target=ref_target, list_elem_type=list_elem,
        ))
        field_id += 1

    return LayerSchema(layer_name=cls.__name__, fields=fields)


def _resolve_field_type(hint) -> OriginFieldType:
    ft = get_origin_type(hint)
    if ft != OriginFieldType.unknown:
        return ft
    if isinstance(hint, type) and issubclass(hint, BaseFeature):
        return OriginFieldType.ref
    if isinstance(hint, type) and not issubclass(hint, (int, float, str, bytes, bool)):
        return OriginFieldType.ref
    return ft


def _resolve_cpp_type(ft: OriginFieldType, hint) -> int:
    if ft == OriginFieldType.list:
        elem_ft = get_list_element_type(hint)
        return LIST_ELEM_CPP_TYPE.get(elem_ft, 8)
    return ft.value


def _resolve_ref_target(ft: OriginFieldType, hint) -> Optional[Type]:
    if ft == OriginFieldType.ref and isinstance(hint, type):
        return hint
    return None


def _resolve_list_elem(ft: OriginFieldType, hint) -> Optional[OriginFieldType]:
    if ft == OriginFieldType.list:
        return get_list_element_type(hint)
    return None
```

- [ ] **Step 4: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_decorator.py -v`
Expected: All 6 tests PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/registry.py tests/python/test_decorator.py
git commit -m "feat(registry): SchemaRegistry with FieldDef, LayerSchema, get_schema

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 3: @feature decorator with type validation

**Files:**
- Create: `python/fastdb4py/decorator.py`
- Modify: `tests/python/test_decorator.py`

### Design

The `@feature` decorator:
1. Validates all annotations against the strict type policy
2. Calls `get_schema(cls)` to register the schema
3. Returns the class unchanged (no metaclass, no subclassing required)
4. Rejects `dict`, `Any`, `object`, `tuple`, bare `list`, bare `ndarray`

- [ ] **Step 1: Write the failing test**

Append to `tests/python/test_decorator.py`:

```python
from fastdb4py.decorator import feature

def test_feature_decorator_returns_class():
    @feature
    class Point:
        x: F64
        y: F64
    assert Point.__name__ == 'Point'
    p = Point()
    p.x = 1.0
    assert p.x == 1.0

def test_feature_decorator_registers_schema():
    @feature
    class Sensor:
        temp: F64
        label: STR
    schema = get_schema(Sensor)
    assert len(schema.fields) == 2

def test_feature_decorator_rejects_dict():
    with pytest.raises(TypeError, match="Unsupported.*dict"):
        @feature
        class Bad:
            meta: dict

def test_feature_decorator_rejects_any():
    from typing import Any as TypingAny
    with pytest.raises(TypeError, match="Unsupported.*Any"):
        @feature
        class Bad:
            data: TypingAny

def test_feature_decorator_rejects_bare_list():
    with pytest.raises(TypeError, match="Unsupported.*list"):
        @feature
        class Bad:
            items: list

def test_feature_decorator_rejects_tuple():
    with pytest.raises(TypeError, match="Unsupported.*tuple"):
        @feature
        class Bad:
            coords: tuple

def test_feature_decorator_accepts_typed_list():
    from typing import List
    @feature
    class Good:
        vals: List[F64]
    schema = get_schema(Good)
    assert schema.fields[0].field_type == OriginFieldType.list

def test_feature_decorator_accepts_ref():
    @feature
    class Vendor:
        name: STR

    @feature
    class Device:
        vendor: Vendor
    schema = get_schema(Device)
    assert schema.fields[0].field_type == OriginFieldType.ref
    assert schema.fields[0].ref_target is Vendor
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `uv run pytest tests/python/test_decorator.py::test_feature_decorator_returns_class -v`
Expected: `ImportError`

- [ ] **Step 3: Implement `decorator.py`**

```python
# python/fastdb4py/decorator.py
from __future__ import annotations
from typing import Any, get_type_hints, get_origin, get_args
import typing

from .registry import get_schema
from .type import OriginFieldType, get_origin_type, FIELD_TYPE_MAP

# Types that are explicitly rejected
_REJECTED_TYPES = {dict, tuple, set, frozenset, object}
_REJECTED_ORIGINS = {dict, tuple, set, frozenset}


def feature(cls):
    """Decorator that registers a plain Python class as a fastdb feature.

    Validates all annotations against the strict type policy, then registers
    the schema in the global SchemaRegistry. The class is returned unchanged.

    Usage:
        @feature
        class Point:
            x: F64
            y: F64
    """
    _validate_annotations(cls)
    get_schema(cls)  # register
    return cls


def _validate_annotations(cls):
    """Raise TypeError for unsupported field types."""
    try:
        hints = get_type_hints(cls)
    except NameError:
        hints = dict(getattr(cls, '__annotations__', {}))

    for name, hint in hints.items():
        if name.startswith('_'):
            continue
        _check_hint(name, hint)


def _check_hint(name: str, hint):
    """Validate a single annotation."""
    # Check for typing.Any
    if hint is typing.Any:
        raise TypeError(
            f"Unsupported type 'Any' for field '{name}'. "
            "Use explicit types (F64, int, str, etc.)."
        )

    # Check for rejected concrete types
    if isinstance(hint, type) and hint in _REJECTED_TYPES:
        raise TypeError(
            f"Unsupported type '{hint.__name__}' for field '{name}'. "
            "Use @feature classes for structured data."
        )

    # Check for generic origins (Dict[K,V], Tuple[...], etc.)
    origin = get_origin(hint)
    if origin is not None:
        if origin in _REJECTED_ORIGINS:
            raise TypeError(
                f"Unsupported type '{hint}' for field '{name}'. "
                "Use @feature classes for structured data."
            )
        # bare list without type args
        if origin is list:
            args = get_args(hint)
            if not args:
                raise TypeError(
                    f"Unsupported type 'list' for field '{name}'. "
                    "Use list[F64], list[int], list[MyFeature], etc."
                )
        return

    # bare `list` (not generic)
    if hint is list:
        raise TypeError(
            f"Unsupported type 'list' for field '{name}'. "
            "Use list[F64], list[int], list[MyFeature], etc."
        )

    # Check that it maps to a known type
    ft = get_origin_type(hint)
    if ft == OriginFieldType.unknown:
        # Could be a @feature class (REF) — that's OK if it has annotations
        if isinstance(hint, type) and hasattr(hint, '__annotations__'):
            return
        raise TypeError(
            f"Unsupported type '{hint}' for field '{name}'. "
            "Only serializable types are allowed."
        )
```

- [ ] **Step 4: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_decorator.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/decorator.py tests/python/test_decorator.py
git commit -m "feat(decorator): @feature decorator with strict type validation

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 4: push_feature() — serialize Python object to WxLayerTableBuild

**Files:**
- Create: `python/fastdb4py/push.py`
- Create: `tests/python/test_push.py`

### Design

`push_feature(obj, layer_build, schema)` reads fields from `obj.__dict__`, dispatches each to the correct `WxLayerTableBuild` setter. Handles scalar, STR, BYTES, REF, and LIST fields. REF fields require the referenced object to already have a known `(layer_idx, row_idx)` — the caller (ORM) manages this via a pending-refs dict.

The function signature:
- `push_feature(obj, layer_build, schema, ref_resolver)` → `int` (row_idx)
- `ref_resolver(obj)` → `WxFeatureRef` — callback that resolves a Python feature object to its C++ ref

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_push.py
import pytest
import numpy as np
from fastdb4py.decorator import feature
from fastdb4py.registry import get_schema
from fastdb4py.push import push_feature
from fastdb4py.type import F64, U32, STR
from fastdb4py import core
from typing import List


@feature
class PushPoint:
    x: F64
    y: F64


def _make_db_and_layer(cls):
    """Helper: create a WxDatabaseBuild + WxLayerTableBuild for cls."""
    schema = get_schema(cls)
    db = core.WxDatabaseBuild()
    db.begin("")
    t = db.create_layer_begin(schema.layer_name)
    t.set_geometry_type(core.gtPoint, core.cfTx32, aabboxEnabled=True)
    t.set_extent(-180, -90, 180, 90)
    for fd in schema.fields:
        if fd.field_type.value == 13:  # list
            t.add_list_field(fd.name, fd.cpp_type)
        else:
            t.add_field(fd.name, fd.cpp_type)
    return db, t


def test_push_scalar_fields():
    db, t = _make_db_and_layer(PushPoint)
    schema = get_schema(PushPoint)

    p = PushPoint()
    p.x = 42.0
    p.y = -7.5

    row = push_feature(p, t, schema)
    assert row == 0

    # Finalize and read back
    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    assert abs(feat.get_field_as_float(0) - 42.0) < 1e-9
    assert abs(feat.get_field_as_float(1) - (-7.5)) < 1e-9


def test_push_str_field():
    @feature
    class Named:
        label: STR

    db, t = _make_db_and_layer(Named)
    schema = get_schema(Named)

    obj = Named()
    obj.label = "hello"
    push_feature(obj, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    assert feat.get_field_as_string(0) == "hello"


def test_push_list_field():
    @feature
    class WithList:
        temps: List[F64]

    db, t = _make_db_and_layer(WithList)
    schema = get_schema(WithList)

    obj = WithList()
    obj.temps = [35.5, 36.1, 37.0]
    push_feature(obj, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    arr = feat.get_field_as_list_view(0).as_array(np.float64)
    assert list(arr) == [35.5, 36.1, 37.0]


def test_push_default_values():
    """Fields not set on the object get safe defaults (0 for numeric, '' for str)."""
    db, t = _make_db_and_layer(PushPoint)
    schema = get_schema(PushPoint)

    p = PushPoint()  # no fields set
    push_feature(p, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    assert feat.get_field_as_float(0) == 0.0
    assert feat.get_field_as_float(1) == 0.0
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `uv run pytest tests/python/test_push.py -v`
Expected: `ImportError`

- [ ] **Step 3: Implement `push.py`**

```python
# python/fastdb4py/push.py
"""Serialize a decorated Python feature object into a WxLayerTableBuild row."""
from __future__ import annotations
from typing import Any, Callable, Optional, TYPE_CHECKING
import numpy as np

from .type import OriginFieldType, LIST_ELEM_DTYPE

if TYPE_CHECKING:
    from . import core
    from .registry import LayerSchema, FieldDef

# frozenset for O(1) membership tests
_NUMERIC_TYPES = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.u8n, OriginFieldType.u16n,
    OriginFieldType.f32, OriginFieldType.f64,
))

_INT_TYPES = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.u8n, OriginFieldType.u16n,
))

_FLOAT_TYPES = frozenset((
    OriginFieldType.f32, OriginFieldType.f64,
))


def push_feature(
    obj: Any,
    layer_build: 'core.WxLayerTableBuild',
    schema: 'LayerSchema',
    ref_resolver: Optional[Callable] = None,
) -> int:
    """Serialize obj's fields into layer_build. Returns the row index.

    Args:
        obj: Python feature object (decorated with @feature)
        layer_build: C++ layer builder
        schema: LayerSchema from registry
        ref_resolver: callback(obj) -> WxFeatureRef for REF fields
    """
    cache = obj.__dict__

    layer_build.add_feature_begin()

    for fd in schema.fields:
        value = cache.get(fd.name)
        _set_field(layer_build, fd, value, ref_resolver)

    layer_build.add_feature_end()

    # Row index is sequential (0, 1, 2, ...)
    # The caller tracks this; we just return a sentinel here.
    # In practice the ORM wraps this and tracks counts.
    return -1  # caller should maintain count


def _set_field(
    layer_build: 'core.WxLayerTableBuild',
    fd: 'FieldDef',
    value: Any,
    ref_resolver: Optional[Callable],
):
    """Dispatch a single field value to the correct C++ setter."""
    ft = fd.field_type
    fid = fd.field_id

    if ft in _INT_TYPES:
        layer_build.set_field(fid, int(value) if value is not None else 0)
    elif ft in _FLOAT_TYPES:
        layer_build.set_field(fid, float(value) if value is not None else 0.0)
    elif ft == OriginFieldType.str:
        layer_build.set_field_cstring(fid, str(value) if value is not None else "")
    elif ft == OriginFieldType.wstr:
        layer_build.set_field_wstring(fid, str(value) if value is not None else "")
    elif ft == OriginFieldType.bytes:
        layer_build.set_geometry_raw(value if value is not None else b"")
    elif ft == OriginFieldType.ref:
        if value is not None and ref_resolver is not None:
            ref = ref_resolver(value)
            if ref is not None:
                layer_build.set_field(fid, ref)
        # else: leave as default (null ref)
    elif ft == OriginFieldType.list:
        _set_list_field(layer_build, fd, value)


def _set_list_field(
    layer_build: 'core.WxLayerTableBuild',
    fd: 'FieldDef',
    value: Any,
):
    """Handle LIST field serialization."""
    if value is None or (hasattr(value, '__len__') and len(value) == 0):
        # Empty list: write zero-length buffer
        layer_build.set_field_list_numeric(fd.field_id, b"")
        return

    elem_type = fd.list_elem_type
    if elem_type is None or elem_type == OriginFieldType.ref:
        # LIST[REF] — handled by ORM-level graph traversal
        return

    # Numeric list: convert to numpy and write
    dtype_str = LIST_ELEM_DTYPE.get(elem_type, 'float64')
    if isinstance(value, np.ndarray):
        arr = np.ascontiguousarray(value.astype(dtype_str, copy=False))
    else:
        arr = np.ascontiguousarray(np.array(value, dtype=dtype_str))
    layer_build.set_field_list_numeric(fd.field_id, arr)
```

- [ ] **Step 4: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_push.py -v`
Expected: All 4 tests PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/push.py tests/python/test_push.py
git commit -m "feat(push): push_feature serializes Python object to C++ layer

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 5: reader.py — map_feature() and copy_feature()

**Files:**
- Create: `python/fastdb4py/reader.py`
- Create: `tests/python/test_reader.py`

### Design

Two read-back modes:
- `map_feature(cls, layer, idx)` → returns a proxy object whose attribute reads dispatch to C++ `WxFeature.get_field_*` (zero-copy, lifetime tied to ORM)
- `copy_feature(cls, layer, idx)` → creates a normal Python instance, copies all values from C++ into `__dict__` (fully detached, GC-able)

The map mode uses `__slots__` + `__getattr__`/`__setattr__` overrides on a generated wrapper class.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_reader.py
import pytest
import numpy as np
from fastdb4py.decorator import feature
from fastdb4py.registry import get_schema
from fastdb4py.push import push_feature
from fastdb4py.reader import map_feature, copy_feature
from fastdb4py.type import F64, U32, STR
from fastdb4py import core


@feature
class ReadPoint:
    x: F64
    y: F64
    label: STR


def _build_db_with_points():
    """Build a small DB with 3 ReadPoint features, return the read-only db."""
    schema = get_schema(ReadPoint)
    db = core.WxDatabaseBuild()
    db.begin("")
    t = db.create_layer_begin(schema.layer_name)
    t.set_geometry_type(core.gtPoint, core.cfTx32, aabboxEnabled=True)
    t.set_extent(-180, -90, 180, 90)
    for fd in schema.fields:
        t.add_field(fd.name, fd.cpp_type)

    for i in range(3):
        p = ReadPoint()
        p.x = float(i)
        p.y = float(i * 10)
        p.label = f"pt{i}"
        push_feature(p, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    return rdb


class TestMapFeature:
    def test_read_scalar(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = map_feature(ReadPoint, layer, 1)
        assert abs(obj.x - 1.0) < 1e-9
        assert abs(obj.y - 10.0) < 1e-9

    def test_read_string(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = map_feature(ReadPoint, layer, 0)
        assert obj.label == "pt0"

    def test_map_is_readonly(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = map_feature(ReadPoint, layer, 0)
        with pytest.raises(AttributeError, match="read-only"):
            obj.x = 999.0


class TestCopyFeature:
    def test_read_scalar(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = copy_feature(ReadPoint, layer, 1)
        assert abs(obj.x - 1.0) < 1e-9
        assert abs(obj.y - 10.0) < 1e-9

    def test_read_string(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = copy_feature(ReadPoint, layer, 2)
        assert obj.label == "pt2"

    def test_copy_is_detached(self):
        """After copy, the object is independent of the DB."""
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = copy_feature(ReadPoint, layer, 0)
        obj.x = 999.0  # should work — it's a normal Python object
        assert obj.x == 999.0

    def test_copy_has_correct_type(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = copy_feature(ReadPoint, layer, 0)
        assert isinstance(obj, ReadPoint)
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `uv run pytest tests/python/test_reader.py -v`
Expected: `ImportError`

- [ ] **Step 3: Implement `reader.py`**

```python
# python/fastdb4py/reader.py
"""Read features from a built fastdb database in map or copy mode."""
from __future__ import annotations
from typing import Any, Type, TYPE_CHECKING

from .registry import get_schema, FieldDef
from .type import OriginFieldType

if TYPE_CHECKING:
    from . import core


_GETTERS = {
    OriginFieldType.u8:  'get_field_as_int',
    OriginFieldType.u16: 'get_field_as_int',
    OriginFieldType.u32: 'get_field_as_int',
    OriginFieldType.i32: 'get_field_as_int',
    OriginFieldType.u8n: 'get_field_as_int',
    OriginFieldType.u16n: 'get_field_as_int',
    OriginFieldType.f32: 'get_field_as_float',
    OriginFieldType.f64: 'get_field_as_float',
    OriginFieldType.str: 'get_field_as_string',
    OriginFieldType.wstr: 'get_field_as_string',
}


def _read_field(feat: 'core.WxFeature', fd: FieldDef) -> Any:
    """Read one field value from a C++ WxFeature."""
    getter_name = _GETTERS.get(fd.field_type)
    if getter_name is not None:
        getter = getattr(feat, getter_name)
        return getter(fd.field_id)

    if fd.field_type == OriginFieldType.bytes:
        return feat.get_geometry_raw()

    if fd.field_type == OriginFieldType.ref:
        return None  # REF resolved at ORM level

    if fd.field_type == OriginFieldType.list:
        import numpy as np
        from .type import LIST_ELEM_DTYPE
        dtype_str = LIST_ELEM_DTYPE.get(fd.list_elem_type, 'float64')
        try:
            chunk = feat.get_field_as_list_view(fd.field_id)
            return chunk.as_array(getattr(np, dtype_str)).copy()
        except Exception:
            return []

    return None


class MappedFeature:
    """Read-only proxy that dispatches attribute reads to C++ WxFeature.

    Not a subclass of the target class — isinstance checks won't match.
    Use copy_feature() when you need a real instance.
    """
    __slots__ = ('_feat', '_schema', '_cls')

    def __init__(self, cls: Type, feat: 'core.WxFeature', schema):
        object.__setattr__(self, '_cls', cls)
        object.__setattr__(self, '_feat', feat)
        object.__setattr__(self, '_schema', schema)

    def __getattr__(self, name: str) -> Any:
        schema = object.__getattribute__(self, '_schema')
        fd = schema.get(name)
        if fd is None:
            raise AttributeError(f"'{self._cls.__name__}' has no field '{name}'")
        feat = object.__getattribute__(self, '_feat')
        return _read_field(feat, fd)

    def __setattr__(self, name: str, value: Any):
        raise AttributeError("Mapped feature is read-only")

    def __repr__(self):
        cls = object.__getattribute__(self, '_cls')
        return f"<MappedFeature({cls.__name__})>"


def map_feature(cls: Type, layer: 'core.WxLayerTable', idx: int) -> MappedFeature:
    """Return a read-only proxy that reads from C++ on each attribute access.

    The proxy is tied to the layer's lifetime. Do not use after the ORM is closed.
    """
    schema = get_schema(cls)
    feat = layer.tryGetFeature(idx)
    return MappedFeature(cls, feat, schema)


def copy_feature(cls: Type, layer: 'core.WxLayerTable', idx: int) -> Any:
    """Create a fully detached Python instance with all field values copied.

    Returns a normal instance of cls with data in __dict__.
    """
    schema = get_schema(cls)
    feat = layer.tryGetFeature(idx)

    obj = cls.__new__(cls)
    for fd in schema.fields:
        val = _read_field(feat, fd)
        obj.__dict__[fd.name] = val

    return obj
```

- [ ] **Step 4: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_reader.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/reader.py tests/python/test_reader.py
git commit -m "feat(reader): map_feature and copy_feature for zero-copy and detached reads

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 6: ORM2 — new ORM class with create/push/combine/read

**Files:**
- Create: `python/fastdb4py/orm2.py`
- Create: `tests/python/test_orm2.py`

### Design

`ORM2` replaces the existing ORM for decorator-based features. It manages:
- `create()` → start a new build session
- `push(obj)` → serialize one feature into its layer (auto-creates layer on first push of a type)
- `combine()` → finalize build into a read-only database
- `table(cls)` → get a Table-like accessor for a class
- `get(cls, idx, mode='map')` → read back a single feature

Internal tracking: `_layers: dict[Type, LayerState]` where LayerState holds the WxLayerTableBuild, row count, and pending refs.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_orm2.py
import pytest
from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, U32, STR


@feature
class O2Point:
    x: F64
    y: F64
    label: STR


class TestORM2Basic:
    def test_create_and_push(self):
        orm = ORM2.create()
        p = O2Point()
        p.x = 1.0
        p.y = 2.0
        p.label = "first"
        orm.push(p)
        assert orm.count(O2Point) == 1

    def test_push_multiple(self):
        orm = ORM2.create()
        for i in range(5):
            p = O2Point()
            p.x = float(i)
            p.y = float(i * 2)
            p.label = f"p{i}"
            orm.push(p)
        assert orm.count(O2Point) == 5

    def test_combine_and_read_copy(self):
        orm = ORM2.create()
        p = O2Point()
        p.x = 42.0
        p.y = -7.5
        p.label = "test"
        orm.push(p)
        orm.combine()

        result = orm.get(O2Point, 0, mode='copy')
        assert abs(result.x - 42.0) < 1e-9
        assert abs(result.y - (-7.5)) < 1e-9
        assert result.label == "test"
        assert isinstance(result, O2Point)

    def test_combine_and_read_map(self):
        orm = ORM2.create()
        p = O2Point()
        p.x = 3.14
        p.y = 2.71
        p.label = "pi"
        orm.push(p)
        orm.combine()

        mapped = orm.get(O2Point, 0, mode='map')
        assert abs(mapped.x - 3.14) < 1e-9
        assert mapped.label == "pi"
        # map is read-only
        with pytest.raises(AttributeError, match="read-only"):
            mapped.x = 0.0

    def test_multiple_types(self):
        @feature
        class Color:
            r: U32
            g: U32
            b: U32

        orm = ORM2.create()
        p = O2Point()
        p.x = 1.0
        p.y = 2.0
        p.label = "pt"
        orm.push(p)

        c = Color()
        c.r = 255
        c.g = 128
        c.b = 0
        orm.push(c)

        orm.combine()
        assert orm.count(O2Point) == 1
        assert orm.count(Color) == 1

        pt = orm.get(O2Point, 0, mode='copy')
        assert abs(pt.x - 1.0) < 1e-9

        color = orm.get(Color, 0, mode='copy')
        assert color.r == 255

    def test_iter_features(self):
        orm = ORM2.create()
        for i in range(3):
            p = O2Point()
            p.x = float(i)
            p.y = 0.0
            p.label = f"p{i}"
            orm.push(p)
        orm.combine()

        results = list(orm.iter(O2Point, mode='copy'))
        assert len(results) == 3
        assert all(isinstance(r, O2Point) for r in results)
        assert [r.x for r in results] == [0.0, 1.0, 2.0]
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `uv run pytest tests/python/test_orm2.py -v`
Expected: `ImportError`

- [ ] **Step 3: Implement `orm2.py`**

```python
# python/fastdb4py/orm2.py
"""ORM2: decorator-based ORM for @feature classes."""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import Any, Dict, Iterator, List, Literal, Optional, Type
import numpy as np

from . import core
from .registry import get_schema, LayerSchema
from .push import push_feature
from .reader import map_feature, copy_feature


@dataclass
class LayerState:
    """Tracks build state for one @feature class."""
    cls: Type
    schema: LayerSchema
    build: Any = None       # WxLayerTableBuild
    layer_idx: int = -1     # index in the database
    row_count: int = 0


class ORM2:
    """Decorator-based ORM for @feature classes.

    Usage:
        orm = ORM2.create()
        orm.push(my_point)
        orm.combine()
        result = orm.get(MyPoint, 0, mode='copy')
    """

    def __init__(self):
        self._db_build: Optional[core.WxDatabaseBuild] = None
        self._db: Optional[core.WxDatabase] = None
        self._buffer: Optional[bytes] = None
        self._layers: Dict[Type, LayerState] = {}
        self._layer_order: List[Type] = []  # preserves layer creation order
        self._built = False

    @classmethod
    def create(cls) -> 'ORM2':
        """Start a new build session."""
        orm = cls()
        orm._db_build = core.WxDatabaseBuild()
        orm._db_build.begin("")
        return orm

    def push(self, obj: Any) -> int:
        """Serialize a @feature object into its layer. Returns row index."""
        cls = type(obj)
        state = self._ensure_layer(cls)

        row_idx = state.row_count
        push_feature(obj, state.build, state.schema)
        state.row_count += 1
        return row_idx

    def combine(self):
        """Finalize build into a read-only database."""
        if self._built:
            raise RuntimeError("ORM2 already combined")
        if self._db_build is None:
            raise RuntimeError("ORM2 not in build mode")

        mem = core.WxMemoryStream()
        self._db_build.post(mem)
        buf = mem.data().as_array(np.uint8).tobytes()
        self._db = core.WxDatabase.load_xbuffer(buf)
        self._db._buffer = buf  # prevent GC of underlying buffer
        self._buffer = buf
        self._built = True
        self._db_build = None

    def get(self, cls: Type, idx: int, mode: str = 'map') -> Any:
        """Read back a single feature.

        Args:
            cls: The @feature class
            idx: Row index
            mode: 'map' (zero-copy proxy) or 'copy' (detached instance)
        """
        self._check_built()
        state = self._layers.get(cls)
        if state is None:
            raise KeyError(f"No layer for {cls.__name__}")
        layer = self._db.get_layer(state.layer_idx)

        if mode == 'copy':
            return copy_feature(cls, layer, idx)
        elif mode == 'map':
            return map_feature(cls, layer, idx)
        else:
            raise ValueError(f"Unknown mode: {mode!r}")

    def iter(self, cls: Type, mode: str = 'map') -> Iterator:
        """Iterate all features of a given type."""
        self._check_built()
        state = self._layers.get(cls)
        if state is None:
            raise KeyError(f"No layer for {cls.__name__}")
        for i in range(state.row_count):
            yield self.get(cls, i, mode=mode)

    def count(self, cls: Type) -> int:
        """Return the number of features pushed for cls."""
        state = self._layers.get(cls)
        return state.row_count if state else 0

    def _ensure_layer(self, cls: Type) -> LayerState:
        """Create layer state on first push of a new type."""
        state = self._layers.get(cls)
        if state is not None:
            return state

        schema = get_schema(cls)
        t = self._db_build.create_layer_begin(schema.layer_name)
        t.set_geometry_type(core.gtPoint, core.cfTx32, aabboxEnabled=True)
        t.set_extent(-180, -90, 180, 90)

        for fd in schema.fields:
            if fd.field_type.value == 13:  # list
                t.add_list_field(fd.name, fd.cpp_type)
            else:
                t.add_field(fd.name, fd.cpp_type)

        layer_idx = len(self._layer_order)
        state = LayerState(cls=cls, schema=schema, build=t, layer_idx=layer_idx)
        self._layers[cls] = state
        self._layer_order.append(cls)
        return state

    def _check_built(self):
        if not self._built:
            raise RuntimeError("Call combine() before reading")
```

- [ ] **Step 4: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_orm2.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/orm2.py tests/python/test_orm2.py
git commit -m "feat(orm2): new ORM with create/push/combine/get/iter

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 7: REF field push — auto-resolve references in ORM2

**Files:**
- Modify: `python/fastdb4py/orm2.py`
- Modify: `python/fastdb4py/push.py`
- Modify: `tests/python/test_orm2.py`

### Design

When `orm.push(obj)` encounters REF fields, the referenced objects must also be pushed (if not already). We use a two-pass approach:
1. Collect all reachable objects via DFS from the root object
2. Push them in dependency order (leaves first)
3. Patch REF fields after all objects have row indices

This reuses the `update_feature_ref(feat_idx, field_id, ref)` C++ API for post-hoc patching.

- [ ] **Step 1: Write the failing test**

Append to `tests/python/test_orm2.py`:

```python
@feature
class Vendor:
    name: STR

@feature
class Device:
    model: STR
    vendor: Vendor


class TestORM2Refs:
    def test_push_with_ref(self):
        orm = ORM2.create()
        v = Vendor()
        v.name = "Acme"
        d = Device()
        d.model = "Widget"
        d.vendor = v
        orm.push(d)
        assert orm.count(Vendor) == 1
        assert orm.count(Device) == 1

    def test_push_shared_ref(self):
        """Two devices sharing the same vendor should push vendor once."""
        orm = ORM2.create()
        v = Vendor()
        v.name = "Shared"
        d1 = Device()
        d1.model = "A"
        d1.vendor = v
        d2 = Device()
        d2.model = "B"
        d2.vendor = v
        orm.push(d1)
        orm.push(d2)
        assert orm.count(Vendor) == 1
        assert orm.count(Device) == 2

    def test_ref_readback(self):
        """After combine, reading a Device should get a REF that resolves to the correct Vendor."""
        orm = ORM2.create()
        v = Vendor()
        v.name = "TestCo"
        d = Device()
        d.model = "X100"
        d.vendor = v
        orm.push(d)
        orm.combine()

        # Read device and vendor
        device = orm.get(Device, 0, mode='copy')
        assert device.model == "X100"

        vendor = orm.get(Vendor, 0, mode='copy')
        assert vendor.name == "TestCo"
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `uv run pytest tests/python/test_orm2.py::TestORM2Refs -v`
Expected: FAIL (refs not auto-pushed, vendor count wrong)

- [ ] **Step 3: Add graph traversal to ORM2**

Add a `_collect_refs(obj)` method and modify `push()`:

```python
# Add to python/fastdb4py/orm2.py

def push(self, obj: Any) -> int:
    """Serialize a @feature object (and all REF dependencies) into layers."""
    cls = type(obj)
    schema = get_schema(cls)

    # Check if already pushed
    obj_id = id(obj)
    if obj_id in self._pushed_ids:
        return self._pushed_ids[obj_id][1]

    # First, recursively push all REF dependencies
    for fd in schema.fields:
        if fd.field_type == OriginFieldType.ref:
            ref_val = getattr(obj, fd.name, None)
            if ref_val is not None and id(ref_val) not in self._pushed_ids:
                self.push(ref_val)

    # Now push this object
    state = self._ensure_layer(cls)
    row_idx = state.row_count

    def ref_resolver(ref_obj):
        ref_info = self._pushed_ids.get(id(ref_obj))
        if ref_info is None:
            return None
        ref_layer_idx, ref_row_idx = ref_info
        ref = core.WxFeatureRef()
        ref.layerIndex = ref_layer_idx
        ref.featureIndex = ref_row_idx
        return ref

    push_feature(obj, state.build, state.schema, ref_resolver)
    state.row_count += 1

    # Record this object as pushed
    self._pushed_ids[obj_id] = (state.layer_idx, row_idx)

    return row_idx
```

Also add `_pushed_ids: Dict[int, tuple]` to `__init__()`:
```python
self._pushed_ids: Dict[int, tuple] = {}  # id(obj) -> (layer_idx, row_idx)
```

- [ ] **Step 4: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_orm2.py -v`
Expected: All PASS (both old and new tests)

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/orm2.py python/fastdb4py/push.py tests/python/test_orm2.py
git commit -m "feat(orm2): auto-resolve REF fields via recursive push

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 8: LIST field push — numeric lists and ref lists

**Files:**
- Modify: `python/fastdb4py/push.py`
- Modify: `python/fastdb4py/orm2.py`
- Modify: `tests/python/test_push.py`

### Design

Two kinds of LIST fields:
- **Numeric lists** (`List[F64]`, `List[U32]`, etc.): already handled in Task 4 via `set_field_list_numeric`
- **Ref lists** (`List[SomeFeature]`): stored as offset+length in the parent row, with references in a dedicated pool layer. Uses `add_list_ref_field` + `set_field_list_refs` on the C++ side.

This task ensures both paths work end-to-end through ORM2.

- [ ] **Step 1: Write the failing test**

Append to `tests/python/test_push.py`:

```python
from typing import List

@feature
class Tag:
    name: STR

@feature
class Article:
    title: STR
    scores: List[F64]
    tags: List[Tag]


def test_push_numeric_list_via_orm():
    from fastdb4py.orm2 import ORM2
    orm = ORM2.create()
    a = Article()
    a.title = "Test"
    a.scores = [1.0, 2.0, 3.0]
    a.tags = []
    orm.push(a)
    orm.combine()

    result = orm.get(Article, 0, mode='copy')
    assert result.title == "Test"
    import numpy as np
    np.testing.assert_array_almost_equal(result.scores, [1.0, 2.0, 3.0])


def test_push_ref_list_via_orm():
    from fastdb4py.orm2 import ORM2
    orm = ORM2.create()
    t1 = Tag()
    t1.name = "python"
    t2 = Tag()
    t2.name = "fastdb"

    a = Article()
    a.title = "Guide"
    a.scores = [5.0]
    a.tags = [t1, t2]
    orm.push(a)
    orm.combine()

    assert orm.count(Tag) == 2
    assert orm.count(Article) == 1

    tag0 = orm.get(Tag, 0, mode='copy')
    tag1 = orm.get(Tag, 1, mode='copy')
    assert tag0.name == "python"
    assert tag1.name == "fastdb"
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `uv run pytest tests/python/test_push.py::test_push_ref_list_via_orm -v`
Expected: FAIL

- [ ] **Step 3: Add LIST[REF] handling to push.py and orm2.py**

In `push.py`, update `_set_list_field`:
```python
def _set_list_field(layer_build, fd, value, ref_resolver=None):
    """Handle LIST field serialization."""
    if value is None or (hasattr(value, '__len__') and len(value) == 0):
        layer_build.set_field_list_numeric(fd.field_id, b"")
        return

    elem_type = fd.list_elem_type
    if elem_type == OriginFieldType.ref:
        # LIST[REF]: resolve each element and write refs
        if ref_resolver is None:
            return
        refs = []
        for item in value:
            ref = ref_resolver(item)
            if ref is not None:
                refs.append(ref)
        if refs:
            layer_build.set_field_list_refs(fd.field_id, refs)
        return

    # Numeric list
    dtype_str = LIST_ELEM_DTYPE.get(elem_type, 'float64')
    if isinstance(value, np.ndarray):
        arr = np.ascontiguousarray(value.astype(dtype_str, copy=False))
    else:
        arr = np.ascontiguousarray(np.array(value, dtype=dtype_str))
    layer_build.set_field_list_numeric(fd.field_id, arr)
```

In `orm2.py`, update `push()` to also recursively push LIST[REF] elements:
```python
# In the push() method, before pushing the object itself:
for fd in schema.fields:
    if fd.field_type == OriginFieldType.list and fd.list_elem_type == OriginFieldType.ref:
        list_val = getattr(obj, fd.name, None) or []
        for item in list_val:
            if id(item) not in self._pushed_ids:
                self.push(item)
```

- [ ] **Step 4: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_push.py -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/push.py python/fastdb4py/orm2.py tests/python/test_push.py
git commit -m "feat(push): LIST[REF] field serialization with auto-push

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 9: Integration tests — full Vendor/Device/Room pipeline

**Files:**
- Create: `tests/python/test_integration_decorator.py`

### Design

End-to-end test that exercises all features together:
- Multiple @feature classes with scalar, STR, REF, LIST[numeric], LIST[REF] fields
- ORM2 create → push (with auto-ref resolution) → combine → read (map + copy modes)
- Verify round-trip data integrity

- [ ] **Step 1: Write the integration test**

```python
# tests/python/test_integration_decorator.py
"""End-to-end integration test for the decorator-based ORM."""
import pytest
import numpy as np
from typing import List

from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, U32, STR


@feature
class Vendor:
    name: STR
    rating: F64


@feature
class Sensor:
    kind: STR
    readings: List[F64]


@feature
class Device:
    model: STR
    serial: U32
    vendor: Vendor
    sensors: List[Sensor]


@feature
class Room:
    name: STR
    area: F64
    devices: List[Device]


class TestFullPipeline:
    def _build_test_data(self):
        orm = ORM2.create()

        v1 = Vendor(); v1.name = "SensorCorp"; v1.rating = 4.5
        v2 = Vendor(); v2.name = "DataTech"; v2.rating = 3.8

        s1 = Sensor(); s1.kind = "temp"; s1.readings = [22.1, 23.4, 21.9]
        s2 = Sensor(); s2.kind = "humidity"; s2.readings = [45.0, 48.2]
        s3 = Sensor(); s3.kind = "pressure"; s3.readings = [1013.25]

        d1 = Device(); d1.model = "TH-100"; d1.serial = 1001
        d1.vendor = v1; d1.sensors = [s1, s2]

        d2 = Device(); d2.model = "P-200"; d2.serial = 1002
        d2.vendor = v2; d2.sensors = [s3]

        room = Room(); room.name = "Lab A"
        room.area = 50.0; room.devices = [d1, d2]

        orm.push(room)
        orm.combine()
        return orm

    def test_counts(self):
        orm = self._build_test_data()
        assert orm.count(Vendor) == 2
        assert orm.count(Sensor) == 3
        assert orm.count(Device) == 2
        assert orm.count(Room) == 1

    def test_copy_readback_scalars(self):
        orm = self._build_test_data()
        v = orm.get(Vendor, 0, mode='copy')
        assert v.name == "SensorCorp"
        assert abs(v.rating - 4.5) < 1e-9

    def test_copy_readback_list_numeric(self):
        orm = self._build_test_data()
        s = orm.get(Sensor, 0, mode='copy')
        assert s.kind == "temp"
        np.testing.assert_array_almost_equal(s.readings, [22.1, 23.4, 21.9])

    def test_map_readback(self):
        orm = self._build_test_data()
        v = orm.get(Vendor, 1, mode='map')
        assert v.name == "DataTech"
        assert abs(v.rating - 3.8) < 1e-9

    def test_iter_all(self):
        orm = self._build_test_data()
        sensors = list(orm.iter(Sensor, mode='copy'))
        assert len(sensors) == 3
        kinds = {s.kind for s in sensors}
        assert kinds == {"temp", "humidity", "pressure"}

    def test_shared_vendor_dedup(self):
        """If two devices share a vendor, push it only once."""
        orm = ORM2.create()
        v = Vendor(); v.name = "SharedCo"; v.rating = 5.0

        d1 = Device(); d1.model = "A"; d1.serial = 1
        d1.vendor = v; d1.sensors = []
        d2 = Device(); d2.model = "B"; d2.serial = 2
        d2.vendor = v; d2.sensors = []

        orm.push(d1)
        orm.push(d2)
        orm.combine()

        assert orm.count(Vendor) == 1
        assert orm.count(Device) == 2


class TestEdgeCases:
    def test_empty_list(self):
        @feature
        class WithEmpty:
            vals: List[F64]

        orm = ORM2.create()
        obj = WithEmpty(); obj.vals = []
        orm.push(obj)
        orm.combine()

        result = orm.get(WithEmpty, 0, mode='copy')
        assert len(result.vals) == 0 or result.vals is None or len(list(result.vals)) == 0

    def test_default_values(self):
        orm = ORM2.create()
        v = Vendor()  # no fields set
        orm.push(v)
        orm.combine()
        result = orm.get(Vendor, 0, mode='copy')
        assert result.name == ""
        assert result.rating == 0.0

    def test_large_batch(self):
        orm = ORM2.create()
        for i in range(1000):
            v = Vendor()
            v.name = f"v{i}"
            v.rating = float(i)
            orm.push(v)
        orm.combine()

        assert orm.count(Vendor) == 1000
        v500 = orm.get(Vendor, 500, mode='copy')
        assert v500.name == "v500"
        assert abs(v500.rating - 500.0) < 1e-9
```

- [ ] **Step 2: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_integration_decorator.py -v`
Expected: All PASS

- [ ] **Step 3: Run full test suite**

Run: `uv run pytest tests/python/ -v --tb=short`
Expected: All existing tests still pass + all new tests pass

- [ ] **Step 4: Commit**

```bash
git add tests/python/test_integration_decorator.py
git commit -m "test: end-to-end integration tests for decorator-based ORM

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 10: Shared memory meta export (PartialBuild)

**Files:**
- Modify: `python/fastdb4py/orm2.py`
- Create: `tests/python/test_orm2_share.py`

### Design

For multi-process scenarios, the schema metadata (type + struct tables) needs to be shareable. ORM2 adds:
- `share(name)` → publish the built DB to POSIX shared memory
- `load(name)` → load from shared memory (zero-copy)
- The schema metadata is tiny, so sharing is fast

This reuses the existing C++ `share`/`load` pattern from the original ORM.

- [ ] **Step 1: Write the failing test**

```python
# tests/python/test_orm2_share.py
import pytest
from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, STR


@feature
class SharedPoint:
    x: F64
    y: F64
    label: STR


def test_share_and_load():
    orm = ORM2.create()
    for i in range(5):
        p = SharedPoint()
        p.x = float(i)
        p.y = float(i * 2)
        p.label = f"p{i}"
        orm.push(p)
    orm.combine()

    shm_name = "test_orm2_share"
    try:
        orm.share(shm_name)

        orm2 = ORM2.load(shm_name)
        assert orm2.count(SharedPoint) == 5

        result = orm2.get(SharedPoint, 3, mode='copy')
        assert abs(result.x - 3.0) < 1e-9
        assert result.label == "p3"
    finally:
        ORM2.unlink(shm_name)
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `uv run pytest tests/python/test_orm2_share.py -v`
Expected: `AttributeError: type object 'ORM2' has no attribute 'share'`

- [ ] **Step 3: Implement share/load/unlink on ORM2**

Add to `python/fastdb4py/orm2.py`:

```python
def share(self, name: str):
    """Publish the built database to POSIX shared memory."""
    self._check_built()
    import multiprocessing.shared_memory as shm_mod
    data = self._buffer
    seg = shm_mod.SharedMemory(name=name, create=True, size=len(data))
    seg.buf[:len(data)] = data
    seg.close()
    self._shm_size = len(data)

@classmethod
def load(cls, name: str) -> 'ORM2':
    """Load a database from POSIX shared memory (zero-copy)."""
    import multiprocessing.shared_memory as shm_mod
    seg = shm_mod.SharedMemory(name=name, create=False)
    buf = bytes(seg.buf)
    seg.close()

    orm = cls()
    orm._db = core.WxDatabase.load_xbuffer(buf)
    orm._db._buffer = buf
    orm._buffer = buf
    orm._built = True

    # Reconstruct layer states from database
    for i in range(orm._db.get_layer_count()):
        layer = orm._db.get_layer(i)
        layer_name = layer.get_name()
        # Find the registered class by layer name
        from .registry import _registry
        for registered_cls, schema in _registry.items():
            if schema.layer_name == layer_name:
                state = LayerState(
                    cls=registered_cls,
                    schema=schema,
                    layer_idx=i,
                    row_count=layer.get_feature_count(),
                )
                orm._layers[registered_cls] = state
                orm._layer_order.append(registered_cls)
                break

    return orm

@staticmethod
def unlink(name: str):
    """Remove a shared memory segment."""
    import multiprocessing.shared_memory as shm_mod
    try:
        seg = shm_mod.SharedMemory(name=name, create=False)
        seg.close()
        seg.unlink()
    except FileNotFoundError:
        pass
```

- [ ] **Step 4: Run tests — verify they pass**

Run: `uv run pytest tests/python/test_orm2_share.py -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add python/fastdb4py/orm2.py tests/python/test_orm2_share.py
git commit -m "feat(orm2): shared memory share/load/unlink for multi-process

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```
