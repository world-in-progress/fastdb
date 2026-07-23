"""Core-owned portable-payload codegen projection."""

from __future__ import annotations

import ctypes
from contextlib import contextmanager
from dataclasses import dataclass
from enum import IntEnum
import threading
from typing import Iterator, Optional, Type, TypeVar

from . import _ffi
from ._error import PayloadError, binding_error
from ._spec import CompiledSpec, _check_status


class CodegenTarget(IntEnum):
    CPP = 1 << 0
    RUST = 1 << 1
    PYTHON = 1 << 2
    TYPESCRIPT = 1 << 3


class ArtifactKind(IntEnum):
    SOURCE = 1


def _u64(value: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer")
    if value < 0 or value > (1 << 64) - 1:
        raise ValueError(f"{name} must be an unsigned 64-bit integer")
    return value


class CodegenOptions:
    def __init__(
        self,
        *,
        flags: Optional[int] = None,
        max_artifacts: Optional[int] = None,
        max_total_bytes: Optional[int] = None,
    ) -> None:
        raw = _ffi.CodegenOptionsV1()
        _ffi.library().fdb_payload_v1_codegen_options_init(ctypes.byref(raw))
        self.flags = int(raw.flags) if flags is None else _u64(flags, "flags")
        if self.flags > 0xFFFF_FFFF:
            raise ValueError("flags must be an unsigned 32-bit integer")
        self.max_artifacts = (
            int(raw.max_artifacts)
            if max_artifacts is None
            else _u64(max_artifacts, "max_artifacts")
        )
        self.max_total_bytes = (
            int(raw.max_total_bytes)
            if max_total_bytes is None
            else _u64(max_total_bytes, "max_total_bytes")
        )

    def _to_ffi(self) -> _ffi.CodegenOptionsV1:
        flags = _u64(self.flags, "flags")
        if flags > 0xFFFF_FFFF:
            raise ValueError("flags must be an unsigned 32-bit integer")
        max_artifacts = _u64(self.max_artifacts, "max_artifacts")
        max_total_bytes = _u64(self.max_total_bytes, "max_total_bytes")
        return _ffi.CodegenOptionsV1(
            _ffi.CODEGEN_OPTIONS_SIZE,
            flags,
            max_artifacts,
            max_total_bytes,
            (ctypes.c_uint64 * 3)(0, 0, 0),
        )


@dataclass(frozen=True)
class Artifact:
    relative_path: str
    kind: ArtifactKind
    bytes: bytes
    sha256: bytes


_ArtifactSetT = TypeVar("_ArtifactSetT", bound="ArtifactSet")
_HANDLE_TOKEN = object()


class ArtifactSet:
    """Owned reference to one immutable Core ArtifactSet."""

    def __init__(self, handle: _ffi.Handle, token: object = None) -> None:
        if token is not _HANDLE_TOKEN:
            raise TypeError("ArtifactSet handles are created only by fastdb4py.payload")
        if not handle.value:
            raise binding_error(
                "FastDB Core returned success without an artifact set",
                path="/codegen",
                reason="missing_codegen_result",
            )
        self._lock = threading.Lock()
        self._handle: Optional[_ffi.Handle] = handle

    @classmethod
    def _from_handle(cls: Type[_ArtifactSetT], handle: _ffi.Handle) -> _ArtifactSetT:
        try:
            return cls(handle, _HANDLE_TOKEN)
        except BaseException:
            if handle.value:
                _ffi.library().fdb_payload_v1_codegen_result_release(handle)
            raise

    def close(self) -> None:
        with self._lock:
            handle = self._handle
            self._handle = None
        if handle is not None:
            _ffi.library().fdb_payload_v1_codegen_result_release(handle)

    def __enter__(self: _ArtifactSetT) -> _ArtifactSetT:
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

    def clone(self: _ArtifactSetT) -> _ArtifactSetT:
        with self._lock:
            handle = self._require_handle_locked()
            _ffi.library().fdb_payload_v1_codegen_result_retain(handle)
            cloned = _ffi.Handle(handle.value)
        return type(self)._from_handle(cloned)

    def __copy__(self: _ArtifactSetT) -> _ArtifactSetT:
        return self.clone()

    def __deepcopy__(self: _ArtifactSetT, memo: object) -> _ArtifactSetT:
        return self.clone()

    def artifact_count(self) -> int:
        native = _ffi.library()
        count = ctypes.c_uint64(0)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_codegen_result_artifact_count(
                    handle, ctypes.byref(count), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return int(count.value)

    def artifact(self, index: int) -> Artifact:
        index = _u64(index, "artifact_index")
        path = self._query_blob(
            "fdb_payload_v1_codegen_result_artifact_relative_path", index
        )
        try:
            relative_path = path.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise binding_error(
                "FastDB Core returned a non-UTF-8 artifact path",
                path="/codegen/artifacts/relative_path",
                reason="invalid_utf8_artifact_path",
            ) from exc

        native = _ffi.library()
        kind = ctypes.c_uint32(0)
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_codegen_result_artifact_kind(
                    handle, index, ctypes.byref(kind), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        try:
            artifact_kind = ArtifactKind(kind.value)
        except ValueError as exc:
            raise binding_error(
                "FastDB Core returned an unknown artifact kind",
                path="/codegen/artifacts/kind",
                reason="unknown_artifact_kind",
            ) from exc

        content = self._query_blob(
            "fdb_payload_v1_codegen_result_artifact_bytes", index
        )
        digest = (ctypes.c_uint8 * _ffi.SHA256_SIZE)()
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_codegen_result_artifact_sha256(
                    handle, index, digest, ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return Artifact(relative_path, artifact_kind, content, bytes(digest))

    def _query_blob(self, function_name: str, index: int) -> bytes:
        native = _ffi.library()
        blob = _ffi.Handle()
        error = _ffi.Handle()
        function = getattr(native, function_name)
        with self._borrow_handle() as handle:
            status = int(
                function(handle, index, ctypes.byref(blob), ctypes.byref(error))
            )
            _check_status(status, error)
        return CompiledSpec._copy_blob(blob)

    def _require_handle_locked(self) -> _ffi.Handle:
        if self._handle is None:
            raise binding_error(
                "ArtifactSet is closed",
                path="/codegen",
                reason="closed_handle",
            )
        return self._handle

    @contextmanager
    def _borrow_handle(self) -> Iterator[_ffi.Handle]:
        native = _ffi.library()
        with self._lock:
            handle = self._require_handle_locked()
            native.fdb_payload_v1_codegen_result_retain(handle)
            borrowed = _ffi.Handle(handle.value)
        try:
            yield borrowed
        finally:
            native.fdb_payload_v1_codegen_result_release(borrowed)


def generate(
    spec: CompiledSpec,
    target: CodegenTarget,
    options: Optional[CodegenOptions] = None,
) -> ArtifactSet:
    if not isinstance(target, CodegenTarget):
        raise TypeError("target must be a CodegenTarget")
    if options is None:
        options = CodegenOptions()
    elif not isinstance(options, CodegenOptions):
        raise TypeError("options must be a CodegenOptions")
    native = _ffi.library()
    raw_options = options._to_ffi()
    result = _ffi.Handle()
    error = _ffi.Handle()
    with spec._borrow_handle() as handle:
        status = int(
            native.fdb_payload_v1_spec_codegen(
                handle,
                int(target),
                ctypes.byref(raw_options),
                ctypes.byref(result),
                ctypes.byref(error),
            )
        )
        _check_status(status, error)
    return ArtifactSet._from_handle(result)
