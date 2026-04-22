"""
Kostya-Style Serialization Benchmark — ColumnEngine vs ObjectEngine vs PyArrow vs pickle
==========================================================================================
Inspired by Kostya's benchmarks (github.com/kostya/benchmarks).

Data structure: 'Coordinate' records — row_id (u32), x/y/z (float64), name (UTF-8 string).

  Compares fastdb ColumnEngine in multiple modes plus ObjectEngine, PyArrow, and pickle:
   - fastdb ColumnEngine push path            (OLAP/batch columnar, push + combine)
   - fastdb ColumnEngine truncate + STR path  (raw strings: known-size truncate + unified tbl.fill(..., name=names))
   - fastdb ColumnEngine truncate + STR path  (prepacked: pack_utf8_column([...]) + tbl.column.name.fill_utf8(...))
   - fastdb ColumnEngine truncate fast path   (known-size numeric-only apples-to-apples)
   - fastdb ObjectEngine                      (OLTP/graph, deferred batch push + combine)
   - PyArrow                                  (columnar IPC)
   - pickle                                   (Python native binary)

Phases measured (milliseconds, median of `reps` runs):
  build      : construct/fill N in-memory records or columns
  encode     : convert to binary wire format (combine/dumps when needed)
  shm        : allocate POSIX shared memory + memcpy bytes in
  deserialize: load from shared-memory / IPC buffer
  read       : iterate all N records, sum x+y+z

Size column: wire-format bytes (uncompressed).

Usage:
    uv run python tests/python/benchmark_kostya_orm2.py
    uv run python tests/python/benchmark_kostya_orm2.py --quick
    uv run python tests/python/benchmark_kostya_orm2.py --n 100000
    uv run python tests/python/benchmark_kostya_orm2.py --output-json kostya_orm2.json
"""
from __future__ import annotations

import argparse
import gc
import json
import os
import pickle
import statistics
import time
import uuid
from multiprocessing import shared_memory

import numpy as np

from fastdb4py import feature, ColumnEngine, ObjectEngine, Layout, F64, U32, STR, pack_utf8_column

try:
    import pyarrow as pa
    import pyarrow.ipc as pa_ipc
    HAS_ARROW = True
except ImportError:
    HAS_ARROW = False
    print("[WARNING] pyarrow not found — arrow benchmark will be skipped")


# ---------------------------------------------------------------------------
# Data models
# ---------------------------------------------------------------------------

@feature
class Coord:
    """Kostya-style coordinate record (ColumnEngine)."""
    row_id: U32
    x: F64
    y: F64
    z: F64
    name: STR


@feature
class Coord2:
    """Kostya-style coordinate record (ObjectEngine)."""
    row_id: U32
    x: F64
    y: F64
    z: F64
    name: STR


@feature
class CoordNumeric:
    """Numeric-only coordinate record for the truncate fast-path benchmark."""
    row_id: U32
    x: F64
    y: F64
    z: F64


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_name(i: int) -> str:
    return f"coord_{i % 50000:05d}"


def _make_coord_columns(N: int) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, list[str]]:
    ids = np.arange(N, dtype=np.uint32)
    xs = np.arange(N, dtype=np.float64) * 0.1
    ys = np.arange(N, dtype=np.float64) * 0.2
    zs = np.arange(N, dtype=np.float64) * 0.3
    names = [_make_name(i) for i in range(N)]
    return ids, xs, ys, zs, names


def _shm_write(data: bytes) -> shared_memory.SharedMemory:
    shm = shared_memory.SharedMemory(create=True, size=max(len(data), 1))
    shm.buf[:len(data)] = data
    return shm


def _shm_read(shm: shared_memory.SharedMemory, length: int) -> bytes:
    return bytes(shm.buf[:length])


def _median_ms(fn, reps: int) -> float:
    return _stats_ms(fn, reps)["median"]


