# python/fastdb4py/decorator.py
from __future__ import annotations
from typing import Any, get_type_hints, get_origin, get_args
import typing

from .registry import get_schema
from .type import OriginFieldType, get_origin_type, FIELD_TYPE_MAP

# Types that are explicitly rejected
_REJECTED_TYPES = {dict, tuple, set, frozenset, object}
_REJECTED_ORIGINS = {dict, tuple, set, frozenset}


def feature(cls):
    """Decorator that registers a plain Python class as a fastdb feature.

    Validates all annotations against the strict type policy, then registers
    the schema in the global SchemaRegistry. The class is returned unchanged.

    Usage:
        @feature
        class Point:
            x: F64
            y: F64
    """
    _validate_annotations(cls)
    get_schema(cls)  # register
    return cls


def _validate_annotations(cls):
    """Raise TypeError for unsupported field types."""
    try:
        hints = get_type_hints(cls)
    except NameError:
        hints = dict(getattr(cls, '__annotations__', {}))

    for name, hint in hints.items():
        if name.startswith('_'):
            continue
        _check_hint(name, hint)


def _check_hint(name: str, hint):
    """Validate a single annotation."""
    # Check for typing.Any
    if hint is typing.Any:
        raise TypeError(
            f"Unsupported type 'Any' for field '{name}'. "
            "Use explicit types (F64, int, str, etc.)."
        )

    # Check for rejected concrete types
    if isinstance(hint, type) and hint in _REJECTED_TYPES:
        raise TypeError(
            f"Unsupported type '{hint.__name__}' for field '{name}'. "
            "Use @feature classes for structured data."
        )

    # Check for generic origins (Dict[K,V], Tuple[...], etc.)
    origin = get_origin(hint)
    if origin is not None:
        if origin in _REJECTED_ORIGINS:
            raise TypeError(
                f"Unsupported type '{hint}' for field '{name}'. "
                "Use @feature classes for structured data."
            )
        # bare list without type args
        if origin is list:
            args = get_args(hint)
            if not args:
                raise TypeError(
                    f"Unsupported type 'list' for field '{name}'. "
                    "Use list[F64], list[int], list[MyFeature], etc."
                )
        return

    # bare `list` (not generic)
    if hint is list:
        raise TypeError(
            f"Unsupported type 'list' for field '{name}'. "
            "Use list[F64], list[int], list[MyFeature], etc."
        )

    # Check that it maps to a known type
    ft = get_origin_type(hint)
    if ft == OriginFieldType.unknown:
        # Could be a @feature class (REF) — that's OK if it has annotations
        if isinstance(hint, type) and hasattr(hint, '__annotations__'):
            return
        raise TypeError(
            f"Unsupported type '{hint}' for field '{name}'. "
            "Only serializable types are allowed."
        )