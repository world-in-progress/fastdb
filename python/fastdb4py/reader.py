# python/fastdb4py/reader.py
"""Read features from a built fastdb database in map or copy mode."""
from __future__ import annotations
from typing import Any, Type, TYPE_CHECKING

from .decorator import mapped_feature_class
from .registry import get_schema, FieldDef
from .type import (
    OriginFieldType,
    coerce_bool_scalar,
    is_native_list_storage_type,
    native_list_storage_diagnostic,
)
from .view_owner import FdbViewOwner, FdbViewWriteError, trusted_view_owner

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
    OriginFieldType.wstr: 'get_field_as_wstring',
}

_INTEGER_FIELD_TYPES = {
    OriginFieldType.u8,
    OriginFieldType.u16,
    OriginFieldType.u32,
    OriginFieldType.i32,
    OriginFieldType.u8n,
    OriginFieldType.u16n,
}
_FLOAT_FIELD_TYPES = {OriginFieldType.f32, OriginFieldType.f64}


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
        if fd.list_elem_type == OriginFieldType.ref:
            return []
        if not is_native_list_storage_type(fd.list_elem_type):
            diagnostic = native_list_storage_diagnostic(fd.name, fd.list_elem_type)
            raise TypeError(diagnostic)
        dtype_str = LIST_ELEM_DTYPE[fd.list_elem_type]
        try:
            chunk = feat.get_field_as_list_view(fd.field_id)
            return chunk.as_array(getattr(np, dtype_str)).copy()
        except Exception:
            return []

    return None


class MappedFeature:
    """Read-only proxy that dispatches attribute reads to C++ WxFeature."""
    __slots__ = ('_feat', '_schema', '_cls', '_fdb_owner', '_owner_generation')

    def __init__(
        self,
        cls: Type,
        feat: 'core.WxFeature',
        schema,
        *,
        owner: FdbViewOwner | None = None,
    ):
        if owner is None:
            owner = trusted_view_owner(writeable=True)
        object.__setattr__(self, '_cls', cls)
        object.__setattr__(self, '_feat', feat)
        object.__setattr__(self, '_schema', schema)
        object.__setattr__(self, '_fdb_owner', owner)
        object.__setattr__(self, '_owner_generation', owner.generation)

    def __getattr__(self, name: str) -> Any:
        owner = object.__getattribute__(self, '_fdb_owner')
        generation = object.__getattribute__(self, '_owner_generation')
        owner.assert_alive(generation)
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


class FeatureBacking:
    """Backing state for a mapped @feature instance."""

    def __init__(
        self,
        *,
        owner: FdbViewOwner,
        db,
        layer: 'core.WxLayerTable',
        row_index: int,
        schema,
        writeable: bool,
        table=None,
    ) -> None:
        self.owner = owner
        self.db = db
        self.layer = layer
        self.row_index = row_index
        self.schema = schema
        self.writeable = writeable
        self.table = table
        self.generation = owner.generation

    def has_field(self, name: str) -> bool:
        return self.schema.get(name) is not None

    def _field(self, name: str) -> FieldDef:
        fd = self.schema.get(name)
        if fd is None:
            raise AttributeError(f'Field {name!r} not found in mapped FastDB feature.')
        return fd

    def _feature(self):
        feat = self.layer.tryGetFeature(self.row_index)
        if feat is None:
            raise IndexError(f'Feature index {self.row_index} is no longer available.')
        return feat

    def assert_alive(self) -> None:
        self.owner.assert_alive(self.generation)

    def assert_writeable(self) -> None:
        self.owner.assert_alive(self.generation)
        if not self.writeable:
            raise FdbViewWriteError('FastDB mapped feature is read-only.')
        self.owner.assert_writeable(self.generation)

    def read_field(self, name: str) -> Any:
        fd = self._field(name)
        self.assert_alive()
        return _read_field(self._feature(), fd)

    def write_field(self, name: str, value: Any) -> None:
        fd = self._field(name)
        self.assert_writeable()
        feat = self._feature()
        if fd.field_type in _INTEGER_FIELD_TYPES:
            if name in getattr(self.schema, 'bool_field_names', ()):
                value = 1 if coerce_bool_scalar(value) else 0
            feat.set_field(fd.field_id, int(0 if value is None else value))
            return
        if fd.field_type in _FLOAT_FIELD_TYPES:
            feat.set_field(fd.field_id, float(0.0 if value is None else value))
            return
        raise FdbViewWriteError(
            f'FastDB mapped feature field {name!r} does not support direct row writes.'
        )


def mapped_feature(
    cls: Type,
    db,
    layer: 'core.WxLayerTable',
    idx: int,
    *,
    owner: FdbViewOwner | None = None,
    writeable: bool = False,
    table=None,
) -> Any:
    schema = get_schema(cls)
    if owner is None:
        owner = trusted_view_owner(writeable=writeable)

    view_cls = mapped_feature_class(cls, schema=schema)
    obj = view_cls.__new__(view_cls)
    obj.__dict__['_fdb_backing'] = FeatureBacking(
        owner=owner,
        db=db,
        layer=layer,
        row_index=idx,
        schema=schema,
        writeable=writeable,
        table=table,
    )
    return obj


def copy_feature(cls: Type, layer: 'core.WxLayerTable', idx: int) -> Any:
    """Create a fully detached Python instance with all field values copied."""
    schema = get_schema(cls)
    feat = layer.tryGetFeature(idx)

    obj = cls.__new__(cls)
    for fd in schema.fields:
        val = _read_field(feat, fd)
        obj.__dict__[fd.name] = val

    return obj


def bind_feature(
    cls: Type,
    db,
    layer: 'core.WxLayerTable',
    idx: int,
    *,
    owner: FdbViewOwner | None = None,
    writeable: bool = False,
    table=None,
) -> Any:
    """Return a live-mapped instance bound to the C++ backing store.

    For new ``@feature`` classes, returns an instance of that class with
    owner-bound backing state.
    For old Feature subclasses (which expose ``map_from``), delegates to
    ``cls.map_from(db, feat)`` so reads AND writes dispatch to C++.
    """
    if getattr(cls, '__fastdb_feature__', False) is True:
        return mapped_feature(
            cls,
            db,
            layer,
            idx,
            owner=owner,
            writeable=writeable,
            table=table,
        )

    map_from = getattr(cls, 'map_from', None)
    if map_from is not None:
        return map_from(db, layer.tryGetFeature(idx))
    return copy_feature(cls, layer, idx)
