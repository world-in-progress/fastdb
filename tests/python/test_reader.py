# tests/python/test_reader.py
import ctypes
import struct

import pytest
import numpy as np
from fastdb4py.decorator import feature
from fastdb4py.registry import get_schema
from fastdb4py.push import push_feature
from fastdb4py.reader import map_feature, copy_feature
from fastdb4py.type import F64, U32, STR, BYTES
from fastdb4py import core


@feature
class ReadPoint:
    x: F64
    y: F64
    label: STR


def _build_db_with_points():
    """Build a small DB with 3 ReadPoint features, return the read-only db."""
    schema = get_schema(ReadPoint)
    db = core.WxDatabaseBuild()
    db.begin("")
    t = db.create_layer_begin(schema.layer_name)
    t.set_geometry_type(core.gtPoint, core.cfTx32, aabboxEnabled=True)
    t.set_extent(-180, -90, 180, 90)
    for fd in schema.fields:
        t.add_field(fd.name, fd.cpp_type)

    for i in range(3):
        p = ReadPoint()
        p.x = float(i)
        p.y = float(i * 10)
        p.label = f"pt{i}"
        push_feature(p, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    return rdb


def _pack_utf8(strings):
    raw = bytearray()
    offsets = [0]
    for value in strings:
        raw.extend(value.encode("utf-8"))
        offsets.append(len(raw))
    return np.array(offsets, dtype=np.uint32), np.frombuffer(bytes(raw), dtype=np.uint8)


class _LayerHeader(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 64),
        ("feature_count", ctypes.c_uint32),
        ("geometry_type", ctypes.c_uint16),
        ("field_count", ctypes.c_uint16),
        ("coord_format", ctypes.c_uint16),
        ("aabbox_enable", ctypes.c_bool),
        ("string_table_u32", ctypes.c_bool),
        ("n_list_fields", ctypes.c_uint16),
        ("minx", ctypes.c_double),
        ("miny", ctypes.c_double),
        ("maxx", ctypes.c_double),
        ("maxy", ctypes.c_double),
        ("offset_table", ctypes.c_uint64),
        ("offset_strings", ctypes.c_uint64),
        ("offset_wstrings", ctypes.c_uint64),
        ("total_size", ctypes.c_uint64),
    ]


class _FieldDesc(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 16),
        ("type", ctypes.c_uint16),
        ("element_type", ctypes.c_uint16),
        ("vmin", ctypes.c_double),
        ("vmax", ctypes.c_double),
        ("size", ctypes.c_uint64),
        ("offset", ctypes.c_uint64),
    ]


def _build_varlen_string_db_bytes(strings):
    db = core.WxDatabaseBuild()
    db.begin("")
    layer_build = db.create_layer_begin("utf8_rows")
    layer_build.set_geometry_type(core.gtNone, core.cfTx32, aabboxEnabled=False)
    layer_build.add_field("name", core.ftSTR)
    db.truncate("utf8_rows", len(strings))

    offsets, data = _pack_utf8(strings)
    layer_build.set_string_column_bulk(0, offsets, data)

    mem = core.WxMemoryStream()
    db.post(mem)
    return mem.data().as_array(np.uint8).tobytes(), offsets, data


def _corrupt_first_varlen_string_byte_count(buf, byte_count):
    mutated = bytearray(buf)
    layer_offset = 16 + 4
    header = _LayerHeader.from_buffer_copy(mutated[layer_offset:layer_offset + ctypes.sizeof(_LayerHeader)])
    data_offset = layer_offset + ctypes.sizeof(_LayerHeader) + header.field_count * ctypes.sizeof(_FieldDesc)
    wstring_count_offset = data_offset + header.offset_wstrings
    assert struct.unpack_from("=I", mutated, wstring_count_offset)[0] == 0
    string_section_offset = wstring_count_offset + 4
    struct.pack_into("=Q", mutated, string_section_offset + 12, byte_count)
    return bytes(mutated)


