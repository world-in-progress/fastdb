# python/fastdb4py/object_engine.py
"""ObjectEngine: OLTP/graph workloads with REF field support."""
from __future__ import annotations
from collections import defaultdict, deque
from dataclasses import dataclass
from typing import Any, Dict, Iterator, List, Optional, Type
import struct
import numpy as np

from . import core
from .registry import (
    get_schema,
    is_feature,
    LayerSchema,
    FieldDef,
    lookup_class,
    non_native_list_storage_diagnostics,
    raw_payload_storage_diagnostics,
)
from .layout import Layout
from .orm.table import Table
from .reader import map_feature, copy_feature
from .push_compiler import (
    compile_push_fn, compile_ref_push_fn,
    make_batch_inlined_dispatch,
)
from .push import normalize_bool_cache


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
            "ObjectEngine does not support circular class references."
        )
    return result


def _reject_non_native_lists(schema: LayerSchema, cls_name: str) -> None:
    diagnostics = non_native_list_storage_diagnostics(schema)
    if diagnostics:
        raise TypeError(
            f"ObjectEngine cannot store non-native list fields for "
            f"{cls_name}: {'; '.join(diagnostics)}"
        )


def _reject_raw_payload_collisions(schema: LayerSchema, cls_name: str) -> None:
    diagnostics = raw_payload_storage_diagnostics(schema)
    if diagnostics:
        raise TypeError(
            f"ObjectEngine cannot store raw payload fields for "
            f"{cls_name}: {'; '.join(diagnostics)}"
        )


def _reject_unsupported_schema(schema: LayerSchema, cls_name: str) -> None:
    _reject_raw_payload_collisions(schema, cls_name)
    _reject_non_native_lists(schema, cls_name)


