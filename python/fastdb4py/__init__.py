"""fastdb4py — high-performance columnar storage with dual-engine architecture."""
from .type import (
    Array, ArrayRequirement, Batch, BatchRequirement, array, batch,
    BOOL, U8, U16, U32, I32, U8N, U16N,
    F32, F64, STR, WSTR, REF, BYTES
)
from .require import require
from .allocator import (
    BytearrayAllocation, BytearrayAllocator, FinalBackingAllocation,
    FinalBackingResource, HeapFinalBackingResource, HeapScratchAllocator,
    ScratchAllocation, ScratchAllocator, build_call_db,
)
from .decorator import feature
from .registry import is_feature, get_schema, lookup_class
from .layout import Layout
from .column_engine import ColumnEngine
from .object_engine import ObjectEngine
from .orm.table import Table
from .serializer import FastSerializer
from .materialize import materialize
from .view_owner import (
    FdbViewInvalidatedError, FdbViewOwner, FdbViewWriteError, invalidate,
)
from .schema import (
    SCHEMA_VERSION, canonical_schema_json, columnar_capability,
    codec_ref_for_feature, export_schema, object_graph_capability, schema_sha256,
)
from .string_column import StringColumn, pack_utf8_column
from .call_db import (
    CALL_DB_CODEC_ID, CALL_DB_COLUMNAR_PROFILE, CALL_DB_OBJECT_GRAPH_PROFILE,
    CALL_DB_SCHEMA_VERSION, FastdbCallDbArrayItem, FastdbCallDbArrayView,
    FastdbCallDbBinding, FastdbCallDbFeatureDependency,
    FastdbCallDbScalarField, FastdbCallDbTable, FastdbCallDbView,
    FastdbPreparedCallDb, FastdbUnsupportedDirectBuildError, decode_call_db,
    call_db_build_context, encode_call_db, encode_call_db_into, prepare_call_db,
    try_export_call_db, view_call_db,
)

__all__ = [
    'feature', 'is_feature', 'get_schema', 'lookup_class',
    'Layout', 'ColumnEngine', 'ObjectEngine', 'Table', 'StringColumn',
    'FastSerializer', 'materialize', 'pack_utf8_column',
    'FdbViewInvalidatedError', 'FdbViewOwner', 'FdbViewWriteError', 'invalidate',
    'SCHEMA_VERSION', 'canonical_schema_json', 'columnar_capability',
    'codec_ref_for_feature', 'export_schema', 'object_graph_capability',
    'schema_sha256',
    'CALL_DB_CODEC_ID', 'CALL_DB_COLUMNAR_PROFILE', 'CALL_DB_OBJECT_GRAPH_PROFILE',
    'CALL_DB_SCHEMA_VERSION', 'FastdbCallDbArrayItem', 'FastdbCallDbArrayView',
    'FastdbCallDbBinding', 'FastdbCallDbFeatureDependency', 'FastdbCallDbScalarField',
    'FastdbCallDbTable', 'FastdbCallDbView', 'FastdbPreparedCallDb',
    'FastdbUnsupportedDirectBuildError', 'call_db_build_context', 'decode_call_db', 'encode_call_db',
    'encode_call_db_into', 'prepare_call_db', 'try_export_call_db', 'view_call_db',
    'Array', 'ArrayRequirement', 'Batch', 'BatchRequirement', 'array', 'batch',
    'require',
    'BytearrayAllocation', 'BytearrayAllocator', 'FinalBackingAllocation',
    'FinalBackingResource', 'HeapFinalBackingResource', 'HeapScratchAllocator',
    'ScratchAllocation', 'ScratchAllocator', 'build_call_db',
    'BOOL', 'U8', 'U16', 'U32', 'I32', 'U8N', 'U16N',
    'F32', 'F64', 'STR', 'WSTR', 'REF', 'BYTES',
]
