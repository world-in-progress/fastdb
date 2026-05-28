import inspect

import pytest

import fastdb4py as fdb
import fastdb4py.call_db as call_db


@fdb.feature
class RequirePoint:
    row_id: fdb.U32
    x: fdb.F64


@fdb.feature
class RequireNumber:
    value: fdb.F32


@fdb.feature
class RequireText:
    value: fdb.STR


@fdb.feature
class RequireScalars:
    answer: fdb.I32


def _point_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_points',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=RequirePoint,
                kind='feature',
                name='return_0',
                value_position=0,
            ),
        ),
    )


def _point_number_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_pair',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=RequirePoint,
                kind='feature',
                name='return_0',
                value_position=0,
            ),
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=RequireNumber,
                item=fdb.FastdbCallDbArrayItem(kind='f32', name='value'),
                kind='array',
                name='return_1',
                value_position=1,
            ),
        ),
    )


def _scalar_point_number_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='input',
        method='set_pair',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='one',
                feature=RequireScalars,
                fields=(
                    fdb.FastdbCallDbScalarField(kind='i32', name='answer', value_position=0),
                ),
                kind='scalars',
                name='arg_0',
            ),
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=RequirePoint,
                kind='feature',
                name='arg_1',
                value_position=1,
            ),
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=RequireNumber,
                item=fdb.FastdbCallDbArrayItem(kind='f32', name='value'),
                kind='array',
                name='arg_2',
                value_position=2,
            ),
        ),
    )


def _string_array_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_text',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=RequireText,
                item=fdb.FastdbCallDbArrayItem(kind='str', name='value'),
                kind='array',
                name='return_0',
                value_position=0,
            ),
        ),
    )


def _filled_points(count: int) -> fdb.Batch[RequirePoint]:
    cells = fdb.require(fdb.batch(RequirePoint, rows=count))
    cells.fill(
        row_id=list(range(count)),
        x=[float(index) + 0.5 for index in range(count)],
    )
    return cells


def test_requirement_builders_validate_public_shape():
    batch_spec = fdb.batch(RequirePoint, rows=3)
    array_spec = fdb.array(fdb.F32, rows=4)

    assert isinstance(batch_spec, fdb.BatchRequirement)
    assert isinstance(array_spec, fdb.ArrayRequirement)
    assert batch_spec.feature_type is RequirePoint
    assert batch_spec.rows == 3
    assert batch_spec.profile == 'auto'
    assert array_spec.item_type is fdb.F32
    assert array_spec.rows == 4

    assert 'name' not in inspect.signature(fdb.batch).parameters
    assert 'alias' not in inspect.signature(fdb.batch).parameters
    assert 'return_0' not in inspect.signature(fdb.batch).parameters
    assert 'name' not in inspect.signature(fdb.array).parameters
    assert 'alias' not in inspect.signature(fdb.array).parameters


def test_requirement_builders_reject_invalid_shapes():
    class NotFeature:
        value: int

    with pytest.raises(ValueError, match='rows must be a non-negative integer'):
        fdb.batch(RequirePoint, rows=-1)
    with pytest.raises(ValueError, match='rows must be a non-negative integer'):
        fdb.array(fdb.I32, rows=True)
    with pytest.raises(TypeError, match='fastdb @feature'):
        fdb.batch(NotFeature, rows=1)
    with pytest.raises(TypeError, match='supported fastdb scalar alias'):
        fdb.array(object(), rows=1)


def test_require_single_spec_returns_typed_batch():
    cells = fdb.require(fdb.batch(RequirePoint, rows=2))

    assert isinstance(cells, fdb.Batch)
    assert not isinstance(cells, tuple)
    assert cells.feature_type is RequirePoint
    assert len(cells) == 2


def test_require_multiple_specs_returns_position_ordered_tuple():
    cells, residual = fdb.require(
        fdb.batch(RequirePoint, rows=2),
        fdb.array(fdb.F32, rows=3),
    )

    assert isinstance(cells, fdb.Batch)
    assert isinstance(residual, fdb.Array)
    assert cells.feature_type is RequirePoint
    assert residual.item_type is fdb.F32
    assert len(cells) == 2
    assert len(residual) == 0
    residual.fill([0.1, 0.2, 0.3])
    assert list(residual) == [0.1, 0.2, 0.3]


