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

The decorator performs five actions:
1. Validate annotations against the strict type policy (existing), with forward-ref tolerance (see below)
2. Set `cls.__fastdb_feature__ = True` marker (new)
3. Inject `__init__(**kwargs)` if the class does not define its own (new)
4. Register class in the global class registry (`_class_registry[cls.__name__] = cls`) — enables `load()` to reconstruct layers by name
5. Schema registration is **lazy** — `LayerSchema` is built on first `get_schema(cls)` call, not at decoration time

**`__slots__` restriction**: Classes with `__slots__` that do NOT include `'__dict__'` are rejected by the decorator. The constructor, `copy_feature()`, and serializer all rely on `obj.__dict__` for field storage. Raises `TypeError` with a clear message.

### Constructor generation

`@feature` injects a `**kwargs` constructor matching old `Feature.__init__` behavior:

```python
def feature(cls):
    _validate_annotations(cls)
    # Reject __slots__ without __dict__
    if '__slots__' in cls.__dict__ and '__dict__' not in cls.__dict__['__slots__']:
        raise TypeError(
            f"@feature class '{cls.__name__}' defines __slots__ without '__dict__'. "
            "fastdb requires __dict__ for field storage."
        )
    cls.__fastdb_feature__ = True
    # Register class name for load() reconstruction (NOT schema building)
    _class_registry[cls.__name__] = cls
    if '__init__' not in cls.__dict__:
        def __init__(self, **kwargs):
            if kwargs:
                self.__dict__.update(kwargs)
        cls.__init__ = __init__
    return cls
```

- If the user defines their own `__init__`, it is preserved.
- `dict.update(kwargs)` is identical to old `Feature.__init__` — zero performance regression.
- The serializer uses `cls.__new__(cls)` + direct `__dict__` population to bypass `__init__` entirely.

### Lazy schema registration

Schema building is deferred to first `get_schema(cls)` call (not decoration time). This ensures `get_type_hints()` can resolve forward references and self-references that may not be available at class definition time. The `get_schema()` function already supports on-demand building and caching.

**Class registry vs schema cache**: The decorator registers the class in a separate name→class registry (`_class_registry: Dict[str, Type]`) at decoration time. This is NOT the schema cache — it's a lightweight name lookup used by `load()` to reconstruct layer types from layer names. Schema building (the expensive part involving `get_type_hints()`) remains lazy.

```python
# In registry.py
_class_registry: Dict[str, Type] = {}  # name → cls (populated by @feature)

def get_class_by_name(name: str) -> Optional[Type]:
    return _class_registry.get(name)
```

### Forward-reference tolerant validation

Current `_validate_annotations()` calls `_check_hint()` which rejects unresolvable types. But for forward references (e.g. `parent: 'Node'` or `parent: Node` before Node is defined), the hint may be a string or `ForwardRef`, not a resolvable type.

**Fix**: `_check_hint()` skips validation for string annotations and `ForwardRef` objects — these are validated later when `get_schema()` calls `get_type_hints()` which resolves them. If resolution fails at schema-build time, the field is treated as `OriginFieldType.unknown` (existing fallback behavior).

### Marker-based identity

Replace all `isinstance(obj, Feature)` / `issubclass(cls, BaseFeature)` checks with:

```python
def is_feature(cls_or_obj) -> bool:
    cls = cls_or_obj if isinstance(cls_or_obj, type) else type(cls_or_obj)
    return cls.__dict__.get('__fastdb_feature__', False)
```

**Important**: Only `cls.__dict__.get()` is used — never `getattr()`. This prevents MRO-based inheritance of the marker. Non-decorated subclasses will not be recognized as features.

### Inheritance rules

| Rule | Description |
|------|-------------|
| Marker does not auto-inherit | `__fastdb_feature__` is set as the class's own attribute, not inherited via MRO |
| Base classes contribute fields | Any base class can contribute annotations via `get_type_hints()` MRO resolution |
| REF targets must be @feature | A field typed as another class (REF) requires that class to be @feature; validated at schema resolution time (lazy, not decoration time) |
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

