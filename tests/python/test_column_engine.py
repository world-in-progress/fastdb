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


def test_column_engine_truncate():
    engine = ColumnEngine.truncate([Layout(CEPoint, 100)])
    tbl = engine.table(CEPoint)
    assert len(tbl) == 100
    tbl.column.x[:] = np.arange(100, dtype=np.float64)
    assert tbl.column.x[0] == 0.0
    assert tbl.column.x[99] == 99.0


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
