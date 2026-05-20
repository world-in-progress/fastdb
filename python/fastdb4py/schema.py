from __future__ import annotations

import hashlib
import json
from typing import Any, ForwardRef, get_args, get_origin

from .registry import FieldDef, get_schema, is_feature, raw_payload_storage_diagnostics
from .type import BOOL, OriginFieldType, get_origin_type, native_list_storage_diagnostic

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
            _field_descriptor(
                field,
                annotation=schema.hints.get(field.name),
                strict=strict,
            )
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


def feature_schema_dependencies(feature_cls: type) -> tuple[dict[str, Any], ...]:
    _ensure_feature(feature_cls)
    dependencies: dict[str, dict[str, Any]] = {}
    visiting: set[type] = set()

    def visit(current: type) -> None:
        if current in visiting:
            return
        visiting.add(current)
        schema = get_schema(current)
        for annotation in schema.hints.values():
            visit_annotation_targets(annotation)
        for field in schema.ref_fields:
            visit_target(field.ref_target)
        for field in schema.list_ref_fields:
            visit_target(field.list_ref_target)
        visiting.remove(current)

    def visit_annotation_targets(annotation: object) -> None:
        if get_origin(annotation) is list:
            item = _list_item_annotation(annotation)
            if item is not None:
                visit_annotation_targets(item)
            return
        if isinstance(annotation, type) and is_feature(annotation):
            visit_target(annotation)

    def visit_target(target: type | None) -> None:
        if target is None or not is_feature(target):
            return
        descriptor = export_schema(target)
        identity = descriptor['feature']['identity']
        if identity not in dependencies:
            dependencies[identity] = descriptor
            visit(target)

    visit(feature_cls)
    root_identity = export_schema(feature_cls)['feature']['identity']
    dependencies.pop(root_identity, None)
    return tuple(
        dependencies[identity]
        for identity in sorted(dependencies)
    )


