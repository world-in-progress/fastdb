import os
import sys
from multiprocessing import Process
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')))

from python import fastdb

class Point(fastdb.FeaturePipe):
    idx: fastdb.U32
    x: fastdb.F64
    y: fastdb.F64
    z: fastdb.F64

class Triangle(fastdb.FeaturePipe):
    id: fastdb.U32
    a: Point
    b: Point
    c: Point

def process_task(name):
    block = fastdb.Block.load(name)
    t = block.get(Triangle, 'haha')
    print(f'Triangle id: {t.id}')
    print(f'Point a idx: {t.a.idx}, x: {t.a.x}, y: {t.a.y}, z: {t.a.z}')
    print(f'Point b idx: {t.b.idx}, x: {t.b.x}, y: {t.b.y}, z: {t.b.z}')
    print(f'Point c idx: {t.c.idx}, x: {t.c.x}, y: {t.c.y}, z: {t.c.z}')
    
    # Clean up
    block.unlink()

if __name__ == '__main__':
    TEMP_DB_PATH = 'tmp_shared_memory'
    
    block = fastdb.Block.create()
    
    t = Triangle()
    t.id = 1
    t.a = Point(idx=1, x=10.0, y=20.0, z=30.0)
    t.b = Point(idx=2, x=11.0, y=21.0, z=31.0)
    t.c = Point(idx=3, x=12.0, y=22.0, z=32.0)
    
    block.push(t, 'haha')
    block.share(str(TEMP_DB_PATH), close_after=True)
    
    a = Process(target=process_task, args=(str(TEMP_DB_PATH), ))
    a.start()
    a.join()