class TestMapFeature:
    def test_read_scalar(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = map_feature(ReadPoint, layer, 1)
        assert abs(obj.x - 1.0) < 1e-9
        assert abs(obj.y - 10.0) < 1e-9

    def test_read_string(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = map_feature(ReadPoint, layer, 0)
        assert obj.label == "pt0"

    def test_map_is_readonly(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = map_feature(ReadPoint, layer, 0)
        with pytest.raises(AttributeError, match="read-only"):
            obj.x = 999.0


class TestCopyFeature:
    def test_read_scalar(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = copy_feature(ReadPoint, layer, 1)
        assert abs(obj.x - 1.0) < 1e-9
        assert abs(obj.y - 10.0) < 1e-9

    def test_read_string(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = copy_feature(ReadPoint, layer, 2)
        assert obj.label == "pt2"

    def test_copy_is_detached(self):
        """After copy, the object is independent of the DB."""
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = copy_feature(ReadPoint, layer, 0)
        obj.x = 999.0  # should work — it's a normal Python object
        assert obj.x == 999.0

    def test_copy_has_correct_type(self):
        db = _build_db_with_points()
        layer = db.get_layer(0)
        obj = copy_feature(ReadPoint, layer, 0)
        assert isinstance(obj, ReadPoint)

    def test_copy_feature_detaches_bytes(self):
        """copy_feature should return a detached bytes copy, not a memoryview."""
        from fastdb4py.registry import get_schema
        from fastdb4py.push import push_feature

        @feature
        class WithBytes:
            data: BYTES

        schema = get_schema(WithBytes)
        db = core.WxDatabaseBuild()
        db.begin("")
        t = db.create_layer_begin(schema.layer_name)
        t.set_geometry_type(core.gtAny, core.cfTx32, aabboxEnabled=False)
        t.set_extent(-180, -90, 180, 90)
        for fd in schema.fields:
            t.add_field(fd.name, fd.cpp_type)
        push_feature(WithBytes(data=b"hello"), t, schema)

        mem = core.WxMemoryStream()
        db.post(mem)
        buf = mem.data().as_array(np.uint8).tobytes()
        rdb = core.WxDatabase.load_xbuffer(buf)
        rdb._buffer = buf

        layer = rdb.get_layer(0)
        obj = copy_feature(WithBytes, layer, 0)
        assert isinstance(obj.data, bytes)
        assert obj.data == b"hello"


def test_varlen_string_column_reader_exposes_raw_buffers():
    buf, offsets, data = _build_varlen_string_db_bytes(["a", "bé", "中"])
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf

    layer = rdb.get_layer(0)
    np.testing.assert_array_equal(
        layer.get_string_column_offsets(0).as_array(np.uint32),
        offsets,
    )
    np.testing.assert_array_equal(
        layer.get_string_column_data(0).as_array(np.uint8),
        data,
    )

    feature = layer.tryGetFeature(2)
    assert feature.get_field_as_string_view(0).to_bytes().decode("utf-8") == "中"


def test_varlen_string_column_reader_rejects_truncated_section_payload():
    valid_buf, _, data = _build_varlen_string_db_bytes(["a", "bé", "中"])
    corrupted_buf = _corrupt_first_varlen_string_byte_count(valid_buf, data.nbytes + 8)

    rdb = core.WxDatabase.load_xbuffer(corrupted_buf)
    rdb._buffer = corrupted_buf

    layer = rdb.get_layer(0)
    assert layer.get_string_column_offsets(0).size == 0
    assert layer.get_string_column_data(0).size == 0
    assert layer.tryGetFeature(0).get_field_as_string_view(0).size == 0


def test_truncate_layer_build_set_numeric_column_bulk_round_trips():
    db = core.WxDatabaseBuild()
    db.begin("")
    layer = db.create_layer_begin("num_rows")
    layer.set_geometry_type(core.gtNone, core.cfTx32, aabboxEnabled=False)
    layer.add_field("x", core.ftF64)
    db.truncate("num_rows", 3)

    layer.set_numeric_column_bulk(0, np.array([1.0, 2.5, 3.5], dtype=np.float64))

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    out = rdb.get_layer(0).get_column(0).as_nparray()
    np.testing.assert_allclose(out, np.array([1.0, 2.5, 3.5], dtype=np.float64))
