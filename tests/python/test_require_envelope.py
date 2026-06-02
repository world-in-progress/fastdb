import array
import gc
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
class RequireBlob:
    data: fdb.BYTES


@fdb.feature
class RequireList:
    values: list[fdb.F64]


@fdb.feature
class RequireLeaf:
    value: fdb.F64


@fdb.feature
class RequireNode:
    child: RequireLeaf


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


def _text_feature_binding():
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_text_rows',
        profile='fastdb.call.columnar.v1',
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=RequireText,
                kind='feature',
                name='return_0',
                value_position=0,
            ),
        ),
    )


def _feature_binding(feature_type: type, *, profile: str = 'fastdb.call.columnar.v1'):
    return fdb.FastdbCallDbBinding(
        codec_id='org.fastdb.call-db',
        direction='output',
        method='get_rows',
        profile=profile,
        schema_sha256='test-schema',
        tables=(
            fdb.FastdbCallDbTable(
                cardinality='many',
                feature=feature_type,
                kind='feature',
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


def test_prepare_call_db_direct_required_rejects_unbacked_values_without_staging(monkeypatch):
    def fail_layer_staging(*args, **kwargs):
        raise AssertionError('strict direct prepare must not stage temporary call-db layers')

    monkeypatch.setattr(call_db, '_encode_call_table_layer', fail_layer_staging)

    with pytest.raises(fdb.FastdbUnsupportedDirectBuildError, match='existing backed layer|build_call_db'):
        fdb.prepare_call_db(
            _point_binding(),
            [RequirePoint(row_id=1, x=1.5)],
            direct_required=True,
        )


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


def test_build_call_db_direct_uses_native_final_backing(monkeypatch):
    cells, residual = fdb.require(
        fdb.batch(RequirePoint, rows=2),
        fdb.array(fdb.F32, rows=3),
    )
    cells.fill(row_id=[10, 11], x=[10.5, 11.5])
    residual.fill([0.25, 0.5, 0.75])
    allocator = fdb.BytearrayAllocator()

    def fail_python_copy(*args, **kwargs):
        raise AssertionError('direct allocator builds must write through native final backing')

    monkeypatch.setattr(call_db.FastdbPreparedCallDb, 'write_into', fail_python_copy)

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


def test_build_call_db_direct_numeric_values_avoid_native_table_buffer_builder(monkeypatch):
    cells, residual = fdb.require(
        fdb.batch(RequirePoint, rows=2),
        fdb.array(fdb.F32, rows=3),
    )
    cells.fill(row_id=[10, 11], x=[10.5, 11.5])
    residual.fill([0.25, 0.5, 0.75])
    allocator = fdb.BytearrayAllocator()

    def fail_native_build(*args, **kwargs):
        raise AssertionError('fixed numeric direct builds should not stage through native builder table buffers')

    monkeypatch.setattr(call_db, '_prepare_native_columnar_call_db', fail_native_build)

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


def test_build_call_db_direct_numeric_values_accept_native_resource_without_builder(monkeypatch):
    cells = _filled_points(2)
    resource = fdb.HeapFinalBackingResource()

    def fail_native_build(*args, **kwargs):
        raise AssertionError('fixed numeric native-resource direct build should use mapped final backing')

    monkeypatch.setattr(call_db, '_prepare_native_columnar_call_db', fail_native_build)

    allocation = fdb.build_call_db(
        _point_binding(),
        cells,
        resource,
        direct_required=True,
    )
    decoded = fdb.decode_call_db(_point_binding(), allocation.to_bytes())

    assert isinstance(allocation, fdb.FinalBackingAllocation)
    assert resource.allocation_count() == 1
    assert resource.commit_count() == 1
    assert resource.rollback_count() == 0
    assert [row.row_id for row in decoded] == [0, 1]


def test_require_context_builds_into_one_final_backing(monkeypatch):
    allocator = fdb.BytearrayAllocator()

    def fail_owned_publish(*args, **kwargs):
        raise AssertionError('require-context direct build must not publish through an owned bytearray')

    def fail_late_allocator_build(*args, **kwargs):
        raise AssertionError('require-context direct build must commit the existing allocation')

    monkeypatch.setattr('fastdb4py.column_engine._post_build_to_bytearray', fail_owned_publish)
    monkeypatch.setattr(call_db.FastdbPreparedCallDb, 'build_with_allocator', fail_late_allocator_build)

    with fdb.call_db_build_context(_scalar_point_number_binding(), allocator) as context:
        cells, residual = fdb.require(
            fdb.batch(RequirePoint, rows=2),
            fdb.array(fdb.F32, rows=3),
        )
        assert context.build_mode == 'require-context-direct'
        assert allocator.allocate_count == 1
        cells.fill(row_id=[10, 11], x=[10.5, 11.5])
        residual.fill([0.25, 0.5, 0.75])

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


def test_require_context_does_not_materialize_native_table_buffer():
    allocator = fdb.BytearrayAllocator()

    with fdb.call_db_build_context(_point_binding(), allocator) as context:
        cells = fdb.require(fdb.batch(RequirePoint, rows=2))

        assert context.nbytes > 0
        assert context._engine._fixed_build.table_buffer_bytes() == 0  # noqa: SLF001

        cells.fill(row_id=[1, 2], x=[1.5, 2.5])
        payload = fdb.build_call_db(_point_binding(), cells, allocator, direct_required=True)

    decoded = fdb.decode_call_db(_point_binding(), payload)
    assert [row.row_id for row in decoded] == [1, 2]


def test_require_context_maps_typed_allocator_buffer_as_exact_byte_slice():
    class WordAllocation:
        def __init__(self, nbytes: int, allocator: 'WordAllocator'):
            self.nbytes = nbytes
            self.words = array.array('Q', [0] * ((nbytes + 7) // 8))
            self._allocator = allocator

        @property
        def buffer(self):
            return memoryview(self.words)

        def commit(self, used_size: int):
            self._allocator.commit_count += 1
            return bytes(memoryview(self.words).cast('B')[:used_size])

        def rollback(self):
            self._allocator.rollback_count += 1

    class WordAllocator:
        def __init__(self):
            self.allocate_count = 0
            self.commit_count = 0
            self.rollback_count = 0

        def allocate(self, nbytes: int):
            self.allocate_count += 1
            return WordAllocation(nbytes, self)

    allocator = WordAllocator()

    with fdb.call_db_build_context(_point_binding(), allocator) as context:
        cells = fdb.require(fdb.batch(RequirePoint, rows=2))

        assert context.nbytes > 0
        assert context._engine._origin._buffer.nbytes == context.nbytes  # noqa: SLF001
        assert context._engine._origin._buffer.format == 'B'  # noqa: SLF001

        cells.fill(row_id=[1, 2], x=[1.5, 2.5])
        payload = fdb.build_call_db(_point_binding(), cells, allocator, direct_required=True)

    decoded = fdb.decode_call_db(_point_binding(), payload)
    assert allocator.allocate_count == 1
    assert allocator.commit_count == 1
    assert allocator.rollback_count == 0
    assert [row.row_id for row in decoded] == [1, 2]


def test_require_context_commits_original_allocator_backing():
    class IdentityAllocation:
        def __init__(self, nbytes: int, allocator: 'IdentityAllocator'):
            self.data = bytearray(nbytes)
            self._allocator = allocator
            self.rolled_back = False

        @property
        def buffer(self):
            return memoryview(self.data)

        def commit(self, used_size: int):
            assert used_size == len(self.data)
            self._allocator.commit_count += 1
            return self.data

        def rollback(self):
            self.rolled_back = True
            self._allocator.rollback_count += 1

    class IdentityAllocator:
        def __init__(self):
            self.allocate_count = 0
            self.commit_count = 0
            self.rollback_count = 0
            self.allocation = None

        def allocate(self, nbytes: int):
            self.allocate_count += 1
            self.allocation = IdentityAllocation(nbytes, self)
            return self.allocation

    allocator = IdentityAllocator()

    with fdb.call_db_build_context(_point_binding(), allocator):
        cells = fdb.require(fdb.batch(RequirePoint, rows=2))
        cells.fill(row_id=[1, 2], x=[1.5, 2.5])
        payload = fdb.build_call_db(_point_binding(), cells, allocator, direct_required=True)

    assert payload is allocator.allocation.data
    assert allocator.allocate_count == 1
    assert allocator.commit_count == 1
    assert allocator.rollback_count == 0
    decoded = fdb.decode_call_db(_point_binding(), payload)
    assert [row.row_id for row in decoded] == [1, 2]


def test_require_context_rolls_back_uncommitted_allocation():
    allocator = fdb.BytearrayAllocator()

    with fdb.call_db_build_context(_point_binding(), allocator):
        cells = fdb.require(fdb.batch(RequirePoint, rows=1))
        cells.fill(row_id=[1], x=[1.5])

    assert allocator.allocate_count == 1
    assert allocator.commit_count == 0
    assert allocator.rollback_count == 1
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = cells[0]


def test_require_context_invalidates_returned_views_after_commit():
    allocator = fdb.BytearrayAllocator()

    with fdb.call_db_build_context(_point_binding(), allocator):
        cells = fdb.require(fdb.batch(RequirePoint, rows=1))
        cells.fill(row_id=[1], x=[1.5])
        fdb.build_call_db(_point_binding(), cells, allocator, direct_required=True)

    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = cells.column.row_id[0]


def test_require_context_must_commit_with_same_allocator():
    allocator = fdb.BytearrayAllocator()
    other_allocator = fdb.BytearrayAllocator()

    with fdb.call_db_build_context(_point_binding(), allocator):
        cells = fdb.require(fdb.batch(RequirePoint, rows=1))
        cells.fill(row_id=[1], x=[1.5])
        with pytest.raises(ValueError, match='same allocator'):
            fdb.build_call_db(_point_binding(), cells, other_allocator, direct_required=True)

    assert allocator.allocate_count == 1
    assert allocator.commit_count == 0
    assert allocator.rollback_count == 1
    assert other_allocator.allocate_count == 0


def test_require_context_values_cannot_use_fallback_build_or_export():
    allocator = fdb.BytearrayAllocator()

    with fdb.call_db_build_context(_point_binding(), allocator):
        cells = fdb.require(fdb.batch(RequirePoint, rows=1))
        cells.fill(row_id=[1], x=[1.5])
        with pytest.raises(ValueError, match='direct_required=True'):
            fdb.build_call_db(_point_binding(), cells, allocator)
        with pytest.raises(ValueError, match='cannot be prepared'):
            fdb.prepare_call_db(_point_binding(), cells)
        with pytest.raises(ValueError, match='cannot be prepared'):
            fdb.encode_call_db(_point_binding(), cells)
        with pytest.raises(ValueError, match='cannot be exported'):
            fdb.try_export_call_db(_point_binding(), cells)

    assert allocator.allocate_count == 1
    assert allocator.commit_count == 0
    assert allocator.rollback_count == 1


def test_require_context_rejects_unknown_size_strings_before_allocation():
    allocator = fdb.BytearrayAllocator()

    with fdb.call_db_build_context(_text_feature_binding(), allocator):
        with pytest.raises(fdb.FastdbUnsupportedDirectBuildError, match='require-context direct'):
            fdb.require(fdb.batch(RequireText, rows=2))

    assert allocator.allocate_count == 0


def test_require_context_rejects_non_columnar_batch_profile_before_allocation():
    allocator = fdb.BytearrayAllocator()

    with fdb.call_db_build_context(_point_binding(), allocator):
        with pytest.raises(ValueError, match='columnar-compatible'):
            fdb.require(fdb.batch(RequirePoint, rows=1, profile='object_graph'))

    assert allocator.allocate_count == 0


def test_build_call_db_direct_allows_prepacked_string_feature_columns():
    texts = fdb.require(fdb.batch(RequireText, rows=2))
    texts.fill(value=['left', 'right'])
    allocator = fdb.BytearrayAllocator()

    payload = fdb.build_call_db(
        _text_feature_binding(),
        texts,
        allocator,
        direct_required=True,
    )
    decoded = fdb.decode_call_db(_text_feature_binding(), payload)

    assert allocator.allocate_count == 1
    assert [row.value for row in decoded] == ['left', 'right']


def test_build_call_db_direct_accepts_native_final_backing_resource():
    texts = fdb.require(fdb.batch(RequireText, rows=2))
    texts.fill(value=['left', 'right'])
    resource = fdb.HeapFinalBackingResource()

    allocation = fdb.build_call_db(
        _text_feature_binding(),
        texts,
        resource,
        direct_required=True,
    )
    decoded = fdb.decode_call_db(_text_feature_binding(), allocation.to_bytes())

    assert isinstance(allocation, fdb.FinalBackingAllocation)
    assert resource.allocation_count() == 1
    assert resource.commit_count() == 1
    assert resource.rollback_count() == 0
    assert allocation.committed()
    assert allocation.used_size() > 0
    assert [row.value for row in decoded] == ['left', 'right']


def test_build_call_db_direct_handles_prepacked_empty_string_feature_columns():
    texts = fdb.require(fdb.batch(RequireText, rows=2))
    texts.fill(value=['', ''])
    resource = fdb.HeapFinalBackingResource()

    allocation = fdb.build_call_db(
        _text_feature_binding(),
        texts,
        resource,
        direct_required=True,
    )
    decoded = fdb.decode_call_db(_text_feature_binding(), allocation)

    assert resource.allocation_count() == 1
    assert resource.commit_count() == 1
    assert resource.rollback_count() == 0
    assert [row.value for row in decoded] == ['', '']


def test_view_call_db_loads_committed_native_final_backing_with_owner_lifetime():
    cells = _filled_points(2)
    resource = fdb.HeapFinalBackingResource()
    allocation = fdb.build_call_db(
        _point_binding(),
        cells,
        resource,
        direct_required=True,
    )
    owner = fdb.FdbViewOwner(checked=True, writeable=False)

    view = fdb.view_call_db(_point_binding(), allocation, owner=owner)
    logical = view.logical_value()

    assert allocation._readonly_buffer().readonly is True
    assert isinstance(logical, fdb.Batch)
    assert [row.row_id for row in logical] == [0, 1]
    assert view.backing_owner is allocation

    fdb.invalidate(owner)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = logical[0].row_id


def test_native_final_backing_logical_value_keeps_owner_without_view_handle():
    cells = _filled_points(2)
    resource = fdb.HeapFinalBackingResource()

    def build_allocation():
        return fdb.build_call_db(
            _point_binding(),
            cells,
            resource,
            direct_required=True,
        )

    logical = fdb.view_call_db(_point_binding(), build_allocation()).logical_value()
    gc.collect()

    assert [row.row_id for row in logical] == [0, 1]


def test_require_context_accepts_native_final_backing_resource_for_direct_fill():
    resource = fdb.HeapFinalBackingResource()

    with fdb.call_db_build_context(_point_binding(), resource):
        cells = fdb.require(fdb.batch(RequirePoint, rows=2))
        cells.fill(row_id=[1, 2], x=[1.5, 2.5])
        allocation = fdb.build_call_db(_point_binding(), cells, resource, direct_required=True)

    decoded = fdb.decode_call_db(_point_binding(), allocation.to_bytes())

    assert isinstance(allocation, fdb.FinalBackingAllocation)
    assert resource.allocation_count() == 1
    assert resource.commit_count() == 1
    assert resource.rollback_count() == 0
    assert allocation.committed()
    assert [row.row_id for row in decoded] == [1, 2]
    with pytest.raises(RuntimeError, match='already committed'):
        allocation._writable_buffer()
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = cells.column.row_id[0]


def test_require_context_rolls_back_native_final_backing_resource():
    resource = fdb.HeapFinalBackingResource()

    with fdb.call_db_build_context(_point_binding(), resource):
        cells = fdb.require(fdb.batch(RequirePoint, rows=1))
        cells.fill(row_id=[1], x=[1.5])

    assert resource.allocation_count() == 1
    assert resource.commit_count() == 0
    assert resource.rollback_count() == 1
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = cells[0]


def test_build_call_db_rolls_back_when_writer_fails():
    class ReadOnlyAllocation:
        def __init__(self, nbytes: int):
            self._buffer = bytes(nbytes)
            self.rolled_back = False

        @property
        def buffer(self):
            return memoryview(self._buffer)

        def commit(self, used_size: int):
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


def test_strict_direct_build_rejects_native_final_backing_before_allocation():
    texts = fdb.require(fdb.array(fdb.STR, rows=2))
    texts.fill(['a', 'b'])
    resource = fdb.HeapFinalBackingResource()

    with pytest.raises(fdb.FastdbUnsupportedDirectBuildError, match='str'):
        fdb.build_call_db(_string_array_binding(), texts, resource, direct_required=True)

    assert resource.allocation_count() == 0
    assert resource.commit_count() == 0
    assert resource.rollback_count() == 0


def test_strict_direct_build_rejects_object_graph_batch_requirement_before_allocation():
    cells = fdb.require(fdb.batch(RequirePoint, rows=1, profile='object_graph'))
    cells.append(RequirePoint(row_id=1, x=1.5))
    allocator = fdb.BytearrayAllocator()

    with pytest.raises(ValueError, match='columnar-compatible'):
        fdb.build_call_db(_point_binding(), cells, allocator, direct_required=True)

    assert allocator.allocate_count == 0


def test_fallback_build_call_db_accepts_native_final_backing_resource():
    texts = fdb.require(fdb.array(fdb.STR, rows=2))
    texts.fill(['left', 'right'])
    resource = fdb.HeapFinalBackingResource()

    allocation = fdb.build_call_db(_string_array_binding(), texts, resource)
    decoded = fdb.decode_call_db(_string_array_binding(), allocation)

    assert isinstance(allocation, fdb.FinalBackingAllocation)
    assert resource.allocation_count() == 1
    assert resource.commit_count() == 1
    assert resource.rollback_count() == 0
    assert decoded == ['left', 'right']


@pytest.mark.parametrize(
    ('binding', 'value', 'pattern'),
    (
        (
            _feature_binding(RequireBlob),
            [RequireBlob(data=b'payload')],
            'bytes|raw payload|direct build',
        ),
        (
            _feature_binding(RequireList),
            [RequireList(values=[1.0, 2.0])],
            'list|direct build',
        ),
        (
            _feature_binding(RequireNode),
            [RequireNode(child=RequireLeaf(value=1.0))],
            'ref|columnar|object_graph',
        ),
        (
            _feature_binding(RequireNode, profile='fastdb.call.object-graph.v1'),
            [RequireNode(child=RequireLeaf(value=1.0))],
            'object-graph|strict direct',
        ),
    ),
)
def test_strict_direct_build_rejects_unsupported_shapes_before_allocation(binding, value, pattern):
    allocator = fdb.BytearrayAllocator()

    with pytest.raises((fdb.FastdbUnsupportedDirectBuildError, TypeError), match=pattern):
        fdb.build_call_db(binding, value, allocator, direct_required=True)

    assert allocator.allocate_count == 0
