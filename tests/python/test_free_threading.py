"""Thread-safety tests for fastdb4py under free-threaded Python (PEP 703).

These tests verify that module-level caches, ColumnAccessor instances, and
serialization paths are safe under concurrent access.  They are useful on
both standard (GIL) and free-threaded Python builds — on standard builds
they confirm the lock logic does not deadlock, on free-threaded builds they
catch data races.
"""

import threading
import numpy as np
import pytest

from fastdb4py import Feature, F64, U32, STR, ORM, TableDefn
from fastdb4py.feature._schema import get_class_schema, _SCHEMA_ATTR
from fastdb4py.serializer import FastSerializer


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

NUM_THREADS = 20
BARRIER = threading.Barrier(NUM_THREADS)


def _run_concurrent(fn, n=NUM_THREADS):
    """Run *fn(i)* in *n* threads that all start at the same instant."""
    barrier = threading.Barrier(n)
    errors = [None] * n
    results = [None] * n

    def worker(i):
        try:
            barrier.wait()
            results[i] = fn(i)
        except Exception as e:
            errors[i] = e

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    for i, err in enumerate(errors):
        if err is not None:
            raise AssertionError(f"Thread {i} raised: {err}") from err

    return results


# ---------------------------------------------------------------------------
# 4.1  ClassSchema cache concurrent access
# ---------------------------------------------------------------------------

class TestSchemaCacheConcurrency:
    """Verify get_class_schema() is safe when many threads race to build the
    schema for the same (or different) Feature class(es) for the first time."""

    def test_same_class_concurrent(self):
        """All threads should get the identical ClassSchema object."""
        # Define a fresh class so the schema has not been cached yet.
        class ConcurrentPoint(Feature):
            x: F64
            y: F64

        # Purge any cached schema so every thread races to build it.
        ConcurrentPoint.__dict__.get(_SCHEMA_ATTR)  # warm-up type dict
        if _SCHEMA_ATTR in ConcurrentPoint.__dict__:
            delattr(ConcurrentPoint, _SCHEMA_ATTR)

        results = _run_concurrent(lambda i: get_class_schema(ConcurrentPoint))

        # All schemas must be the same object (cached identity).
        first = results[0]
        assert first is not None
        assert all(r is first for r in results), "Schema objects differ across threads"

    def test_different_classes_concurrent(self):
        """Building schemas for different classes concurrently should not corrupt
        the shared cache or deadlock."""
        classes = []
        for k in range(NUM_THREADS):
            # type() dynamically creates a fresh Feature subclass.
            cls = type(f"DynPoint{k}", (Feature,), {"__annotations__": {"x": F64, "y": F64}})
            classes.append(cls)

        results = _run_concurrent(lambda i: get_class_schema(classes[i]))

        for i, r in enumerate(results):
            assert r is not None
            assert "x" in r.origin_hints
            assert "y" in r.origin_hints


# ---------------------------------------------------------------------------
# 4.2  Serializer schema cache concurrent access
# ---------------------------------------------------------------------------

class TestSerializerConcurrency:
    """Verify FastSerializer.dumps/loads under concurrent access."""

    def test_concurrent_dumps(self):
        """Multiple threads serialising different objects of the same type
        should not crash or produce corrupt output."""
        class SerPoint(Feature):
            x: F64
            y: F64

        objects = [SerPoint(x=float(i), y=float(i * 2)) for i in range(NUM_THREADS)]

        def do_dump(i):
            buf = FastSerializer.dumps(objects[i])
            loaded = FastSerializer.loads(buf, SerPoint)
            return loaded

        results = _run_concurrent(do_dump)

        for i, loaded in enumerate(results):
            assert loaded.x == float(i)
            assert loaded.y == float(i * 2)

    def test_concurrent_loads_different_types(self):
        """Concurrent loads of different Feature types sharing the schema cache."""
        class TypeA(Feature):
            val: F64

        class TypeB(Feature):
            val: U32

        buf_a = FastSerializer.dumps(TypeA(val=3.14))
        buf_b = FastSerializer.dumps(TypeB(val=42))

        def do_load(i):
            if i % 2 == 0:
                return FastSerializer.loads(buf_a, TypeA)
            else:
                return FastSerializer.loads(buf_b, TypeB)

        results = _run_concurrent(do_load)

        for i, loaded in enumerate(results):
            if i % 2 == 0:
                assert abs(loaded.val - 3.14) < 1e-9
            else:
                assert loaded.val == 42