**Strict no-REF policy**: ColumnEngine rejects @feature classes that contain REF or List[REF] fields at ALL entry points:
- `Layout()` constructor — fail-fast for truncate path
- `ColumnEngine.push()` / `push_many()` — fail-fast for dynamic path (first push of a REF-containing type raises `TypeError`)
- `ColumnEngine.table()` — fail-fast for read access

Raises `TypeError` with message directing to ObjectEngine.

```python
class Layout:
    def __init__(self, feature_type, capacity):
        if not is_feature(feature_type):
            raise TypeError(...)
        schema = get_schema(feature_type)
        if schema.has_ref_fields:
            raise TypeError(
                f"'{feature_type.__name__}' contains REF fields. "
                "Use ObjectEngine for ref-containing types."
            )
```

**Breaking change: REF detection tightening**: Currently, `_resolve_field_type()` in `registry.py` treats almost any non-builtin class as REF (including non-@feature classes with `__annotations__`). In the new design, REF targets are validated at schema resolution time: they must be `@feature`-decorated classes. This is a deliberate tightening — annotating a field with a plain (non-@feature) class will raise an error instead of silently treating it as REF.

### ObjectEngine (formerly ORM2)

Optimized for object-graph handling. OLTP style.

```python
from fastdb4py import ObjectEngine, feature, F64

@feature
class Node:
    val: F64
    parent: Node  # REF

# Dynamic append with auto topo-sort
eng = ObjectEngine.create()
root = Node(val=1.0)
child = Node(val=2.0, parent=root)
eng.push(child)  # auto topo-sort: parent pushed first
eng.combine()

# Pre-allocated tables (supports REF fields — unlike ColumnEngine)
eng = ObjectEngine.truncate([Layout(Node, 1000)])
tbl = eng.table(Node)
tbl.column.val.fill(np.random.rand(1000))  # scalar fill OK
# REF fields populated per-object (not bulk fill)

obj = eng.get(Node, 0, mode='copy')
arr = eng.table(Node).column.val  # also supports columnar read
```

**ObjectEngine.truncate()**: Supports preallocated tables including those with REF fields (the capability removed from ColumnEngine). Scalar column `fill()` works. REF fields in preallocated tables are populated per-object via individual writes, not bulk fill (dependency ordering makes bulk REF fill high-risk).

**ObjectEngine truncate design**: Unlike ColumnEngine's truncate (which uses the existing ORM truncate → `_combine()` flow), ObjectEngine's truncate creates a fixed-scale database with pre-allocated rows, then exposes per-object write access:

```python
@classmethod
def truncate(cls, layouts: List[Layout]) -> 'ObjectEngine':
    """Pre-allocate fixed-size tables (REF fields allowed)."""
    orm = cls()
    orm._db_build = core.WxDatabaseBuild()
    orm._db_build.begin("")
    for layout in layouts:
        schema = get_schema(layout.feature_type)
        state = orm._ensure_layer(layout.feature_type)
        # Uses C++ truncate (same as ColumnEngine)
        orm._db_build.truncate(schema.layer_name, layout.capacity)
    # Build immediately into read-only database
    orm._finalize_build()
    return orm
```

The key difference from ColumnEngine.truncate: ObjectEngine does NOT reject REF fields, and provides `get(mode='copy')` for REF readback.

**Graph semantics limit**: ObjectEngine inherits ORM2's **class-level circular REF dependency rejection**. `_topo_sort_classes()` uses Kahn's algorithm and raises `RuntimeError` for cycles. **Instance-level** cycles (e.g. `a.parent = b; b.parent = a` where both are same type) are supported — they don't affect class-level ordering.

### Capability matrix

