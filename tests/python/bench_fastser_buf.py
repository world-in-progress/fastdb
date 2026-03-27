"""
Benchmark: FastSerializer vs pickle for a complex Feature.

Uses a realistic Feature that mixes scalars, strings, typed numeric lists,
and numpy ndarrays — the kind of object you'd actually serialize in production.

Output: single metric line "METRIC=<value>" where value = geometric mean of
FastSerializer dumps+loads times across all test cases (in µs). Lower is better.
Also reports pickle times for comparison (not part of metric).
"""
import time
import sys
import math
import pickle
import numpy as np
from dataclasses import dataclass, field

sys.path.insert(0, "python")
from fastdb4py import Feature, F64, U32, I32, STR
from fastdb4py.serializer import FastSerializer
from typing import List


# --- Complex Feature definition ---
# Realistic point-cloud-like object: scalars + string + typed numeric lists + ndarray

class PointCloud(Feature):
    name: STR
    id: U32
    timestamp: F64
    quality: F64
    positions: List[F64]     # 3N floats (x,y,z interleaved)
    indices: List[U32]       # triangle indices
    labels: List[str]        # string labels (non-numeric)
    weights: object          # numpy float64 array (buffer layer path)


# Plain dataclass equivalent for pickle comparison
@dataclass
class PointCloudPlain:
    name: str
    id: int
    timestamp: float
    quality: float
    positions: list
    indices: list
    labels: list
    weights: np.ndarray = field(default_factory=lambda: np.array([]))


# --- Helpers ---

def bench(fn, warmup=5, repeat=30):
    """Return median time in microseconds."""
    for _ in range(warmup):
        fn()
    times = []
    for _ in range(repeat):
        t0 = time.perf_counter_ns()
        fn()
        t1 = time.perf_counter_ns()
        times.append((t1 - t0) / 1000.0)
    times.sort()
    return times[len(times) // 2]


# --- Benchmark ---

SIZES = [10, 100, 1000, 10_000]

def make_cloud(n):
    """Create a PointCloud Feature with n vertices."""
    return PointCloud(
        name=f"cloud_{n}",
        id=n,
        timestamp=1234567890.123,
        quality=0.95,
        positions=[float(i) * 0.01 for i in range(n * 3)],
        indices=[i % n for i in range(n * 3)],
        labels=[f"v{i}" for i in range(min(n, 20))],
        weights=np.random.rand(n).astype(np.float64),
    )

def make_cloud_plain(n):
    """Create equivalent plain dataclass for pickle comparison."""
    return PointCloudPlain(
        name=f"cloud_{n}",
        id=n,
        timestamp=1234567890.123,
        quality=0.95,
        positions=[float(i) * 0.01 for i in range(n * 3)],
        indices=[i % n for i in range(n * 3)],
        labels=[f"v{i}" for i in range(min(n, 20))],
        weights=np.random.rand(n).astype(np.float64),
    )


def run_benchmarks():
    results = {}
    pickle_results = {}

    print("=" * 72)
    print("  FastSerializer vs pickle — Complex PointCloud Feature")
    print("=" * 72)

    for n in SIZES:
        obj = make_cloud(n)
        obj_plain = make_cloud_plain(n)

        # --- FastSerializer ---
        fdb_bytes = FastSerializer.dumps(obj)
        t_fdb_dumps = bench(lambda: FastSerializer.dumps(obj))
        t_fdb_loads = bench(lambda: FastSerializer.loads(fdb_bytes, PointCloud))

        results[f"fdb_dumps_{n}"] = t_fdb_dumps
        results[f"fdb_loads_{n}"] = t_fdb_loads

        # --- pickle (protocol 5) on equivalent plain dataclass ---
        pkl_bytes = pickle.dumps(obj_plain, protocol=5)
        t_pkl_dumps = bench(lambda: pickle.dumps(obj_plain, protocol=5))
        t_pkl_loads = bench(lambda: pickle.loads(pkl_bytes))

        pickle_results[f"pkl_dumps_{n}"] = t_pkl_dumps
        pickle_results[f"pkl_loads_{n}"] = t_pkl_loads

        ratio_d = t_fdb_dumps / t_pkl_dumps if t_pkl_dumps > 0 else float('inf')
        ratio_l = t_fdb_loads / t_pkl_loads if t_pkl_loads > 0 else float('inf')

        print(f"\n  N={n:>6} vertices  (fdb {len(fdb_bytes):>8} B, pkl {len(pkl_bytes):>8} B)")
        print(f"    dumps:  fdb={t_fdb_dumps:>8.1f} µs  pkl={t_pkl_dumps:>8.1f} µs  ratio={ratio_d:>5.1f}×")
        print(f"    loads:  fdb={t_fdb_loads:>8.1f} µs  pkl={t_pkl_loads:>8.1f} µs  ratio={ratio_l:>5.1f}×")

    # Aggregate metric: geometric mean of FastSerializer times only
    all_times = list(results.values())
    geo_mean = math.exp(sum(math.log(t) for t in all_times) / len(all_times))

    pkl_times = list(pickle_results.values())
    pkl_geo = math.exp(sum(math.log(t) for t in pkl_times) / len(pkl_times))

    print(f"\n{'=' * 72}")
    print(f"  FastSerializer geo-mean: {geo_mean:>8.2f} µs")
    print(f"  pickle geo-mean:         {pkl_geo:>8.2f} µs")
    print(f"  ratio (fdb/pickle):      {geo_mean/pkl_geo:>8.1f}×")
    print(f"{'=' * 72}")
    print(f"METRIC={geo_mean:.2f}")

    return geo_mean, results


if __name__ == "__main__":
    run_benchmarks()
