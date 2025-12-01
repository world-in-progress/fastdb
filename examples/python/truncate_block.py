import fastdb4py
from pathlib import Path

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
    
if __name__ == '__main__':
    TEMP_DB_PATH = Path.cwd() / 'truncate_block'
    
    # Create and save a block with fixed scale for Points and Triangles
    db = fastdb4py.ORM.truncate([
        fastdb4py.TableDefn(Point, 6),
        fastdb4py.TableDefn(Rectangle, 1),
        fastdb4py.TableDefn(Triangle, 1, 'TA'),
        fastdb4py.TableDefn(Triangle, 1, 'TB'),
    ])
    
    # # Load the block and populate it with data
    # db = fastdb.ORM.load(str(TEMP_DB_PATH), from_file=True)
    
    points = db[Point][Point]
    for i in range(6):
        point = points[i]
        point.x = i * 0.1
        point.y = i * 0.2
        point.z = i * 0.3
        print(f'Point {i}: x={point.x}, y={point.y}, z={point.z}')
    
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

    # Check the stored data
    print(f'Triangle A: pointA=({triangle_a.a.x}, {triangle_a.a.y}, {triangle_a.a.z}), pointB=({triangle_a.b.x}, {triangle_a.b.y}, {triangle_a.b.z}), pointC=({triangle_a.c.x}, {triangle_a.c.y}, {triangle_a.c.z})')
    print(f'Triangle B: pointA=({triangle_b.a.x}, {triangle_b.a.y}, {triangle_b.a.z}), pointB=({triangle_b.b.x}, {triangle_b.b.y}, {triangle_b.b.z}), pointC=({triangle_b.c.x}, {triangle_b.c.y}, {triangle_b.c.z})')
    print(f'Rectangle: triangleA=(({rectangle.ta.a.x}, {rectangle.ta.a.y}, {rectangle.ta.a.z}), ({rectangle.ta.b.x}, {rectangle.ta.b.y}, {rectangle.ta.b.z}), ({rectangle.ta.c.x}, {rectangle.ta.c.y}, {rectangle.ta.c.z})), triangleB=(({rectangle.tb.a.x}, {rectangle.tb.a.y}, {rectangle.tb.a.z}), ({rectangle.tb.b.x}, {rectangle.tb.b.y}, {rectangle.tb.b.z}), ({rectangle.tb.c.x}, {rectangle.tb.c.y}, {rectangle.tb.c.z}))')
    # Clean up
    # TEMP_DB_PATH.unlink()