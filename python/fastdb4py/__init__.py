"""fastdb4py — high-performance columnar storage with dual-engine architecture."""
from .type import (
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
from .schema import (
    SCHEMA_VERSION, canonical_schema_json, columnar_capability,
    codec_ref_for_feature, export_schema, object_graph_capability, schema_sha256,
)
from .c_two_provider import CTwoFastdbCodecProvider, FastdbCodecProvider, install_c_two_provider
from .string_column import StringColumn, pack_utf8_column

__all__ = [
    'feature', 'is_feature', 'get_schema', 'lookup_class',
    'Layout', 'ColumnEngine', 'ObjectEngine', 'Table', 'StringColumn',
    'FastSerializer', 'pack_utf8_column',
    'SCHEMA_VERSION', 'canonical_schema_json', 'columnar_capability',
    'codec_ref_for_feature', 'export_schema', 'object_graph_capability',
    'schema_sha256', 'CTwoFastdbCodecProvider', 'FastdbCodecProvider', 'install_c_two_provider',
    'BOOL', 'U8', 'U16', 'U32', 'I32', 'U8N', 'U16N',
    'F32', 'F64', 'STR', 'WSTR', 'REF', 'BYTES',
]
