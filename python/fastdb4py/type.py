from enum import unique, IntEnum
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Dict, Generic, NewType, TypeVar, get_type_hints, get_origin, get_args

T = TypeVar('T')

# A map of the enum type used in the core fastdb library
@unique
class OriginFieldType(IntEnum):
    unknown     = 0
    u8          = 1
    u16         = 2
    u32         = 3
    i32         = 4
    u8n         = 5
    u16n        = 6
    f32         = 7
    f64         = 8
    str         = 9
    wstr        = 10
    ref         = 11
    bytes       = 12
    list        = 13
    
    def __repr__(self):
        return self.name

# Not used currently
@dataclass
class OriginFieldDefinition:
    name: str
    type: OriginFieldType
    vmin: float = 0.0
    vmax: float = 1.0


@dataclass(frozen=True)
class BatchRequirement(Generic[T]):
    feature_type: type[T]
    rows: int
    profile: str = 'auto'


@dataclass(frozen=True)
class ArrayRequirement(Generic[T]):
    item_type: object
    rows: int


def batch(feature_type: type[T], *, rows: int, profile: str = 'auto') -> BatchRequirement[T]:
    _validate_requirement_rows(rows)
    from .registry import is_feature

    if not is_feature(feature_type):
        raise TypeError(
            f'{getattr(feature_type, "__name__", feature_type)!r} is not a fastdb @feature class.',
        )
    return BatchRequirement(feature_type=feature_type, rows=rows, profile=profile)


def array(item_type: object, *, rows: int) -> ArrayRequirement[Any]:
    _validate_requirement_rows(rows)
    field_type = get_origin_type(item_type)
    if field_type in {
        OriginFieldType.unknown,
        OriginFieldType.ref,
        OriginFieldType.list,
    }:
        raise TypeError(
            f'{item_type!r} is not a supported fastdb scalar alias for ArrayRequirement.',
        )
    return ArrayRequirement(item_type=item_type, rows=rows)


def _validate_requirement_rows(rows: int) -> None:
    if type(rows) is not int or rows < 0:
        raise ValueError('rows must be a non-negative integer.')


class Array(Generic[T]):
    """Logical homogeneous FastDB scalar array.

    ``Array[T]`` is both the public CRM ABI marker and a small runtime value for
    authoring call payloads without touching the physical table representation.
    """

    def __init__(self, item_type: object, values: list[Any] | None = None, *, capacity: int | None = None):
        self.item_type = item_type
        self._values = list(values or [])
        self._capacity = capacity
        self._fastdb_require_envelope = None
        self._fastdb_require_index = None

    @classmethod
    def allocate(cls, item_type: object, capacity: int) -> 'Array':
        if type(capacity) is not int or capacity < 0:
            raise ValueError('Array.allocate capacity must be a non-negative integer.')
        return cls(item_type, capacity=capacity)

    def fill(self, values: object) -> None:
        if isinstance(values, (str, bytes, bytearray, memoryview)):
            raise TypeError('Array.fill expects an iterable of scalar values.')
        try:
            new_values = list(values)  # type: ignore[arg-type]
        except TypeError as exc:
            raise TypeError('Array.fill expects an iterable of scalar values.') from exc
        if self._capacity is not None and len(new_values) != self._capacity:
            raise ValueError(f'Array.fill expected {self._capacity} values, got {len(new_values)}.')
        self._values = new_values

    def append(self, value: object) -> None:
        if self._capacity is not None and len(self._values) >= self._capacity:
            raise ValueError('Array capacity exceeded.')
        self._values.append(value)

    def extend(self, values: object) -> None:
        for value in values:  # type: ignore[union-attr]
            self.append(value)

    def to_owned(self) -> list[Any]:
        from .materialize import materialize

        return [materialize(item) for item in self._values]

    def materialize(self) -> list[Any]:
        return self.to_owned()

    def __len__(self) -> int:
        return len(self._values)

    def __getitem__(self, index: int | slice) -> Any:
        return self._values[index]

    def __iter__(self):
        return iter(self._values)


