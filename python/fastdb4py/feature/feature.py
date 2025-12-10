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
        self._cache: Dict[str, Any] = {}
        self._origin: core.WxFeature | None = None
        self._db: core.WxDatabase | core.WxDatabaseBuild | None = None
        self._type_hints: Dict[str, Any] = _get_feature_hints(self.__class__)
        self._origin_hints: Dict[str, tuple[OriginFieldType, int]] = parse_defns(self.__class__)
        
        for key, value in kwargs.items():
            setattr(self, key, value)
    
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
        # Try to get origin type definition with the given name
        defn = self._origin_hints.get(name, None)
        if defn is None or defn[0] is OriginFieldType.unknown:
            warnings.warn(f'Field "{name}" not found in feature "{self.__class__.__name__}".', UserWarning)
            return None
        
        ft, fid = defn
        
        # Case for not mapping from database ##############################################
        
        # If not on mapping, return cached value or default value
        if not self.fixed:
            if name in self._cache:
                return self._cache[name]
            else:
                if ft == OriginFieldType.ref:
                    ref_feature_type = self._type_hints[name]
                    default_ref_feature = ref_feature_type()
                    self._cache[name] = default_ref_feature
                    return default_ref_feature
                else:
                    default_value = FIELD_TYPE_DEFAULTS.get(ft, None)
                    self._cache[name] = default_value
                    return default_value
        
        # Case for mapping from database ##################################################
        
        # Type Bytes is specially stored in fastdb as geometry-like chunk
        # Return it directly from table feature
        if ft == OriginFieldType.bytes:
            return self._origin.get_geometry_like_chunk()
        
        # Type Ref requires special handling to get referenced feature
        elif ft == OriginFieldType.ref:
            # Get feature referencing
            ref = self._origin.get_field_as_ref(fid)
            
            # Return as Feature object
            ref_feature_type: Feature = self._type_hints[name]
            feature_origin: core.WxFeature = self._db.tryGetFeature(ref)
            return ref_feature_type.map_from(self._db, feature_origin)
        
        # Other types: map to corresponding get_field_as_* method
        elif ft == OriginFieldType.u8:
            return self._origin.get_field_as_int(fid)
        elif ft == OriginFieldType.u16:
            return self._origin.get_field_as_int(fid)
        elif ft == OriginFieldType.u32:
            return self._origin.get_field_as_int(fid)
        elif ft == OriginFieldType.i32:
            return self._origin.get_field_as_int(fid)
        elif ft == OriginFieldType.f32:
            return self._origin.get_field_as_float(fid)
        elif ft == OriginFieldType.f64:
            return self._origin.get_field_as_float(fid)
        elif ft == OriginFieldType.str:
            return self._origin.get_field_as_string(fid)
        elif ft == OriginFieldType.wstr:
            return self._origin.get_field_as_wstring(fid)
    
    def __setattr__(self, name: str, value):
        # Allow setting internal attributes directly
        if name.startswith('_'):
            object.__setattr__(self, name, value)
            return
        
        # Try to get origin type definition with the given name
        defn = self._origin_hints.get(name, None)
        if defn is None or defn[0] is OriginFieldType.unknown:
            warnings.warn(f'Field "{name}" not found in feature "{self.__class__.__name__}".', UserWarning)
            return
        
        ft, fid = defn
        
        # Case for not mapping from database ##############################################
        
        # Cache the value for later use
        if not self.fixed:
            self._cache[name] = value
            return
        
        # Case for mapping from database ##################################################
        
        # Directly set field value to database according to its type
        if ft == OriginFieldType.u8     \
        or ft == OriginFieldType.u16    \
        or ft == OriginFieldType.u32    \
        or ft == OriginFieldType.i32    \
        or ft == OriginFieldType.f32    \
        or ft == OriginFieldType.f64    \
        or ft == OriginFieldType.u8n    \
        or ft == OriginFieldType.u16n:
            self._origin.set_field(fid, value)
        elif ft == OriginFieldType.ref:
            # Get referenced feature type
            ref_feature_type: Feature = self._type_hints[name]
            if not isinstance(value, ref_feature_type):
                warnings.warn(f'Field "{name}" expects a reference to type "{ref_feature_type.__name__}", but got "{type(value).__name__}".', UserWarning)
                return
            
            self._origin.set_field(fid, value._origin)
            
            # Get the origin ref feature and set all its fields with the given feature
            # Note: this is a deep copy operation, performance may be affected for feature with many fields
            # origin_feature: Feature = getattr(self, name)
            # for ref_field_name in origin_feature._type_hints.keys():
            #     setattr(origin_feature, ref_field_name, getattr(value, ref_field_name))
            
        else:
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
