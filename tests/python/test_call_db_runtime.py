import pytest

import fastdb4py as fdb
import numpy as np


@fdb.feature
class CallDbPoint:
    row_id: fdb.U32
    x: fdb.F64
    name: fdb.STR


@fdb.feature
class CallDbNumber:
    __fastdb_layer_name__ = 'return_0'

    value: fdb.I32


@fdb.feature
class CallDbScalars:
    __fastdb_layer_name__ = '__return_scalars'

    answer: fdb.I32
    label: fdb.STR


@fdb.feature
class CallDbBoolScalars:
    __fastdb_layer_name__ = '__return_bool_scalars'

    enabled: fdb.BOOL


@fdb.feature
class CallDbFlag:
    row_id: fdb.U32
    enabled: fdb.BOOL


def _feature_binding(*, cardinality='many'):
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_points',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality=cardinality,
                feature=CallDbPoint,
                kind='feature',
                name='return_0',
                value_position=0,
            ),
        ),
    )


def _two_feature_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_point_pairs',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=CallDbPoint,
                kind='feature',
                name='return_0',
                value_position=0,
            ),
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=CallDbPoint,
                kind='feature',
                name='return_1',
                value_position=1,
            ),
        ),
    )


def _flag_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_flags',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=CallDbFlag,
                kind='feature',
                name='return_0',
                value_position=0,
            ),
        ),
    )


def _array_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_numbers',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=CallDbNumber,
                item=fdb.FastdbCallDbArrayItem(kind='i32', name='value'),
                kind='array',
                name='return_0',
                value_position=0,
            ),
        ),
    )


def _scalar_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_scalar_pair',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='one',
                feature=CallDbScalars,
                fields=(
                    fdb.FastdbCallDbScalarField(kind='i32', name='answer', value_position=0),
                    fdb.FastdbCallDbScalarField(kind='str', name='label', value_position=1),
                ),
                kind='scalars',
                name='__return_scalars',
            ),
        ),
    )


def _bool_scalar_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_bool',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='one',
                feature=CallDbBoolScalars,
                fields=(
                    fdb.FastdbCallDbScalarField(kind='bool', name='enabled', value_position=0),
                ),
                kind='scalars',
                name='__return_bool_scalars',
            ),
        ),
    )


def _backed_points_table(count: int = 3, *, start: int = 0, prefix: str = 'point') -> fdb.Table[CallDbPoint]:
    idx = np.arange(start, start + count, dtype=np.uint32)
    engine = fdb.ColumnEngine.truncate([fdb.Layout(CallDbPoint, count)])
    table = engine.table(CallDbPoint)
    table.fill(
        row_id=idx,
        x=idx.astype(np.float64) + 0.5,
        name=[f'{prefix}-{i}' for i in range(start, start + count)],
    )
    return table


def _backed_flags_table() -> fdb.Table[CallDbFlag]:
    engine = fdb.ColumnEngine.truncate([fdb.Layout(CallDbFlag, 4)])
    table = engine.table(CallDbFlag)
    table.fill(
        row_id=np.arange(4, dtype=np.uint32),
        enabled=np.array([True, False, 1, 0], dtype=np.uint8),
    )
    return table


def test_encode_decode_call_db_feature_batch_returns_materialized_rows():
    binding = _feature_binding()
    payload = fdb.encode_call_db(binding, [
        CallDbPoint(row_id=1, x=1.5, name='alpha'),
        CallDbPoint(row_id=2, x=2.5, name='beta'),
    ])

    rows = fdb.decode_call_db(binding, payload)

    assert [row.row_id for row in rows] == [1, 2]
    assert [row.name for row in rows] == ['alpha', 'beta']
    rows[0].x = 99.0
    assert rows[0].x == pytest.approx(99.0)


def test_encode_call_db_feature_batch_preserves_backed_table_rows():
    binding = _feature_binding()

    payload = fdb.encode_call_db(binding, _backed_points_table())
    rows = fdb.decode_call_db(binding, payload)

    assert [row.row_id for row in rows] == [0, 1, 2]
    assert [row.x for row in rows] == [0.5, 1.5, 2.5]
    assert [row.name for row in rows] == ['point-0', 'point-1', 'point-2']


def test_encode_call_db_feature_batch_uses_bulk_columns_for_backed_table(monkeypatch):
    binding = _feature_binding()
    table = _backed_points_table()

    def fail_push_many(*args, **kwargs):
        raise AssertionError('backed fdb.Table call-db encoding must not use row-wise push_many')

    monkeypatch.setattr(fdb.ColumnEngine, 'push_many', fail_push_many)

    payload = fdb.encode_call_db(binding, table)
    view = fdb.view_call_db(binding, memoryview(payload)).logical_value()

    assert list(view.column.row_id) == [0, 1, 2]
    assert view.column.name[2] == 'point-2'


