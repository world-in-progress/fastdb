# ORM2 Push Performance Optimization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Match or exceed old ORM push performance by deferring all serialization to `combine()` time with batch C calls.

**Architecture:** Extract push compilation infrastructure from `feature/_schema.py` into shared `push_compiler.py`. ORM2's `push()` becomes a pure Python append with dedup. `combine()` groups objects by class, topologically sorts classes by REF dependencies, and batch-pushes each class using compiled C functions.

**Tech Stack:** Python 3.10+, SWIG C extension (`_fastdb4py`), exec-compiled push functions, `functools.partial`

**File Map:**

| File | Action | Responsibility |
|------|--------|----------------|
| `python/fastdb4py/push_compiler.py` | CREATE | Shared push compilation: C ext refs, compile_push_fn, compile_ref_push_fn, make_inlined_dispatch, make_batch_dispatch |
| `python/fastdb4py/feature/_schema.py` | MODIFY | Replace inline implementations with imports from push_compiler |
| `python/fastdb4py/registry.py` | MODIFY | Add push plans, ref_fields, list_ref_fields, has_ref_fields to LayerSchema |
| `python/fastdb4py/orm2.py` | MODIFY | Deferred batch push + topo sort combine |
| `python/fastdb4py/push.py` | MODIFY | Use push_compiler C refs; becomes thin wrapper for REF push path |
| `tests/python/test_orm2_batch.py` | CREATE | Batch correctness, topo sort, dedup, mutation, perf parity tests |

---

### Task 1: Extract push_compiler.py from feature/_schema.py

**Files:**
- Create: `python/fastdb4py/push_compiler.py`
- Modify: `python/fastdb4py/feature/_schema.py`

- [ ] **Step 1: Create `push_compiler.py` with C extension refs and `compile_push_fn`**