# ---------------------------------------------------------------------------
# 4.3  ColumnAccessor concurrent read
# ---------------------------------------------------------------------------

class TestColumnAccessorConcurrency:
    """Verify that concurrent access to table.column.{field} is safe."""

    def test_concurrent_column_read(self):
        """Multiple threads reading the same column should all get identical
        numpy arrays without corruption."""
        N = 100

        class ColPoint(Feature):
            x: F64
            y: F64

        orm = ORM.truncate([TableDefn(ColPoint, N)])
        tbl = orm[ColPoint][ColPoint]

        # Fill with known data
        xs = np.arange(N, dtype=np.float64)
        ys = np.arange(N, dtype=np.float64) * 2
        tbl.fill(x=xs, y=ys)

        def read_columns(i):
            col_x = tbl.column.x
            col_y = tbl.column.y
            return (col_x.copy(), col_y.copy())

        results = _run_concurrent(read_columns)

        for i, (rx, ry) in enumerate(results):
            np.testing.assert_array_equal(rx, xs, err_msg=f"Thread {i} got wrong x")
            np.testing.assert_array_equal(ry, ys, err_msg=f"Thread {i} got wrong y")

    def test_concurrent_column_first_access(self):
        """Race on the very first access to a column (cold cache path)."""
        N = 50

        class FreshColPoint(Feature):
            a: F64
            b: F64

        orm = ORM.truncate([TableDefn(FreshColPoint, N)])
        tbl = orm[FreshColPoint][FreshColPoint]

        data = np.ones(N, dtype=np.float64) * 7.0
        tbl.fill(a=data, b=data)

        # All threads race to access column 'a' for the first time
        results = _run_concurrent(lambda i: tbl.column.a.copy())

        for i, r in enumerate(results):
            np.testing.assert_array_equal(r, data, err_msg=f"Thread {i} got wrong data")


# ---------------------------------------------------------------------------
# 4.4  Feature instance concurrent access (no-crash guarantee)
# ---------------------------------------------------------------------------

class TestFeatureConcurrency:
    """Feature instances are NOT guaranteed thread-safe, but concurrent access
    must not cause interpreter crashes (segfaults, corrupted refcounts, etc.).
    
    These tests verify the "no crash" contract, not data consistency."""

    def test_concurrent_read_write_no_crash(self):
        """Readers and writers on the same Feature instance must not crash."""
        class SharedPoint(Feature):
            x: F64
            y: F64

        obj = SharedPoint(x=1.0, y=2.0)
        stop = threading.Event()

        errors = []

        def reader():
            try:
                while not stop.is_set():
                    _ = obj.x
                    _ = obj.y
            except Exception as e:
                errors.append(e)

        def writer():
            try:
                for v in range(200):
                    obj.x = float(v)
                    obj.y = float(v * 2)
            except Exception as e:
                errors.append(e)

        readers = [threading.Thread(target=reader) for _ in range(5)]
        writers = [threading.Thread(target=writer) for _ in range(3)]

        for t in readers + writers:
            t.start()

        # Let writers finish naturally, then stop readers
        for t in writers:
            t.join()
        stop.set()
        for t in readers:
            t.join()

        assert not errors, f"Threads raised errors: {errors}"

    def test_concurrent_construction(self):
        """Many threads constructing Feature instances of the same class."""
        class ConPoint(Feature):
            x: F64
            y: F64

        results = _run_concurrent(lambda i: ConPoint(x=float(i), y=float(i)))

        for i, r in enumerate(results):
            assert r.x == float(i)
            assert r.y == float(i)


# ---------------------------------------------------------------------------
# 4.5  ORM lifecycle concurrent access
# ---------------------------------------------------------------------------

