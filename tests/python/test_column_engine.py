import secrets

import fastdb4py as fdb
from fastdb4py.decorator import feature
from fastdb4py.column_engine import ColumnEngine
from fastdb4py import core
from fastdb4py.layout import Layout
from fastdb4py.type import BOOL, BYTES, F64, U8, U32, STR
from fastdb4py.view_owner import FdbViewInvalidatedError, FdbViewOwner, invalidate
import numpy as np
import pytest


@feature
class CEPoint:
    x: F64
    y: F64


@feature
class CEStringPoint:
    row_id: U32
    x: F64
    name: STR


@feature
class CEOtherPoint:
    y: F64


@feature
class CEListPoint:
    x: F64
    values: list[F64]


@feature
class CEStringListPoint:
    names: list[STR]


@feature
class CENestedListPoint:
    values: list[list[F64]]


@feature
class CEDoubleBytesPoint:
    left: BYTES
    right: BYTES


@feature
class CEBytesPoint:
    data: BYTES


@feature
class CEBoolPoint:
    active: BOOL


@feature
class CEU8Point:
    value: U8


@feature
class CEBoolListPoint:
    flags: list[BOOL]


def test_column_engine_truncate():
    engine = ColumnEngine.truncate([Layout(CEPoint, 100)])
    tbl = engine.table(CEPoint)
    assert len(tbl) == 100
    tbl.column.x[:] = np.arange(100, dtype=np.float64)
    assert tbl.column.x[0] == 0.0
    assert tbl.column.x[99] == 99.0


def test_column_engine_truncate_keeps_other_tables_after_string_layer():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2), Layout(CEOtherPoint, 3)])

    assert len(engine.table(CEStringPoint)) == 2
    assert len(engine.table(CEOtherPoint)) == 3


@feature
class CENode:
    x: F64
    child: 'CENode'


def test_column_engine_rejects_ref_in_truncate():
    with pytest.raises(TypeError, match="REF"):
        ColumnEngine.truncate([Layout(CENode, 10)])


def test_column_engine_rejects_non_native_scalar_list_in_truncate():
    with pytest.raises(TypeError, match='names: list\\[str\\].*native fixed-width list storage'):
        ColumnEngine.truncate([Layout(CEStringListPoint, 2)])


def test_column_engine_rejects_nested_list_in_truncate():
    with pytest.raises(TypeError, match='values: list\\[list\\].*native fixed-width list storage'):
        ColumnEngine.truncate([Layout(CENestedListPoint, 2)])


def test_column_engine_create_push_combine():
    engine = ColumnEngine.create()
    engine.push(CEPoint(x=1.0, y=2.0))
    engine.push(CEPoint(x=3.0, y=4.0))
    engine.combine()
    tbl = engine.table(CEPoint)
    assert len(tbl) == 2
    assert tbl.column.x[0] == pytest.approx(1.0)
    assert tbl.column.x[1] == pytest.approx(3.0)


def test_column_engine_rejects_non_native_scalar_list_in_push():
    engine = ColumnEngine.create()

    with pytest.raises(TypeError, match='names: list\\[str\\].*native fixed-width list storage'):
        engine.push(CEStringListPoint(names=["a", "b"]))


def test_column_engine_rejects_non_native_scalar_list_in_push_many():
    engine = ColumnEngine.create()

    with pytest.raises(TypeError, match='names: list\\[str\\].*native fixed-width list storage'):
        engine.push_many([CEStringListPoint(names=["a"])])


def test_column_engine_rejects_nested_list_in_push_many():
    engine = ColumnEngine.create()

    with pytest.raises(TypeError, match='values: list\\[list\\].*native fixed-width list storage'):
        engine.push_many([CENestedListPoint(values=[[1.0, 2.0]])])


def test_column_engine_rejects_multiple_bytes_fields_in_push():
    engine = ColumnEngine.create()

    with pytest.raises(TypeError, match='multiple bytes fields share the feature raw payload'):
        engine.push(CEDoubleBytesPoint(left=b'a', right=b'b'))


