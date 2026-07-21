"""Private ctypes declarations for the stable FastDB payload C ABI."""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path
from typing import Optional


ABI_VERSION = 1
SHA256_SIZE = 32

Handle = ctypes.c_void_p
HandlePointer = ctypes.POINTER(Handle)
BytePointer = ctypes.POINTER(ctypes.c_uint8)
BytePointerPointer = ctypes.POINTER(BytePointer)


class CapabilitiesV1(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("profile", ctypes.c_uint32),
        ("semantic_flags", ctypes.c_uint64),
        ("operation_flags", ctypes.c_uint64),
        ("codegen_target_flags", ctypes.c_uint64),
        ("direct_build_status", ctypes.c_uint32),
        ("reserved32", ctypes.c_uint32),
        ("reserved64", ctypes.c_uint64 * 4),
    ]


def _library_names() -> tuple[str, ...]:
    if sys.platform == "darwin":
        return ("libfastdb.dylib", "libfastdb.so")
    if sys.platform == "win32":
        return ("fastdb.dll", "libfastdb.dll")
    return ("libfastdb.so", "libfastdb.dylib")


def _library_path() -> Path:
    override = os.environ.get("FASTDB_PAYLOAD_LIBRARY")
    if override:
        path = Path(override).expanduser().resolve()
        if not path.is_file():
            raise ImportError(
                "FASTDB_PAYLOAD_LIBRARY does not name a native library file: "
                f"{path}"
            )
        return path

    core_dir = Path(__file__).resolve().parents[1] / "core"
    for name in _library_names():
        candidate = core_dir / name
        if candidate.is_file():
            return candidate
    names = ", ".join(_library_names())
    raise ImportError(
        f"fastdb4py.payload could not find the packaged FastDB library "
        f"({names}) under {core_dir}"
    )


def _declare(library: ctypes.CDLL) -> None:
    library.fdb_payload_v1_abi_version.argtypes = []
    library.fdb_payload_v1_abi_version.restype = ctypes.c_uint32

    library.fdb_payload_v1_capabilities_init.argtypes = [
        ctypes.POINTER(CapabilitiesV1)
    ]
    library.fdb_payload_v1_capabilities_init.restype = None

    library.fdb_payload_v1_spec_compile_json.argtypes = [
        BytePointer,
        ctypes.c_uint64,
        ctypes.c_void_p,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_compile_json.restype = ctypes.c_uint32
    library.fdb_payload_v1_spec_retain.argtypes = [Handle]
    library.fdb_payload_v1_spec_retain.restype = None
    library.fdb_payload_v1_spec_release.argtypes = [Handle]
    library.fdb_payload_v1_spec_release.restype = None

    for name in (
        "fdb_payload_v1_spec_canonical_json",
        "fdb_payload_v1_spec_manifest_json",
    ):
        function = getattr(library, name)
        function.argtypes = [Handle, HandlePointer, HandlePointer]
        function.restype = ctypes.c_uint32

    library.fdb_payload_v1_spec_sha256.argtypes = [
        Handle,
        BytePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_sha256.restype = ctypes.c_uint32
    library.fdb_payload_v1_spec_profile.argtypes = [
        Handle,
        ctypes.POINTER(ctypes.c_uint32),
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_profile.restype = ctypes.c_uint32
    library.fdb_payload_v1_spec_capabilities.argtypes = [
        Handle,
        ctypes.POINTER(CapabilitiesV1),
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_capabilities.restype = ctypes.c_uint32

    for name in (
        "fdb_payload_v1_spec_entry_count",
        "fdb_payload_v1_spec_component_count",
    ):
        function = getattr(library, name)
        function.argtypes = [
            Handle,
            ctypes.POINTER(ctypes.c_uint32),
            HandlePointer,
        ]
        function.restype = ctypes.c_uint32

    library.fdb_payload_v1_spec_entry_id.argtypes = [
        Handle,
        ctypes.c_uint32,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_entry_id.restype = ctypes.c_uint32
    library.fdb_payload_v1_spec_component_id.argtypes = [
        Handle,
        ctypes.c_uint32,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_component_id.restype = ctypes.c_uint32
    library.fdb_payload_v1_spec_component_field_id.argtypes = [
        Handle,
        ctypes.c_uint32,
        ctypes.c_uint32,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_component_field_id.restype = ctypes.c_uint32

    for name in (
        "fdb_payload_v1_spec_entry_index",
        "fdb_payload_v1_spec_component_index",
    ):
        function = getattr(library, name)
        function.argtypes = [
            Handle,
            BytePointer,
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_uint32),
            HandlePointer,
        ]
        function.restype = ctypes.c_uint32

    library.fdb_payload_v1_spec_component_field_count.argtypes = [
        Handle,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_component_field_count.restype = ctypes.c_uint32
    library.fdb_payload_v1_spec_component_field_index.argtypes = [
        Handle,
        ctypes.c_uint32,
        BytePointer,
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_uint32),
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_component_field_index.restype = ctypes.c_uint32

    library.fdb_payload_v1_blob_release.argtypes = [Handle]
    library.fdb_payload_v1_blob_release.restype = None
    library.fdb_payload_v1_blob_data.argtypes = [Handle]
    library.fdb_payload_v1_blob_data.restype = BytePointer
    library.fdb_payload_v1_blob_size.argtypes = [Handle]
    library.fdb_payload_v1_blob_size.restype = ctypes.c_uint64

    library.fdb_payload_v1_error_release.argtypes = [Handle]
    library.fdb_payload_v1_error_release.restype = None
    library.fdb_payload_v1_error_code.argtypes = [Handle]
    library.fdb_payload_v1_error_code.restype = ctypes.c_uint32
    for name in (
        "fdb_payload_v1_error_symbol",
        "fdb_payload_v1_error_path",
        "fdb_payload_v1_error_message",
        "fdb_payload_v1_error_details_json",
    ):
        function = getattr(library, name)
        function.argtypes = [
            Handle,
            BytePointerPointer,
            ctypes.POINTER(ctypes.c_uint64),
        ]
        function.restype = None


_LOADED: Optional[ctypes.CDLL] = None


def library() -> ctypes.CDLL:
    global _LOADED
    if _LOADED is None:
        loaded = ctypes.CDLL(str(_library_path()))
        _declare(loaded)
        actual_abi = int(loaded.fdb_payload_v1_abi_version())
        if actual_abi != ABI_VERSION:
            raise ImportError(
                "fastdb4py.payload requires FastDB payload ABI "
                f"{ABI_VERSION}, but the packaged library reports {actual_abi}"
            )
        _LOADED = loaded
    return _LOADED