| Capability | ColumnEngine | ObjectEngine |
|------------|:---:|:---:|
| `truncate()` + `fill()` (pre-allocated tables, scalar only) | ✅ | ✅ |
| `truncate()` with REF fields | ❌ (rejected) | ✅ |
| `push()` / `push_many()` per-object | ✅ | ✅ |
| REF fields | ❌ (rejected at all entry points) | ✅ (auto topo-sort) |
| Object dedup by `id()` | ❌ | ✅ |
| `get(cls, idx, mode='copy')` | ❌ | ✅ (copy with REF resolution) |
| `get(cls, idx, mode='map')` | ❌ | ✅ (read-only proxy, REFs return None) |
| `table().column` columnar read/write | ✅ | ✅ |
| `table[i]` / `iter(table)` | ✅ (detached copy) | ✅ (detached copy) |
| `iter_reuse()` | ✅ (read-only proxy) | ✅ (read-only proxy) |
| REF readback (resolve refs to objects) | ❌ | ✅ (copy mode only, with cycle detection) |
| Shared memory IPC (`share`/`load`/`unlink`) | ✅ | ✅ |
| `save(path)` / file load | ✅ | ❌ (future — add to ObjectEngine) |
| Named features (`get(Type, name)`) | ✅ (kept from ORM) | ❌ (not supported) |
| `push_many()` bulk push | ✅ | ❌ (use push() — dedup requires per-object) |
| Deferred mutation (push then modify) | ❌ | ✅ |

**REF readback in map mode**: `MappedFeature` returns `None` for REF fields (same as `_read_field()` behavior). This is by design — mapped proxies are for fast scalar reads. Use `get(mode='copy')` for full REF resolution.

---

## 3. Read Mechanism (replaces Feature.map_from)

### Problem

Old `Feature.map_from()` created a DB-mapped wrapper with `__getattr__` reading from C++ and `_db_setattr` writing to C++. Removing `Feature` removes this read/write path.

### Solution: reader.py + copy semantics

`reader.py` already provides two read modes that work with `@feature` classes:
- `copy_feature(cls, layer, idx)` → detached instance with all values in `__dict__`
- `map_feature(cls, layer, idx)` → `MappedFeature` read-only proxy

### Detached copy contract

`copy_feature()` returns a fully detached Python object with NO dependency on C++ memory. This means:
- Scalar fields: copied by value (Python ints/floats are immutable, no issue)
- String fields: Python strings from `get_field_as_string()` are already copies
- **Bytes fields**: Currently `get_geometry_like_chunk()` returns a C++ memory view — must call `bytes()` to copy. Fix: `_read_field()` for bytes must return `bytes(feat.get_geometry_like_chunk())` to enforce detach semantics.
- List fields: already call `.copy()` on numpy arrays (line 46-47 of reader.py)

### Table read API changes

| API | Old behavior | New behavior |
|-----|-------------|-------------|
| `table[i]` | Mapped `Feature` (read + write to C++) | Detached copy (all values in `__dict__`) |
| `iter(table)` | Mapped `Feature` per iteration | Detached copy per iteration |
| `iter_reuse()` | Reused mapped `Feature` (read + write) | Reused `MappedFeature` proxy (read-only) |
| `table.column.x` | numpy array (zero-copy) | unchanged |

**Rationale**: Per-feature DB writes should use column access (`tbl.column.x[:] = values`) which is faster and doesn't require DB-mapped objects. Detached copies are simpler, safer (no stale C++ pointer risk), and sufficient for the read-then-process pattern.

**Removed APIs**: `read_all_scalars()` and `write_all_scalars()` (Feature methods that required DB-mapped instances) are removed. Equivalent functionality via column access: `tbl.column.x[i]` for reads, `tbl.column.x[i] = v` for writes.

### iter_reuse() implementation

```python
def iter_reuse(self):
    schema = get_schema(self._feature_type)
    wrapper = MappedFeature.__new__(MappedFeature)
    object.__setattr__(wrapper, '_cls', self._feature_type)
    object.__setattr__(wrapper, '_schema', schema)
    count = self._origin.get_feature_count()
    for i in range(count):
        feat = self._origin.tryGetFeature(i)
        object.__setattr__(wrapper, '_feat', feat)
        yield wrapper
```

### ObjectEngine REF readback

`reader._read_field()` returns `None` for REF fields (correct for ColumnEngine). ObjectEngine provides its own `get(cls, idx, mode='copy')` that recursively resolves refs with cycle detection:

