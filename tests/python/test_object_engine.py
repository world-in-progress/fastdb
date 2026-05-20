# tests/python/test_object_engine.py
from fastdb4py.decorator import feature
from fastdb4py.object_engine import ObjectEngine
from fastdb4py.layout import Layout
from fastdb4py.type import BOOL, BYTES, F64, U32, STR
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


@feature
class OEStringListPoint:
    names: list[STR]


@feature
class OENestedListPoint:
    values: list[list[F64]]


@feature
class OEDoubleBytesPoint:
    left: BYTES
    right: BYTES


@feature
class OEBytesPoint:
    data: BYTES


@feature
class OEBoolPoint:
    active: BOOL


@feature
class OEBoolListPoint:
    flags: list[BOOL]


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


def test_object_engine_rejects_non_native_scalar_list_in_truncate():
    with pytest.raises(TypeError, match='names: list\\[str\\].*native fixed-width list storage'):
        ObjectEngine.truncate([Layout(OEStringListPoint, 2)])


def test_object_engine_rejects_nested_list_in_truncate():
    with pytest.raises(TypeError, match='values: list\\[list\\].*native fixed-width list storage'):
        ObjectEngine.truncate([Layout(OENestedListPoint, 2)])


def test_object_engine_rejects_non_native_scalar_list_in_push():
    engine = ObjectEngine.create()

    with pytest.raises(TypeError, match='names: list\\[str\\].*native fixed-width list storage'):
        engine.push(OEStringListPoint(names=["a", "b"]))


def test_object_engine_rejects_non_native_scalar_list_before_queue_mutation():
    engine = ObjectEngine.create()

    with pytest.raises(TypeError, match='names: list\\[str\\].*native fixed-width list storage'):
        engine.push(OEStringListPoint(names=["a", "b"]))

    assert engine.count(OEStringListPoint) == 0
    engine.push(OEPoint(x=1.0, y=2.0))
    engine.combine()
    assert engine.count(OEPoint) == 1


def test_object_engine_rejects_nested_list_before_queue_mutation():
    engine = ObjectEngine.create()

    with pytest.raises(TypeError, match='values: list\\[list\\].*native fixed-width list storage'):
        engine.push(OENestedListPoint(values=[[1.0, 2.0]]))

    assert engine.count(OENestedListPoint) == 0
    engine.push(OEPoint(x=1.0, y=2.0))
    engine.combine()
    assert engine.count(OEPoint) == 1


def test_object_engine_rejects_multiple_bytes_fields_in_truncate():
    with pytest.raises(TypeError, match='multiple bytes fields share the feature raw payload'):
        ObjectEngine.truncate([Layout(OEDoubleBytesPoint, 2)])


def test_object_engine_rejects_multiple_bytes_fields_before_queue_mutation():
    engine = ObjectEngine.create()

    with pytest.raises(TypeError, match='multiple bytes fields share the feature raw payload'):
        engine.push(OEDoubleBytesPoint(left=b'a', right=b'b'))

    assert engine.count(OEDoubleBytesPoint) == 0
    engine.push(OEPoint(x=1.0, y=2.0))
    engine.combine()
    assert engine.count(OEPoint) == 1


def test_object_engine_single_bytes_field_round_trips():
    engine = ObjectEngine.create()
    engine.push(OEBytesPoint(data=b'payload'))
    engine.combine()

    restored = engine.get(OEBytesPoint, 0, mode='copy')
    assert restored.data == b'payload'


def test_object_engine_bool_fields_parse_strings_without_truthiness():
    engine = ObjectEngine.create()
    engine.push(OEBoolPoint(active='false'))
    engine.push(OEBoolPoint(active='true'))
    engine.combine()

    assert engine.get(OEBoolPoint, 0, mode='copy').active == 0
    assert engine.get(OEBoolPoint, 1, mode='copy').active == 1


def test_object_engine_bool_fields_reject_ambiguous_strings():
    engine = ObjectEngine.create()
    engine.push(OEBoolPoint(active='maybe'))

    with pytest.raises(ValueError, match='fastdb bool scalar'):
        engine.combine()


def test_object_engine_bool_list_fields_parse_strings_without_truthiness():
    engine = ObjectEngine.create()
    engine.push(OEBoolListPoint(flags=['false', 'true', 0, 1]))
    engine.combine()

    restored = engine.get(OEBoolListPoint, 0, mode='copy')
    assert restored.flags.tolist() == [0, 1, 0, 1]


def test_object_engine_bool_list_fields_reject_ambiguous_strings():
    engine = ObjectEngine.create()
    engine.push(OEBoolListPoint(flags=['false', 'maybe']))

    with pytest.raises(ValueError, match='fastdb bool scalar'):
        engine.combine()


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
