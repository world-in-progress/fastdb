from __future__ import annotations

import copy
from pathlib import Path
import threading

import pytest

from fastdb4py.payload import (
    Access,
    BuildPlan,
    BuildPolicy,
    Builder,
    CompiledSpec,
    ExternalBytes,
    MemoryBacking,
    Payload,
    PayloadError,
    View,
    ViewKind,
)


RECORD_SPEC = (
    Path(__file__).parents[2]
    / "golden/payload/v1/spec/valid/record-all-types.source.json"
)


def record_plan() -> tuple[CompiledSpec, BuildPlan]:
    spec = CompiledSpec.compile(RECORD_SPEC.read_bytes())
    builder = Builder.create(spec)
    (
        builder.entry_begin(0, 1)
        .value_component_begin()
        .value_bool(True)
        .value_u8(0xAB)
        .value_u16(0x1234)
        .value_u32(0x89AB_CDEF)
        .value_i32(-42)
        .value_u8n(0.0)
        .value_u16n(1.0)
        .value_f32_bits(0x3FC0_0000)
        .value_f64_bits(0x4004_0000_0000_0000)
        .value_str("\ufeffA\0B")
        .value_wstr_units([0xFEFF, 0x0041, 0, 0xD83C, 0xDF0D, 0x03A9])
        .value_bytes(b"\x00\x01\xff")
        .value_component_begin()
        .value_list_begin(3)
        .value_list_begin(0)
        .value_null()
        .value_list_begin(3)
        .value_str("")
        .value_null()
        .value_str("tail")
    )
    (
        builder.entry_begin(1, 4)
        .value_null()
        .value_list_begin(0)
        .value_list_begin(3)
        .value_u8(0)
        .value_null()
        .value_u8(0xFF)
        .value_list_begin(1)
        .value_u8(7)
    )
    plan = builder.freeze()
    builder.close()
    return spec, plan


def record_payload() -> tuple[CompiledSpec, Payload]:
    spec, plan = record_plan()
    try:
        result = plan.execute(BuildPolicy.ALLOW_STAGING)
    finally:
        plan.close()
    return spec, result.payload


def require_error(
    error: PayloadError,
    code: int,
    symbol: str,
    path: str,
    message: str,
    details: str,
) -> None:
    assert error.code == code
    assert error.symbol == symbol
    assert error.path == path
    assert error.message == message
    assert error.details_json == details


def test_complete_record_views_are_core_owned_and_materialized() -> None:
    spec, payload = record_payload()
    binary = payload.binary_bytes()
    with payload.acquire() as payload_access:
        assert payload_access.payload_bytes() == binary
        with pytest.raises(PayloadError) as raised:
            payload_access.str()
        require_error(
            raised.value,
            2004,
            "TYPE_MISMATCH",
            "/access",
            "Portable payload view kind does not match the operation",
            '{"reason":"view_kind_mismatch"}',
        )

    sequence = payload.entry_view(0)
    assert sequence.kind() is ViewKind.SEQUENCE
    assert not sequence.is_null()
    assert sequence.length() == 1
    root = sequence.at(0)
    assert root.kind() is ViewKind.COMPONENT
    assert root.component_index() == 0
    assert root.field_count() == 14

    with root.field(0) as value:
        assert value.get_bool() is True
    with root.field(1) as value:
        assert value.get_u8() == 0xAB
    with root.field(2) as value:
        assert value.get_u16() == 0x1234
    with root.field(3) as value:
        assert value.get_u32() == 0x89AB_CDEF
    with root.field(4) as value:
        assert value.get_i32() == -42
    with root.field(5) as value:
        assert value.get_u8n_f64_bits() == 0
        assert value.get_u8n() == 0.0
    with root.field(6) as value:
        assert value.get_u16n_f64_bits() == 0x3FF0_0000_0000_0000
        assert value.get_u16n() == 1.0
    with root.field(7) as value:
        assert value.get_f32_bits() == 0x3FC0_0000
        assert value.get_f32() == 1.5
    with root.field(8) as value:
        assert value.get_f64_bits() == 0x4004_0000_0000_0000
        assert value.get_f64() == 2.5

    text = root.field(9)
    assert text.kind() is ViewKind.STR
    text_access = text.acquire()
    text.close()
    assert text_access.str() == "\ufeffA\0B"

    with root.field(10) as wide:
        assert wide.kind() is ViewKind.WSTR
        with wide.acquire() as wide_access:
            assert wide_access.wstr() == "\ufeffA\0🌍Ω"

    with root.field(11) as opaque:
        assert opaque.kind() is ViewKind.BYTES
        with opaque.acquire() as opaque_access:
            assert opaque_access.bytes() == b"\x00\x01\xff"

    with root.field(12) as leaf:
        assert leaf.kind() is ViewKind.COMPONENT
        assert leaf.component_index() == 1
        assert leaf.field_count() == 0

    with root.field(13) as nested:
        assert nested.kind() is ViewKind.LIST
        assert nested.length() == 3
        with nested.at(0) as empty:
            assert not empty.is_null()
            assert empty.length() == 0
        with nested.at(1) as null_list:
            assert null_list.is_null()
        with nested.at(2) as present:
            assert present.length() == 3
            with present.at(0) as empty_text:
                with empty_text.acquire() as access:
                    assert access.str() == ""
            with present.at(1) as null_text:
                assert null_text.is_null()
            with present.at(2) as tail:
                with tail.acquire() as access:
                    assert access.str() == "tail"

    with payload.entry_view(1) as series:
        assert series.length() == 4
        with series.at(0) as null_list:
            assert null_list.is_null()
        with series.at(1) as empty:
            assert not empty.is_null()
            assert empty.length() == 0
        with series.at(2) as values:
            assert values.length() == 3
            with values.at(0) as value:
                assert value.get_u8() == 0
            with values.at(1) as value:
                assert value.is_null()
            with values.at(2) as value:
                assert value.get_u8() == 0xFF
        with series.at(3) as values:
            with values.at(0) as value:
                assert value.get_u8() == 7

    with pytest.raises(PayloadError) as raised:
        root.get_u8()
    require_error(
        raised.value,
        2004,
        "TYPE_MISMATCH",
        "/entries/0/0",
        "Portable payload view kind does not match the operation",
        '{"reason":"view_kind_mismatch"}',
    )

    for _ in range(1_000):
        retained = root.clone()
        assert retained.field_count() == 14
        retained.close()

    detached = root.materialize()
    text_access.close()
    payload.invalidate()
    payload.close()
    spec.close()

    with pytest.raises(PayloadError) as raised:
        root.kind()
    require_error(
        raised.value,
        4001,
        "VIEW_INVALIDATED",
        "/view",
        "Portable payload access barrier rejected the operation",
        '{"reason":"view_invalidated"}',
    )
    with detached.field(1) as value:
        assert value.get_u8() == 0xAB
    with detached.field(9) as detached_text:
        with detached_text.acquire() as access:
            assert access.str() == "\ufeffA\0B"

    detached.close()
    root.close()
    sequence.close()


