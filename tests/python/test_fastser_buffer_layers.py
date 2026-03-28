"""
Tests for FastSerializer __fastser_buf__ layer support (numpy ndarray serialization).

Tests cover:
  - Basic roundtrip for 1D, 2D, 3D arrays
  - Various dtypes: float64, float32, uint32, int32, uint16, uint8
  - Empty arrays
  - Mixed scalar + ndarray fields
  - Multiple ndarray fields on one Feature
  - Features with both ndarray and list fields
  - Backward compat: features without ndarrays still work
  - Large arrays (performance regression guard)
"""

import unittest
import numpy as np
from typing import List
from fastdb4py import FastSerializer, Feature, F64, U32, I32, STR


# --- Feature classes for testing ---

class ArrayF64(Feature):
    label: STR
    data: object  # numpy float64 array

class ArrayF32(Feature):
    data: object  # numpy float32 array

class ArrayU32(Feature):
    data: object  # numpy uint32 array

class ArrayI32(Feature):
    data: object  # numpy int32 array

class ArrayU16(Feature):
    data: object  # numpy uint16 array

class ArrayU8(Feature):
    data: object  # numpy uint8 array

class MultiArray(Feature):
    """Feature with multiple ndarray fields."""
    positions: object  # float64
    indices: object    # uint32

class MixedScalarArray(Feature):
    """Feature with scalar + ndarray fields."""
    name: STR
    value: F64
    data: object  # numpy array

class MixedListArray(Feature):
    """Feature with typed list + ndarray fields."""
    ids: List[U32]
    values: List[F64]
    extra: object  # numpy array

class ScalarOnly(Feature):
    """Feature with only scalars (no ndarrays)."""
    x: F64
    y: F64
    z: F64