def test_encode_call_db_feature_batch_bulk_supports_multiple_tables():
    binding = _two_feature_binding()

    payload = fdb.encode_call_db(binding, (
        _backed_points_table(2, start=10, prefix='left'),
        _backed_points_table(3, start=20, prefix='right'),
    ))
    left, right = fdb.decode_call_db(binding, payload)

    assert [row.row_id for row in left] == [10, 11]
    assert [row.name for row in left] == ['left-10', 'left-11']
    assert [row.row_id for row in right] == [20, 21, 22]
    assert [row.name for row in right] == ['right-20', 'right-21', 'right-22']


def test_encode_call_db_feature_batch_bulk_preserves_bool_columns():
    binding = _flag_binding()

    payload = fdb.encode_call_db(binding, _backed_flags_table())
    rows = fdb.decode_call_db(binding, payload)

    assert [row.row_id for row in rows] == [0, 1, 2, 3]
    assert [bool(row.enabled) for row in rows] == [True, False, True, False]


def test_encode_call_db_single_feature_preserves_backed_row():
    binding = _feature_binding(cardinality='one')
    row = _backed_points_table()[1]

    payload = fdb.encode_call_db(binding, row)
    decoded = fdb.decode_call_db(binding, payload)

    assert decoded.row_id == 1
    assert decoded.x == pytest.approx(1.5)
    assert decoded.name == 'point-1'


def test_view_call_db_feature_batch_returns_owner_bound_fastdb_table():
    binding = _feature_binding()
    payload = fdb.encode_call_db(binding, [
        CallDbPoint(row_id=1, x=1.5, name='alpha'),
        CallDbPoint(row_id=2, x=2.5, name='beta'),
    ])
    owner = fdb.FdbViewOwner(checked=True, writeable=False)

    view = fdb.view_call_db(binding, memoryview(payload), owner=owner)
    table = view.logical_value()
    row = table[0]
    name_col = table.column.name
    owned_rows = fdb.materialize(table)

    assert isinstance(table, fdb.Table)
    assert row.name == 'alpha'
    assert name_col[1] == 'beta'
    assert [item.x for item in owned_rows] == [1.5, 2.5]

    fdb.invalidate(owner)

    with pytest.raises(fdb.FdbViewInvalidatedError):
        len(table)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = row.x
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = name_col[0]
    assert [item.name for item in owned_rows] == ['alpha', 'beta']


def test_view_call_db_single_feature_returns_owner_bound_feature():
    binding = _feature_binding(cardinality='one')
    payload = fdb.encode_call_db(binding, CallDbPoint(row_id=7, x=3.5, name='single'))
    owner = fdb.FdbViewOwner(checked=True, writeable=False)

    feature = fdb.view_call_db(binding, memoryview(payload), owner=owner).logical_value()

    assert isinstance(feature, CallDbPoint)
    assert feature.name == 'single'

    fdb.invalidate(owner)

    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = feature.x


def test_view_call_db_scalar_array_is_owner_bound_and_materializable():
    binding = _array_binding()
    payload = fdb.encode_call_db(binding, [1, 2, 3])
    owner = fdb.FdbViewOwner(checked=True, writeable=False)

    numbers = fdb.view_call_db(binding, memoryview(payload), owner=owner).logical_value()
    owned_numbers = fdb.materialize(numbers)

    assert list(numbers) == [1, 2, 3]
    assert numbers[1] == 2
    assert numbers[:2] == [1, 2]
    assert numbers[::2] == [1, 3]
    assert numbers == [1, 2, 3]
    assert numbers == (1, 2, 3)
    assert owned_numbers == [1, 2, 3]

    fdb.invalidate(owner)

    with pytest.raises(fdb.FdbViewInvalidatedError):
        len(numbers)
    assert owned_numbers == [1, 2, 3]


def test_decode_call_db_scalar_tuple_materializes_values():
    binding = _scalar_binding()
    payload = fdb.encode_call_db(binding, (42, 'ready'))

    assert fdb.decode_call_db(binding, payload) == (42, 'ready')


def test_call_db_bool_scalar_keeps_logical_bool_kind():
    binding = _bool_scalar_binding()

    payload = fdb.encode_call_db(binding, 'false')

    assert fdb.decode_call_db(binding, payload) is False
    assert fdb.view_call_db(binding, memoryview(payload)).logical_value() is False


def test_view_call_db_object_graph_profile_fails_deterministically():
    binding = fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='graph',
        profile='fastdb.call.object-graph.v1',
        schema_sha256='test-schema',
        tables=(),
    )

    with pytest.raises(ValueError, match='does not support retained buffer views'):
        fdb.view_call_db(binding, b'', owner=fdb.FdbViewOwner(checked=True))
