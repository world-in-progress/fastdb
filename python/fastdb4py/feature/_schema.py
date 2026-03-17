from threading import Lock
from weakref import WeakKeyDictionary
from typing import Dict, Any, List, Tuple, Type, get_type_hints

import numpy as _np

from ..type import OriginFieldType, get_origin_type
from .base import BaseFeature

# Scalar field types that can be read/written via get_fields_as_doubles / set_fields_from_doubles.
_SCALAR_ORIGIN_TYPES = frozenset((
    OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
    OriginFieldType.i32, OriginFieldType.f32, OriginFieldType.f64,
    OriginFieldType.u8n, OriginFieldType.u16n,
))

_SCHEMA_CACHE: WeakKeyDictionary = WeakKeyDictionary()
_SCHEMA_LOCK = Lock()
# Attribute name used to cache the ClassSchema directly on the class object.
# cls.__dict__.get(_SCHEMA_ATTR) is ~40-50 ns vs 209 ns for WeakKeyDict lookup.
_SCHEMA_ATTR = '__fastdb_schema__'


class ClassSchema:
    """Unified per-class metadata cache for Feature subclasses.

    Merges what was previously 4 separate WeakKeyDictionary caches:
    - _feature_hints_cache (feature.py): get_type_hints() result
    - _global_feature_defn_cache (utils.py): parse_defns() result
    - _column_accessor_cache (table.py): ColumnAccessor class (stored as column_accessor_class)
    - _CLASS_SCHEMA_CACHE (serializer.py): reads hints + ordered_defns from here
    """
    __slots__ = (
        'hints',                 # Dict[str, Any] — get_type_hints() result
        'origin_hints',          # Dict[str, (OriginFieldType, int)] — parse_defns() result
        'ordered_defns',         # List[(name, OriginFieldType)] sorted by field index
        'field_index_map',       # Dict[str, int] — name → column position for ColumnAccessor
        'column_accessor_class', # Dynamically-created ColumnAccessor class, or None
        'scalar_field_ids_np',   # numpy uint32 array of scalar field indices (for batch API)
    )

    def __init__(
        self,
        hints: Dict[str, Any],
        origin_hints: Dict[str, tuple],
        ordered_defns: List[Tuple[str, OriginFieldType]],
        field_index_map: Dict[str, int],
        scalar_field_ids_np,
    ):
        self.hints = hints
        self.origin_hints = origin_hints
        self.ordered_defns = ordered_defns
        self.field_index_map = field_index_map
        self.column_accessor_class = None  # lazily populated by table.py
        self.scalar_field_ids_np = scalar_field_ids_np


def get_class_schema(cls: Type) -> ClassSchema:
    """Return the ClassSchema for the given Feature subclass, computing it on first call.

    Two-level cache:
    1. Class-level attribute (cls.__dict__.get): ~40-50 ns — no WeakRef overhead.
    2. WeakKeyDictionary fallback: ~209 ns — used for metaclass-protected types that
       rejected setattr, and kept in sync for GC safety.
    """
    # Hot path: class owns the schema as a plain class attribute (~40-50 ns).
    schema = cls.__dict__.get(_SCHEMA_ATTR)
    if schema is not None:
        return schema

    # Warm fallback: WeakKeyDict for types that could not accept setattr.
    schema = _SCHEMA_CACHE.get(cls)
    if schema is not None:
        return schema

    with _SCHEMA_LOCK:
        # Re-check both under lock (double-checked locking).
        schema = cls.__dict__.get(_SCHEMA_ATTR) or _SCHEMA_CACHE.get(cls)
        if schema is not None:
            return schema

        hints = get_type_hints(cls)

        # Build origin_hints: field_name → (OriginFieldType, field_index)
        # This is what parse_defns() used to compute (Cache 2).
        origin_hints: Dict[str, tuple] = {}
        for idx, (field_name, hint) in enumerate(hints.items()):
            if field_name.startswith('_'):
                continue
            try:
                origin_type = get_origin_type(hint)
                if origin_type == OriginFieldType.unknown:
                    if hasattr(hint, '__mro__') and issubclass(hint, BaseFeature):
                        origin_type = OriginFieldType.ref
                    elif isinstance(hint, str) or hasattr(hint, '__forward_arg__'):
                        origin_type = OriginFieldType.ref
            except Exception:
                origin_type = OriginFieldType.unknown
            origin_hints[field_name] = (origin_type, idx)

        # ordered_defns: sorted by field index — equivalent to get_all_defns() result.
        ordered_defns = [
            (name, ft)
            for name, (ft, _) in sorted(origin_hints.items(), key=lambda x: x[1][1])
        ]

        # field_index_map: name → position in ordered list, used by ColumnAccessor.
        field_index_map = {name: i for i, (name, _) in enumerate(ordered_defns)}

        # scalar_field_ids_np: numpy uint32 array of scalar field indices for batch API.
        scalar_ids = [idx for _, (ft, idx) in origin_hints.items() if ft in _SCALAR_ORIGIN_TYPES]
        scalar_field_ids_np = _np.array(scalar_ids, dtype=_np.uint32)

        schema = ClassSchema(hints, origin_hints, ordered_defns, field_index_map, scalar_field_ids_np)

        # Primary cache: store directly on the class — O(1) dict lookup next time.
        try:
            setattr(cls, _SCHEMA_ATTR, schema)
        except (TypeError, AttributeError):
            pass  # metaclass-protected class: WeakKeyDict only
        _SCHEMA_CACHE[cls] = schema  # keep WeakKeyDict in sync as GC-safe fallback
        return schema
