# python/fastdb4py/registry.py
from __future__ import annotations
from dataclasses import dataclass
from threading import Lock
from typing import Any, Dict, List, Optional, Type, get_type_hints, get_origin, get_args
import typing
import weakref

from .type import (
    OriginFieldType, get_origin_type, get_list_element_type,
    LIST_ELEM_CPP_TYPE, FIELD_TYPE_MAP,
)
from .feature.base import BaseFeature


@dataclass(frozen=True, slots=True)
class FieldDef:
    """Metadata for a single field in a @feature class."""
    name: str
    field_type: OriginFieldType
    field_id: int                          # 0-based column index
    cpp_type: int                          # raw C++ FieldTypeEnum int
    ref_target: Optional[Type] = None      # target class for REF fields
    list_elem_type: Optional[OriginFieldType] = None


class LayerSchema:
    """Schema for one @feature class (= one fastdb layer)."""
    __slots__ = ('layer_name', 'fields', '_by_name')

    def __init__(self, layer_name: str, fields: List[FieldDef]):
        self.layer_name = layer_name
        self.fields = fields
        self._by_name: Dict[str, FieldDef] = {f.name: f for f in fields}

    def get(self, name: str) -> Optional[FieldDef]:
        return self._by_name.get(name)

    def __len__(self):
        return len(self.fields)


_registry_lock = Lock()
_registry: weakref.WeakKeyDictionary = weakref.WeakKeyDictionary()


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
        ))
        field_id += 1

    return LayerSchema(layer_name=cls.__name__, fields=fields)


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


def _resolve_list_elem(ft: OriginFieldType, hint) -> Optional[OriginFieldType]:
    if ft == OriginFieldType.list:
        return get_list_element_type(hint)
    return None