def _stats_ms(fn, reps: int) -> dict:
    times = []
    for _ in range(reps):
        gc.collect()
        t0 = time.perf_counter()
        fn()
        times.append((time.perf_counter() - t0) * 1000)
    return {
        "median": statistics.median(times),
        "min": min(times),
        "max": max(times),
        "stddev": statistics.pstdev(times) if len(times) > 1 else 0.0,
        "samples": times,
    }


def _throughput(N: int, ms: float) -> float:
    """Million records/second."""
    if ms <= 0:
        return float("inf")
    return (N / 1_000_000.0) / (ms / 1000.0)


def _bytes_per_record(size_bytes: int, N: int) -> float:
    if N <= 0:
        return float("nan")
    return size_bytes / N


# ---------------------------------------------------------------------------
# ObjectEngine benchmark
# ---------------------------------------------------------------------------

def bench_object_engine(N: int, reps: int) -> dict:
    shm_name = f"oe_kostya_{uuid.uuid4().hex[:8]}"

    # --- build: push N Coord2 features ---
    def do_build():
        orm = ObjectEngine.create()
        for i in range(N):
            f = Coord2()
            f.row_id = i
            f.x = float(i) * 0.1
            f.y = float(i) * 0.2
            f.z = float(i) * 0.3
            f.name = _make_name(i)
            orm.push(f)
        return orm

    build_ms = _median_ms(do_build, reps)

    # --- encode: batch combine() ---
    _unflushed = [do_build() for _ in range(reps)]

    def do_encode():
        _unflushed.pop().combine()

    encode_ms = _median_ms(do_encode, reps)

    # --- shm: write built binary to POSIX shared memory ---
    orm = do_build()
    orm.combine()
    _raw = orm._buffer

    def do_shm():
        s = _shm_write(_raw)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)

    orm.share(shm_name)

    try:
        _probe = shared_memory.SharedMemory(name=shm_name)
        size_bytes = _probe.size
        _probe.close()
    except Exception:
        size_bytes = len(_raw)

    orm2 = None
    try:
        # --- deserialize: load from shm ---
        def do_deserial():
            h = ObjectEngine.load(shm_name)

        deserial_ms = _median_ms(do_deserial, reps)
        orm2 = ObjectEngine.load(shm_name)

        # --- read: sum x+y+z via columnar numpy (zero-copy) ---
        def do_read():
            tbl = orm2.table(Coord2)
            cx = tbl.column.x
            cy = tbl.column.y
            cz = tbl.column.z
            return float(cx[:].sum() + cy[:].sum() + cz[:].sum())

        read_ms = _median_ms(do_read, reps)
    finally:
        ObjectEngine.unlink(shm_name)

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "object",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


# ---------------------------------------------------------------------------
# PyArrow benchmark
# ---------------------------------------------------------------------------

