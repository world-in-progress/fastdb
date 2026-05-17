import pytest

from fastdb4py import F64, STR, BYTES, WSTR, feature
from fastdb4py.schema import (
    columnar_capability,
    codec_ref_for_feature,
    export_schema,
    object_graph_capability,
    schema_sha256,
)
from fastdb4py.c_two_provider import CTwoFastdbCodecProvider, FastdbCodecProvider, install_c_two_provider


@feature
class C2SchemaPoint:
    x: F64
    y: F64
    name: STR


@feature
class C2SchemaBlob:
    data: BYTES
    label: WSTR


@feature
class C2SchemaNode:
    value: F64
    child: C2SchemaPoint


@feature
class C2SchemaGraph:
    root: C2SchemaNode
    leaves: list[C2SchemaPoint]


class PlainAnnotated:
    value: F64


@feature
class C2SchemaBadRef:
    child: PlainAnnotated


def test_export_schema_is_neutral_and_excludes_layer_runtime_state():
    descriptor = export_schema(C2SchemaPoint)

    assert descriptor == {
        'feature': {
            'identity': f'{C2SchemaPoint.__module__}:{C2SchemaPoint.__qualname__}',
            'module': C2SchemaPoint.__module__,
            'name': 'C2SchemaPoint',
            'qualname': C2SchemaPoint.__qualname__,
        },
        'fields': [
            {'kind': 'f64', 'name': 'x'},
            {'kind': 'f64', 'name': 'y'},
            {'kind': 'str', 'name': 'name'},
        ],
        'schema': 'fastdb.schema.v1',
    }
    encoded = repr(descriptor)
    assert 'numeric_plan' not in encoded
    assert 'column_accessor_class' not in encoded
    assert 'cpp_type' not in encoded


def test_schema_hash_is_stable_for_same_descriptor():
    assert schema_sha256(C2SchemaPoint) == schema_sha256(export_schema(C2SchemaPoint))


def test_strict_export_rejects_non_feature_ref_targets():
    with pytest.raises(TypeError, match='explicit @feature'):
        export_schema(C2SchemaBadRef)


def test_capability_profiles_are_separate_from_semantic_schema():
    semantic = export_schema(C2SchemaGraph)
    columnar = columnar_capability(C2SchemaGraph)
    graph = object_graph_capability(C2SchemaGraph)

    assert semantic['schema'] == 'fastdb.schema.v1'
    assert columnar['profile'] == 'columnar.v1'
    assert columnar['eligible'] is False
    assert 'root' in columnar['unsupported_fields']
    assert 'leaves' in columnar['unsupported_fields']
    assert graph['profile'] == 'object_graph.v1'
    assert graph['eligible'] is True


def test_columnar_capability_reports_fixed_table_variable_length_fields():
    capability = columnar_capability(C2SchemaBlob, fixed_table=True)

    assert capability['eligible'] is False
    assert capability['unsupported_fields'] == ['data', 'label']
    assert capability['diagnostics'] == [
        'data: bytes is not supported by fixed-table columnar layout',
        'label: wstr is not supported by fixed-table columnar layout',
    ]


def test_codec_ref_for_feature_uses_opaque_schema_hash():
    ref = codec_ref_for_feature(C2SchemaPoint, profile='columnar')

    assert ref == {
        'capabilities': ['bytes', 'buffer-view'],
        'id': 'org.fastdb.columnar',
        'kind': 'codec_ref',
        'portable': True,
        'schema': 'fastdb.schema.v1',
        'schema_sha256': schema_sha256(C2SchemaPoint),
        'version': '1',
    }


