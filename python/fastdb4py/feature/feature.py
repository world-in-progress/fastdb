import warnings
from threading import Lock
from weakref import WeakKeyDictionary
from typing import Dict, Any, TypeVar, Type, get_type_hints

from .. import core
from .base import BaseFeature
from .utils import parse_defns
from ..type import FIELD_TYPE_DEFAULTS, OriginFieldType

T = TypeVar('T', bound='Feature')
_feature_hints_cache_lock = Lock()
_feature_hints_cache: WeakKeyDictionary = WeakKeyDictionary()

# Module-level dispatch table: replaces the if-chain in __getattr__ for scalar fields.
# dict lookup is O(1) and avoids repeated branch evaluation on every field access.
_SCALAR_GETTER = {
    OriginFieldType.u8:   lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.u16:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.u32:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.i32:  lambda o, fid: o.get_field_as_int(fid),
    OriginFieldType.f32:  lambda o, fid: o.get_field_as_float(fid),
    OriginFieldType.f64:  lambda o, fid: o.get_field_as_float(fid),
    OriginFieldType.str:  lambda o, fid: o.get_field_as_string(fid),
    OriginFieldType.wstr: lambda o, fid: o.get_field_as_wstring(fid),
}

# frozenset for O(1) membership test in __setattr__ numeric branch.
_NUMERIC_FIELD_TYPES = frozenset((
    OriginFieldType.u8,
    OriginFieldType.u16,
    OriginFieldType.u32,
    OriginFieldType.i32,
    OriginFieldType.f32,
    OriginFieldType.f64,
    OriginFieldType.u8n,
    OriginFieldType.u16n,
))

