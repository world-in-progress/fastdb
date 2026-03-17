from threading import Lock
from weakref import WeakKeyDictionary
from typing import Dict, Any, List, Tuple, Type, get_type_hints

from ..type import OriginFieldType, get_origin_type
from .base import BaseFeature

_SCHEMA_CACHE: WeakKeyDictionary = WeakKeyDictionary()
_SCHEMA_LOCK = Lock()


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
    )

    def __init__(
        self,
        hints: Dict[str, Any],
        origin_hints: Dict[str, tuple],
        ordered_defns: List[Tuple[str, OriginFieldType]],
        field_index_map: Dict[str, int],
    ):
        self.hints = hints
        self.origin_hints = origin_hints
        self.ordered_defns = ordered_defns
        self.field_index_map = field_index_map
        self.column_accessor_class = None  # lazily populated by table.py


def get_class_schema(cls: Type) -> ClassSchema:
    """Return the ClassSchema for the given Feature subclass, computing it on first call."""
    if cls in _SCHEMA_CACHE:
        return _SCHEMA_CACHE[cls]

    with _SCHEMA_LOCK:
        if cls in _SCHEMA_CACHE:
            return _SCHEMA_CACHE[cls]

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

        schema = ClassSchema(hints, origin_hints, ordered_defns, field_index_map)
        _SCHEMA_CACHE[cls] = schema
        return schema
