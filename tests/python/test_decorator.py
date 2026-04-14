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