def test_column_engine_rejects_multiple_bytes_fields_in_push_many():
    engine = ColumnEngine.create()

    with pytest.raises(TypeError, match='multiple bytes fields share the feature raw payload'):
        engine.push_many([CEDoubleBytesPoint(left=b'a', right=b'b')])


def test_column_engine_dynamic_single_bytes_field_round_trips():
    from fastdb4py.reader import copy_feature

    engine = ColumnEngine.create()
    engine.push(CEBytesPoint(data=b'payload'))
    engine.push(CEBytesPoint(data=b'second'))
    engine.combine()

    layer = engine._origin.get_layer(0)
    restored = copy_feature(CEBytesPoint, layer, 0)
    assert restored.data == b'payload'
    restored_second = copy_feature(CEBytesPoint, layer, 1)
    assert restored_second.data == b'second'


def test_column_engine_bool_fields_parse_strings_without_truthiness():
    engine = ColumnEngine.create()
    engine.push(CEBoolPoint(active='false'))
    engine.push(CEBoolPoint(active='true'))
    engine.combine()

    tbl = engine.table(CEBoolPoint)
    assert tbl.column.active[0] == 0
    assert tbl.column.active[1] == 1


def test_column_engine_push_many_bool_fields_parse_strings_without_truthiness():
    engine = ColumnEngine.create()
    engine.push_many([
        CEBoolPoint(active='false'),
        CEBoolPoint(active='true'),
    ])
    engine.combine()

    tbl = engine.table(CEBoolPoint)
    assert tbl.column.active[0] == 0
    assert tbl.column.active[1] == 1


def test_column_engine_bool_fields_reject_ambiguous_strings():
    engine = ColumnEngine.create()

    with pytest.raises(ValueError, match='fastdb bool scalar'):
        engine.push(CEBoolPoint(active='maybe'))


def test_column_engine_bool_list_fields_parse_strings_without_truthiness():
    engine = ColumnEngine.create()
    engine.push(CEBoolListPoint(flags=['false', 'true', 0, 1]))
    engine.combine()

    restored = engine.table(CEBoolListPoint)[0]
    assert restored.flags.tolist() == [0, 1, 0, 1]


def test_column_engine_bool_list_fields_reject_ambiguous_strings():
    engine = ColumnEngine.create()

    with pytest.raises(ValueError, match='fastdb bool scalar'):
        engine.push(CEBoolListPoint(flags=['false', 'maybe']))


def test_column_engine_push_many_bool_fields_reject_before_partial_write():
    engine = ColumnEngine.create()

    with pytest.raises(ValueError, match='fastdb bool scalar'):
        engine.push_many([
            CEBoolPoint(active='true'),
            CEBoolPoint(active='maybe'),
        ])

    engine.push(CEBoolPoint(active='false'))
    engine.combine()

    tbl = engine.table(CEBoolPoint)
    assert len(tbl) == 1
    assert tbl.column.active[0] == 0


def test_column_engine_push_many_bool_fields_reject_before_table_creation():
    engine = ColumnEngine.create()

    with pytest.raises(ValueError, match='fastdb bool scalar'):
        engine.push_many([
            CEBoolPoint(active='true'),
            CEBoolPoint(active='maybe'),
        ])

    assert CEBoolPoint.__name__ not in engine._table_map
    assert CEBoolPoint.__name__ not in engine._table_feature_types


def test_column_engine_fixed_fill_bool_fields_parse_strings_without_truthiness():
    engine = ColumnEngine.truncate([Layout(CEBoolPoint, 2)])
    tbl = engine.table(CEBoolPoint)

    tbl.fill(active=['false', 'true'])

    assert tbl.column.active[0] == 0
    assert tbl.column.active[1] == 1


def test_column_engine_fixed_fill_bool_fields_reject_ambiguous_strings():
    engine = ColumnEngine.truncate([Layout(CEBoolPoint, 2)])
    tbl = engine.table(CEBoolPoint)

    with pytest.raises(ValueError, match='fastdb bool scalar'):
        tbl.fill(active=['true', 'maybe'])


def test_column_engine_fixed_fill_bool_fields_treat_none_as_false():
    engine = ColumnEngine.truncate([Layout(CEBoolPoint, 2)])
    tbl = engine.table(CEBoolPoint)

    tbl.fill(active=[None, True])

    assert tbl.column.active[0] == 0
    assert tbl.column.active[1] == 1