def bench_arrow(N: int, reps: int) -> dict:
    if not HAS_ARROW:
        return {"system": "arrow", "error": "pyarrow not installed"}

    # --- build: construct PyArrow Table ---
    def do_build():
        ids = np.arange(N, dtype=np.uint32)
        xs = np.arange(N, dtype=np.float64) * 0.1
        ys = np.arange(N, dtype=np.float64) * 0.2
        zs = np.arange(N, dtype=np.float64) * 0.3
        names = [_make_name(i) for i in range(N)]
        return pa.table({
            "row_id": pa.array(ids, type=pa.uint32()),
            "x": pa.array(xs, type=pa.float64()),
            "y": pa.array(ys, type=pa.float64()),
            "z": pa.array(zs, type=pa.float64()),
            "name": pa.array(names, type=pa.string()),
        })

    build_ms = _median_ms(do_build, reps)
    tbl = do_build()

    # --- encode: IPC stream → bytes ---
    def _to_ipc(t) -> bytes:
        sink = pa.BufferOutputStream()
        writer = pa_ipc.new_stream(sink, t.schema)
        writer.write_table(t)
        writer.close()
        return sink.getvalue().to_pybytes()

    encode_ms = _median_ms(lambda: _to_ipc(tbl), reps)
    ipc_bytes = _to_ipc(tbl)

    # --- shm: write encoded bytes to POSIX shared memory ---
    def do_shm():
        s = _shm_write(ipc_bytes)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)
    shm = _shm_write(ipc_bytes)
    size_bytes = len(ipc_bytes)

    # --- deserialize: shm → IPC → Table ---
    def do_deserial():
        raw = _shm_read(shm, size_bytes)
        buf = pa.py_buffer(raw)
        reader = pa_ipc.open_stream(buf)
        _ = reader.read_all()

    deserial_ms = _median_ms(do_deserial, reps)
    raw = _shm_read(shm, size_bytes)
    tbl2 = pa_ipc.open_stream(pa.py_buffer(raw)).read_all()

    # --- read: sum x+y+z using NumPy ---
    def do_read():
        cx = tbl2.column("x").to_numpy()
        cy = tbl2.column("y").to_numpy()
        cz = tbl2.column("z").to_numpy()
        return float(cx.sum() + cy.sum() + cz.sum())

    read_ms = _median_ms(do_read, reps)

    shm.close()
    shm.unlink()

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "arrow",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


# ---------------------------------------------------------------------------
# Pickle benchmark
# ---------------------------------------------------------------------------

def bench_pickle(N: int, reps: int) -> dict:
    # --- build: list of dicts ---
    def do_build():
        return [
            {
                "row_id": i,
                "x": float(i) * 0.1,
                "y": float(i) * 0.2,
                "z": float(i) * 0.3,
                "name": _make_name(i),
            }
            for i in range(N)
        ]

    build_ms = _median_ms(do_build, reps)
    data = do_build()

    # --- encode: pickle.dumps → bytes ---
    encode_ms = _median_ms(lambda: pickle.dumps(data, protocol=5), reps)
    pkl_bytes = pickle.dumps(data, protocol=5)

    # --- shm: write encoded bytes to POSIX shared memory ---
    def do_shm():
        s = _shm_write(pkl_bytes)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)
    shm = _shm_write(pkl_bytes)
    size_bytes = len(pkl_bytes)

    # --- deserialize: shm → unpickle ---
    def do_deserial():
        raw = _shm_read(shm, size_bytes)
        _ = pickle.loads(raw)

    deserial_ms = _median_ms(do_deserial, reps)
    raw = _shm_read(shm, size_bytes)
    data2 = pickle.loads(raw)

    # --- read: iterate dicts, sum x+y+z ---
    def do_read():
        total = 0.0
        for row in data2:
            total += row["x"] + row["y"] + row["z"]
        return total

    read_ms = _median_ms(do_read, reps)

    shm.close()
    shm.unlink()

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "pickle",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }

# ---------------------------------------------------------------------------
# ColumnEngine benchmarks
# ---------------------------------------------------------------------------