def test_require_rejects_empty_or_non_requirement_specs():
    with pytest.raises(ValueError, match='at least one requirement'):
        fdb.require()
    with pytest.raises(TypeError, match='BatchRequirement or ArrayRequirement'):
        fdb.require(object())


def test_prepare_call_db_imports_require_batch_without_bulk_repack(monkeypatch):
    cells = _filled_points(3)

    def fail_bulk(*args, **kwargs):
        raise AssertionError('require-backed Batch should import its backing layer')

    monkeypatch.setattr(call_db, '_try_encode_feature_table_bulk', fail_bulk)

    plan = fdb.prepare_call_db(_point_binding(), cells, direct_required=True)
    destination = bytearray(plan.nbytes)
    written = plan.write_into(destination)
    decoded = fdb.decode_call_db(_point_binding(), destination)

    assert written == plan.nbytes
    assert [row.row_id for row in decoded] == [0, 1, 2]
    assert [row.x for row in decoded] == [0.5, 1.5, 2.5]


def test_prepare_call_db_rejects_wrong_require_order():
    residual, cells = fdb.require(
        fdb.array(fdb.F32, rows=2),
        fdb.batch(RequirePoint, rows=2),
    )
    residual.fill([1.0, 2.0])
    cells.fill(row_id=[1, 2], x=[1.5, 2.5])

    with pytest.raises(ValueError, match='call-db slot 0 expected Batch\\[RequirePoint\\]'):
        fdb.prepare_call_db(_point_number_binding(), (residual, cells), direct_required=True)


def test_build_call_db_allocates_once_for_scalar_plus_require_values():
    cells, residual = fdb.require(
        fdb.batch(RequirePoint, rows=2),
        fdb.array(fdb.F32, rows=3),
    )
    cells.fill(row_id=[10, 11], x=[10.5, 11.5])
    residual.fill([0.25, 0.5, 0.75])
    allocator = fdb.BytearrayAllocator()

    payload = fdb.build_call_db(
        _scalar_point_number_binding(),
        (7, cells, residual),
        allocator,
        direct_required=True,
    )
    answer, decoded_cells, decoded_residual = fdb.decode_call_db(
        _scalar_point_number_binding(),
        payload,
    )

    assert allocator.allocate_count == 1
    assert allocator.commit_count == 1
    assert allocator.rollback_count == 0
    assert answer == 7
    assert [row.row_id for row in decoded_cells] == [10, 11]
    assert decoded_residual == [0.25, 0.5, 0.75]


def test_build_call_db_rolls_back_when_writer_fails():
    class ReadOnlyAllocation:
        def __init__(self, nbytes: int):
            self._buffer = bytes(nbytes)
            self.rolled_back = False

        @property
        def buffer(self):
            return memoryview(self._buffer)

        def commit(self):
            raise AssertionError('commit must not run after write failure')

        def rollback(self):
            self.rolled_back = True

    class ReadOnlyAllocator:
        def __init__(self):
            self.allocation = None

        def allocate(self, nbytes: int):
            self.allocation = ReadOnlyAllocation(nbytes)
            return self.allocation

    allocator = ReadOnlyAllocator()

    with pytest.raises(TypeError, match='destination buffer must be writable'):
        fdb.build_call_db(_point_binding(), _filled_points(1), allocator, direct_required=True)

    assert allocator.allocation.rolled_back is True


def test_strict_direct_build_rejects_string_array_before_allocation():
    texts = fdb.require(fdb.array(fdb.STR, rows=2))
    texts.fill(['a', 'b'])
    allocator = fdb.BytearrayAllocator()

    with pytest.raises(fdb.FastdbUnsupportedDirectBuildError, match='str'):
        fdb.build_call_db(_string_array_binding(), texts, allocator, direct_required=True)

    assert allocator.allocate_count == 0