def test_column_engine_fixed_fill_bool_fields_reject_scalar_string_column():
    engine = ColumnEngine.truncate([Layout(CEBoolPoint, 2)])
    tbl = engine.table(CEBoolPoint)

    with pytest.raises(TypeError, match='iterable of bool items'):
        tbl.fill(active='false')


def test_column_engine_fixed_fill_bool_fields_reject_multidimensional_column():
    engine = ColumnEngine.truncate([Layout(CEBoolPoint, 2)])
    tbl = engine.table(CEBoolPoint)

    with pytest.raises(ValueError, match='1-D column'):
        tbl.fill(active=np.array([[True], [False]], dtype=np.bool_))


def test_column_engine_fixed_fill_u8_fields_keep_numeric_cast_path():
    engine = ColumnEngine.truncate([Layout(CEU8Point, 2)])
    tbl = engine.table(CEU8Point)

    tbl.fill(value=['1', '2'])

    assert tbl.column.value[0] == 1
    assert tbl.column.value[1] == 2


def test_low_level_list_push_rejects_unknown_element_type():
    from fastdb4py.push import _set_list_field
    from fastdb4py.registry import FieldDef
    from fastdb4py.type import OriginFieldType

    field = FieldDef(
        name='values',
        field_type=OriginFieldType.list,
        field_id=0,
        cpp_type=0,
        list_elem_type=None,
    )

    with pytest.raises(TypeError, match='values: list\\[None\\].*native fixed-width list storage'):
        _set_list_field(object(), field, [1])


def test_column_engine_push_many():
    engine = ColumnEngine.create()
    points = [CEPoint(x=float(i), y=float(i * 2)) for i in range(50)]
    engine.push_many(points)
    engine.combine()
    tbl = engine.table(CEPoint)
    assert len(tbl) == 50


def test_column_engine_iter_reuse():
    engine = ColumnEngine.truncate([Layout(CEPoint, 10)])
    tbl = engine.table(CEPoint)
    tbl.column.x[:] = np.arange(10, dtype=np.float64)
    tbl.column.y[:] = np.arange(10, dtype=np.float64) * 2
    vals = []
    for feat in tbl.iter_reuse():
        vals.append(feat.x)
    assert vals == [pytest.approx(float(i)) for i in range(10)]


def test_table_getitem_rejects_negative_index_underflow():
    engine = ColumnEngine.truncate([Layout(CEPoint, 2)])
    tbl = engine.table(CEPoint)

    with pytest.raises(IndexError, match='out of range'):
        tbl[-3]


def test_table_iter_reuse_locks_row_materialization(monkeypatch):
    from fastdb4py.orm import table as table_module

    class LockProbe:
        def __init__(self):
            self.active = False
            self.unlocked_reads = 0
            self.enter_count = 0

        def __enter__(self):
            self.enter_count += 1
            self.active = True

        def __exit__(self, exc_type, exc, tb):
            self.active = False
            return False

    class FakeOrigin:
        def __init__(self, lock: LockProbe):
            self._lock = lock

        def get_feature_count(self):
            return 2

        def tryGetFeature(self, index):
            if not self._lock.active:
                self._lock.unlocked_reads += 1
            return f'feature-{index}'

    class FakeMappedFeature:
        def __init__(self, feature_type, feat, schema, *, owner=None):
            self._feat = feat

    lock = LockProbe()
    fake_origin = FakeOrigin(lock)
    tbl = table_module.Table()
    tbl._origin = fake_origin
    tbl._feature_type = CEPoint
    tbl._read_lock = lock

    monkeypatch.setattr(table_module.core, 'WxLayerTable', FakeOrigin)
    monkeypatch.setattr(table_module, 'get_schema', lambda feature_type: object())
    monkeypatch.setattr(table_module, 'MappedFeature', FakeMappedFeature)

    seen = [proxy._feat for proxy in tbl.iter_reuse()]

    assert seen == ['feature-0', 'feature-1']
    assert lock.enter_count == 2
    assert lock.unlocked_reads == 0