def bench_column_push(N: int, reps: int) -> dict:
    """ColumnEngine via dynamic create() + per-row push() (handles STR)."""
    shm_name = f"ce_kostya_{uuid.uuid4().hex[:8]}"

    # --- build: push N Coord features ---
    def do_build():
        orm = ColumnEngine.create()
        for i in range(N):
            f = Coord()
            f.row_id = i
            f.x = float(i) * 0.1
            f.y = float(i) * 0.2
            f.z = float(i) * 0.3
            f.name = _make_name(i)
            orm.push(f)
        return orm

    build_ms = _median_ms(do_build, reps)

    # --- encode: C++ columnar flush (combine) ---
    _unflushed = [do_build() for _ in range(reps)]

    def do_encode():
        _unflushed.pop().combine()

    encode_ms = _median_ms(do_encode, reps)

    # --- shm: write flushed binary to POSIX shared memory ---
    orm = do_build()
    orm.combine()
    _raw = bytes(orm._origin.buffer().as_array(np.uint8))

    def do_shm():
        s = _shm_write(_raw)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)

    orm.share(shm_name)

    try:
        _probe = shared_memory.SharedMemory(name=shm_name)
        size_bytes = _probe.size
        _probe.close()
    except Exception:
        size_bytes = 0

    orm2 = None
    try:
        # --- deserialize: zero-copy load from shm ---
        def do_deserial():
            h = ColumnEngine.load(shm_name)
            h.close()

        deserial_ms = _median_ms(do_deserial, reps)
        orm2 = ColumnEngine.load(shm_name)

        # --- read: sum x+y+z via columnar numpy ---
        def do_read():
            tbl = orm2.table(Coord)
            cx = tbl.column.x
            cy = tbl.column.y
            cz = tbl.column.z
            return float(cx[:].sum() + cy[:].sum() + cz[:].sum())

        read_ms = _median_ms(do_read, reps)
    finally:
        if orm2 is not None:
            orm2.unlink()
        else:
            try:
                h = ColumnEngine.load(shm_name)
                h.unlink()
            except Exception:
                pass

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "column_push",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


def bench_column_trunc_str(N: int, reps: int) -> dict:
    """ColumnEngine via truncate(Layout) + unified tbl.fill(..., name=names)."""
    shm_name = f"cets_kostya_{uuid.uuid4().hex[:8]}"

    def do_build():
        ids, xs, ys, zs, names = _make_coord_columns(N)
        orm = ColumnEngine.truncate([Layout(Coord, N)])
        tbl = orm.table(Coord)
        tbl.fill(row_id=ids, x=xs, y=ys, z=zs, name=names)
        return orm

    build_ms = _median_ms(do_build, reps)

    # truncate() returns a fixed buffer immediately; unified fixed-table fill writes it in-place.
    encode_ms = 0.0

    orm = do_build()
    _raw = bytes(orm._origin.buffer().as_array(np.uint8))

    def do_shm():
        s = _shm_write(_raw)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)

    orm.share(shm_name)

    try:
        _probe = shared_memory.SharedMemory(name=shm_name)
        size_bytes = _probe.size
        _probe.close()
    except Exception:
        size_bytes = 0

    orm2 = None
    try:
        def do_deserial():
            h = ColumnEngine.load(shm_name)
            h.close()

        deserial_ms = _median_ms(do_deserial, reps)
        orm2 = ColumnEngine.load(shm_name)

        def do_read():
            tbl = orm2.table(Coord)
            cx = tbl.column.x
            cy = tbl.column.y
            cz = tbl.column.z
            return float(cx[:].sum() + cy[:].sum() + cz[:].sum())

        read_ms = _median_ms(do_read, reps)
    finally:
        if orm2 is not None:
            orm2.unlink()
        else:
            try:
                h = ColumnEngine.load(shm_name)
                h.unlink()
            except Exception:
                pass

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "column_trunc_str_raw",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


