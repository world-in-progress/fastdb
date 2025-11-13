import weakref
import warnings
import inspect
from typing import Optional
from pydantic import BaseModel, ConfigDict, PrivateAttr
from threading import Lock
from dataclasses import dataclass
from typing import Dict, Any, Type, TypeVar, get_type_hints, get_origin, get_args

from . import core
from .type import OriginFieldType

T = TypeVar('T')

# Not used currently
@dataclass
class OriginFieldDefinition:
    name: str
    type: OriginFieldType
    vmin: float = 0.0
    vmax: float = 1.0

# TypeVars for Python side type annotations
BOOL = TypeVar('BOOL', bound=bool)
U8 = TypeVar('U8', bound=int)
U16 = TypeVar('U16', bound=int)
U32 = TypeVar('U32', bound=int)
I32 = TypeVar('I32', bound=int)
U8N = TypeVar('U8N', bound=int)
U16N = TypeVar('U16N', bound=int)
F32 = TypeVar('F32', bound=float)
F64 = TypeVar('F64', bound=float)
STR = TypeVar('STR', bound=str)
WSTR = TypeVar('WSTR', bound=str)
REF = TypeVar('REF', bound=object)
BYTES = TypeVar('BYTES', bound=bytes)

# class LIST(list, Generic[T]):
#     def __init__(self, *args, **kwargs):
#         super().__init__(*args, **kwargs)
    
#     def __getitem__(self, index: int) -> T:

class FeaturePipe:
    def __init__(self, **kwargs):
        self._cache: Dict[str, any] = {}
        self._origin: core.WxFeature | None = None
        self._db: core.WxDatabase | core.WxDatabaseBuild | None = None
        self._layer: core.WxLayerTable | core.WxLayerTableBuild | None = None
        
        for key, value in kwargs.items():
            setattr(self, key, value)
    
    @property
    def fixed(self) -> bool:
        # If the feature is mapped from a fixed layer
        # The _origin member must exist
        return self._origin is not None
    
    def map_from(
        self,
        db: core.WxDatabase | core.WxDatabaseBuild,
        layer: core.WxLayerTable | core.WxLayerTableBuild,
        origin: core.WxFeature | None = None
    ):
        self._db = db
        self._layer = layer
        self._origin = origin
    
    def __getattr__(self, name: str):
        # Try to get origin type definition for schema member with the given name
        defn = get_defn(self.__class__, name)
        if defn is None or defn[0] is OriginFieldType.unknown:
            warnings.warn(f'Field "{name}" not found in the layer schema.', UserWarning)
            return None
        
        ft, fid = defn
        
        # If not on mapping, return cached value or default value
        if not self.fixed:
            value = self._cache.get(name, None)
            if value is None:
                if ft == OriginFieldType.ref:
                    ref_pipe_type = self.__class__.__annotations__[name]
                    pipe = ref_pipe_type()
                    self._cache[name] = pipe
                    return pipe
                else:
                    default_value = FIELD_TYPE_DEFAULTS.get(ft, None)
                    self._cache[name] = default_value
                    return default_value
            return value
        
        # Case for mapping from database ##########
        
        # Type Bytes is specially stored in fastdb as geometry-like chunk
        # return it directly from layer
        if ft == OriginFieldType.bytes:
            return self._origin.get_geometry_like_chunk()
        
        # Type Ref requires special handling to get referenced feature
        elif ft == OriginFieldType.ref:
            # Get referenced feature
            ref = self._origin.get_field_as_ref(fid)
            
            # Return as Feature object
            feature = self._db.tryGetFeature(ref)
            ref_pipe_type = self.__class__.__annotations__[name]
            pipe = ref_pipe_type()
            pipe.map_from(self._db, feature.layer(), feature)
            return pipe

        # elif ft == OriginFieldType.list:
        #     pipe_type = get_args(self.__class__.__annotations__[name])[0]
        #     layer_name = pipe_type.__name__
        #     layer_count = self._db.get_layer_count()
            
        #     layer = None
        #     list_layer = None
        #     for i in range (layer_count):
        #         l: core.WxLayerTable = self._db.get_layer(i)
        #         l_name = l.name()
        #         if l_name == layer_name:
        #             layer = l
        #         elif l_name == '_list':
        #             list_layer = l
            
        #     if layer is None:
        #         raise ValueError(f'Layer "{layer_name}" not found in database for "{pipe_type.__name__}" when initializing LIST pipe.')
        #     if list_layer is None:
        #         raise ValueError(f'Layer "_list" not found in database for "{pipe_type.__name__}" when initializing LIST pipe.')
            
            
            
        #     pipe = LIST()
        #     pipe.map_from(self._db, layer, )
            
        
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
        
        # Try to get origin type definition for schema member with the given name
        defn = get_defn(self.__class__, name)
        if defn is None or defn[0] is OriginFieldType.unknown:
            raise AttributeError(f'Field "{name}" not found in the layer schema.')
        
        ft, fid = defn
        
        if not self.fixed:
            # Cache the value for later use
            self._cache[name] = value
            return
        
        # Set field value according to its type
        if ft == OriginFieldType.u8     \
        or ft == OriginFieldType.u16    \
        or ft == OriginFieldType.u32    \
        or ft == OriginFieldType.i32    \
        or ft == OriginFieldType.f32    \
        or ft == OriginFieldType.f64    \
        or ft == OriginFieldType.u8n    \
        or ft == OriginFieldType.u16n:
            self._origin.set_field(fid, value)
        else:
            warnings.warn(f'Fastdb only support pipes to set numeric field for a scale-known block.', UserWarning)