```python
def _read_with_refs(cls, layer, idx, db, seen=None):
    if seen is None:
        seen = {}
    key = (layer_idx, idx)
    if key in seen:
        return seen[key]
    
    obj = cls.__new__(cls)
    seen[key] = obj  # cache before recursive reads (breaks cycles)
    
    schema = get_schema(cls)
    feat = layer.tryGetFeature(idx)
    for fd in schema.fields:
        if fd.field_type == OriginFieldType.ref:
            ref = feat.get_field_as_ref(fd.field_id)
            ref_feat = db.tryGetFeature(ref)
            ref_cls = fd.ref_target or cls
            obj.__dict__[fd.name] = _read_with_refs(ref_cls, ..., db, seen)
        else:
            obj.__dict__[fd.name] = _read_field(feat, fd)
    return obj
```

The `seen` dict keyed by `(layer_idx, feature_idx)` prevents infinite recursion on cyclic references.

### FeatureRefList

`FeatureRefList` is only used for list-of-ref reads from DB-mapped Features. In the new design:
- ColumnEngine: no REFs → no FeatureRefList needed
- ObjectEngine: `_read_with_refs` handles `List[REF]` fields by resolving each ref in the list, with the same cycle-detection cache

---

## 4. Serializer Adaptation

### Problem

`serializer.py` has deep coupling to `Feature`:
1. Imports `Feature` and its slot descriptors (`_feat_origin_s`, `_feat_db_s`)
2. Sets `_origin`/`_db` on deserialized objects (lines 461-462)
3. Detaches `_origin`/`_db` after deserialization (lines 321-322)
4. Uses `isinstance(obj, Feature)` / `issubclass(cls, Feature)` for type checks (~12 sites)
5. Has its own schema adapter cache (`_get_class_schema`, line 917+)
6. Uses `issubclass(base, Feature)` in type discovery (`_discover_types_impl`)
7. Uses `Feature` as **fallback/sentinel type** in at least 6 places:
   - `ref_type = hints.get(fn, Feature)` — fallback when hint missing (line 557)
   - `isinstance(first, Feature)` — list element type detection (line 775)
   - `issubclass(inner, Feature)` — list element type check (line 787)
   - Multiple uses in `unpack_list` for ref resolution and fallback type (lines 825-855)

### Solution

**Object creation**: Use `cls.__new__(cls)` + direct `__dict__` population. The serializer already reads ALL field values directly from C++ getters into `obj.__dict__` (lines 545-572) — it never goes through `Feature.__getattr__`. The `_origin`/`_db` slots were vestigial safety nets; removing them is safe.

```python
# Old:
obj = cls()
_feat_origin_s.__set__(obj, feature_data)
_feat_db_s.__set__(obj, self.db)

# New:
obj = cls.__new__(cls)
# Read fields directly into obj.__dict__ (already done)
# No _origin/_db needed — object is fully populated
```

**Detach loop removal**: No `_origin`/`_db` to detach → the detach loop in `loads_shm()` is removed entirely.

**Feature sentinel replacement**: All uses of `Feature` as a type sentinel are replaced with `is_feature()`:

```python
# Old: ref_type = hints.get(fn, Feature)
# New: ref_type = hints.get(fn, None)  # None means "unknown type"

# Old: isinstance(first, Feature)
# New: is_feature(first)

# Old: issubclass(inner, Feature)
# New: is_feature(inner)
```

For the fallback type pattern (line 557), `None` replaces `Feature` as the sentinel. The downstream code that checks `issubclass(ref_type, Feature)` becomes `ref_type is not None and is_feature(ref_type)`.

**Type identity**: Replace all `isinstance(obj, Feature)` with `is_feature(obj)`, and all `issubclass(cls, Feature)` with `is_feature(cls)`.

**Schema adapter**: Keep `_get_class_schema()` as an internal adapter, but source from `get_schema()` (LayerSchema) instead of ClassSchema:

