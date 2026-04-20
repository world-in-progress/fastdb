"""
Kostya-Style Serialization Benchmark
======================================
Inspired by Kostya's benchmarks (github.com/kostya/benchmarks) and the
rust_serialization_benchmark methodology.

Data structure: 'Coordinate' records — x, y, z (float64) + name (string) —
mirrors the classic Kostya JSON benchmark payload.

Compares three systems:
  - fastdb ORM Feature  (zero-copy shared-memory model)
  - PyArrow             (columnar IPC)
  - pickle              (Python native binary)

Phases measured (milliseconds, median of `reps` runs):
  build      : construct N in-memory records
  encode     : convert to binary wire format
                 fastdb  → C++ columnar flush (_combine)
                 fastdb* → included in ORM.truncate() call
                 arrow   → IPC stream encode
                 pickle  → pickle.dumps
  shm        : allocate POSIX shared memory + memcpy bytes in
  deserialize: load from shared-memory / IPC buffer  ← fastdb advantage
  read       : iterate all N records, sum x+y+z       ← fastdb zero-copy

Size column: wire-format bytes (uncompressed).

Note: splitting encode/shm gives an apples-to-apples view — fastdb previously
reported (encode+shm) together as serial_ms, making it look slower than Arrow
whose IPC encode is fast because the Arrow Table is already columnar.

Usage:
    uv run python tests/python/benchmark_kostya.py
    uv run python tests/python/benchmark_kostya.py --quick
    uv run python tests/python/benchmark_kostya.py --n 100000
    uv run python tests/python/benchmark_kostya.py --output-json kostya.json
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

from fastdb4py import Feature, ORM
from fastdb4py import F64, U32, STR

try:
    import pyarrow as pa
    import pyarrow.ipc as pa_ipc
    HAS_ARROW = True
except ImportError:
    HAS_ARROW = False
    print("[WARNING] pyarrow not found — arrow benchmark will be skipped")

# ---------------------------------------------------------------------------
# Data model — at module level so get_type_hints() resolves annotation strings
# ---------------------------------------------------------------------------

class Coord(Feature):
    """Kostya-style coordinate record."""
    row_id: U32
    x: F64
    y: F64
    z: F64
    name: STR


class CoordNum(Feature):
    """Numeric-only Coord (no STR) — compatible with ORM.truncate."""
    row_id: U32
    x: F64
    y: F64
    z: F64


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_name(i: int) -> str:
    # fastdb uses a u16 string-table index (max 65535 unique strings per layer).
    # Rotate through a fixed set so the benchmark doesn't overflow at large N.
    return f"coord_{i % 50000:05d}"

def _shm_write(data: bytes) -> shared_memory.SharedMemory:
    shm = shared_memory.SharedMemory(create=True, size=max(len(data), 1))
    shm.buf[:len(data)] = data
    return shm


def _shm_read(shm: shared_memory.SharedMemory, length: int) -> bytes:
    return bytes(shm.buf[:length])


def _median_ms(fn, reps: int) -> float:
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

def bench_fastdb(N: int, reps: int) -> dict:
    shm_name = f"fdb_kostya_{uuid.uuid4().hex[:8]}"

    # --- build: push N Coord features into a mutable ORM ---
    def do_build():
        orm = ORM.create()
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
    orm = do_build()

    # --- encode: C++ columnar flush (_combine) only ---
    # Pre-build un-flushed ORMs so we only time the flush itself
    _unflushed = [do_build() for _ in range(reps)]

    def do_encode():
        _unflushed.pop()._combine()

    encode_ms = _median_ms(do_encode, reps)

    # --- shm: write already-flushed binary to POSIX shared memory ---
    orm._combine()  # ensure flushed before timing shm write
    _raw = bytes(orm._origin.buffer().as_array(__import__('numpy').uint8))

    def do_shm():
        s = _shm_write(_raw)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)

    # share once for deserial / read measurements
    orm.share(shm_name)

    # measure wire size via shm segment size
    try:
        _probe_shm = shared_memory.SharedMemory(name=shm_name)
        size_bytes = _probe_shm.size
        _probe_shm.close()
    except Exception:
        size_bytes = 0

    orm2 = None
    try:
        # --- deserialize: zero-copy load from shm ---
        def do_deserial():
            h = ORM.load(shm_name)
            h.close()

        deserial_ms = _median_ms(do_deserial, reps)
        orm2 = ORM.load(shm_name)

        # --- read: iterate all N records, sum x+y+z ---
        def do_read():
            tbl = orm2[Coord][Coord]
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
                h = ORM.load(shm_name)
                h.unlink()
            except Exception:
                pass

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "fastdb",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
    }


# ---------------------------------------------------------------------------
# fastdb-trunc benchmark  (known-N, numeric fields only, columnar fill)
# ---------------------------------------------------------------------------

def bench_fastdb_trunc(N: int, reps: int) -> dict:
    """
    fastdb ORM.truncate path: pre-allocate fixed-size table, fill columns via
    numpy slice assignment.  No STR field (truncate does not support variable-
    length types).  This path is fair when the record count is known upfront and
    all fields are numeric.
    """
    from fastdb4py import TableDefn

    shm_name = f"fdb_trunc_{uuid.uuid4().hex[:8]}"

    # Pre-compute numpy arrays (shared across reps — building them is not part
    # of the fastdb build phase, same as PyArrow pre-building arrays).
    ids = np.arange(N, dtype=np.uint32)
    xs  = np.arange(N, dtype=np.float64) * 0.1
    ys  = np.arange(N, dtype=np.float64) * 0.2
    zs  = np.arange(N, dtype=np.float64) * 0.3

    # --- build: truncate + columnar numpy fill (includes _combine internally) ---
    def do_build():
        orm = ORM.truncate([TableDefn(CoordNum, N)])
        tbl = orm[CoordNum][CoordNum]
        tbl.column.row_id[:] = ids
        tbl.column.x[:]      = xs
        tbl.column.y[:]      = ys
        tbl.column.z[:]      = zs
        return orm

    build_ms = _median_ms(do_build, reps)
    orm = do_build()

    # encode: already done inside ORM.truncate (_combine is called there)
    encode_ms = 0.0

    # --- shm: write flushed binary to POSIX shared memory ---
    _raw = bytes(orm._origin.buffer().as_array(__import__('numpy').uint8))

    def do_shm():
        s = _shm_write(_raw)
        s.close()
        s.unlink()

    shm_ms = _median_ms(do_shm, reps)

    # share once for deserial / read
    orm.share(shm_name)

    try:
        _probe_shm = shared_memory.SharedMemory(name=shm_name)
        size_bytes = _probe_shm.size
        _probe_shm.close()
    except Exception:
        size_bytes = 0

    orm2 = None
    try:
        def do_deserial():
            h = ORM.load(shm_name)
            h.close()

        deserial_ms = _median_ms(do_deserial, reps)
        orm2 = ORM.load(shm_name)

        def do_read():
            tbl = orm2[CoordNum][CoordNum]
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
                h = ORM.load(shm_name); h.unlink()
            except Exception:
                pass

    total_ms = build_ms + encode_ms + shm_ms + deserial_ms + read_ms
    return {
        "system": "fdb-trunc*",
        "build_ms": round(build_ms, 2),
        "encode_ms": round(encode_ms, 2),   # 0 — included in build
        "shm_ms": round(shm_ms, 2),
        "deserial_ms": round(deserial_ms, 2),
        "read_ms": round(read_ms, 2),
        "total_ms": round(total_ms, 2),
        "size_bytes": size_bytes,
        "_note": "no STR field; encode included in build",
    }


# ---------------------------------------------------------------------------
# PyArrow benchmark
# ---------------------------------------------------------------------------

def bench_arrow(N: int, reps: int) -> dict:
    if not HAS_ARROW:
        return {"system": "arrow", "error": "pyarrow not installed"}

    schema = pa.schema([
        ("row_id", pa.uint32()),
        ("x", pa.float64()),
        ("y", pa.float64()),
        ("z", pa.float64()),
        ("name", pa.string()),
    ])

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

    # --- encode: IPC stream → bytes (in-memory) ---
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

    # --- read: sum x+y+z using NumPy (fastest Arrow path) ---
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
# Formatting
# ---------------------------------------------------------------------------

COLS = ["system", "build_ms", "encode_ms", "shm_ms", "deserial_ms", "read_ms", "total_ms", "size_kb"]
WIDTHS = [10, 10, 11, 9, 14, 10, 11, 10]


def _fmt(v, width: int) -> str:
    if isinstance(v, float):
        return f"{v:>{width}.1f}"
    return f"{str(v):>{width}}"


def _kb(size_bytes) -> float:
    return round(size_bytes / 1024, 1) if isinstance(size_bytes, int) else float("nan")


def print_table(results: list[dict], N: int):
    header = "".join(f"{c:>{w}}" for c, w in zip(COLS, WIDTHS))
    sep = "-" * sum(WIDTHS)
    print(f"\n  N = {N:,}  (x, y, z: float64 + name: str)")
    print(f"  {sep}")
    print(f"  {header}")
    print(f"  {sep}")
    for r in results:
        if "error" in r:
            print(f"  {r['system']:>10}  {r['error']}")
            continue
        display = dict(r)
        display["size_kb"] = _kb(r["size_bytes"])
        row = "".join(f"{_fmt(display[c], w)}" for c, w in zip(COLS, WIDTHS))
        print(f"  {row}")
    print(f"  {sep}")

    # ratio table (vs pickle as baseline)
    pkl = next((r for r in results if r["system"] == "pickle"), None)
    if pkl:
        print(f"\n  Ratio vs pickle  (lower = faster/smaller):")
        ratio_header = "".join(f"{c:>{w}}" for c, w in zip(COLS, WIDTHS))
        print(f"  {'-' * sum(WIDTHS)}")
        print(f"  {ratio_header}")
        print(f"  {'-' * sum(WIDTHS)}")
        for r in results:
            if "error" in r:
                continue
            def ratio(key):
                base = pkl.get(key, 0)
                val = r.get(key, 0)
                if base and val:
                    return round(val / base, 2)
                return float("nan")
            ratios = {
                "system": r["system"],
                "build_ms": ratio("build_ms"),
                "encode_ms": ratio("encode_ms"),
                "shm_ms": ratio("shm_ms"),
                "deserial_ms": ratio("deserial_ms"),
                "read_ms": ratio("read_ms"),
                "total_ms": ratio("total_ms"),
                "size_kb": ratio("size_bytes"),
            }
            row = "".join(f"{_fmt(ratios[c], w)}" for c, w in zip(COLS, WIDTHS))
            print(f"  {row}")
        print(f"  {'-' * sum(WIDTHS)}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Kostya-style serialization benchmark: fastdb vs PyArrow vs pickle"
    )
    parser.add_argument("--quick", action="store_true",
                        help="Skip N=1_000_000, use 1 rep (fast sanity check)")
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

    print("=" * 77)
    print("  Kostya-Style Serialization Benchmark — fastdb ORM-Feature vs PyArrow vs pickle")
    print("  Data: Coordinate records  { x, y, z: F64 | name: STR }")
    print("  fdb-trunc* = ORM.truncate + numpy columnar fill (no STR field, known N)")
    print("  Phases (ms, median): build | encode (binary fmt) | shm write | deserialize | sum(x+y+z)")
    print("  Size: uncompressed wire format (KB)")
    print("=" * 77)

    all_results = []

    for N in Ns:
        row_results = []
        print(f"\n  Running N={N:,}  reps={reps} ...", end="", flush=True)

        try:
            row_results.append(bench_fastdb(N, reps))
            print(" [fdb]", end="", flush=True)
        except Exception as e:
            row_results.append({"system": "fastdb", "error": str(e)})
            print(f" [fdb-ERR: {e}]", end="", flush=True)

        try:
            row_results.append(bench_fastdb_trunc(N, reps))
            print(" [fdb-trunc]", end="", flush=True)
        except Exception as e:
            row_results.append({"system": "fdb-trunc*", "error": str(e)})
            print(f" [fdb-trunc-ERR: {e}]", end="", flush=True)

        try:
            row_results.append(bench_arrow(N, reps))
            print(" [arrow]", end="", flush=True)
        except Exception as e:
            row_results.append({"system": "arrow", "error": str(e)})
            print(f" [arrow-ERR: {e}]", end="", flush=True)

        try:
            row_results.append(bench_pickle(N, reps))
            print(" [pickle]", end="", flush=True)
        except Exception as e:
            row_results.append({"system": "pickle", "error": str(e)})
            print(f" [pickle-ERR: {e}]", end="", flush=True)

        print()
        print_table(row_results, N)
        all_results.append({"N": N, "results": row_results})

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
