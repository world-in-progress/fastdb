"""Compile/query projection backed only by the FastDB payload C ABI."""

from __future__ import annotations

import ctypes
from contextlib import contextmanager
from dataclasses import dataclass
from enum import IntEnum
import threading
from typing import TYPE_CHECKING, Callable, Iterator, Optional, Type, TypeVar

from . import _ffi
from ._error import PayloadError, binding_error

if TYPE_CHECKING:
    from ._codegen import ArtifactSet, CodegenOptions, CodegenTarget


class Profile(IntEnum):
    RECORD_V1 = 1
    OBJECT_GRAPH_V1 = 2


@dataclass(frozen=True)
class Capabilities:
    profile: Profile
    semantic_flags: int
    operation_flags: int
    codegen_target_flags: int
    direct_build_status: int


_CompiledSpecT = TypeVar("_CompiledSpecT", bound="CompiledSpec")
_HANDLE_TOKEN = object()


def _copy_error_field(
    handle: _ffi.Handle,
    accessor: Callable[..., None],
) -> str:
    data = _ffi.BytePointer()
    size = ctypes.c_uint64(0)
    accessor(handle, ctypes.byref(data), ctypes.byref(size))
    if size.value == 0:
        return ""
    if not bool(data):
        return "<FastDB error field has null storage>"
    return ctypes.string_at(data, size.value).decode("utf-8", errors="replace")


def _owned_error(status: int, handle: _ffi.Handle) -> PayloadError:
    if not handle.value:
        return PayloadError(
            status or 9001,
            "BINDING_CONTRACT",
            "",
            "FastDB Core returned a failure without an owned error",
            '{"reason":"missing_error_handle"}',
        )
    native = _ffi.library()
    try:
        return PayloadError(
            int(native.fdb_payload_v1_error_code(handle)),
            _copy_error_field(handle, native.fdb_payload_v1_error_symbol),
            _copy_error_field(handle, native.fdb_payload_v1_error_path),
            _copy_error_field(handle, native.fdb_payload_v1_error_message),
            _copy_error_field(handle, native.fdb_payload_v1_error_details_json),
        )
    finally:
        native.fdb_payload_v1_error_release(handle)


def _check_status(status: int, error: _ffi.Handle) -> None:
    if status != 0:
        raise _owned_error(status, error)
    if error.value:
        _ffi.library().fdb_payload_v1_error_release(error)
        raise binding_error(
            "FastDB Core returned success with an error handle",
            reason="unexpected_error_handle",
        )


def _input_bytes(value: bytes) -> tuple[object, _ffi.BytePointer, int]:
    copied = bytes(value)
    if not copied:
        return copied, _ffi.BytePointer(), 0
    storage = (ctypes.c_uint8 * len(copied)).from_buffer_copy(copied)
    return storage, ctypes.cast(storage, _ffi.BytePointer), len(copied)


def _input_digest(value: bytes) -> tuple[object, _ffi.BytePointer]:
    storage, pointer, size = _input_bytes(value)
    if size != _ffi.SHA256_SIZE:
        raise ValueError(f"expected_sha256 must contain {_ffi.SHA256_SIZE} bytes")
    return storage, pointer