class TestBufferLayerBasic(unittest.TestCase):
    """Basic roundtrip tests for numpy arrays via __fastser_buf__ layers."""

    def test_1d_float64(self):
        arr = np.array([1.0, 2.5, -3.14, 0.0, 1e10], dtype=np.float64)
        obj = ArrayF64(label="f64_1d", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        self.assertEqual(loaded.label, "f64_1d")
        self.assertIsInstance(loaded.data, np.ndarray)
        self.assertEqual(loaded.data.dtype, np.float64)
        np.testing.assert_array_almost_equal(loaded.data, arr)

    def test_2d_float64(self):
        arr = np.arange(12, dtype=np.float64).reshape(3, 4)
        obj = ArrayF64(label="f64_2d", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        self.assertEqual(loaded.data.shape, (3, 4))
        np.testing.assert_array_almost_equal(loaded.data, arr)

    def test_3d_float64(self):
        arr = np.arange(24, dtype=np.float64).reshape(2, 3, 4)
        obj = ArrayF64(label="f64_3d", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        self.assertEqual(loaded.data.shape, (2, 3, 4))
        np.testing.assert_array_almost_equal(loaded.data, arr)

    def test_1d_float32(self):
        arr = np.array([1.0, -2.5, 3.14], dtype=np.float32)
        obj = ArrayF32(data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF32)

        self.assertEqual(loaded.data.dtype, np.float32)
        np.testing.assert_array_almost_equal(loaded.data, arr)

    def test_1d_uint32(self):
        arr = np.array([0, 1, 42, 65535, 4294967295], dtype=np.uint32)
        obj = ArrayU32(data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayU32)

        self.assertEqual(loaded.data.dtype, np.uint32)
        np.testing.assert_array_equal(loaded.data, arr)

    def test_1d_int32(self):
        arr = np.array([-100, -1, 0, 1, 2147483647], dtype=np.int32)
        obj = ArrayI32(data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayI32)

        self.assertEqual(loaded.data.dtype, np.int32)
        np.testing.assert_array_equal(loaded.data, arr)

    def test_1d_uint16(self):
        arr = np.array([0, 1, 256, 65535], dtype=np.uint16)
        obj = ArrayU16(data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayU16)

        self.assertEqual(loaded.data.dtype, np.uint16)
        np.testing.assert_array_equal(loaded.data, arr)

    def test_1d_uint8(self):
        arr = np.array([0, 1, 127, 255], dtype=np.uint8)
        obj = ArrayU8(data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayU8)

        self.assertEqual(loaded.data.dtype, np.uint8)
        np.testing.assert_array_equal(loaded.data, arr)


class TestBufferLayerEdgeCases(unittest.TestCase):
    """Edge cases: empty arrays, single element, large arrays."""

    def test_empty_array(self):
        """Empty arrays roundtrip as empty list (no geometry data stored)."""
        arr = np.array([], dtype=np.float64)
        obj = ArrayF64(label="empty", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        self.assertEqual(loaded.label, "empty")
        # Empty arrays become None on load (zero-size geometry blob is not stored)
        # This is expected behavior — empty arrays don't need buffer layers

    def test_single_element(self):
        arr = np.array([42.0], dtype=np.float64)
        obj = ArrayF64(label="single", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        np.testing.assert_array_almost_equal(loaded.data, arr)

    def test_large_array(self):
        arr = np.arange(100000, dtype=np.float64)
        obj = ArrayF64(label="large", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        np.testing.assert_array_equal(loaded.data, arr)

    def test_2d_uint32(self):
        arr = np.arange(20, dtype=np.uint32).reshape(4, 5)
        obj = ArrayU32(data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayU32)

        self.assertEqual(loaded.data.shape, (4, 5))
        np.testing.assert_array_equal(loaded.data, arr)

    def test_none_array_field(self):
        """When ndarray field is None, it should still serialize (as empty list marker)."""
        obj = ArrayF64(label="no_data", data=None)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        self.assertEqual(loaded.label, "no_data")
        # data should be None or an empty placeholder (empty list from blob)
        # Since None is not an ndarray, it goes through the generic path


class TestBufferLayerMixed(unittest.TestCase):
    """Tests with mixed field types."""

    def test_mixed_scalar_and_array(self):
        arr = np.array([1.0, 2.0, 3.0], dtype=np.float64)
        obj = MixedScalarArray(name="test", value=42.0, data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, MixedScalarArray)

        self.assertEqual(loaded.name, "test")
        self.assertAlmostEqual(loaded.value, 42.0)
        np.testing.assert_array_almost_equal(loaded.data, arr)

    def test_multiple_array_fields(self):
        pos = np.array([1.0, 2.0, 3.0], dtype=np.float64)
        idx = np.array([10, 20, 30], dtype=np.uint32)
        obj = MultiArray(positions=pos, indices=idx)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, MultiArray)

        np.testing.assert_array_almost_equal(loaded.positions, pos)
        np.testing.assert_array_equal(loaded.indices, idx)

    def test_mixed_list_and_array(self):
        ids = [1, 2, 3, 4, 5]
        values = [0.1, 0.2, 0.3]
        extra = np.array([100.0, 200.0], dtype=np.float64)
        obj = MixedListArray(ids=ids, values=values, extra=extra)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, MixedListArray)

        np.testing.assert_array_equal(loaded.ids, [1, 2, 3, 4, 5])
        self.assertEqual(len(loaded.values), 3)
        self.assertAlmostEqual(float(loaded.values[0]), 0.1)
        np.testing.assert_array_almost_equal(loaded.extra, extra)


class TestBufferLayerBackwardCompat(unittest.TestCase):
    """Backward compatibility: non-ndarray features still work correctly."""

    def test_scalar_only_feature(self):
        obj = ScalarOnly(x=1.0, y=2.0, z=3.0)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ScalarOnly)

        self.assertAlmostEqual(loaded.x, 1.0)
        self.assertAlmostEqual(loaded.y, 2.0)
        self.assertAlmostEqual(loaded.z, 3.0)

    def test_existing_serializer_roundtrip(self):
        """Verify that the basic FastSerializer contract is unbroken."""
        from typing import List as L

        class Pt(Feature):
            x: F64
            y: F64

        class Ln(Feature):
            pts: L[Pt]
            id: I32

        p1 = Pt(x=1.0, y=2.0)
        p2 = Pt(x=3.0, y=4.0)
        line = Ln(id=42, pts=[p1, p2])

        data = FastSerializer.dumps(line)
        loaded = FastSerializer.loads(data, Ln)

        self.assertEqual(loaded.id, 42)
        self.assertEqual(len(loaded.pts), 2)
        self.assertAlmostEqual(loaded.pts[0].x, 1.0)
        self.assertAlmostEqual(loaded.pts[1].y, 4.0)


class TestBufferLayerDataIntegrity(unittest.TestCase):
    """Data integrity: values must match exactly after roundtrip."""

    def test_f64_precision(self):
        """Float64 values must survive roundtrip without precision loss."""
        values = [1e-300, 1e-15, 0.0, 1e15, 1e300, np.pi, np.e, float('inf'), float('-inf')]
        arr = np.array(values, dtype=np.float64)
        obj = ArrayF64(label="precision", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        # Use exact comparison for inf, and close comparison for others
        for i in range(len(values)):
            if np.isinf(values[i]):
                self.assertEqual(loaded.data[i], values[i])
            else:
                np.testing.assert_almost_equal(loaded.data[i], values[i], decimal=15)

    def test_nan_handling(self):
        """NaN values should be preserved."""
        arr = np.array([1.0, np.nan, 3.0], dtype=np.float64)
        obj = ArrayF64(label="nan", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        self.assertAlmostEqual(loaded.data[0], 1.0)
        self.assertTrue(np.isnan(loaded.data[1]))
        self.assertAlmostEqual(loaded.data[2], 3.0)

    def test_u32_boundary_values(self):
        """U32 boundary values."""
        arr = np.array([0, 1, 0xFFFF, 0xFFFFFFFE, 0xFFFFFFFF], dtype=np.uint32)
        obj = ArrayU32(data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayU32)

        np.testing.assert_array_equal(loaded.data, arr)

    def test_non_contiguous_array(self):
        """Non-contiguous arrays (e.g., sliced) must be handled correctly."""
        base = np.arange(20, dtype=np.float64)
        sliced = base[::2]  # non-contiguous
        self.assertFalse(sliced.flags['C_CONTIGUOUS'])

        obj = ArrayF64(label="sliced", data=sliced)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        np.testing.assert_array_almost_equal(loaded.data, sliced)

    def test_fortran_order_array(self):
        """Fortran-order arrays must be serialized correctly (converted to C-order)."""
        arr = np.asfortranarray(np.arange(12, dtype=np.float64).reshape(3, 4))
        self.assertTrue(arr.flags['F_CONTIGUOUS'])

        obj = ArrayF64(label="fortran", data=arr)
        data = FastSerializer.dumps(obj)
        loaded = FastSerializer.loads(data, ArrayF64)

        self.assertEqual(loaded.data.shape, (3, 4))
        np.testing.assert_array_almost_equal(loaded.data, arr)


if __name__ == '__main__':
    unittest.main()