def test_fastdb_provider_reports_codec_candidates_without_c_two_dependency():
    provider = FastdbCodecProvider()

    point_candidates = provider.candidates_for_type(C2SchemaPoint)
    graph_candidates = provider.candidates_for_type(C2SchemaGraph)

    assert point_candidates == [
        {
            'codec_ref': codec_ref_for_feature(C2SchemaPoint, profile='columnar'),
            'profile': 'columnar.v1',
            'schema': export_schema(C2SchemaPoint),
        },
    ]
    assert graph_candidates == [
        {
            'codec_ref': codec_ref_for_feature(C2SchemaGraph, profile='object_graph'),
            'profile': 'object_graph.v1',
            'schema': export_schema(C2SchemaGraph),
        },
    ]
    assert provider.candidates_for_type(object) == []


def test_fastdb_provider_columnar_adapter_round_trips_feature():
    provider = FastdbCodecProvider()
    adapter = provider.adapter_for_type(C2SchemaPoint)

    point = C2SchemaPoint(x=1.5, y=2.5, name='alpha')
    restored = adapter.deserialize(adapter.serialize(point))

    assert restored.x == pytest.approx(1.5)
    assert restored.y == pytest.approx(2.5)
    assert restored.name == 'alpha'
    assert adapter.codec_ref == codec_ref_for_feature(C2SchemaPoint, profile='columnar')


def test_fastdb_provider_object_graph_adapter_round_trips_feature():
    provider = FastdbCodecProvider()
    adapter = provider.adapter_for_type(C2SchemaNode)

    node = C2SchemaNode(value=3.5, child=C2SchemaPoint(x=1.0, y=2.0, name='leaf'))
    restored = adapter.deserialize(adapter.serialize(node))

    assert restored.value == pytest.approx(3.5)
    assert restored.child.x == pytest.approx(1.0)
    assert restored.child.y == pytest.approx(2.0)
    assert restored.child.name == 'leaf'
    assert adapter.codec_ref == codec_ref_for_feature(C2SchemaNode, profile='object_graph')


def test_fastdb_provider_object_graph_adapter_round_trips_list_refs():
    provider = FastdbCodecProvider()
    adapter = provider.adapter_for_type(C2SchemaGraph)

    leaf = C2SchemaPoint(x=4.0, y=5.0, name='leaf-list')
    graph = C2SchemaGraph(
        root=C2SchemaNode(value=6.0, child=leaf),
        leaves=[leaf],
    )
    restored = adapter.deserialize(adapter.serialize(graph))

    assert restored.root.value == pytest.approx(6.0)
    assert restored.root.child.name == 'leaf-list'
    assert len(restored.leaves) == 1
    assert restored.leaves[0].x == pytest.approx(4.0)


def test_c_two_wrapper_provider_materializes_runtime_transferable_with_fake_c_two():
    class FakeCC:
        def __init__(self):
            self.registered_provider = None

        def transferable(self, *, codec_ref):
            def decorate(cls):
                cls.__cc_codec_ref__ = codec_ref
                return cls

            return decorate

        def use_codec(self, provider):
            self.registered_provider = provider
            return provider

    cc = FakeCC()
    provider = CTwoFastdbCodecProvider(cc_module=cc)

    candidates = provider.candidates_for_type(C2SchemaPoint)

    assert len(candidates) == 1
    candidate = candidates[0]
    transferable = candidate['transferable']
    ref = codec_ref_for_feature(C2SchemaPoint, profile='columnar')
    assert candidate['codec_ref'] == ref
    assert transferable.__cc_codec_ref__ == ref

    point = C2SchemaPoint(x=8.0, y=9.0, name='rpc')
    payload = transferable.serialize(point)
    restored = transferable.deserialize(payload)
    from_buffer = transferable.from_buffer(memoryview(payload))

    assert restored.x == pytest.approx(8.0)
    assert restored.name == 'rpc'
    assert from_buffer.y == pytest.approx(9.0)

    same_candidate = provider.candidates_for_type(C2SchemaPoint)[0]
    assert same_candidate['transferable'] is transferable

    installed = install_c_two_provider(cc_module=cc)
    assert isinstance(installed, CTwoFastdbCodecProvider)
    assert cc.registered_provider is installed