```python
# python/fastdb4py/push_compiler.py
"""Shared push compilation infrastructure for both old ORM and ORM2.

Extracted from feature/_schema.py to avoid coupling ORM2 to the Feature subclass system.
"""
import struct as _struct
import functools as _ft

from .core import _fastdb4py as _fdb_c

# Direct C extension function references — bypass SWIG wrapper (~200ns/call savings)
_c_add_begin  = _fdb_c.WxLayerTableBuild_add_feature_begin
_c_add_end    = _fdb_c.WxLayerTableBuild_add_feature_end
_c_set_field  = _fdb_c.WxLayerTableBuild_set_field
_c_set_cstr   = _fdb_c.WxLayerTableBuild_set_field_cstring
_c_set_wstr   = _fdb_c.WxLayerTableBuild_set_field_wstring
_c_set_raw    = _fdb_c.WxLayerTableBuild_set_geometry_raw
_c_set_list   = _fdb_c.WxLayerTableBuild_set_field_list_numeric
_c_push_dict  = _fdb_c.WxLayerTableBuild_push_from_dict
_c_pfd_fc     = _fdb_c.WxLayerTableBuild_push_from_dict_fc
_c_pmfd_fc    = getattr(_fdb_c, 'WxLayerTableBuild_push_many_from_dicts_fc', None)


def compile_push_fn(numeric_plan, str_plan, bytes_plan, list_plan):
    """Generate and compile a specialized per-class push function.

    The compiled function signature is:
        push_fn(cache, t) -> None

    For list fields, uses per-field int-keyed dicts for fast struct.Struct
    pack-method lookup.
    """
    lines = ['def _push(cache, t, _ab=_c_add_begin, _ae=_c_add_end, _sf=_c_set_field, _sfc=_c_set_cstr, _sfw=_c_set_wstr, _sr=_c_set_raw, _sl=_c_set_list):']
    lines.append('    _ab(t)')
    for idx, fn in numeric_plan:
        lines.append(f'    _sf(t, {idx}, cache.get({fn!r}) or 0)')
    for idx, fn, is_wide in str_plan:
        if is_wide:
            lines.append(f'    _sfw(t, {idx}, cache.get({fn!r}) or "")')
        else:
            lines.append(f'    _sfc(t, {idx}, cache.get({fn!r}) or "")')
    for idx, fn in bytes_plan:
        lines.append(f'    _sr(t, cache.get({fn!r}) or b"")')
    for i, (idx, fn, typecode) in enumerate(list_plan):
        gv = f'_gsp{i}'
        lines.append(f'    _items = cache.get({fn!r}) or []')
        lines.append(f'    _n = len(_items)')
        lines.append(f'    _sl(t, {idx}, ({gv}[_n] if _n in {gv} else {gv}.setdefault(_n, _SS(str(_n)+{typecode!r}).pack))(*_items))')
    lines.append('    _ae(t)')
    src = '\n'.join(lines)
    ns: dict = {f'_gsp{i}': {} for i in range(len(list_plan))}
    ns['_c_add_begin'] = _c_add_begin
    ns['_c_add_end'] = _c_add_end
    ns['_c_set_field'] = _c_set_field
    ns['_c_set_cstr'] = _c_set_cstr
    ns['_c_set_wstr'] = _c_set_wstr
    ns['_c_set_raw'] = _c_set_raw
    ns['_c_set_list'] = _c_set_list
    if list_plan:
        ns['_SS'] = _struct.Struct
    exec(compile(src, '<push_fn>', 'exec'), ns)
    return ns['_push']


def compile_ref_push_fn(numeric_plan, str_plan, bytes_plan, list_plan,
                        ref_plan, list_ref_plan):
    """Generate a push function that handles pre-resolved REF fields.

    Like compile_push_fn but with two extra plan types:
      - ref_plan: List[(field_id, field_name)]
          Scalar REF values have been resolved to WxFeatureRef ints before calling.
          Generated code: _sf(t, idx, cache.get(name) or 0)
      - list_ref_plan: List[(field_id, field_name)]
          LIST[REF] values have been pre-packed to raw bytes before calling.
          Generated code: _sl_raw(t, idx, cache.get(name) or b"")
    """
    lines = ['def _push(cache, t, _ab=_c_add_begin, _ae=_c_add_end, _sf=_c_set_field, _sfc=_c_set_cstr, _sfw=_c_set_wstr, _sr=_c_set_raw, _sl=_c_set_list):']
    lines.append('    _ab(t)')
    for idx, fn in numeric_plan:
        lines.append(f'    _sf(t, {idx}, cache.get({fn!r}) or 0)')
    for idx, fn, is_wide in str_plan:
        if is_wide:
            lines.append(f'    _sfw(t, {idx}, cache.get({fn!r}) or "")')
        else:
            lines.append(f'    _sfc(t, {idx}, cache.get({fn!r}) or "")')
    for idx, fn in bytes_plan:
        lines.append(f'    _sr(t, cache.get({fn!r}) or b"")')
    for i, (idx, fn, typecode) in enumerate(list_plan):
        gv = f'_gsp{i}'
        lines.append(f'    _items = cache.get({fn!r}) or []')
        lines.append(f'    _n = len(_items)')
        lines.append(f'    _sl(t, {idx}, ({gv}[_n] if _n in {gv} else {gv}.setdefault(_n, _SS(str(_n)+{typecode!r}).pack))(*_items))')
    # Scalar REF fields — value already resolved to int
    for idx, fn in ref_plan:
        lines.append(f'    _sf(t, {idx}, cache.get({fn!r}) or 0)')
    # LIST[REF] fields — value already packed as raw bytes
    for idx, fn in list_ref_plan:
        lines.append(f'    _sl(t, {idx}, cache.get({fn!r}) or b"")')
    lines.append('    _ae(t)')
    src = '\n'.join(lines)
    ns: dict = {f'_gsp{i}': {} for i in range(len(list_plan))}
    ns['_c_add_begin'] = _c_add_begin
    ns['_c_add_end'] = _c_add_end
    ns['_c_set_field'] = _c_set_field
    ns['_c_set_cstr'] = _c_set_cstr
    ns['_c_set_wstr'] = _c_set_wstr
    ns['_c_set_raw'] = _c_set_raw
    ns['_c_set_list'] = _c_set_list
    if list_plan:
        ns['_SS'] = _struct.Struct
    exec(compile(src, '<ref_push_fn>', 'exec'), ns)
    return ns['_push']


def make_inlined_dispatch(numeric_plan, str_plan, bytes_plan, list_plan, t_obj,
                          pfd_num_names=None, pfd_num_ids=None,
                          pfd_str_names=None, pfd_str_ids=None):
    """Generate a per-(class, table) inlined push+dispatch function.

    For simple features (numeric + cstring only): uses push_from_dict_fc.
    For complex features (wstr, bytes, list): per-field C extension calls.
    """
    t_origin = t_obj._origin

    use_pfd = (
        not bytes_plan and not list_plan and
        all(not is_wide for _, _, is_wide in str_plan) and
        pfd_num_names is not None
    )

    if use_pfd:
        return _ft.partial(_c_pfd_fc, t_origin,
                           pfd_num_names, pfd_num_ids,
                           pfd_str_names, pfd_str_ids,
                           t_obj._fc)

    lines = ['def _dispatch(cache, _ab=_c_ab, _ae=_c_ae, _sf=_c_sf, _sfc=_c_sfc, _to=to, _t=t_obj, _SS=None):']
    lines.append('    _ab(_to)')
    for idx, fn in numeric_plan:
        lines.append(f'    _sf(_to, {idx}, cache.get({fn!r}) or 0)')
    for idx, fn, is_wide in str_plan:
        if is_wide:
            lines.append(f'    _c_wstr(_to, {idx}, cache.get({fn!r}) or "")')
        else:
            lines.append(f'    _sfc(_to, {idx}, cache.get({fn!r}) or "")')
    for idx, fn in bytes_plan:
        lines.append(f'    _c_raw(_to, cache.get({fn!r}) or b"")')
    for i, (idx, fn, typecode) in enumerate(list_plan):
        gv = f'_gsp{i}'
        lines.append(f'    _items = cache.get({fn!r}) or []')
        lines.append(f'    _n = len(_items)')
        lines.append(f'    _c_list(_to, {idx}, ({gv}[_n] if _n in {gv} else {gv}.setdefault(_n, _SS(str(_n)+{typecode!r}).pack))(*_items))')
    lines.append('    _ae(_to)')
    lines.append('    _t.feature_count += 1')
    src = '\n'.join(lines)
    ns: dict = {
        '_c_ab': _c_add_begin, '_c_ae': _c_add_end,
        '_c_sf': _c_set_field, '_c_sfc': _c_set_cstr,
        '_c_wstr': _c_set_wstr, '_c_raw': _c_set_raw, '_c_list': _c_set_list,
        'to': t_origin, 't_obj': t_obj,
        **{f'_gsp{i}': {} for i in range(len(list_plan))},
    }
    if list_plan:
        ns['_SS'] = _struct.Struct
    exec(compile(src, '<inlined_dispatch>', 'exec'), ns)
    return ns['_dispatch']


def make_batch_inlined_dispatch(numeric_plan, str_plan, bytes_plan, list_plan, t_obj,
                                pfd_num_names=None, pfd_num_ids=None,
                                pfd_str_names=None, pfd_str_ids=None):
    """Like make_inlined_dispatch but returns a function that accepts a LIST of cache dicts.

    Returns None if the batch path is unavailable.
    """
    if _c_pmfd_fc is None:
        return None
    use_pfd = (
        not bytes_plan and not list_plan and
        all(not is_wide for _, _, is_wide in str_plan) and
        pfd_num_names is not None
    )
    if not use_pfd:
        return None
    return _ft.partial(_c_pmfd_fc, t_obj._origin,
                        pfd_num_names, pfd_num_ids,
                        pfd_str_names, pfd_str_ids,
                        t_obj._fc)
```

