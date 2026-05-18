from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from fastdb4py.schema import SCHEMA_VERSION, schema_sha256

FASTDB_CODEC_IDS = {
    'org.fastdb.columnar': 'columnar.v1',
    'org.fastdb.object-graph': 'object_graph.v1',
}

_SCALAR_SCHEMA = {
    'u8': ('U8', 'number'),
    'u16': ('U16', 'number'),
    'u32': ('U32', 'number'),
    'i32': ('I32', 'number'),
    'u8n': ('U8N', 'number'),
    'u16n': ('U16N', 'number'),
    'f32': ('F32', 'number'),
    'f64': ('F64', 'number'),
    'str': ('STR', 'string'),
    'wstr': ('WSTR', 'string'),
    'bytes': ('BYTES', 'Uint8Array'),
}


class CTwoCodegenError(Exception):
    pass


def generate_c_two_typescript_helpers(
    contract_descriptor: dict[str, Any] | str | bytes,
    schema_descriptors: list[dict[str, Any] | str | bytes],
) -> str:
    contract = _coerce_json(contract_descriptor, label='contract descriptor')
    schemas = [
        _coerce_json(descriptor, label='fastdb schema descriptor')
        for descriptor in schema_descriptors
    ]
    schemas_by_hash, schemas_by_identity = _index_schemas(schemas)
    requirements = _collect_fastdb_requirements(contract)
    matched: list[tuple[dict[str, Any], dict[str, Any]]] = []
    for codec_ref in requirements:
        digest = codec_ref.get('schema_sha256')
        if not digest:
            raise CTwoCodegenError(f'fastdb codec {codec_ref.get("id")!r} is missing schema_sha256.')
        if codec_ref.get('schema') != SCHEMA_VERSION:
            raise CTwoCodegenError(
                f'fastdb codec {codec_ref.get("id")!r} must reference schema {SCHEMA_VERSION}.',
            )
        schema = schemas_by_hash.get(digest)
        if schema is None:
            raise CTwoCodegenError(f'No fastdb.schema.v1 descriptor supplied for schema hash {digest}.')
        _validate_codec_schema_profile(codec_ref, schema)
        matched.append((codec_ref, schema))

    needed_identities = _schema_identity_closure(
        [schema for _codec, schema in matched],
        schemas_by_identity,
    )
    emitted_schemas = [
        schema
        for identity, schema in schemas_by_identity.items()
        if identity in needed_identities
    ]
    return _render_typescript_helpers(matched, emitted_schemas, schemas_by_identity)


def run_codegen_c_two_ts(
    contract_path: str,
    output_path: str,
    schema_paths: list[str],
) -> None:
    contract = json.loads(Path(contract_path).read_text())
    schemas = [
        json.loads(Path(path).read_text())
        for path in schema_paths
    ]
    output = generate_c_two_typescript_helpers(contract, schemas)
    Path(output_path).write_text(output)


def _coerce_json(value: dict[str, Any] | str | bytes, *, label: str) -> dict[str, Any]:
    if isinstance(value, dict):
        return value
    if isinstance(value, bytes):
        value = value.decode()
    if isinstance(value, str):
        parsed = json.loads(value)
        if isinstance(parsed, dict):
            return parsed
    raise CTwoCodegenError(f'{label} must be a JSON object.')


def _schema_hash(descriptor: dict[str, Any]) -> str:
    _schema_identity(descriptor)
    return schema_sha256(descriptor)


