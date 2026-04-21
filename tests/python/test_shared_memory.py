import secrets
from multiprocessing import Process, shared_memory

from fastdb4py.decorator import feature
from fastdb4py.object_engine import ObjectEngine
from fastdb4py.type import U32, F64


@feature
class ShmPoint:
    idx: U32
    x: F64
    y: F64
    z: F64


def verify_shared_data(shm_name: str):
    try:
        db = ObjectEngine.load(shm_name)

        assert db.count(ShmPoint) == 3

        p0 = db.get(ShmPoint, 0, mode='copy')
        assert p0.idx == 1
        assert p0.x == 10.0
        assert p0.y == 20.0
        assert p0.z == 30.0

        p1 = db.get(ShmPoint, 1, mode='copy')
        assert p1.idx == 2
        assert p1.x == 11.0

        p2 = db.get(ShmPoint, 2, mode='copy')
        assert p2.idx == 3
        assert p2.z == 32.0

    except Exception as e:
        print(f"Verification failed: {e}")
        import traceback
        traceback.print_exc()
        exit(1)
    finally:
        ObjectEngine.unlink(shm_name)


def test_shared_memory():
    shm_name = f"fastdb_{secrets.token_hex(8)}"

    db = ObjectEngine.create()

    for i, (idx, x, y, z) in enumerate([
        (1, 10.0, 20.0, 30.0),
        (2, 11.0, 21.0, 31.0),
        (3, 12.0, 22.0, 32.0),
    ]):
        p = ShmPoint(idx=idx, x=x, y=y, z=z)
        db.push(p)

    db.combine()
    db.share(shm_name)

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
