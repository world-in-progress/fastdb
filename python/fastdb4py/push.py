"""Serialize a decorated Python feature object into a WxLayerTableBuild row."""
from __future__ import annotations
from typing import Any, Callable, Optional, TYPE_CHECKING
import numpy as np

from .type import OriginFieldType, LIST_ELEM_DTYPE
from .push_compiler import (
    _c_add_begin, _c_add_end, _c_set_field, _c_set_cstr,
    _c_set_wstr, _c_set_raw, _c_set_list,
)

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
    _c_add_begin(layer_build)
    for fd in schema.fields:
        value = cache.get(fd.name)
        _set_field(layer_build, fd, value, ref_resolver)
    _c_add_end(layer_build)
    return -1


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


def _set_list_field(layer_build, fd, value, ref_resolver=None):
    """Set a list field using set_field_list_numeric."""
    if value is None or (hasattr(value, '__len__') and len(value) == 0):
        _c_set_list(layer_build, fd.field_id, b"")
        return
    elem_type = fd.list_elem_type
    if elem_type == OriginFieldType.ref:
        # LIST[REF]: pack as raw 5-byte ref entries via set_field_list_numeric
        import struct
        _zero_ref = b'\x00\x00\x00\x00\x00'
        parts = []
        if ref_resolver is not None:
            for ref_obj in value:
                if ref_obj is not None:
                    ref = ref_resolver(ref_obj)
                    if ref is not None:
                        parts.append(struct.pack('<HBH', ref.ilayer, ref.ifeature, ref.ifeatureH))
                    else:
                        parts.append(_zero_ref)
                else:
                    parts.append(_zero_ref)
        if parts:
            _c_set_list(layer_build, fd.field_id, b''.join(parts))
        else:
            _c_set_list(layer_build, fd.field_id, b"")
        return
    if elem_type is None:
        return  # Unknown list type
    dtype_str = LIST_ELEM_DTYPE.get(elem_type, 'float64')
    if isinstance(value, np.ndarray):
        arr = np.ascontiguousarray(value.astype(dtype_str, copy=False))
    else:
        arr = np.ascontiguousarray(np.array(value, dtype=dtype_str))
    _c_set_list(layer_build, fd.field_id, arr)