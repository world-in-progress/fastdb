"""Payload execution, backing, and open projection over the FastDB Core ABI."""

from __future__ import annotations

import ctypes
from contextlib import contextmanager, nullcontext
from dataclasses import dataclass
from enum import IntEnum
import threading
from typing import Iterator, Optional, Type, TypeVar

from . import _ffi
from ._builder import BuildPlan
from ._error import PayloadError, binding_error
from ._spec import CompiledSpec, Profile, _check_status, _input_bytes


class BackingStatus(IntEnum):
    OK = 0
    DIRECT_UNAVAILABLE = 2007
    BACKING_CONTRACT = 5001
    ALLOCATION_FAILED = 5002
    COMMIT_FAILED = 5003
    ROLLBACK_FAILED = 5004


class BuildPolicy(IntEnum):
    ALLOW_STAGING = 1
    REQUIRE_DIRECT = 2


class ReserveMode(IntEnum):
    DIRECT = 1
    STAGED = 2


class ExecutionMode(IntEnum):
    DIRECT = 1
    STAGED = 2


class FallbackReason(IntEnum):
    NONE = 0
    PLAN_REQUIRES_STAGING = 1
    BACKING_DECLINED_DIRECT = 2


@dataclass(frozen=True)
class ExecutionReport:
    mode: ExecutionMode
    fallback_reason: FallbackReason
    requested_bytes: int
    used_bytes: int
    staging_bytes: int
    region_count: int
    backing_capacity: int


@dataclass(frozen=True)
class OpenOptions:
    """Optional overrides applied to Core-owned hardened reader defaults."""

    flags: Optional[int] = None
    max_total_bytes: Optional[int] = None
    max_regions: Optional[int] = None
    max_entries: Optional[int] = None
    max_components: Optional[int] = None
    max_nesting_depth: Optional[int] = None
    max_list_elements: Optional[int] = None
    max_graph_objects: Optional[int] = None
    max_string_bytes: Optional[int] = None
    max_validation_work: Optional[int] = None

    def _to_ffi(self) -> _ffi.OpenOptionsV1:
        native = _ffi.library()
        raw = _ffi.OpenOptionsV1()
        native.fdb_payload_v1_open_options_init(ctypes.byref(raw))
        if self.flags is not None:
            raw.flags = _checked_unsigned(self.flags, 0xFFFF_FFFF, "flags")
        for name in (
            "max_total_bytes",
            "max_regions",
            "max_entries",
            "max_components",
            "max_nesting_depth",
            "max_list_elements",
            "max_graph_objects",
            "max_string_bytes",
            "max_validation_work",
        ):
            value = getattr(self, name)
            if value is not None:
                setattr(raw, name, _checked_unsigned(value, (1 << 64) - 1, name))
        return raw


@dataclass
class _Reservation:
    storage: bytearray
    pin: object
    address: int
    capacity: int
    references: int = 1


class _NoopObserver:
    def reserve(self, mode: ReserveMode, capacity: int, alignment: int) -> BackingStatus:
        return BackingStatus.OK

    def write(self, offset: int, source: bytes) -> BackingStatus:
        return BackingStatus.OK

    def commit(self, committed: bytes) -> BackingStatus:
        return BackingStatus.OK

    def rollback(self) -> BackingStatus:
        return BackingStatus.OK

    def release(self) -> None:
        return None


