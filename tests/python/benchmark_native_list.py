"""
Native List Column Benchmark
=============================
Compares fastdb native list columns vs PyArrow vs pickle across:
  - build   : construct in-memory structure
  - serial  : write to shared-memory / IPC buffer
  - deserial: load from shared-memory / IPC buffer
  - read_col: iterate all N rows and access the list column (force NumPy view)

Matrix: N ∈ {10_000, 100_000, 1_000_000} × list_len ∈ {8, 64, 512}

Usage:
    uv run python tests/python/benchmark_native_list.py
    uv run python tests/python/benchmark_native_list.py --quick
    uv run python tests/python/benchmark_native_list.py --output-json results_list.json
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
from typing import List

import numpy as np

from fastdb4py import Feature, ORM
from fastdb4py import U32, F64

try:
    import pyarrow as pa
    import pyarrow.ipc as pa_ipc
    HAS_ARROW = True
except ImportError:
    HAS_ARROW = False
    print("[WARNING] pyarrow not found — arrow benchmark will be skipped")

# ---------------------------------------------------------------------------
# Feature class must be at module level so get_type_hints() resolves 'F64' etc.
# ---------------------------------------------------------------------------

class BenchListFeature(Feature):
    row_id: U32
    xs: List[F64]


# ---------------------------------------------------------------------------
# Shared memory helpers (POSIX)
# ---------------------------------------------------------------------------

def _shm_write(data: bytes) -> shared_memory.SharedMemory:
    shm = shared_memory.SharedMemory(create=True, size=max(len(data), 1))
    shm.buf[:len(data)] = data
    return shm


def _shm_read(shm: shared_memory.SharedMemory, length: int) -> bytes:
    return bytes(shm.buf[:length])


# ---------------------------------------------------------------------------
# Timer
# ---------------------------------------------------------------------------

def _median_ms(fn, reps: int) -> float:
    """Run fn() `reps` times, return median elapsed ms."""
    times = []
    for _ in range(reps):
        gc.collect()
        t0 = time.perf_counter()
        fn()
        times.append((time.perf_counter() - t0) * 1000)
    return statistics.median(times)


# ---------------------------------------------------------------------------
# fastdb benchmark
# ---------------------------------------------------------------------------

def bench_fastdb(N: int, list_len: int, reps: int) -> dict:
    xs_template = [float(j) for j in range(list_len)]
    shm_name = f"fdb_bench_{uuid.uuid4().hex[:8]}"

    # --- build ---
    def do_build():
        orm = ORM.create()
        for i in range(N):
            f = BenchListFeature()
            f.row_id = i
            f.xs = xs_template[:]
            orm.push(f)
        return orm

    build_ms = _median_ms(do_build, reps)
    orm = do_build()

    # --- serialize: share to POSIX shm (done once — shm name unique per run) ---
    t0 = time.perf_counter()
    orm.share(shm_name)
    serial_ms = (time.perf_counter() - t0) * 1000

    orm2 = None
    try:
        # --- deserialize (load) ---
        def do_deserial():
            h = ORM.load(shm_name)
            h.close()

        deserial_ms = _median_ms(do_deserial, reps)
        orm2 = ORM.load(shm_name)

        # --- read column (access xs for all N features) ---
        def do_read():
            total = 0.0
            for i in range(N):
                f = orm2[BenchListFeature][BenchListFeature][i]
                xs = f.xs  # zero-copy NumPy
                total += xs[0]
            return total

        read_ms = _median_ms(do_read, reps)
    finally:
        if orm2 is not None:
            orm2.unlink()
        else:
            # clean up shm even if deserialization failed
            try:
                h = ORM.load(shm_name)
                h.unlink()
            except Exception:
                pass

    total_ms = build_ms + serial_ms + deserial_ms + read_ms
    data_bytes = N * list_len * 8  # 8 bytes per float64
    return {
        "system": "fastdb",
        "build_ms": round(build_ms, 2),
        "serial_ms": round(serial_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_col_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "throughput_mb_s": round(data_bytes / 1024 / 1024 / (total_ms / 1000), 1),
    }


# ---------------------------------------------------------------------------
# PyArrow benchmark
# ---------------------------------------------------------------------------

def bench_arrow(N: int, list_len: int, reps: int) -> dict:
    if not HAS_ARROW:
        return {"system": "arrow", "error": "pyarrow not installed"}

    xs_template = [float(j) for j in range(list_len)]
    schema = pa.schema([("row_id", pa.uint32()), ("xs", pa.list_(pa.float64()))])

    # --- build ---
    def do_build():
        ids = list(range(N))
        xss = [xs_template[:] for _ in range(N)]
        tbl = pa.table({"row_id": pa.array(ids, type=pa.uint32()),
                        "xs":     pa.array(xss, type=pa.list_(pa.float64()))})
        return tbl

    build_ms = _median_ms(do_build, reps)
    tbl = do_build()

    # --- serialize (IPC → bytes → shm) ---
    def _ipc_serialize(t) -> bytes:
        sink = pa.BufferOutputStream()
        writer = pa_ipc.new_stream(sink, t.schema)
        writer.write_table(t)
        writer.close()
        return sink.getvalue().to_pybytes()

    def do_serial():
        buf = _ipc_serialize(tbl)
        shm = _shm_write(buf)
        shm.close()
        shm.unlink()

    serial_ms = _median_ms(do_serial, reps)
    ipc_bytes = _ipc_serialize(tbl)
    shm = _shm_write(ipc_bytes)

    # --- deserialize (shm → IPC → table) ---
    def do_deserial():
        raw = _shm_read(shm, len(ipc_bytes))
        buf = pa.py_buffer(raw)
        reader = pa_ipc.open_stream(buf)
        _ = reader.read_all()

    deserial_ms = _median_ms(do_deserial, reps)
    raw = _shm_read(shm, len(ipc_bytes))
    tbl2 = pa_ipc.open_stream(pa.py_buffer(raw)).read_all()

    # --- read column (access xs for all N rows, force NumPy materialization) ---
    def do_read():
        xs_col = tbl2.column("xs")
        total = 0.0
        for i in range(N):
            arr = xs_col[i].as_py()
            total += arr[0]
        return total

    read_ms = _median_ms(do_read, reps)

    shm.close()
    shm.unlink()

    total_ms = build_ms + serial_ms + deserial_ms + read_ms
    data_bytes = N * list_len * 8
    return {
        "system": "arrow",
        "build_ms": round(build_ms, 2),
        "serial_ms": round(serial_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_col_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "throughput_mb_s": round(data_bytes / 1024 / 1024 / (total_ms / 1000), 1),
    }


# ---------------------------------------------------------------------------
# Pickle benchmark
# ---------------------------------------------------------------------------

def bench_pickle(N: int, list_len: int, reps: int) -> dict:
    xs_template = [float(j) for j in range(list_len)]

    # --- build ---
    def do_build():
        return [{"row_id": i, "xs": xs_template[:]} for i in range(N)]

    build_ms = _median_ms(do_build, reps)
    data = do_build()

    # --- serialize (pickle → bytes → shm) ---
    def do_serial():
        buf = pickle.dumps(data)
        shm = _shm_write(buf)
        shm.close()
        shm.unlink()

    serial_ms = _median_ms(do_serial, reps)
    pkl_bytes = pickle.dumps(data)
    shm = _shm_write(pkl_bytes)

    # --- deserialize (shm → unpickle) ---
    def do_deserial():
        raw = _shm_read(shm, len(pkl_bytes))
        _ = pickle.loads(raw)

    deserial_ms = _median_ms(do_deserial, reps)
    raw = _shm_read(shm, len(pkl_bytes))
    data2 = pickle.loads(raw)

    # --- read column (iterate all rows, access xs[0]) ---
    def do_read():
        total = 0.0
        for row in data2:
            total += row["xs"][0]
        return total

    read_ms = _median_ms(do_read, reps)

    shm.close()
    shm.unlink()

    total_ms = build_ms + serial_ms + deserial_ms + read_ms
    data_bytes = N * list_len * 8
    return {
        "system": "pickle",
        "build_ms": round(build_ms, 2),
        "serial_ms": round(serial_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_col_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "throughput_mb_s": round(data_bytes / 1024 / 1024 / (total_ms / 1000), 1),
    }


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

def _fmt(v, width=10):
    if isinstance(v, float):
        return f"{v:>{width}.1f}"
    return f"{str(v):>{width}}"


def print_table(results: list[dict], N: int, list_len: int):
    cols = ["system", "build_ms", "serial_ms", "deserial_ms", "read_col_ms", "total_ms", "throughput_mb_s"]
    widths = [12, 10, 12, 14, 13, 11, 17]
    header = "".join(f"{c:>{w}}" for c, w in zip(cols, widths))
    sep = "-" * sum(widths)
    print(f"\n  N={N:,}  list_len={list_len}")
    print(f"  {sep}")
    print(f"  {header}")
    print(f"  {sep}")
    for r in results:
        if "error" in r:
            print(f"  {'  '.join([r['system'], r['error']])}")
            continue
        row = "".join(f"{_fmt(r[c], w)}" for c, w in zip(cols, widths))
        print(f"  {row}")
    print(f"  {sep}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Native list column benchmark")
    parser.add_argument("--quick", action="store_true",
                        help="Skip N=1_000_000, use 1 repetition")
    parser.add_argument("--output-json", metavar="FILE",
                        help="Write results to JSON file")
    parser.add_argument("--reps", type=int, default=3,
                        help="Repetitions per cell (default 3)")
    args = parser.parse_args()

    Ns = [10_000, 100_000] if args.quick else [10_000, 100_000, 1_000_000]
    list_lens = [8, 64, 512]
    reps = 1 if args.quick else args.reps

    print("=" * 70)
    print("  Native List Column Benchmark — fastdb vs PyArrow vs pickle")
    print("  Phases: build | serialize-to-shm | deserialize | read-column")
    print("  Metrics in milliseconds (median); throughput in MB/s")
    print("=" * 70)

    all_results = []

    for N in Ns:
        for list_len in list_lens:
            row_results = []
            print(f"\n  Running N={N:,}  list_len={list_len} ...", end="", flush=True)

            try:
                row_results.append(bench_fastdb(N, list_len, reps))
                print(".", end="", flush=True)
            except Exception as e:
                row_results.append({"system": "fastdb", "error": str(e)})

            try:
                row_results.append(bench_arrow(N, list_len, reps))
                print(".", end="", flush=True)
            except Exception as e:
                row_results.append({"system": "arrow", "error": str(e)})

            try:
                row_results.append(bench_pickle(N, list_len, reps))
                print(".", end="", flush=True)
            except Exception as e:
                row_results.append({"system": "pickle", "error": str(e)})

            print_table(row_results, N, list_len)
            all_results.append({"N": N, "list_len": list_len, "results": row_results})

    if args.output_json:
        with open(args.output_json, "w") as f:
            json.dump(all_results, f, indent=2)
        print(f"\n  Results written to {args.output_json}")


if __name__ == "__main__":
    main()