def bench_column_trunc_str_prepacked(N: int, reps: int) -> dict:
    """ColumnEngine via truncate(Layout) + pack_utf8_column(...) + fill_utf8(...)."""
    shm_name = f"cetsp_kostya_{uuid.uuid4().hex[:8]}"

    def do_build():
        ids, xs, ys, zs, names = _make_coord_columns(N)
        offsets, data = pack_utf8_column(names)
        orm = ColumnEngine.truncate([Layout(Coord, N)])
        tbl = orm.table(Coord)
        tbl.fill(row_id=ids, x=xs, y=ys, z=zs)
        return orm, offsets, data

    build_ms = _median_ms(do_build, reps)
    orm, offsets, data = do_build()
    tbl = orm.table(Coord)

    def do_encode():
        tbl.column.name.fill_utf8(offsets, data)

    encode_ms = _median_ms(do_encode, reps)
    do_encode()

    _raw = bytes(orm._origin.buffer().as_array(np.uint8))

    def do_shm():
        s = _shm_write(_raw)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)

    orm.share(shm_name)

    try:
        _probe = shared_memory.SharedMemory(name=shm_name)
        size_bytes = _probe.size
        _probe.close()
    except Exception:
        size_bytes = 0

    orm2 = None
    try:
        def do_deserial():
            h = ColumnEngine.load(shm_name)
            h.close()

        deserial_ms = _median_ms(do_deserial, reps)
        orm2 = ColumnEngine.load(shm_name)

        def do_read():
            tbl = orm2.table(Coord)
            cx = tbl.column.x
            cy = tbl.column.y
            cz = tbl.column.z
            return float(cx[:].sum() + cy[:].sum() + cz[:].sum())

        read_ms = _median_ms(do_read, reps)
    finally:
        if orm2 is not None:
            orm2.unlink()
        else:
            try:
                h = ColumnEngine.load(shm_name)
                h.unlink()
            except Exception:
                pass

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "column_trunc_str_prepacked",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


def bench_column_truncate(N: int, reps: int) -> dict:
    """ColumnEngine via truncate(Layout) + bulk numpy fill() (numeric-only fast path)."""
    shm_name = f"cet_kostya_{uuid.uuid4().hex[:8]}"

    # --- build: pre-allocate + bulk fill numeric columns from numpy ---
    def do_build():
        ids = np.arange(N, dtype=np.uint32)
        xs = np.arange(N, dtype=np.float64) * 0.1
        ys = np.arange(N, dtype=np.float64) * 0.2
        zs = np.arange(N, dtype=np.float64) * 0.3
        orm = ColumnEngine.truncate([Layout(CoordNumeric, N)])
        tbl = orm.table(CoordNumeric)
        tbl.fill(row_id=ids, x=xs, y=ys, z=zs)
        return orm

    build_ms = _median_ms(do_build, reps)

    # --- encode: no extra combine() step; truncate already materializes fixed buffer ---
    encode_ms = 0.0

    # --- shm: write flushed binary to POSIX shared memory ---
    orm = do_build()
    _raw = bytes(orm._origin.buffer().as_array(np.uint8))

    def do_shm():
        s = _shm_write(_raw)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)

    orm.share(shm_name)

    try:
        _probe = shared_memory.SharedMemory(name=shm_name)
        size_bytes = _probe.size
        _probe.close()
    except Exception:
        size_bytes = 0

    orm2 = None
    try:
        def do_deserial():
            h = ColumnEngine.load(shm_name)
            h.close()

        deserial_ms = _median_ms(do_deserial, reps)
        orm2 = ColumnEngine.load(shm_name)

        def do_read():
            tbl = orm2.table(CoordNumeric)
            cx = tbl.column.x
            cy = tbl.column.y
            cz = tbl.column.z
            return float(cx[:].sum() + cy[:].sum() + cz[:].sum())

        read_ms = _median_ms(do_read, reps)
    finally:
        if orm2 is not None:
            orm2.unlink()
        else:
            try:
                h = ColumnEngine.load(shm_name)
                h.unlink()
            except Exception:
                pass

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "column_truncate",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


