import pytest
from fastdb4py.registry import get_schema, FieldDef, LayerSchema
from fastdb4py.type import OriginFieldType, F64, U32, STR

def test_get_schema_scalar_fields():
    class Point:
        x: F64
        y: F64
        label: STR

    schema = get_schema(Point)
    assert isinstance(schema, LayerSchema)
    assert schema.layer_name == 'Point'
    assert len(schema.fields) == 3

    f_x = schema.fields[0]
    assert f_x.name == 'x'
    assert f_x.field_type == OriginFieldType.f64
    assert f_x.field_id == 0

    f_label = schema.fields[2]
    assert f_label.name == 'label'
    assert f_label.field_type == OriginFieldType.str
    assert f_label.field_id == 2

def test_get_schema_python_builtins():
    class Simple:
        count: int
        value: float
        name: str

    schema = get_schema(Simple)
    assert schema.fields[0].field_type == OriginFieldType.i32
    assert schema.fields[1].field_type == OriginFieldType.f64
    assert schema.fields[2].field_type == OriginFieldType.str

def test_get_schema_ref_field():
    class Vendor:
        name: STR

    class Device:
        vendor: Vendor

    schema = get_schema(Device)
    f = schema.fields[0]
    assert f.field_type == OriginFieldType.ref
    assert f.ref_target is Vendor

def test_get_schema_list_field():
    from typing import List
    class Sensor:
        temps: List[F64]

    schema = get_schema(Sensor)
    f = schema.fields[0]
    assert f.field_type == OriginFieldType.list
    assert f.list_elem_type == OriginFieldType.f64

def test_get_schema_caches():
    class Cached:
        x: F64
    s1 = get_schema(Cached)
    s2 = get_schema(Cached)
    assert s1 is s2

def test_get_schema_skips_private():
    class WithPrivate:
        _internal: int
        x: F64

    schema = get_schema(WithPrivate)
    assert len(schema.fields) == 1
    assert schema.fields[0].name == 'x'

from fastdb4py.decorator import feature

def test_feature_decorator_returns_class():
    @feature
    class Point:
        x: F64
        y: F64
    assert Point.__name__ == 'Point'
    p = Point()
    p.x = 1.0
    assert p.x == 1.0

def test_feature_decorator_registers_schema():
    @feature
    class Sensor:
        temp: F64
        label: STR
    schema = get_schema(Sensor)
    assert len(schema.fields) == 2

def test_feature_decorator_rejects_dict():
    with pytest.raises(TypeError, match="Unsupported.*dict"):
        @feature
        class Bad:
            meta: dict

def test_feature_decorator_rejects_any():
    from typing import Any as TypingAny
    with pytest.raises(TypeError, match="Unsupported.*Any"):
        @feature
        class Bad:
            data: TypingAny

def test_feature_decorator_rejects_bare_list():
    with pytest.raises(TypeError, match="Unsupported.*list"):
        @feature
        class Bad:
            items: list

def test_feature_decorator_rejects_tuple():
    with pytest.raises(TypeError, match="Unsupported.*tuple"):
        @feature
        class Bad:
            coords: tuple

def test_feature_decorator_accepts_typed_list():
    from typing import List
    @feature
    class Good:
        vals: List[F64]
    schema = get_schema(Good)
    assert schema.fields[0].field_type == OriginFieldType.list

def test_feature_decorator_accepts_ref():
    @feature
    class Vendor:
        name: STR

    @feature
    class Device:
        vendor: Vendor
    schema = get_schema(Device)
    assert schema.fields[0].field_type == OriginFieldType.ref
    assert schema.fields[0].ref_target is Vendor


def test_feature_injects_init():
    """@feature should inject __init__(**kwargs) for pure-Python construction."""
    @feature
    class WithInit:
        x: F64
        y: F64
    obj = WithInit(x=1.0, y=2.0)
    assert obj.x == 1.0
    assert obj.y == 2.0

def test_feature_init_ignores_unknown_kwargs():
    """Unknown kwargs should be stored in __dict__ anyway (duck typing)."""
    @feature
    class Flexible:
        x: F64
    obj = Flexible(x=1.0, extra="hello")
    assert obj.x == 1.0
    assert obj.extra == "hello"

def test_feature_rejects_slots():
    """@feature should reject classes with __slots__ (unless they include __dict__)."""
    with pytest.raises(TypeError, match="__slots__"):
        @feature
        class Slotted:
            __slots__ = ('x',)
            x: F64

def test_feature_allows_slots_with_dict():
    """Classes with __slots__ = ('__dict__',) are fine."""
    @feature
    class SlottedDict:
        __slots__ = ('__dict__',)
        x: F64
    obj = SlottedDict(x=1.0)
    assert obj.x == 1.0

def test_feature_forward_ref_tolerance():
    """@feature should not crash on forward references in annotations."""
    @feature
    class Container:
        x: F64
        child: 'ForwardRefTarget'
    # Should not raise — forward refs are deferred to schema resolution time
    assert hasattr(Container, '__fastdb_feature__')