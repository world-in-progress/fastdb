import ctypes
import gc
import json
import mmap
import threading
import weakref

import numpy as np
import pytest

from fastdb4py.payload import (
    BuildPolicy,
    Builder,
    CompiledSpec,
    DirectBacking,
    ExecutionMode,
    ExternalBytes,
    FallbackReason,
    FixedRun,
    MemoryBacking,
    Payload,
    PayloadError,
    ReserveMode,
    WritableBufferBacking,
)


CONTENT = b"\x00\xff payload"
TEXT = "雪"


def compile_spec(profile, entries, components=()):
    return CompiledSpec.compile(json.dumps({
        "schema": "fastdb.payload.v1",
        "profile": profile,
        "entries": entries,
        "components": components,
    }).encode())


def entry(name, kind, **type_options):
    return {"id": name, "cardinality": "many", "type": {"kind": kind, **type_options}}


@pytest.fixture(params=["record.v1", "object_graph.v1"])
def image(request):
    graph = request.param == "object_graph.v1"
    fields = [
        {"id": "data", "type": {"kind": "bytes"}},
        {"id": "label", "type": {"kind": "str"}},
    ]
    entries = [entry("root", "component", id="Item")]
    components = [{"id": "Item", "kind": "record", "fields": fields}]
    with compile_spec(request.param, entries, components) as spec:
        with Builder.create(spec) as builder:
            if graph:
                node = builder.declare_object(0)
                builder.object_fill_begin(node).value_bytes(CONTENT).value_str(TEXT)
                builder.entry_begin(0, 1).value_object(node)
            else:
                builder.entry_begin(0, 1).value_component_begin()
                builder.value_bytes(CONTENT).value_str(TEXT)
            plan = builder.freeze()
        with plan, plan.execute(BuildPolicy.REQUIRE_DIRECT).payload as default:
            yield spec, plan, default.binary_bytes()


def address(buffer):
    return np.frombuffer(buffer, dtype=np.uint8).ctypes.data


def aligned_buffer(size, alignment):
    storage = bytearray(size + alignment - 1)
    offset = -address(storage) % alignment
    return memoryview(storage)[offset:offset + size]


def test_external_and_access_share_the_original_address(image):
    spec, _, binary = image
    source = np.frombuffer(binary, dtype=np.uint8)
    external = ExternalBytes(source)
    with Payload.open_external(spec, external) as payload, payload.memoryview() as whole:
        assert whole.readonly
        assert whole == binary
        assert address(source) == address(whole)
        with pytest.raises(TypeError):
            whole[0] = 0
        with pytest.raises(TypeError):
            (ctypes.c_char * 1).from_buffer(whole.obj)
        with payload.entry_view(0) as entries, entries.at(0) as row:
            for index, value in enumerate((CONTENT, TEXT.encode())):
                with row.field(index) as field, field.acquire() as access:
                    with access.memoryview() as span:
                        assert span.readonly
                        assert span == value
                        offset = binary.index(value)
                        assert address(span) == address(whole) + offset
                        assert offset + len(span) <= len(whole)


def test_external_freezes_writable_sources(image):
    spec, _, binary = image
    source = np.frombuffer(bytearray(binary), dtype=np.uint8)
    frozen = ExternalBytes(source)
    source[:] = 0
    with Payload.open_external(spec, frozen) as payload, payload.memoryview() as view:
        assert view == binary
        assert address(view) != address(source)


def test_last_derived_export_unpins_external_buffer(image):
    spec, _, binary = image
    storage = mmap.mmap(-1, len(binary))
    storage[:] = binary
    readonly = memoryview(storage).toreadonly()
    external = ExternalBytes(readonly)
    reference = weakref.ref(external)
    payload = Payload.open_external(spec, external)
    field = payload.entry_view(0).at(0).field(0)
    access = field.acquire()
    view = access.memoryview()
    exporter = view.obj
    array = np.frombuffer(view, dtype=np.uint8)

    del external, payload
    assert reference() is None
    gc.collect()
    with pytest.raises(BufferError):
        readonly.release()
    with field.acquire() as derived:
        assert derived.bytes() == CONTENT
    field.close()
    with pytest.raises(PayloadError, match="closed"):
        field.acquire()
    assert access.bytes() == CONTENT
    access.close()
    with pytest.raises(PayloadError, match="closed"):
        access.memoryview()
    assert view == CONTENT
    view.release()
    assert array.tobytes() == CONTENT
    with pytest.raises(BufferError):
        readonly.release()

    del array
    readonly.release()
    storage.close()
    with pytest.raises(BufferError, match="released"):
        memoryview(exporter)