class Batch(Generic[T]):
    """Logical batch of FastDB features.

    ``Batch[T]`` is the public authoring/runtime surface. Columnar batches own a
    physical ``Table`` internally; object-graph batches keep feature rows until
    the object graph encoder consumes them.
    """

    def __init__(
        self,
        feature_type: type[T],
        *,
        profile: str,
        table: object | None = None,
        engine: object | None = None,
        rows: list[T] | None = None,
        capacity: int | None = None,
    ):
        self.feature_type = feature_type
        self.profile = profile
        self._table = table
        self._engine = engine
        self._rows = list(rows or [])
        self._capacity = capacity
        self._fastdb_require_envelope = None
        self._fastdb_require_index = None

    @classmethod
    def allocate(cls, feature_type: type[T], capacity: int, *, profile: str = 'auto') -> 'Batch[T]':
        if type(capacity) is not int or capacity < 0:
            raise ValueError('Batch.allocate capacity must be a non-negative integer.')

        from .column_engine import ColumnEngine
        from .layout import Layout
        from .registry import is_feature
        from .schema import columnar_capability, object_graph_capability

        if not is_feature(feature_type):
            raise TypeError(f'{getattr(feature_type, "__name__", feature_type)!r} is not a fastdb @feature class.')

        normalized = _normalize_batch_profile(profile)
        if normalized == 'auto':
            if columnar_capability(feature_type, fixed_table=True)['eligible']:
                normalized = 'columnar'
            elif object_graph_capability(feature_type)['eligible']:
                normalized = 'object_graph'
            else:
                columnar = columnar_capability(feature_type, fixed_table=True)
                graph = object_graph_capability(feature_type)
                raise TypeError(
                    f'{feature_type.__name__} is not eligible for Batch allocation: '
                    f'columnar={columnar["diagnostics"]}; object_graph={graph["diagnostics"]}',
                )

        if normalized == 'columnar':
            engine = ColumnEngine.truncate([Layout(feature_type, capacity)])
            table = engine.table(feature_type)
            return cls(feature_type, profile='columnar', table=table, engine=engine, capacity=capacity)
        if normalized == 'object_graph':
            capability = object_graph_capability(feature_type)
            if not capability['eligible']:
                raise TypeError(
                    f'{feature_type.__name__} is not eligible for object graph Batch allocation: '
                    f'{capability["diagnostics"]}',
                )
            return cls(feature_type, profile='object_graph', rows=[], capacity=capacity)
        raise ValueError(f'Unsupported Batch profile {profile!r}.')

    @classmethod
    def from_table(cls, table: object) -> 'Batch':
        from .orm.table import Table

        if not isinstance(table, Table):
            raise TypeError(f'Batch.from_table expected a fastdb Table, got {type(table).__name__}.')
        feature_type = table._feature_type  # noqa: SLF001
        if feature_type is None:
            raise TypeError('Batch.from_table requires a Table mapped with a feature type.')
        return cls(feature_type, profile='columnar', table=table)

    @property
    def _fastdb_table(self) -> object:
        if self._table is None:
            raise TypeError('This Batch is not backed by a columnar fastdb Table.')
        return self._table

    @property
    def column(self) -> object:
        return self._fastdb_table.column

    @property
    def name(self) -> str:
        return self._fastdb_table.name

    def fill(self, **columns: object) -> None:
        if self._table is None:
            raise TypeError('Batch.fill is available only for columnar Batch values.')
        self._table.fill(**columns)

    def append(self, value: T) -> None:
        if self._table is not None:
            raise TypeError('Columnar Batch values are fixed-size; use fill(...) for direct column writes.')
        if self._capacity is not None and self._capacity > 0 and len(self._rows) >= self._capacity:
            raise ValueError('Batch capacity exceeded.')
        if not isinstance(value, self.feature_type):
            raise TypeError(f'Batch[{self.feature_type.__name__}] expected {self.feature_type.__name__}, got {type(value).__name__}.')
        self._rows.append(value)

    def extend(self, values: object) -> None:
        for value in values:  # type: ignore[union-attr]
            self.append(value)

    def to_owned(self) -> list[T]:
        from .materialize import materialize

        if self._table is not None:
            return self._table.to_owned()
        return [materialize(row) for row in self._rows]

    def materialize(self) -> list[T]:
        return self.to_owned()

    def __len__(self) -> int:
        if self._table is not None:
            return len(self._table)
        return len(self._rows)

    def __getitem__(self, index: int | slice) -> Any:
        if self._table is not None:
            return self._table[index]
        return self._rows[index]

    def __iter__(self):
        if self._table is not None:
            return iter(self._table)
        return iter(self._rows)