class MemoryBacking:
    """Binding-owned aligned memory exposed through the Core backing contract.

    Executions sharing this instance are serialized. Observer callbacks must
    not re-enter an execution using the same backing.
    """

    def __init__(self, observer: object = None) -> None:
        self._observer = observer if observer is not None else _NoopObserver()
        self._execution_lock = threading.Lock()
        self._lock = threading.Lock()
        self._tokens: dict[int, _Reservation] = {}
        self._next_token = 1
        self._reserve_callback = _ffi.BackingReserveCallback(self._reserve)
        self._write_callback = _ffi.BackingWriteCallback(self._write)
        self._commit_callback = _ffi.BackingCommitCallback(self._commit)
        self._rollback_callback = _ffi.BackingRollbackCallback(self._rollback)
        self._retain_callback = _ffi.BackingRetainCallback(self._retain)
        self._release_callback = _ffi.BackingReleaseCallback(self._release)

    def _raw(self) -> _ffi.BackingV1:
        raw = _ffi.BackingV1()
        _ffi.library().fdb_payload_v1_backing_init(ctypes.byref(raw))
        raw.context = id(self)
        raw.reserve = self._reserve_callback
        raw.write = self._write_callback
        raw.commit = self._commit_callback
        raw.rollback = self._rollback_callback
        raw.retain = self._retain_callback
        raw.release = self._release_callback
        return raw

    def _reserve(
        self,
        _context: int,
        reserve_mode: int,
        minimum_capacity: int,
        alignment: int,
        out_owner_token: _ffi.VoidPointerPointer,
        out_writable_data: _ffi.BytePointerPointer,
        out_capacity: ctypes.POINTER(ctypes.c_uint64),
    ) -> int:
        try:
            mode = ReserveMode(reserve_mode)
            if alignment <= 0 or alignment & (alignment - 1):
                return int(BackingStatus.BACKING_CONTRACT)
            status = _observer_status(
                self._observer,
                "reserve",
                mode,
                int(minimum_capacity),
                int(alignment),
            )
            if status is not BackingStatus.OK:
                return int(status)
            capacity = int(minimum_capacity)
            try:
                storage = bytearray(max(capacity, 1) + alignment - 1)
                pin_type = ctypes.c_uint8 * len(storage)
                pin = pin_type.from_buffer(storage)
                base = ctypes.addressof(pin)
            except (MemoryError, OverflowError):
                return int(BackingStatus.ALLOCATION_FAILED)
            address = (base + alignment - 1) & ~(alignment - 1)
            reservation = _Reservation(storage, pin, address, capacity)
            with self._lock:
                token = self._next_token
                self._next_token += 1
                self._tokens[token] = reservation
            out_owner_token[0] = token
            out_writable_data[0] = (
                ctypes.cast(address, _ffi.BytePointer)
                if mode is ReserveMode.DIRECT
                else _ffi.BytePointer()
            )
            out_capacity[0] = capacity
            return int(BackingStatus.OK)
        except BaseException:
            return int(BackingStatus.BACKING_CONTRACT)

    def _write(
        self,
        _context: int,
        owner_token: int,
        offset: int,
        source: _ffi.BytePointer,
        source_size: int,
    ) -> int:
        try:
            reservation = self._reservation(owner_token)
            offset = int(offset)
            size = int(source_size)
            if offset < 0 or size < 0 or offset + size > reservation.capacity:
                return int(BackingStatus.BACKING_CONTRACT)
            if size and not bool(source):
                return int(BackingStatus.BACKING_CONTRACT)
            copied = bytes(ctypes.string_at(source, size)) if size else b""
            status = _observer_status(self._observer, "write", offset, copied)
            if status is not BackingStatus.OK:
                return int(status)
            if size:
                ctypes.memmove(reservation.address + offset, copied, size)
            return int(BackingStatus.OK)
        except BaseException:
            return int(BackingStatus.BACKING_CONTRACT)

    def _commit(
        self,
        _context: int,
        owner_token: int,
        used_size: int,
        out_readable_data: _ffi.BytePointerPointer,
        out_readable_size: ctypes.POINTER(ctypes.c_uint64),
    ) -> int:
        try:
            reservation = self._reservation(owner_token)
            size = int(used_size)
            if size < 0 or size > reservation.capacity:
                return int(BackingStatus.BACKING_CONTRACT)
            committed = bytes(ctypes.string_at(reservation.address, size))
            status = _observer_status(self._observer, "commit", committed)
            if status is not BackingStatus.OK:
                return int(status)
            out_readable_data[0] = ctypes.cast(
                reservation.address, _ffi.BytePointer
            )
            out_readable_size[0] = size
            return int(BackingStatus.OK)
        except BaseException:
            return int(BackingStatus.BACKING_CONTRACT)

    def _rollback(self, _context: int, owner_token: int) -> int:
        try:
            token = _pointer_value(owner_token)
            with self._lock:
                if self._tokens.pop(token, None) is None:
                    return int(BackingStatus.BACKING_CONTRACT)
            return int(_observer_status(self._observer, "rollback"))
        except BaseException:
            return int(BackingStatus.BACKING_CONTRACT)

    def _retain(self, _context: int, owner_token: int) -> int:
        try:
            token = _pointer_value(owner_token)
            with self._lock:
                reservation = self._tokens.get(token)
                if reservation is None:
                    return int(BackingStatus.BACKING_CONTRACT)
                reservation.references += 1
            return int(BackingStatus.OK)
        except BaseException:
            return int(BackingStatus.BACKING_CONTRACT)

    def _release(self, _context: int, owner_token: int) -> None:
        try:
            token = _pointer_value(owner_token)
            released = False
            with self._lock:
                reservation = self._tokens.get(token)
                if reservation is None or reservation.references <= 0:
                    return
                reservation.references -= 1
                if reservation.references == 0:
                    del self._tokens[token]
                    released = True
            if released:
                callback = getattr(self._observer, "release", None)
                if callback is not None:
                    callback()
        except BaseException:
            return

    def _reservation(self, owner_token: int) -> _Reservation:
        token = _pointer_value(owner_token)
        with self._lock:
            reservation = self._tokens.get(token)
        if reservation is None:
            raise KeyError("unknown backing owner token")
        return reservation


