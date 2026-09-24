"""Payload execution, backing, and open projection over the FastDB Core ABI."""

from __future__ import annotations

import ctypes
from contextlib import contextmanager, nullcontext
from dataclasses import dataclass
from enum import IntEnum
import struct
import sys
import threading
from typing import Iterator, Optional, Type, TypeVar

from . import _ffi
from ._builder import BuildPlan
from ._error import PayloadError, binding_error
from ._spec import CompiledSpec, Profile, _check_status, _input_bytes, _input_digest


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

    def __copy__(self) -> MemoryBacking:
        raise TypeError("MemoryBacking owns mutable callback state and cannot be copied")

    def __deepcopy__(self, memo: object) -> MemoryBacking:
        raise TypeError("MemoryBacking owns mutable callback state and cannot be copied")

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

    def __copy__(self) -> ExternalBytes:
        return self

    def __deepcopy__(self, memo: object) -> ExternalBytes:
        return self

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


class ViewKind(IntEnum):
    SEQUENCE = 1
    BOOL = 2
    U8 = 3
    U16 = 4
    U32 = 5
    I32 = 6
    U8N = 7
    U16N = 8
    F32 = 9
    F64 = 10
    STR = 11
    WSTR = 12
    BYTES = 13
    COMPONENT = 14
    LIST = 15
    REF = 16


@dataclass(frozen=True)
class GraphIdentity:
    """Core identity coordinates scoped to one payload owner."""

    component_index: int
    object_id: int


_ACCESS_TOKEN = object()
_AccessT = TypeVar("_AccessT", bound="Access")