- [ ] **Step 2: Update `feature/_schema.py` to import from `push_compiler`**

Replace lines 11-23 and functions `_compile_push_fn`, `make_inlined_dispatch`, `make_batch_inlined_dispatch` with imports:

```python
# Replace the C extension imports (lines 11-23) with:
from ..push_compiler import (
    compile_push_fn as _compile_push_fn,
    make_inlined_dispatch, make_batch_inlined_dispatch,
    _c_add_begin, _c_add_end, _c_set_field, _c_set_cstr,
    _c_set_wstr, _c_set_raw, _c_set_list,
    _c_push_dict, _c_pfd_fc, _c_pmfd_fc,
)

# Delete the inline definitions of:
# - _compile_push_fn (lines 124-163)
# - make_inlined_dispatch (lines 166-226)
# - make_batch_inlined_dispatch (lines 228-250)
# ClassSchema.__init__ already calls _compile_push_fn — no change needed there.
```

- [ ] **Step 3: Run all tests to verify extraction is behavior-preserving**

Run: `uv run pytest tests/python/ -x -q`
Expected: 215 tests pass (zero failures)

- [ ] **Step 4: Commit**

```bash
git add python/fastdb4py/push_compiler.py python/fastdb4py/feature/_schema.py
git commit -m "refactor: extract push_compiler.py from feature/_schema.py

Shared push compilation infrastructure for both old ORM and ORM2.
No behavior change — all imports redirected."
```

---

### Task 2: Enhance LayerSchema with push plans and ref field lists

**Files:**
- Modify: `python/fastdb4py/registry.py:16-41`

- [ ] **Step 1: Add `list_ref_target` to FieldDef and fix `_resolve_ref_target`**

```python
# registry.py — update FieldDef (line 16-24):
@dataclass(frozen=True, slots=True)
class FieldDef:
    """Metadata for a single field in a @feature class."""
    name: str
    field_type: OriginFieldType
    field_id: int                          # 0-based column index
    cpp_type: int                          # raw C++ FieldTypeEnum int
    ref_target: Optional[Type] = None      # target class for REF fields
    list_elem_type: Optional[OriginFieldType] = None
    list_ref_target: Optional[Type] = None  # target class for List[REF] fields
```

Also add `_resolve_list_ref_target` and update `_build_schema`:

```python
def _resolve_list_ref_target(ft: OriginFieldType, hint) -> Optional[Type]:
    """Extract the target class from List[SomeFeatureClass] annotations."""
    if ft != OriginFieldType.list:
        return None
    elem_ft = get_list_element_type(hint)
    if elem_ft != OriginFieldType.ref:
        return None
    args = get_args(hint)
    if args and isinstance(args[0], type):
        return args[0]
    return None
```

Update `_build_schema` to pass `list_ref_target`:

```python
# In _build_schema, update the FieldDef constructor call:
fields.append(FieldDef(
    name=name, field_type=ft, field_id=field_id,
    cpp_type=cpp_type, ref_target=ref_target, list_elem_type=list_elem,
    list_ref_target=_resolve_list_ref_target(ft, hint),
))
```

- [ ] **Step 2: Add push plan fields and ref field lists to LayerSchema**

```python
# registry.py — replace existing LayerSchema class (lines 27-41) with:
class LayerSchema:
    """Schema for one @feature class (= one fastdb layer)."""
    __slots__ = (
        'layer_name', 'fields', '_by_name',
        # Push plans — computed once at @feature time
        'numeric_plan',     # List[(field_id, field_name)]
        'str_plan',         # List[(field_id, field_name, is_wide)]
        'bytes_plan',       # List[(field_id, field_name)]
        'list_plan',        # List[(field_id, field_name, typecode)]
        'ref_fields',       # List[FieldDef] where field_type == ref
        'list_ref_fields',  # List[FieldDef] where list_elem_type == ref
        'has_ref_fields',   # bool — any ref or list[ref] fields
        # Compiled push fns — created at combine() time, NOT at @feature time
        'push_fn',          # compiled zero-branch push function
        'batch_fn',         # batch push (push_many_from_dicts_fc partial)
        # push_from_dict helpers
        'pfd_num_names',    # List[str]
        'pfd_num_ids',      # numpy uint32 array
        'pfd_str_names',    # List[str]
        'pfd_str_ids',      # numpy uint32 array
    )

    def __init__(self, layer_name: str, fields: List[FieldDef]):
        self.layer_name = layer_name
        self.fields = fields
        self._by_name: Dict[str, FieldDef] = {f.name: f for f in fields}
        # Push plans
        self.numeric_plan = []
        self.str_plan = []
        self.bytes_plan = []
        self.list_plan = []
        self.ref_fields = []
        self.list_ref_fields = []
        self.has_ref_fields = False
        # Compiled push fns — populated later by ORM2.combine()
        self.push_fn = None
        self.batch_fn = None
        self.pfd_num_names = []
        self.pfd_num_ids = None
        self.pfd_str_names = []
        self.pfd_str_ids = None

    def get(self, name: str) -> Optional[FieldDef]:
        return self._by_name.get(name)

    def __len__(self):
        return len(self.fields)
```

