from enum import unique, IntEnum
from dataclasses import dataclass
from typing import TYPE_CHECKING, Dict, NewType, get_type_hints, get_origin, get_args

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
    # Assume any remaining class is a Feature subclass → ref
    if isinstance(elem, type):
        return OriginFieldType.ref
    return OriginFieldType.unknown
