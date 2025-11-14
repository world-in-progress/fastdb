import os
import sys
from pathlib import Path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))

from python import fastdb

class Point(fastdb.FeaturePipe):
    x: fastdb.F64
    y: fastdb.F64
    z: fastdb.F64

if __name__ == '__main__':
    TEMP_DB_PATH = Path.cwd() / 'alias_test_block'
    
    fastdb.Block.truncate([
        fastdb.BlockScale(Point, 10),
        fastdb.BlockScale(Point, 5, 'PointA'),
    ]).save(str(TEMP_DB_PATH))
    
    block = fastdb.Block.load(str(TEMP_DB_PATH), from_file=True)
    ps = block[Point]['PointA']
    for i in range(5):
        point = ps[i]
        point.x = i * 1.0
        point.y = i * 2.0
        point.z = i * 3.0
        print(f'PointA {i}: x={point.x}, y={point.y}, z={point.z}')
    
    block.save(str(TEMP_DB_PATH))
    block = fastdb.Block.load(str(TEMP_DB_PATH), from_file=True)
    
    points_a = block[Point]['PointA']
    points_p = block[Point][Point]
        
    for p in points_a:
        print(f'PointA before modify: x={p.x}, y={p.y}, z={p.z}')
    
    xs = points_a.column.x
    for i in range(len(xs)):
        xs[i] = xs[i] + 1
        
    for p in points_a:
        print(f'x of PointA after modify: {p.x}')
        
    # Clean up
    TEMP_DB_PATH.unlink()