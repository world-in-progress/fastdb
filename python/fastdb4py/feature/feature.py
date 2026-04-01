import warnings
import numpy as np
from typing import Dict, Any, TypeVar, Type

from .. import core
from .base import BaseFeature
from .utils import parse_defns
from ._schema import ClassSchema, get_class_schema, _SCHEMA_ATTR
from ..type import FIELD_TYPE_FACTORIES, OriginFieldType

T = TypeVar('T', bound='Feature')

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
        # _cache is eagerly allocated to avoid race conditions under free-threaded
        # Python (PEP 703).  The cost (~50-100 ns for an empty dict) is negligible
        # compared to the complexity of lazy-init synchronisation.
        self._cache: Dict[str, Any] = {}
        # Origin feature mapped from fastdb layer (None means pure Python object).
        self._origin: core.WxFeature | None = None
        # Database handle used when the feature is mapped from fastdb.
        self._db: core.WxDatabase | core.WxDatabaseBuild | None = None
        # Class-attr lookup (~40 ns) beats WeakKeyDict (~209 ns). Falls back to
        # get_class_schema() only on the very first instantiation of this class.
        _schema: ClassSchema = (
            type(self).__dict__.get(_SCHEMA_ATTR) or get_class_schema(type(self))
        )
        # Store schema ref for cold-path access (ref/unknown fields).
        self._schema: ClassSchema = _schema
        # Parsed fastdb field definitions: name -> (field_type, field_index).
        # Kept as instance attr so hot-path __getattr__/__setattr__ avoids extra lookup.
        self._origin_hints: Dict[str, tuple[OriginFieldType, int]] = _schema.origin_hints

        # Constructor fast-path:
        # kwargs are applied directly to avoid __setattr__ dispatch overhead.
        if kwargs:
            cache: Dict[str, Any] = self._cache
            for key, value in kwargs.items():
                if key.startswith('_'):
                    object.__setattr__(self, key, value)
                else:
                    cache[key] = value

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
        if name in cache:
            return cache[name]

        # Resolve field metadata from parsed feature definitions.
        defn = self._origin_hints.get(name)

        # Unknown field behavior:
        # - If it is a typed Python field (e.g. List[T]), return None by default.
        # - Otherwise, follow Python protocol and raise AttributeError.
        if defn is None or defn[0] is OriginFieldType.unknown:
            if name in self._schema.hints:
                return None
            raise AttributeError(f"'{self.__class__.__name__}' object has no attribute '{name}'")

        ft, fid = defn

        # Case 1: not mapped from database yet (pure Python object).
        # Return cached default values and persist them into cache.
        if not self.fixed:
            if ft == OriginFieldType.ref:
                ref_feature_type = self._schema.hints[name]
                default_ref_feature = ref_feature_type()
                self._cache[name] = default_ref_feature
                return default_ref_feature

            # Use factory to produce a fresh default per field per instance.
            # Mutable types (list, bytes) get new objects; scalars get CPython singletons.
            factory = FIELD_TYPE_FACTORIES.get(ft)
            default_value = factory() if factory is not None else None
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

            ref_feature_type = self._schema.hints.get(name, Feature)
            feature = ref_feature_type.map_from(self._db, ref_feature_origin)
            self._cache[name] = feature
            return feature

        if ft == OriginFieldType.list:
            from .ref_list import FeatureRefList
            import typing
            elem_type = self._schema.list_element_types.get(name)
            if elem_type == OriginFieldType.ref:
                hint = self._schema.hints.get(name)
                args = getattr(hint, '__args__', None)
                ref_cls = args[0] if args else Feature
                # Resolve ForwardRef or string to actual class
                if isinstance(ref_cls, (str, typing.ForwardRef)):
                    fwd_name = ref_cls if isinstance(ref_cls, str) else ref_cls.__forward_arg__
                    # Self-referential: check parent class name first
                    parent_cls = type(self)
                    if fwd_name == parent_cls.__name__:
                        ref_cls = parent_cls
                    else:
                        import sys
                        mod = sys.modules.get(parent_cls.__module__, None)
                        ref_cls = getattr(mod, fwd_name, Feature) if mod else Feature
                result = FeatureRefList(self._origin, fid, ref_cls, self._db)
            else:
                _LIST_ELEM_DTYPE = {
                    OriginFieldType.u8:  np.uint8,
                    OriginFieldType.u16: np.uint16,
                    OriginFieldType.u32: np.uint32,
                    OriginFieldType.i32: np.int32,
                    OriginFieldType.f32: np.float32,
                    OriginFieldType.f64: np.float64,
                }
                dtype = _LIST_ELEM_DTYPE.get(elem_type, np.float64)
                chunk = self._origin.get_field_as_list_view(fid)
                result = chunk.as_array(dtype)
            self._cache[name] = result
            return result

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
            self._cache[name] = value
            return

        ft, fid = defn

        # Pure Python object path: assign to cache only.
        if self._origin is None:
            self._cache[name] = value
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
                self._cache[name] = None
                return

            ref_feature_type: Feature = self._schema.hints[name]
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

    def read_all_scalars(self, out=None) -> np.ndarray:
        """Batch-read all scalar fields into a numpy float64 array (1 SWIG call).

        Requires a db-mapped Feature (feature.fixed == True).
        Returns a float64 array of length = number of scalar fields, ordered by field index.

        Args:
            out: Pre-allocated numpy float64 array to fill. Created if not provided.
        """
        if not self.fixed:
            raise RuntimeError('read_all_scalars() requires a db-mapped Feature.')
        fids = self._schema.scalar_field_ids_np
        if out is None:
            out = np.empty(len(fids), dtype=np.float64)
        self._origin.get_fields_into(fids, out)
        return out

    def write_all_scalars(self, values: np.ndarray) -> None:
        """Batch-write all scalar fields from a numpy float64 array (1 SWIG call).

        Requires a db-mapped Feature (feature.fixed == True).
        values: float64 array of length = number of scalar fields, ordered by field index.
        """
        if not self.fixed:
            raise RuntimeError('write_all_scalars() requires a db-mapped Feature.')
        fids = self._schema.scalar_field_ids_np
        self._origin.set_fields_from_doubles(fids, values)

