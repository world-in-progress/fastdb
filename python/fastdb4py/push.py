"""Serialize a decorated Python feature object into a WxLayerTableBuild row."""
from __future__ import annotations
from typing import Any, Callable, Optional, TYPE_CHECKING
import numpy as np

from .type import OriginFieldType, LIST_ELEM_DTYPE

if TYPE_CHECKING:
    from . import core
    from .registry import LayerSchema, FieldDef

_NUMERIC_TYPES = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.u8n, OriginFieldType.u16n,
    OriginFieldType.f32, OriginFieldType.f64,
))

_INT_TYPES = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.u8n, OriginFieldType.u16n,
))

_FLOAT_TYPES = frozenset((OriginFieldType.f32, OriginFieldType.f64))


def push_feature(
    obj: Any,
    layer_build: 'core.WxLayerTableBuild',
    schema: 'LayerSchema',
    ref_resolver: Optional[Callable] = None,
) -> int:
    """Serialize Python feature object to WxLayerTableBuild row.
    
    Reads fields from obj.__dict__, dispatches each to the correct
    WxLayerTableBuild setter. Returns row index (or -1 to indicate 
    caller should track).
    """
    cache = obj.__dict__
    layer_build.add_feature_begin()
    for fd in schema.fields:
        value = cache.get(fd.name)
        _set_field(layer_build, fd, value, ref_resolver)
    layer_build.add_feature_end()
    return -1


def _set_field(layer_build, fd, value, ref_resolver):
    """Set a single field in the layer build."""
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
    elif ft == OriginFieldType.list:
        _set_list_field(layer_build, fd, value)


def _set_list_field(layer_build, fd, value):
    """Set a list field using set_field_list_numeric."""
    if value is None or (hasattr(value, '__len__') and len(value) == 0):
        layer_build.set_field_list_numeric(fd.field_id, b"")
        return
    elem_type = fd.list_elem_type
    if elem_type is None or elem_type == OriginFieldType.ref:
        return  # LIST[REF] handled by ORM-level graph traversal
    dtype_str = LIST_ELEM_DTYPE.get(elem_type, 'float64')
    if isinstance(value, np.ndarray):
        arr = np.ascontiguousarray(value.astype(dtype_str, copy=False))
    else:
        arr = np.ascontiguousarray(np.array(value, dtype=dtype_str))
    layer_build.set_field_list_numeric(fd.field_id, arr)