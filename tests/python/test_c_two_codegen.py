import json
import sys

import pytest

from fastdb4py import F64, STR, feature
from fastdb4py.codegen.c_two_ts import CTwoCodegenError, generate_c_two_typescript_helpers
from fastdb4py.schema import codec_ref_for_feature, export_schema, schema_sha256


@feature
class CodegenPoint:
    x: F64
    y: F64
    name: STR


@feature
class CodegenNode:
    value: F64
    child: CodegenPoint


@feature
class CodegenCluster:
    name: STR
    children: list[CodegenPoint]


def _contract_with_codec_ref(codec_ref):
    return {
        'schema': 'c-two.contract.v1',
        'crm': {
            'namespace': 'test.fastdb',
            'name': 'FastdbPayload',
            'version': '0.1.0',
        },
        'methods': [
            {
                'access': 'write',
                'buffer': 'view',
                'name': 'load_point',
                'parameters': [],
                'return': {
                    'kind': 'codec',
                    'codec': codec_ref,
                },
                'wire': {
                    'input': None,
                    'output': codec_ref,
                },
            },
        ],
    }


def test_c_two_typescript_helpers_emit_schema_class_and_codec_stub():
    codec_ref = codec_ref_for_feature(CodegenPoint, profile='columnar')
    output = generate_c_two_typescript_helpers(
        _contract_with_codec_ref(codec_ref),
        [export_schema(CodegenPoint)],
    )

    assert "import { F64, Feature, STR, defineSchema } from 'fastdb4ts';" in output
    assert 'export class CodegenPoint extends Feature {' in output
    assert 'static schema = defineSchema({' in output
    assert 'x: F64,' in output
    assert 'declare name: string;' in output
    assert 'export const CODEGEN_POINT_COLUMNAR_CODEC' in output
    assert f'schemaSha256: "{schema_sha256(CodegenPoint)}"' in output
    assert 'encode(_value: CodegenPoint): never' in output
    assert 'fastdb TypeScript codec runtime is not implemented' in output


def test_c_two_typescript_helpers_reject_missing_fastdb_schema():
    codec_ref = codec_ref_for_feature(CodegenPoint, profile='columnar')

    with pytest.raises(CTwoCodegenError, match=schema_sha256(CodegenPoint)):
        generate_c_two_typescript_helpers(
            _contract_with_codec_ref(codec_ref),
            [],
        )


def test_c_two_typescript_helpers_emit_object_graph_refs():
    codec_ref = codec_ref_for_feature(CodegenNode, profile='object_graph')
    output = generate_c_two_typescript_helpers(
        _contract_with_codec_ref(codec_ref),
        [
            export_schema(CodegenPoint),
            export_schema(CodegenNode),
        ],
    )

    assert 'import { F64, Feature, STR, defineSchema, ref } from \'fastdb4ts\';' in output
    assert 'export class CodegenPoint extends Feature' in output
    assert 'export class CodegenNode extends Feature' in output
    assert 'child: ref(() => CodegenPoint),' in output
    assert 'declare child: CodegenPoint | null;' in output
    assert 'export const CODEGEN_NODE_OBJECT_GRAPH_CODEC' in output


def test_c_two_typescript_helpers_emit_object_graph_list_refs():
    codec_ref = codec_ref_for_feature(CodegenCluster, profile='object_graph')
    output = generate_c_two_typescript_helpers(
        _contract_with_codec_ref(codec_ref),
        [
            export_schema(CodegenPoint),
            export_schema(CodegenCluster),
        ],
    )

    assert 'import { F64, Feature, STR, defineSchema, listOf } from \'fastdb4ts\';' in output
    assert 'children: listOf(() => CodegenPoint),' in output
    assert 'listOf(ref(' not in output
    assert 'declare children: CodegenPoint[];' in output
    assert 'export const CODEGEN_CLUSTER_OBJECT_GRAPH_CODEC' in output


def test_c_two_typescript_helpers_ignore_non_fastdb_codecs():
    output = generate_c_two_typescript_helpers(
        _contract_with_codec_ref({
            'kind': 'codec_ref',
            'id': 'org.apache.arrow.ipc',
            'version': '1',
            'schema': 'arrow.ipc.schema.v1',
            'schema_sha256': '0' * 64,
            'portable': True,
        }),
        [export_schema(CodegenPoint)],
    )

    assert 'export class CodegenPoint' not in output
    assert 'export const FASTDB_C2_CODEC_BINDINGS = [];' in output


