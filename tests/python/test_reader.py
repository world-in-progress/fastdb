# tests/python/test_reader.py
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