class ExternalBytes:
    """An immutable binding-owned byte image suitable for external open."""

    def __init__(self, source: bytes) -> None:
        copied = bytes(source)
        self._size = len(copied)
        pin_type = ctypes.c_uint8 * max(self._size, 1)
        self._pin = pin_type()
        if copied:
            ctypes.memmove(ctypes.addressof(self._pin), copied, self._size)
        self._address = ctypes.addressof(self._pin)
        self._lock = threading.Lock()
        self._references = 0
        self._retain_callback = _ffi.BackingRetainCallback(self._retain)
        self._release_callback = _ffi.BackingReleaseCallback(self._release)

    def _raw(self) -> _ffi.BackingV1:
        raw = _ffi.BackingV1()
        _ffi.library().fdb_payload_v1_backing_init(ctypes.byref(raw))
        raw.context = id(self)
        raw.retain = self._retain_callback
        raw.release = self._release_callback
        return raw

    def _pointer(self) -> _ffi.BytePointer:
        if self._size == 0:
            return _ffi.BytePointer()
        return ctypes.cast(self._address, _ffi.BytePointer)

    def _retain(self, _context: int, owner_token: int) -> int:
        try:
            if _pointer_value(owner_token) != 1:
                return int(BackingStatus.BACKING_CONTRACT)
            with self._lock:
                self._references += 1
            return int(BackingStatus.OK)
        except BaseException:
            return int(BackingStatus.BACKING_CONTRACT)

    def _release(self, _context: int, owner_token: int) -> None:
        try:
            if _pointer_value(owner_token) != 1:
                return
            with self._lock:
                if self._references > 0:
                    self._references -= 1
        except BaseException:
            return


_PayloadT = TypeVar("_PayloadT", bound="Payload")
_PAYLOAD_TOKEN = object()


