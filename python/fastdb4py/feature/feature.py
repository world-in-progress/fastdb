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

class Feature(BaseFeature):
    def __init__(self, **kwargs):
        # Local cache for Python-side fields and serializer-hydrated values.
        self._cache: Dict[str, Any] = {}
        # Origin feature mapped from fastdb layer (None means pure Python object).
        self._origin: core.WxFeature | None = None
        # Database handle used when the feature is mapped from fastdb.
        self._db: core.WxDatabase | core.WxDatabaseBuild | None = None
        # Full Python type hints declared on this Feature subclass.
        self._type_hints: Dict[str, Any] = _get_feature_hints(self.__class__)
        # Parsed fastdb field definitions: name -> (field_type, field_index).
        self._origin_hints: Dict[str, tuple[OriginFieldType, int]] = parse_defns(self.__class__)
        
        # Constructor fast-path:
        # kwargs are applied directly to cache to avoid __setattr__ dispatch overhead.
        # This object is not fixed yet (_origin is None), so cache assignment is equivalent
        # to the non-fixed path in __setattr__.
        for key, value in kwargs.items():
            if key.startswith('_'):
                object.__setattr__(self, key, value)
            else:
                self._cache[key] = value
    
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
        if name in self._cache:
            return self._cache[name]

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
                self._cache[name] = default_ref_feature
                return default_ref_feature

            default_value = FIELD_TYPE_DEFAULTS.get(ft, None)
            self._cache[name] = default_value
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
            self._cache[name] = feature
            return feature

        # Scalar field mapping to fastdb getters.
        if ft == OriginFieldType.u8:
            return self._origin.get_field_as_int(fid)
        if ft == OriginFieldType.u16:
            return self._origin.get_field_as_int(fid)
        if ft == OriginFieldType.u32:
            return self._origin.get_field_as_int(fid)
        if ft == OriginFieldType.i32:
            return self._origin.get_field_as_int(fid)
        if ft == OriginFieldType.f32:
            return self._origin.get_field_as_float(fid)
        if ft == OriginFieldType.f64:
            return self._origin.get_field_as_float(fid)
        if ft == OriginFieldType.str:
            return self._origin.get_field_as_string(fid)
        if ft == OriginFieldType.wstr:
            return self._origin.get_field_as_wstring(fid)

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
            self._cache[name] = value
            return

        ft, fid = defn

        # Pure Python object path: assign to cache only.
        if not self.fixed:
            self._cache[name] = value
            return

        # Database-mapped numeric fields are written directly to fastdb origin.
        if ft in (
            OriginFieldType.u8,
            OriginFieldType.u16,
            OriginFieldType.u32,
            OriginFieldType.i32,
            OriginFieldType.f32,
            OriginFieldType.f64,
            OriginFieldType.u8n,
            OriginFieldType.u16n,
        ):
            self._origin.set_field(fid, value)
            return

        # Ref field handling strategy:
        # - Accept None as a nullable ref and keep it in cache.
        # - Validate Python type against annotation.
        # - Try native fastdb ref assignment when referenced origin exists.
        # - Always cache Python-side value for serializer compatibility.
        if ft == OriginFieldType.ref:
            if value is None:
                self._cache[name] = None
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

            self._cache[name] = value
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
