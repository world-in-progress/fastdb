from threading import Lock
from weakref import WeakKeyDictionary
from typing import Dict, Any, List, Tuple, Type, get_type_hints

import numpy as _np

from ..type import OriginFieldType, get_origin_type, get_list_element_type, LIST_ELEM_DTYPE, LIST_ELEM_CPP_TYPE, LIST_ELEM_ARRAY_TYPECODE
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
        'list_element_types',    # Dict[str, OriginFieldType] — list field name → element type
        'has_ref_fields',        # bool — True if any field is ref or list-of-ref (needs DFS)
        'numeric_plan',          # List[(idx, fn)] for numeric scalar fields (set_field)
        'str_plan',              # List[(idx, fn, bool)] for str fields (bool=is_wide)
        'bytes_plan',            # List[(idx, fn)] for bytes fields
        'list_plan',             # List[(idx, fn, typecode)] for list fields
        'push_fn',               # Compiled per-class push function (avoids loops/conditionals)
    )

    def __init__(
        self,
        hints: Dict[str, Any],
        origin_hints: Dict[str, tuple],
        ordered_defns: List[Tuple[str, OriginFieldType]],
        field_index_map: Dict[str, int],
        scalar_field_ids_np,
        list_element_types: Dict[str, 'OriginFieldType'] = None,
    ):
        self.hints = hints
        self.origin_hints = origin_hints
        self.ordered_defns = ordered_defns
        self.field_index_map = field_index_map
        self.column_accessor_class = None  # lazily populated by table.py
        self.scalar_field_ids_np = scalar_field_ids_np
        self.list_element_types = list_element_types if list_element_types is not None else {}
        self.has_ref_fields = (
            any(ft == OriginFieldType.ref for _, ft in ordered_defns) or
            any(et == OriginFieldType.ref for et in self.list_element_types.values())
        )
        # Pre-split write plans to eliminate per-field if/elif in hot push path
        _numeric_types = frozenset((
            OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
            OriginFieldType.i32, OriginFieldType.f32, OriginFieldType.f64,
            OriginFieldType.u8n, OriginFieldType.u16n,
        ))
        _num = []
        _str = []
        _byt = []
        _lst = []
        for i, (fn, ft) in enumerate(ordered_defns):
            if ft in _numeric_types:
                _num.append((i, fn))
            elif ft == OriginFieldType.str:
                _str.append((i, fn, False))
            elif ft == OriginFieldType.wstr:
                _str.append((i, fn, True))
            elif ft == OriginFieldType.bytes:
                _byt.append((i, fn))
            elif ft == OriginFieldType.list:
                et = self.list_element_types.get(fn)
                typecode = LIST_ELEM_ARRAY_TYPECODE.get(et, 'd')
                _lst.append((i, fn, typecode))
        self.numeric_plan = _num
        self.str_plan = _str
        self.bytes_plan = _byt
        self.list_plan = _lst
        # Generate a specialized push function for this class to avoid loops/conditionals
        self.push_fn = _compile_push_fn(_num, _str, _byt, _lst)


def _compile_push_fn(numeric_plan, str_plan, bytes_plan, list_plan):
    """Generate and compile a specialized per-class push function.

    The compiled function signature is:
        push_fn(cache, t, _struct_pack, _get_struct_fmt) -> None

    Avoids per-field if/elif dispatch and loop overhead in the hot push path.
    """
    import struct as _s
    lines = ['def _push(cache, t, _struct_pack, _get_struct_fmt):']
    lines.append('    t.add_feature_begin()')
    for idx, fn in numeric_plan:
        lines.append(f'    _v = cache.get({fn!r})')
        lines.append(f'    t.set_field({idx}, _v if _v is not None else 0)')
    for idx, fn, is_wide in str_plan:
        lines.append(f'    _v = cache.get({fn!r}) or ""')
        if is_wide:
            lines.append(f'    t.set_field_wstring({idx}, _v)')
        else:
            lines.append(f'    t.set_field_cstring({idx}, _v)')
    for idx, fn in bytes_plan:
        lines.append(f'    t.set_geometry_raw(cache.get({fn!r}) or b"")')
    for idx, fn, typecode in list_plan:
        lines.append(f'    _items = cache.get({fn!r}) or []')
        lines.append(f'    _n = len(_items)')
        lines.append(f'    t.set_field_list_numeric({idx}, _struct_pack(_get_struct_fmt({typecode!r}, _n), *_items))')
    lines.append('    t.add_feature_end()')
    src = '\n'.join(lines)
    ns = {}
    exec(compile(src, '<push_fn>', 'exec'), ns)
    return ns['_push']


def get_class_schema(cls: Type) -> ClassSchema:
    """Return the ClassSchema for the given Feature subclass, computing it on first call.

    Two-level cache:
    1. Class-level attribute (cls.__dict__.get): ~40-50 ns — no WeakRef overhead.
       Safe under free-threaded Python because CPython protects type.__dict__
       with a per-type lock.
    2. WeakKeyDictionary fallback: ~209 ns — used for metaclass-protected types that
       rejected setattr.  All WeakKeyDictionary access is under _SCHEMA_LOCK
       to be safe under free-threaded Python (PEP 703).
    """
    # Hot path: class owns the schema as a plain class attribute (~40-50 ns).
    # cls.__dict__ access is safe under free-threaded CPython (per-type critical section).
    schema = cls.__dict__.get(_SCHEMA_ATTR)
    if schema is not None:
        return schema

    with _SCHEMA_LOCK:
        # Re-check class attribute under lock (another thread may have populated it).
        schema = cls.__dict__.get(_SCHEMA_ATTR)
        if schema is not None:
            return schema

        # WeakKeyDict access is only done under lock for free-threading safety.
        schema = _SCHEMA_CACHE.get(cls)
        if schema is not None:
            return schema

        try:
            hints = get_type_hints(cls)
        except NameError:
            # Forward references that cannot be resolved (e.g. locally defined classes).
            # Fall back to raw annotations so the schema is still usable.
            hints = dict(getattr(cls, '__annotations__', {}))

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

        # list_element_types: field_name → element OriginFieldType for List[X] fields
        list_element_types = {
            name: get_list_element_type(hint)
            for name, hint in hints.items()
            if not name.startswith('_') and get_list_element_type(hint) != OriginFieldType.unknown
        }

        schema = ClassSchema(hints, origin_hints, ordered_defns, field_index_map, scalar_field_ids_np, list_element_types)

        # Primary cache: store directly on the class — O(1) dict lookup next time.
        try:
            setattr(cls, _SCHEMA_ATTR, schema)
        except (TypeError, AttributeError):
            pass  # metaclass-protected class: WeakKeyDict only
        _SCHEMA_CACHE[cls] = schema  # keep WeakKeyDict in sync as GC-safe fallback
        return schema
