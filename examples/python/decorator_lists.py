"""
Decorator-based ORM2: List Fields
===================================
Demonstrates numeric list fields (List[F64], etc.).

Numeric lists are stored as contiguous byte arrays and returned as numpy.ndarray.
Both Python lists and numpy arrays are accepted as input.
"""

from __future__ import annotations
from typing import List

import numpy as np
from fastdb4py import feature, ORM2, F64, U32, STR


@feature
class Polygon:
    name: STR
    xs: List[F64]     # numeric list — stored columnar
    ys: List[F64]


@feature
class ScoreBoard:
    title: STR
    scores: List[U32]  # integer list


if __name__ == '__main__':
    orm = ORM2.create()

    # Numeric list from Python list
    square = Polygon()
    square.name = "square"
    square.xs = [0.0, 1.0, 1.0, 0.0]
    square.ys = [0.0, 0.0, 1.0, 1.0]
    orm.push(square)

    # Numeric list from numpy array
    triangle = Polygon()
    triangle.name = "triangle"
    triangle.xs = np.array([0.0, 1.0, 0.5])
    triangle.ys = np.array([0.0, 0.0, 0.87])
    orm.push(triangle)

    # Integer list
    board = ScoreBoard()
    board.title = "level_1"
    board.scores = [100, 200, 300, 400]
    orm.push(board)

    orm.combine()

    # Read back — numeric lists come back as numpy arrays
    print("=== Numeric list fields ===")
    sq = orm.get(Polygon, 0, mode='copy')
    print(f"  {sq.name}: xs={sq.xs}, ys={sq.ys}")
    print(f"  xs type: {type(sq.xs).__name__}, dtype: {sq.xs.dtype}")

    tri = orm.get(Polygon, 1, mode='copy')
    print(f"  {tri.name}: xs={tri.xs}, ys={tri.ys}")

    b = orm.get(ScoreBoard, 0, mode='copy')
    print(f"\n  {b.title}: scores={b.scores}")

    print("\n✓ List fields example complete.")
