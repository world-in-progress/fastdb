import secrets

import numpy as np
import pytest

from fastdb4py import ColumnEngine, Layout, feature, F64, U32, STR


@feature
class CEStringPoint:
    row_id: U32
    x: F64
    name: STR


def _pack_utf8(strings: list[str]):
    raw = bytearray()
    offsets = [0]
    for s in strings:
        raw.extend(s.encode("utf-8"))
        offsets.append(len(raw))
    return np.array(offsets, dtype=np.uint32), np.frombuffer(bytes(raw), dtype=np.uint8)


def test_column_engine_truncate_supports_str_fill_utf8():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 3)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(
        row_id=np.array([1, 2, 3], dtype=np.uint32),
        x=np.array([1.0, 2.0, 3.0], dtype=np.float64),
    )
    offsets, data = _pack_utf8(["a", "be", "中"])
    tbl.column.name.fill_utf8(offsets, data)
    assert tbl.column.name.get(0) == "a"
    assert tbl.column.name.get(2) == "中"
    assert tbl[1].name == "be"


def test_column_engine_dynamic_push_still_reads_strings():
    engine = ColumnEngine.create()
    engine.push(CEStringPoint(row_id=1, x=1.5, name="legacy"))
    engine.combine()
    assert engine.table(CEStringPoint)[0].name == "legacy"


def test_column_engine_share_load_keeps_string_column():
    shm_name = f"fastdb_str_{secrets.token_hex(4)}"
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(
        row_id=np.array([10, 11], dtype=np.uint32),
        x=np.array([3.0, 4.0], dtype=np.float64),
    )
    offsets, data = _pack_utf8(["alpha", "beta"])
    tbl.column.name.fill_utf8(offsets, data)
    engine.share(shm_name)
    loaded = None
    try:
        loaded = ColumnEngine.load(shm_name)
        assert loaded.table(CEStringPoint).column.name.to_pylist() == ["alpha", "beta"]
    finally:
        if loaded is not None:
            loaded.unlink()
            engine.close()
        else:
            engine.unlink()


def test_table_fill_accepts_string_field_keyword():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 1)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(name=["bad"])
    assert tbl.column.name.to_pylist() == ["bad"]


def test_string_column_fill_utf8_uses_unified_fixed_writer(monkeypatch):
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)

    monkeypatch.setattr(
        engine,
        '_rewrite_string_column',
        lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError('old path called')),
    )

    offsets, data = _pack_utf8(["hi", "中"])
    tbl.column.name.fill_utf8(offsets, data)

    assert tbl.column.name.to_pylist() == ["hi", "中"]
