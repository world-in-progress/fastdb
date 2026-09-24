from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys

import pytest

from fastdb4py.payload import (
    Builder,
    BuilderOptions,
    CompiledSpec,
    FixedRun,
    ObjectHandle,
    PayloadError,
)


ROOT = Path(__file__).resolve().parents[3]
SPEC_ROOT = ROOT / "tests" / "golden" / "payload" / "v1" / "binary" / "spec"


def _compile(name: str) -> CompiledSpec:
    return CompiledSpec.compile((SPEC_ROOT / name).read_bytes())


def _finish_fixed(builder: Builder) -> None:
    builder.entry_begin(1, 1).value_u8(0xAB)
    fixed = b"".join(
        value.to_bytes(2, byteorder=sys.byteorder)
        for value in (0x1234, 0, 0xFFFF)
    )
    run = FixedRun(
        fixed,
        count=3,
        stride_bytes=2,
        validity=b"\x05",
        validity_bit_offset=0,
    )
    (
        builder.entry_begin(2, 3)
        .value_fixed_run(run)
        .entry_begin(3, 1)
        .value_u32(0x1234_5678)
        .entry_begin(4, 1)
        .value_i32(-2)
        .entry_begin(5, 4)
        .value_f32_bits(0x8000_0000)
        .value_f32_bits(0x7F80_0000)
        .value_f32_bits(0xFF80_0000)
        .value_f32_bits(0x7FA1_2345)
        .entry_begin(6, 4)
        .value_f64_bits(0x8000_0000_0000_0000)
        .value_f64_bits(0x7FF0_0000_0000_0000)
        .value_f64_bits(0xFFF0_0000_0000_0000)
        .value_f64_bits(0x7FF0_0000_0000_0042)
    )


def _author_fixed():
    spec = _compile("fixed-scalars.source.json")
    try:
        builder = Builder.create(spec)
    finally:
        spec.close()
    try:
        builder.entry_begin(0, 1).value_bool(True)
        _finish_fixed(builder)
        return builder.freeze()
    finally:
        builder.close()


def test_fixed_run_and_immutable_plan_facts_are_core_owned() -> None:
    plan = _author_fixed()
    try:
        info = plan.info()
        assert info.total_bytes == 928
        assert info.logical_value_count == 22
        assert info.list_element_count == 0
        assert info.text_bytes == 0
        assert info.opaque_bytes == 0
        assert info.max_alignment == 8
        assert info.direct_build_status == 1
        assert info.graph_object_count == 0

        clone = plan.clone()
        plan.close()
        with ThreadPoolExecutor(max_workers=8) as executor:
            facts = list(executor.map(lambda _: clone.info(), range(32)))
        assert all(item == facts[0] for item in facts)
        clone.close()
        clone.close()
    finally:
        plan.close()


def test_nested_component_lists_preserve_null_and_empty_shapes() -> None:
    with _compile("nested-lists.source.json") as spec:
        builder = Builder.create(spec)
    try:
        (
            builder.entry_begin(0, 2)
            .value_component_begin()
            .value_list_begin(3)
            .value_null()
            .value_list_begin(0)
            .value_list_begin(3)
            .value_str("")
            .value_null()
            .value_str("alpha")
            .value_null()
            .value_list_begin(0)
            .value_component_begin()
            .value_list_begin(0)
            .value_list_begin(3)
            .value_null()
            .value_wstr_units(())
            .value_wstr_units((0x0041, 0xD83D, 0xDE03))
            .value_list_begin(2)
            .value_null()
            .value_bytes(b"\x00\xffA")
            .entry_begin(1, 3)
            .value_null()
            .value_list_begin(0)
            .value_list_begin(3)
            .value_u16(7)
            .value_null()
            .value_u16(9)
            .entry_begin(2, 1)
            .value_list_begin(3)
            .value_null()
            .value_list_begin(0)
            .value_list_begin(3)
            .value_str("")
            .value_null()
            .value_str("tail")
        )
        plan = builder.freeze()
        try:
            info = plan.info()
            assert info.total_bytes == 1960
            assert info.list_element_count == 20
            assert info.graph_object_count == 0
        finally:
            plan.close()
    finally:
        builder.close()


