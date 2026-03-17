"""
fastdb4py Comprehensive Benchmark Suite
========================================

Covers all major operation paths to quantify performance bottlenecks:

  Section 1 – Microbenchmarks:  individual operation cost (ns precision)
  Section 2 – Meso-benchmarks:  ORM lifecycle operations
  Section 3 – Macro-benchmarks: real-world scenarios (point cloud, ref graphs)
  Section 4 – FastSerializer:   dumps/loads latency and throughput (vs pickle)

Usage:
    uv run python tests/python/benchmark_comprehensive.py
    uv run python tests/python/benchmark_comprehensive.py --quick
    uv run python tests/python/benchmark_comprehensive.py --sections micro,macro
    uv run python tests/python/benchmark_comprehensive.py --output-json results.json
"""

from __future__ import annotations

import argparse
import gc
import json
import math
import multiprocessing as mp
import pickle
import platform
import statistics
import tempfile
import time
import traceback
import uuid
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Callable, List, Optional

from fastdb4py import Feature, ORM, TableDefn, FastSerializer
from fastdb4py import U8, U16, U32, I32, F32, F64, STR, BOOL
from fastdb4py.feature.utils import parse_defns

# ──────────────────────────────────────────────────────────────────────────────
# Feature subclasses – MUST be at module level for get_type_hints() to resolve
# forward references correctly.
# ──────────────────────────────────────────────────────────────────────────────

class BenchPoint(Feature):
    """3 × F64: most common use case (point in 3D space)."""
    x: F64
    y: F64
    z: F64


class BenchIntPoint(Feature):
    """Mixed int/float: exercises different SWIG getter branches."""
    a: I32
    b: I32
    id: U32
    w: F32


class BenchTriPt(Feature):
    """Reference-target point: U32 idx + 3×F64."""
    idx: U32
    x: F64
    y: F64
    z: F64


class BenchTriangle(Feature):
    """1-level ref: Triangle with 3 BenchTriPt references."""
    id: U32
    a: BenchTriPt
    b: BenchTriPt
    c: BenchTriPt


class BenchNode(Feature):
    """Self-referential: I32 val + 'BenchNode' next (for cyclic/chain tests)."""
    val: I32
    next: 'BenchNode'


class BenchNL(Feature):
    """Numeric list payload: List[U32] + List[F64] (auxiliary columnar layers)."""
    ids: List[U32]
    values: List[F64]


class BenchMixed(Feature):
    """Mixed payload: STR name + F64 score + List[str] tags."""
    name: STR
    score: F64
    tags: List[str]


class BenchWide(Feature):
    """Wide feature: 10 × F64 – exposes O(n) ColumnAccessor field scan."""
    f0: F64
    f1: F64
    f2: F64
    f3: F64
    f4: F64
    f5: F64
    f6: F64
    f7: F64
    f8: F64
    f9: F64


class BenchPointCloud(Feature):
    """Container: List[BenchPoint] for FastSerializer tests."""
    points: List[BenchPoint]


# ──────────────────────────────────────────────────────────────────────────────
# Infrastructure
# ──────────────────────────────────────────────────────────────────────────────

@dataclass
class BenchResult:
    section: str
    name: str
    param: str
    n_ops: int
    mean_ns: float
    median_ns: float
    p95_ns: float
    p99_ns: float
    min_ns: float
    max_ns: float
    throughput: str = ""
    note: str = ""


@dataclass
class BenchConfig:
    n_iters: int = 300
    n_warmup: int = 30
    quick: bool = False
    sections: set = field(default_factory=lambda: {"micro", "meso", "macro", "serializer"})
    output_json: str = ""


def _timeit_ns(fn: Callable, n_iters: int, n_warmup: int) -> list[int]:
    for _ in range(n_warmup):
        fn()
    gc.collect()
    samples = []
    for _ in range(n_iters):
        t0 = time.perf_counter_ns()
        fn()
        samples.append(time.perf_counter_ns() - t0)
    return samples


