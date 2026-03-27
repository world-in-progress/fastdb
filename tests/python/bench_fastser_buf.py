"""
Benchmark: FastSerializer buffer-protocol performance.

Measures dumps/loads latency for numpy arrays at various sizes.
Output: single metric line "METRIC=<value>" where value = geometric mean of
dumps+loads times across all sizes (in microseconds). Lower is better.
"""
import time
import sys
import math
import numpy as np

sys.path.insert(0, "python")
from fastdb4py import Feature, F64, U32, I32, STR
from fastdb4py.serializer import FastSerializer
from typing import List


# --- Feature definitions for benchmark ---

class ScalarPoint(Feature):
    x: F64
    y: F64
    z: F64

class WithFloatList(Feature):
    label: STR
    values: List[float]

class WithF64List(Feature):
    label: STR
    values: List[F64]

class WithU32List(Feature):
    label: STR
    ids: List[U32]

class WithNdArray(Feature):
    label: STR
    data: object  # numpy array, detected at runtime

class WithNdArrayU32(Feature):
    label: STR
    data: object


# --- Helpers ---

def bench(fn, warmup=3, repeat=20):
    """Return median time in microseconds."""
    for _ in range(warmup):
        fn()
    times = []
    for _ in range(repeat):
        t0 = time.perf_counter_ns()
        fn()
        t1 = time.perf_counter_ns()
        times.append((t1 - t0) / 1000.0)  # ns → µs
    times.sort()
    return times[len(times) // 2]  # median


def roundtrip_check(obj, cls):
    """Verify dumps→loads roundtrip correctness."""
    data = FastSerializer.dumps(obj)
    loaded = FastSerializer.loads(data, cls)
    return data, loaded


# --- Benchmark Cases ---

SIZES = [10, 100, 1000, 10_000]

def run_benchmarks():
    results = {}

    # 1. List[float] (blob path)
    print("=== List[float] (blob-encoded) ===")
    for n in SIZES:
        obj = WithFloatList(label="test", values=[float(i) * 0.1 for i in range(n)])
        data_bytes = FastSerializer.dumps(obj)

        t_dumps = bench(lambda: FastSerializer.dumps(obj))
        t_loads = bench(lambda: FastSerializer.loads(data_bytes, WithFloatList))
        results[f"list_float_dumps_{n}"] = t_dumps
        results[f"list_float_loads_{n}"] = t_loads
        print(f"  N={n:>6}: dumps={t_dumps:>10.1f} µs, loads={t_loads:>10.1f} µs, size={len(data_bytes)} bytes")

    # 2. List[F64] (auxiliary columnar layer path)
    print("\n=== List[F64] (auxiliary layer) ===")
    for n in SIZES:
        obj = WithF64List(label="test", values=[float(i) * 0.1 for i in range(n)])
        data_bytes = FastSerializer.dumps(obj)

        t_dumps = bench(lambda: FastSerializer.dumps(obj))
        t_loads = bench(lambda: FastSerializer.loads(data_bytes, WithF64List))
        results[f"list_f64_dumps_{n}"] = t_dumps
        results[f"list_f64_loads_{n}"] = t_loads
        print(f"  N={n:>6}: dumps={t_dumps:>10.1f} µs, loads={t_loads:>10.1f} µs, size={len(data_bytes)} bytes")

    # 3. List[U32] (auxiliary columnar layer path)
    print("\n=== List[U32] (auxiliary layer) ===")
    for n in SIZES:
        obj = WithU32List(label="test", ids=[i % 100000 for i in range(n)])
        data_bytes = FastSerializer.dumps(obj)

        t_dumps = bench(lambda: FastSerializer.dumps(obj))
        t_loads = bench(lambda: FastSerializer.loads(data_bytes, WithU32List))
        results[f"list_u32_dumps_{n}"] = t_dumps
        results[f"list_u32_loads_{n}"] = t_loads
        print(f"  N={n:>6}: dumps={t_dumps:>10.1f} µs, loads={t_loads:>10.1f} µs, size={len(data_bytes)} bytes")

    # 4. Scalar point (baseline small object)
    print("\n=== Scalar Feature (3×F64) ===")
    obj = ScalarPoint(x=1.0, y=2.0, z=3.0)
    data_bytes = FastSerializer.dumps(obj)
    t_dumps = bench(lambda: FastSerializer.dumps(obj))
    t_loads = bench(lambda: FastSerializer.loads(data_bytes, ScalarPoint))
    results["scalar_dumps"] = t_dumps
    results["scalar_loads"] = t_loads
    print(f"  dumps={t_dumps:.1f} µs, loads={t_loads:.1f} µs, size={len(data_bytes)} bytes")

    # 5. numpy ndarray F64 (__fastser_buf__ layer path)
    print("\n=== numpy ndarray F64 (buffer layer) ===")
    for n in SIZES:
        arr = np.arange(n, dtype=np.float64) * 0.1
        obj = WithNdArray(label="test", data=arr)
        data_bytes = FastSerializer.dumps(obj)

        t_dumps = bench(lambda: FastSerializer.dumps(obj))
        t_loads = bench(lambda: FastSerializer.loads(data_bytes, WithNdArray))
        results[f"ndarray_f64_dumps_{n}"] = t_dumps
        results[f"ndarray_f64_loads_{n}"] = t_loads
        # Verify correctness
        loaded = FastSerializer.loads(data_bytes, WithNdArray)
        assert isinstance(loaded.data, np.ndarray), f"Expected ndarray, got {type(loaded.data)}"
        np.testing.assert_array_almost_equal(loaded.data, arr)
        print(f"  N={n:>6}: dumps={t_dumps:>10.1f} µs, loads={t_loads:>10.1f} µs, size={len(data_bytes)} bytes")

    # 6. numpy ndarray U32 (buffer layer)
    print("\n=== numpy ndarray U32 (buffer layer) ===")
    for n in SIZES:
        arr = np.arange(n, dtype=np.uint32)
        obj = WithNdArrayU32(label="test", data=arr)
        data_bytes = FastSerializer.dumps(obj)

        t_dumps = bench(lambda: FastSerializer.dumps(obj))
        t_loads = bench(lambda: FastSerializer.loads(data_bytes, WithNdArrayU32))
        results[f"ndarray_u32_dumps_{n}"] = t_dumps
        results[f"ndarray_u32_loads_{n}"] = t_loads
        print(f"  N={n:>6}: dumps={t_dumps:>10.1f} µs, loads={t_loads:>10.1f} µs, size={len(data_bytes)} bytes")

    # Compute aggregate metric: geometric mean of all dumps+loads times
    all_times = list(results.values())
    geo_mean = math.exp(sum(math.log(t) for t in all_times) / len(all_times))

    print(f"\n{'='*60}")
    print(f"METRIC={geo_mean:.2f}")
    print(f"{'='*60}")

    return geo_mean, results


if __name__ == "__main__":
    geo_mean, results = run_benchmarks()
