import secrets

from fastdb4py.decorator import feature
from fastdb4py.column_engine import ColumnEngine
from fastdb4py.layout import Layout
from fastdb4py.type import F64, U32, STR
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


def test_column_engine_create_push_combine():
    engine = ColumnEngine.create()
    engine.push(CEPoint(x=1.0, y=2.0))
    engine.push(CEPoint(x=3.0, y=4.0))
    engine.combine()
    tbl = engine.table(CEPoint)
    assert len(tbl) == 2
    assert tbl.column.x[0] == pytest.approx(1.0)
    assert tbl.column.x[1] == pytest.approx(3.0)


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


def test_table_fill_accepts_string_only_columns():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)

    tbl.fill(name=["left", "right"])

    assert tbl.column.name.to_pylist() == ["left", "right"]


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
