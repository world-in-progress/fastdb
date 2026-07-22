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