def test_column_engine_fill():
    engine = ColumnEngine.truncate([Layout(CEPoint, 5)])
    tbl = engine.table(CEPoint)
    tbl.fill(
        x=np.array([1, 2, 3, 4, 5], dtype=np.float64),
        y=np.array([10, 20, 30, 40, 50], dtype=np.float64),
    )
    assert tbl.column.x[2] == pytest.approx(3.0)
    assert tbl.column.y[4] == pytest.approx(50.0)


def test_table_fill_coerces_float32_into_f64_columns():
    engine = ColumnEngine.truncate([Layout(CEPoint, 3)])
    tbl = engine.table(CEPoint)

    tbl.fill(
        x=np.array([1.5, 2.5, 3.5], dtype=np.float32),
        y=np.array([10.0, 20.0, 30.0], dtype=np.float32),
    )

    np.testing.assert_array_equal(
        tbl.column.x,
        np.array([1.5, 2.5, 3.5], dtype=np.float64),
    )
    np.testing.assert_array_equal(
        tbl.column.y,
        np.array([10.0, 20.0, 30.0], dtype=np.float64),
    )


def test_table_fill_rejects_non_fixed_tables():
    engine = ColumnEngine.create()
    engine.push(CEPoint(x=1.0, y=2.0))
    tbl = engine._table_map[CEPoint.__name__]

    with pytest.raises(RuntimeError, match='fixed-scale tables'):
        tbl.fill(x=np.array([1.0], dtype=np.float64))


def test_table_fill_rejects_empty_call():
    engine = ColumnEngine.truncate([Layout(CEPoint, 2)])
    tbl = engine.table(CEPoint)

    with pytest.raises(ValueError, match='at least one column'):
        tbl.fill()


def test_table_fill_accepts_mixed_numeric_and_string_columns():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 3)])
    tbl = engine.table(CEStringPoint)

    tbl.fill(
        row_id=np.array([1, 2, 3], dtype=np.uint32),
        x=np.array([1.0, 2.0, 3.0], dtype=np.float64),
        name=["a", "be", "中"],
    )

    assert tbl.column.name.to_pylist() == ["a", "be", "中"]
    np.testing.assert_array_equal(
        tbl.column.row_id,
        np.array([1, 2, 3], dtype=np.uint32),
    )


def test_owner_bound_truncated_table_preserves_direct_fill_and_lifetime():
    engine = ColumnEngine.truncate([Layout(CEPoint, 2)])
    owner = FdbViewOwner(checked=True, writeable=True)
    tbl = engine.table(CEPoint, owner=owner, writeable=True)

    tbl.fill(x=[1.5, 2.5], y=[3.5, 4.5])

    assert tbl[0].x == 1.5
    invalidate(owner)
    with pytest.raises(FdbViewInvalidatedError):
        _ = tbl[0].x


def test_table_fill_accepts_string_only_columns():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)

    tbl.fill(name=["left", "right"])

    assert tbl.column.name.to_pylist() == ["left", "right"]


def test_native_build_post_into_buffer_matches_memory_stream():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2, name='return_0')])
    tbl = engine.table(CEStringPoint, name='return_0')
    tbl.fill(
        row_id=np.array([1, 2], dtype=np.uint32),
        x=np.array([1.5, 2.5], dtype=np.float64),
        name=['left', 'right'],
    )

    build = engine._fixed_build
    memory_stream = core.WxMemoryStream()
    build.post(memory_stream)
    reference = memory_stream.data().to_bytes()

    destination = bytearray(build.byte_length())
    written = build.post_into_buffer(destination)

    assert build.byte_length() == len(reference)
    assert written == len(reference)
    assert bytes(destination) == reference


def test_regular_truncate_keeps_materialized_native_table_buffer():
    engine = ColumnEngine.truncate([Layout(CEPoint, 2, name='return_0')])

    assert engine._fixed_build.table_buffer_bytes() > 0


