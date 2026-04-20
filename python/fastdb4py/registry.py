# python/fastdb4py/registry.py
from __future__ import annotations
from dataclasses import dataclass
from threading import Lock
from typing import Any, Dict, List, Optional, Type, get_type_hints, get_origin, get_args
import typing
import weakref

import numpy as np

from .type import (
    OriginFieldType, get_origin_type, get_list_element_type,
    LIST_ELEM_CPP_TYPE, FIELD_TYPE_MAP, LIST_ELEM_ARRAY_TYPECODE,
)
from .feature.base import BaseFeature


def is_feature(cls) -> bool:
    """Check if cls was decorated with @feature.

    Uses cls.__dict__.get() (not getattr) so subclasses that merely
    inherit the marker are rejected — every class must be explicitly
    decorated.
    """
    return isinstance(cls, type) and cls.__dict__.get('__fastdb_feature__', False) is True


_class_registry: Dict[str, Type] = {}


def register_class(cls, *, allow_replace: bool = False) -> None:
    """Register a @feature class by name. Fails fast on collision.

    Parameters
    ----------
    allow_replace : bool
        If True, silently overwrite an existing entry (used by the
        @feature decorator where redefinition is normal across modules).
        If False (default), raise ValueError when a *different* class
        object with the same __name__ is already registered.
    """
    name = cls.__name__
    existing = _class_registry.get(name)
    if existing is not None and existing is not cls and not allow_replace:
        raise ValueError(
            f"Feature class name {name!r} already registered by "
            f"{existing.__module__}.{existing.__qualname__}. "
            f"Conflicting: {cls.__module__}.{cls.__qualname__}"
        )
    _class_registry[name] = cls


def lookup_class(name: str):
    """Look up a @feature class by name. Returns None if not found."""
    return _class_registry.get(name)


@dataclass(frozen=True, slots=True)
class FieldDef:
    """Metadata for a single field in a @feature class."""
    name: str
    field_type: OriginFieldType
    field_id: int                          # 0-based column index
    cpp_type: int                          # raw C++ FieldTypeEnum int
    ref_target: Optional[Type] = None      # target class for REF fields
    list_elem_type: Optional[OriginFieldType] = None
    list_ref_target: Optional[Type] = None  # target class for List[REF] fields


class LayerSchema:
    """Schema for one @feature class (= one fastdb layer)."""
    __slots__ = (
        'layer_name', 'fields', '_by_name',
        # Push plans — computed once at schema build time
        'numeric_plan',     # List[(field_id, field_name)]
        'str_plan',         # List[(field_id, field_name, is_wide)]
        'bytes_plan',       # List[(field_id, field_name)]
        'list_plan',        # List[(field_id, field_name, typecode)]
        'ref_fields',       # List[FieldDef] where field_type == ref
        'list_ref_fields',  # List[FieldDef] where list_elem_type == ref
        'has_ref_fields',   # bool — any ref or list[ref] fields
        # Compiled push fns — created at combine() time, NOT at @feature time
        'push_fn',          # compiled zero-branch push function
        'batch_fn',         # batch push (push_many_from_dicts_fc partial)
        # push_from_dict helpers
        'pfd_num_names',    # List[str]
        'pfd_num_ids',      # numpy uint32 array
        'pfd_str_names',    # List[str]
        'pfd_str_ids',      # numpy uint32 array
    )

    def __init__(self, layer_name: str, fields: List[FieldDef]):
        self.layer_name = layer_name
        self.fields = fields
        self._by_name: Dict[str, FieldDef] = {f.name: f for f in fields}
        # Push plans — populated by _build_schema
        self.numeric_plan = []
        self.str_plan = []
        self.bytes_plan = []
        self.list_plan = []
        self.ref_fields = []
        self.list_ref_fields = []
        self.has_ref_fields = False
        # Compiled push fns — populated later by ORM2.combine()
        self.push_fn = None
        self.batch_fn = None
        self.pfd_num_names = []
        self.pfd_num_ids = None
        self.pfd_str_names = []
        self.pfd_str_ids = None

    def get(self, name: str) -> Optional[FieldDef]:
        return self._by_name.get(name)

    def __len__(self):
        return len(self.fields)


_registry_lock = Lock()
_registry: weakref.WeakKeyDictionary = weakref.WeakKeyDictionary()

_NUMERIC_FT = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.f32, OriginFieldType.f64,
    OriginFieldType.u8n, OriginFieldType.u16n,
))


