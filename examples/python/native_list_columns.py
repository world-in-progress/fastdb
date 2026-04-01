"""
Native list columns example
============================
Demonstrates List[F64] and List[Feature] as first-class ORM column types.
No FastSerializer needed — data lives directly in shared-memory ORM layers,
and field access returns zero-copy NumPy arrays or lazy FeatureRefList proxies.

Topics covered:
  1. Numeric list columns  (List[F64])
  2. Ref list columns      (List[Point])
  3. Self-referential refs (List['Node'] — tree / graph structures)
  4. Cross-process zero-copy read via ORM.share / ORM.load
"""

from __future__ import annotations
from multiprocessing import Process
from typing import List

import numpy as np
import fastdb4py
from fastdb4py import Feature, F64, U32, STR, ORM

# ---------------------------------------------------------------------------
# 1. Feature definitions
# ---------------------------------------------------------------------------

class Point(Feature):
    x: F64
    y: F64

class Polygon(Feature):
    name: STR
    vertex_count: U32
    # Numeric list: zero-copy NumPy array backed by C++ memory
    xs: List[F64]
    ys: List[F64]
    # Ref list: lazy FeatureRefList — each element resolved on demand
    vertices: List[Point]

class Node(Feature):
    """Binary tree node — self-referential list."""
    value: F64
    children: List['Node']


# ---------------------------------------------------------------------------
# 2. Helpers
# ---------------------------------------------------------------------------

def make_polygon(name: str, coords: list[tuple[float, float]]) -> Polygon:
    p = Polygon()
    p.name = name
    p.vertex_count = len(coords)
    p.xs = [c[0] for c in coords]
    p.ys = [c[1] for c in coords]
    p.vertices = [Point(x=c[0], y=c[1]) for c in coords]
    return p


def make_tree(values: list) -> Node:
    """Build a simple flat list tree (root → children)."""
    nodes = [Node(value=v) for v in values]
    root = nodes[0]
    root.children = nodes[1:]
    for child in nodes[1:]:
        child.children = []
    return root


# ---------------------------------------------------------------------------
# 3. Numeric list columns demo
# ---------------------------------------------------------------------------

def demo_numeric_lists():
    print("=== Numeric list columns ===")
    orm = ORM.create()

    coords = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    square = make_polygon("square", coords)
    orm.push(square, feature_name="square")

    orm.share("example_native_list")
    orm2 = ORM.load("example_native_list")

    sq = orm2.get(Polygon, "square")

    # xs / ys are zero-copy NumPy arrays
    xs: np.ndarray = sq.xs
    ys: np.ndarray = sq.ys
    print(f"  name         : {sq.name}")
    print(f"  vertex_count : {sq.vertex_count}")
    print(f"  xs (numpy)   : {xs}  dtype={xs.dtype}")
    print(f"  ys (numpy)   : {ys}  dtype={ys.dtype}")
    assert list(xs) == [c[0] for c in coords]
    assert list(ys) == [c[1] for c in coords]

    orm2.unlink()

    print("  ✓ numeric list round-trip OK\n")


# ---------------------------------------------------------------------------
# 4. Ref list columns demo
# ---------------------------------------------------------------------------

def demo_ref_lists():
    print("=== Ref list columns ===")
    orm = ORM.create()

    coords = [(0.0, 0.0), (3.0, 0.0), (1.5, 2.5)]
    triangle = make_polygon("triangle", coords)
    orm.push(triangle, feature_name="tri")

    orm.share("example_ref_list")
    orm2 = ORM.load("example_ref_list")

    tri = orm2.get(Polygon, "tri")

    # vertices is a FeatureRefList — each element is a Point resolved lazily
    verts = tri.vertices
    print(f"  vertex list length : {len(verts)}")
    for i, pt in enumerate(verts):
        print(f"  vertex[{i}] : x={pt.x:.1f}  y={pt.y:.1f}")
        assert (pt.x, pt.y) == coords[i]

    orm2.unlink()
    print("  ✓ ref list round-trip OK\n")


# ---------------------------------------------------------------------------
# 5. Self-referential (tree) demo
# ---------------------------------------------------------------------------

def demo_tree():
    print("=== Self-referential list (tree) ===")
    orm = ORM.create()

    root = make_tree([10.0, 20.0, 30.0, 40.0])
    orm.push(root, feature_name="root")

    orm.share("example_tree")
    orm2 = ORM.load("example_tree")

    r = orm2.get(Node, "root")
    print(f"  root.value : {r.value}")
    children = r.children  # FeatureRefList[Node]
    print(f"  children count : {len(children)}")
    for i, child in enumerate(children):
        print(f"  child[{i}].value = {child.value}")
        assert child.value == [20.0, 30.0, 40.0][i]

    orm2.unlink()
    print("  ✓ self-referential list round-trip OK\n")


# ---------------------------------------------------------------------------
# 6. Cross-process zero-copy demo
# ---------------------------------------------------------------------------

def _reader_process(shm_name: str):
    orm = ORM.load(shm_name)
    sq = orm.get(Polygon, "square")
    xs = sq.xs
    print(f"  [child] xs from shared memory: {list(xs)}")
    # No copy — array is backed by the shared memory segment
    orm.unlink()


def demo_cross_process():
    print("=== Cross-process zero-copy read ===")
    orm = ORM.create()
    coords = [(float(i), float(i * 2)) for i in range(5)]
    poly = make_polygon("penta", coords)
    orm.push(poly, feature_name="square")

    orm.share("example_cross_proc", close_after=True)

    p = Process(target=_reader_process, args=("example_cross_proc",))
    p.start()
    p.join()
    print("  ✓ cross-process zero-copy OK\n")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    demo_numeric_lists()
    demo_ref_lists()
    demo_tree()
    demo_cross_process()
    print("All demos passed.")
