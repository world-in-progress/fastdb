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