- [ ] **Step 2: Pre-compute plans in `_build_schema`**

Add plan computation at the end of `_build_schema()` (after line 84):

```python
# At the end of _build_schema(), before `return schema`:
import numpy as np
from .type import LIST_ELEM_ARRAY_TYPECODE

_NUMERIC_FT = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.f32, OriginFieldType.f64,
    OriginFieldType.u8n, OriginFieldType.u16n,
))
for fd in fields:
    ft = fd.field_type
    if ft in _NUMERIC_FT:
        schema.numeric_plan.append((fd.field_id, fd.name))
    elif ft == OriginFieldType.str:
        schema.str_plan.append((fd.field_id, fd.name, False))
    elif ft == OriginFieldType.wstr:
        schema.str_plan.append((fd.field_id, fd.name, True))
    elif ft == OriginFieldType.bytes:
        schema.bytes_plan.append((fd.field_id, fd.name))
    elif ft == OriginFieldType.list:
        if fd.list_elem_type == OriginFieldType.ref:
            # list[ref] is NOT added to list_plan — handled separately
            # via raw bytes write in combine()'s REF path
            pass
        else:
            typecode = LIST_ELEM_ARRAY_TYPECODE.get(fd.list_elem_type, 'd')
            schema.list_plan.append((fd.field_id, fd.name, typecode))
    if ft == OriginFieldType.ref:
        schema.ref_fields.append(fd)
    if ft == OriginFieldType.list and fd.list_elem_type == OriginFieldType.ref:
        schema.list_ref_fields.append(fd)

schema.has_ref_fields = bool(schema.ref_fields or schema.list_ref_fields)
schema.pfd_num_names = [fn for _, fn in schema.numeric_plan]
schema.pfd_num_ids = np.array([idx for idx, _ in schema.numeric_plan], dtype=np.uint32)
schema.pfd_str_names = [fn for _, fn, _ in schema.str_plan]
schema.pfd_str_ids = np.array([idx for idx, _, _ in schema.str_plan], dtype=np.uint32)
```

- [ ] **Step 3: Run all tests**

Run: `uv run pytest tests/python/ -x -q`
Expected: 215 tests pass

- [ ] **Step 4: Commit**

```bash
git add python/fastdb4py/registry.py
git commit -m "feat(registry): add push plans and ref field lists to LayerSchema

Pre-compute numeric/str/bytes/list plans and ref_fields/list_ref_fields
at schema build time. push_fn/batch_fn remain None until combine() time."
```

---

### Task 3: Update push.py to use push_compiler C refs

**Files:**
- Modify: `python/fastdb4py/push.py:1-10`

This task replaces the SWIG method-resolution overhead in `push.py` with direct C extension calls from `push_compiler`. The `push_feature()` function in `push.py` is still used by ORM2 for the REF path during migration, so it must keep working.

- [ ] **Step 1: Import C extension refs from push_compiler**

At the top of `push.py`, add after existing imports:

```python
# python/fastdb4py/push.py — add after line 10:
from .push_compiler import (
    _c_add_begin, _c_add_end, _c_set_field, _c_set_cstr,
    _c_set_wstr, _c_set_raw, _c_set_list,
)
```

- [ ] **Step 2: Replace method calls with direct C calls in `_set_field`**

Replace the `_set_field` function body to use direct C calls:

```python
def _set_field(layer_build, fd, value, ref_resolver):
    """Set a single field in the layer build."""
    ft = fd.field_type
    fid = fd.field_id
    if ft in _INT_TYPES:
        _c_set_field(layer_build, fid, int(value) if value is not None else 0)
    elif ft in _FLOAT_TYPES:
        _c_set_field(layer_build, fid, float(value) if value is not None else 0.0)
    elif ft == OriginFieldType.str:
        _c_set_cstr(layer_build, fid, str(value) if value is not None else "")
    elif ft == OriginFieldType.wstr:
        _c_set_wstr(layer_build, fid, str(value) if value is not None else "")
    elif ft == OriginFieldType.bytes:
        _c_set_raw(layer_build, value if value is not None else b"")
    elif ft == OriginFieldType.ref:
        if value is not None and ref_resolver is not None:
            ref = ref_resolver(value)
            if ref is not None:
                _c_set_field(layer_build, fid, ref)
    elif ft == OriginFieldType.list:
        _set_list_field(layer_build, fd, value, ref_resolver)
```

Also update `push_feature` to use direct C calls for begin/end:

```python
def push_feature(obj, layer_build, schema, ref_resolver=None):
    cache = obj.__dict__
    _c_add_begin(layer_build)
    for fd in schema.fields:
        value = cache.get(fd.name)
        _set_field(layer_build, fd, value, ref_resolver)
    _c_add_end(layer_build)
    return -1
```

- [ ] **Step 3: Run all tests**

Run: `uv run pytest tests/python/ -x -q`
Expected: 215 tests pass

- [ ] **Step 4: Commit**

```bash
git add python/fastdb4py/push.py
git commit -m "perf(push): use direct C extension calls from push_compiler

Replaces SWIG method-resolution in push.py with direct _c_* calls.
~200ns savings per field set call."
```

---

### Task 4: Rewrite ORM2.push() as deferred append

**Files:**
- Modify: `python/fastdb4py/orm2.py:1-96`

