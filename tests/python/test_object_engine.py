# tests/python/test_object_engine.py
from fastdb4py.decorator import feature
from fastdb4py.object_engine import ObjectEngine
from fastdb4py.layout import Layout
from fastdb4py.type import F64, U32, STR
import numpy as np
import pytest


@feature
class OEPoint:
    x: F64
    y: F64


@feature
class OELeaf:
    val: F64


@feature
class OENode:
    val: F64
    child: OELeaf


def test_object_engine_create_push_combine():
    engine = ObjectEngine.create()
    engine.push(OEPoint(x=1.0, y=2.0))
    engine.push(OEPoint(x=3.0, y=4.0))
    engine.combine()
    obj = engine.get(OEPoint, 0, mode='copy')
    assert obj.x == pytest.approx(1.0)


def test_object_engine_ref_support():
    leaf = OELeaf(val=1.0)
    root = OENode(val=2.0, child=leaf)
    engine = ObjectEngine.create()
    engine.push(root)
    engine.combine()
    result = engine.get(OENode, 0, mode='copy')
    assert result.val == pytest.approx(2.0)
    leaf_result = engine.get(OELeaf, 0, mode='copy')
    assert leaf_result.val == pytest.approx(1.0)


def test_object_engine_truncate_without_refs():
    engine = ObjectEngine.truncate([Layout(OEPoint, 50)])
    tbl = engine.table(OEPoint)
    assert len(tbl) == 50
    tbl.column.x[:] = np.arange(50, dtype=np.float64)
    assert tbl.column.x[0] == pytest.approx(0.0)


def test_object_engine_columnar_access():
    engine = ObjectEngine.create()
    for i in range(100):
        engine.push(OEPoint(x=float(i), y=float(i * 2)))
    engine.combine()
    tbl = engine.table(OEPoint)
    assert len(tbl) == 100
    xs = tbl.column.x
    assert xs[50] == pytest.approx(50.0)


def test_object_engine_iter():
    engine = ObjectEngine.create()
    for i in range(5):
        engine.push(OEPoint(x=float(i), y=float(i * 10)))
    engine.combine()
    results = list(engine.iter(OEPoint, mode='copy'))
    assert len(results) == 5
    assert results[3].x == pytest.approx(3.0)


def test_object_engine_count():
    engine = ObjectEngine.create()
    engine.push(OEPoint(x=1.0, y=2.0))
    engine.push(OEPoint(x=3.0, y=4.0))
    assert engine.count(OEPoint) == 2
    engine.combine()
    assert engine.count(OEPoint) == 2