class Payload:
    """Owned immutable Core payload, optionally retaining its backing adapter."""

    def __init__(self, handle: _ffi.Handle, token: object, keeper: object = None) -> None:
        if token is not _PAYLOAD_TOKEN:
            raise TypeError("Payload handles are created only by fastdb4py.payload")
        if not handle.value:
            raise binding_error(
                "FastDB Core returned success without a payload",
                reason="missing_payload_handle",
            )
        self._lock = threading.Lock()
        self._handle: Optional[_ffi.Handle] = handle
        self._keeper = keeper

    @classmethod
    def _from_handle(
        cls: Type[_PayloadT], handle: _ffi.Handle, keeper: object = None
    ) -> _PayloadT:
        return cls(handle, _PAYLOAD_TOKEN, keeper)

    @classmethod
    def open_copy(
        cls: Type[_PayloadT],
        spec: CompiledSpec,
        source: bytes,
        options: Optional[OpenOptions] = None,
    ) -> _PayloadT:
        native = _ffi.library()
        storage, pointer, size = _input_bytes(bytes(source))
        raw_options = (options or OpenOptions())._to_ffi()
        handle = _ffi.Handle()
        error = _ffi.Handle()
        with spec._borrow_handle() as spec_handle:
            status = int(
                native.fdb_payload_v1_payload_open_copy(
                    spec_handle,
                    pointer,
                    size,
                    ctypes.byref(raw_options),
                    ctypes.byref(handle),
                    ctypes.byref(error),
                )
            )
        del storage
        _check_status(status, error)
        return cls._from_handle(handle)

    @classmethod
    def open_external(
        cls: Type[_PayloadT],
        spec: CompiledSpec,
        source: ExternalBytes,
        options: Optional[OpenOptions] = None,
    ) -> _PayloadT:
        if not isinstance(source, ExternalBytes):
            raise TypeError("source must be ExternalBytes")
        native = _ffi.library()
        backing = source._raw()
        raw_options = (options or OpenOptions())._to_ffi()
        handle = _ffi.Handle()
        error = _ffi.Handle()
        with spec._borrow_handle() as spec_handle:
            status = int(
                native.fdb_payload_v1_payload_open_external(
                    spec_handle,
                    source._pointer(),
                    source._size,
                    ctypes.byref(backing),
                    ctypes.c_void_p(1),
                    ctypes.byref(raw_options),
                    ctypes.byref(handle),
                    ctypes.byref(error),
                )
            )
        _check_status(status, error)
        return cls._from_handle(handle, source)

    def close(self) -> None:
        with self._lock:
            handle = self._handle
            keeper = self._keeper
            self._handle = None
            self._keeper = None
        if handle is not None:
            _ffi.library().fdb_payload_v1_payload_release(handle)
        del keeper

    def __enter__(self: _PayloadT) -> _PayloadT:
        with self._lock:
            self._require_handle_locked()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def clone(self: _PayloadT) -> _PayloadT:
        with self._lock:
            handle = self._require_handle_locked()
            _ffi.library().fdb_payload_v1_payload_retain(handle)
            clone = _ffi.Handle(handle.value)
            keeper = self._keeper
        return type(self)._from_handle(clone, keeper)

    def sha256(self) -> bytes:
        native = _ffi.library()
        digest = (ctypes.c_uint8 * _ffi.SHA256_SIZE)()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_payload_sha256(
                    handle, digest, ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return bytes(digest)

    def profile(self) -> Profile:
        native = _ffi.library()
        raw = ctypes.c_uint32(0)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_payload_profile(
                    handle, ctypes.byref(raw), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        try:
            return Profile(raw.value)
        except ValueError as exc:
            raise binding_error(
                "FastDB Core returned an unknown payload profile",
                path="/profile",
                reason="unknown_profile",
            ) from exc

    def execution_report(self) -> ExecutionReport:
        native = _ffi.library()
        raw = _new_report()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_payload_execution_report(
                    handle, ctypes.byref(raw), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return _execution_report(raw)

    def binary_bytes(self) -> bytes:
        native = _ffi.library()
        blob = _ffi.Handle()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_payload_binary_blob(
                    handle, ctypes.byref(blob), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return CompiledSpec._copy_blob(blob)

    def _require_handle_locked(self) -> _ffi.Handle:
        if self._handle is None:
            raise binding_error(
                "Payload is closed", path="/payload", reason="closed_handle"
            )
        return self._handle

    @contextmanager
    def _borrow_handle(self) -> Iterator[_ffi.Handle]:
        native = _ffi.library()
        with self._lock:
            handle = self._require_handle_locked()
            native.fdb_payload_v1_payload_retain(handle)
            borrowed = _ffi.Handle(handle.value)
        try:
            yield borrowed
        finally:
            native.fdb_payload_v1_payload_release(borrowed)


@dataclass(frozen=True)
class BuildResult:
    payload: Payload
    report: ExecutionReport


def execute_plan(
    plan: BuildPlan,
    policy: BuildPolicy,
    backing: Optional[MemoryBacking] = None,
) -> BuildResult:
    if not isinstance(policy, BuildPolicy):
        raise TypeError("policy must be BuildPolicy")
    if backing is not None and not isinstance(backing, MemoryBacking):
        raise TypeError("backing must be MemoryBacking or None")
    native = _ffi.library()
    report = _new_report()
    handle = _ffi.Handle()
    error = _ffi.Handle()
    execution = backing._execution_lock if backing is not None else nullcontext()
    with execution:
        raw_backing = backing._raw() if backing is not None else None
        with plan._borrow_handle() as plan_handle:
            status = int(
                native.fdb_payload_v1_plan_execute(
                    plan_handle,
                    int(policy),
                    ctypes.byref(raw_backing) if raw_backing is not None else None,
                    ctypes.byref(handle),
                    ctypes.byref(report),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
    payload = Payload._from_handle(handle, backing)
    try:
        projected = _execution_report(report)
    except BaseException:
        payload.close()
        raise
    return BuildResult(payload, projected)


def _new_report() -> _ffi.ExecutionReportV1:
    raw = _ffi.ExecutionReportV1()
    _ffi.library().fdb_payload_v1_execution_report_init(ctypes.byref(raw))
    return raw


def _execution_report(raw: _ffi.ExecutionReportV1) -> ExecutionReport:
    try:
        mode = ExecutionMode(raw.mode)
        fallback = FallbackReason(raw.fallback_reason)
    except ValueError as exc:
        raise binding_error(
            "FastDB Core returned an unknown execution report value",
            path="/execution_report",
            reason="unknown_report_value",
        ) from exc
    return ExecutionReport(
        mode,
        fallback,
        int(raw.requested_bytes),
        int(raw.used_bytes),
        int(raw.staging_bytes),
        int(raw.region_count),
        int(raw.backing_capacity),
    )


def _observer_status(observer: object, name: str, *args: object) -> BackingStatus:
    callback = getattr(observer, name, None)
    if callback is None:
        return BackingStatus.OK
    return BackingStatus(callback(*args))


def _pointer_value(value: object) -> int:
    if isinstance(value, int):
        return value
    raw = getattr(value, "value", None)
    return int(raw or 0)


def _checked_unsigned(value: int, maximum: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer")
    if value < 0 or value > maximum:
        raise ValueError(f"{name} is outside its unsigned ABI range")
    return value
