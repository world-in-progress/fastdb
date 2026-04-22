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
    OriginFieldType.wstr: 'get_field_as_string',
}


def _read_field(feat: 'core.WxFeature', fd: FieldDef) -> Any:
    """Read one field value from a C++ WxFeature."""
    if fd.field_type == OriginFieldType.str:
        raw = feat.get_field_as_string_view(fd.field_id)
        if raw is None:
            return ''
        return raw.to_bytes().decode('utf-8')

    getter_name = _GETTERS.get(fd.field_type)
    if getter_name is not None:
        getter = getattr(feat, getter_name)
        return getter(fd.field_id)

    if fd.field_type == OriginFieldType.bytes:
        raw = feat.get_geometry_like_chunk()
        if raw is None:
            return b''
        return raw.to_bytes() if hasattr(raw, 'to_bytes') else bytes(raw)

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
    """Read-only proxy that dispatches attribute reads to C++ WxFeature."""
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
    """Return a read-only proxy that reads from C++ on each attribute access."""
    schema = get_schema(cls)
    feat = layer.tryGetFeature(idx)
    return MappedFeature(cls, feat, schema)


def copy_feature(cls: Type, layer: 'core.WxLayerTable', idx: int) -> Any:
    """Create a fully detached Python instance with all field values copied."""
    schema = get_schema(cls)
    feat = layer.tryGetFeature(idx)

    obj = cls.__new__(cls)
    for fd in schema.fields:
        val = _read_field(feat, fd)
        obj.__dict__[fd.name] = val

    return obj


def bind_feature(cls: Type, db, layer: 'core.WxLayerTable', idx: int) -> Any:
    """Return a live-mapped instance bound to the C++ backing store.

    For old Feature subclasses (which expose ``map_from``), delegates to
    ``cls.map_from(db, feat)`` so reads AND writes dispatch to C++.
    For new ``@feature`` classes, falls back to ``copy_feature``.
    """
    map_from = getattr(cls, 'map_from', None)
    if map_from is not None:
        return map_from(db, layer.tryGetFeature(idx))
    return copy_feature(cls, layer, idx)
