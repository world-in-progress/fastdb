from __future__ import annotations

import ctypes
import gc
from pathlib import Path
import threading

import pytest

from fastdb4py.payload import (
    BackingStatus,
    BuildPolicy,
    Builder,
    CompiledSpec,
    ExecutionMode,
    ExternalBytes,
    FallbackReason,
    MemoryBacking,
    OpenOptions,
    Payload,
    PayloadError,
    Profile,
    ReserveMode,
)


SPEC_ROOT = Path(__file__).parents[2] / "golden/payload/v1/binary/spec"


def fixed_plan():
    spec = CompiledSpec.compile((SPEC_ROOT / "fixed-scalars.source.json").read_bytes())
    builder = Builder.create(spec)
    builder.entry_begin(0, 1).value_bool(True)
    builder.entry_begin(1, 1).value_u8(0xAB)
    builder.entry_begin(2, 3).value_u16(0x1234).value_null().value_u16(0xFFFF)
    builder.entry_begin(3, 1).value_u32(0x1234_5678)
    builder.entry_begin(4, 1).value_i32(-2)
    builder.entry_begin(5, 4)
    for bits in (0x8000_0000, 0x7F80_0000, 0xFF80_0000, 0x7FA1_2345):
        builder.value_f32_bits(bits)
    builder.entry_begin(6, 4)
    for bits in (
        0x8000_0000_0000_0000,
        0x7FF0_0000_0000_0000,
        0xFFF0_0000_0000_0000,
        0x7FF0_0000_0000_0042,
    ):
        builder.value_f64_bits(bits)
    plan = builder.freeze()
    builder.close()
    return spec, plan


class Observer:
    def __init__(
        self,
        *,
        decline_direct: bool = False,
        fail_write: bool = False,
        fail_commit: bool = False,
        raise_commit: bool = False,
    ) -> None:
        self.decline_direct = decline_direct
        self.fail_write = fail_write
        self.fail_commit = fail_commit
        self.raise_commit = raise_commit
        self.writes = 0
        self.commits = 0
        self.rollbacks = 0
        self.releases = 0

    def reserve(self, mode, minimum_capacity, alignment):
        assert minimum_capacity > 0
        assert alignment > 0 and alignment & (alignment - 1) == 0
        if self.decline_direct and mode is ReserveMode.DIRECT:
            return BackingStatus.DIRECT_UNAVAILABLE
        return BackingStatus.OK

    def write(self, offset, source):
        assert offset >= 0
        assert source
        self.writes += 1
        return (
            BackingStatus.ALLOCATION_FAILED if self.fail_write else BackingStatus.OK
        )

    def commit(self, committed):
        assert committed
        self.commits += 1
        if self.raise_commit:
            raise RuntimeError("observer exceptions must stop at the C boundary")
        return BackingStatus.COMMIT_FAILED if self.fail_commit else BackingStatus.OK

    def rollback(self):
        self.rollbacks += 1
        return BackingStatus.OK

    def release(self):
        self.releases += 1


class OverlapObserver:
    def __init__(self) -> None:
        self.condition = threading.Condition()
        self.calls = 0
        self.active = 0
        self.max_active = 0

    def reserve(self, _mode, _minimum_capacity, _alignment):
        with self.condition:
            self.calls += 1
            self.active += 1
            self.max_active = max(self.max_active, self.active)
            self.condition.notify_all()
            if self.calls == 1:
                self.condition.wait_for(lambda: self.calls >= 2, timeout=0.25)
            self.active -= 1
        return BackingStatus.OK

    def wait_for_first_reserve(self) -> None:
        with self.condition:
            assert self.condition.wait_for(lambda: self.calls >= 1, timeout=2)


def test_internal_heap_execution_and_payload_facts_are_core_owned():
    spec, plan = fixed_plan()
    info = plan.info()
    result = plan.execute(BuildPolicy.ALLOW_STAGING)
    assert result.report.mode is ExecutionMode.DIRECT
    assert result.report.fallback_reason is FallbackReason.NONE
    assert result.report.requested_bytes == info.total_bytes
    assert result.report.used_bytes == info.total_bytes
    assert result.report.staging_bytes == 0
    assert result.report.region_count == info.region_count
    assert result.report.backing_capacity >= info.total_bytes
    assert result.payload.profile() is Profile.RECORD_V1
    assert result.payload.execution_report() == result.report
    assert len(result.payload.sha256()) == 32
    assert len(result.payload.binary_bytes()) == info.total_bytes

    clone = result.payload.clone()
    result.payload.close()
    assert len(clone.binary_bytes()) == info.total_bytes
    clone.close()
    plan.close()
    spec.close()


