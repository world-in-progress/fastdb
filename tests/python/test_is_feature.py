# tests/python/test_is_feature.py
from fastdb4py.decorator import feature
from fastdb4py.registry import is_feature
from fastdb4py.type import F64, STR

def test_is_feature_decorated_class():
    @feature
    class Point:
        x: F64
        y: F64
    assert is_feature(Point) is True

def test_is_feature_plain_class():
    class NotAFeature:
        x: float
    assert is_feature(NotAFeature) is False

def test_is_feature_non_class():
    assert is_feature(42) is False
    assert is_feature("hello") is False
    assert is_feature(None) is False

def test_is_feature_rejects_subclass_forgery():
    """is_feature uses cls.__dict__.get(), not getattr(), to prevent inheritance forgery."""
    @feature
    class Base:
        x: F64
    class Sub(Base):
        pass
    # Sub inherits __fastdb_feature__ via getattr, but __dict__ won't have it
    assert is_feature(Sub) is False

def test_is_feature_with_old_feature_class():
    """Old Feature subclasses are NOT features under the new system."""
    from fastdb4py.feature import Feature
    class Old(Feature):
        x: F64
    assert is_feature(Old) is False


# --- Class registry tests ---

from fastdb4py.registry import lookup_class

def test_class_registry_lookup():
    @feature
    class Lookup1:
        x: F64
    found = lookup_class("Lookup1")
    assert found is Lookup1

def test_class_registry_not_found():
    assert lookup_class("NonExistent__") is None

def test_class_registry_collision_detection():
    """Same name from different definitions should fail fast."""
    import pytest
    @feature
    class Collider:
        x: F64

    # Create a new class with the same name but different identity
    ns = {}
    exec("""
from fastdb4py.type import F64
class Collider:
    x: F64
    """, ns)
    other_cls = ns['Collider']

    with pytest.raises(ValueError, match="already registered"):
        from fastdb4py.registry import register_class
        register_class(other_cls)