class TestORMConcurrency:
    """Verify that concurrent reads from the same ORM instance are safe."""

    def test_concurrent_table_get(self):
        """Multiple threads getting the same table from an ORM."""
        N = 50

        class OrmPoint(Feature):
            x: F64
            y: F64

        orm = ORM.truncate([TableDefn(OrmPoint, N)])

        def get_table(i):
            tbl = orm[OrmPoint][OrmPoint]
            return len(tbl)

        results = _run_concurrent(get_table)

        assert all(r == N for r in results)

    def test_concurrent_read_different_tables(self):
        """Multiple threads reading from different tables in the same ORM."""
        N = 30

        class TableA(Feature):
            a: F64

        class TableB(Feature):
            b: U32

        orm = ORM.truncate([TableDefn(TableA, N), TableDefn(TableB, N)])

        def read_table(i):
            if i % 2 == 0:
                tbl = orm[TableA][TableA]
                return ("A", len(tbl))
            else:
                tbl = orm[TableB][TableB]
                return ("B", len(tbl))

        results = _run_concurrent(read_table)

        for i, (tag, count) in enumerate(results):
            assert count == N, f"Thread {i} got wrong count for table {tag}"

    def test_concurrent_feature_iteration(self):
        """Multiple threads iterating through the same table concurrently."""
        N = 20

        class IterPoint(Feature):
            x: F64

        orm = ORM.truncate([TableDefn(IterPoint, N)])
        tbl = orm[IterPoint][IterPoint]
        tbl.fill(x=np.arange(N, dtype=np.float64))

        def iterate(i):
            values = [f.x for f in tbl]
            return values

        results = _run_concurrent(iterate, n=10)

        expected = list(range(N))
        for i, vals in enumerate(results):
            assert [int(v) for v in vals] == expected, f"Thread {i} got wrong iteration"


# ---------------------------------------------------------------------------
# 4.6  Integration stress test
# ---------------------------------------------------------------------------

class TestFreeThreadingStress:
    """Full workflow stress test combining multiple operations concurrently."""

    def test_mixed_workload(self):
        """Simulate realistic concurrent usage:
        - Threads creating independent ORMs with truncate
        - Threads serialising/deserialising Feature objects
        - Threads reading columns from a shared ORM
        """
        N = 50

        class StressPoint(Feature):
            x: F64
            y: F64

        # Shared ORM for read operations
        shared_orm = ORM.truncate([TableDefn(StressPoint, N)])
        shared_tbl = shared_orm[StressPoint][StressPoint]
        shared_tbl.fill(
            x=np.arange(N, dtype=np.float64),
            y=np.arange(N, dtype=np.float64) * 3
        )

        errors = []

        def workload_truncate():
            """Create independent ORM, write, read back."""
            try:
                orm = ORM.truncate([TableDefn(StressPoint, 10)])
                tbl = orm[StressPoint][StressPoint]
                tbl.fill(x=np.ones(10, dtype=np.float64))
                assert np.all(tbl.column.x == 1.0)
            except Exception as e:
                errors.append(("truncate", e))

        def workload_serialize():
            """Serialise and deserialise a Feature."""
            try:
                obj = StressPoint(x=42.0, y=84.0)
                buf = FastSerializer.dumps(obj)
                loaded = FastSerializer.loads(buf, StressPoint)
                assert loaded.x == 42.0
                assert loaded.y == 84.0
            except Exception as e:
                errors.append(("serialize", e))

        def workload_read_shared():
            """Read from shared ORM."""
            try:
                col_x = shared_tbl.column.x
                col_y = shared_tbl.column.y
                assert len(col_x) == N
                assert len(col_y) == N
            except Exception as e:
                errors.append(("read", e))

        threads = []
        for i in range(6):
            threads.append(threading.Thread(target=workload_truncate))
        for i in range(6):
            threads.append(threading.Thread(target=workload_serialize))
        for i in range(8):
            threads.append(threading.Thread(target=workload_read_shared))

        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert not errors, f"Stress test errors: {errors}"