class ObjectEngine:
    """Decorator-based ORM for @feature classes with REF support.

    Usage:
        engine = ObjectEngine.create()
        engine.push(my_point)
        engine.combine()
        result = engine.get(MyPoint, 0, mode='copy')
    """

    def __init__(self):
        self._db_build: Optional[core.WxDatabaseBuild] = None
        self._db: Optional[core.WxDatabase] = None
        self._buffer: Optional[bytes] = None
        self._layers: Dict[Type, LayerState] = {}
        self._layer_order: List[Type] = []
        self._built = False
        # Deferred batch push state
        self._pending: List[Any] = []
        self._pushed_ids: Dict[int, bool] = {}
        self._pushed_objs: List[Any] = []
        self._pending_counts: Dict[Type, int] = defaultdict(int)

    @classmethod
    def create(cls) -> 'ObjectEngine':
        """Start a new build session."""
        orm = cls()
        orm._db_build = core.WxDatabaseBuild()
        orm._db_build.begin("")
        return orm

    @classmethod
    def truncate(cls, layouts) -> 'ObjectEngine':
        """Pre-allocate fixed-size tables. REF fields initialized to null."""
        engine = cls()
        engine._db_build = core.WxDatabaseBuild()
        engine._db_build.begin("")
        for layout in layouts:
            if not is_feature(layout.feature_type):
                raise TypeError(f"{layout.feature_type!r} not a @feature class")
            schema = get_schema(layout.feature_type)
            _reject_unsupported_schema(schema, layout.feature_type.__name__)
            state = engine._ensure_layer(layout.feature_type)
            engine._db_build.truncate(schema.layer_name, layout.capacity)
            state._fc[0] = layout.capacity
            state.row_count = layout.capacity
        # Build immediately
        mem = core.WxMemoryStream()
        engine._db_build.post(mem)
        buf = mem.data().as_array(np.uint8).tobytes()
        engine._db = core.WxDatabase.load_xbuffer(buf)
        engine._db._buffer = buf
        engine._buffer = buf
        engine._built = True
        engine._db_build = None
        return engine

    def push(self, obj: Any) -> None:
        """Queue a @feature object for batch serialization at combine() time."""
        obj_id = id(obj)
        if obj_id in self._pushed_ids:
            return
        schema = get_schema(type(obj))
        _reject_unsupported_schema(schema, type(obj).__name__)
        self._pushed_ids[obj_id] = True
        self._pending.append(obj)
        self._pushed_objs.append(obj)
        self._pending_counts[type(obj)] += 1
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
            raise RuntimeError("ObjectEngine already combined")
        if self._db_build is None:
            raise RuntimeError("ObjectEngine not in build mode")

        # 1. Group pending objects by class
        groups: Dict[Type, List[Any]] = defaultdict(list)
        for obj in self._pending:
            groups[type(obj)].append(obj)

        # 2. Topological sort classes by REF dependencies
        sorted_classes = _topo_sort_classes(groups)

        # 3. Push each class in topo order
        obj_to_row: Dict[int, tuple] = {}
        for cls in sorted_classes:
            objs = groups[cls]
            schema = get_schema(cls)
            state = self._ensure_layer(cls)
            layer_build = state.build

            if not schema.has_ref_fields:
                self._push_no_refs(objs, schema, state, layer_build, obj_to_row)
            else:
                self._push_with_refs(objs, schema, state, layer_build, obj_to_row)

        # 4. Finalize into read-only database
        mem = core.WxMemoryStream()
        self._db_build.post(mem)
        buf = mem.data().as_array(np.uint8).tobytes()
        self._db = core.WxDatabase.load_xbuffer(buf)
        self._db._buffer = buf
        self._buffer = buf
        self._built = True
        self._db_build = None
        self._pending.clear()
        self._pushed_ids.clear()
        self._pending_counts.clear()
        self._pushed_objs.clear()

    def _push_no_refs(self, objs, schema, state, layer_build, obj_to_row):
        """Fast path: batch push for classes without REF fields."""
        batch_fn = make_batch_inlined_dispatch(
            schema.numeric_plan, schema.str_plan,
            schema.bytes_plan, schema.list_plan, state,
            schema.pfd_num_names, schema.pfd_num_ids,
            schema.pfd_str_names, schema.pfd_str_ids,
        )
        if batch_fn is not None:
            dicts = [normalize_bool_cache(obj.__dict__, schema) for obj in objs]
            for i in range(0, len(dicts), 1024):
                batch_fn(dicts[i:i + 1024])
        else:
            push_fn = compile_push_fn(
                schema.numeric_plan, schema.str_plan,
                schema.bytes_plan, schema.list_plan,
            )
            for obj in objs:
                push_fn(normalize_bool_cache(obj.__dict__, schema), layer_build)
        state._fc[0] = len(objs)
        for row_idx, obj in enumerate(objs):
            obj_to_row[id(obj)] = (state.layer_idx, row_idx)
        state.row_count = len(objs)

    def _push_with_refs(self, objs, schema, state, layer_build, obj_to_row):
        """REF path: resolve refs, then push with compiled fn."""
        ref_push_fn = compile_ref_push_fn(
            schema.numeric_plan, schema.str_plan,
            schema.bytes_plan, schema.list_plan,
            [(fd.field_id, fd.name) for fd in schema.ref_fields],
            [(fd.field_id, fd.name) for fd in schema.list_ref_fields],
        )
        for obj in objs:
            row_idx = state.row_count
            cache = obj.__dict__.copy()
            for fd in schema.ref_fields:
                ref_obj = cache.get(fd.name)
                if ref_obj is not None:
                    loc = obj_to_row.get(id(ref_obj))
                    if loc is not None:
                        li, ri = loc
                        cache[fd.name] = core.WxFeatureRef.make_ref(li, ri)
                    else:
                        cache[fd.name] = 0
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
            ref_push_fn(normalize_bool_cache(cache, schema), layer_build)
            obj_to_row[id(obj)] = (state.layer_idx, row_idx)
            state.row_count += 1
        state._fc[0] = state.row_count

    def get(self, cls: Type, idx: int, mode: str = 'map') -> Any:
        """Read back a single feature."""
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

    def table(self, cls: Type) -> Table:
        """Get a Table with zero-copy numpy column access."""
        self._check_built()
        state = self._layers.get(cls)
        if state is None:
            raise KeyError(f"No layer for {cls.__name__}")
        layer = self._db.get_layer(state.layer_idx)
        return Table.map_from(cls, layer, self._db)

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
        _reject_unsupported_schema(schema, cls.__name__)
        t = self._db_build.create_layer_begin(schema.layer_name)
        if schema.bytes_plan:
            t.set_geometry_type(core.gtAny, core.cfTx32, aabboxEnabled=False)
        else:
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
    def load(cls, name: str) -> 'ObjectEngine':
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

        for i in range(orm._db.get_layer_count()):
            layer = orm._db.get_layer(i)
            layer_name = layer.name()
            registered_cls = lookup_class(layer_name)
            if registered_cls is not None:
                schema = get_schema(registered_cls)
                state = LayerState(
                    cls=registered_cls,
                    schema=schema,
                    layer_idx=i,
                    row_count=layer.get_feature_count(),
                )
                orm._layers[registered_cls] = state
                orm._layer_order.append(registered_cls)
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
