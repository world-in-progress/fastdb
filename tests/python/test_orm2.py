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