def columnar_capability(
    feature_cls: type,
    *,
    fixed_table: bool = False,
) -> dict[str, Any]:
    _ensure_feature(feature_cls)
    schema = get_schema(feature_cls)
    unsupported_fields: list[str] = []
    diagnostics: list[str] = []
    raw_payload_diagnostics = raw_payload_storage_diagnostics(schema)
    if raw_payload_diagnostics:
        unsupported_fields.extend(
            field.name
            for field in schema.fields
            if field.field_type == OriginFieldType.bytes
        )
        diagnostics.extend(raw_payload_diagnostics)
    for field in schema.fields:
        if field.field_type == OriginFieldType.ref:
            unsupported_fields.append(field.name)
            diagnostics.append(f'{field.name}: ref requires object_graph.v1')
        elif field.field_type == OriginFieldType.list and field.list_elem_type == OriginFieldType.ref:
            unsupported_fields.append(field.name)
            diagnostics.append(f'{field.name}: list[ref] requires object_graph.v1')
        elif field.field_type == OriginFieldType.list:
            diagnostic = _non_native_list_storage_diagnostic(field)
            if diagnostic is not None:
                unsupported_fields.append(field.name)
                diagnostics.append(diagnostic)
        elif fixed_table and field.field_type in {OriginFieldType.bytes, OriginFieldType.wstr}:
            if field.name not in unsupported_fields:
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
    schema = get_schema(feature_cls)
    unsupported_fields: list[str] = []
    diagnostics: list[str] = []
    raw_payload_diagnostics = raw_payload_storage_diagnostics(schema)
    if raw_payload_diagnostics:
        unsupported_fields.extend(
            field.name
            for field in schema.fields
            if field.field_type == OriginFieldType.bytes
        )
        diagnostics.extend(raw_payload_diagnostics)
    try:
        export_schema(feature_cls, strict=True)
    except TypeError as exc:
        diagnostics.append(str(exc))
    for field in schema.fields:
        if field.field_type != OriginFieldType.list:
            continue
        if field.list_elem_type == OriginFieldType.ref:
            continue
        diagnostic = _non_native_list_storage_diagnostic(field)
        if diagnostic is not None:
            unsupported_fields.append(field.name)
            diagnostics.append(diagnostic)
    return {
        'diagnostics': diagnostics,
        'eligible': not diagnostics,
        'profile': 'object_graph.v1',
        'schema': SCHEMA_VERSION,
        'schema_sha256': schema_sha256(feature_cls) if not diagnostics else None,
        'unsupported_fields': unsupported_fields,
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


def _field_descriptor(
    field: FieldDef,
    *,
    annotation: object,
    strict: bool,
) -> dict[str, Any]:
    if field.field_type == OriginFieldType.ref:
        return {
            'kind': 'ref',
            'name': field.name,
            'target': _ref_target_identity(field.name, field.ref_target, strict=strict),
        }
    if field.field_type == OriginFieldType.list:
        return {
            'item': _list_item_descriptor(
                field,
                annotation=annotation,
                strict=strict,
            ),
            'kind': 'list',
            'name': field.name,
        }
    kind = _field_kind(field.field_type, annotation)
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


def _non_native_list_storage_diagnostic(field: FieldDef) -> str | None:
    return native_list_storage_diagnostic(field.name, field.list_elem_type)


def _list_item_descriptor(
    field: FieldDef,
    *,
    annotation: object,
    strict: bool,
) -> dict[str, Any]:
    item_annotation = _list_item_annotation(annotation)
    if field.list_elem_type == OriginFieldType.list:
        return _list_item_descriptor_from_annotation(
            field.name,
            item_annotation,
            strict=strict,
        )
    if field.list_elem_type == OriginFieldType.ref:
        return {
            'kind': 'ref',
            'target': _ref_target_identity(
                field.name,
                _ref_target_from_annotation(item_annotation),
                strict=strict,
            ),
        }
    kind = _field_kind(field.list_elem_type, item_annotation)
    if kind is None:
        if strict:
            raise TypeError(
                f'{field.name}: unsupported list element type '
                f'{field.list_elem_type!r} for portable fastdb.schema.v1 export.',
            )
        kind = 'unknown'
    return {'kind': kind}


def _list_item_descriptor_from_annotation(
    field_name: str,
    annotation: object,
    *,
    strict: bool,
) -> dict[str, Any]:
    if get_origin(annotation) is list:
        return {
            'item': _list_item_descriptor_from_annotation(
                field_name,
                _list_item_annotation(annotation),
                strict=strict,
            ),
            'kind': 'list',
        }
    if isinstance(annotation, (str, ForwardRef)):
        return {
            'kind': 'ref',
            'target': _ref_target_identity(field_name, None, strict=strict),
        }
    if isinstance(annotation, type):
        if is_feature(annotation):
            return {
                'kind': 'ref',
                'target': _ref_target_identity(field_name, annotation, strict=strict),
            }
        if not issubclass(annotation, (int, float, str, bytes, bool)):
            return {
                'kind': 'ref',
                'target': _ref_target_identity(field_name, annotation, strict=strict),
            }
    kind = _field_kind(get_origin_type(annotation), annotation)
    if kind is None:
        if strict:
            raise TypeError(
                f'{field_name}: unsupported nested list element annotation '
                f'{annotation!r} for portable fastdb.schema.v1 export.',
            )
        kind = 'unknown'
    return {'kind': kind}


def _field_kind(field_type: OriginFieldType | None, annotation: object) -> str | None:
    if annotation is BOOL:
        return 'bool'
    return _KIND_BY_FIELD_TYPE.get(field_type)


def _list_item_annotation(annotation: object) -> object:
    if get_origin(annotation) is not list:
        return None
    args = get_args(annotation)
    return args[0] if args else None


def _ref_target_from_annotation(annotation: object) -> type | None:
    if isinstance(annotation, type):
        return annotation
    return None


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
