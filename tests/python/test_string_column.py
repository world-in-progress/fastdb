import secrets

import numpy as np
import pytest

from fastdb4py import ColumnEngine, Layout, feature, F64, U32, STR, pack_utf8_column
import fastdb4py.string_column as string_column_mod


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


def test_string_column_fill_coerces_none_to_empty_string():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(
        row_id=np.array([1, 2], dtype=np.uint32),
        x=np.array([1.0, 2.0], dtype=np.float64),
    )
    tbl.column.name.fill(["hi", None])
    assert tbl.column.name.to_pylist() == ["hi", ""]


def test_string_column_fill_validates_payload_once(monkeypatch):
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    column = tbl.column.name
    validate_calls = []
    original = column._validate_utf8_payload

    def capture_validate(offsets, data, expected_len):
        validate_calls.append(expected_len)
        return original(offsets, data, expected_len)

    monkeypatch.setattr(column, '_validate_utf8_payload', capture_validate)

    column.fill(["hi", "中"])

    assert validate_calls == [2]
    assert column.to_pylist() == ["hi", "中"]


def test_string_column_fill_uses_shared_utf8_packer(monkeypatch):
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    calls = []

    def fake_pack(values):
        calls.append(tuple(values))
        return _pack_utf8(["hi", ""])

    monkeypatch.setattr(string_column_mod, '_pack_utf8_values', fake_pack, raising=False)

    tbl.column.name.fill(["hi", None])

    assert calls == [("hi", None)]
    assert tbl.column.name.to_pylist() == ["hi", ""]


def test_string_column_fill_utf8_uses_unified_fixed_writer(monkeypatch):
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    captured = []

    original = engine._fill_fixed_table

    def capture_fill(table_name, writes):
        captured.append((table_name, writes))
        return original(table_name, writes)

    monkeypatch.setattr(engine, '_fill_fixed_table', capture_fill)

    offsets, data = _pack_utf8(["hi", "中"])
    tbl.column.name.fill_utf8(offsets, data)

    assert len(captured) == 1
    table_name, writes = captured[0]
    assert table_name == CEStringPoint.__name__
    assert list(writes) == ["name"]
    written_offsets, written_data = writes["name"]
    np.testing.assert_array_equal(written_offsets, offsets)
    np.testing.assert_array_equal(written_data, data)
    assert tbl.column.name.to_pylist() == ["hi", "中"]


def test_loaded_string_column_fill_utf8_rejects_read_only_table():
    shm_name = f"fastdb_str_fill_{secrets.token_hex(4)}"
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(
        row_id=np.array([1, 2], dtype=np.uint32),
        x=np.array([1.0, 2.0], dtype=np.float64),
        name=["aa", "bb"],
    )
    offsets, data = _pack_utf8(["x", "y"])

    loaded = None
    try:
        engine.share(shm_name)
        loaded = ColumnEngine.load(shm_name)
        with pytest.raises(RuntimeError, match='read-only'):
            loaded.table(CEStringPoint).column.name.fill_utf8(offsets, data)
    finally:
        if loaded is not None:
            loaded.unlink()
            engine.close()
        else:
            engine.unlink()


def test_pack_utf8_column_outputs_expected_dtypes_and_offsets():
    offsets, data = pack_utf8_column(['', 'a', '中'])
    assert offsets.dtype == np.uint32
    assert data.dtype == np.uint8
    np.testing.assert_array_equal(offsets, np.array([0, 0, 1, 4], dtype=np.uint32))
    np.testing.assert_array_equal(data, np.frombuffer('a中'.encode('utf-8'), dtype=np.uint8))


def test_pack_utf8_column_and_fill_utf8_round_trip():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 3)])
    tbl = engine.table(CEStringPoint)
    tbl.fill(row_id=np.array([1,2,3], dtype=np.uint32), x=np.array([1.0,2.0,3.0], dtype=np.float64))
    offsets, data = pack_utf8_column(['', 'bb', '中'])
    tbl.column.name.fill_utf8(offsets, data)
    assert tbl.column.name.to_pylist() == ['', 'bb', '中']


def test_pack_utf8_column_coerces_none_to_empty_string():
    offsets, data = pack_utf8_column([None, 'x'])
    np.testing.assert_array_equal(offsets, np.array([0, 0, 1], dtype=np.uint32))
    np.testing.assert_array_equal(data, np.frombuffer('x'.encode('utf-8'), dtype=np.uint8))
import fastdb4py.core as core

def test_native_string_column_sequence_setter_round_trips():
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 3)])
    table_name = CEStringPoint.__name__
    layer_build = engine._fixed_layer_builds[table_name]
    field_index = engine._fixed_table_fields[table_name]["name"]

    layer_build.set_string_column_from_sequence(field_index, ["a", "be", "中"])
    engine._publish_fixed_snapshot()

    assert engine.table(CEStringPoint).column.name.to_pylist() == ["a", "be", "中"]


def test_table_fill_routes_strings_to_native_sequence_setter(monkeypatch):
    engine = ColumnEngine.truncate([Layout(CEStringPoint, 2)])
    tbl = engine.table(CEStringPoint)
    table_name = CEStringPoint.__name__
    field_index = engine._fixed_table_fields[table_name]["name"]
    calls = []

    def fake_setter(self, got_field_index, values):
        calls.append((got_field_index, list(values)))
        offsets, data = _pack_utf8(["hi", ""])
        self.set_string_column_bulk(got_field_index, offsets, data)

    monkeypatch.setattr(
        core.WxLayerTableBuild,
        "set_string_column_from_sequence",
        fake_setter,
        raising=False,
    )

    tbl.fill(
        row_id=np.array([1, 2], dtype=np.uint32),
        x=np.array([1.0, 2.0], dtype=np.float64),
        name=["hi", None],
    )

    assert calls == [(field_index, ["hi", None])]
    assert tbl.column.name.to_pylist() == ["hi", ""]