The push() method becomes a pure Python append — no C calls, no schema lookup per push.

- [ ] **Step 1: Replace imports and add new data structures**

Replace the top of `orm2.py`:

```python
# python/fastdb4py/orm2.py
"""ORM2: decorator-based ORM for @feature classes."""
from __future__ import annotations
from collections import defaultdict
from typing import Any, Dict, Iterator, List, Optional, Type
import struct
import numpy as np

from . import core
from .registry import get_schema, LayerSchema, FieldDef
from .reader import map_feature, copy_feature
from .type import OriginFieldType
from .push_compiler import (
    compile_push_fn, compile_ref_push_fn,
    make_inlined_dispatch, make_batch_inlined_dispatch,
    _c_pmfd_fc,
)
```

- [ ] **Step 2: Rewrite ORM2.__init__ and push()**

Replace the `ORM2.__init__` and `push` methods:

```python
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
        self._layer_order: List[Type] = []
        self._built = False
        # Deferred batch push state
        self._pending: List[Any] = []          # all pushed objects in push order
        self._pushed_ids: Dict[int, bool] = {} # id(obj) -> True (dedup set)
        self._pushed_objs: List[Any] = []      # prevent GC so id() stays unique

    @classmethod
    def create(cls) -> 'ORM2':
        """Start a new build session."""
        orm = cls()
        orm._db_build = core.WxDatabaseBuild()
        orm._db_build.begin("")
        return orm

    def push(self, obj: Any) -> None:
        """Queue a @feature object for batch serialization at combine() time.

        Automatically handles REF and LIST[REF] fields — no manual ordering needed.
        Object mutation after push is visible at combine() time.
        Deduplicates by object identity (same id() → same row).
        """
        obj_id = id(obj)
        if obj_id in self._pushed_ids:
            return
        self._pushed_ids[obj_id] = True
        self._pending.append(obj)
        self._pushed_objs.append(obj)  # prevent GC so id() stays unique
        # Recursively enqueue REF dependencies
        schema = get_schema(type(obj))
        if schema.has_ref_fields:
            self._enqueue_deps(obj, schema)

    def _enqueue_deps(self, obj: Any, schema: LayerSchema):
        """Recursively enqueue REF and LIST[REF] dependencies."""
        cache = obj.__dict__
        for fd in schema.ref_fields:
            val = cache.get(fd.name)
            if val is not None:
                self.push(val)
        for fd in schema.list_ref_fields:
            items = cache.get(fd.name)
            if items:
                for ref_obj in items:
                    if ref_obj is not None:
                        self.push(ref_obj)
```

- [ ] **Step 3: Run existing tests (expect some failures — combine not yet rewritten)**

Run: `uv run pytest tests/python/test_orm2.py -x -q`
Expected: Tests that only test push+count may pass; tests that test combine+read will fail.
This is expected — we verify push works before rewriting combine in the next task.

- [ ] **Step 4: Commit (WIP)**

```bash
git add python/fastdb4py/orm2.py
git commit -m "wip: rewrite ORM2.push() as deferred append

push() is now a pure Python append with dedup.
combine() not yet updated — some tests expected to fail."
```

---

### Task 5: Rewrite ORM2.combine() with topo sort + batch push

**Files:**
- Modify: `python/fastdb4py/orm2.py:116-131`

This is the core performance task. `combine()` groups objects by class, topologically sorts classes, and uses batch C calls.

- [ ] **Step 1: Add topological sort helper**

Add this helper function above the `ORM2` class in `orm2.py`:

```python
def _topo_sort_classes(groups: Dict[Type, List[Any]]) -> List[Type]:
    """Topological sort classes by REF field dependencies (Kahn's algorithm).

    Classes with no REF dependencies come first so their rows exist
    when REF-dependent classes resolve references.
    """
    in_degree: Dict[Type, int] = {cls: 0 for cls in groups}
    adj: Dict[Type, List[Type]] = {cls: [] for cls in groups}

    for cls in groups:
        schema = get_schema(cls)
        deps = set()
        for fd in schema.ref_fields:
            if fd.ref_target is not None and fd.ref_target in groups and fd.ref_target != cls:
                deps.add(fd.ref_target)
        for fd in schema.list_ref_fields:
            if fd.list_ref_target is not None and fd.list_ref_target in groups and fd.list_ref_target != cls:
                deps.add(fd.list_ref_target)
        for dep in deps:
            adj[dep].append(cls)
            in_degree[cls] += 1

    queue = [cls for cls, deg in in_degree.items() if deg == 0]
    result: List[Type] = []
    while queue:
        cls = queue.pop(0)
        result.append(cls)
        for child in adj[cls]:
            in_degree[child] -= 1
            if in_degree[child] == 0:
                queue.append(child)

    if len(result) != len(groups):
        missing = set(groups) - set(result)
        names = [c.__name__ for c in missing]
        raise RuntimeError(
            f"Circular class-level REF dependency detected among: {names}. "
            "ORM2 does not support circular class references."
        )
    return result
```

- [ ] **Step 2: Rewrite combine() method**

Replace the existing `combine()` method:

