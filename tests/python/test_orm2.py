# tests/python/test_orm2.py
import pytest
from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, U32, STR


@feature
class O2Point:
    x: F64
    y: F64
    label: STR


class TestORM2Basic:
    def test_create_and_push(self):
        orm = ORM2.create()
        p = O2Point()
        p.x = 1.0
        p.y = 2.0
        p.label = "first"
        orm.push(p)
        assert orm.count(O2Point) == 1

    def test_push_multiple(self):
        orm = ORM2.create()
        for i in range(5):
            p = O2Point()
            p.x = float(i)
            p.y = float(i * 2)
            p.label = f"p{i}"
            orm.push(p)
        assert orm.count(O2Point) == 5

    def test_combine_and_read_copy(self):
        orm = ORM2.create()
        p = O2Point()
        p.x = 42.0
        p.y = -7.5
        p.label = "test"
        orm.push(p)
        orm.combine()

        result = orm.get(O2Point, 0, mode='copy')
        assert abs(result.x - 42.0) < 1e-9
        assert abs(result.y - (-7.5)) < 1e-9
        assert result.label == "test"
        assert isinstance(result, O2Point)

    def test_combine_and_read_map(self):
        orm = ORM2.create()
        p = O2Point()
        p.x = 3.14
        p.y = 2.71
        p.label = "pi"
        orm.push(p)
        orm.combine()

        mapped = orm.get(O2Point, 0, mode='map')
        assert abs(mapped.x - 3.14) < 1e-9
        assert mapped.label == "pi"
        # map is read-only
        with pytest.raises(AttributeError, match="read-only"):
            mapped.x = 0.0

    def test_multiple_types(self):
        @feature
        class Color:
            r: U32
            g: U32
            b: U32

        orm = ORM2.create()
        p = O2Point()
        p.x = 1.0
        p.y = 2.0
        p.label = "pt"
        orm.push(p)

        c = Color()
        c.r = 255
        c.g = 128
        c.b = 0
        orm.push(c)

        orm.combine()
        assert orm.count(O2Point) == 1
        assert orm.count(Color) == 1

        pt = orm.get(O2Point, 0, mode='copy')
        assert abs(pt.x - 1.0) < 1e-9

        color = orm.get(Color, 0, mode='copy')
        assert color.r == 255

    def test_iter_features(self):
        orm = ORM2.create()
        for i in range(3):
            p = O2Point()
            p.x = float(i)
            p.y = 0.0
            p.label = f"p{i}"
            orm.push(p)
        orm.combine()

        results = list(orm.iter(O2Point, mode='copy'))
        assert len(results) == 3
        assert all(isinstance(r, O2Point) for r in results)
        assert [r.x for r in results] == [0.0, 1.0, 2.0]


from fastdb4py.type import F64, U32, STR
from typing import List

class TestORM2PreCombineCount:
    def test_count_before_combine(self):
        orm = ORM2.create()
        assert orm.count(O2Point) == 0
        p = O2Point()
        p.x = 1.0; p.y = 2.0; p.label = "a"
        orm.push(p)
        assert orm.count(O2Point) == 1
        p2 = O2Point()
        p2.x = 3.0; p2.y = 4.0; p2.label = "b"
        orm.push(p2)
        assert orm.count(O2Point) == 2
        orm.combine()
        assert orm.count(O2Point) == 2

    def test_count_dedup(self):
        orm = ORM2.create()
        p = O2Point()
        p.x = 1.0; p.y = 2.0; p.label = "a"
        orm.push(p)
        orm.push(p)  # duplicate
        assert orm.count(O2Point) == 1


@feature
class Vendor:
    name: STR

@feature
class Device:
    model: STR
    vendor: Vendor