def _normalize_batch_profile(profile: str) -> str:
    if profile in {'auto', None}:
        return 'auto'
    if profile in {'columnar', 'columnar.v1', 'fastdb.call.columnar.v1'}:
        return 'columnar'
    if profile in {'object_graph', 'object-graph', 'object_graph.v1', 'object-graph.v1', 'fastdb.call.object-graph.v1'}:
        return 'object_graph'
    return profile

# Field type aliases for Python-side type annotations.
#
# TYPE_CHECKING branch: plain type aliases so that Pylance/Pyright accepts
# literal assignments like `point.x = 1.0` without complaint.
#
# Runtime branch: NewType objects so each field type is a unique hashable
# object, which FIELD_TYPE_MAP uses to distinguish e.g. F32 from F64.
if TYPE_CHECKING:
    from typing import TypeAlias
    BOOL: TypeAlias = bool
    U8:   TypeAlias = int
    U16:  TypeAlias = int
    U32:  TypeAlias = int
    I32:  TypeAlias = int
    U8N:  TypeAlias = int
    U16N: TypeAlias = int
    F32:  TypeAlias = float
    F64:  TypeAlias = float
    STR:  TypeAlias = str
    WSTR: TypeAlias = str
    REF:  TypeAlias = object
    BYTES: TypeAlias = bytes
else:
    BOOL = NewType('BOOL', bool)
    U8 = NewType('U8', int)
    U16 = NewType('U16', int)
    U32 = NewType('U32', int)
    I32 = NewType('I32', int)
    U8N = NewType('U8N', int)
    U16N = NewType('U16N', int)
    F32 = NewType('F32', float)
    F64 = NewType('F64', float)
    STR = NewType('STR', str)
    WSTR = NewType('WSTR', str)
    REF = NewType('REF', object)
    BYTES = NewType('BYTES', bytes)

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

FIELD_TYPE_FACTORIES = {
    OriginFieldType.u8:    int,
    OriginFieldType.u16:   int,
    OriginFieldType.u32:   int,
    OriginFieldType.i32:   int,
    OriginFieldType.u8n:   int,
    OriginFieldType.u16n:  int,
    OriginFieldType.f32:   float,
    OriginFieldType.f64:   float,
    OriginFieldType.str:   str,
    OriginFieldType.wstr:  str,
    OriginFieldType.bytes: bytes,
    OriginFieldType.list:  list,
}

# Mapping from Python type annotations to OriginFieldType
def get_origin_type(type_var: type) -> OriginFieldType:
    if get_origin(type_var) is list or type_var is list:
        return OriginFieldType.list
    # Native Python built-in types map to their closest fastdb counterparts.
    # Note: Python int is arbitrary precision; i32 is used as a temporary mapping
    # (range ±2^31-1). A future i64 type is tracked in the project issue tracker.
    if type_var is int:
        return OriginFieldType.i32
    if type_var is float:
        return OriginFieldType.f64
    if type_var is str:
        return OriginFieldType.str
    if type_var is bool:
        return OriginFieldType.u8
    return FIELD_TYPE_MAP.get(type_var, OriginFieldType.unknown)

# Mapping of list element OriginFieldType → C++ element_type enum value for add_list_field.
# C++ ftFeatureRef = 11, numeric types match C++ FieldTypeEnum values.
LIST_ELEM_CPP_TYPE = {
    OriginFieldType.u8:  1,
    OriginFieldType.u16: 2,
    OriginFieldType.u32: 3,
    OriginFieldType.i32: 4,
    OriginFieldType.f32: 7,
    OriginFieldType.f64: 8,
    OriginFieldType.ref: 11,
}

# numpy dtype strings for numeric list element types
LIST_ELEM_DTYPE = {
    OriginFieldType.u8:  'uint8',
    OriginFieldType.u16: 'uint16',
    OriginFieldType.u32: 'uint32',
    OriginFieldType.i32: 'int32',
    OriginFieldType.f32: 'float32',
    OriginFieldType.f64: 'float64',
}

# array.array typecodes for numeric list element types (faster than np.asarray for Python lists)
LIST_ELEM_ARRAY_TYPECODE = {
    OriginFieldType.u8:  'B',
    OriginFieldType.u16: 'H',
    OriginFieldType.u32: 'I',
    OriginFieldType.i32: 'i',
    OriginFieldType.f32: 'f',
    OriginFieldType.f64: 'd',
}

