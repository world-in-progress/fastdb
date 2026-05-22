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


def mapped_feature_class(cls, schema=None):
    """Return a mapped-row subclass without slowing ordinary owned instances."""
    mapped_cls = cls.__dict__.get('__fastdb_mapped_feature_class__')
    if mapped_cls is not None:
        return mapped_cls

    namespace = {
        '__module__': cls.__module__,
        '__fastdb_feature__': True,
        '__fastdb_feature_base__': cls,
        '__fastdb_layer_name__': getattr(cls, '__fastdb_layer_name__', cls.__name__),
    }
    if schema is not None:
        namespace['__fastdb_schema__'] = schema

    mapped_cls = type(f'{cls.__name__}FastdbView', (cls,), namespace)
    _install_feature_accessors(mapped_cls)
    cls.__fastdb_mapped_feature_class__ = mapped_cls
    return mapped_cls


def _install_feature_accessors(cls) -> None:
    """Install dual owned/mapped field access on a @feature class."""
    if cls.__dict__.get('__fastdb_field_accessors_installed__', False):
        return

    original_getattribute = getattr(cls, '__getattribute__', object.__getattribute__)
    original_setattr = getattr(cls, '__setattr__', object.__setattr__)
    default_getattribute = original_getattribute is object.__getattribute__
    default_setattr = original_setattr is object.__setattr__

    def __getattribute__(self, name: str):
        if name.startswith('_') or name in {'__class__', '__dict__'}:
            return object.__getattribute__(self, name)

        cache = object.__getattribute__(self, '__dict__')
        backing = cache.get('_fdb_backing')
        if backing is not None and backing.has_field(name):
            return backing.read_field(name)
        if default_getattribute:
            return object.__getattribute__(self, name)
        return original_getattribute(self, name)

    if default_setattr:
        def __setattr__(self, name: str, value):
            if name.startswith('_'):
                object.__setattr__(self, name, value)
                return

            cache = object.__getattribute__(self, '__dict__')
            backing = cache.get('_fdb_backing')
            if backing is not None and backing.has_field(name):
                backing.write_field(name, value)
                return
            cache[name] = value
    else:
        def __setattr__(self, name: str, value):
            if name.startswith('_'):
                object.__setattr__(self, name, value)
                return

            cache = object.__getattribute__(self, '__dict__')
            backing = cache.get('_fdb_backing')
            if backing is not None and backing.has_field(name):
                backing.write_field(name, value)
                return
            original_setattr(self, name, value)

    cls.__getattribute__ = __getattribute__
    cls.__setattr__ = __setattr__
    cls.__fastdb_field_accessors_installed__ = True


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