def get_schema(cls: Type) -> LayerSchema:
    """Return (or compute) the LayerSchema for cls. Thread-safe, cached."""
    schema = _registry.get(cls)
    if schema is not None:
        return schema
    with _registry_lock:
        schema = _registry.get(cls)
        if schema is not None:
            return schema
        schema = _build_schema(cls)
        _registry[cls] = schema
        return schema


def _build_schema(cls: Type) -> LayerSchema:
    """Parse class annotations and build a LayerSchema."""
    try:
        hints = get_type_hints(cls)
    except NameError:
        hints = dict(getattr(cls, '__annotations__', {}))

    fields: List[FieldDef] = []
    field_id = 0
    for name, hint in hints.items():
        if name.startswith('_'):
            continue
        ft = _resolve_field_type(hint)
        cpp_type = _resolve_cpp_type(ft, hint)
        ref_target = _resolve_ref_target(ft, hint)
        list_elem = _resolve_list_elem(ft, hint)

        fields.append(FieldDef(
            name=name, field_type=ft, field_id=field_id,
            cpp_type=cpp_type, ref_target=ref_target, list_elem_type=list_elem,
            list_ref_target=_resolve_list_ref_target(ft, hint),
        ))
        field_id += 1

    schema = LayerSchema(layer_name=cls.__name__, fields=fields)

    for fd in fields:
        ft = fd.field_type
        if ft in _NUMERIC_FT:
            schema.numeric_plan.append((fd.field_id, fd.name))
        elif ft == OriginFieldType.str:
            schema.str_plan.append((fd.field_id, fd.name, False))
        elif ft == OriginFieldType.wstr:
            schema.str_plan.append((fd.field_id, fd.name, True))
        elif ft == OriginFieldType.bytes:
            schema.bytes_plan.append((fd.field_id, fd.name))
        elif ft == OriginFieldType.list:
            if fd.list_elem_type == OriginFieldType.ref:
                pass  # list[ref] handled separately in combine()
            else:
                typecode = LIST_ELEM_ARRAY_TYPECODE.get(fd.list_elem_type, 'd')
                schema.list_plan.append((fd.field_id, fd.name, typecode))
        if ft == OriginFieldType.ref:
            schema.ref_fields.append(fd)
        if ft == OriginFieldType.list and fd.list_elem_type == OriginFieldType.ref:
            schema.list_ref_fields.append(fd)

    schema.has_ref_fields = bool(schema.ref_fields or schema.list_ref_fields)
    schema.pfd_num_names = [fn for _, fn in schema.numeric_plan]
    schema.pfd_num_ids = np.array([idx for idx, _ in schema.numeric_plan], dtype=np.uint32)
    schema.pfd_str_names = [fn for _, fn, _ in schema.str_plan]
    schema.pfd_str_ids = np.array([idx for idx, _, _ in schema.str_plan], dtype=np.uint32)

    return schema


def _resolve_field_type(hint) -> OriginFieldType:
    ft = get_origin_type(hint)
    if ft != OriginFieldType.unknown:
        return ft
    if isinstance(hint, type) and issubclass(hint, BaseFeature):
        return OriginFieldType.ref
    if isinstance(hint, type) and not issubclass(hint, (int, float, str, bytes, bool)):
        return OriginFieldType.ref
    return ft


def _resolve_cpp_type(ft: OriginFieldType, hint) -> int:
    if ft == OriginFieldType.list:
        elem_ft = get_list_element_type(hint)
        return LIST_ELEM_CPP_TYPE.get(elem_ft, 8)
    return ft.value


def _resolve_ref_target(ft: OriginFieldType, hint) -> Optional[Type]:
    if ft == OriginFieldType.ref and isinstance(hint, type):
        return hint
    return None


def _resolve_list_ref_target(ft: OriginFieldType, hint) -> Optional[Type]:
    """Extract the target class from List[SomeFeatureClass] annotations."""
    if ft != OriginFieldType.list:
        return None
    elem_ft = get_list_element_type(hint)
    if elem_ft != OriginFieldType.ref:
        return None
    args = get_args(hint)
    if args and isinstance(args[0], type):
        return args[0]
    return None


def _resolve_list_elem(ft: OriginFieldType, hint) -> Optional[OriginFieldType]:
    if ft == OriginFieldType.list:
        return get_list_element_type(hint)
    return None