```python
    def combine(self):
        """Finalize: batch-push all pending objects, then build read-only database."""
        if self._built:
            raise RuntimeError("ORM2 already combined")
        if self._db_build is None:
            raise RuntimeError("ORM2 not in build mode")

        # 1. Group pending objects by class
        groups: Dict[Type, List[Any]] = defaultdict(list)
        for obj in self._pending:
            groups[type(obj)].append(obj)

        # 2. Topological sort classes by REF dependencies
        sorted_classes = _topo_sort_classes(groups)

        # 3. Push each class in topo order
        obj_to_row: Dict[int, tuple] = {}  # id(obj) -> (layer_idx, row_idx)
        for cls in sorted_classes:
            objs = groups[cls]
            schema = get_schema(cls)
            state = self._ensure_layer(cls)
            layer_build = state.build

            if not schema.has_ref_fields:
                # FAST PATH: batch push via push_many_from_dicts_fc
                batch_fn = make_batch_inlined_dispatch(
                    schema.numeric_plan, schema.str_plan,
                    schema.bytes_plan, schema.list_plan, state,
                    schema.pfd_num_names, schema.pfd_num_ids,
                    schema.pfd_str_names, schema.pfd_str_ids,
                )
                if batch_fn is not None:
                    # Batch C call: 1024 dicts at a time
                    dicts = [obj.__dict__ for obj in objs]
                    for i in range(0, len(dicts), 1024):
                        batch_fn(dicts[i:i+1024])
                else:
                    # Fallback: single compiled push_fn
                    push_fn = compile_push_fn(
                        schema.numeric_plan, schema.str_plan,
                        schema.bytes_plan, schema.list_plan,
                    )
                    for obj in objs:
                        push_fn(obj.__dict__, layer_build)

                # Record row indices
                for row_idx, obj in enumerate(objs):
                    obj_to_row[id(obj)] = (state.layer_idx, row_idx)
                state.row_count = len(objs)
            else:
                # REF PATH: pre-resolve all refs into cache, then use compiled fn.
                # compile_ref_push_fn handles:
                #   - numeric/str/bytes/list(non-ref) from standard plans
                #   - scalar REF as set_field(idx, int) — value already resolved
                #   - list[ref] as set_field_list_numeric(idx, bytes) — already packed
                ref_push_fn = compile_ref_push_fn(
                    schema.numeric_plan, schema.str_plan,
                    schema.bytes_plan, schema.list_plan,
                    [(fd.field_id, fd.name) for fd in schema.ref_fields],
                    [(fd.field_id, fd.name) for fd in schema.list_ref_fields],
                )
                for obj in objs:
                    row_idx = state.row_count
                    cache = obj.__dict__.copy()  # COPY — don't mutate original
                    # Resolve scalar REF fields → WxFeatureRef int
                    for fd in schema.ref_fields:
                        ref_obj = cache.get(fd.name)
                        if ref_obj is not None:
                            loc = obj_to_row.get(id(ref_obj))
                            if loc is not None:
                                li, ri = loc
                                cache[fd.name] = core.WxFeatureRef.make_ref(li, ri)
                            else:
                                cache[fd.name] = 0
                    # Pack LIST[REF] fields → raw bytes
                    for fd in schema.list_ref_fields:
                        items = cache.get(fd.name)
                        if items:
                            parts = []
                            for ref_obj in items:
                                if ref_obj is not None:
                                    loc = obj_to_row.get(id(ref_obj))
                                    if loc is not None:
                                        li, ri = loc
                                        parts.append(struct.pack('<HBH', li, ri & 0xFF, ri >> 8))
                                    else:
                                        parts.append(b'\x00\x00\x00\x00\x00')
                                else:
                                    parts.append(b'\x00\x00\x00\x00\x00')
                            cache[fd.name] = b''.join(parts)
                        else:
                            cache[fd.name] = b''
                    ref_push_fn(cache, layer_build)
                    obj_to_row[id(obj)] = (state.layer_idx, row_idx)
                    state.row_count += 1

        # 4. Finalize into read-only database
        mem = core.WxMemoryStream()
        self._db_build.post(mem)
        buf = mem.data().as_array(np.uint8).tobytes()
        self._db = core.WxDatabase.load_xbuffer(buf)
        self._db._buffer = buf
        self._buffer = buf
        self._built = True
        self._db_build = None
        # Clear pending state
        self._pending.clear()
        self._pushed_ids.clear()
```

- [ ] **Step 3: Update `LayerState` for batch_fn compatibility**

The batch dispatch functions (`make_batch_inlined_dispatch`) access `t_obj._origin` and `t_obj._fc`. The C function `push_many_from_dicts_fc` dereferences `_fc` via `PyArray_DATA()` and **increments it in-place** — so `_fc` MUST be a 1-element numpy int64 array, not a plain int.

```python
import numpy as np

@dataclass
class LayerState:
    """Tracks build state for one @feature class."""
    cls: Type
    schema: LayerSchema
    build: Any = None       # WxLayerTableBuild
    layer_idx: int = -1
    row_count: int = 0
    _fc: Any = None         # np.zeros(1, dtype=np.int64) — C increments in-place

    def __post_init__(self):
        self._fc = np.zeros(1, dtype=np.int64)

    @property
    def _origin(self):
        """Compatibility shim for push_compiler functions that expect t_obj._origin."""
        return self.build

    @property
    def feature_count(self):
        return int(self._fc[0])
```

- [ ] **Step 4: Update count() with O(1) counter**

Add `_pending_counts: defaultdict(int)` to `__init__` and increment in `push()`:

```python
# In __init__:
self._pending_counts: Dict[Type, int] = defaultdict(int)

# In push(), after self._pending.append(obj):
self._pending_counts[type(obj)] += 1

# count() method:
def count(self, cls: Type) -> int:
    """Return the number of features for cls.

    Before combine(): returns pending object count (O(1)).
    After combine(): returns actual row count.
    """
    if self._built:
        state = self._layers.get(cls)
        return state.row_count if state else 0
    return self._pending_counts.get(cls, 0)

# In combine(), after clearing pending:
self._pending_counts.clear()
```

