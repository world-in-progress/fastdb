import unittest
import struct
from typing import List, Optional
from fastdb4py import FastSerializer, Feature, I32, U32, F64, STR, REF

class Point(Feature):
    x: F64
    y: F64

class Line(Feature):
    points: List[Point]
    id: I32

class Node(Feature):
    id: I32
    # next: REF # Recursive ref directly 
    # children: List[REF] # List[Node]
    # To support recursive types properly in Python < 3.10 without from __future__ annotations,
    # we rely on FastSerializer's robust discovery.
    # But explicit type hints are needed.
    pass

class RecursiveNode(Feature):
    val: I32
    next: 'RecursiveNode' 

class TreeNode(Feature):
    val: I32
    children: List['TreeNode']

class User(Feature):
    name: STR
    age: I32
    scores: List[float]

class MultiListPayload(Feature):
    ints: List[int]
    names: List[str]
    points: List[Point]

class StringListOnly(Feature):
    names: List[str]

class NumericColumnarLists(Feature):
    ids: List[U32]
    values: List[F64]

# Native Python type annotation classes (no TypeVar aliases)
class NativeScalars(Feature):
    count: int
    ratio: float
    label: str

class NativeLists(Feature):
    ints: List[int]
    floats: List[float]
    names: List[str]

class TestFastSerializer(unittest.TestCase):
    def test_simple_object(self):
        p = Point(x=1.0, y=2.0)
        data = FastSerializer.dumps(p)
        self.assertTrue(len(data) > 0)
        
        p2 = FastSerializer.loads(data, Point)
        self.assertAlmostEqual(p2.x, 1.0)
        self.assertAlmostEqual(p2.y, 2.0)

    def test_nested_list(self):
        p1 = Point(x=1.0, y=2.0)
        p2 = Point(x=3.0, y=4.0)
        line = Line(id=100, points=[p1, p2])
        
        data = FastSerializer.dumps(line)
        line2 = FastSerializer.loads(data, Line)
        
        self.assertEqual(line2.id, 100)
        self.assertEqual(len(line2.points), 2)
        self.assertAlmostEqual(line2.points[0].x, 1.0)
        self.assertAlmostEqual(line2.points[1].y, 4.0)

    def test_scalar_list(self):
        print("Running test_basic_types...")
        u = User(name="Alice", age=30, scores=[90.5, 80.0, 95.5])
        data = FastSerializer.dumps(u)
        print("Dumps done.")
        
        u2 = FastSerializer.loads(data, User)
        print("Loads done.")
        self.assertEqual(u2.name, "Alice")
        self.assertEqual(u2.age, 30)
        self.assertEqual(len(u2.scores), 3)
        self.assertAlmostEqual(u2.scores[0], 90.5)

    def test_cyclic_reference(self):
        print("Running test_cyclic_reference...")
        # A -> B -> A
        n1 = RecursiveNode(val=1)
        n2 = RecursiveNode(val=2)
        n1.next = n2
        n2.next = n1
        
        data = FastSerializer.dumps(n1)
        print("Cyclic dumps done.")
        
        # Load
        check_n1 = FastSerializer.loads(data, RecursiveNode)
        print("Cyclic loads done.")
        self.assertEqual(check_n1.val, 1)
        self.assertIsNotNone(check_n1.next)
        self.assertEqual(check_n1.next.val, 2)
        print("Cyclic checks 1 done.")
        
        # Check cycle identity
        # Note: FastSerializer ensures identity preservation within one load
        self.assertIs(check_n1.next.next, check_n1)
        print("Cyclic checks done.")

    def test_tree_structure(self):
        print("Running test_tree_structure...")
        root = TreeNode(val=0, children=[])
        child1 = TreeNode(val=1, children=[])
        child2 = TreeNode(val=2, children=[])
        root.children.append(child1)
        root.children.append(child2)
        
        subchild = TreeNode(val=3, children=[])
        child1.children.append(subchild)
        
        data = FastSerializer.dumps(root)
        print("Tree dumps done.")
        
        f_root = FastSerializer.loads(data, TreeNode)
        print("Tree loads done.")
        self.assertEqual(f_root.val, 0)
        self.assertEqual(len(f_root.children), 2)
        self.assertEqual(f_root.children[0].val, 1)
        self.assertEqual(f_root.children[0].children[0].val, 3)

    def test_multi_list_and_string_list(self):
        payload = MultiListPayload(
            ints=[1, 2, 3, 5, 8],
            names=["alpha", "beta", "你好", "emoji🙂"],
            points=[Point(x=10.0, y=20.0), Point(x=30.0, y=40.0)]
        )

        data = FastSerializer.dumps(payload)
        payload2 = FastSerializer.loads(data, MultiListPayload)

        self.assertEqual(payload2.ints, [1, 2, 3, 5, 8])
        self.assertEqual(payload2.names, ["alpha", "beta", "你好", "emoji🙂"])
        self.assertEqual(len(payload2.points), 2)
        self.assertAlmostEqual(payload2.points[0].x, 10.0)
        self.assertAlmostEqual(payload2.points[1].y, 40.0)

    def test_string_list_edge_cases(self):
        long_text = "x" * 10000
        payload = StringListOnly(
            names=["", "ascii", "你好", "emoji🙂", "line1\nline2", long_text]
        )

        data = FastSerializer.dumps(payload)
        payload2 = FastSerializer.loads(data, StringListOnly)

        self.assertEqual(
            payload2.names,
            ["", "ascii", "你好", "emoji🙂", "line1\nline2", long_text]
        )

        empty_payload = StringListOnly(names=[])
        empty_data = FastSerializer.dumps(empty_payload)
        empty_payload2 = FastSerializer.loads(empty_data, StringListOnly)
        self.assertEqual(empty_payload2.names, [])

    def test_numeric_list_columnar_path(self):
        payload = NumericColumnarLists(
            ids=[0, 1, 2, 1024, 65535, 4294967295],
            values=[0.0, 1.5, -3.25, 1e-6, 1e6]
        )

        data = FastSerializer.dumps(payload)
        payload2 = FastSerializer.loads(data, NumericColumnarLists)

        self.assertEqual(payload2.ids, [0, 1, 2, 1024, 65535, 4294967295])
        self.assertEqual(len(payload2.values), 5)
        self.assertAlmostEqual(payload2.values[0], 0.0)
        self.assertAlmostEqual(payload2.values[1], 1.5)
        self.assertAlmostEqual(payload2.values[2], -3.25)
        self.assertAlmostEqual(payload2.values[3], 1e-6)
        self.assertAlmostEqual(payload2.values[4], 1e6)

    def test_native_scalar_types(self):
        """Native Python int/float/str annotations round-trip correctly."""
        obj = NativeScalars(count=42, ratio=3.14, label="hello")
        data = FastSerializer.dumps(obj)
        obj2 = FastSerializer.loads(data, NativeScalars)
        self.assertEqual(obj2.count, 42)
        self.assertAlmostEqual(obj2.ratio, 3.14)
        self.assertEqual(obj2.label, "hello")

    def test_native_list_types(self):
        """List[int], List[float], List[str] with native annotations round-trip correctly."""
        obj = NativeLists(
            ints=[-2147483648, 0, 42, 2147483647],
            floats=[1.5, -3.25, 0.0, 1e12],
            names=["alpha", "beta", "你好"],
        )
        data = FastSerializer.dumps(obj)
        obj2 = FastSerializer.loads(data, NativeLists)

        self.assertEqual(obj2.ints, [-2147483648, 0, 42, 2147483647])
        self.assertEqual(len(obj2.floats), 4)
        self.assertAlmostEqual(obj2.floats[0], 1.5)
        self.assertAlmostEqual(obj2.floats[1], -3.25)
        self.assertEqual(obj2.names, ["alpha", "beta", "你好"])

    def test_native_list_int_overflow(self):
        """list[int] raises OverflowError for values outside i32 range."""
        obj = NativeLists(ints=[2**31], floats=[], names=[])
        with self.assertRaises(OverflowError):
            FastSerializer.dumps(obj)

        obj2 = NativeLists(ints=[-2**31 - 1], floats=[], names=[])
        with self.assertRaises(OverflowError):
            FastSerializer.dumps(obj2)

if __name__ == '__main__':
    unittest.main()
