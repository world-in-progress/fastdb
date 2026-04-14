"""End-to-end integration test for the decorator-based ORM."""
import pytest
import numpy as np
from typing import List

from fastdb4py.decorator import feature
from fastdb4py.orm2 import ORM2
from fastdb4py.type import F64, U32, STR


@feature
class Vendor:
    name: STR
    rating: F64


@feature
class Sensor:
    kind: STR
    readings: List[F64]


@feature
class Device:
    model: STR
    serial: U32
    vendor: Vendor
    sensors: List[Sensor]


@feature
class Room:
    name: STR
    area: F64
    devices: List[Device]


class TestFullPipeline:
    def _build_test_data(self):
        orm = ORM2.create()

        v1 = Vendor(); v1.name = "SensorCorp"; v1.rating = 4.5
        v2 = Vendor(); v2.name = "DataTech"; v2.rating = 3.8

        s1 = Sensor(); s1.kind = "temp"; s1.readings = [22.1, 23.4, 21.9]
        s2 = Sensor(); s2.kind = "humidity"; s2.readings = [45.0, 48.2]
        s3 = Sensor(); s3.kind = "pressure"; s3.readings = [1013.25]

        d1 = Device(); d1.model = "TH-100"; d1.serial = 1001
        d1.vendor = v1; d1.sensors = [s1, s2]

        d2 = Device(); d2.model = "P-200"; d2.serial = 1002
        d2.vendor = v2; d2.sensors = [s3]

        room = Room(); room.name = "Lab A"
        room.area = 50.0; room.devices = [d1, d2]

        orm.push(room)
        orm.combine()
        return orm

    def test_counts(self):
        orm = self._build_test_data()
        assert orm.count(Vendor) == 2
        assert orm.count(Sensor) == 3
        assert orm.count(Device) == 2
        assert orm.count(Room) == 1

    def test_copy_readback_scalars(self):
        orm = self._build_test_data()
        v = orm.get(Vendor, 0, mode='copy')
        assert v.name == "SensorCorp"
        assert abs(v.rating - 4.5) < 1e-9

    def test_copy_readback_list_numeric(self):
        orm = self._build_test_data()
        s = orm.get(Sensor, 0, mode='copy')
        assert s.kind == "temp"
        np.testing.assert_array_almost_equal(s.readings, [22.1, 23.4, 21.9])

    def test_map_readback(self):
        orm = self._build_test_data()
        v = orm.get(Vendor, 1, mode='map')
        assert v.name == "DataTech"
        assert abs(v.rating - 3.8) < 1e-9

    def test_iter_all(self):
        orm = self._build_test_data()
        sensors = list(orm.iter(Sensor, mode='copy'))
        assert len(sensors) == 3
        kinds = {s.kind for s in sensors}
        assert kinds == {"temp", "humidity", "pressure"}

    def test_shared_vendor_dedup(self):
        """If two devices share a vendor, push it only once."""
        orm = ORM2.create()
        v = Vendor(); v.name = "SharedCo"; v.rating = 5.0

        d1 = Device(); d1.model = "A"; d1.serial = 1
        d1.vendor = v; d1.sensors = []
        d2 = Device(); d2.model = "B"; d2.serial = 2
        d2.vendor = v; d2.sensors = []

        orm.push(d1)
        orm.push(d2)
        orm.combine()

        assert orm.count(Vendor) == 1
        assert orm.count(Device) == 2


class TestEdgeCases:
    def test_empty_list(self):
        @feature
        class WithEmpty:
            vals: List[F64]

        orm = ORM2.create()
        obj = WithEmpty(); obj.vals = []
        orm.push(obj)
        orm.combine()

        result = orm.get(WithEmpty, 0, mode='copy')
        assert len(result.vals) == 0 or result.vals is None or len(list(result.vals)) == 0

    def test_default_values(self):
        orm = ORM2.create()
        v = Vendor()  # no fields set
        orm.push(v)
        orm.combine()
        result = orm.get(Vendor, 0, mode='copy')
        assert result.name == ""
        assert result.rating == 0.0

    def test_large_batch(self):
        """Test pushing multiple items - verifies batch operations work correctly."""
        orm = ORM2.create()
        for i in range(10):
            v = Vendor()
            v.name = f"v{i}"
            v.rating = float(i)
            orm.push(v)
        orm.combine()

        assert orm.count(Vendor) == 10
        # Test first, middle, and last items
        v0 = orm.get(Vendor, 0, mode='copy')
        assert v0.name == "v0"
        assert abs(v0.rating - 0.0) < 1e-9
        
        v5 = orm.get(Vendor, 5, mode='copy')
        assert v5.name == "v5"
        assert abs(v5.rating - 5.0) < 1e-9
        
        v9 = orm.get(Vendor, 9, mode='copy')
        assert v9.name == "v9"
        assert abs(v9.rating - 9.0) < 1e-9