def test_graph_authoring_preserves_forward_self_and_mutual_refs() -> None:
    with _compile("graph-all-values.source.json") as spec:
        node_component = spec.component_index("Node")
        asset_component = spec.component_index("Asset")
        builder = Builder.create(spec)
    try:
        node = builder.declare_object(node_component)
        asset = builder.declare_object(asset_component)
        (
            builder.object_fill_begin(node)
            .value_bool(True)
            .value_u8(0x12)
            .value_u16(0x3456)
            .value_u32(0x789A_BCDE)
            .value_i32(-1_234_567)
            .value_u8n_bits(0x3FE0_0000_0000_0000)
            .value_u16n_bits(0)
            .value_f32_bits(0x7FA1_2345)
            .value_f64_bits(0xFFF8_0000_0000_1234)
            .value_str("same")
            .value_wstr_units((0x0041, 0xD83D, 0xDE00))
            .value_bytes(b"\x00\xff~")
            .value_component_begin()
            .value_null()
            .value_u16(0xBEEF)
            .value_list_begin(3)
            .value_f32_bits(0x8000_0000)
            .value_null()
            .value_f32_bits(0xFF80_0001)
            .value_ref(node)
            .value_ref(asset)
            .object_fill_begin(asset)
            .value_str("same")
            .value_ref(node)
            .entry_begin(0, 1)
            .value_object(node)
            .entry_begin(1, 1)
            .value_object(asset)
            .entry_begin(2, 2)
            .value_ref(node)
            .value_null()
            .entry_begin(3, 3)
            .value_u8n_bits(0)
            .value_u8n_bits(0x3FE0_0000_0000_0000)
            .value_u8n_bits(0x3FF0_0000_0000_0000)
            .entry_begin(4, 3)
            .value_u16n_bits(0xBFF0_0000_0000_0000)
            .value_u16n_bits(0)
            .value_u16n_bits(0x3FF0_0000_0000_0000)
        )
        plan = builder.freeze()
        try:
            info = plan.info()
            assert info.total_bytes == 1304
            assert info.region_count == 13
            assert info.logical_value_count == 40
            assert info.list_element_count == 3
            assert info.text_bytes == 14
            assert info.opaque_bytes == 3
            assert info.validation_work == 146
            assert info.max_alignment == 8
            assert info.direct_build_status == 1
            assert info.graph_object_count == 2
        finally:
            plan.close()
    finally:
        builder.close()


def test_disconnected_objects_are_explicit_roots() -> None:
    with _compile("graph-disconnected-roots.source.json") as spec:
        component = spec.component_index("Node")
        builder = Builder.create(spec)
    try:
        first = builder.declare_object(component)
        second = builder.declare_object(component)
        (
            builder.object_fill_begin(first)
            .value_u32(11)
            .value_null()
            .object_fill_begin(second)
            .value_u32(22)
            .value_null()
            .entry_begin(0, 2)
            .value_object(first)
            .value_object(second)
        )
        plan = builder.freeze()
        try:
            assert plan.info().total_bytes == 328
            assert plan.info().graph_object_count == 2
        finally:
            plan.close()
    finally:
        builder.close()


def test_core_error_is_retryable_and_builder_is_thread_confined() -> None:
    with _compile("fixed-scalars.source.json") as spec:
        builder = Builder.create(spec)
    try:
        builder.entry_begin(0, 1)
        with pytest.raises(PayloadError) as caught:
            builder.value_u8(1)
        assert caught.value.code == 2004
        assert caught.value.symbol == "TYPE_MISMATCH"

        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(builder.value_bool, True)
            with pytest.raises(PayloadError, match="creating thread"):
                future.result()

        builder.value_bool(True)
        _finish_fixed(builder)
        plan = builder.freeze()
        try:
            assert plan.info().total_bytes == 928
        finally:
            plan.close()
    finally:
        builder.close()


def test_core_owned_builder_limits_are_projected_without_binding_semantics() -> None:
    with _compile("graph-disconnected-roots.source.json") as spec:
        component = spec.component_index("Node")
        builder = Builder.create(spec, BuilderOptions(max_graph_objects=1))
    try:
        builder.declare_object(component)
        with pytest.raises(PayloadError) as caught:
            builder.declare_object(component)
        assert caught.value.code == 2013
        assert caught.value.symbol == "BUILDER_RESOURCE_LIMIT"
        assert caught.value.path == "/objects/Node/1"
    finally:
        builder.close()


def test_object_handles_cannot_be_forged_from_python() -> None:
    with pytest.raises(TypeError, match="created only"):
        ObjectHandle(1)


def test_compiled_spec_handles_cannot_be_forged_from_python() -> None:
    with pytest.raises(TypeError, match="created only"):
        CompiledSpec(None)