NATIVE_LIST_STORAGE_FIELD_TYPES = frozenset(LIST_ELEM_DTYPE)
_TRUE_BOOL_STRINGS = frozenset({'1', 'true', 't', 'yes', 'y', 'on'})
_FALSE_BOOL_STRINGS = frozenset({'0', 'false', 'f', 'no', 'n', 'off'})


def coerce_bool_scalar(value: object) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in _TRUE_BOOL_STRINGS:
            return True
        if normalized in _FALSE_BOOL_STRINGS:
            return False
        raise ValueError(
            f'cannot coerce {value!r} to fastdb bool scalar; expected bool, 0/1, or true/false string.',
        )
    if isinstance(value, (bytes, bytearray, memoryview)):
        try:
            return coerce_bool_scalar(bytes(value).decode('utf-8'))
        except UnicodeDecodeError as exc:
            raise ValueError(
                f'cannot coerce {value!r} to fastdb bool scalar; expected bool, 0/1, or true/false string.',
            ) from exc
    if isinstance(value, int):
        if value in {0, 1}:
            return bool(value)
        raise ValueError(
            f'cannot coerce {value!r} to fastdb bool scalar; expected bool, 0/1, or true/false string.',
        )
    if isinstance(value, float):
        if value in {0.0, 1.0}:
            return bool(value)
        raise ValueError(
            f'cannot coerce {value!r} to fastdb bool scalar; expected bool, 0/1, or true/false string.',
        )
    try:
        if value == 0:
            return False
        if value == 1:
            return True
    except Exception:
        pass
    raise ValueError(
        f'cannot coerce {value!r} to fastdb bool scalar; expected bool, 0/1, or true/false string.',
    )

_SCHEMA_KIND_BY_FIELD_TYPE = {
    OriginFieldType.u8: 'u8',
    OriginFieldType.u16: 'u16',
    OriginFieldType.u32: 'u32',
    OriginFieldType.i32: 'i32',
    OriginFieldType.u8n: 'u8n',
    OriginFieldType.u16n: 'u16n',
    OriginFieldType.f32: 'f32',
    OriginFieldType.f64: 'f64',
    OriginFieldType.str: 'str',
    OriginFieldType.wstr: 'wstr',
    OriginFieldType.bytes: 'bytes',
    OriginFieldType.ref: 'ref',
    OriginFieldType.list: 'list',
    OriginFieldType.unknown: 'unknown',
}


def is_native_list_storage_type(field_type: OriginFieldType | None) -> bool:
    return field_type in NATIVE_LIST_STORAGE_FIELD_TYPES


def native_list_storage_diagnostic(
    field_name: str,
    element_type: OriginFieldType | None,
) -> str | None:
    if is_native_list_storage_type(element_type):
        return None
    kind = _SCHEMA_KIND_BY_FIELD_TYPE.get(element_type, repr(element_type))
    return f'{field_name}: list[{kind}] is not backed by native fixed-width list storage'

def get_list_element_type(annotation) -> OriginFieldType:
    """Return the OriginFieldType of the element type inside a List[X] annotation.

    Returns OriginFieldType.ref for List[SomeFeature] or List['ForwardRef'].
    Returns the scalar OriginFieldType for List[F64] etc.
    Returns OriginFieldType.unknown if not a List or element type is unrecognised.
    """
    import typing
    origin = get_origin(annotation)
    if origin is not list:
        return OriginFieldType.unknown
    args = get_args(annotation)
    if not args:
        return OriginFieldType.unknown
    elem = args[0]
    if elem is list or get_origin(elem) is list:
        return OriginFieldType.list
    # Forward references and Feature subclasses → ref
    if isinstance(elem, str):
        return OriginFieldType.ref
    if isinstance(elem, typing.ForwardRef):
        return OriginFieldType.ref
    # Check scalar map first
    scalar = FIELD_TYPE_MAP.get(elem, OriginFieldType.unknown)
    if scalar != OriginFieldType.unknown:
        return scalar
    # Native built-in scalars
    if elem is float:
        return OriginFieldType.f64
    if elem is int:
        return OriginFieldType.i32
    if elem is bool:
        return OriginFieldType.u8
    # Assume any remaining class is a Feature subclass → ref
    if isinstance(elem, type):
        return OriginFieldType.ref
    return OriginFieldType.unknown