def test_native_build_posts_through_final_backing_resource():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2, name='return_0')])
    tbl = engine.table(CEStringPoint, name='return_0')
    tbl.fill(
        row_id=np.array([1, 2], dtype=np.uint32),
        x=np.array([1.5, 2.5], dtype=np.float64),
        name=['left', 'right'],
    )

    build = engine._fixed_build
    memory_stream = core.WxMemoryStream()
    build.post(memory_stream)
    reference = memory_stream.data().to_bytes()

    resource = core.WxHeapFinalBackingResource()
    allocation = build.post_to_final_backing(resource)

    assert resource.allocation_count() == 1
    assert resource.commit_count() == 1
    assert resource.rollback_count() == 0
    assert allocation.size() == build.byte_length()
    assert allocation.used_size() == build.byte_length()
    assert allocation.committed()
    assert not allocation.rolled_back()
    assert allocation.to_bytes() == reference


def test_native_heap_scratch_allocator_exposes_separate_core_role():
    allocator = fdb.HeapScratchAllocator()
    allocation = allocator._allocate_for_context(16)
    buffer = allocation._writable_buffer()

    buffer[:4] = b'fdb!'

    assert isinstance(allocation, fdb.ScratchAllocation)
    assert isinstance(allocator, fdb.ScratchAllocator)
    assert allocation.size() == 16
    assert bytes(buffer[:4]) == b'fdb!'
    assert allocator.allocation_count() == 1


def test_native_final_backing_resource_does_not_expose_uncommitted_allocation_surface():
    resource = core.WxHeapFinalBackingResource()
    assert not hasattr(resource, 'allocate')

    with pytest.raises(AttributeError):
        core.WxHeapFinalBackingAllocation(8)


def test_native_final_backing_allocation_cannot_be_read_before_commit():
    resource = fdb.HeapFinalBackingResource()
    allocation = resource._allocate_for_context(8)

    with pytest.raises(RuntimeError, match='not committed'):
        allocation._readonly_buffer()
    with pytest.raises(RuntimeError, match='not committed'):
        allocation.to_bytes()

    allocation.rollback()
    with pytest.raises(RuntimeError, match='rolled back|not committed'):
        allocation.to_bytes()


def test_table_fill_round_trips_empty_strings():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 3)])
    tbl = engine.table(CEStringPoint)

    tbl.fill(name=["", "mid", ""])

    assert tbl.column.name.to_pylist() == ["", "mid", ""]


def test_table_fill_rejects_mismatched_lengths():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)

    with pytest.raises(ValueError, match='name.*expected 2.*got 1'):
        tbl.fill(
            row_id=np.array([1, 2], dtype=np.uint32),
            name=["only-one"],
        )


def test_table_fill_rejects_unknown_field_name():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)

    with pytest.raises(AttributeError, match='missing'):
        tbl.fill(missing=np.array([1.0, 2.0], dtype=np.float64))


def test_table_fill_rejects_list_field_with_clear_error():
    engine = ColumnEngine.truncate([Layout(CEListPoint, 2)])
    tbl = engine.table(CEListPoint)

    with pytest.raises(TypeError, match='values.*does not support'):
        tbl.fill(values=[[1.0], [2.0]])


def test_table_fill_validation_failure_does_not_mutate_existing_values():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)

    tbl.fill(
        row_id=np.array([7, 8], dtype=np.uint32),
        x=np.array([1.0, 2.0], dtype=np.float64),
        name=["aa", "bb"],
    )

    with pytest.raises(ValueError, match='name.*expected 2.*got 1'):
        tbl.fill(
            row_id=np.array([9, 10], dtype=np.uint32),
            name=["only-one"],
        )

    np.testing.assert_array_equal(
        tbl.column.row_id,
        np.array([7, 8], dtype=np.uint32),
    )
    assert tbl.column.name.to_pylist() == ["aa", "bb"]