def bench_arrow_numeric(N: int, reps: int) -> dict:
    """PyArrow Table without `name` — apples-to-apples vs column_truncate."""
    if not HAS_ARROW:
        return {"system": "arrow_num", "error": "pyarrow not installed"}

    def do_build():
        ids = np.arange(N, dtype=np.uint32)
        xs = np.arange(N, dtype=np.float64) * 0.1
        ys = np.arange(N, dtype=np.float64) * 0.2
        zs = np.arange(N, dtype=np.float64) * 0.3
        return pa.table({
            "row_id": pa.array(ids, type=pa.uint32()),
            "x": pa.array(xs, type=pa.float64()),
            "y": pa.array(ys, type=pa.float64()),
            "z": pa.array(zs, type=pa.float64()),
        })

    build_ms = _median_ms(do_build, reps)
    tbl = do_build()

    def _to_ipc(t) -> bytes:
        sink = pa.BufferOutputStream()
        writer = pa_ipc.new_stream(sink, t.schema)
        writer.write_table(t)
        writer.close()
        return sink.getvalue().to_pybytes()

    encode_ms = _median_ms(lambda: _to_ipc(tbl), reps)
    ipc_bytes = _to_ipc(tbl)

    def do_shm():
        s = _shm_write(ipc_bytes)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)
    shm = _shm_write(ipc_bytes)
    size_bytes = len(ipc_bytes)

    def do_deserial():
        raw = _shm_read(shm, size_bytes)
        buf = pa.py_buffer(raw)
        reader = pa_ipc.open_stream(buf)
        _ = reader.read_all()

    deserial_ms = _median_ms(do_deserial, reps)
    raw = _shm_read(shm, size_bytes)
    tbl2 = pa_ipc.open_stream(pa.py_buffer(raw)).read_all()

    def do_read():
        cx = tbl2.column("x").to_numpy()
        cy = tbl2.column("y").to_numpy()
        cz = tbl2.column("z").to_numpy()
        return float(cx.sum() + cy.sum() + cz.sum())

    read_ms = _median_ms(do_read, reps)
    shm.close()
    shm.unlink()

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "arrow_num",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


def bench_pickle_numeric(N: int, reps: int) -> dict:
    """Pickle list[dict] without `name` — apples-to-apples vs column_truncate."""
    def do_build():
        return [
            {"row_id": i, "x": float(i) * 0.1, "y": float(i) * 0.2, "z": float(i) * 0.3}
            for i in range(N)
        ]

    build_ms = _median_ms(do_build, reps)
    data = do_build()

    encode_ms = _median_ms(lambda: pickle.dumps(data, protocol=5), reps)
    pkl_bytes = pickle.dumps(data, protocol=5)

    def do_shm():
        s = _shm_write(pkl_bytes)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)
    shm = _shm_write(pkl_bytes)
    size_bytes = len(pkl_bytes)

    def do_deserial():
        raw = _shm_read(shm, size_bytes)
        _ = pickle.loads(raw)

    deserial_ms = _median_ms(do_deserial, reps)
    raw = _shm_read(shm, size_bytes)
    data2 = pickle.loads(raw)

    def do_read():
        total = 0.0
        for row in data2:
            total += row["x"] + row["y"] + row["z"]
        return total

    read_ms = _median_ms(do_read, reps)
    shm.close()
    shm.unlink()

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "pickle_num",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

COLS = ["system", "build_ms", "encode_ms", "shm_ms", "deserial_ms", "read_ms", "total_ms", "size_kb"]
WIDTHS = [16, 10, 11, 9, 14, 10, 11, 10]


def _fmt(v, width: int) -> str:
    if isinstance(v, float):
        return f"{v:>{width}.1f}"
    return f"{str(v):>{width}}"


def _kb(size_bytes) -> float:
    return round(size_bytes / 1024, 1) if isinstance(size_bytes, int) else float("nan")