- [ ] **Step 5: Run all ORM2 tests**

Run: `uv run pytest tests/python/test_orm2.py -x -v`
Expected: All 10 tests pass

- [ ] **Step 6: Run full test suite**

Run: `uv run pytest tests/python/ -x -q`
Expected: 215 tests pass

- [ ] **Step 7: Commit**

```bash
git add python/fastdb4py/orm2.py
git commit -m "feat(orm2): batch push via topo-sorted combine()

- push() is pure Python append with dedup (~50ns vs ~7µs before)
- combine() groups by class, topo sorts by REF deps, batch pushes
- Non-REF classes use push_many_from_dicts_fc (1 C call / 1024 dicts)
- REF classes use compiled push_fn with inline ref resolution
- count() returns pending count before combine, row count after"
```

---

### Task 6: Write batch correctness tests

**Files:**
- Create: `tests/python/test_orm2_batch.py`

- [ ] **Step 1: Write test_batch_correctness — 1000 simple objects**

```python
# tests/python/test_orm2_batch.py
import pytest
from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, U32, STR


@feature
class BatchPoint:
    x: F64
    y: F64
    tag: STR


class TestBatchCorrectness:
    def test_1000_simple_objects(self):
        """Push 1000 simple objects, verify all column values after combine."""
        orm = ORM2.create()
        for i in range(1000):
            p = BatchPoint()
            p.x = float(i)
            p.y = float(i * 2)
            p.tag = f"p{i}"
            orm.push(p)
        assert orm.count(BatchPoint) == 1000
        orm.combine()
        for i in range(1000):
            r = orm.get(BatchPoint, i, mode='copy')
            assert abs(r.x - float(i)) < 1e-9, f"x mismatch at {i}"
            assert abs(r.y - float(i * 2)) < 1e-9, f"y mismatch at {i}"
            assert r.tag == f"p{i}", f"tag mismatch at {i}"
```

- [ ] **Step 2: Run test**

Run: `uv run pytest tests/python/test_orm2_batch.py::TestBatchCorrectness::test_1000_simple_objects -v`
Expected: PASS

- [ ] **Step 3: Write test_dedup — same object pushed twice**

```python
    def test_dedup_same_object(self):
        """Push same object twice — should produce only one row."""
        orm = ORM2.create()
        p = BatchPoint()
        p.x = 42.0
        p.y = 0.0
        p.tag = "dupe"
        orm.push(p)
        orm.push(p)
        assert orm.count(BatchPoint) == 1
        orm.combine()
        assert orm.get(BatchPoint, 0, mode='copy').x == 42.0
```

- [ ] **Step 4: Write test_mutation_after_push**

```python
    def test_mutation_after_push(self):
        """Mutating object after push should reflect in combine output.

        This matches old ORM batch semantics — push() stores a reference,
        not a snapshot.
        """
        orm = ORM2.create()
        p = BatchPoint()
        p.x = 1.0
        p.y = 2.0
        p.tag = "before"
        orm.push(p)
        # Mutate AFTER push
        p.x = 99.0
        p.tag = "after"
        orm.combine()
        r = orm.get(BatchPoint, 0, mode='copy')
        assert abs(r.x - 99.0) < 1e-9, "mutation after push should be visible"
        assert r.tag == "after"
```

- [ ] **Step 5: Run all new tests**

Run: `uv run pytest tests/python/test_orm2_batch.py::TestBatchCorrectness -v`
Expected: 3 tests PASS

- [ ] **Step 6: Commit**

```bash
git add tests/python/test_orm2_batch.py
git commit -m "test(orm2): add batch correctness tests

Tests 1000-object batch, dedup, and mutation-after-push semantics."
```

---

### Task 7: Write REF topo sort tests

**Files:**
- Modify: `tests/python/test_orm2_batch.py`

- [ ] **Step 1: Write test_ref_topo_sort — A→B→C chain**

```python
@feature
class Company:
    name: STR

@feature
class Department:
    name: STR
    company: Company

@feature
class Employee:
    name: STR
    dept: Department


class TestRefTopoSort:
    def test_three_level_chain(self):
        """A→B→C: push C first (Employee), deps auto-pushed in correct order."""
        orm = ORM2.create()
        c = Company()
        c.name = "Acme"
        d = Department()
        d.name = "Engineering"
        d.company = c
        e = Employee()
        e.name = "Alice"
        e.dept = d
        orm.push(e)  # should auto-push d and c
        assert orm.count(Company) == 1
        assert orm.count(Department) == 1
        assert orm.count(Employee) == 1
        orm.combine()
        co = orm.get(Company, 0, mode='copy')
        assert co.name == "Acme"
        dep = orm.get(Department, 0, mode='copy')
        assert dep.name == "Engineering"
        emp = orm.get(Employee, 0, mode='copy')
        assert emp.name == "Alice"

    def test_shared_ref_dedup(self):
        """Two objects referencing the same dep — dep pushed once."""
        orm = ORM2.create()
        c = Company()
        c.name = "SharedCo"
        d1 = Department()
        d1.name = "Sales"
        d1.company = c
        d2 = Department()
        d2.name = "Marketing"
        d2.company = c
        orm.push(d1)
        orm.push(d2)
        assert orm.count(Company) == 1
        assert orm.count(Department) == 2
        orm.combine()
        assert orm.get(Company, 0, mode='copy').name == "SharedCo"
```

- [ ] **Step 2: Write test for list[ref]**