class TestORM2Refs:
    def test_push_with_ref(self):
        orm = ORM2.create()
        v = Vendor()
        v.name = "Acme"
        d = Device()
        d.model = "Widget"
        d.vendor = v
        orm.push(d)
        assert orm.count(Vendor) == 1
        assert orm.count(Device) == 1

    def test_push_shared_ref(self):
        """Two devices sharing the same vendor should push vendor once."""
        orm = ORM2.create()
        v = Vendor()
        v.name = "Shared"
        d1 = Device()
        d1.model = "A"
        d1.vendor = v
        d2 = Device()
        d2.model = "B"
        d2.vendor = v
        orm.push(d1)
        orm.push(d2)
        assert orm.count(Vendor) == 1
        assert orm.count(Device) == 2

    def test_ref_readback(self):
        orm = ORM2.create()
        v = Vendor()
        v.name = "TestCo"
        d = Device()
        d.model = "X100"
        d.vendor = v
        orm.push(d)
        orm.combine()
        device = orm.get(Device, 0, mode='copy')
        assert device.model == "X100"
        vendor = orm.get(Vendor, 0, mode='copy')
        assert vendor.name == "TestCo"


# ---------------------------------------------------------------------------
# LayerSchema push-plan tests
# ---------------------------------------------------------------------------
import numpy as np
from fastdb4py.registry import get_schema
from fastdb4py.type import BYTES, WSTR, I32, F32, U8, U16, U8N, U16N, OriginFieldType


@feature
class PlanTestItem:
    x: F64
    y: U32
    label: STR
    data: BYTES


@feature
class PlanTestWide:
    name: WSTR
    score: F64


@feature
class PlanTestAllNumeric:
    a_u8: U8
    b_u16: U16
    c_u32: U32
    d_i32: I32
    e_f32: F32
    f_f64: F64
    g_u8n: U8N
    h_u16n: U16N


@feature
class PlanTag:
    value: STR


@feature
class PlanRefItem:
    tag: PlanTag
    score: F64


@feature
class PlanListNumItem:
    vals: List[F64]
    ids: List[U32]


@feature
class PlanListRefItem:
    score: F64
    tags: List[PlanTag]


class TestLayerSchemaPushPlans:
    """Verify LayerSchema pre-computes push plans correctly."""

    def test_basic_numeric_str_bytes(self):
        schema = get_schema(PlanTestItem)
        # numeric: x (F64) and y (U32)
        assert len(schema.numeric_plan) == 2
        assert schema.pfd_num_names == ['x', 'y']
        assert schema.pfd_num_ids.dtype == np.uint32
        assert list(schema.pfd_num_ids) == [0, 1]
        # str: label
        assert len(schema.str_plan) == 1
        assert schema.str_plan[0] == (2, 'label', False)
        # bytes: data
        assert len(schema.bytes_plan) == 1
        assert schema.bytes_plan[0] == (3, 'data')
        # no refs
        assert schema.has_ref_fields is False
        assert len(schema.ref_fields) == 0
        assert len(schema.list_ref_fields) == 0
        # list plan empty
        assert len(schema.list_plan) == 0

    def test_wide_string(self):
        schema = get_schema(PlanTestWide)
        assert len(schema.str_plan) == 1
        assert schema.str_plan[0] == (0, 'name', True)
        assert schema.pfd_str_names == ['name']
        assert list(schema.pfd_str_ids) == [0]

    def test_all_numeric_types(self):
        schema = get_schema(PlanTestAllNumeric)
        assert len(schema.numeric_plan) == 8
        names = [name for _, name in schema.numeric_plan]
        assert names == ['a_u8', 'b_u16', 'c_u32', 'd_i32', 'e_f32', 'f_f64', 'g_u8n', 'h_u16n']
        assert len(schema.pfd_num_ids) == 8
        assert list(schema.pfd_num_ids) == list(range(8))
        # no str/bytes/list/ref
        assert len(schema.str_plan) == 0
        assert len(schema.bytes_plan) == 0
        assert len(schema.list_plan) == 0
        assert schema.has_ref_fields is False

    def test_ref_field(self):
        schema = get_schema(PlanRefItem)
        assert len(schema.ref_fields) == 1
        assert schema.ref_fields[0].name == 'tag'
        assert schema.ref_fields[0].ref_target is PlanTag
        assert schema.has_ref_fields is True
        assert len(schema.list_ref_fields) == 0
        # score goes to numeric_plan
        assert len(schema.numeric_plan) == 1
        assert schema.numeric_plan[0] == (1, 'score')

    def test_list_numeric(self):
        schema = get_schema(PlanListNumItem)
        assert len(schema.list_plan) == 2
        # vals: List[F64] → typecode 'd'
        assert schema.list_plan[0] == (0, 'vals', 'd')
        # ids: List[U32] → typecode 'I'
        assert schema.list_plan[1] == (1, 'ids', 'I')
        assert schema.has_ref_fields is False

    def test_list_ref(self):
        schema = get_schema(PlanListRefItem)
        # tags is List[PlanTag] → list_ref_fields, NOT list_plan
        assert len(schema.list_ref_fields) == 1
        assert schema.list_ref_fields[0].name == 'tags'
        assert schema.list_ref_fields[0].list_ref_target is PlanTag
        assert schema.list_ref_fields[0].list_elem_type == OriginFieldType.ref
        assert schema.has_ref_fields is True
        # list_plan should NOT contain list[ref]
        assert len(schema.list_plan) == 0
        # score goes to numeric_plan
        assert len(schema.numeric_plan) == 1

    def test_push_fn_none_before_combine(self):
        """push_fn and batch_fn are None until combine() is called."""
        schema = get_schema(PlanTestItem)
        assert schema.push_fn is None
        assert schema.batch_fn is None

    def test_pfd_arrays_empty_when_no_match(self):
        """pfd_str arrays empty for a class with no string fields."""
        schema = get_schema(PlanTestAllNumeric)
        assert schema.pfd_str_names == []
        assert len(schema.pfd_str_ids) == 0