```python
def _get_class_schema(cls):
    base = get_schema(cls)  # LayerSchema (unified)
    
    # Compute serializer-specific derived data
    hints = base.hints
    defns = base.ordered_defns
    # ... db_field_index_by_schema, has_blob_fields, ref_traversal_fields ...
    
    return {"hints": hints, "defns": defns, ...}
```

**Type discovery**: Replace `issubclass(base, Feature)` with `is_feature(base)` in `_discover_types_impl()`.

---

## 5. Schema System Unification

### Single schema: LayerSchema

`LayerSchema` (from `registry.py`) becomes the only schema class. `ClassSchema` (from `feature/_schema.py`) is removed.

### Merged attributes

`LayerSchema` already has push-related fields. The following attributes from `ClassSchema` are merged in, all computed once at build time from the `fields` list:

| Attribute | Type | Source | Purpose |
|-----------|------|--------|---------|
| `hints` | `Dict[str, type]` | `get_type_hints(cls)` | Raw type hints for all fields (used by serializer, REF resolution) |
| `ordered_defns` | `List[Tuple[str, OriginFieldType]]` | `fields` | Ordered (name, field_type) pairs (used by serializer) |
| `origin_hints` | `Dict[str, Tuple[OriginFieldType, int]]` | `fields` | name→(ft, field_id) mapping (used by MappedFeature read dispatch) |
| `list_element_types` | `Dict[str, OriginFieldType]` | `fields` | name→elem_type for list fields (used by reader, serializer) |
| `field_index_map` | `Dict[str, int]` | `fields` | name→field_id (used by ColumnAccessor) |
| `column_accessor_class` | `Optional[type]` | mutable | Cached accessor class (populated at first column access) |
| `scalar_field_ids_np` | `np.ndarray` | `fields` | Pre-computed scalar field IDs for batch read/write |

All are trivially derivable from `self.fields`:
```python
self.ordered_defns = [(fd.name, fd.field_type) for fd in fields]
self.origin_hints = {fd.name: (fd.field_type, fd.field_id) for fd in fields}
self.field_index_map = {fd.name: fd.field_id for fd in fields}
self.list_element_types = {fd.name: fd.list_elem_type for fd in fields if fd.list_elem_type}
self.scalar_field_ids_np = np.array([fd.field_id for fd in fields if fd.field_type in _NUMERIC_FT], dtype=np.uint32)
self.column_accessor_class = None
```

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

### Push cache location

**Push caches are per-engine-instance, NOT per-schema.** The compiled push functions (`push_fn`, `batch_fn` from `push_compiler.py`) are **table-bound** — they bake in `t_obj._origin` (the specific C++ layer object) via `functools.partial`. The same `@feature` class may exist in multiple engine instances, each with different underlying C++ layers.

Current locations (unchanged by this refactoring):
- **ColumnEngine**: stores in `self._push_buf`, `self._push_batch_fn`, `self._push_dispatch` (per-instance dicts keyed by class)
- **ObjectEngine**: creates fresh push functions per `combine()` call (acceptable since combine is amortized)

`LayerSchema` stores type-level metadata only (field definitions, type hints, field ordering). It NEVER stores engine-instance-specific caches.

---

## 6. Migration Impact

### Compatibility decision: hard v2.0 break

This is a deliberate major-version break. No shims, no aliases, no `Feature` base class retention.

Migration checklist for users:
- `class Point(Feature): ...` → `@feature class Point: ...`
- `from fastdb4py import ORM` → `from fastdb4py import ColumnEngine`
- `from fastdb4py import ORM2` → `from fastdb4py import ObjectEngine`
- `from fastdb4py import TableDefn` → `from fastdb4py import Layout`
- `isinstance(obj, Feature)` → `is_feature(obj)`

### Complete API diff

Every public API with keep/remove/migrate status:

