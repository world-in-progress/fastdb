# tests/python/test_schema_unified.py
"""Verify LayerSchema has all attributes needed by Table, serializer, and push compiler."""
from fastdb4py.decorator import feature
from fastdb4py.registry import get_schema
from fastdb4py.type import F64, U32, STR, BYTES
import numpy as np


@feature
class SchemaTestPoint:
    x: F64
    y: F64
    name: STR


@feature
class SchemaStringListPoint:
    names: list[STR]


@feature
class SchemaNestedListPoint:
    values: list[list[F64]]


def test_schema_has_hints():
    schema = get_schema(SchemaTestPoint)
    assert 'x' in schema.hints
    assert 'y' in schema.hints
    assert 'name' in schema.hints


def test_schema_has_ordered_defns():
    schema = get_schema(SchemaTestPoint)
    assert schema.ordered_defns is not None
    names = [name for name, _ in schema.ordered_defns]
    assert 'x' in names
    assert 'y' in names
    assert 'name' in names


def test_schema_has_field_index_map():
    schema = get_schema(SchemaTestPoint)
    assert schema.field_index_map is not None
    assert 'x' in schema.field_index_map
    assert isinstance(schema.field_index_map['x'], int)


def test_schema_has_origin_hints():
    """origin_hints: dict mapping field_name -> (OriginFieldType, schema_index)."""
    schema = get_schema(SchemaTestPoint)
    assert schema.origin_hints is not None
    ft, idx = schema.origin_hints['x']
    from fastdb4py.type import OriginFieldType
    assert ft == OriginFieldType.f64


def test_schema_has_column_accessor_class():
    """column_accessor_class should start as None and be populated later."""
    schema = get_schema(SchemaTestPoint)
    # Initially None - populated lazily when Table creates column accessor
    assert schema.column_accessor_class is None


def test_schema_has_scalar_field_ids_np():
    """numpy array of field IDs for all scalar fields."""
    schema = get_schema(SchemaTestPoint)
    assert isinstance(schema.scalar_field_ids_np, np.ndarray)
    assert schema.scalar_field_ids_np.dtype == np.int32


def test_schema_cls_dict_fastpath():
    """get_schema() should use cls.__dict__ fast-path for repeated calls."""
    schema1 = get_schema(SchemaTestPoint)
    schema2 = get_schema(SchemaTestPoint)
    assert schema1 is schema2
    # Verify it's cached on the class
    assert hasattr(SchemaTestPoint, '__fastdb_schema__')


def test_schema_does_not_plan_non_native_scalar_lists_as_numeric_fallback():
    schema = get_schema(SchemaStringListPoint)

    assert schema.list_plan == []


def test_schema_preserves_nested_list_semantics_without_native_plan():
    from fastdb4py.type import OriginFieldType

    schema = get_schema(SchemaNestedListPoint)

    assert schema.fields[0].field_type == OriginFieldType.list
    assert schema.fields[0].list_elem_type == OriginFieldType.list
    assert schema.list_plan == []
