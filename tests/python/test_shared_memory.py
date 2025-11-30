import uuid
import fastdb
from multiprocessing import Process, shared_memory

class Point(fastdb.Feature):
    idx: fastdb.U32
    x: fastdb.F64
    y: fastdb.F64
    z: fastdb.F64

class Triangle(fastdb.Feature):
    id: fastdb.U32
    a: Point
    b: Point
    c: Point

def verify_shared_data(shm_name: str):
    try:
        db = fastdb.ORM.load(shm_name)
        
        triangles = db[Triangle][Triangle]
        assert len(triangles) == 1, f"Expected 1 triangle, got {len(triangles)}"
        
        t = triangles[0]
        
        assert t.id == 1
        
        assert t.a.idx == 1
        assert t.a.x == 10.0
        assert t.a.y == 20.0
        assert t.a.z == 30.0
        
        assert t.b.idx == 2
        assert t.b.x == 11.0
        assert t.b.y == 21.0
        assert t.b.z == 31.0
        
        assert t.c.idx == 3
        assert t.c.x == 12.0
        assert t.c.y == 22.0
        assert t.c.z == 32.0
        
    except Exception as e:
        print(f"Verification failed: {e}")
        import traceback
        traceback.print_exc()
        exit(1)
    finally:
        if 'db' in locals():
            db.unlink()

def test_shared_memory():
    shm_name = f"fastdb_test_{uuid.uuid4().hex}"
    
    db = fastdb.ORM.create()
    
    t = Triangle()
    t.id = 1
    t.a = Point(idx=1, x=10.0, y=20.0, z=30.0)
    t.b = Point(idx=2, x=11.0, y=21.0, z=31.0)
    t.c = Point(idx=3, x=12.0, y=22.0, z=32.0)
    
    db.push(t)
    
    db.share(shm_name, close_after=True)
    
    p = Process(target=verify_shared_data, args=(shm_name,))
    p.start()
    p.join()
    
    # Cleanup if child failed
    if p.exitcode != 0:
        try:
            s = shared_memory.SharedMemory(name=shm_name)
            s.unlink()
        except FileNotFoundError:
            pass
            
    assert p.exitcode == 0, "Child process failed verification"