def _print_throughput(results: list[dict], N: int):
    """Print throughput (M records/sec) and bytes-per-record per system."""
    THRU_COLS = ["system", "build", "encode", "shm", "deserial", "read", "total", "B/rec"]
    THRU_WIDTHS = [16, 10, 10, 10, 10, 10, 10, 10]
    sep = "-" * sum(THRU_WIDTHS)
    print(f"\n  Throughput (M records/sec)  + bytes/record")
    print(f"  {sep}")
    print(f"  {''.join(f'{c:>{w}}' for c, w in zip(THRU_COLS, THRU_WIDTHS))}")
    print(f"  {sep}")
    for r in results:
        if "error" in r:
            continue
        row = {
            "system": r["system"],
            "build":    round(_throughput(N, r["build_ms"]), 2),
            "encode":   round(_throughput(N, r["encode_ms"]), 2),
            "shm":      round(_throughput(N, r["shm_ms"]), 2),
            "deserial": round(_throughput(N, r["deserial_ms"]), 2),
            "read":     round(_throughput(N, r["read_ms"]), 2),
            "total":    round(_throughput(N, r["total_ms"]), 2),
            "B/rec":    round(_bytes_per_record(r["size_bytes"], N), 1),
        }
        print(f"  {''.join(_fmt(row[c], w) for c, w in zip(THRU_COLS, THRU_WIDTHS))}")
    print(f"  {sep}")


def _print_ratio(results: list[dict], baseline_system: str, label: str):
    base = next((r for r in results if r.get("system") == baseline_system and "error" not in r), None)
    if base is None:
        return
    print(f"\n  Ratio vs {label}  (lower = faster/smaller; 1.00 = baseline):")
    sep = "-" * sum(WIDTHS)
    print(f"  {sep}")
    print(f"  {''.join(f'{c:>{w}}' for c, w in zip(COLS, WIDTHS))}")
    print(f"  {sep}")
    for r in results:
        if "error" in r:
            continue
        def ratio(key):
            b = base.get(key, 0)
            v = r.get(key, 0)
            if b == 0:
                return 1.0 if v == 0 else float("inf")
            return round(v / b, 2)
        row = {
            "system":      r["system"],
            "build_ms":    ratio("build_ms"),
            "encode_ms":   ratio("encode_ms"),
            "shm_ms":      ratio("shm_ms"),
            "deserial_ms": ratio("deserial_ms"),
            "read_ms":     ratio("read_ms"),
            "total_ms":    ratio("total_ms"),
            "size_kb":     ratio("size_bytes"),
        }
        print(f"  {''.join(_fmt(row[c], w) for c, w in zip(COLS, WIDTHS))}")
    print(f"  {sep}")


