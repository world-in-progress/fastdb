# python/fastdb4py/orm2.py
"""ORM2: decorator-based ORM for @feature classes."""
from __future__ import annotations
from collections import defaultdict, deque
from dataclasses import dataclass
from typing import Any, Dict, Iterator, List, Optional, Type
import struct
import numpy as np

from . import core
from .registry import get_schema, LayerSchema, FieldDef
from .reader import map_feature, copy_feature
from .push_compiler import (
    compile_push_fn, compile_ref_push_fn,
    make_batch_inlined_dispatch,
)


class _ColumnAccessor2:
    """Zero-copy numpy column accessor for ORM2 layers."""
    __slots__ = ('_layer', '_field_map', '_cache')

    def __init__(self, layer, field_map: Dict[str, int]):
        object.__setattr__(self, '_layer', layer)
        object.__setattr__(self, '_field_map', field_map)
        object.__setattr__(self, '_cache', {})

    def __getattr__(self, name: str) -> np.ndarray:
        cache = object.__getattribute__(self, '_cache')
        arr = cache.get(name)
        if arr is not None:
            return arr
        fmap = object.__getattribute__(self, '_field_map')
        fid = fmap.get(name)
        if fid is None:
            raise AttributeError(f'No column "{name}"')
        layer = object.__getattribute__(self, '_layer')
        arr = layer.get_column(fid).as_nparray()
        cache[name] = arr
        return arr


class Table2:
    """Lightweight read-only table with columnar access for ORM2."""
    __slots__ = ('_cls', '_layer', '_count', '_col')

    def __init__(self, cls: Type, layer, count: int, schema: LayerSchema):
        self._cls = cls
        self._layer = layer
        self._count = count
        fmap = {fd.name: fd.field_id for fd in schema.fields}
        self._col = _ColumnAccessor2(layer, fmap)

    @property
    def column(self) -> _ColumnAccessor2:
        return self._col

    def __len__(self) -> int:
        return self._count


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

    queue = deque(cls for cls, deg in in_degree.items() if deg == 0)
    result: List[Type] = []
    while queue:
        cls = queue.popleft()
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
        self._pending_counts: Dict[Type, int] = defaultdict(int)

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
        self._pending_counts[type(obj)] += 1
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
                state._fc[0] = len(objs)

                # Record row indices
                for row_idx, obj in enumerate(objs):
                    obj_to_row[id(obj)] = (state.layer_idx, row_idx)
                state.row_count = len(objs)
            else:
                # REF PATH: pre-resolve all refs into cache copy, then use compiled fn
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
                state._fc[0] = state.row_count

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
        self._pending_counts.clear()
        self._pushed_objs.clear()

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

    def table(self, cls: Type) -> Table2:
        """Get a Table2 with zero-copy numpy column access.

        Usage:
            tbl = orm.table(Point)
            xs = tbl.column.x       # numpy array, zero-copy
            ys = tbl.column.y[:]    # slice also works
        """
        self._check_built()
        state = self._layers.get(cls)
        if state is None:
            raise KeyError(f"No layer for {cls.__name__}")
        layer = self._db.get_layer(state.layer_idx)
        return Table2(cls, layer, state.row_count, state.schema)

    def iter(self, cls: Type, mode: str = 'map') -> Iterator:
        """Iterate all features of a given type."""
        self._check_built()
        state = self._layers.get(cls)
        if state is None:
            raise KeyError(f"No layer for {cls.__name__}")
        for i in range(state.row_count):
            yield self.get(cls, i, mode=mode)

    def count(self, cls: Type) -> int:
        """Return the number of features for cls."""
        if self._built:
            state = self._layers.get(cls)
            return state.row_count if state else 0
        return self._pending_counts.get(cls, 0)

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

    def share(self, name: str):
        """Publish the built database to POSIX shared memory."""
        if not self._built:
            raise RuntimeError("Call combine() before sharing")
        
        import multiprocessing.shared_memory as shm_mod
        data = self._buffer
        seg = shm_mod.SharedMemory(name=name, create=True, size=len(data))
        seg.buf[:len(data)] = data
        seg.close()

    @classmethod
    def load(cls, name: str) -> 'ORM2':
        """Load a database from POSIX shared memory."""
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
        from .registry import _registry
        for i in range(orm._db.get_layer_count()):
            layer = orm._db.get_layer(i)
            layer_name = layer.name()
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