| API | Status | Notes |
|-----|--------|-------|
| `Feature` base class | **REMOVED** | Use `@feature` decorator |
| `BaseFeature` | **REMOVED** | Use `is_feature()` |
| `Feature.__getattr__` / `__setattr__` dispatch | **REMOVED** | Replaced by column access + copy/map reads |
| `Feature.map_from(layer, idx)` | **REMOVED** | Use `map_feature(cls, layer, idx)` or `table[i]` |
| `Feature.fixed` property | **REMOVED** | No two-mode objects; all instances are plain Python |
| `read_all_scalars()` / `write_all_scalars()` | **REMOVED** | Use column access: `tbl.column.x[i]` |
| `FeatureRefList` | **REMOVED** | ObjectEngine handles list-ref reads internally |
| `parse_defns()` / `get_all_defns()` | **REMOVED** | Use `get_schema(cls).fields` |
| `ClassSchema` | **REMOVED** | Merged into `LayerSchema` |
| `_FeatureDBMixin` | **REMOVED** | DB-mapped mixin no longer needed |
| `ORM` | **RENAMED** → `ColumnEngine` | Same functionality, no REF support |
| `ORM2` | **RENAMED** → `ObjectEngine` | Same functionality + truncate() |
| `TableDefn` | **RENAMED** → `Layout` | Same semantics |
| `ORM.push()` | **KEPT** as `ColumnEngine.push()` | Rejects REF-containing types |
| `ORM.push_many()` | **KEPT** as `ColumnEngine.push_many()` | Bulk push, rejects REFs |
| `ORM.truncate()` | **KEPT** as `ColumnEngine.truncate()` | Takes `List[Layout]` |
| `ORM.share/load/unlink` | **KEPT** | Shared memory IPC |
| `ORM.save(path)` | **KEPT** as `ColumnEngine.save(path)` | File persistence |
| `ORM.get(cls, name)` | **KEPT** as `ColumnEngine.get(cls, name)` | Named feature lookup |
| `Table.column` / `ColumnAccessor` | **KEPT** | Zero-copy NumPy columns |
| `Table.push2()` context manager | **KEPT** | Low-level push API |
| `Table[i]` / `iter(table)` | **CHANGED** | Returns detached copy (was mapped) |
| `Table.iter_reuse()` | **CHANGED** | Returns `MappedFeature` proxy (was mutable mapped) |
| `TableBuilder` / `orm[Point]["name"]` | **KEPT** as `engine[Point]["name"]` | Syntax preserved |
| `@feature` decorator | **KEPT** | Now with `__init__` injection + class registry |
| `is_feature()` | **NEW** | Replaces `isinstance(x, Feature)` |
| `Layout()` | **NEW** | Replaces `TableDefn`; validates no-REF for ColumnEngine |
| `get_schema()` | **KEPT** | Unified schema source |
| `MappedFeature` | **KEPT** | Read-only proxy for iter_reuse |
| `copy_feature()` | **KEPT** | Detached copy for table[i] |
| `FastSerializer` | **KEPT** | Adapted for `@feature` classes |

### Breaking change: REF detection tightening

Current behavior: `_resolve_field_type()` in `registry.py` treats any class with `__annotations__` and at least one known-type field as a REF target — even non-@feature plain classes.

New behavior: REF targets must be `@feature`-decorated classes. `is_feature(hint)` must return `True` for the annotation to be treated as REF. Non-@feature class annotations raise `TypeError` at schema build time.

This is a deliberate tightening for type safety. Users who currently annotate fields with plain classes will need to add `@feature` to those classes.

### Files to modify

| File | Change |
|------|--------|
| `decorator.py` | Add `__fastdb_feature__` marker; add `__init__` injection; remove eager `get_schema()` |
| `registry.py` | Add `is_feature()`; add `cls.__dict__` fast-path to `get_schema()`; merge ClassSchema attributes into LayerSchema |
| `reader.py` | Expand `_read_field` for ObjectEngine REF resolution (keep current for ColumnEngine) |
| `orm/__init__.py` → `column_engine.py` | Rename `ORM` → `ColumnEngine`; replace `issubclass(x, Feature)` with `is_feature(x)`; add REF rejection at `Layout`; change iter to use `copy_feature`/`MappedFeature` |
| `orm/table.py` | Replace `Feature.map_from()` with `copy_feature()`/`MappedFeature`; remove `_FeatureDBMixin`/`_get_db_cls` dependency |
| `orm2.py` → `object_engine.py` | Rename `ORM2` → `ObjectEngine`; add `truncate()`/`fill()` support; add REF readback with cycle detection |
| `serializer.py` | Replace `Feature` imports with `is_feature`; use `cls.__new__(cls)` instead of `cls()`; remove `_origin`/`_db` slot access; source schema from `get_schema()` |
| `codegen/ts_gen.py` | Replace ~8 `issubclass(cls, BaseFeature)` with `is_feature(cls)` |
| `__init__.py` | Update exports |
| All test files | Update class names and imports |