# ---------------------------------------------------------------------------
# Column access tests
# ---------------------------------------------------------------------------
class TestORM2ColumnAccess:
    def test_table_column_numpy(self):
        """table().column.x returns numpy array with correct values."""
        orm = ORM2.create()
        for i in range(100):
            p = O2Point()
            p.x = float(i)
            p.y = float(i * 10)
            p.label = f"p{i}"
            orm.push(p)
        orm.combine()

        tbl = orm.table(O2Point)
        assert len(tbl) == 100
        xs = tbl.column.x
        ys = tbl.column.y
        assert isinstance(xs, np.ndarray)
        assert xs[0] == 0.0
        assert xs[99] == 99.0
        assert ys[50] == 500.0

    def test_table_column_slice(self):
        """Slicing column arrays works."""
        orm = ORM2.create()
        for i in range(10):
            p = O2Point()
            p.x = float(i)
            p.y = 0.0
            p.label = ""
            orm.push(p)
        orm.combine()

        arr = orm.table(O2Point).column.x[2:5]
        np.testing.assert_array_equal(arr, [2.0, 3.0, 4.0])

    def test_table_before_combine_raises(self):
        """table() before combine() raises."""
        orm = ORM2.create()
        p = O2Point()
        p.x = 1.0; p.y = 2.0; p.label = ""
        orm.push(p)
        with pytest.raises(RuntimeError):
            orm.table(O2Point)

    def test_table_unknown_class_raises(self):
        """table() with unregistered class raises KeyError."""
        @feature
        class Ghost:
            v: F64
        orm = ORM2.create()
        p = O2Point()
        p.x = 1.0; p.y = 2.0; p.label = ""
        orm.push(p)
        orm.combine()
        with pytest.raises(KeyError):
            orm.table(Ghost)

    def test_column_bad_name_raises(self):
        """Accessing a non-existent column raises AttributeError."""
        orm = ORM2.create()
        p = O2Point()
        p.x = 1.0; p.y = 2.0; p.label = ""
        orm.push(p)
        orm.combine()
        with pytest.raises(AttributeError):
            _ = orm.table(O2Point).column.nonexistent