def test_c_two_typescript_helpers_reject_feature_name_collisions():
    codec_ref = codec_ref_for_feature(CodegenPoint, profile='columnar')
    colliding = export_schema(CodegenPoint)
    colliding['feature'] = {
        **colliding['feature'],
        'identity': 'test:Codegen-Point',
        'name': 'CodegenPoint',
        'qualname': 'Codegen-Point',
    }
    colliding_ref = {
        **codec_ref,
        'schema_sha256': schema_sha256(colliding),
    }
    contract = _contract_with_codec_ref(codec_ref)
    contract['methods'].append({
        **contract['methods'][0],
        'name': 'load_colliding_point',
        'return': {'kind': 'codec', 'codec': colliding_ref},
        'wire': {'input': None, 'output': colliding_ref},
    })

    with pytest.raises(CTwoCodegenError, match='feature name collision'):
        generate_c_two_typescript_helpers(
            contract,
            [
                export_schema(CodegenPoint),
                colliding,
            ],
        )


def test_c_two_typescript_helpers_reject_field_name_collisions():
    codec_ref = codec_ref_for_feature(CodegenPoint, profile='columnar')
    schema = export_schema(CodegenPoint)
    schema['fields'] = [
        {'kind': 'f64', 'name': 'value-id'},
        {'kind': 'f64', 'name': 'value_id'},
    ]
    digest = schema_sha256(schema)
    codec_ref = {
        **codec_ref,
        'schema_sha256': digest,
    }

    with pytest.raises(CTwoCodegenError, match='field name collision'):
        generate_c_two_typescript_helpers(
            _contract_with_codec_ref(codec_ref),
            [schema],
        )


def test_c_two_typescript_helpers_reject_duplicate_schema_identity_with_different_hash():
    codec_ref = codec_ref_for_feature(CodegenPoint, profile='columnar')
    changed = export_schema(CodegenPoint)
    changed['fields'] = [
        *changed['fields'],
        {'kind': 'f64', 'name': 'extra_value'},
    ]
    changed_ref = {
        **codec_ref,
        'schema_sha256': schema_sha256(changed),
    }
    contract = _contract_with_codec_ref(changed_ref)

    with pytest.raises(CTwoCodegenError, match='duplicate fastdb schema identity'):
        generate_c_two_typescript_helpers(
            contract,
            [
                export_schema(CodegenPoint),
                changed,
            ],
        )


def test_c_two_typescript_helpers_escapes_typescript_reserved_identifiers():
    codec_ref = codec_ref_for_feature(CodegenPoint, profile='columnar')
    schema = export_schema(CodegenPoint)
    schema['feature'] = {
        **schema['feature'],
        'identity': 'test:reserved',
        'name': 'class',
        'qualname': 'class',
    }
    schema['fields'] = [
        {'kind': 'f64', 'name': 'default'},
    ]
    codec_ref = {
        **codec_ref,
        'schema_sha256': schema_sha256(schema),
    }

    output = generate_c_two_typescript_helpers(
        _contract_with_codec_ref(codec_ref),
        [schema],
    )

    assert 'export class class_ extends Feature' in output
    assert 'default_: F64,' in output
    assert 'declare default_: number;' in output


def test_c_two_typescript_helpers_reject_profile_schema_mismatch():
    codec_ref = codec_ref_for_feature(CodegenNode, profile='object_graph')
    wrong_ref = {
        **codec_ref,
        'id': 'org.fastdb.columnar',
    }

    with pytest.raises(CTwoCodegenError, match='columnar codec cannot represent'):
        generate_c_two_typescript_helpers(
            _contract_with_codec_ref(wrong_ref),
            [
                export_schema(CodegenPoint),
                export_schema(CodegenNode),
            ],
        )


def test_c_two_typescript_codegen_cli_writes_helpers(tmp_path, monkeypatch):
    codec_ref = codec_ref_for_feature(CodegenPoint, profile='columnar')
    contract_path = tmp_path / 'contract.json'
    schema_path = tmp_path / 'point.schema.json'
    output_path = tmp_path / 'fastdb-codecs.ts'
    contract_path.write_text(json.dumps(_contract_with_codec_ref(codec_ref)))
    schema_path.write_text(json.dumps(export_schema(CodegenPoint)))
    monkeypatch.setattr(sys, 'argv', [
        'fdb',
        'codegen',
        '--c-two-ts',
        '--schema',
        str(schema_path),
        str(contract_path),
        str(output_path),
    ])

    from fastdb4py.cli import main

    main()

    generated = output_path.read_text()
    assert 'export class CodegenPoint extends Feature' in generated
    assert 'export const CODEGEN_POINT_COLUMNAR_CODEC' in generated