def _stats(samples: list[int]) -> dict:
    s = sorted(samples)
    n = len(s)
    return {
        "mean":   statistics.mean(s),
        "median": s[n // 2],
        "p95":    s[max(0, int(n * 0.95) - 1)],
        "p99":    s[max(0, int(n * 0.99) - 1)],
        "min":    s[0],
        "max":    s[-1],
    }


def _fmt_ns(ns: float) -> str:
    if ns < 1_000:
        return f"{ns:6.1f} ns"
    if ns < 1_000_000:
        return f"{ns/1_000:6.2f} µs"
    return f"{ns/1_000_000:6.3f} ms"


def _throughput_str(n_ops: int, mean_ns: float) -> str:
    if mean_ns <= 0:
        return ""
    ops_per_s = n_ops / mean_ns * 1e9
    if ops_per_s >= 1e6:
        return f"{ops_per_s/1e6:.1f}M ops/s"
    if ops_per_s >= 1e3:
        return f"{ops_per_s/1e3:.1f}K ops/s"
    return f"{ops_per_s:.1f} ops/s"


def _make_result(
    section: str, name: str, param: str,
    samples: list[int], n_ops: int = 1, note: str = ""
) -> BenchResult:
    st = _stats(samples)
    tp = _throughput_str(n_ops, st["mean"])
    return BenchResult(
        section=section, name=name, param=param, n_ops=n_ops,
        mean_ns=st["mean"], median_ns=st["median"],
        p95_ns=st["p95"], p99_ns=st["p99"],
        min_ns=st["min"], max_ns=st["max"],
        throughput=tp, note=note,
    )


def _print_header(title: str):
    w = 84
    print(f"\n{'=' * w}")
    print(f"  {title}")
    print(f"{'=' * w}")
    hdr = f"{'Operation':<38} {'Param':<12} {'Median':>10} {'P95':>10} {'Mean':>10}  Note"
    print(hdr)
    print("-" * w)


def _print_row(r: BenchResult):
    print(
        f"{r.name:<38} {r.param:<12} "
        f"{_fmt_ns(r.median_ns):>10} {_fmt_ns(r.p95_ns):>10} {_fmt_ns(r.mean_ns):>10}  {r.note}"
    )


def _print_speedup(label_a: str, a: BenchResult, label_b: str, b: BenchResult):
    ratio = b.median_ns / a.median_ns if a.median_ns > 0 else float("inf")
    print(
        f"  >> {label_a} ({_fmt_ns(a.median_ns)}) vs "
        f"{label_b} ({_fmt_ns(b.median_ns)}) → speedup {ratio:.1f}×"
    )


# ──────────────────────────────────────────────────────────────────────────────
# Helpers to build fixed/dynamic databases
# ──────────────────────────────────────────────────────────────────────────────

def _build_truncated_point_db(n: int) -> ORM:
    return ORM.truncate([TableDefn(BenchPoint, n)])


def _get_point_table(db: ORM):
    return db[BenchPoint][BenchPoint]


def _build_triangle_db(n_triangles: int) -> ORM:
    """Build a dynamic ORM with n_triangles BenchTriangle + 3*n_triangles BenchTriPt."""
    db = ORM.create()
    for i in range(n_triangles):
        tri = BenchTriangle(id=i)
        tri.a = BenchTriPt(idx=3*i,   x=float(3*i),   y=0.0, z=0.0)
        tri.b = BenchTriPt(idx=3*i+1, x=float(3*i+1), y=1.0, z=0.0)
        tri.c = BenchTriPt(idx=3*i+2, x=float(3*i+2), y=0.0, z=1.0)
        db.push(tri)
    db._combine()
    return db


# ──────────────────────────────────────────────────────────────────────────────
# Section 1: Microbenchmarks
# ──────────────────────────────────────────────────────────────────────────────

def run_micro(cfg: BenchConfig) -> list[BenchResult]:
    _print_header("SECTION 1: MICROBENCHMARKS  (single operation overhead)")
    results: list[BenchResult] = []

    # ── Feature instantiation ──────────────────────────────────────────────────
    r = _make_result(
        "micro", "feature_init_pure_python", "BenchPoint",
        _timeit_ns(lambda: BenchPoint(x=1.0, y=2.0, z=3.0), cfg.n_iters, cfg.n_warmup),
        note="dict alloc + WeakKeyDict lookup ×2"
    )
    _print_row(r); results.append(r)

    # ── Scalar read – pure Python object (dict cache path) ────────────────────
    pt_pure = BenchPoint(x=3.14, y=2.71, z=1.41)
    r_read_pure = _make_result(
        "micro", "scalar_read_pure_python", "F64",
        _timeit_ns(lambda: pt_pure.x, cfg.n_iters, cfg.n_warmup),
        note="dict lookup in _cache"
    )
    _print_row(r_read_pure); results.append(r_read_pure)

    # ── DB-mapped feature setup ────────────────────────────────────────────────
    db_p = _build_truncated_point_db(1)
    tbl_p = _get_point_table(db_p)
    # Write initial value so read is non-trivial
    tbl_p[0].x = 3.14

    pt_db = BenchPoint.map_from(db_p._origin, db_p._origin.get_layer(0).tryGetFeature(0))

    r = _make_result(
        "micro", "feature_init_db_mapped", "BenchPoint",
        _timeit_ns(
            lambda: BenchPoint.map_from(
                db_p._origin,
                db_p._origin.get_layer(0).tryGetFeature(0)
            ),
            cfg.n_iters, cfg.n_warmup
        ),
        note="Feature() ctor + _db + _origin setattr"
    )
    _print_row(r); results.append(r)

    # ── Scalar read – db-mapped (SWIG path) ───────────────────────────────────
    r_read_db = _make_result(
        "micro", "scalar_read_db_mapped", "F64",
        _timeit_ns(lambda: pt_db.x, cfg.n_iters, cfg.n_warmup),
        note="__getattr__ + if-chain + SWIG get_field_as_float"
    )
    _print_row(r_read_db); results.append(r_read_db)

    # ── Scalar write – db-mapped ───────────────────────────────────────────────
    r = _make_result(
        "micro", "scalar_write_db_mapped", "F64",
        _timeit_ns(lambda: pt_db.__setattr__('x', 3.14), cfg.n_iters, cfg.n_warmup),
        note="__setattr__ + SWIG set_field"
    )
    _print_row(r); results.append(r)

    # ── I32 read – exercises different SWIG getter branch ─────────────────────
    db_i = ORM.truncate([TableDefn(BenchIntPoint, 1)])
    tbl_i = db_i[BenchIntPoint][BenchIntPoint]
    tbl_i[0].__setattr__('a', 42)
    pt_i = BenchIntPoint.map_from(db_i._origin, db_i._origin.get_layer(0).tryGetFeature(0))
    r = _make_result(
        "micro", "scalar_read_db_mapped", "I32",
        _timeit_ns(lambda: pt_i.a, cfg.n_iters, cfg.n_warmup),
        note="__getattr__ + if-chain + SWIG get_field_as_int"
    )
    _print_row(r); results.append(r)

    # ── Ref resolve – 1-level, fresh wrapper each time ────────────────────────
    db_tri = _build_triangle_db(1)
    tri_layer = db_tri._origin.get_layer(
        next(i for i in range(db_tri._origin.get_layer_count())
             if db_tri._origin.get_layer(i).name() == "BenchTriangle")
    )

    def _fresh_tri_a():
        tri = BenchTriangle.map_from(db_tri._origin, tri_layer.tryGetFeature(0))
        return tri.a  # first access: 3 SWIG calls + map_from

    r_ref_fresh = _make_result(
        "micro", "ref_resolve_1level_fresh", "Tri→BenchTriPt",
        _timeit_ns(_fresh_tri_a, cfg.n_iters, cfg.n_warmup),
        note="get_field_as_ref + tryGetFeature + map_from (3 SWIG calls)"
    )
    _print_row(r_ref_fresh); results.append(r_ref_fresh)

    # ── Ref resolve – cached (2nd access hits _cache) ─────────────────────────
    tri_cached = BenchTriangle.map_from(db_tri._origin, tri_layer.tryGetFeature(0))
    _ = tri_cached.a  # prime the cache

    r_ref_cached = _make_result(
        "micro", "ref_resolve_cached", "Tri→BenchTriPt",
        _timeit_ns(lambda: tri_cached.a, cfg.n_iters, cfg.n_warmup),
        note="_cache dict lookup (no SWIG)"
    )
    _print_row(r_ref_cached); results.append(r_ref_cached)

    # ── Schema cache hit ───────────────────────────────────────────────────────
    _ = parse_defns(BenchPoint)  # ensure cached
    r = _make_result(
        "micro", "schema_cache_hit", "BenchPoint",
        _timeit_ns(lambda: parse_defns(BenchPoint), cfg.n_iters, cfg.n_warmup),
        note="WeakKeyDict.__contains__ + __getitem__"
    )
    _print_row(r); results.append(r)

    # ── Schema cache miss – dynamically created class forces get_type_hints() ──
    miss_samples: list[int] = []
    miss_n = min(cfg.n_iters, 100)
    for i in range(miss_n):
        DynCls = type(f'_DynPoint_{i}_{id(object())}', (Feature,),
                      {'__annotations__': {'x': F64, 'y': F64}})
        t0 = time.perf_counter_ns()
        parse_defns(DynCls)
        miss_samples.append(time.perf_counter_ns() - t0)
    r = _make_result(
        "micro", "schema_cache_miss", "2-field dynamic class",
        miss_samples,
        note="get_type_hints() full traversal"
    )
    _print_row(r); results.append(r)

    # ── ColumnAccessor – O(n) scan: 3-field table, access last field ──────────
    db_p3 = _build_truncated_point_db(4)
    tbl_p3 = _get_point_table(db_p3)
    r_col3 = _make_result(
        "micro", "column_accessor_scan", "last of 3 fields",
        _timeit_ns(lambda: tbl_p3.column.z, cfg.n_iters, cfg.n_warmup),
        note="ColumnAccessor.__getattr__ O(n=3) linear scan"
    )
    _print_row(r_col3); results.append(r_col3)

    # ── ColumnAccessor – O(n) scan: 10-field table, access last field ─────────
    db_wide = ORM.truncate([TableDefn(BenchWide, 4)])
    tbl_wide = db_wide[BenchWide][BenchWide]
    r_col10 = _make_result(
        "micro", "column_accessor_scan", "last of 10 fields",
        _timeit_ns(lambda: tbl_wide.column.f9, cfg.n_iters, cfg.n_warmup),
        note="ColumnAccessor.__getattr__ O(n=10) linear scan"
    )
    _print_row(r_col10); results.append(r_col10)

    # ── Speedup callouts ──────────────────────────────────────────────────────
    print()
    _print_speedup("pure python scalar read",  r_read_pure,
                   "db-mapped scalar read",    r_read_db)
    _print_speedup("ref (cached)",              r_ref_cached,
                   "ref (fresh/SWIG)",          r_ref_fresh)
    _print_speedup("col scan 3 fields",         r_col3,
                   "col scan 10 fields",        r_col10)

    return results


# ──────────────────────────────────────────────────────────────────────────────
# Section 2: Meso-benchmarks
# ──────────────────────────────────────────────────────────────────────────────

def run_meso(cfg: BenchConfig) -> list[BenchResult]:
    _print_header("SECTION 2: MESO-BENCHMARKS  (ORM lifecycle)")
    results: list[BenchResult] = []

    sizes = [10, 100, 500] if cfg.quick else [10, 100, 1000, 5000]
    meso_iters = max(5, cfg.n_iters // 10)
    meso_warmup = max(2, cfg.n_warmup // 5)

    # ── ORM.truncate – dominated by _combine() post+load_xbuffer ──────────────
    for n in sizes:
        samples = _timeit_ns(
            lambda n=n: ORM.truncate([TableDefn(BenchPoint, n)]),
            meso_iters, meso_warmup
        )
        r = _make_result(
            "meso", "ORM.truncate", f"N={n}",
            samples, n_ops=n,
            note="schema defn + WxDatabaseBuild.truncate + _combine()"
        )
        _print_row(r); results.append(r)

    print()

    # ── ORM.create + push – per-push cost ─────────────────────────────────────
    push_sizes = [10, 100, 500] if cfg.quick else [10, 100, 1000]
    for n in push_sizes:
        proto = BenchPoint(x=1.0, y=2.0, z=3.0)
        samples = _timeit_ns(
            lambda n=n, proto=proto: _push_n(proto, n),
            meso_iters, meso_warmup
        )
        r = _make_result(
            "meso", "ORM.create + push", f"N={n}",
            samples, n_ops=n,
            note="add_feature_begin + N×set_field + add_feature_end (SWIG per field)"
        )
        _print_row(r); results.append(r)

    print()

    # ── _combine() isolation – post + load_xbuffer ────────────────────────────
    for n in push_sizes:
        def _build_and_combine(n=n):
            db = ORM.create()
            proto = BenchPoint(x=1.0, y=2.0, z=3.0)
            for _ in range(n):
                db.push(proto)
            db._combine()

        samples = _timeit_ns(_build_and_combine, meso_iters, meso_warmup)
        r = _make_result(
            "meso", "build+push+_combine", f"N={n}",
            samples, n_ops=n,
            note="full lifecycle: create→push×N→_combine (post+load_xbuffer)"
        )
        _print_row(r); results.append(r)

    print()

    # ── ORM.save + ORM.load (file) ────────────────────────────────────────────
    file_sizes = [100, 500] if cfg.quick else [100, 1000]
    for n in file_sizes:
        db_save = _build_truncated_point_db(n)
        with tempfile.NamedTemporaryFile(suffix='.fastdb', delete=False) as f:
            tmp_path = f.name
        try:
            db_save.save(tmp_path)

            save_samples = _timeit_ns(lambda: db_save.save(tmp_path), meso_iters, meso_warmup)
            r = _make_result(
                "meso", "ORM.save (file)", f"N={n}",
                save_samples, note="buffer().to_bytes() + file write"
            )
            _print_row(r); results.append(r)

            load_samples = _timeit_ns(
                lambda: ORM.load(tmp_path, from_file=True), meso_iters, meso_warmup
            )
            r = _make_result(
                "meso", "ORM.load (file)", f"N={n}",
                load_samples, note="WxDatabase.load() parse from file"
            )
            _print_row(r); results.append(r)
        finally:
            Path(tmp_path).unlink(missing_ok=True)

    return results


def _push_n(proto: Feature, n: int):
    db = ORM.create()
    for _ in range(n):
        db.push(proto)


# ──────────────────────────────────────────────────────────────────────────────
# Section 3: Macro-benchmarks
# ──────────────────────────────────────────────────────────────────────────────

def run_macro(cfg: BenchConfig) -> list[BenchResult]:
    _print_header("SECTION 3: MACRO-BENCHMARKS  (real-world scenarios)")
    results: list[BenchResult] = []

    cloud_n  = 1000 if cfg.quick else 10_000
    tri_n    = 100  if cfg.quick else 1000
    iter_n   = 500  if cfg.quick else 2000
    macro_iters = max(3, cfg.n_iters // 50)
    macro_warmup = max(1, cfg.n_warmup // 10)

    # ── Point cloud: row-wise write ───────────────────────────────────────────
    db_cloud = _build_truncated_point_db(cloud_n)
    tbl_cloud = _get_point_table(db_cloud)

    def _rowwise_write():
        for i in range(cloud_n):
            pt = tbl_cloud[i]
            pt.x = float(i)
            pt.y = float(i) * 0.5
            pt.z = float(i) * 0.25

    r_row_write = _make_result(
        "macro", "point_cloud_write_rowwise", f"N={cloud_n}",
        _timeit_ns(_rowwise_write, macro_iters, macro_warmup),
        n_ops=cloud_n,
        note="table[i]→tryGetFeature+map_from, pt.x=val→SWIG set_field ×3"
    )
    _print_row(r_row_write); results.append(r_row_write)

    # ── Point cloud: row-wise read ────────────────────────────────────────────
    def _rowwise_read():
        total = 0.0
        for i in range(cloud_n):
            pt = tbl_cloud[i]
            total += pt.x + pt.y + pt.z
        return total

    r_row_read = _make_result(
        "macro", "point_cloud_read_rowwise", f"N={cloud_n}",
        _timeit_ns(_rowwise_read, macro_iters, macro_warmup),
        n_ops=cloud_n,
        note="tryGetFeature+map_from+SWIG get_field_as_float ×3 per point"
    )
    _print_row(r_row_read); results.append(r_row_read)

    # ── Point cloud: column-wise read (numpy, zero-copy) ──────────────────────
    import numpy as np

    def _colwise_read():
        xs = tbl_cloud.column.x
        ys = tbl_cloud.column.y
        zs = tbl_cloud.column.z
        return float(xs.sum() + ys.sum() + zs.sum())

    r_col_read = _make_result(
        "macro", "point_cloud_read_columnwise", f"N={cloud_n}",
        _timeit_ns(_colwise_read, cfg.n_iters // 5, cfg.n_warmup),
        n_ops=cloud_n,
        note="ColumnAccessor O(n) scan + get_column SWIG + numpy sum (zero-copy)"
    )
    _print_row(r_col_read); results.append(r_col_read)

    # ── Point cloud: column-wise write (numpy in-place) ───────────────────────
    def _colwise_write():
        col = tbl_cloud.column.x
        col[:] = 1.0

    r_col_write = _make_result(
        "macro", "point_cloud_write_columnwise", f"N={cloud_n}",
        _timeit_ns(_colwise_write, cfg.n_iters // 5, cfg.n_warmup),
        n_ops=cloud_n,
        note="ColumnAccessor + numpy in-place write to C++ memory (zero-copy)"
    )
    _print_row(r_col_write); results.append(r_col_write)

    # ── Point cloud: fill() — batch column write via Table.fill() ─────────────
    xs_fill = np.ones(cloud_n, dtype=np.float64)
    ys_fill = np.zeros(cloud_n, dtype=np.float64)
    zs_fill = np.zeros(cloud_n, dtype=np.float64)

    def _fill_write():
        tbl_cloud.fill(x=xs_fill, y=ys_fill, z=zs_fill)

    r_fill_write = _make_result(
        "macro", "point_cloud_write_fill", f"N={cloud_n}",
        _timeit_ns(_fill_write, cfg.n_iters // 5, cfg.n_warmup),
        n_ops=cloud_n,
        note="Table.fill(): 3 column writes (1×SWIG+memcpy per field)"
    )
    _print_row(r_fill_write); results.append(r_fill_write)

    # ── Speedup callout ───────────────────────────────────────────────────────
    read_speedup = r_row_read.median_ns / r_col_read.median_ns if r_col_read.median_ns > 0 else float("inf")
    write_speedup = r_row_write.median_ns / r_col_write.median_ns if r_col_write.median_ns > 0 else float("inf")
    print(f"\n  >> Column read  vs row-wise read:  {read_speedup:.0f}× faster")
    print(f"  >> Column write vs row-wise write: {write_speedup:.0f}× faster\n")

    # ── Ref graph build: N triangles → 3N points ──────────────────────────────
    def _build_tri_graph():
        _build_triangle_db(tri_n)

    r = _make_result(
        "macro", "ref_graph_build", f"N={tri_n} tri",
        _timeit_ns(_build_tri_graph, max(2, macro_iters // 2), macro_warmup),
        n_ops=tri_n,
        note="push(Triangle)+3×push(BenchTriPt ref)+_combine per triangle"
    )
    _print_row(r); results.append(r)

    # ── Ref graph traversal: walk all triangle.a.x ────────────────────────────
    db_tri = _build_triangle_db(tri_n)
    tri_layer_idx = next(
        i for i in range(db_tri._origin.get_layer_count())
        if db_tri._origin.get_layer(i).name() == "BenchTriangle"
    )
    tri_layer = db_tri._origin.get_layer(tri_layer_idx)

    def _traverse_refs():
        total = 0.0
        for i in range(tri_n):
            tri = BenchTriangle.map_from(db_tri._origin, tri_layer.tryGetFeature(i))
            total += tri.a.x + tri.b.y + tri.c.z
        return total

    r = _make_result(
        "macro", "ref_graph_traverse", f"N={tri_n} tri",
        _timeit_ns(_traverse_refs, max(2, macro_iters), macro_warmup),
        n_ops=tri_n,
        note="per triangle: 3×(get_field_as_ref+tryGetFeature+map_from+get_field_as_float)"
    )
    _print_row(r); results.append(r)

    # ── Table iteration: for pt in table ─────────────────────────────────────
    db_iter = _build_truncated_point_db(iter_n)
    tbl_iter = db_iter[BenchPoint][BenchPoint]

    def _iter_table():
        for _pt in tbl_iter:
            pass

    r = _make_result(
        "macro", "iter_table (for pt in table)", f"N={iter_n}",
        _timeit_ns(_iter_table, macro_iters * 3, macro_warmup),
        n_ops=iter_n,
        note="__iter__: tryGetFeature(i) + map_from per step"
    )
    _print_row(r); results.append(r)

    def _iter_reuse():
        for _pt in tbl_iter.iter_reuse():
            pass

    r_reuse = _make_result(
        "macro", "iter_reuse (table.iter_reuse())", f"N={iter_n}",
        _timeit_ns(_iter_reuse, macro_iters * 3, macro_warmup),
        n_ops=iter_n,
        note="iter_reuse: reuse wrapper, update _origin+_cache per step (no Feature alloc)"
    )
    _print_row(r_reuse); results.append(r_reuse)

    return results


# ──────────────────────────────────────────────────────────────────────────────
# Section 4: FastSerializer Benchmarks
# ──────────────────────────────────────────────────────────────────────────────

def _ser_benchmark_case(
    name: str, fast_obj, root_type, pickle_obj,
    n_iters: int, n_warmup: int
) -> dict:
    gc.collect()
    fast_bytes  = FastSerializer.dumps(fast_obj)
    pickle_bytes = pickle.dumps(pickle_obj, protocol=pickle.HIGHEST_PROTOCOL)

    fast_dump = _timeit_ns(lambda: FastSerializer.dumps(fast_obj), n_iters, n_warmup)
    fast_load = _timeit_ns(lambda: FastSerializer.loads(fast_bytes, root_type), n_iters, n_warmup)
    pk_dump   = _timeit_ns(lambda: pickle.dumps(pickle_obj, protocol=pickle.HIGHEST_PROTOCOL), n_iters, n_warmup)
    pk_load   = _timeit_ns(lambda: pickle.loads(pickle_bytes), n_iters, n_warmup)

    def _mb(sz, ns): return sz / ns * 1e3 if ns > 0 else 0  # MB/s

    return {
        "name":          name,
        "fast_size":     len(fast_bytes),
        "pickle_size":   len(pickle_bytes),
        "fast_dump_p50": statistics.median(fast_dump) / 1000,    # µs
        "fast_load_p50": statistics.median(fast_load) / 1000,
        "pk_dump_p50":   statistics.median(pk_dump)   / 1000,
        "pk_load_p50":   statistics.median(pk_load)   / 1000,
        "fast_dump_mbs": _mb(len(fast_bytes), statistics.mean(fast_dump)),
        "fast_load_mbs": _mb(len(fast_bytes), statistics.mean(fast_load)),
    }


def _ser_worker(q: mp.Queue, case_fn_pickle, n_iters: int, n_warmup: int, **kwargs):
    try:
        fast_obj, root_type, pickle_obj, name = case_fn_pickle(**kwargs)
        result = _ser_benchmark_case(name, fast_obj, root_type, pickle_obj, n_iters, n_warmup)
        q.put({"ok": True, "result": result})
    except Exception as exc:
        q.put({"ok": False, "error": repr(exc), "tb": traceback.format_exc()})


def _run_isolated_ser(case_fn_pickle, n_iters: int, n_warmup: int, timeout: int = 60, **kwargs) -> Optional[dict]:
    q: mp.Queue = mp.Queue()
    p = mp.Process(target=_ser_worker, args=(q, case_fn_pickle, n_iters, n_warmup), kwargs=kwargs)
    p.start()
    p.join(timeout=timeout)
    if p.is_alive():
        p.terminate(); p.join()
        return None
    if p.exitcode != 0 or q.empty():
        return None
    msg = q.get()
    return msg.get("result") if msg.get("ok") else None


def _ser_case_numeric(n: int):
    ids    = list(range(n))
    values = [float(i) * 0.125 for i in range(n)]
    fast_obj   = BenchNL(ids=ids, values=values)
    pickle_obj = {"ids": ids, "values": values}
    return fast_obj, BenchNL, pickle_obj, f"numeric_list N={n}"


def _ser_case_mixed(n: int):
    pts  = [BenchPoint(x=float(i), y=float(i)*0.5, z=0.0) for i in range(n)]
    tags = [f"tag_{i}" for i in range(min(n, 20))]
    fast_obj   = BenchMixed(name="test", score=3.14, tags=tags)
    pickle_obj = {"name": "test", "score": 3.14, "tags": tags, "pts": [(p.x, p.y) for p in pts]}
    return fast_obj, BenchMixed, pickle_obj, f"mixed N={n}"


def _ser_case_chain(n: int):
    head = BenchNode(val=0)
    cur  = head
    for i in range(1, n):
        nxt = BenchNode(val=i)
        cur.next = nxt
        cur = nxt
    cur.next = head  # cyclic
    pickle_obj = list(range(n))  # placeholder
    return head, BenchNode, pickle_obj, f"cyclic_chain N={n}"


def _ser_case_point_cloud(n: int):
    pts  = [BenchPoint(x=float(i), y=float(i)*0.5, z=float(i)*0.25) for i in range(n)]
    fast_obj   = BenchPointCloud(points=pts)
    pickle_obj = [(p.x, p.y, p.z) for p in pts]  # type: ignore
    return fast_obj, BenchPointCloud, pickle_obj, f"point_cloud N={n}"


def _print_ser_header():
    w = 110
    hdr = (
        f"{'Case':<28} {'Fast size':>10} {'Pkl size':>10} "
        f"{'Dump Fast µs':>13} {'Dump Pkl µs':>12} "
        f"{'Load Fast µs':>13} {'Load Pkl µs':>12} "
        f"{'Dump MB/s':>10} {'Load MB/s':>10}"
    )
    print(hdr)
    print("-" * w)


def _print_ser_row(r: dict):
    if r is None:
        print(f"  (skipped – timeout or crash)")
        return
    print(
        f"{r['name']:<28} {r['fast_size']:>10} {r['pickle_size']:>10} "
        f"{r['fast_dump_p50']:>13.2f} {r['pk_dump_p50']:>12.2f} "
        f"{r['fast_load_p50']:>13.2f} {r['pk_load_p50']:>12.2f} "
        f"{r['fast_dump_mbs']:>10.1f} {r['fast_load_mbs']:>10.1f}"
    )


def run_serializer(cfg: BenchConfig) -> list[BenchResult]:
    _print_header("SECTION 4: FASTSERIALIZER BENCHMARKS  (vs pickle, µs)")
    _print_ser_header()

    results: list[BenchResult] = []
    ser_iters  = max(20, cfg.n_iters // 5)
    ser_warmup = max(5,  cfg.n_warmup // 3)

    sizes_numeric = [8, 64]       if cfg.quick else [8, 64, 512, 4096]
    sizes_cloud   = [4, 8]        if cfg.quick else [4, 8, 16]
    sizes_chain   = [4, 16]       if cfg.quick else [4, 16, 64]

    # Numeric list
    for n in sizes_numeric:
        r = _run_isolated_ser(_ser_case_numeric, ser_iters, ser_warmup, n=n)
        _print_ser_row(r)
        if r:
            results.append(BenchResult(
                section="serializer", name=r["name"], param="",
                n_ops=n, mean_ns=r["fast_dump_p50"]*1000,
                median_ns=r["fast_dump_p50"]*1000,
                p95_ns=0, p99_ns=0, min_ns=0, max_ns=0,
                throughput=f"{r['fast_dump_mbs']:.1f} MB/s",
                note="List[U32]+List[F64] → auxiliary columnar layer"
            ))

    # Point cloud (scalar Feature list)
    for n in sizes_cloud:
        r = _run_isolated_ser(_ser_case_point_cloud, ser_iters, ser_warmup, n=n)
        _print_ser_row(r)

    # Mixed
    for n in [4, 16] if cfg.quick else [4, 16, 64]:
        r = _run_isolated_ser(_ser_case_mixed, ser_iters, ser_warmup, n=n)
        _print_ser_row(r)

    # Cyclic chain (deep nesting)
    for n in sizes_chain:
        r = _run_isolated_ser(_ser_case_chain, ser_iters, ser_warmup, n=n)
        _print_ser_row(r)

    return results


# ──────────────────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────────────────

def _parse_args() -> BenchConfig:
    p = argparse.ArgumentParser(
        description="fastdb4py comprehensive performance benchmark",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("--quick",        action="store_true",
                   help="Use small sizes and fewer iterations (CI-friendly, <60s)")
    p.add_argument("--sections",     default="micro,meso,macro,serializer",
                   help="Comma-separated sections to run (micro meso macro serializer)")
    p.add_argument("--iters",        type=int, default=300,
                   help="Measurement iterations per benchmark (default: 300)")
    p.add_argument("--warmup",       type=int, default=30,
                   help="Warmup iterations (default: 30)")
    p.add_argument("--output-json",  default="",
                   help="Write full results as JSON to PATH")
    args = p.parse_args()
    cfg = BenchConfig(
        n_iters=args.iters,
        n_warmup=args.warmup,
        quick=args.quick,
        sections={s.strip() for s in args.sections.split(",")},
        output_json=args.output_json,
    )
    if cfg.quick:
        cfg.n_iters  = min(cfg.n_iters,  100)
        cfg.n_warmup = min(cfg.n_warmup,  10)
    return cfg


def main():
    import sys
    cfg = _parse_args()

    print("fastdb4py Comprehensive Benchmark")
    print(f"Python {sys.version.split()[0]} | platform: {platform.system()} "
          f"| iters={cfg.n_iters} warmup={cfg.n_warmup} quick={cfg.quick}")
    print(f"Sections: {', '.join(sorted(cfg.sections))}\n")

    all_results: list[BenchResult] = []

    if "micro" in cfg.sections:
        all_results.extend(run_micro(cfg))

    if "meso" in cfg.sections:
        all_results.extend(run_meso(cfg))

    if "macro" in cfg.sections:
        all_results.extend(run_macro(cfg))

    if "serializer" in cfg.sections:
        all_results.extend(run_serializer(cfg))

    # ── Summary ───────────────────────────────────────────────────────────────
    print(f"\n{'=' * 84}")
    print(f"  DONE  ({len(all_results)} benchmark rows)")
    print(f"{'=' * 84}")

    # ── JSON export ───────────────────────────────────────────────────────────
    if cfg.output_json:
        with open(cfg.output_json, "w") as f:
            json.dump([asdict(r) for r in all_results], f, indent=2)
        print(f"\nResults written to: {cfg.output_json}")


if __name__ == "__main__":
    main()
