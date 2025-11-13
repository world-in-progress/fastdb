import warnings
from dataclasses import dataclass
from typing import Generic, TypeVar, Type

from ... import core
from ..type import OriginFieldType, get_defn

T = TypeVar('T')

class Feature(Generic[T]):
    def __init__(self, schema: Type[T], db: core.WxDatabase | core.WxLayerTableBuild, origin: core.WxFeature | None = None):
        self._on_build = origin is None
        self._schema = schema
        self._origin = origin
        self._db = db
    
    def __getattr__(self, name: str):
        if self._on_build:
            warnings.warn(f'Accessing field "{name}" during building phase may not return valid data.', UserWarning)
            return None
        
        # Try to get origin type definition for schema member with the given name
        defn = get_defn(self._schema, name)
        if defn is None or defn[0] is OriginFieldType.unknown:
            warnings.warn(f'Field "{name}" not found in the layer schema.', UserWarning)
            return None
        
        ft, fid = defn
        
        # Type Bytes is specially stored in fastdb as geometry-like chunk
        # return it directly from layer
        # TODO: wrong?
        if ft == OriginFieldType.bytes:
            return self._origin.layer().get_geometry_like_chunk()
        
        # Type Ref requires special handling to get referenced feature
        elif ft == OriginFieldType.ref:
            # Get referenced feature
            ref = self._origin.get_field_as_ref(fid)
            
            # Return as Feature object
            feature = self._db.tryGetFeature(ref)
            ref_schema_class = self._schema.__annotations__[name]
            return Feature(ref_schema_class, self._db, feature)
        
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
    
    # def __setattr__(self, name, value):
    #     if not self._on_build:
    #         raise AttributeError(f'Cannot set value of field "{name}" from feature after building phase.')
        
    #     # Try to get origin type definition for schema member with the given name
    #     defn = get_defn(self._schema, name)
    #     if defn is None or defn[0] is OriginFieldType.unknown:
    #         raise AttributeError(f'Field "{name}" not found in the layer schema.')
        
    #     ft, fid = defn
        
    #     # Type Bytes is specially stored in fastdb as geometry-like chunk
    #     # set it directly to current feature
    #     if ft == OriginFieldType.bytes:
    #         self._db.set_geometry_raw(value)
        
    #     # Type Ref requires special handling to set referenced feature
    #     elif ft == OriginFieldType.ref:
    #         # Expect value to be Feature object
    #         if not isinstance(value, Feature):
    #             raise TypeError(f'Field "{name}" expects a Feature object as reference.')
            
    #         # Set field as ref
    #         self._origin.set_field_as_ref(fid, value._origin)
        
    #     # Other types: map to corresponding set_field_* method
    #     elif ft == OriginFieldType.u8:
    #         self._db.set_field(fid, value)
    #     elif ft == OriginFieldType.u16:
    #         self._db.set_field(fid, value)
    #     elif ft == OriginFieldType.u32:
    #         self._db.set_field(fid, value)
    #     elif ft == OriginFieldType.i32:
    #         self._db.set_field(fid, value)
    #     elif ft == OriginFieldType.f32:
    #         self._db.set_field(fid, value)
    #     elif ft == OriginFieldType.f64:
    #         self._db.set_field(fid, value)
    #     elif ft == OriginFieldType.str:
    #         self._db.set_field_cstring(fid, value)
    #     elif ft == OriginFieldType.wstr:
    #         self._db.set_field_wstring(fid, value)
        