### Files to delete

| File | Reason |
|------|--------|
| `feature/base.py` | `BaseFeature` no longer needed |
| `feature/feature.py` | `Feature` class no longer needed |
| `feature/_schema.py` | `ClassSchema` merged into `LayerSchema` |
| `feature/ref_list.py` | `FeatureRefList` removed; ObjectEngine handles list-ref reads internally |
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

## 7. Cross-Language Considerations

The marker-based approach maps naturally to each language's idioms:

| Language | Feature declaration | Marker equivalent |
|----------|---|---|
| **Python** | `@feature class Point: ...` | `__fastdb_feature__ = True` |
| **TypeScript** | `class Point extends Feature { static schema = defineSchema({...}) }` | `static schema` presence |
| **Fortran** (future) | `TYPE :: Point` with naming convention | Module-level registration |
| **Go** (future) | `type Point struct { X float64 \`fastdb:"f64"\` }` | Struct tag `fastdb:` |

Python-side changes have zero impact on TS binding or C++ core. Each language binding maintains its own idiom.

---

## 8. Testing Strategy

### Converted tests
- Convert all existing ORM tests to use `@feature` + `ColumnEngine`
- Convert all existing ORM2 tests to use `ObjectEngine`

### New test categories

**Decorator & class identity:**
- Marker inheritance rules (non-inheritance, `is_feature` edge cases with subclasses, non-decorated classes)
- `@feature` constructor generation (kwargs, custom `__init__` preserved, empty kwargs)
- `__slots__` rejection (with and without `__dict__` in slots)
- Forward-reference and self-reference resolution (lazy schema build, circular class refs)
- Class registry population at decoration time (`_class_registry[cls.__name__]`)

**ColumnEngine:**
- REF rejection at `Layout()`, `push()`, `push_many()`, `table()` — all entry points
- `truncate()` + `fill()` + columnar read round-trip
- `push_many()` bulk push correctness
- Named features (`get(Type, name)`)
- `save(path)` / file load round-trip
- Shared memory IPC

**ObjectEngine:**
- `truncate()` with REF-containing types + scalar fill + per-object REF write
- `push()` with auto topo-sort (class-level deps)
- REF readback with `get(mode='copy')` — including cycle detection
- `get(mode='map')` returns `None` for REF fields
- Instance-level cycles (A.parent = B, B.parent = A, same type)
- Class-level circular REF deps → `RuntimeError`
- Shared memory IPC (share/load)

**Read mechanism:**
- `table[i]` returns detached copy (no C++ memory dependency)
- `iter(table)` returns detached copies
- `iter_reuse()` returns `MappedFeature` proxy (read-only)
- `copy_feature()` bytes field produces `bytes()` copy (not C++ view)
- `MappedFeature` raises `AttributeError` on write attempt

**Serializer:**
- `FastSerializer.dumps/loads` with `@feature` classes (no Feature inheritance)
- `is_feature()` replaces `isinstance(obj, Feature)` in all type detection paths
- `cls.__new__(cls)` object creation path (no `__init__` side effects)
- Nested/cyclic object graphs with `@feature` classes
- Buffer-layer protocol compatibility (unchanged)
- Cross-language interop (Python ↔ TypeScript)

**Schema system:**
- `get_schema()` produces `LayerSchema` with all merged attributes
- `cls.__dict__` fast-path works correctly
- Schema computed lazily (not at decoration time)
- Same class schema from both engines

**Performance:**
- Re-run Kostya benchmark to verify zero performance regression
- Column access latency unchanged
- Push throughput unchanged