def print_table(results: list[dict], N: int, *, title: str, schema_desc: str):
    sep = "-" * sum(WIDTHS)
    print(f"\n  ━━━ {title}  (N = {N:,}) ━━━")
    print(f"  Schema: {schema_desc}")
    print(f"  {sep}")
    print(f"  {''.join(f'{c:>{w}}' for c, w in zip(COLS, WIDTHS))}")
    print(f"  {sep}")
    for r in results:
        if "error" in r:
            print(f"  {r['system']:>16}  {r['error']}")
            continue
        d = dict(r)
        d["size_kb"] = _kb(r["size_bytes"])
        print(f"  {''.join(_fmt(d[c], w) for c, w in zip(COLS, WIDTHS))}")
    print(f"  {sep}")

    _print_throughput(results, N)

    # Dual baselines: pickle (general) and arrow (columnar best-of-breed)
    for baseline in ("pickle", "pickle_num", "arrow", "arrow_num"):
        if any(r.get("system") == baseline for r in results):
            _print_ratio(results, baseline, baseline)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Kostya-style benchmark: fastdb engines vs PyArrow vs pickle"
    )
    parser.add_argument("--quick", action="store_true",
                        help="Skip N=1_000_000, use 1 rep")
    parser.add_argument("--n", type=int, default=None,
                        help="Run only this single N value")
    parser.add_argument("--reps", type=int, default=3,
                        help="Repetitions per measurement (default 3)")
    parser.add_argument("--output-json", metavar="FILE",
                        help="Append results to JSON file")
    args = parser.parse_args()

    if args.n:
        Ns = [args.n]
    elif args.quick:
        Ns = [10_000, 100_000]
    else:
        Ns = [10_000, 100_000, 1_000_000]

    reps = 1 if args.quick else args.reps

    print("=" * 95)
    print("  Kostya-Style Benchmark — fastdb engines vs PyArrow vs pickle")
    print("  Two sections: (A) full schema with real UTF-8 string-column writes; (B) numeric-only fast path")
    print("  Phases (ms, median): build | encode (combine/dumps) | shm | deserialize | read sum(x+y+z)")
    print("  Throughput: million records/sec; B/rec: wire bytes per record")
    print("  Notes:")
    print("    column_push            = ColumnEngine.create() + per-row push() + combine()")
    print("    column_trunc_str_raw    = ColumnEngine.truncate(Layout) + tbl.fill(..., name=names)")
    print("    column_trunc_str_prepacked = pack_utf8_column(names) + tbl.column.name.fill_utf8(...)")
    print("                               (build = string prep + packing; encode = native UTF-8 ingest)")
    print("    column_truncate   = ColumnEngine.truncate(Layout) + tbl.fill(numpy)  [numeric-only fast path]")
    print("    object            = ObjectEngine.create() + per-row push() + combine()")
    print("    arrow / arrow_num = PyArrow Table + IPC stream + numpy read")
    print("    pickle / pickle_num = list[dict] + pickle.dumps/loads + dict iteration")
    print("    raw truncate path reports encode_ms = 0; prepacked path reports native fill_utf8 in encode_ms")
    print("=" * 95)

    all_results = []

    full_benches = [
        ("object",                  bench_object_engine),
        ("column_push",             bench_column_push),
        ("column_trunc_str_raw",    bench_column_trunc_str),
        ("column_trunc_str_prepacked", bench_column_trunc_str_prepacked),
        ("arrow",                   bench_arrow),
        ("pickle",                  bench_pickle),
    ]
    numeric_benches = [
        ("column_truncate", bench_column_truncate),
        ("arrow_num",       bench_arrow_numeric),
        ("pickle_num",      bench_pickle_numeric),
    ]

    for N in Ns:
        # ---- Section A: full schema (with STR `name`) ----
        print(f"\n  Running N={N:,}  reps={reps}  [Section A — full schema with STR] ...", end="", flush=True)
        full_results = []
        for name, fn in full_benches:
            try:
                full_results.append(fn(N, reps))
                print(f" [{name}]", end="", flush=True)
            except Exception as e:
                full_results.append({"system": name, "error": str(e)})
                print(f" [{name}-ERR: {e}]", end="", flush=True)
        print()
        print_table(
            full_results, N,
            title="Section A — Full schema with STR (raw vs prepacked)",
            schema_desc="row_id: U32 | x, y, z: F64 | name: STR (raw tbl.fill vs prepacked pack_utf8_column + fill_utf8)",
        )

        # ---- Section B: numeric-only (apples-to-apples for ColumnEngine truncate) ----
        print(f"\n  Running N={N:,}  reps={reps}  [Section B — numeric-only] ...", end="", flush=True)
        num_results = []
        for name, fn in numeric_benches:
            try:
                num_results.append(fn(N, reps))
                print(f" [{name}]", end="", flush=True)
            except Exception as e:
                num_results.append({"system": name, "error": str(e)})
                print(f" [{name}-ERR: {e}]", end="", flush=True)
        print()
        print_table(
            num_results, N,
            title="Section B — Numeric only (apples-to-apples truncate fast path)",
            schema_desc="row_id: U32 | x, y, z: F64",
        )

        all_results.append({
            "N": N,
            "section_full": full_results,
            "section_numeric": num_results,
        })

    if args.output_json:
        existing = []
        if os.path.exists(args.output_json):
            with open(args.output_json) as f:
                existing = json.load(f)
        existing.extend(all_results)
        with open(args.output_json, "w") as f:
            json.dump(existing, f, indent=2)
        print(f"\n  Results written to {args.output_json}")

    print("\n  Done.")


if __name__ == "__main__":
    main()
