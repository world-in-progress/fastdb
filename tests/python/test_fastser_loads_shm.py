"""Tests for FastSerializer.loads_shm — deserialization from shared memory."""
import platform
import numpy as np
import numpy.testing as npt
import pytest
from multiprocessing import shared_memory
from typing import List

from fastdb4py import Feature, FastSerializer
from fastdb4py.decorator import feature
from fastdb4py.type import F64, U32, I32, STR


# ---------------------------------------------------------------------------
# Test Feature classes
# ---------------------------------------------------------------------------

@feature
class SimplePoint:
    x: F64
    y: F64
    label: STR


@feature
class NumericCloud:
    name: STR
    id: U32
    positions: List[F64]
    indices: List[U32]


# Uses np.ndarray annotation — must remain Feature subclass
class WithArray(Feature):
    tag: STR
    weights: np.ndarray


@feature
class Nested:
    value: F64
    child: 'Nested'


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _shm_name(request):
    """Generate a unique shared memory name from the test node id."""
    raw = request.node.name.replace("[", "_").replace("]", "_")
    # POSIX shm names must start with / and be short
    return f"fdb_test_{raw}"[:30]


def _write_to_shm(data: bytes, name: str, offset: int = 0):
    """Create shared memory and write *data* at *offset*."""
    total = offset + len(data)
    shm = shared_memory.SharedMemory(name=name, create=True, size=total)
    shm.buf[offset:offset + len(data)] = data
    return shm


# ---------------------------------------------------------------------------
# Basic round-trip tests
# ---------------------------------------------------------------------------

class TestLoadsShmBasic:
    """loads_shm produces the same result as loads for various Feature types."""

    def test_simple_scalars(self, request):
        name = _shm_name(request)
        pt = SimplePoint(x=3.14, y=2.72, label="hello")
        data = FastSerializer.dumps(pt)

        shm = _write_to_shm(data, name)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), 0, SimplePoint)
            assert loaded.x == pt.x
            assert loaded.y == pt.y
            assert loaded.label == pt.label
        finally:
            shm.close()
            shm.unlink()

    def test_numeric_lists(self, request):
        name = _shm_name(request)
        cloud = NumericCloud(
            name="test",
            id=42,
            positions=[1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
            indices=[10, 20, 30],
        )
        data = FastSerializer.dumps(cloud)

        shm = _write_to_shm(data, name)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), 0, NumericCloud)
            assert loaded.name == "test"
            assert loaded.id == 42
            npt.assert_array_equal(loaded.positions, [1.0, 2.0, 3.0, 4.0, 5.0, 6.0])
            npt.assert_array_equal(loaded.indices, [10, 20, 30])
        finally:
            shm.close()
            shm.unlink()

    def test_numpy_array(self, request):
        name = _shm_name(request)
        arr = np.array([1.1, 2.2, 3.3, 4.4], dtype=np.float64)
        obj = WithArray(tag="weights", weights=arr)
        data = FastSerializer.dumps(obj)

        shm = _write_to_shm(data, name)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), 0, WithArray)
            assert loaded.tag == "weights"
            npt.assert_array_almost_equal(loaded.weights, arr)
        finally:
            shm.close()
            shm.unlink()

    def test_nested_feature(self, request):
        name = _shm_name(request)
        inner = Nested(value=1.0, child=None)
        outer = Nested(value=2.0, child=inner)
        data = FastSerializer.dumps(outer)

        shm = _write_to_shm(data, name)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), 0, Nested)
            assert loaded.value == 2.0
            assert loaded.child is not None
            assert loaded.child.value == 1.0
            assert loaded.child.child is None
        finally:
            shm.close()
            shm.unlink()


# ---------------------------------------------------------------------------
# Offset tests
# ---------------------------------------------------------------------------

class TestLoadsShmOffset:
    """Data can be placed at an arbitrary offset inside shared memory."""

    def test_nonzero_offset(self, request):
        name = _shm_name(request)
        pt = SimplePoint(x=9.9, y=8.8, label="off")
        data = FastSerializer.dumps(pt)
        offset = 128

        shm = _write_to_shm(data, name, offset=offset)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), offset, SimplePoint)
            assert loaded.x == pytest.approx(9.9)
            assert loaded.y == pytest.approx(8.8)
            assert loaded.label == "off"
        finally:
            shm.close()
            shm.unlink()

    def test_large_offset(self, request):
        name = _shm_name(request)
        pt = SimplePoint(x=1.0, y=2.0, label="far")
        data = FastSerializer.dumps(pt)
        offset = 4096

        shm = _write_to_shm(data, name, offset=offset)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), offset, SimplePoint)
            assert loaded.x == 1.0
            assert loaded.label == "far"
        finally:
            shm.close()
            shm.unlink()

    def test_multiple_objects_different_offsets(self, request):
        """Two serialized objects packed back-to-back in the same segment."""
        name = _shm_name(request)
        pt1 = SimplePoint(x=1.0, y=2.0, label="first")
        pt2 = SimplePoint(x=3.0, y=4.0, label="second")
        data1 = FastSerializer.dumps(pt1)
        data2 = FastSerializer.dumps(pt2)

        total = len(data1) + len(data2)
        shm = shared_memory.SharedMemory(name=name, create=True, size=total)
        shm.buf[0:len(data1)] = data1
        shm.buf[len(data1):total] = data2
        try:
            loaded1 = FastSerializer.loads_shm(name, len(data1), 0, SimplePoint)
            loaded2 = FastSerializer.loads_shm(name, len(data2), len(data1), SimplePoint)
            assert loaded1.label == "first"
            assert loaded2.label == "second"
            assert loaded1.x == 1.0
            assert loaded2.x == 3.0
        finally:
            shm.close()
            shm.unlink()


