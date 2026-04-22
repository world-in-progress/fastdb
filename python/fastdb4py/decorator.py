# python/fastdb4py/decorator.py
from __future__ import annotations
from typing import Any, get_type_hints, get_origin, get_args
import typing

from .registry import register_class
from .type import OriginFieldType, get_origin_type, FIELD_TYPE_MAP

# Types that are explicitly rejected
_REJECTED_TYPES = {dict, tuple, set, frozenset, object}
_REJECTED_ORIGINS = {dict, tuple, set, frozenset}


def feature(cls):
    """Decorator that registers a plain class as a fastdb feature.

    Actions performed:
    1. Validate __slots__ (reject unless __dict__ is included)
    2. Set __fastdb_feature__ marker
    3. Inject __init__(**kwargs) if no user-defined __init__
    4. Register in class registry
    5. Build schema lazily (deferred to first get_schema() call)
    """
    # 1. Reject __slots__ without __dict__
    if '__slots__' in cls.__dict__:
        slots = cls.__dict__['__slots__']
        if '__dict__' not in slots:
            raise TypeError(
                f"@feature class {cls.__name__} defines __slots__ without "
                f"'__dict__'. Remove __slots__ or include '__dict__'."
            )

    # 2. Set marker
    cls.__fastdb_feature__ = True

    # 3. Inject __init__ if not user-defined
    if '__init__' not in cls.__dict__:
        def __init__(self, **kwargs):
            self.__dict__.update(kwargs)
        cls.__init__ = __init__

    # 4. Register in class registry
    register_class(cls, allow_replace=True)

    # 5. Validate annotations (forward-ref tolerant, NO eager schema build)
    _validate_annotations(cls)

    return cls


def _validate_annotations(cls):
    """Validate type annotations on a @feature class. Skips forward references."""
    try:
        hints = get_type_hints(cls)
    except NameError:
        # Forward references that can't be resolved yet — skip validation entirely
        return

    for name, hint in hints.items():
        if name.startswith('_'):
            continue
        # Skip string forward refs and ForwardRef objects
        if isinstance(hint, str) or hasattr(hint, '__forward_arg__'):
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
        # numpy.ndarray (and other ndarray subclasses) — handled by FastSerializer buffer layers
        try:
            import numpy as _np
            if isinstance(hint, type) and issubclass(hint, _np.ndarray):
                return
        except ImportError:
            pass
        # Could be a @feature class (REF) — that's OK if it has annotations
        if isinstance(hint, type) and hasattr(hint, '__annotations__'):
            return
        raise TypeError(
            f"Unsupported type '{hint}' for field '{name}'. "
            "Only serializable types are allowed."
        )