class Access:
    """Unique Core access pin whose safe methods return Python-owned copies."""

    def __init__(self, handle: _ffi.Handle, token: object) -> None:
        if token is not _ACCESS_TOKEN:
            raise TypeError("Access handles are created only by fastdb4py.payload")
        if not handle.value:
            raise binding_error(
                "FastDB Core returned success without an access pin",
                reason="missing_access_handle",
            )
        self._lock = threading.Lock()
        self._handle: Optional[_ffi.Handle] = handle

    @classmethod
    def _from_handle(cls: Type[_AccessT], handle: _ffi.Handle) -> _AccessT:
        try:
            return cls(handle, _ACCESS_TOKEN)
        except BaseException:
            if handle.value:
                _ffi.library().fdb_payload_v1_access_release(handle)
            raise

    def close(self) -> None:
        with self._lock:
            handle = self._handle
            self._handle = None
        if handle is not None:
            _ffi.library().fdb_payload_v1_access_release(handle)

    def __enter__(self: _AccessT) -> _AccessT:
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

    def __copy__(self) -> Access:
        raise TypeError("Access is unique and cannot be copied")

    def __deepcopy__(self, memo: object) -> Access:
        raise TypeError("Access is unique and cannot be copied")

    def payload_bytes(self) -> bytes:
        return self._copy_bytes("fdb_payload_v1_access_payload_bytes")

    def str(self) -> str:
        value = self._copy_bytes("fdb_payload_v1_access_str")
        try:
            return value.decode("utf-8", "strict")
        except UnicodeDecodeError as exc:
            raise binding_error(
                "FastDB Core returned invalid UTF-8 from a validated str view",
                path="/access",
                reason="invalid_core_utf8",
            ) from exc

    def wstr(self) -> str:
        native = _ffi.library()
        data = _ffi.U16Pointer()
        count = ctypes.c_uint64(0)
        error = _ffi.Handle()
        with self._lock:
            handle = self._require_handle_locked()
            status = int(
                native.fdb_payload_v1_access_wstr(
                    handle,
                    ctypes.byref(data),
                    ctypes.byref(count),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
            byte_count = _checked_span_size(int(count.value), 2, "/access/wstr")
            if byte_count and not bool(data):
                raise binding_error(
                    "FastDB Core returned non-empty wstr storage with a null pointer",
                    path="/access/wstr",
                    reason="invalid_core_wstr_span",
                )
            address = ctypes.cast(data, ctypes.c_void_p).value or 0
            if byte_count and address % ctypes.alignment(ctypes.c_uint16) != 0:
                raise binding_error(
                    "FastDB Core returned unaligned wstr storage",
                    path="/access/wstr",
                    reason="invalid_core_wstr_span",
                )
            copied = ctypes.string_at(data, byte_count) if byte_count else b""
        codec = "utf-16-le" if sys.byteorder == "little" else "utf-16-be"
        try:
            return copied.decode(codec, "strict")
        except UnicodeDecodeError as exc:
            raise binding_error(
                "FastDB Core returned invalid UTF-16 from a validated wstr view",
                path="/access/wstr",
                reason="invalid_core_utf16",
            ) from exc

    def bytes(self) -> bytes:
        return self._copy_bytes("fdb_payload_v1_access_bytes")

    def _copy_bytes(self, name: str) -> bytes:
        native = _ffi.library()
        data = _ffi.BytePointer()
        size = ctypes.c_uint64(0)
        error = _ffi.Handle()
        with self._lock:
            handle = self._require_handle_locked()
            status = int(
                getattr(native, name)(
                    handle,
                    ctypes.byref(data),
                    ctypes.byref(size),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
            native_size = _checked_span_size(int(size.value), 1, "/access")
            if native_size and not bool(data):
                raise binding_error(
                    "FastDB Core returned non-empty access storage with a null pointer",
                    path="/access",
                    reason="invalid_core_byte_span",
                )
            return ctypes.string_at(data, native_size) if native_size else b""

    def _require_handle_locked(self) -> _ffi.Handle:
        if self._handle is None:
            raise binding_error(
                "Access is closed", path="/access", reason="closed_handle"
            )
        return self._handle


_VIEW_TOKEN = object()
_ViewT = TypeVar("_ViewT", bound="View")


class View:
    """Retainable immutable Core view with generation-checked operations."""

    def __init__(self, handle: _ffi.Handle, token: object) -> None:
        if token is not _VIEW_TOKEN:
            raise TypeError("View handles are created only by fastdb4py.payload")
        if not handle.value:
            raise binding_error(
                "FastDB Core returned success without a view",
                reason="missing_view_handle",
            )
        self._lock = threading.Lock()
        self._handle: Optional[_ffi.Handle] = handle

    @classmethod
    def _from_handle(cls: Type[_ViewT], handle: _ffi.Handle) -> _ViewT:
        try:
            return cls(handle, _VIEW_TOKEN)
        except BaseException:
            if handle.value:
                _ffi.library().fdb_payload_v1_view_release(handle)
            raise

    def close(self) -> None:
        with self._lock:
            handle = self._handle
            self._handle = None
        if handle is not None:
            _ffi.library().fdb_payload_v1_view_release(handle)

    def __enter__(self: _ViewT) -> _ViewT:
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

    def clone(self: _ViewT) -> _ViewT:
        with self._lock:
            handle = self._require_handle_locked()
            _ffi.library().fdb_payload_v1_view_retain(handle)
            clone = _ffi.Handle(handle.value)
        return type(self)._from_handle(clone)

    def __copy__(self: _ViewT) -> _ViewT:
        return self.clone()

    def __deepcopy__(self: _ViewT, memo: object) -> _ViewT:
        return self.clone()

    def require_spec_sha256(self, expected_sha256: bytes) -> None:
        native = _ffi.library()
        storage, digest = _input_digest(expected_sha256)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_view_require_spec_sha256(
                    handle, digest, ctypes.byref(error)
                )
            )
            _check_status(status, error)
        del storage

    def kind(self) -> ViewKind:
        value = self._scalar("fdb_payload_v1_view_kind", ctypes.c_uint32)
        try:
            return ViewKind(value)
        except ValueError as exc:
            raise binding_error(
                "FastDB Core returned an unknown payload view kind",
                path="/view/kind",
                reason="unknown_view_kind",
            ) from exc

    def is_null(self) -> bool:
        return bool(self._scalar("fdb_payload_v1_view_is_null", ctypes.c_uint8))

    def length(self) -> int:
        return self._scalar("fdb_payload_v1_view_length", ctypes.c_uint64)

    def at(self: _ViewT, index: int) -> _ViewT:
        return self._child(
            "fdb_payload_v1_view_at",
            ctypes.c_uint64(_checked_unsigned(index, (1 << 64) - 1, "index")),
        )

    def component_index(self) -> int:
        return self._scalar(
            "fdb_payload_v1_view_component_index", ctypes.c_uint32
        )

    def field_count(self) -> int:
        return self._scalar("fdb_payload_v1_view_field_count", ctypes.c_uint32)

    def field(self: _ViewT, index: int) -> _ViewT:
        return self._child(
            "fdb_payload_v1_view_field",
            ctypes.c_uint32(_checked_unsigned(index, 0xFFFF_FFFF, "index")),
        )

    def ref_target(self: _ViewT) -> _ViewT:
        """Follow exactly one explicit Core ref edge."""
        native = _ffi.library()
        result = _ffi.Handle()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_view_ref_target(
                    handle, ctypes.byref(result), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return type(self)._from_handle(result)

    def graph_identity(self) -> GraphIdentity:
        """Return Core coordinates meaningful only within this payload."""
        native = _ffi.library()
        component_index = ctypes.c_uint32(0)
        object_id = ctypes.c_uint64(0)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_view_graph_identity(
                    handle,
                    ctypes.byref(component_index),
                    ctypes.byref(object_id),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
        return GraphIdentity(int(component_index.value), int(object_id.value))

    def get_bool(self) -> bool:
        return bool(self._scalar("fdb_payload_v1_view_get_bool", ctypes.c_uint8))

    def get_u8(self) -> int:
        return self._scalar("fdb_payload_v1_view_get_u8", ctypes.c_uint8)

    def get_u16(self) -> int:
        return self._scalar("fdb_payload_v1_view_get_u16", ctypes.c_uint16)

    def get_u32(self) -> int:
        return self._scalar("fdb_payload_v1_view_get_u32", ctypes.c_uint32)

    def get_i32(self) -> int:
        return self._scalar("fdb_payload_v1_view_get_i32", ctypes.c_int32)

    def get_u8n_f64_bits(self) -> int:
        return self._scalar(
            "fdb_payload_v1_view_get_u8n_f64_bits", ctypes.c_uint64
        )

    def get_u8n(self) -> float:
        return struct.unpack("=d", struct.pack("=Q", self.get_u8n_f64_bits()))[0]

    def get_u16n_f64_bits(self) -> int:
        return self._scalar(
            "fdb_payload_v1_view_get_u16n_f64_bits", ctypes.c_uint64
        )

    def get_u16n(self) -> float:
        return struct.unpack("=d", struct.pack("=Q", self.get_u16n_f64_bits()))[0]

    def get_f32_bits(self) -> int:
        return self._scalar("fdb_payload_v1_view_get_f32_bits", ctypes.c_uint32)

    def get_f32(self) -> float:
        return struct.unpack("=f", struct.pack("=I", self.get_f32_bits()))[0]

    def get_f64_bits(self) -> int:
        return self._scalar("fdb_payload_v1_view_get_f64_bits", ctypes.c_uint64)

    def get_f64(self) -> float:
        return struct.unpack("=d", struct.pack("=Q", self.get_f64_bits()))[0]

    def acquire(self) -> Access:
        native = _ffi.library()
        result = _ffi.Handle()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_view_acquire(
                    handle, ctypes.byref(result), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return Access._from_handle(result)

    def materialize(self: _ViewT) -> _ViewT:
        native = _ffi.library()
        result = _ffi.Handle()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_view_materialize(
                    handle, ctypes.byref(result), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return type(self)._from_handle(result)

    def _scalar(self, name: str, value_type: type[ctypes._SimpleCData]) -> int:
        native = _ffi.library()
        result = value_type(0)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                getattr(native, name)(
                    handle, ctypes.byref(result), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return int(result.value)

    def _child(self: _ViewT, name: str, index: object) -> _ViewT:
        native = _ffi.library()
        result = _ffi.Handle()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                getattr(native, name)(
                    handle, index, ctypes.byref(result), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return type(self)._from_handle(result)

    def _require_handle_locked(self) -> _ffi.Handle:
        if self._handle is None:
            raise binding_error("View is closed", path="/view", reason="closed_handle")
        return self._handle

    @contextmanager
    def _borrow_handle(self) -> Iterator[_ffi.Handle]:
        native = _ffi.library()
        with self._lock:
            handle = self._require_handle_locked()
            native.fdb_payload_v1_view_retain(handle)
            borrowed = _ffi.Handle(handle.value)
        try:
            yield borrowed
        finally:
            native.fdb_payload_v1_view_release(borrowed)


def _checked_span_size(count: int, width: int, path: str) -> int:
    if count < 0 or count > sys.maxsize // width:
        raise binding_error(
            "FastDB Core access span exceeds the host address space",
            path=path,
            reason="host_size_overflow",
        )
    return count * width


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
        try:
            return cls(handle, _PAYLOAD_TOKEN, keeper)
        except BaseException:
            if handle.value:
                _ffi.library().fdb_payload_v1_payload_release(handle)
            raise

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

    def __copy__(self: _PayloadT) -> _PayloadT:
        return self.clone()

    def __deepcopy__(self: _PayloadT, memo: object) -> _PayloadT:
        return self.clone()

    def require_spec_sha256(self, expected_sha256: bytes) -> None:
        native = _ffi.library()
        storage, digest = _input_digest(expected_sha256)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_payload_require_spec_sha256(
                    handle, digest, ctypes.byref(error)
                )
            )
            _check_status(status, error)
        del storage

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

    def acquire(self) -> Access:
        native = _ffi.library()
        result = _ffi.Handle()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_payload_acquire(
                    handle, ctypes.byref(result), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return Access._from_handle(result)

    def entry_view(self, entry_index: int) -> View:
        native = _ffi.library()
        result = _ffi.Handle()
        error = _ffi.Handle()
        entry_index = _checked_unsigned(entry_index, 0xFFFF_FFFF, "entry_index")
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_payload_entry_view(
                    handle,
                    entry_index,
                    ctypes.byref(result),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
        return View._from_handle(result)

    def invalidate(self) -> None:
        """Invalidate after all same-thread access pins have been closed.

        Core waits synchronously for every active access pin to drain, so a
        caller cannot keep an Access on this thread and release it after this
        method returns.
        """
        native = _ffi.library()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_payload_invalidate(
                    handle, ctypes.byref(error)
                )
            )
            _check_status(status, error)

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
