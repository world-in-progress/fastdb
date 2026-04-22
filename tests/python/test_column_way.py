import pytest

from fastdb4py.decorator import feature
from fastdb4py.column_engine import ColumnEngine
from fastdb4py.layout import Layout
from fastdb4py.type import F64


@feature
class Point:
    x: F64
    y: F64
    z: F64

def test_column_way():
    import numpy as np
    db = ColumnEngine.truncate([Layout(Point, 5)])

    ps = db.table(Point)

    # Write via column API
    ps.column.x[:] = np.arange(5, dtype=np.float64)
    ps.column.y[:] = np.arange(5, dtype=np.float64) * 2
    ps.column.z[:] = np.arange(5, dtype=np.float64) * 3

    for i in range(5):
        assert ps.column.x[i] == pytest.approx(i * 1.0)
        assert ps.column.y[i] == pytest.approx(i * 2.0)
        assert ps.column.z[i] == pytest.approx(i * 3.0)

    # Modify column in-place
    xs = ps.column.x
    for i in range(len(xs)):
        xs[i] = xs[i] + 1

    for i in range(5):
        assert ps.column.x[i] == pytest.approx(i * 1.0 + 1)
