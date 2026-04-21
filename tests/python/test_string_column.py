import secrets

import numpy as np

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
    loaded = ColumnEngine.load(shm_name)
    try:
        assert loaded.table(CEStringPoint).column.name.to_pylist() == ["alpha", "beta"]
    finally:
        loaded.unlink()


def test_table_fill_rejects_string_field_keyword():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 1)])
    tbl = engine.table(CEStringPoint)
    try:
        tbl.fill(name=np.array(["bad"], dtype=object))
    except Exception as exc:
        assert "StringColumn.fill" in str(exc)
    else:
        raise AssertionError("expected string fill rejection")