def test_table_fill_invalidates_fixed_writer_after_bulk_setter_failure():
    engine = ColumnEngine.truncate([Layout(CEPoint, 2)])
    tbl = engine.table(CEPoint)

    class FailingLayerBuild:
        def __init__(self):
            self.numeric_calls = 0

        def set_numeric_column_bulk(self, field_index, payload):
            self.numeric_calls += 1
            if self.numeric_calls == 2:
                raise RuntimeError("boom")

        def set_string_column_bulk(self, field_index, offsets, data):
            raise AssertionError("unexpected string bulk write")

    engine._fixed_layer_builds[CEPoint.__name__] = FailingLayerBuild()

    with pytest.raises(RuntimeError, match="boom"):
        tbl.fill(
            x=np.array([1.0, 2.0], dtype=np.float64),
            y=np.array([3.0, 4.0], dtype=np.float64),
        )

    assert engine._fixed_build is None
    assert engine._fixed_layer_builds == {}
    assert engine._fixed_table_fields == {}
    assert tbl._fixed_fill_handler is None

    with pytest.raises(RuntimeError, match="read-only fixed tables"):
        tbl.fill(x=np.array([5.0, 6.0], dtype=np.float64))


def test_loaded_fixed_table_rejects_fill():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(
        row_id=np.array([1, 2], dtype=np.uint32),
        x=np.array([1.0, 2.0], dtype=np.float64),
        name=["aa", "bb"],
    )

    shm_name = f"fastdb_fill_{secrets.token_hex(4)}"
    loaded = None
    try:
        engine.share(shm_name)
        loaded = ColumnEngine.load(shm_name)
        with pytest.raises(RuntimeError, match="read-only"):
            loaded.table(CEStringPoint).fill(name=["x", "y"])
    finally:
        if loaded is not None:
            loaded.unlink()
            engine.close()
        else:
            engine.unlink()


def test_shared_writer_fixed_table_rejects_fill():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(
        row_id=np.array([1, 2], dtype=np.uint32),
        x=np.array([1.0, 2.0], dtype=np.float64),
        name=["aa", "bb"],
    )

    shm_name = f"fastdb_fill_writer_{secrets.token_hex(4)}"
    try:
        engine.share(shm_name)
        with pytest.raises(RuntimeError, match="read-only fixed tables"):
            engine.table(CEStringPoint).fill(name=["x", "y"])
    finally:
        engine.unlink()


def test_table_fill_overwrites_prior_values_on_repeated_success():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)

    tbl.fill(
        row_id=np.array([1, 2], dtype=np.uint32),
        x=np.array([1.0, 2.0], dtype=np.float64),
        name=["aa", "bb"],
    )
    tbl.fill(
        row_id=np.array([9, 10], dtype=np.uint32),
        x=np.array([9.0, 10.0], dtype=np.float64),
        name=["left", "right"],
    )

    np.testing.assert_array_equal(
        tbl.column.row_id,
        np.array([9, 10], dtype=np.uint32),
    )
    np.testing.assert_array_equal(
        tbl.column.x,
        np.array([9.0, 10.0], dtype=np.float64),
    )
    assert tbl.column.name.to_pylist() == ["left", "right"]


def test_table_fill_preserves_other_tables_across_snapshot_publish():
    engine = ColumnEngine.truncate([Layout(CEPoint, 2), Layout(CEStringPoint, 2)])
    point_tbl = engine.table(CEPoint)
    string_tbl = engine.table(CEStringPoint)

    point_tbl.fill(
        x=np.array([1.0, 2.0], dtype=np.float64),
        y=np.array([10.0, 20.0], dtype=np.float64),
    )
    string_tbl.fill(
        row_id=np.array([7, 8], dtype=np.uint32),
        x=np.array([3.0, 4.0], dtype=np.float64),
        name=["aa", "bb"],
    )

    np.testing.assert_array_equal(
        point_tbl.column.x,
        np.array([1.0, 2.0], dtype=np.float64),
    )
    np.testing.assert_array_equal(
        point_tbl.column.y,
        np.array([10.0, 20.0], dtype=np.float64),
    )


def test_column_engine_rejects_ref_in_push():
    engine = ColumnEngine.create()
    node = CENode(x=1.0, child=None)
    with pytest.raises(TypeError, match="REF"):
        engine.push(node)


def test_column_engine_rejects_ref_in_push_many():
    engine = ColumnEngine.create()
    nodes = [CENode(x=float(i), child=None) for i in range(5)]
    with pytest.raises(TypeError, match="REF"):
        engine.push_many(nodes)