def test_invalidation_drains_exports(image):
    spec, _, binary = image
    external = ExternalBytes(binary)
    with Payload.open_external(spec, external) as payload, payload.entry_view(0) as view:
        export = payload.memoryview()
        started, done = threading.Event(), threading.Event()

        def invalidate():
            started.set()
            payload.invalidate()
            done.set()

        thread = threading.Thread(target=invalidate, daemon=True)
        thread.start()
        try:
            assert started.wait(2)
            assert not done.wait(0.05)
            assert export == binary
        finally:
            export.release()
            thread.join(2)
        assert done.is_set()
        with pytest.raises(PayloadError) as caught:
            view.at(0)
        assert caught.value.symbol in {"VIEW_INVALIDATED", "STALE_GENERATION"}
        with pytest.raises(PayloadError):
            payload.memoryview()


def test_sync_buffer_calls_release_and_leave_no_ctypes_cycle(image):
    spec, _, binary = image
    opaque = b"\xa7" * 12_901
    tracked_types = (ctypes.c_uint8 * len(binary), ctypes.c_uint8 * len(opaque))

    def arrays():
        return {id(obj) for obj in gc.get_objects() if type(obj) in tracked_types}

    enabled = gc.isenabled()
    gc.disable()
    try:
        before = arrays()
        source = np.frombuffer(binary, dtype=np.uint8)
        reference = weakref.ref(source)
        with Payload.open_copy(spec, source) as payload:
            del source
            assert reference() is None
            assert payload.binary_bytes() == binary
        with compile_spec("record.v1", [entry("data", "bytes")]) as bytes_spec:
            with Builder.create(bytes_spec) as builder:
                source = np.frombuffer(opaque, dtype=np.uint8)
                reference = weakref.ref(source)
                builder.entry_begin(0, 1).value_bytes(source)
                del source
                assert reference() is None
        source = np.frombuffer(binary, dtype=np.uint8)
        reference = weakref.ref(source)
        external = ExternalBytes(source)
        del source
        assert reference() is not None
        del external
        assert reference() is None
        assert arrays() == before
    finally:
        if enabled:
            gc.enable()


def test_compile_and_fixed_run_consume_buffer_inputs_synchronously():
    with compile_spec("record.v1", [entry("values", "u16", nullable=True)]) as spec:
        source = np.frombuffer(spec.canonical_json(), dtype=np.uint8)
        with CompiledSpec.compile(source) as compiled:
            assert spec.sha256() == compiled.sha256()
        data = np.array([2, 3, 5], dtype=np.uint16)
        validity = bytearray(b"\x05")
        with Builder.create(spec) as builder:
            with memoryview(data) as values, memoryview(validity) as bits:
                builder.entry_begin(0, 3).value_fixed_run(FixedRun(values, 3, 2, bits))
            data[:] = 0
            validity[:] = b"\x00"
            with builder.freeze() as plan, plan.execute(BuildPolicy.REQUIRE_DIRECT).payload as payload:
                with payload.entry_view(0) as view:
                    assert view.at(0).get_u16() == 2
                    assert view.at(1).is_null()
                    assert view.at(2).get_u16() == 5
        discontiguous = source[::2]
        with pytest.raises((BufferError, ValueError)):
            CompiledSpec.compile(discontiguous)
        with pytest.raises((BufferError, ValueError)):
            ExternalBytes(discontiguous)


def assert_direct(result, buffer, binary):
    assert result.report.mode is ExecutionMode.DIRECT
    assert result.report.staging_bytes == 0
    with result.payload.memoryview() as exported:
        assert address(exported) == address(buffer)
        assert exported == binary


def assert_staged(result, binary):
    assert result.report.mode is ExecutionMode.STAGED
    assert result.report.fallback_reason is FallbackReason.BACKING_DECLINED_DIRECT
    assert result.report.staging_bytes == len(binary)
    with result.payload as payload, payload.memoryview() as view:
        assert view == binary


