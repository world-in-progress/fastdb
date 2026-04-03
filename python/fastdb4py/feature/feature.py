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
    # Slot descriptors for the 5 private instance attrs: ~15ns access vs ~25ns __dict__.
    # Subclasses that don't define __slots__ get __dict__ (Python default), so user
    # field writes via __setattr__ → _cache still work; no API change.
    __slots__ = ('_cache', '_origin', '_db', '_schema', '_origin_hints')

    def __init__(self, **kwargs):
        # Use kwargs dict directly as _cache — avoids empty-dict allocation and copy loop.
        # All private slots (_origin, _db, _schema, _origin_hints) are lazily initialised
        # on first access via __getattr__ — avoids 1-2 slot writes per Feature creation.
        _cache_s.__set__(self, kwargs)

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
        # Use the DB variant of this class so __setattr__ dispatches through origin.
        # object.__new__ bypasses __init__ (and its **kwargs dict overhead); slots are
        # set explicitly below.
        db_cls = _get_db_cls(cls)
        feature = object.__new__(db_cls)
        _cache_s.__set__(feature, {})
        _origin_s.__set__(feature, origin)
        _db_s.__set__(feature, db)
        return feature

    def __getattr__(self, name: str):
        # Lazy-init guard: private slots not set in __init__ return safe defaults.
        # This fires only on the first access of each unset slot — thereafter the slot
        # holds a real value so CPython finds it via the type descriptor directly.
        if name[0] == '_':
            if name == '_origin':
                _origin_s.__set__(self, None)
                return None
            if name == '_db':
                _db_s.__set__(self, None)
                return None
            if name == '_schema':
                s = type(self).__dict__.get(_SCHEMA_ATTR) or get_class_schema(type(self))
                _schema_s.__set__(self, s)
                return s
            if name == '_origin_hints':
                s = type(self).__dict__.get(_SCHEMA_ATTR) or get_class_schema(type(self))
                h = s.origin_hints
                _hints_s.__set__(self, h)
                return h
            raise AttributeError(name)

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
                    # Self-referential: walk MRO to handle DB-variant classes (_XyzDB)
                    # whose __name__ differs from the original user class name.
                    ref_cls = Feature  # fallback
                    for base in type(self).__mro__:
                        if base.__name__ == fwd_name:
                            ref_cls = base
                            break
                    else:
                        import sys
                        mod = sys.modules.get(type(self).__module__, None)
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
        # Pure Python mode: direct cache write, no origin check.
        # Internal slot writes must use module-level slot descriptors (e.g. _origin_s.__set__)
        # or object.__setattr__ — NOT this method.
        # DB-mapped features use _FeatureDBMixin.__setattr__ (class is swapped in map_from).
        self._cache[name] = value

    def _db_setattr(self, name: str, value):
        """Full DB-mapped __setattr__ logic; used by _FeatureDBMixin.__setattr__."""
        # This function is called via the module-level _feature_db_setattr to avoid
        # bound method creation overhead in the DB-mapped hot path.
        # Database-mapped path: resolve field metadata.
        defn = self._origin_hints.get(name)

        # Unknown or non-fastdb-mapped fields are kept in local cache.
        if defn is None or defn[0] is OriginFieldType.unknown:
            self._cache[name] = value
            return

        ft, fid = defn

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


# Module-level slot descriptor references for Feature's 5 private attrs.
# Used in Feature.__init__ for faster slot writes (~1.22× vs object.__setattr__).
_cache_s = Feature.__dict__['_cache']
_origin_s = Feature.__dict__['_origin']
_db_s = Feature.__dict__['_db']
_schema_s = Feature.__dict__['_schema']
_hints_s = Feature.__dict__['_origin_hints']

# Module-level function for DB-mapped __setattr__ — avoids bound-method creation
# overhead when called from _FeatureDBMixin.__setattr__.
def _feature_db_setattr(self, name: str, value):
    Feature._db_setattr(self, name, value)


# -------------------------------------------------------------------
# _FeatureDBMixin: installed on DB-mapped feature instances via
# __class__ swap in map_from(). Provides full DB-aware __setattr__.
# -------------------------------------------------------------------
class _FeatureDBMixin:
    """Mixin that overrides __setattr__ with DB-aware dispatch.

    Instances of user Feature subclasses have their __class__ set to
    a dynamically-created subclass that includes this mixin, so the
    pure-Python Feature.__setattr__ (fast, cache-only) is bypassed.
    """
    __slots__ = ()

    def __setattr__(self, name: str, value):
        _feature_db_setattr(self, name, value)


# Per-class cache: maps user Feature subclass → its _FeatureDB variant.
_DB_CLS_CACHE: dict = {}


def _get_db_cls(cls):
    """Return (or create) the DB-mapped variant of a Feature subclass."""
    db_cls = _DB_CLS_CACHE.get(cls)
    if db_cls is None:
        db_cls = type(f'_{cls.__name__}DB', (_FeatureDBMixin, cls), {'__slots__': ()})
        # Copy parent schema to the DB class so schema/hint lookups in __getattr__
        # and _db_setattr resolve field definitions correctly.
        parent_schema = cls.__dict__.get(_SCHEMA_ATTR) or get_class_schema(cls)
        try:
            setattr(db_cls, _SCHEMA_ATTR, parent_schema)
        except (TypeError, AttributeError):
            pass
        _DB_CLS_CACHE[cls] = db_cls
    return db_cls
