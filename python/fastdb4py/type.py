from enum import unique, IntEnum
from dataclasses import dataclass
from typing import TYPE_CHECKING, Dict, Generic, NewType, TypeVar, get_type_hints, get_origin, get_args

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


class Array(Generic[T]):
    """CRM ABI marker for a homogeneous fastdb scalar array."""


class Batch(Generic[T]):
    """CRM ABI marker for a table-shaped batch of fastdb features."""

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
