import numpy as np

from fastdb4py import pack_utf8_column, ColumnEngine, Layout, feature, F64, U32, STR


@feature
class CEStringPoint:
    row_id: U32
    x: F64
    name: STR


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
