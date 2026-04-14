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
from .type import OriginFieldType


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
        self._pushed_ids: Dict[int, tuple] = {}  # id(obj) -> (layer_idx, row_idx)

    @classmethod
    def create(cls) -> 'ORM2':
        """Start a new build session."""
        orm = cls()
        orm._db_build = core.WxDatabaseBuild()
        orm._db_build.begin("")
        return orm

    def push(self, obj: Any) -> int:
        """Serialize a @feature object into its layer. Returns row index.
        
        Automatically resolves REF and LIST[REF] fields by recursively pushing
        dependencies first. Deduplicates by object identity.
        """
        return self._push_recursive(obj)
        
    def _push_recursive(self, obj: Any) -> int:
        """Internal recursive push with deduplication."""
        obj_id = id(obj)
        if obj_id in self._pushed_ids:
            # Already pushed, return existing row
            layer_idx, row_idx = self._pushed_ids[obj_id]
            return row_idx
            
        cls = type(obj)
        schema = get_schema(cls)
        
        # First, recursively push all REF and LIST[REF] dependencies
        self._push_dependencies(obj, schema)
        
        # Now push this object
        state = self._ensure_layer(cls)
        row_idx = state.row_count
        
        # Create ref_resolver closure that uses our _pushed_ids
        def ref_resolver(ref_obj):
            if ref_obj is None:
                return None
            ref_id = id(ref_obj)
            if ref_id in self._pushed_ids:
                layer_idx, row_idx = self._pushed_ids[ref_id]
                return core.WxFeatureRef.make_ref(layer_idx, row_idx)
            return None
            
        push_feature(obj, state.build, state.schema, ref_resolver)
        state.row_count += 1
        
        # Record this object as pushed
        self._pushed_ids[obj_id] = (state.layer_idx, row_idx)
        
        return row_idx
        
    def _push_dependencies(self, obj: Any, schema: 'LayerSchema'):
        """Recursively push all REF and LIST[REF] field dependencies."""
        cache = obj.__dict__
        for fd in schema.fields:
            value = cache.get(fd.name)
            if value is None:
                continue
                
            if fd.field_type == OriginFieldType.ref:
                # Single REF field - push the referenced object
                self._push_recursive(value)
            elif fd.field_type == OriginFieldType.list and fd.list_elem_type == OriginFieldType.ref:
                # LIST[REF] field - push all referenced objects
                if hasattr(value, '__iter__'):
                    for ref_obj in value:
                        if ref_obj is not None:
                            self._push_recursive(ref_obj)

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