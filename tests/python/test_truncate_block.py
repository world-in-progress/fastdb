import pytest

from fastdb4py.decorator import feature
from fastdb4py.object_engine import ObjectEngine
from fastdb4py.type import F64


@feature
class TBPoint:
    x: F64
    y: F64
    z: F64


@feature
class TBTriangle:
    a: TBPoint
    b: TBPoint
    c: TBPoint


@feature
class TBRectangle:
    ta: TBTriangle
    tb: TBTriangle


def test_truncate_block_logic():
    # Build the graph: 6 points → 2 triangles → 1 rectangle
    pts = [TBPoint(x=i * 0.1, y=i * 0.2, z=i * 0.3) for i in range(6)]

    ta = TBTriangle(a=pts[0], b=pts[1], c=pts[2])
    tb = TBTriangle(a=pts[3], b=pts[4], c=pts[5])
    rect = TBRectangle(ta=ta, tb=tb)

    orm = ObjectEngine.create()
    orm.push(rect)
    orm.combine()

    # Verify counts
    assert orm.count(TBPoint) == 6
    assert orm.count(TBTriangle) == 2
    assert orm.count(TBRectangle) == 1

    # Verify scalar field round-trip via copy
    for i in range(6):
        pt = orm.get(TBPoint, i, mode='copy')
        assert pt.x == pytest.approx(i * 0.1)
        assert pt.y == pytest.approx(i * 0.2)
        assert pt.z == pytest.approx(i * 0.3)

    # TODO: migrate REF traversal assertions when ObjectEngine supports
    # reading back REF fields (currently reader returns None for REFs)
    