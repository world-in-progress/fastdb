from __future__ import annotations

import importlib

import numpy as np
import pytest

import fastdb4py
import fastdb4py.type as fastdb_type


LEGACY_ENGINE_NAME = "Column" + "Engine"
LEGACY_ENGINE_MODULE = "fastdb4py." + "column" + "_engine"


EXPECTED_P5 = {
    "feature",
    "is_feature",
    "get_schema",
    "lookup_class",
    "Layout",
    "RecordEngine",
    "ObjectEngine",
    "Table",
    "StringColumn",
    "pack_utf8_column",
    "FdbViewOwner",
    "FdbViewInvalidatedError",
    "FdbViewWriteError",
    "invalidate",
    "materialize",
    "FastSerializer",
    "BOOL",
    "U8",
    "U16",
    "U32",
    "I32",
    "U8N",
    "U16N",
    "F32",
    "F64",
    "STR",
    "WSTR",
    "REF",
    "BYTES",
}

REMOVED_MODULES = (
    LEGACY_ENGINE_MODULE,
    "fastdb4py.call_db",
    "fastdb4py.schema",
    "fastdb4py.require",
    "fastdb4py.allocator",
    "fastdb4py.codegen",
)

REMOVED_TOP_LEVEL_NAMES = {
    LEGACY_ENGINE_NAME,
    "Array",
    "ArrayRequirement",
    "Batch",
    "BatchRequirement",
    "array",
    "batch",
    "require",
    "build_call_db",
    "encode_call_db",
    "decode_call_db",
    "export_schema",
    "schema_sha256",
}

REMOVED_TYPE_NAMES = {
    "Array",
    "ArrayRequirement",
    "Batch",
    "BatchRequirement",
    "array",
    "batch",
    "_validate_requirement_rows",
    "_normalize_batch_profile",
}


@fastdb4py.feature
class PublicSurfacePoint:
    x: fastdb4py.F64
    y: fastdb4py.F64
    name: fastdb4py.STR


@fastdb4py.feature
class PublicSurfaceStringList:
    names: list[fastdb4py.STR]


@fastdb4py.feature
class PublicSurfaceNestedList:
    values: list[list[fastdb4py.F64]]


def test_package_root_has_exact_p5_public_surface() -> None:
    assert set(fastdb4py.__all__) == EXPECTED_P5
    assert len(fastdb4py.__all__) == len(EXPECTED_P5)
    assert {
        name for name in EXPECTED_P5 if getattr(fastdb4py, name, None) is None
    } == set()
    assert {
        name for name in REMOVED_TOP_LEVEL_NAMES if hasattr(fastdb4py, name)
    } == set()


def test_record_engine_has_only_the_final_public_name_and_module() -> None:
    assert fastdb4py.RecordEngine.__module__ == "fastdb4py.record_engine"
    assert "RecordEngine" in fastdb4py.__all__
    assert not hasattr(fastdb4py, LEGACY_ENGINE_NAME)
    with pytest.raises(ModuleNotFoundError) as error:
        importlib.import_module(LEGACY_ENGINE_MODULE)
    assert error.value.name == LEGACY_ENGINE_MODULE


@pytest.mark.parametrize("module_name", REMOVED_MODULES)
def test_removed_authority_module_is_not_importable(module_name: str) -> None:
    with pytest.raises(ModuleNotFoundError) as error:
        importlib.import_module(module_name)
    assert error.value.name == module_name


def test_type_module_retains_introspection_without_requirement_values() -> None:
    assert fastdb_type.OriginFieldType.str.name == "str"
    assert fastdb_type.get_origin_type(fastdb_type.WSTR) is fastdb_type.OriginFieldType.wstr
    assert {
        name for name in REMOVED_TYPE_NAMES if hasattr(fastdb_type, name)
    } == set()


def test_retained_schema_has_type_hints() -> None:
    schema = fastdb4py.get_schema(PublicSurfacePoint)

    assert {"x", "y", "name"} <= set(schema.hints)


def test_retained_schema_has_ordered_definitions() -> None:
    schema = fastdb4py.get_schema(PublicSurfacePoint)

    assert [name for name, _ in schema.ordered_defns] == ["x", "y", "name"]


def test_retained_schema_has_field_index_map() -> None:
    schema = fastdb4py.get_schema(PublicSurfacePoint)

    assert schema.field_index_map == {"x": 0, "y": 1, "name": 2}


def test_retained_schema_has_origin_hints() -> None:
    schema = fastdb4py.get_schema(PublicSurfacePoint)

    field_type, index = schema.origin_hints["x"]
    assert field_type is fastdb_type.OriginFieldType.f64
    assert index == 0


def test_retained_schema_column_accessor_starts_unmaterialized() -> None:
    schema = fastdb4py.get_schema(PublicSurfacePoint)

    assert schema.column_accessor_class is None


def test_retained_schema_has_scalar_field_ids() -> None:
    schema = fastdb4py.get_schema(PublicSurfacePoint)

    assert isinstance(schema.scalar_field_ids_np, np.ndarray)
    assert schema.scalar_field_ids_np.dtype == np.int32


def test_retained_schema_uses_class_cache_fast_path() -> None:
    first = fastdb4py.get_schema(PublicSurfacePoint)
    second = fastdb4py.get_schema(PublicSurfacePoint)

    assert first is second
    assert PublicSurfacePoint.__dict__["__fastdb_schema__"] is first


def test_retained_schema_does_not_plan_non_native_scalar_lists() -> None:
    schema = fastdb4py.get_schema(PublicSurfaceStringList)

    assert schema.list_plan == []


def test_retained_schema_preserves_nested_list_semantics() -> None:
    schema = fastdb4py.get_schema(PublicSurfaceNestedList)

    assert schema.fields[0].field_type is fastdb_type.OriginFieldType.list
    assert schema.fields[0].list_elem_type is fastdb_type.OriginFieldType.list
    assert schema.list_plan == []
