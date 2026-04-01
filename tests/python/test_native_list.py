"""
Tests for native list column support (List[F64], List[Feature], cyclic refs).
Each test group is gated on the feature being available; the tests will fail
with AttributeError / Warning until the corresponding implementation tasks
are complete.
"""
import numpy as np
import pytest
from typing import List

from fastdb4py import Feature, F64, U32, I32
from fastdb4py.type import OriginFieldType
from fastdb4py.orm import ORM, TableDefn


# ---------------------------------------------------------------------------
# Feature definitions shared across tests
# ---------------------------------------------------------------------------

class Weights(Feature):
    id: U32
    scores: List[F64]


class Chain(Feature):
    val: F64
    next_nodes: List['Chain']


# ---------------------------------------------------------------------------
# Task 2 — baseline failing test (list<f64> round-trip)
# ---------------------------------------------------------------------------

def test_list_f64_roundtrip():
    """Write a Weights feature with a list<f64> field, share, load, verify zero-copy."""
    orm = ORM.create()
    w = Weights()
    w.id = 42
    w.scores = [1.0, 2.0, 3.0]
    orm.push(w, feature_name='w0')
    orm.share('test_list_f64')

    orm2 = ORM.load('test_list_f64')
    loaded = orm2.get(Weights, 'w0')
    assert loaded.id == 42
    arr = loaded.scores
    assert isinstance(arr, np.ndarray)
    assert arr.dtype == np.float64
    np.testing.assert_array_equal(arr, [1.0, 2.0, 3.0])
    orm.unlink()


# ---------------------------------------------------------------------------
# Task 5 — schema introspection
# ---------------------------------------------------------------------------

def test_get_list_element_type_f64():
    from fastdb4py.type import get_list_element_type
    assert get_list_element_type(List[F64]) == OriginFieldType.f64


def test_get_list_element_type_forward_ref():
    from fastdb4py.type import get_list_element_type
    assert get_list_element_type(List['Chain']) == OriginFieldType.ref


def test_schema_list_element_type():
    from fastdb4py.feature._schema import get_class_schema
    schema = get_class_schema(Chain)
    assert 'next_nodes' in schema.list_element_types
    assert schema.list_element_types['next_nodes'] == OriginFieldType.ref


# ---------------------------------------------------------------------------
# Task 6 — FeatureRefList
# ---------------------------------------------------------------------------

def test_feature_ref_list_lazy():
    """FeatureRefList: iterable, len, negative index, to_list."""
    from fastdb4py.feature.ref_list import FeatureRefList

    orm = ORM.create()
    b = Chain(); b.val = 2.0; b.next_nodes = []
    c = Chain(); c.val = 3.0; c.next_nodes = []
    a = Chain(); a.val = 1.0; a.next_nodes = [b, c]
    orm.push(a, feature_name='root')
    orm.share('test_ref_list')

    orm2 = ORM.load('test_ref_list')
    root = orm2.get(Chain, 'root')
    kids = root.next_nodes
    assert isinstance(kids, FeatureRefList)
    assert len(kids) == 2
    assert kids[0].val == 2.0
    assert kids[1].val == 3.0
    assert kids[-1].val == 3.0
    flat = kids.to_list()
    assert len(flat) == 2
    orm.unlink()


# ---------------------------------------------------------------------------
# Task 7 — Feature.__getattr__ pure-Python fallthrough
# ---------------------------------------------------------------------------

def test_feature_getattr_list_numeric_pure_python():
    w = Weights()
    w.scores = [1.0, 2.0]
    assert w.scores == [1.0, 2.0]


def test_feature_getattr_list_ref_pure_python():
    a = Chain()
    b = Chain(); b.val = 7.0; b.next_nodes = []
    a.next_nodes = [b]
    assert a.next_nodes[0].val == 7.0


# ---------------------------------------------------------------------------
# Task 8 — _GraphCollector
# ---------------------------------------------------------------------------

def test_graph_collector_simple():
    """Acyclic graph: post-order (leaves first), no back-edges."""
    from fastdb4py.orm._graph import _GraphCollector
    leaf = Chain(); leaf.val = 2.0; leaf.next_nodes = []
    root = Chain(); root.val = 1.0; root.next_nodes = [leaf]

    gc = _GraphCollector()
    gc.collect(root)

    assert gc.order.index(leaf) < gc.order.index(root)
    assert len(gc.back_edges) == 0


def test_graph_collector_cycle():
    """Cyclic graph: back-edge recorded, no infinite loop."""
    from fastdb4py.orm._graph import _GraphCollector

    class Ring(Feature):
        val: F64
        next_nodes: List['Ring']

    a = Ring(); a.val = 1.0; a.next_nodes = []
    b = Ring(); b.val = 2.0; b.next_nodes = [a]
    a.next_nodes = [b]

    gc = _GraphCollector()
    gc.collect(a)

    assert len(gc.order) == 2
    assert len(gc.back_edges) == 1


# ---------------------------------------------------------------------------
# Task 10 — Cyclic ref + zero-copy integration
# ---------------------------------------------------------------------------

def test_cyclic_ref_roundtrip():
    """Two nodes with mutual list<ref> — both survive the round-trip."""
    class Peer(Feature):
        val: F64
        partners: List['Peer']

    orm = ORM.create()
    a = Peer(); a.val = 10.0; a.partners = []
    b = Peer(); b.val = 20.0; b.partners = [a]
    a.partners = [b]

    orm.push(a, feature_name='a')
    orm.share('test_cycle')

    orm2 = ORM.load('test_cycle')
    la = orm2.get(Peer, 'a')
    assert la.val == 10.0
    assert la.partners[0].val == 20.0
    orm.unlink()


def test_list_f64_zero_copy():
    """NumPy array from list<f64> must point inside the SHM buffer."""
    import ctypes

    class Vec(Feature):
        data: List[F64]

    v = Vec(); v.data = [3.14, 2.71, 1.41]
    orm = ORM.create()
    orm.push(v, feature_name='v')
    orm.share('test_zerocopy')

    orm2 = ORM.load('test_zerocopy')
    loaded = orm2.get(Vec, 'v')
    arr = loaded.data
    assert isinstance(arr, np.ndarray)

    shm_buf = orm2._shm.buf
    shm_start = ctypes.addressof(ctypes.c_char.from_buffer(shm_buf))
    arr_start = arr.ctypes.data
    assert shm_start <= arr_start < shm_start + len(shm_buf), \
        "list<f64> array is NOT zero-copy from SHM — a copy was made"
    orm.unlink()