def test_direct_backing_reserve_commit_and_last_release(image):
    _, plan, binary = image
    info = plan.info()
    storage = mmap.mmap(-1, info.total_bytes)
    token = object()
    events = []

    def reserve(mode, capacity, alignment):
        events.append((mode, capacity, alignment))
        return storage, token

    def commit(owner, size):
        events.append(("commit", owner, size))
        return memoryview(storage).toreadonly()

    def release(owner):
        storage.close()
        events.append(("release", owner))

    backing = DirectBacking(reserve, commit, release=release)
    result = plan.execute(BuildPolicy.REQUIRE_DIRECT, backing)
    assert_direct(result, storage, binary)
    assert events == [
        (ReserveMode.DIRECT, info.total_bytes, info.max_alignment),
        ("commit", token, len(binary)),
    ]
    view = result.payload.entry_view(0)
    export = result.payload.memoryview()
    del backing
    result.payload.close()
    view.close()
    assert not storage.closed
    export.release()
    assert storage.closed
    assert events[-1] == ("release", token)


@pytest.mark.parametrize("reason", ["small", "unaligned", "used", "readonly", "strided"])
def test_writable_buffer_backing_fallback_matrix(image, reason):
    _, plan, binary = image
    info = plan.info()
    aligned = aligned_buffer(info.total_bytes * 2 + 1, info.max_alignment)
    candidate = {
        "small": aligned[:info.total_bytes - 1],
        "unaligned": aligned[1:info.total_bytes + 1],
        "used": aligned[:info.total_bytes],
        "readonly": aligned.toreadonly(),
        "strided": aligned[::2],
    }[reason]
    backing = WritableBufferBacking(candidate)
    if reason == "used":
        first = plan.execute(BuildPolicy.REQUIRE_DIRECT, backing)
        assert_direct(first, candidate, binary)
        first.payload.close()
    before = bytes(aligned)
    with pytest.raises(PayloadError) as caught:
        plan.execute(BuildPolicy.REQUIRE_DIRECT, backing)
    assert caught.value.symbol == "DIRECT_UNAVAILABLE"
    assert_staged(plan.execute(BuildPolicy.ALLOW_STAGING, backing), binary)
    assert bytes(aligned) == before


@pytest.mark.parametrize("failure", ["none", "raises", "small", "unaligned"])
def test_direct_backing_retries_with_staged_reservation(image, failure):
    _, plan, binary = image
    modes = []

    def reserve(mode, capacity, alignment):
        modes.append(mode)
        if mode is ReserveMode.STAGED:
            return aligned_buffer(capacity, alignment), None
        if failure == "none":
            return None
        if failure == "raises":
            raise RuntimeError("unavailable")
        if failure == "small":
            return aligned_buffer(capacity - 1, alignment), None
        return aligned_buffer(capacity + 1, alignment)[1:], None

    backing = DirectBacking(reserve)
    with pytest.raises(PayloadError) as caught:
        plan.execute(BuildPolicy.REQUIRE_DIRECT, backing)
    assert caught.value.symbol == "DIRECT_UNAVAILABLE"
    assert_staged(plan.execute(BuildPolicy.ALLOW_STAGING, backing), binary)
    assert modes == [ReserveMode.DIRECT, ReserveMode.DIRECT, ReserveMode.STAGED]


def test_direct_backing_rolls_back_commit_failure_once(image):
    _, plan, binary = image
    storage = mmap.mmap(-1, len(binary))
    token = object()
    events = []

    def fail_commit(owner, used_bytes):
        events.append(("commit", owner, used_bytes))
        raise ValueError("commit failed")

    def rollback(owner):
        storage.close()
        events.append(("rollback", owner))

    backing = DirectBacking(
        lambda *_: (storage, token),
        fail_commit,
        rollback,
        lambda owner: events.append(("release", owner)),
    )
    with pytest.raises(PayloadError) as caught:
        plan.execute(BuildPolicy.REQUIRE_DIRECT, backing)
    assert caught.value.symbol == "COMMIT_FAILED"
    assert storage.closed
    assert events == [("commit", token, len(binary)), ("rollback", token)]


def test_memory_backing_also_outlives_payload_wrapper(image):
    _, plan, binary = image
    payload = plan.execute(BuildPolicy.REQUIRE_DIRECT, MemoryBacking()).payload
    export = payload.memoryview()
    del payload
    gc.collect()
    assert export == binary
    export.release()
