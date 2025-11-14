import os
import sys
from pathlib import Path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))

from python import fastdb

class Point(fastdb.FeaturePipe):
    x: fastdb.F64
    y: fastdb.F64
    z: fastdb.F64

class Triangle(fastdb.FeaturePipe):
    a: fastdb.U32
    b: fastdb.U32
    c: fastdb.U32
    
if __name__ == '__main__':
    TEMP_DB_PATH = Path.cwd() / 'truncate_block'
    
    # Create and save a block with fixed scale for Points and Triangles
    fastdb.Block.truncate([
        fastdb.BlockScale(Point, 6),
        fastdb.BlockScale(Triangle, 1, 'TA'),
        fastdb.BlockScale(Triangle, 1, 'TB'),
    ]).save(str(TEMP_DB_PATH))
    
    # Load the block and populate it with data
    block = fastdb.Block.load(str(TEMP_DB_PATH), from_file=True)
    
    points = block[Point][Point]
    for i in range(6):
        point = points[i]
        point.x = i * 0.1
        point.y = i * 0.2
        point.z = i * 0.3
        print(f'Point {i}: x={point.x}, y={point.y}, z={point.z}')
    
    triangle_a = block[Triangle]['TA'][0]
    triangle_a.a = 0
    triangle_a.b = 1
    triangle_a.c = 2
    
    triangle_b = block[Triangle]['TB'][0]
    triangle_b.a = 3
    triangle_b.b = 4
    triangle_b.c = 5

    # Check the stored data
    print(f'Triangle A: pointA=({points[triangle_a.a].x}, {points[triangle_a.a].y}, {points[triangle_a.a].z}), pointB=({points[triangle_a.b].x}, {points[triangle_a.b].y}, {points[triangle_a.b].z}), pointC=({points[triangle_a.c].x}, {points[triangle_a.c].y}, {points[triangle_a.c].z}))')
    print(f'Triangle B: pointA=({points[triangle_b.a].x}, {points[triangle_b.a].y}, {points[triangle_b.a].z}), pointB=({points[triangle_b.b].x}, {points[triangle_b.b].y}, {points[triangle_b.b].z}), pointC=({points[triangle_b.c].x}, {points[triangle_b.c].y}, {points[triangle_b.c].z}))')
    
    # Clean up
    TEMP_DB_PATH.unlink()