def _index_schemas(
    schemas: list[dict[str, Any]],
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    by_hash: dict[str, dict[str, Any]] = {}
    by_identity: dict[str, dict[str, Any]] = {}
    identity_hashes: dict[str, str] = {}
    for descriptor in schemas:
        identity = _schema_identity(descriptor)
        digest = schema_sha256(descriptor)
        previous_hash = identity_hashes.get(identity)
        if previous_hash is not None:
            if previous_hash != digest:
                raise CTwoCodegenError(
                    'duplicate fastdb schema identity with different hashes: '
                    f'{identity} has {previous_hash} and {digest}.',
                )
            continue
        identity_hashes[identity] = digest
        by_hash[digest] = descriptor
        by_identity[identity] = descriptor
    return by_hash, by_identity


def _schema_identity(descriptor: dict[str, Any]) -> str:
    if descriptor.get('schema') != SCHEMA_VERSION:
        raise CTwoCodegenError(f'Expected {SCHEMA_VERSION}, got {descriptor.get("schema")!r}.')
    feature = descriptor.get('feature')
    if not isinstance(feature, dict):
        raise CTwoCodegenError('fastdb schema descriptor must contain a feature object.')
    identity = feature.get('identity')
    if not isinstance(identity, str) or not identity:
        raise CTwoCodegenError('fastdb schema feature.identity must be a non-empty string.')
    name = feature.get('name')
    if not isinstance(name, str) or not name:
        raise CTwoCodegenError('fastdb schema feature.name must be a non-empty string.')
    fields = descriptor.get('fields')
    if not isinstance(fields, list):
        raise CTwoCodegenError('fastdb schema descriptor fields must be a list.')
    return identity


def _collect_fastdb_requirements(contract: dict[str, Any]) -> list[dict[str, Any]]:
    if contract.get('schema') != 'c-two.contract.v1':
        raise CTwoCodegenError('contract descriptor must use schema c-two.contract.v1.')
    requirements: dict[tuple[str, str | None], dict[str, Any]] = {}
    for method in contract.get('methods', []):
        wire = method.get('wire') or {}
        for position in ('input', 'output'):
            codec_ref = wire.get(position)
            if not isinstance(codec_ref, dict):
                continue
            codec_id = codec_ref.get('id')
            if codec_id in FASTDB_CODEC_IDS:
                requirements[(codec_id, codec_ref.get('schema_sha256'))] = codec_ref
    return list(requirements.values())


def _render_typescript_helpers(
    matched: list[tuple[dict[str, Any], dict[str, Any]]],
    schemas: list[dict[str, Any]],
    schemas_by_identity: dict[str, dict[str, Any]],
) -> str:
    imports = _collect_imports(schemas, schemas_by_identity)
    lines: list[str] = ['// Auto-generated by fdb codegen --c-two-ts. Do not edit manually.']
    if imports:
        lines.append(f"import {{ {', '.join(sorted(imports))} }} from 'fastdb4ts';")
    lines.append('')
    if not matched:
        lines.append('export const FASTDB_C2_CODEC_BINDINGS = [];')
        return '\n'.join(lines) + '\n'

    lines.extend([
        'export interface FastdbC2CodecBinding<T> {',
        '  readonly codecId: string;',
        '  readonly profile: string;',
        '  readonly schemaSha256: string;',
        '  readonly feature: new () => T;',
        '  encode(_value: T): never;',
        '  decode(_payload: Uint8Array): never;',
        '}',
        '',
    ])
    class_names: dict[str, tuple[str, str]] = {}
    for schema in schemas:
        original_name = schema['feature']['name']
        class_name = _identifier(original_name)
        identity = schema['feature']['identity']
        previous = class_names.setdefault(class_name, (original_name, identity))
        if previous[1] != identity:
            raise CTwoCodegenError(
                'feature name collision after TypeScript identifier sanitization: '
                f'{previous[0]!r} and {original_name!r} both map to {class_name!r}.',
            )
        lines.append(_render_schema_class(schema, schemas_by_identity))
        lines.append('')
    binding_names = []
    for codec_ref, schema in matched:
        binding_name = _binding_name(schema, codec_ref)
        binding_names.append(binding_name)
        lines.append(_render_codec_binding(binding_name, codec_ref, schema))
        lines.append('')
    lines.append(f'export const FASTDB_C2_CODEC_BINDINGS = [{", ".join(binding_names)}] as const;')
    return '\n'.join(lines) + '\n'


def _schema_identity_closure(
    roots: list[dict[str, Any]],
    schemas_by_identity: dict[str, dict[str, Any]],
) -> set[str]:
    needed = {schema['feature']['identity'] for schema in roots}
    changed = True
    while changed:
        changed = False
        for identity in list(needed):
            schema = schemas_by_identity.get(identity)
            if schema is None:
                raise CTwoCodegenError(f'Missing fastdb schema descriptor for ref target {identity}.')
            for target in _iter_ref_targets(schema):
                target_identity = target.get('identity')
                if target_identity and target_identity not in needed:
                    if target_identity not in schemas_by_identity:
                        raise CTwoCodegenError(
                            f'Missing fastdb schema descriptor for ref target {target_identity}.',
                        )
                    needed.add(target_identity)
                    changed = True
    return needed


def _iter_ref_targets(schema: dict[str, Any]):
    for field in schema.get('fields', []):
        if field.get('kind') == 'ref':
            yield field.get('target') or {}
        elif field.get('kind') == 'list':
            item = field.get('item') or {}
            if item.get('kind') == 'ref':
                yield item.get('target') or {}


def _validate_codec_schema_profile(codec_ref: dict[str, Any], schema: dict[str, Any]) -> None:
    codec_id = codec_ref.get('id')
    if codec_id == 'org.fastdb.columnar':
        for field in schema.get('fields', []):
            if field.get('kind') == 'ref':
                raise CTwoCodegenError(
                    f'columnar codec cannot represent ref field {field.get("name")!r}; use org.fastdb.object-graph.',
                )
            if field.get('kind') == 'list' and (field.get('item') or {}).get('kind') == 'ref':
                raise CTwoCodegenError(
                    f'columnar codec cannot represent list[ref] field {field.get("name")!r}; use org.fastdb.object-graph.',
                )
        return
    if codec_id == 'org.fastdb.object-graph':
        return
    raise CTwoCodegenError(f'unsupported fastdb codec {codec_id!r}.')


def _collect_imports(
    schemas: list[dict[str, Any]],
    schemas_by_identity: dict[str, dict[str, Any]],
) -> set[str]:
    imports = {'Feature', 'defineSchema'}
    for schema in schemas:
        for field in schema.get('fields', []):
            _schema_expr, _type_expr, field_imports = _field_ts(field, schemas_by_identity)
            imports.update(field_imports)
    return imports


def _render_schema_class(
    schema: dict[str, Any],
    schemas_by_identity: dict[str, dict[str, Any]],
) -> str:
    class_name = _identifier(schema['feature']['name'])
    body = [f'export class {class_name} extends Feature {{', '  static schema = defineSchema({']
    declares: list[str] = []
    field_names: dict[str, str] = {}
    for field in schema.get('fields', []):
        schema_expr, type_expr, _imports = _field_ts(field, schemas_by_identity)
        field_name = _identifier(field['name'])
        previous = field_names.setdefault(field_name, field['name'])
        if previous != field['name']:
            raise CTwoCodegenError(
                f'field name collision after TypeScript identifier sanitization in {class_name}: '
                f'{previous!r} and {field["name"]!r} both map to {field_name!r}.',
            )
        body.append(f'    {field_name}: {schema_expr},')
        declares.append(f'  declare {field_name}: {type_expr};')
    body.append('  });')
    body.extend(declares)
    body.append('}')
    return '\n'.join(body)


def _field_ts(
    field: dict[str, Any],
    schemas_by_identity: dict[str, dict[str, Any]],
) -> tuple[str, str, set[str]]:
    kind = field.get('kind')
    if kind in _SCALAR_SCHEMA:
        symbol, ts_type = _SCALAR_SCHEMA[kind]
        return symbol, ts_type, {symbol}
    if kind == 'ref':
        target = _target_class(field.get('target'), schemas_by_identity)
        return f'ref(() => {target})', f'{target} | null', {'ref'}
    if kind == 'list':
        item = field.get('item') or {}
        item_schema, item_type, imports = _list_item_ts(item, schemas_by_identity)
        imports.add('listOf')
        return f'listOf({item_schema})', f'{item_type}[]', imports
    raise CTwoCodegenError(f'Unsupported fastdb schema field kind {kind!r}.')


def _list_item_ts(
    item: dict[str, Any],
    schemas_by_identity: dict[str, dict[str, Any]],
) -> tuple[str, str, set[str]]:
    kind = item.get('kind')
    if kind in _SCALAR_SCHEMA:
        symbol, ts_type = _SCALAR_SCHEMA[kind]
        return symbol, ts_type, {symbol}
    if kind == 'ref':
        target = _target_class(item.get('target'), schemas_by_identity)
        return f'() => {target}', target, set()
    raise CTwoCodegenError(f'Unsupported fastdb schema list item kind {kind!r}.')


def _target_class(
    target: dict[str, Any] | None,
    schemas_by_identity: dict[str, dict[str, Any]],
) -> str:
    if not target:
        raise CTwoCodegenError('fastdb ref target is missing from schema descriptor.')
    identity = target.get('identity')
    if identity and identity not in schemas_by_identity:
        raise CTwoCodegenError(f'Missing fastdb schema descriptor for ref target {identity}.')
    return _identifier(target['name'])


def _render_codec_binding(
    binding_name: str,
    codec_ref: dict[str, Any],
    schema: dict[str, Any],
) -> str:
    class_name = _identifier(schema['feature']['name'])
    codec_id = codec_ref['id']
    profile = FASTDB_CODEC_IDS[codec_id]
    digest = codec_ref['schema_sha256']
    message = (
        f'fastdb TypeScript codec runtime is not implemented for {codec_id}; '
        'wire this generated binding to fastdb4ts runtime before use.'
    )
    return '\n'.join([
        f'export const {binding_name}: FastdbC2CodecBinding<{class_name}> = {{',
        f'  codecId: {_ts_string(codec_id)},',
        f'  profile: {_ts_string(profile)},',
        f'  schemaSha256: {_ts_string(digest)},',
        f'  feature: {class_name},',
        f'  encode(_value: {class_name}): never {{',
        f'    throw new Error({_ts_string(message)});',
        '  },',
        '  decode(_payload: Uint8Array): never {',
        f'    throw new Error({_ts_string(message)});',
        '  },',
        '};',
    ])


def _binding_name(schema: dict[str, Any], codec_ref: dict[str, Any]) -> str:
    profile = FASTDB_CODEC_IDS[codec_ref['id']]
    return f'{_const_identifier(schema["feature"]["name"])}_{_const_identifier(profile.replace(".v1", ""))}_CODEC'


def _identifier(value: str) -> str:
    result = ''.join(ch if ch == '_' or ch.isalnum() else '_' for ch in value)
    if not result:
        return '_'
    if result[0].isdigit():
        result = f'_{result}'
    if result in _TS_RESERVED_WORDS:
        result = f'{result}_'
    return result


def _const_identifier(value: str) -> str:
    result: list[str] = []
    previous_lower_or_digit = False
    for ch in value:
        if ch.isalnum():
            if ch.isupper() and previous_lower_or_digit and result and result[-1] != '_':
                result.append('_')
            result.append(ch.upper())
            previous_lower_or_digit = ch.islower() or ch.isdigit()
        elif result and result[-1] != '_':
            result.append('_')
            previous_lower_or_digit = False
    return ''.join(result).strip('_') or 'FASTDB'


def _ts_string(value: str) -> str:
    return json.dumps(value)


_TS_RESERVED_WORDS = {
    'abstract',
    'any',
    'as',
    'asserts',
    'async',
    'await',
    'bigint',
    'boolean',
    'break',
    'case',
    'catch',
    'class',
    'const',
    'constructor',
    'continue',
    'debugger',
    'declare',
    'default',
    'delete',
    'do',
    'else',
    'enum',
    'export',
    'extends',
    'false',
    'finally',
    'for',
    'from',
    'function',
    'get',
    'global',
    'if',
    'implements',
    'import',
    'in',
    'infer',
    'instanceof',
    'interface',
    'is',
    'keyof',
    'let',
    'module',
    'namespace',
    'never',
    'new',
    'null',
    'number',
    'object',
    'of',
    'package',
    'private',
    'protected',
    'public',
    'readonly',
    'require',
    'return',
    'satisfies',
    'set',
    'static',
    'string',
    'super',
    'switch',
    'symbol',
    'this',
    'throw',
    'true',
    'try',
    'type',
    'typeof',
    'undefined',
    'unique',
    'unknown',
    'var',
    'void',
    'while',
    'with',
    'yield',
}
