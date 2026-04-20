"""
Decorator-based ORM2: Shared Memory
=====================================
Demonstrates cross-process data sharing:
  1. Build + share with ORM2.share()
  2. Load in child process with ORM2.load()
  3. Clean up with ORM2.unlink()
"""

from multiprocessing import Process
from fastdb4py import feature, ORM2, F64, U32, STR


@feature
class Sensor:
    name: STR
    value: F64
    channel: U32


def reader_process(shm_name: str):
    """Child process: load from shared memory and read."""
    orm = ORM2.load(shm_name)
    print(f"  [child] loaded {orm.count(Sensor)} sensors")
    for s in orm.iter(Sensor, mode='copy'):
        print(f"  [child] {s.name}: value={s.value:.2f}, ch={s.channel}")
    ORM2.unlink(shm_name)


if __name__ == '__main__':
    SHM_NAME = "example_orm2_sensors"

    # Parent: build database
    orm = ORM2.create()
    for i in range(4):
        s = Sensor()
        s.name = f"sensor_{i}"
        s.value = 20.0 + i * 1.5
        s.channel = i
        orm.push(s)
    orm.combine()

    # Publish to shared memory
    orm.share(SHM_NAME)
    print(f"[parent] shared {orm.count(Sensor)} sensors as '{SHM_NAME}'")

    # Child process reads from shared memory
    p = Process(target=reader_process, args=(SHM_NAME,))
    p.start()
    p.join()

    print("\n✓ Shared memory example complete.")
