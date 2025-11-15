import warnings
import numpy as np
from typing import Dict, TypeVar, Type

from .. import core
from .base import BasePipe
from .utils import get_defn, get_all_defns
from ..type import FIELD_TYPE_DEFAULTS, OriginFieldType

T = TypeVar('T', bound='FeaturePipe')

class FeaturePipe(BasePipe):
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
        
        # Case for not mapping from database ##############################################
        
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
        
        # Case for mapping from database ##################################################
        
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
        
        # Try to get origin type definition for member with the given name
        defn = get_defn(self.__class__, name)
        if defn is None or defn[0] is OriginFieldType.unknown:
            raise AttributeError(f'Field "{name}" not found in feature pipe "{self.__class__.__name__}".')
        
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
        if ft == OriginFieldType.ref:
            # Get referenced feature
            ref_pipe_type = self.__class__.__annotations__[name]
            if not isinstance(value, ref_pipe_type):
                raise TypeError(f'Field "{name}" expects a reference to type "{ref_pipe_type.__name__}", but got "{type(value).__name__}".')
            
            # Get the origin ref pipe and set all its fields with the given pipe
            origin_pipe = getattr(self, name)
            defns = get_all_defns(ref_pipe_type)
            for ref_field_name, _ in defns:
                setattr(origin_pipe, ref_field_name, getattr(value, ref_field_name))
            
        else:
            warnings.warn(f'Fastdb only support pipes to set numeric field for a scale-known block.', UserWarning)