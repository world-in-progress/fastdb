# Unified @feature + ColumnEngine / ObjectEngine Design

## Problem

fastdb Python binding has two independent ORM systems with separate class definition mechanisms:
- **Old ORM** (`ORM`): requires `class Point(Feature)` inheritance, fast columnar batch writes
- **ORM2**: uses `@feature` decorator on plain classes, automatic object-graph handling

This creates user confusion: different class definitions, separate schema systems (`ClassSchema` vs `LayerSchema`), and unclear naming (`ORM` / `ORM2` don't convey intent).

## Approach

Unify class definition under `@feature` decorator with marker-based identity (`__fastdb_feature__`), rename the two engines to `ColumnEngine` (OLAP/batch) and `ObjectEngine` (OLTP/graph), unify the schema system under `LayerSchema`, and establish strict capability boundaries.

---

## 1. Class Definition Layer

### `@feature` decorator

The single entry point for declaring a fastdb-compatible class:

```python
@feature
class Point:
    x: F64
    y: F64
    label: STR
```

The decorator performs three actions:
1. Validate annotations against the strict type policy (existing)
2. Register `LayerSchema` in the global registry (existing)
3. Set `cls.__fastdb_feature__ = True` marker (new)

### Marker-based identity

Replace all `isinstance(obj, Feature)` / `issubclass(cls, BaseFeature)` checks with:

```python
def is_feature(cls_or_obj) -> bool:
    cls = cls_or_obj if isinstance(cls_or_obj, type) else type(cls_or_obj)
    return getattr(cls, '__fastdb_feature__', False)
```

### Inheritance rules

| Rule | Description |
|------|-------------|
| Marker does not auto-inherit | `__fastdb_feature__` is set as the class's own attribute, not inherited via MRO |
| Base classes contribute fields | Any base class can contribute annotations via `get_type_hints()` MRO resolution |
| REF targets must be @feature | A field typed as another class (REF) requires that class to be @feature; validated at decoration time |
| Non-decorated subclasses are plain Python | A subclass of a @feature class is not a feature unless explicitly decorated |

### Marker non-inheritance implementation

To prevent MRO-based inheritance of the marker, `@feature` sets it as the class's **own** `__dict__` entry. The `is_feature()` check must use `cls.__dict__.get('__fastdb_feature__', False)` (not `getattr`) to avoid picking up a parent's marker.

```python
# In decorator:
cls.__dict__  # <- can't write directly; use:
cls.__fastdb_feature__ = True  # setattr on the class itself

# In is_feature():
def is_feature(cls_or_obj) -> bool:
    cls = cls_or_obj if isinstance(cls_or_obj, type) else type(cls_or_obj)
    return cls.__dict__.get('__fastdb_feature__', False)
```

### Removed: Feature base class

`Feature` and `BaseFeature` base classes are removed. Migration path:
- `class Point(Feature): ...` → `@feature class Point: ...`

---

## 2. Dual Engine Architecture

### ColumnEngine (formerly ORM)

Optimized for batch columnar writes. OLAP style.

```python
from fastdb4py import ColumnEngine, feature, Layout, F64

@feature
class Point:
    x: F64
    y: F64

# Pre-allocated + columnar write (fastest path)
eng = ColumnEngine.truncate([Layout(Point, 100_000)])
tbl = eng.table(Point)
tbl.column.x.fill(np.random.rand(100_000))

# Dynamic push (per-object)
eng = ColumnEngine.create()
eng.push(Point(x=1.0, y=2.0))
eng.combine()

# Columnar read
arr = eng.table(Point).column.x  # numpy zero-copy

# Shared memory IPC
eng.share("my_data")
eng2 = ColumnEngine.load("my_data")
```

**Strict no-REF policy**: ColumnEngine rejects @feature classes that contain REF or List[REF] fields. Detection happens at first `push()` call via `get_schema(cls).has_ref_fields`. Raises `TypeError` with message directing to ObjectEngine.

### ObjectEngine (formerly ORM2)

Optimized for object-graph handling. OLTP style.

```python
from fastdb4py import ObjectEngine, feature, F64

@feature
class Node:
    val: F64
    parent: Node  # REF

eng = ObjectEngine.create()
root = Node(val=1.0)
child = Node(val=2.0, parent=root)
eng.push(child)  # auto topo-sort: parent pushed first
eng.combine()

obj = eng.get(Node, 0, mode='copy')
arr = eng.table(Node).column.val  # also supports columnar read
```

### Capability matrix

| Capability | ColumnEngine | ObjectEngine |
|------------|:---:|:---:|
| `truncate()` + `fill()` (pre-allocated tables) | ✅ | ❌ |
| `push()` per-object (scalar/list/bytes) | ✅ | ✅ |
| REF fields | ❌ (rejected) | ✅ (auto topo-sort) |
| Object dedup by `id()` | ❌ | ✅ |
| `get(mode='map'/'copy')` | ❌ | ✅ |
| `table().column` columnar read | ✅ | ✅ |
| Shared memory IPC | ✅ | ✅ |
| `save(path)` / file load | ✅ | ✅ |
| `iter()` / `iter_reuse()` | ✅ | ✅ |
| Deferred mutation (push then modify) | ❌ | ✅ |

---

## 3. Schema System Unification

### Single schema: LayerSchema

`LayerSchema` (from `registry.py`) becomes the only schema class. `ClassSchema` (from `feature/_schema.py`) is removed.

`LayerSchema` already has all required fields. The push compilation attributes (`push_fn`, `batch_fn`, `column_accessor_class`, `field_index_map`) from `ClassSchema` are merged into `LayerSchema`.

### Performance-safe lookup

Add `cls.__dict__` fast-path to `get_schema()` to match `get_class_schema()`'s ~40-50 ns hot path:

```python
_SCHEMA_ATTR = '__fastdb_schema__'

def get_schema(cls: Type) -> LayerSchema:
    schema = cls.__dict__.get(_SCHEMA_ATTR)
    if schema is not None:
        return schema
    # ... WeakKeyDict fallback + build ...
```

Schema lookup only occurs once per class per engine lifetime (at first push). All subsequent pushes use cached `_push_buf` / `_push_dispatch`. Zero impact on push hot path.

---

## 4. Migration Impact

### Files to modify

| File | Change |
|------|--------|
| `decorator.py` | Add `cls.__fastdb_feature__ = True`; add REF-target validation |
| `registry.py` | Add `is_feature()`; add `cls.__dict__` fast-path to `get_schema()`; merge ClassSchema fields into LayerSchema |
| `orm/__init__.py` → `column_engine.py` | Rename `ORM` → `ColumnEngine`; replace `issubclass(x, Feature)` with `is_feature(x)`; add REF rejection; rename `TableDefn` → `Layout` |
| `orm2.py` → `object_engine.py` | Rename `ORM2` → `ObjectEngine` |
| `serializer.py` | Replace ~12 `isinstance(obj, Feature)` with `is_feature(obj)` |
| `codegen/ts_gen.py` | Replace ~8 `issubclass(cls, BaseFeature)` with `is_feature(cls)` |
| `__init__.py` | Update exports |
| All test files | Update class names and imports |

### Files to delete

| File | Reason |
|------|--------|
| `feature/base.py` | `BaseFeature` no longer needed |
| `feature/feature.py` | `Feature` class no longer needed |
| `feature/_schema.py` | `ClassSchema` merged into `LayerSchema` |
| `feature/utils.py` | `parse_defns()` / `get_all_defns()` replaced by `get_schema()` |

### New public API

```python
# Types (unchanged)
from fastdb4py import BOOL, U8, U16, U32, I32, U8N, U16N, F32, F64, STR, WSTR, BYTES

# Class definition
from fastdb4py import feature, is_feature

# Engines
from fastdb4py import ColumnEngine, Layout
from fastdb4py import ObjectEngine

# Serialization
from fastdb4py import FastSerializer
```

---

## 5. Cross-Language Considerations

The marker-based approach maps naturally to each language's idioms:

| Language | Feature declaration | Marker equivalent |
|----------|---|---|
| **Python** | `@feature class Point: ...` | `__fastdb_feature__ = True` |
| **TypeScript** | `class Point extends Feature { static schema = defineSchema({...}) }` | `static schema` presence |
| **Fortran** (future) | `TYPE :: Point` with naming convention | Module-level registration |
| **Go** (future) | `type Point struct { X float64 \`fastdb:"f64"\` }` | Struct tag `fastdb:` |

Python-side changes have zero impact on TS binding or C++ core. Each language binding maintains its own idiom.

---

## 6. Testing Strategy

- Convert all existing ORM tests to use `@feature` + `ColumnEngine`
- Convert all existing ORM2 tests to use `ObjectEngine`
- Add specific tests for: marker inheritance rules, REF rejection in ColumnEngine, `is_feature()` edge cases
- Re-run Kostya benchmark to verify zero performance regression
- Verify FastSerializer and Codegen work with `@feature` classes (no BaseFeature inheritance)
