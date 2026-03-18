from __future__ import annotations

import struct
import sys
from pathlib import Path
from typing import List

from fastdb4py import F64, I32, U32, FastSerializer, Feature


TMP_DIR = Path(__file__).resolve().parent / ".tmp"
PY_TO_TS_PATH = TMP_DIR / "python-to-ts.bin"
TS_TO_PY_PATH = TMP_DIR / "ts-to-python.bin"


class Point(Feature):
    x: F64
    y: F64


class Line(Feature):
    points: List[Point]
    id: I32


class RecursiveNode(Feature):
    val: I32
    next: "RecursiveNode"


class NumericColumnarLists(Feature):
    ids: List[U32]
    values: List[F64]


def write_python_fixture() -> None:
    TMP_DIR.mkdir(parents=True, exist_ok=True)
    obj = Line(id=42, points=[Point(x=1.5, y=2.5), Point(x=3.5, y=4.5)])
    PY_TO_TS_PATH.write_bytes(FastSerializer.dumps(obj))
    print(f"wrote {PY_TO_TS_PATH}")


def verify_ts_fixture() -> None:
    blob = TS_TO_PY_PATH.read_bytes()
    cycle_len = struct.unpack_from("<I", blob, 0)[0]
    cycle_bytes = blob[4 : 4 + cycle_len]
    numeric_bytes = blob[4 + cycle_len :]

    node = FastSerializer.loads(cycle_bytes, RecursiveNode)
    assert node.val == 7
    assert node.next.val == 8
    assert node.next.next is node

    numeric = FastSerializer.loads(numeric_bytes, NumericColumnarLists)
    assert numeric.ids == [1, 2, 4294967295]
    assert len(numeric.values) == 3
    assert abs(numeric.values[1] + 1.25) < 1e-12
    assert abs(numeric.values[2] - 9.75) < 1e-12

    print(f"verified {TS_TO_PY_PATH}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: serializer_interop.py [write-python-fixture|verify-ts-fixture]", file=sys.stderr)
        return 2

    command = sys.argv[1]
    if command == "write-python-fixture":
        write_python_fixture()
        return 0
    if command == "verify-ts-fixture":
        verify_ts_fixture()
        return 0

    print(f"unknown command: {command}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