```python
    def test_list_ref(self):
        """LIST[REF] field: list of referenced objects."""
        from typing import List

        @feature
        class Tag:
            label: STR

        @feature
        class Article:
            title: STR
            tags: List[Tag]

        orm = ORM2.create()
        t1 = Tag(); t1.label = "python"
        t2 = Tag(); t2.label = "perf"
        a = Article()
        a.title = "ORM2 Optimization"
        a.tags = [t1, t2]
        orm.push(a)
        assert orm.count(Tag) == 2
        assert orm.count(Article) == 1
        orm.combine()
        assert orm.get(Article, 0, mode='copy').title == "ORM2 Optimization"
        assert orm.get(Tag, 0, mode='copy').label == "python"
        assert orm.get(Tag, 1, mode='copy').label == "perf"
```

- [ ] **Step 3: Run tests**

Run: `uv run pytest tests/python/test_orm2_batch.py::TestRefTopoSort -v`
Expected: 3 tests PASS

- [ ] **Step 4: Commit**

```bash
git add tests/python/test_orm2_batch.py
git commit -m "test(orm2): add REF topo sort and LIST[REF] tests

Tests 3-level chain, shared ref dedup, and list[ref] serialization."
```

---

### Task 8: Write performance parity benchmark test

**Files:**
- Modify: `tests/python/test_orm2_batch.py`

- [ ] **Step 1: Write performance comparison test**

```python
import time


class TestPerformanceParity:
    def test_simple_push_perf(self):
        """ORM2 batch push should be within 2x of old ORM for 2000 simple features.

        This is a guardrail, not a precise benchmark — CI variability is high.
        The real benchmark is run manually via benchmark_comprehensive.py.
        """
        N = 2000
        WARMUP = 1

        # Warmup
        for _ in range(WARMUP):
            orm = ORM2.create()
            for i in range(100):
                p = BatchPoint()
                p.x = float(i)
                p.y = float(i)
                p.tag = f"w{i}"
                orm.push(p)
            orm.combine()

        # Timed run
        t0 = time.perf_counter()
        orm = ORM2.create()
        for i in range(N):
            p = BatchPoint()
            p.x = float(i)
            p.y = float(i * 2)
            p.tag = f"p{i}"
            orm.push(p)
        orm.combine()
        elapsed_ms = (time.perf_counter() - t0) * 1000

        # Verify correctness
        assert orm.count(BatchPoint) == N
        r = orm.get(BatchPoint, N - 1, mode='copy')
        assert abs(r.x - float(N - 1)) < 1e-9

        # Performance guardrail: must be under 15ms (old ORM is ~3.25ms)
        # We target ~3ms but allow 5x headroom for CI.
        print(f"\nORM2 batch push {N} features: {elapsed_ms:.1f}ms "
              f"({elapsed_ms/N*1000:.0f}µs/push)")
        assert elapsed_ms < 15, (
            f"ORM2 push {N} features took {elapsed_ms:.1f}ms, "
            "expected < 15ms (old ORM baseline: 3.25ms)"
        )
```

- [ ] **Step 2: Run benchmark test**

Run: `uv run pytest tests/python/test_orm2_batch.py::TestPerformanceParity -v -s`
Expected: PASS with output like "ORM2 batch push 2000 features: ~4ms"

- [ ] **Step 3: Commit**

```bash
git add tests/python/test_orm2_batch.py
git commit -m "test(orm2): add performance parity guardrail test

ORM2 batch push 2000 features must complete under 15ms.
Old ORM baseline: 3.25ms. Target: ~3ms."
```

---

### Task 9: Final integration verification

**Files:** None (verification only)

- [ ] **Step 1: Run full test suite**

Run: `uv run pytest tests/python/ -x -q`
Expected: 215+ tests pass (215 existing + new batch tests)

- [ ] **Step 2: Run interactive benchmark comparison**

Run a quick manual comparison:

```bash
uv run python -c "
import time
from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, STR

@feature
class BenchPt:
    x: F64
    y: F64
    tag: STR

N = 2000
times = []
for trial in range(5):
    t0 = time.perf_counter()
    orm = ORM2.create()
    for i in range(N):
        p = BenchPt(); p.x = float(i); p.y = float(i); p.tag = f'p{i}'
        orm.push(p)
    orm.combine()
    elapsed = (time.perf_counter() - t0) * 1000
    times.append(elapsed)
    print(f'Trial {trial}: {elapsed:.2f}ms')
print(f'Median: {sorted(times)[2]:.2f}ms')
"
```

Expected: Median around 3-5ms

- [ ] **Step 3: Verify shared memory round-trip still works**

Run: `uv run pytest tests/python/test_orm2_share.py -v`
Expected: All share/load tests pass

- [ ] **Step 4: Final commit if any fixups needed**

```bash
git add -A
git status
# If clean, no commit needed. If fixups, commit with descriptive message.
```

---

## Summary

| Task | Description | Key change |
|------|-------------|------------|
| 1 | Extract push_compiler.py | New shared module with compile_push_fn + compile_ref_push_fn; _schema.py imports from it |
| 2 | Enhance LayerSchema | Pre-computed plans + ref field lists |
| 3 | Update push.py | Direct C extension calls |
| 4 | Rewrite push() | Pure Python append with dedup |
| 5 | Rewrite combine() | Topo sort + batch C calls |
| 6 | Batch correctness tests | 1000 objects, dedup, mutation |
| 7 | REF topo sort tests | 3-level chain, shared ref, list[ref] |
| 8 | Performance benchmark | Guardrail: <15ms for 2000 pushes |
| 9 | Integration verification | Full suite + manual benchmark |
