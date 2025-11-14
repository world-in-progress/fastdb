from enum import unique, IntEnum
from dataclasses import dataclass
from typing import Dict, TypeVar, get_type_hints, get_origin, get_args

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