def _checked_u32(value: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer")
    if value < 0 or value > 0xFFFF_FFFF:
        raise ValueError(f"{name} must be an unsigned 32-bit integer")
    return value


class CompiledSpec:
    """An owned reference to an immutable, thread-safe Core compiled spec."""

    def __init__(self, handle: _ffi.Handle, token: object = None) -> None:
        if token is not _HANDLE_TOKEN:
            raise TypeError("CompiledSpec handles are created only by fastdb4py.payload")
        if not handle.value:
            raise binding_error(
                "FastDB Core returned success without a compiled spec",
                reason="missing_spec_handle",
            )
        self._lock = threading.Lock()
        self._handle: Optional[_ffi.Handle] = handle

    @classmethod
    def _from_handle(cls: Type[_CompiledSpecT], handle: _ffi.Handle) -> _CompiledSpecT:
        try:
            return cls(handle, _HANDLE_TOKEN)
        except BaseException:
            if handle.value:
                _ffi.library().fdb_payload_v1_spec_release(handle)
            raise

    @classmethod
    def compile(cls: Type[_CompiledSpecT], source: bytes) -> _CompiledSpecT:
        native = _ffi.library()
        storage, source_pointer, source_size = _input_bytes(source)
        handle = _ffi.Handle()
        error = _ffi.Handle()
        status = int(
            native.fdb_payload_v1_spec_compile_json(
                source_pointer,
                source_size,
                None,
                ctypes.byref(handle),
                ctypes.byref(error),
            )
        )
        del storage
        _check_status(status, error)
        return cls._from_handle(handle)

    def close(self) -> None:
        with self._lock:
            handle = self._handle
            self._handle = None
        if handle is not None:
            _ffi.library().fdb_payload_v1_spec_release(handle)

    def __enter__(self: _CompiledSpecT) -> _CompiledSpecT:
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

    def clone(self: _CompiledSpecT) -> _CompiledSpecT:
        with self._lock:
            handle = self._require_handle_locked()
            _ffi.library().fdb_payload_v1_spec_retain(handle)
            cloned_handle = _ffi.Handle(handle.value)
        return type(self)._from_handle(cloned_handle)

    def __copy__(self: _CompiledSpecT) -> _CompiledSpecT:
        return self.clone()

    def __deepcopy__(self: _CompiledSpecT, memo: object) -> _CompiledSpecT:
        return self.clone()

    def canonical_json(self) -> bytes:
        return self._query_blob("fdb_payload_v1_spec_canonical_json")

    def manifest_json(self) -> bytes:
        return self._query_blob("fdb_payload_v1_spec_manifest_json")

    def sha256(self) -> bytes:
        native = _ffi.library()
        digest = (ctypes.c_uint8 * _ffi.SHA256_SIZE)()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_spec_sha256(
                    handle, digest, ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return bytes(digest)

    def profile(self) -> Profile:
        native = _ffi.library()
        value = ctypes.c_uint32(0)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_spec_profile(
                    handle, ctypes.byref(value), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        try:
            return Profile(value.value)
        except ValueError as exc:
            raise binding_error(
                "FastDB Core returned an unknown payload profile",
                path="/profile",
                reason="unknown_profile",
            ) from exc

    def capabilities(self) -> Capabilities:
        native = _ffi.library()
        raw = _ffi.CapabilitiesV1()
        native.fdb_payload_v1_capabilities_init(ctypes.byref(raw))
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_spec_capabilities(
                    handle, ctypes.byref(raw), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        try:
            profile = Profile(raw.profile)
        except ValueError as exc:
            raise binding_error(
                "FastDB Core returned an unknown payload profile",
                path="/capabilities/profile",
                reason="unknown_profile",
            ) from exc
        return Capabilities(
            profile=profile,
            semantic_flags=int(raw.semantic_flags),
            operation_flags=int(raw.operation_flags),
            codegen_target_flags=int(raw.codegen_target_flags),
            direct_build_status=int(raw.direct_build_status),
        )

    def generate(
        self,
        target: "CodegenTarget",
        options: Optional["CodegenOptions"] = None,
    ) -> "ArtifactSet":
        from ._codegen import generate

        return generate(self, target, options)

    def entry_count(self) -> int:
        return self._query_count("fdb_payload_v1_spec_entry_count")

    def entry_id(self, index: int) -> str:
        return self._query_indexed_id(
            "fdb_payload_v1_spec_entry_id", _checked_u32(index, "entry_index")
        )

    def entry_index(self, identifier: str) -> int:
        return self._query_named_index(
            "fdb_payload_v1_spec_entry_index", identifier
        )

    def component_count(self) -> int:
        return self._query_count("fdb_payload_v1_spec_component_count")

    def component_id(self, index: int) -> str:
        return self._query_indexed_id(
            "fdb_payload_v1_spec_component_id",
            _checked_u32(index, "component_index"),
        )

    def component_index(self, identifier: str) -> int:
        return self._query_named_index(
            "fdb_payload_v1_spec_component_index", identifier
        )

    def component_field_count(self, component_index: int) -> int:
        native = _ffi.library()
        value = ctypes.c_uint32(0)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_spec_component_field_count(
                    handle,
                    _checked_u32(component_index, "component_index"),
                    ctypes.byref(value),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
        return int(value.value)

    def component_field_id(self, component_index: int, field_index: int) -> str:
        native = _ffi.library()
        blob = _ffi.Handle()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_spec_component_field_id(
                    handle,
                    _checked_u32(component_index, "component_index"),
                    _checked_u32(field_index, "field_index"),
                    ctypes.byref(blob),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
            return self._copy_blob(blob).decode("utf-8")

    def component_field_index(self, component_index: int, identifier: str) -> int:
        native = _ffi.library()
        storage, data, size = _input_bytes(identifier.encode("utf-8"))
        value = ctypes.c_uint32(0)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_spec_component_field_index(
                    handle,
                    _checked_u32(component_index, "component_index"),
                    data,
                    size,
                    ctypes.byref(value),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
        del storage
        return int(value.value)

    def _require_handle_locked(self) -> _ffi.Handle:
        if self._handle is None:
            raise binding_error(
                "CompiledSpec is closed",
                path="/spec",
                reason="closed_handle",
            )
        return self._handle

    @contextmanager
    def _borrow_handle(self) -> Iterator[_ffi.Handle]:
        native = _ffi.library()
        with self._lock:
            handle = self._require_handle_locked()
            native.fdb_payload_v1_spec_retain(handle)
            borrowed = _ffi.Handle(handle.value)
        try:
            yield borrowed
        finally:
            native.fdb_payload_v1_spec_release(borrowed)

    def _query_blob(self, function_name: str) -> bytes:
        native = _ffi.library()
        blob = _ffi.Handle()
        error = _ffi.Handle()
        function = getattr(native, function_name)
        with self._borrow_handle() as handle:
            status = int(function(handle, ctypes.byref(blob), ctypes.byref(error)))
            _check_status(status, error)
            return self._copy_blob(blob)

    @staticmethod
    def _copy_blob(blob: _ffi.Handle) -> bytes:
        if not blob.value:
            raise binding_error(
                "FastDB Core returned success without a blob",
                reason="missing_blob_handle",
            )
        native = _ffi.library()
        try:
            size = int(native.fdb_payload_v1_blob_size(blob))
            data = native.fdb_payload_v1_blob_data(blob)
            if size == 0:
                return b""
            if not bool(data):
                raise binding_error(
                    "FastDB Core returned a non-empty blob with null storage",
                    reason="null_blob_storage",
                )
            return bytes(ctypes.string_at(data, size))
        finally:
            native.fdb_payload_v1_blob_release(blob)

    def _query_count(self, function_name: str) -> int:
        native = _ffi.library()
        value = ctypes.c_uint32(0)
        error = _ffi.Handle()
        function = getattr(native, function_name)
        with self._borrow_handle() as handle:
            status = int(
                function(handle, ctypes.byref(value), ctypes.byref(error))
            )
            _check_status(status, error)
        return int(value.value)

    def _query_indexed_id(self, function_name: str, index: int) -> str:
        native = _ffi.library()
        blob = _ffi.Handle()
        error = _ffi.Handle()
        function = getattr(native, function_name)
        with self._borrow_handle() as handle:
            status = int(
                function(
                    handle,
                    index,
                    ctypes.byref(blob),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
            return self._copy_blob(blob).decode("utf-8")

    def _query_named_index(self, function_name: str, identifier: str) -> int:
        native = _ffi.library()
        storage, data, size = _input_bytes(identifier.encode("utf-8"))
        value = ctypes.c_uint32(0)
        error = _ffi.Handle()
        function = getattr(native, function_name)
        with self._borrow_handle() as handle:
            status = int(
                function(
                    handle,
                    data,
                    size,
                    ctypes.byref(value),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
        del storage
        return int(value.value)
