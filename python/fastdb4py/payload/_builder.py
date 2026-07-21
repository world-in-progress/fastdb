"""Authoring and immutable-plan projection over the FastDB payload ABI."""

from __future__ import annotations

import ctypes
from contextlib import contextmanager
from dataclasses import dataclass
import struct
import threading
from typing import Iterator, Optional, Sequence, Type, TypeVar

from . import _ffi
from ._error import PayloadError, binding_error
from ._spec import CompiledSpec, _check_status, _input_bytes


_U64_MAX = (1 << 64) - 1
_HANDLE_TOKEN = object()


def _checked_unsigned(value: int, maximum: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer")
    if value < 0 or value > maximum:
        raise ValueError(f"{name} is outside its unsigned ABI range")
    return value


def _checked_i32(value: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer")
    if value < -(1 << 31) or value > (1 << 31) - 1:
        raise ValueError(f"{name} must be a signed 32-bit integer")
    return value


def _u64(value: int, name: str) -> int:
    return _checked_unsigned(value, _U64_MAX, name)


@dataclass(frozen=True)
class BuilderOptions:
    """Optional overrides applied to Core-owned builder defaults."""

    flags: Optional[int] = None
    max_value_nodes: Optional[int] = None
    max_list_elements: Optional[int] = None
    max_text_bytes: Optional[int] = None
    max_opaque_bytes: Optional[int] = None
    max_nesting_depth: Optional[int] = None
    max_total_builder_bytes: Optional[int] = None
    max_graph_objects: Optional[int] = None

    def _to_ffi(self) -> _ffi.BuilderOptionsV2:
        native = _ffi.library()
        raw = _ffi.BuilderOptionsV2()
        native.fdb_payload_v1_builder_options_init(ctypes.byref(raw))
        if self.flags is not None:
            raw.flags = _checked_unsigned(self.flags, 0xFFFF_FFFF, "flags")
        for name in (
            "max_value_nodes",
            "max_list_elements",
            "max_text_bytes",
            "max_opaque_bytes",
            "max_nesting_depth",
            "max_total_builder_bytes",
            "max_graph_objects",
        ):
            value = getattr(self, name)
            if value is not None:
                setattr(raw, name, _u64(value, name))
        return raw


@dataclass(frozen=True)
class FixedRun:
    """Borrowed exact-width native scalar bytes for one synchronous call."""

    data: bytes
    count: int
    stride_bytes: int
    validity: Optional[bytes] = None
    validity_bit_offset: int = 0

    def __post_init__(self) -> None:
        object.__setattr__(self, "data", bytes(self.data))
        if self.validity is not None:
            object.__setattr__(self, "validity", bytes(self.validity))


@dataclass(frozen=True, init=False)
class ObjectHandle:
    """Opaque, non-owning, builder-local object token."""

    _raw: int

    def __init__(self, raw: int, token: object = None) -> None:
        if token is not _HANDLE_TOKEN:
            raise TypeError("ObjectHandle values are created only by fastdb4py.payload")
        object.__setattr__(self, "_raw", raw)

    @classmethod
    def _from_raw(cls, raw: int) -> ObjectHandle:
        return cls(raw, _HANDLE_TOKEN)


@dataclass(frozen=True)
class PlanInfo:
    flags: int
    total_bytes: int
    region_count: int
    logical_value_count: int
    list_element_count: int
    text_bytes: int
    opaque_bytes: int
    validation_work: int
    max_alignment: int
    direct_build_status: int
    graph_object_count: int


_BuildPlanT = TypeVar("_BuildPlanT", bound="BuildPlan")


class BuildPlan:
    """Owned reference to an immutable, thread-safe Core build plan."""

    def __init__(self, handle: _ffi.Handle, token: object) -> None:
        if token is not _HANDLE_TOKEN:
            raise TypeError("BuildPlan handles are created only by fastdb4py.payload")
        if not handle.value:
            raise binding_error(
                "FastDB Core returned success without a build plan",
                reason="missing_plan_handle",
            )
        self._lock = threading.Lock()
        self._handle: Optional[_ffi.Handle] = handle

    @classmethod
    def _from_handle(cls: Type[_BuildPlanT], handle: _ffi.Handle) -> _BuildPlanT:
        return cls(handle, _HANDLE_TOKEN)

    def close(self) -> None:
        with self._lock:
            handle = self._handle
            self._handle = None
        if handle is not None:
            _ffi.library().fdb_payload_v1_plan_release(handle)

    def __enter__(self: _BuildPlanT) -> _BuildPlanT:
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

    def clone(self: _BuildPlanT) -> _BuildPlanT:
        with self._lock:
            handle = self._require_handle_locked()
            _ffi.library().fdb_payload_v1_plan_retain(handle)
            clone = _ffi.Handle(handle.value)
        return type(self)._from_handle(clone)

    def info(self) -> PlanInfo:
        native = _ffi.library()
        raw = _ffi.PlanInfoV2()
        native.fdb_payload_v1_plan_info_init(ctypes.byref(raw))
        error = _ffi.Handle()
        with self._borrow_handle() as handle:
            status = int(
                native.fdb_payload_v1_plan_info(
                    handle, ctypes.byref(raw), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return PlanInfo(
            flags=int(raw.flags),
            total_bytes=int(raw.total_bytes),
            region_count=int(raw.region_count),
            logical_value_count=int(raw.logical_value_count),
            list_element_count=int(raw.list_element_count),
            text_bytes=int(raw.text_bytes),
            opaque_bytes=int(raw.opaque_bytes),
            validation_work=int(raw.validation_work),
            max_alignment=int(raw.max_alignment),
            direct_build_status=int(raw.direct_build_status),
            graph_object_count=int(raw.graph_object_count),
        )

    def _require_handle_locked(self) -> _ffi.Handle:
        if self._handle is None:
            raise binding_error(
                "BuildPlan is closed", path="/plan", reason="closed_handle"
            )
        return self._handle

    @contextmanager
    def _borrow_handle(self) -> Iterator[_ffi.Handle]:
        native = _ffi.library()
        with self._lock:
            handle = self._require_handle_locked()
            native.fdb_payload_v1_plan_retain(handle)
            borrowed = _ffi.Handle(handle.value)
        try:
            yield borrowed
        finally:
            native.fdb_payload_v1_plan_release(borrowed)


_BuilderT = TypeVar("_BuilderT", bound="Builder")


class Builder:
    """Unique Core builder whose mutations are confined to its creating thread."""

    def __init__(self, handle: _ffi.Handle, token: object) -> None:
        if token is not _HANDLE_TOKEN:
            raise TypeError("Builder handles are created only by fastdb4py.payload")
        if not handle.value:
            raise binding_error(
                "FastDB Core returned success without a builder",
                reason="missing_builder_handle",
            )
        self._lock = threading.Lock()
        self._handle: Optional[_ffi.Handle] = handle
        self._thread_id = threading.get_ident()

    @classmethod
    def _from_handle(cls: Type[_BuilderT], handle: _ffi.Handle) -> _BuilderT:
        return cls(handle, _HANDLE_TOKEN)

    @classmethod
    def create(
        cls: Type[_BuilderT],
        spec: CompiledSpec,
        options: Optional[BuilderOptions] = None,
    ) -> _BuilderT:
        native = _ffi.library()
        raw_options = options._to_ffi() if options is not None else None
        handle = _ffi.Handle()
        error = _ffi.Handle()
        with spec._borrow_handle() as spec_handle:
            status = int(
                native.fdb_payload_v1_builder_create(
                    spec_handle,
                    ctypes.byref(raw_options) if raw_options is not None else None,
                    ctypes.byref(handle),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
        return cls._from_handle(handle)

    def close(self) -> None:
        with self._lock:
            handle = self._handle
            self._handle = None
        if handle is not None:
            _ffi.library().fdb_payload_v1_builder_release(handle)

    def __enter__(self: _BuilderT) -> _BuilderT:
        self._check_thread()
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

    def entry_begin(self, entry_index: int, value_count: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_entry_begin",
            _checked_unsigned(entry_index, 0xFFFF_FFFF, "entry_index"),
            _u64(value_count, "value_count"),
        )

    def declare_object(self, component_index: int) -> ObjectHandle:
        native = _ffi.library()
        raw = ctypes.c_uint64(0)
        error = _ffi.Handle()
        self._check_thread()
        with self._lock:
            handle = self._require_handle_locked()
            status = int(
                native.fdb_payload_v1_builder_object_declare(
                    handle,
                    _checked_unsigned(
                        component_index, 0xFFFF_FFFF, "component_index"
                    ),
                    ctypes.byref(raw),
                    ctypes.byref(error),
                )
            )
            _check_status(status, error)
        if raw.value == 0:
            raise binding_error(
                "FastDB Core returned success without an object handle",
                path="/object",
                reason="missing_object_handle",
            )
        return ObjectHandle._from_raw(int(raw.value))

    def object_fill_begin(self, object_handle: ObjectHandle) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_object_fill_begin",
            self._object_value(object_handle),
        )

    def value_null(self) -> Builder:
        return self._call("fdb_payload_v1_builder_value_null")

    def value_bool(self, value: bool) -> Builder:
        if not isinstance(value, bool):
            raise TypeError("value must be bool")
        return self._call("fdb_payload_v1_builder_value_bool", int(value))

    def value_u8(self, value: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_u8",
            _checked_unsigned(value, 0xFF, "value"),
        )

    def value_u16(self, value: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_u16",
            _checked_unsigned(value, 0xFFFF, "value"),
        )

    def value_u32(self, value: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_u32",
            _checked_unsigned(value, 0xFFFF_FFFF, "value"),
        )

    def value_i32(self, value: int) -> Builder:
        return self._call("fdb_payload_v1_builder_value_i32", _checked_i32(value, "value"))

    def value_u8n(self, value: float) -> Builder:
        return self.value_u8n_bits(_f64_bits(value))

    def value_u8n_bits(self, bits: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_u8n_f64_bits",
            _u64(bits, "bits"),
        )

    def value_u16n(self, value: float) -> Builder:
        return self.value_u16n_bits(_f64_bits(value))

    def value_u16n_bits(self, bits: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_u16n_f64_bits",
            _u64(bits, "bits"),
        )

    def value_f32(self, value: float) -> Builder:
        return self.value_f32_bits(_f32_bits(value))

    def value_f32_bits(self, bits: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_f32_bits",
            _checked_unsigned(bits, 0xFFFF_FFFF, "bits"),
        )

    def value_f64(self, value: float) -> Builder:
        return self.value_f64_bits(_f64_bits(value))

    def value_f64_bits(self, bits: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_f64_bits", _u64(bits, "bits")
        )

    def value_str(self, value: str) -> Builder:
        if not isinstance(value, str):
            raise TypeError("value must be str")
        return self.value_str_bytes(value.encode("utf-8", errors="surrogatepass"))

    def value_str_bytes(self, value: bytes) -> Builder:
        return self._call_bytes("fdb_payload_v1_builder_value_str", value)

    def value_wstr(self, value: str) -> Builder:
        if not isinstance(value, str):
            raise TypeError("value must be str")
        encoded = value.encode("utf-16-le", errors="surrogatepass")
        units = tuple(
            int.from_bytes(encoded[index : index + 2], byteorder="little")
            for index in range(0, len(encoded), 2)
        )
        return self.value_wstr_units(units)

    def value_wstr_units(self, value: Sequence[int]) -> Builder:
        units = tuple(
            _checked_unsigned(item, 0xFFFF, "wstr unit") for item in value
        )
        storage = (ctypes.c_uint16 * len(units))(*units)
        pointer = (
            ctypes.cast(storage, ctypes.POINTER(ctypes.c_uint16))
            if units
            else ctypes.POINTER(ctypes.c_uint16)()
        )
        return self._call(
            "fdb_payload_v1_builder_value_wstr", pointer, len(units)
        )

    def value_bytes(self, value: bytes) -> Builder:
        return self._call_bytes("fdb_payload_v1_builder_value_bytes", value)

    def value_fixed_run(self, run: FixedRun) -> Builder:
        if not isinstance(run, FixedRun):
            raise TypeError("run must be FixedRun")
        data_storage, data, data_size = _input_bytes(run.data)
        if run.validity is None:
            validity_storage: object = b""
            validity = _ffi.BytePointer()
            validity_size = 0
        else:
            validity_storage, validity, validity_size = _input_bytes(run.validity)
        raw = _ffi.FixedRunV1()
        native = _ffi.library()
        native.fdb_payload_v1_fixed_run_init(ctypes.byref(raw))
        raw.data = ctypes.cast(data, ctypes.c_void_p)
        raw.data_byte_length = data_size
        raw.count = _u64(run.count, "count")
        raw.stride_bytes = _u64(run.stride_bytes, "stride_bytes")
        raw.validity = validity
        raw.validity_byte_length = validity_size
        raw.validity_bit_offset = _u64(
            run.validity_bit_offset, "validity_bit_offset"
        )
        result = self._call(
            "fdb_payload_v1_builder_value_fixed_run", ctypes.byref(raw)
        )
        del data_storage, validity_storage
        return result

    def value_component_begin(self) -> Builder:
        return self._call("fdb_payload_v1_builder_value_component_begin")

    def value_list_begin(self, item_count: int) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_list_begin",
            _u64(item_count, "item_count"),
        )

    def value_object(self, object_handle: ObjectHandle) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_object",
            self._object_value(object_handle),
        )

    def value_ref(self, object_handle: ObjectHandle) -> Builder:
        return self._call(
            "fdb_payload_v1_builder_value_ref",
            self._object_value(object_handle),
        )

    def freeze(self) -> BuildPlan:
        native = _ffi.library()
        plan = _ffi.Handle()
        error = _ffi.Handle()
        self._check_thread()
        with self._lock:
            handle = self._require_handle_locked()
            status = int(
                native.fdb_payload_v1_builder_freeze(
                    handle, ctypes.byref(plan), ctypes.byref(error)
                )
            )
            _check_status(status, error)
        return BuildPlan._from_handle(plan)

    def _call(self, function_name: str, *arguments: object) -> Builder:
        native = _ffi.library()
        function = getattr(native, function_name)
        error = _ffi.Handle()
        self._check_thread()
        with self._lock:
            handle = self._require_handle_locked()
            status = int(function(handle, *arguments, ctypes.byref(error)))
            _check_status(status, error)
        return self

    def _call_bytes(self, function_name: str, value: bytes) -> Builder:
        storage, data, size = _input_bytes(value)
        result = self._call(function_name, data, size)
        del storage
        return result

    def _check_thread(self) -> None:
        if threading.get_ident() != self._thread_id:
            raise binding_error(
                "Builder mutations must run on the creating thread",
                path="/builder",
                reason="wrong_thread",
            )

    def _require_handle_locked(self) -> _ffi.Handle:
        if self._handle is None:
            raise binding_error(
                "Builder is closed", path="/builder", reason="closed_handle"
            )
        return self._handle

    @staticmethod
    def _object_value(object_handle: ObjectHandle) -> int:
        if not isinstance(object_handle, ObjectHandle):
            raise TypeError("object_handle must be ObjectHandle")
        return _u64(object_handle._raw, "object_handle")


def _f32_bits(value: float) -> int:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise TypeError("value must be a real number")
    return int(struct.unpack("=I", struct.pack("=f", float(value)))[0])


def _f64_bits(value: float) -> int:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise TypeError("value must be a real number")
    return int(struct.unpack("=Q", struct.pack("=d", float(value)))[0])
