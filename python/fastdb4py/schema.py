from __future__ import annotations

import hashlib
import json
from typing import Any

from .registry import FieldDef, get_schema, is_feature
from .type import OriginFieldType

SCHEMA_VERSION = 'fastdb.schema.v1'

_KIND_BY_FIELD_TYPE = {
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
}


def export_schema(feature_cls: type, *, strict: bool = True) -> dict[str, Any]:
    if not is_feature(feature_cls):
        raise TypeError(f'{feature_cls!r} is not an explicit @feature class.')
    schema = get_schema(feature_cls)
    return {
        'feature': _feature_identity(feature_cls),
        'fields': [
            _field_descriptor(field, strict=strict)
            for field in schema.fields
        ],
        'schema': SCHEMA_VERSION,
    }


def canonical_schema_json(feature_or_descriptor: type | dict[str, Any]) -> str:
    descriptor = (
        export_schema(feature_or_descriptor)
        if isinstance(feature_or_descriptor, type)
        else feature_or_descriptor
    )
    return json.dumps(descriptor, sort_keys=True, separators=(',', ':'))


def schema_sha256(feature_or_descriptor: type | dict[str, Any]) -> str:
    return hashlib.sha256(
        canonical_schema_json(feature_or_descriptor).encode(),
    ).hexdigest()


def columnar_capability(
    feature_cls: type,
    *,
    fixed_table: bool = False,
) -> dict[str, Any]:
    _ensure_feature(feature_cls)
    schema = get_schema(feature_cls)
    unsupported_fields: list[str] = []
    diagnostics: list[str] = []
    for field in schema.fields:
        if field.field_type == OriginFieldType.ref:
            unsupported_fields.append(field.name)
            diagnostics.append(f'{field.name}: ref requires object_graph.v1')
        elif field.field_type == OriginFieldType.list and field.list_elem_type == OriginFieldType.ref:
            unsupported_fields.append(field.name)
            diagnostics.append(f'{field.name}: list[ref] requires object_graph.v1')
        elif fixed_table and field.field_type in {OriginFieldType.bytes, OriginFieldType.wstr}:
            unsupported_fields.append(field.name)
            diagnostics.append(
                f'{field.name}: {field.field_type.name} is not supported by '
                'fixed-table columnar layout',
            )
    return {
        'diagnostics': diagnostics,
        'eligible': not diagnostics,
        'profile': 'columnar.v1',
        'schema': SCHEMA_VERSION,
        'schema_sha256': schema_sha256(feature_cls),
        'unsupported_fields': unsupported_fields,
    }


def object_graph_capability(feature_cls: type) -> dict[str, Any]:
    _ensure_feature(feature_cls)
    diagnostics: list[str] = []
    try:
        export_schema(feature_cls, strict=True)
    except TypeError as exc:
        diagnostics.append(str(exc))
    return {
        'diagnostics': diagnostics,
        'eligible': not diagnostics,
        'profile': 'object_graph.v1',
        'schema': SCHEMA_VERSION,
        'schema_sha256': schema_sha256(feature_cls) if not diagnostics else None,
        'unsupported_fields': [],
    }


def codec_ref_for_feature(feature_cls: type, *, profile: str) -> dict[str, Any]:
    _ensure_feature(feature_cls)
    if profile == 'columnar':
        capability = columnar_capability(feature_cls)
        if not capability['eligible']:
            raise TypeError(
                f'{feature_cls.__name__} is not eligible for columnar codec: '
                f'{capability["diagnostics"]}',
            )
        codec_id = 'org.fastdb.columnar'
        capabilities = ['bytes', 'buffer-view']
    elif profile in {'object_graph', 'object-graph'}:
        capability = object_graph_capability(feature_cls)
        if not capability['eligible']:
            raise TypeError(
                f'{feature_cls.__name__} is not eligible for object graph codec: '
                f'{capability["diagnostics"]}',
            )
        codec_id = 'org.fastdb.object-graph'
        capabilities = ['bytes']
    else:
        raise ValueError("profile must be 'columnar' or 'object_graph'.")
    return {
        'capabilities': capabilities,
        'id': codec_id,
        'kind': 'codec_ref',
        'portable': True,
        'schema': SCHEMA_VERSION,
        'schema_sha256': capability['schema_sha256'],
        'version': '1',
    }


def _field_descriptor(field: FieldDef, *, strict: bool) -> dict[str, Any]:
    if field.field_type == OriginFieldType.ref:
        return {
            'kind': 'ref',
            'name': field.name,
            'target': _ref_target_identity(field.name, field.ref_target, strict=strict),
        }
    if field.field_type == OriginFieldType.list:
        return {
            'item': _list_item_descriptor(field, strict=strict),
            'kind': 'list',
            'name': field.name,
        }
    kind = _KIND_BY_FIELD_TYPE.get(field.field_type)
    if kind is None:
        if strict:
            raise TypeError(
                f'{field.name}: unsupported field type {field.field_type!r} '
                'for portable fastdb.schema.v1 export.',
            )
        kind = 'unknown'
    return {
        'kind': kind,
        'name': field.name,
    }


def _ensure_feature(feature_cls: type) -> None:
    if not is_feature(feature_cls):
        raise TypeError(f'{feature_cls!r} is not an explicit @feature class.')


def _list_item_descriptor(field: FieldDef, *, strict: bool) -> dict[str, Any]:
    if field.list_elem_type == OriginFieldType.ref:
        return {
            'kind': 'ref',
            'target': _ref_target_identity(
                field.name,
                field.list_ref_target,
                strict=strict,
            ),
        }
    kind = _KIND_BY_FIELD_TYPE.get(field.list_elem_type)
    if kind is None:
        if strict:
            raise TypeError(
                f'{field.name}: unsupported list element type '
                f'{field.list_elem_type!r} for portable fastdb.schema.v1 export.',
            )
        kind = 'unknown'
    return {'kind': kind}


def _ref_target_identity(
    field_name: str,
    target: type | None,
    *,
    strict: bool,
) -> dict[str, str] | None:
    if target is None:
        if strict:
            raise TypeError(
                f'{field_name}: unresolved ref target; portable export '
                'requires explicit @feature targets.',
            )
        return None
    if not is_feature(target):
        if strict:
            raise TypeError(
                f'{field_name}: ref target {target.__name__} is not an '
                'explicit @feature class.',
            )
        return _feature_identity(target)
    return _feature_identity(target)


def _feature_identity(feature_cls: type) -> dict[str, str]:
    return {
        'identity': f'{feature_cls.__module__}:{feature_cls.__qualname__}',
        'module': feature_cls.__module__,
        'name': feature_cls.__name__,
        'qualname': feature_cls.__qualname__,
    }
