import fastdb4py
import pytest
from pathlib import Path
from typing import Generator

class Point(fastdb4py.Feature):
    x: fastdb4py.F64
    y: fastdb4py.F64
    z: fastdb4py.F64

class Triangle(fastdb4py.Feature):
    a: Point
    b: Point
    c: Point

class Rectangle(fastdb4py.Feature):
    ta: Triangle
    tb: Triangle

@pytest.fixture
def temp_db_path(tmp_path: Path) -> Generator[str, None, None]:
    db_file = tmp_path / 'test_truncate_block'
    yield str(db_file)
    if db_file.exists():
        db_file.unlink()

def test_truncate_block_logic(temp_db_path: str):
    db = fastdb4py.ORM.truncate([
        fastdb4py.TableDefn(Point, 6),
        fastdb4py.TableDefn(Rectangle, 1),
        fastdb4py.TableDefn(Triangle, 1, 'TA'),
        fastdb4py.TableDefn(Triangle, 1, 'TB'),
    ])
    
    points = db[Point][Point]
    for i in range(6):
        point = points[i]
        point.x = i * 0.1
        point.y = i * 0.2
        point.z = i * 0.3
        
        assert point.x == pytest.approx(i * 0.1)
        assert point.y == pytest.approx(i * 0.2)
        assert point.z == pytest.approx(i * 0.3)
    
    triangle_a = db[Triangle]['TA'][0]
    triangle_a.a = points[0]
    triangle_a.b = points[1]
    triangle_a.c = points[2]
    
    triangle_b = db[Triangle]['TB'][0]
    triangle_b.a = points[3]
    triangle_b.b = points[4]
    triangle_b.c = points[5]
    
    rectangle = db[Rectangle][Rectangle][0]
    rectangle.ta = triangle_a
    rectangle.tb = triangle_b
    
    assert triangle_a.a.x == pytest.approx(0.0)
    assert triangle_a.a.y == pytest.approx(0.0)
    assert triangle_a.a.z == pytest.approx(0.0)
    
    assert triangle_a.b.x == pytest.approx(0.1)
    assert triangle_a.b.y == pytest.approx(0.2)
    assert triangle_a.b.z == pytest.approx(0.3)
    
    assert rectangle.ta.a.x == triangle_a.a.x
    assert rectangle.tb.a.x == triangle_b.a.x
    
    assert rectangle.tb.c.z == pytest.approx(5 * 0.3)
    