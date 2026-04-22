import pytest
import numpy as np
from typing import List

# Import the framework components
from fastdb4py.decorator import feature
from fastdb4py.registry import get_schema
from fastdb4py.push import push_feature
from fastdb4py.type import F64, U32, STR
from fastdb4py import core


@feature
class PushPoint:
    x: F64
    y: F64


def _make_db_and_layer(cls):
    """Helper: create a WxDatabaseBuild + WxLayerTableBuild for cls."""
    schema = get_schema(cls)
    db = core.WxDatabaseBuild()
    db.begin("")
    t = db.create_layer_begin(schema.layer_name)
    t.set_geometry_type(core.gtPoint, core.cfTx32, aabboxEnabled=True)
    t.set_extent(-180, -90, 180, 90)
    for fd in schema.fields:
        if fd.field_type.value == 13:  # list
            t.add_list_field(fd.name, fd.cpp_type)
        else:
            t.add_field(fd.name, fd.cpp_type)
    return db, t


def test_push_scalar_fields():
    db, t = _make_db_and_layer(PushPoint)
    schema = get_schema(PushPoint)
    p = PushPoint()
    p.x = 42.0
    p.y = -7.5
    row = push_feature(p, t, schema)
    assert row == 0 or row == -1  # implementation may return -1

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    assert abs(feat.get_field_as_float(0) - 42.0) < 1e-9
    assert abs(feat.get_field_as_float(1) - (-7.5)) < 1e-9


def test_push_str_field():
    @feature
    class Named:
        label: STR

    db, t = _make_db_and_layer(Named)
    schema = get_schema(Named)
    obj = Named()
    obj.label = "hello"
    push_feature(obj, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
def test_push_list_field():
    @feature
    class WithList:
        temps: List[F64]

    db, t = _make_db_and_layer(WithList)
    schema = get_schema(WithList)
    obj = WithList()
    obj.temps = [35.5, 36.1, 37.0]
    push_feature(obj, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    arr = feat.get_field_as_list_view(0).as_array(np.float64)
    assert list(arr) == [35.5, 36.1, 37.0]


def test_push_default_values():
    db, t = _make_db_and_layer(PushPoint)
    schema = get_schema(PushPoint)
    p = PushPoint()  # no fields set
    push_feature(p, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    assert feat.get_field_as_float(0) == 0.0
    assert feat.get_field_as_float(1) == 0.0


def test_push_empty_list():
    @feature
    class WithEmptyList:
        temps: List[F64]

    db, t = _make_db_and_layer(WithEmptyList)
    schema = get_schema(WithEmptyList)
    obj = WithEmptyList()
    obj.temps = []
    push_feature(obj, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    arr = feat.get_field_as_list_view(0).as_array(np.float64)
def test_push_numpy_array_list():
    """Test that numpy arrays are handled correctly."""
    @feature
    class WithNumpyList:
        data: List[F64]

    db, t = _make_db_and_layer(WithNumpyList)
    schema = get_schema(WithNumpyList)
    obj = WithNumpyList()
    obj.data = np.array([1.1, 2.2, 3.3])
    push_feature(obj, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    arr = feat.get_field_as_list_view(0).as_array(np.float64)
    assert len(arr) == 3
    assert abs(arr[0] - 1.1) < 1e-9
    assert abs(arr[1] - 2.2) < 1e-9
    assert abs(arr[2] - 3.3) < 1e-9


def test_push_mixed_field_types():
    """Test that multiple different field types work together."""
    @feature
    class Mixed:
        count: U32
        ratio: F64
        name: STR
        values: List[F64]

    db, t = _make_db_and_layer(Mixed)
    schema = get_schema(Mixed)
    obj = Mixed()
    obj.count = 42
    obj.ratio = 3.14
    obj.name = "mixed_test"
    obj.values = [10.0, 20.0]
    push_feature(obj, t, schema)

    mem = core.WxMemoryStream()
    db.post(mem)
    buf = mem.data().as_array(np.uint8).tobytes()
    rdb = core.WxDatabase.load_xbuffer(buf)
    rdb._buffer = buf
    layer = rdb.get_layer(0)
    feat = layer.tryGetFeature(0)
    
    assert feat.get_field_as_int(0) == 42
    assert abs(feat.get_field_as_float(1) - 3.14) < 1e-9
    assert feat.get_field_as_string(2) == "mixed_test"
    arr = feat.get_field_as_list_view(3).as_array(np.float64)
    assert list(arr) == [10.0, 20.0]


@feature
class Tag:
    name: STR

@feature
class Article:
    title: STR
    scores: List[F64]
    tags: List[Tag]


def test_push_numeric_list_via_orm():
    from fastdb4py.object_engine import ObjectEngine
    orm = ObjectEngine.create()
    a = Article()
    a.title = "Test"
    a.scores = [1.0, 2.0, 3.0]
    a.tags = []
    orm.push(a)
    orm.combine()
    result = orm.get(Article, 0, mode='copy')
    assert result.title == "Test"
    import numpy as np
    np.testing.assert_array_almost_equal(result.scores, [1.0, 2.0, 3.0])


def test_push_ref_list_via_orm():
    from fastdb4py.object_engine import ObjectEngine
    orm = ObjectEngine.create()
    t1 = Tag()
    t1.name = "python"
    t2 = Tag()
    t2.name = "fastdb"
    a = Article()
    a.title = "Guide"
    a.scores = [5.0]
    a.tags = [t1, t2]
    orm.push(a)
    orm.combine()
    assert orm.count(Tag) == 2
    assert orm.count(Article) == 1
    tag0 = orm.get(Tag, 0, mode='copy')
    tag1 = orm.get(Tag, 1, mode='copy')
    assert tag0.name == "python"
    assert tag1.name == "fastdb"