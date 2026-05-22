"""fastdb4py — high-performance columnar storage with dual-engine architecture."""
from .type import (
    Array, Batch,
    BOOL, U8, U16, U32, I32, U8N, U16N,
    F32, F64, STR, WSTR, REF, BYTES
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

__all__ = [
    'feature', 'is_feature', 'get_schema', 'lookup_class',
    'Layout', 'ColumnEngine', 'ObjectEngine', 'Table', 'StringColumn',
    'FastSerializer', 'materialize', 'pack_utf8_column',
    'FdbViewInvalidatedError', 'FdbViewOwner', 'FdbViewWriteError', 'invalidate',
    'SCHEMA_VERSION', 'canonical_schema_json', 'columnar_capability',
    'codec_ref_for_feature', 'export_schema', 'object_graph_capability',
    'schema_sha256',
    'Array', 'Batch',
    'BOOL', 'U8', 'U16', 'U32', 'I32', 'U8N', 'U16N',
    'F32', 'F64', 'STR', 'WSTR', 'REF', 'BYTES',
]