# class BOOL(FeaturePipe):
#     pass
# class U8(FeaturePipe):
#     pass
# class U16(FeaturePipe):
#     pass
# class U32(FeaturePipe):
#     pass
# class I32(FeaturePipe):
#     pass
# class U8N(FeaturePipe):
#     pass
# class U16N(FeaturePipe):
#     pass
# class F32(FeaturePipe):
#     pass
# class F64(FeaturePipe):
#     pass
# class STR(FeaturePipe):
#     pass
# class WSTR(FeaturePipe):
#     pass
# class REF(FeaturePipe):
#     pass
# class BYTES(FeaturePipe):
#     pass
# class LIST(FeaturePipe, Generic[T]):
#     begin: U32
#     length: U32
    
#     def __init__(self):
#         super().__init__()
#         layer_name = pipe_type.__name__
#         layer_count = self._db.get_layer_count()
        
#         self._layer = None
#         for i in range (layer_count):
#             layer: core.WxLayerTable = self._db.get_layer(i)
#             if layer.name() == layer_name:
#                 self._layer = layer
        
#         if self._layer is None:
#             raise ValueError(f'Layer "{layer_name}" not found in database for "{pipe_type.__name__}" when initializing LIST pipe.')
#         self._pipe_type = pipe_type
    
#     def __getattr__(self, name):
#         self._db.get_layer
    
        
FIELD_TYPE_MAP = {
    BOOL:   OriginFieldType.u8,
    U8:     OriginFieldType.u8,
    U16:    OriginFieldType.u16,
    U32:    OriginFieldType.u32,
    I32:    OriginFieldType.i32,
    U8N:    OriginFieldType.u8n,
    U16N:   OriginFieldType.u16n,
    F32:    OriginFieldType.f32,
    F64:    OriginFieldType.f64,
    STR:    OriginFieldType.str,
    WSTR:   OriginFieldType.wstr,
    REF:    OriginFieldType.ref,
    BYTES:  OriginFieldType.bytes
}

FIELD_TYPE_DEFAULTS = {
    OriginFieldType.u8:    0,
    OriginFieldType.u16:   0,
    OriginFieldType.u32:   0,
    OriginFieldType.i32:   0,
    OriginFieldType.u8n:   0,
    OriginFieldType.u16n:  0,
    OriginFieldType.f32:   0.0,
    OriginFieldType.f64:   0.0,
    OriginFieldType.str:   '',
    OriginFieldType.wstr:  '',
    OriginFieldType.ref:   None,
    OriginFieldType.bytes: bytes()
}

# Mapping from Python type annotations to OriginFieldType
def get_origin_type(type_var: type) -> OriginFieldType:
    return FIELD_TYPE_MAP.get(type_var, OriginFieldType.unknown)

# Helpers ##################################################

_global_pipe_defn_cache = weakref.WeakKeyDictionary()
_cache_lock = Lock()

def parse_defns(cls):
    if cls in _global_pipe_defn_cache:
        return _global_pipe_defn_cache[cls]
    
    with _cache_lock:
        if cls in _global_pipe_defn_cache:
            return _global_pipe_defn_cache[cls]
        
        m: Dict[str, tuple[OriginFieldType, int]] = {}
        hints = get_type_hints(cls)
        for idx, (field_name, hint) in enumerate(hints.items()):
            if field_name.startswith('_'):
                continue
            
            try:
                origin_type = get_origin_type(hint)
                if origin_type == OriginFieldType.unknown:
                    if issubclass(hint, FeaturePipe):
                        origin_type = OriginFieldType.ref
            except Exception as e:
                origin_type = OriginFieldType.unknown
            m[field_name] = (origin_type, idx)
        
        _global_pipe_defn_cache[cls] = m
        return m

def get_defn(cls, field_name) -> tuple[OriginFieldType, int] | None:
    m = parse_defns(cls)
    return m.get(field_name)

def get_all_defns(cls) -> list[tuple[str, OriginFieldType]]:
    m = parse_defns(cls)
    # Return sorted list of definitions by field index
    return [(field_name, defn[0]) for field_name, defn in sorted(m.items(), key=lambda item: item[1][1])]