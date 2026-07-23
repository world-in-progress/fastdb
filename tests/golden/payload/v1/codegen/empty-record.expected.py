# generated-by: fastdb.payload.codegen.v1
# payload-sha256: 92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71
# core-abi-version: 1
# generator-version: fastdb.payload.codegen.v1
# target: python
from __future__ import annotations

from dataclasses import dataclass

from fastdb4py.payload import (
    Builder,
    CompiledSpec,
    GraphIdentity,
    ObjectHandle,
    Payload,
    PayloadError,
    View,
)

CANONICAL_SOURCE = b'{"components":[],"entries":[],"profile":"record.v1","schema":"fastdb.payload.v1"}'
PAYLOAD_SHA256 = '92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71'
PAYLOAD_SHA256_BYTES = bytes([146, 251, 188, 101, 252, 121, 173, 156, 166, 233, 99, 112, 99, 200, 241, 90, 128, 107, 64, 231, 136, 83, 34, 37, 202, 254, 37, 239, 23, 205, 252, 113])

def compile_spec() -> CompiledSpec:
    return CompiledSpec.compile(CANONICAL_SOURCE)

@dataclass(frozen=True)
class IdMetadata:
    symbol: str
    original_id: str
    stable_index: int

ENTRIES: tuple[IdMetadata, ...] = (
)

COMPONENTS: tuple[IdMetadata, ...] = (
)

@dataclass(frozen=True)
class FieldMetadata:
    symbol: str
    original_id: str
    component_index: int
    field_index: int

FIELDS: tuple[FieldMetadata, ...] = (
)
