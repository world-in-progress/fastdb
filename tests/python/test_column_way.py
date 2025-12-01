import fastdb
import pytest

class Point(fastdb.Feature):
    x: fastdb.F64
    y: fastdb.F64
    z: fastdb.F64

def test_column_way():
    db = fastdb.ORM.truncate([
        fastdb.TableDefn(Point, 10),
        fastdb.TableDefn(Point, 5, 'PointA'),
    ])
    
    ps = db[Point]['PointA']
    for i in range(5):
        point = ps[i]
        point.x = i * 1.0
        point.y = i * 2.0
        point.z = i * 3.0
        
        assert point.x == pytest.approx(i * 1.0)
        assert point.y == pytest.approx(i * 2.0)
        assert point.z == pytest.approx(i * 3.0)
    
    xs = ps.column.x
    for i in range(len(xs)):
        xs[i] = xs[i] + 1
        
    for i in range(5):
        point = ps[i]
        assert point.x == pytest.approx(i * 1.0 + 1)