class Feature(BaseFeature):
    def __init__(self, **kwargs):
        # _cache is lazily allocated on first write to avoid dict alloc overhead
        # for db-mapped read-only Features (scalar reads go directly to SWIG).
        self._cache: Dict[str, Any] | None = None
        # Origin feature mapped from fastdb layer (None means pure Python object).
        self._origin: core.WxFeature | None = None
        # Database handle used when the feature is mapped from fastdb.
        self._db: core.WxDatabase | core.WxDatabaseBuild | None = None
        # Full Python type hints declared on this Feature subclass.
        self._type_hints: Dict[str, Any] = _get_feature_hints(self.__class__)
        # Parsed fastdb field definitions: name -> (field_type, field_index).
        self._origin_hints: Dict[str, tuple[OriginFieldType, int]] = parse_defns(self.__class__)

        # Constructor fast-path:
        # Only allocate _cache when kwargs are actually provided.
        # kwargs are applied directly to avoid __setattr__ dispatch overhead.
        if kwargs:
            cache: Dict[str, Any] = {}
            for key, value in kwargs.items():
                if key.startswith('_'):
                    object.__setattr__(self, key, value)
                else:
                    cache[key] = value
            object.__setattr__(self, '_cache', cache)

    def _get_cache(self) -> Dict[str, Any]:
        """Return _cache, allocating it on first call."""
        cache = self._cache
        if cache is None:
            cache = {}
            object.__setattr__(self, '_cache', cache)
        return cache

    @property
    def fixed(self) -> bool:
        # If the feature is mapped from a fixed table
        # Its _origin member must exist
        return self._origin is not None

    @classmethod
    def map_from(
        cls,
        db: core.WxDatabase | core.WxDatabaseBuild,
        origin: core.WxFeature | None = None
    ) -> T:
        feature = cls()
        feature._db = db
        feature._origin = origin
        return feature

    def __getattr__(self, name: str):
        # Cache-first access: serializer-populated values and dynamic fields live here.
        cache = self._cache
        if cache is not None and name in cache:
            return cache[name]

        # Resolve field metadata from parsed feature definitions.
        defn = self._origin_hints.get(name)

        # Unknown field behavior:
        # - If it is a typed Python field (e.g. List[T]), return None by default.
        # - Otherwise, follow Python protocol and raise AttributeError.
        if defn is None or defn[0] is OriginFieldType.unknown:
            if name in self._type_hints:
                return None
            raise AttributeError(f"'{self.__class__.__name__}' object has no attribute '{name}'")

        ft, fid = defn

        # Case 1: not mapped from database yet (pure Python object).
        # Return cached default values and persist them into cache.
        if not self.fixed:
            if ft == OriginFieldType.ref:
                ref_feature_type = self._type_hints[name]
                default_ref_feature = ref_feature_type()
                self._get_cache()[name] = default_ref_feature
                return default_ref_feature

            default_value = FIELD_TYPE_DEFAULTS.get(ft, None)
            self._get_cache()[name] = default_value
            return default_value

        # Case 2: mapped from database.
        # Bytes field is stored as geometry-like chunk in fastdb.
        if ft == OriginFieldType.bytes:
            return self._origin.get_geometry_like_chunk()

        # Ref field handling strategy:
        # 1) Try native fastdb ref lookup when origin/db are available.
        # 2) Return None when native ref is unavailable/invalid.
        # 3) Cache resolved feature instance for subsequent fast access.
        if ft == OriginFieldType.ref:
            try:
                ref = self._origin.get_field_as_ref(fid)
            except Exception:
                return None

            if not ref or self._db is None:
                return None

            ref_feature_origin = self._db.tryGetFeature(ref)
            if not ref_feature_origin:
                return None

            ref_feature_type = self._type_hints.get(name, Feature)
            feature = ref_feature_type.map_from(self._db, ref_feature_origin)
            self._get_cache()[name] = feature
            return feature

        # Scalar field mapping via dispatch table (O(1) dict lookup).
        getter = _SCALAR_GETTER.get(ft)
        if getter is not None:
            return getter(self._origin, fid)

        return None

    def __setattr__(self, name: str, value):
        # Internal runtime attributes bypass field mapping.
        if name.startswith('_'):
            object.__setattr__(self, name, value)
            return

        # Resolve field metadata from parsed feature definitions.
        defn = self._origin_hints.get(name)

        # Unknown or non-fastdb-mapped fields are kept in local cache.
        if defn is None or defn[0] is OriginFieldType.unknown:
            self._get_cache()[name] = value
            return

        ft, fid = defn

        # Pure Python object path: assign to cache only.
        if not self.fixed:
            self._get_cache()[name] = value
            return

        # Database-mapped numeric fields are written directly to fastdb origin.
        # frozenset membership test is O(1) vs tuple's O(n) scan.
        if ft in _NUMERIC_FIELD_TYPES:
            self._origin.set_field(fid, value)
            return

        # Ref field handling strategy:
        # - Accept None as a nullable ref and keep it in cache.
        # - Validate Python type against annotation.
        # - Try native fastdb ref assignment when referenced origin exists.
        # - Always cache Python-side value for serializer compatibility.
        if ft == OriginFieldType.ref:
            if value is None:
                self._get_cache()[name] = None
                return

            ref_feature_type: Feature = self._type_hints[name]
            if not isinstance(value, ref_feature_type):
                warnings.warn(f'Field "{name}" expects a reference to type "{ref_feature_type.__name__}", but got "{type(value).__name__}".', UserWarning)
                return

            try:
                if value._origin is not None:
                    self._origin.set_field(fid, value._origin)
            except Exception:
                pass

            self._get_cache()[name] = value
            return

        # Non-numeric writes are not supported by direct fastdb set_field API.
        warnings.warn(f'Fastdb only support features to set numeric field for a scale-known block.', UserWarning)

# Helpers ##################################################

def _get_feature_hints(feature_type: Type[T]) -> Dict[str, Any]:
    if feature_type in _feature_hints_cache:
        return _feature_hints_cache[feature_type]

    with _feature_hints_cache_lock:
        if feature_type in _feature_hints_cache:
            return _feature_hints_cache[feature_type]

        hints = get_type_hints(feature_type)
        _feature_hints_cache[feature_type] = hints
        return hints