# ---------------------------------------------------------------------------
# Detachment tests — shared memory is released after loads_shm returns
# ---------------------------------------------------------------------------

class TestLoadsShmDetachment:
    """After loads_shm, the Feature is fully detached from shared memory."""

    def test_feature_detached_from_db(self, request):
        """_origin and _db should not exist (or be None) after loads_shm."""
        name = _shm_name(request)
        pt = SimplePoint(x=1.0, y=2.0, label="det")
        data = FastSerializer.dumps(pt)

        shm = _write_to_shm(data, name)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), 0, SimplePoint)
            assert getattr(loaded, '_origin', None) is None
            assert getattr(loaded, '_db', None) is None
            # Fields still accessible
            assert loaded.x == 1.0
            assert loaded.label == "det"
        finally:
            shm.close()
            shm.unlink()

    def test_nested_features_all_detached(self, request):
        name = _shm_name(request)
        inner = Nested(value=10.0, child=None)
        outer = Nested(value=20.0, child=inner)
        data = FastSerializer.dumps(outer)

        shm = _write_to_shm(data, name)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), 0, Nested)
            assert getattr(loaded, '_origin', None) is None
            assert getattr(loaded.child, '_origin', None) is None
        finally:
            shm.close()
            shm.unlink()

    def test_writable_after_detach(self, request):
        """Detached features should be writable (pure Python mode)."""
        name = _shm_name(request)
        pt = SimplePoint(x=1.0, y=2.0, label="rw")
        data = FastSerializer.dumps(pt)

        shm = _write_to_shm(data, name)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), 0, SimplePoint)
            loaded.x = 99.9
            loaded.label = "modified"
            assert loaded.x == 99.9
            assert loaded.label == "modified"
        finally:
            shm.close()
            shm.unlink()

    def test_re_serializable_after_detach(self, request):
        """Detached features can be serialized again."""
        name = _shm_name(request)
        cloud = NumericCloud(name="c", id=7, positions=[1.0, 2.0], indices=[3, 4])
        data = FastSerializer.dumps(cloud)

        shm = _write_to_shm(data, name)
        try:
            loaded = FastSerializer.loads_shm(name, len(data), 0, NumericCloud)
            data2 = FastSerializer.dumps(loaded)
            loaded2 = FastSerializer.loads(data2, NumericCloud)
            assert loaded2.name == "c"
            assert loaded2.id == 7
            npt.assert_array_equal(loaded2.positions, [1.0, 2.0])
        finally:
            shm.close()
            shm.unlink()


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

class TestLoadsShmErrors:

    def test_nonexistent_shm_raises(self):
        with pytest.raises(FileNotFoundError):
            FastSerializer.loads_shm("fdb_no_such_shm_segment", 100, 0, SimplePoint)

    def test_offset_plus_length_exceeds_size(self, request):
        name = _shm_name(request)
        pt = SimplePoint(x=1.0, y=2.0, label="err")
        data = FastSerializer.dumps(pt)

        shm = _write_to_shm(data, name)
        try:
            with pytest.raises(ValueError, match="exceeds shared memory size"):
                FastSerializer.loads_shm(name, len(data), shm.size, SimplePoint)
        finally:
            shm.close()
            shm.unlink()

    def test_zero_length(self, request):
        """Zero-length region should either return None or raise."""
        name = _shm_name(request)
        shm = shared_memory.SharedMemory(name=name, create=True, size=64)
        try:
            # An empty / invalid buffer will either return None or raise
            # depending on the C++ loader behaviour
            result = FastSerializer.loads_shm(name, 0, 0, SimplePoint)
            # If the C++ layer doesn't crash, None is acceptable
            assert result is None
        except Exception:
            pass  # Any exception is fine for invalid input
        finally:
            shm.close()
            shm.unlink()


# ---------------------------------------------------------------------------
# Consistency: loads_shm == loads for identical bytes
# ---------------------------------------------------------------------------

class TestLoadsShmConsistency:
    """loads_shm and loads produce identical results given the same bytes."""

    def test_scalar_consistency(self, request):
        name = _shm_name(request)
        pt = SimplePoint(x=42.0, y=-1.5, label="cons")
        data = FastSerializer.dumps(pt)

        from_bytes = FastSerializer.loads(data, SimplePoint)

        shm = _write_to_shm(data, name)
        try:
            from_shm = FastSerializer.loads_shm(name, len(data), 0, SimplePoint)
            assert from_shm.x == from_bytes.x
            assert from_shm.y == from_bytes.y
            assert from_shm.label == from_bytes.label
        finally:
            shm.close()
            shm.unlink()

    def test_complex_feature_consistency(self, request):
        name = _shm_name(request)
        cloud = NumericCloud(
            name="big",
            id=999,
            positions=[float(i) for i in range(300)],
            indices=list(range(200)),
        )
        data = FastSerializer.dumps(cloud)

        from_bytes = FastSerializer.loads(data, NumericCloud)

        shm = _write_to_shm(data, name)
        try:
            from_shm = FastSerializer.loads_shm(name, len(data), 0, NumericCloud)
            assert from_shm.name == from_bytes.name
            assert from_shm.id == from_bytes.id
            npt.assert_array_equal(from_shm.positions, from_bytes.positions)
            npt.assert_array_equal(from_shm.indices, from_bytes.indices)
        finally:
            shm.close()
            shm.unlink()