def test_caller_backing_falls_back_truthfully_and_direct_required_is_exact():
    spec, plan = fixed_plan()
    direct_observer = Observer()
    direct = plan.execute(
        BuildPolicy.ALLOW_STAGING, MemoryBacking(direct_observer)
    )
    assert direct.report.mode is ExecutionMode.DIRECT
    assert direct.report.fallback_reason is FallbackReason.NONE
    assert direct.report.staging_bytes == 0
    assert direct_observer.writes == 0
    assert direct_observer.commits == 1
    direct.payload.close()
    assert direct_observer.releases == 1

    observer = Observer(decline_direct=True)
    backing = MemoryBacking(observer)
    result = plan.execute(BuildPolicy.ALLOW_STAGING, backing)
    assert result.report.mode is ExecutionMode.STAGED
    assert result.report.fallback_reason is FallbackReason.BACKING_DECLINED_DIRECT
    assert result.report.staging_bytes == result.report.used_bytes
    assert observer.writes > 0
    assert observer.commits == 1

    with pytest.raises(PayloadError) as raised:
        plan.execute(BuildPolicy.REQUIRE_DIRECT, backing)
    assert raised.value.code == 2007
    assert raised.value.symbol == "DIRECT_UNAVAILABLE"
    assert raised.value.path == "/backing"
    assert raised.value.details_json == '{"reason":"backing_declined_direct"}'

    result.payload.close()
    assert observer.releases == 1
    plan.close()
    spec.close()


@pytest.mark.parametrize(
    ("observer", "expected"),
    [
        (Observer(decline_direct=True, fail_write=True), 5002),
        (Observer(fail_commit=True), 5003),
        (Observer(raise_commit=True), 5001),
    ],
)
def test_callback_failures_rollback_once_without_unwinding(observer, expected):
    spec, plan = fixed_plan()
    backing = MemoryBacking(observer)
    with pytest.raises(PayloadError) as raised:
        plan.execute(BuildPolicy.ALLOW_STAGING, backing)
    assert raised.value.code == expected
    assert observer.rollbacks == 1
    assert observer.releases == 0
    plan.close()
    spec.close()


def test_copied_and_external_open_have_explicit_independent_lifetimes():
    spec, plan = fixed_plan()
    built = plan.execute(BuildPolicy.ALLOW_STAGING)
    original = built.payload.binary_bytes()

    source = bytearray(original)
    copied = Payload.open_copy(spec, source, OpenOptions())
    source[:] = b"\xff" * len(source)
    assert copied.binary_bytes() == original

    external = ExternalBytes(bytearray(original))
    opened = Payload.open_external(spec, external, OpenOptions())
    del external
    gc.collect()
    assert opened.binary_bytes() == original
    with pytest.raises(PayloadError) as raised:
        opened.execution_report()
    assert raised.value.code == 2008
    assert (
        raised.value.details_json
        == '{"reason":"opened_payload_has_no_execution_report"}'
    )

    opened.close()
    copied.close()
    built.payload.close()
    plan.close()
    spec.close()


def test_shared_safe_backing_serializes_complete_plan_executions():
    spec, plan = fixed_plan()
    observer = OverlapObserver()
    backing = MemoryBacking(observer)
    results = []
    failures = []

    def execute():
        try:
            results.append(plan.execute(BuildPolicy.ALLOW_STAGING, backing))
        except BaseException as exc:
            failures.append(exc)

    first = threading.Thread(target=execute)
    first.start()
    observer.wait_for_first_reserve()
    second = threading.Thread(target=execute)
    second.start()
    first.join(timeout=2)
    second.join(timeout=2)

    assert not first.is_alive()
    assert not second.is_alive()
    assert failures == []
    assert observer.max_active == 1
    for result in results:
        result.payload.close()
    plan.close()
    spec.close()


def test_backing_reports_unrepresentable_reservation_as_allocation_failure():
    backing = MemoryBacking()
    owner_token = ctypes.c_void_p()
    writable = ctypes.POINTER(ctypes.c_uint8)()
    capacity = ctypes.c_uint64()

    status = backing._reserve(
        0,
        int(ReserveMode.DIRECT),
        (1 << 64) - 1,
        8,
        ctypes.pointer(owner_token),
        ctypes.pointer(writable),
        ctypes.pointer(capacity),
    )

    assert status == int(BackingStatus.ALLOCATION_FAILED)


def test_payload_handles_cannot_be_forged_from_python():
    with pytest.raises(TypeError, match="created only"):
        Payload(1, object())