def test_invalidation_waits_for_the_unique_access_pin() -> None:
    spec, payload = record_payload()
    with payload.entry_view(0) as sequence:
        with sequence.at(0) as root:
            text = root.field(9)
    access = text.acquire()
    started = threading.Event()
    done = threading.Event()
    failures: list[BaseException] = []

    def invalidate() -> None:
        started.set()
        try:
            payload.invalidate()
        except BaseException as error:
            failures.append(error)
        finally:
            done.set()

    worker = threading.Thread(target=invalidate)
    worker.start()
    assert started.wait(timeout=2)
    assert not done.wait(timeout=0.05)
    assert access.str() == "\ufeffA\0B"
    access.close()
    worker.join(timeout=2)
    assert not worker.is_alive()
    assert failures == []

    with pytest.raises(PayloadError) as raised:
        text.kind()
    require_error(
        raised.value,
        4001,
        "VIEW_INVALIDATED",
        "/view",
        "Portable payload access barrier rejected the operation",
        '{"reason":"view_invalidated"}',
    )
    text.close()
    payload.close()
    spec.close()


def test_view_and_access_handles_cannot_be_forged() -> None:
    with pytest.raises(TypeError, match="created only"):
        View(1, object())
    with pytest.raises(TypeError, match="created only"):
        Access(1, object())


def test_python_copy_protocol_preserves_native_ownership() -> None:
    spec, plan = record_plan()
    shallow_spec = copy.copy(spec)
    deep_spec = copy.deepcopy(spec)
    spec.close()
    assert shallow_spec.profile() == deep_spec.profile()

    builder = Builder.create(shallow_spec)
    with pytest.raises(TypeError, match="unique"):
        copy.copy(builder)
    with pytest.raises(TypeError, match="unique"):
        copy.deepcopy(builder)
    builder.close()

    shallow_plan = copy.copy(plan)
    deep_plan = copy.deepcopy(plan)
    plan.close()
    assert shallow_plan.info() == deep_plan.info()
    result = shallow_plan.execute(BuildPolicy.ALLOW_STAGING)
    shallow_plan.close()
    deep_plan.close()

    backing = MemoryBacking()
    with pytest.raises(TypeError, match="mutable callback state"):
        copy.copy(backing)
    with pytest.raises(TypeError, match="mutable callback state"):
        copy.deepcopy(backing)

    source = ExternalBytes(result.payload.binary_bytes())
    assert copy.copy(source) is source
    assert copy.deepcopy(source) is source

    shallow_payload = copy.copy(result.payload)
    deep_payload = copy.deepcopy(result.payload)
    result.payload.close()
    assert shallow_payload.binary_bytes() == deep_payload.binary_bytes()

    access = shallow_payload.acquire()
    with pytest.raises(TypeError, match="unique"):
        copy.copy(access)
    with pytest.raises(TypeError, match="unique"):
        copy.deepcopy(access)
    access.close()

    view = shallow_payload.entry_view(0)
    shallow_view = copy.copy(view)
    deep_view = copy.deepcopy(view)
    view.close()
    assert shallow_view.length() == deep_view.length() == 1
    shallow_view.close()
    deep_view.close()

    shallow_payload.close()
    deep_payload.close()
    shallow_spec.close()
    deep_spec.close()
