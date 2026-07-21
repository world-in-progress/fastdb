"""Private ctypes declarations for the stable FastDB payload C ABI."""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path
from typing import Optional


ABI_VERSION = 1
SHA256_SIZE = 32
BUILDER_OPTIONS_SIZE = 96
FIXED_RUN_SIZE = 96
PLAN_INFO_SIZE = 112

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


class BuilderOptionsV2(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("max_value_nodes", ctypes.c_uint64),
        ("max_list_elements", ctypes.c_uint64),
        ("max_text_bytes", ctypes.c_uint64),
        ("max_opaque_bytes", ctypes.c_uint64),
        ("max_nesting_depth", ctypes.c_uint64),
        ("max_total_builder_bytes", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 4),
        ("max_graph_objects", ctypes.c_uint64),
    ]


class FixedRunV1(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
        ("data_byte_length", ctypes.c_uint64),
        ("count", ctypes.c_uint64),
        ("stride_bytes", ctypes.c_uint64),
        ("validity", BytePointer),
        ("validity_byte_length", ctypes.c_uint64),
        ("validity_bit_offset", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 4),
    ]


class PlanInfoV2(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("total_bytes", ctypes.c_uint64),
        ("region_count", ctypes.c_uint64),
        ("logical_value_count", ctypes.c_uint64),
        ("list_element_count", ctypes.c_uint64),
        ("text_bytes", ctypes.c_uint64),
        ("opaque_bytes", ctypes.c_uint64),
        ("validation_work", ctypes.c_uint64),
        ("max_alignment", ctypes.c_uint32),
        ("direct_build_status", ctypes.c_uint32),
        ("reserved", ctypes.c_uint64 * 4),
        ("graph_object_count", ctypes.c_uint64),
    ]


if ctypes.sizeof(BuilderOptionsV2) != BUILDER_OPTIONS_SIZE:
    raise ImportError("FastDB builder-options ABI layout mismatch")
if ctypes.sizeof(FixedRunV1) != FIXED_RUN_SIZE:
    raise ImportError("FastDB fixed-run ABI layout mismatch")
if ctypes.sizeof(PlanInfoV2) != PLAN_INFO_SIZE:
    raise ImportError("FastDB plan-info ABI layout mismatch")


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
    library.fdb_payload_v1_builder_options_init.argtypes = [
        ctypes.POINTER(BuilderOptionsV2)
    ]
    library.fdb_payload_v1_builder_options_init.restype = None
    library.fdb_payload_v1_fixed_run_init.argtypes = [
        ctypes.POINTER(FixedRunV1)
    ]
    library.fdb_payload_v1_fixed_run_init.restype = None
    library.fdb_payload_v1_plan_info_init.argtypes = [
        ctypes.POINTER(PlanInfoV2)
    ]
    library.fdb_payload_v1_plan_info_init.restype = None

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

    library.fdb_payload_v1_builder_create.argtypes = [
        Handle,
        ctypes.POINTER(BuilderOptionsV2),
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_builder_create.restype = ctypes.c_uint32
    library.fdb_payload_v1_builder_release.argtypes = [Handle]
    library.fdb_payload_v1_builder_release.restype = None
    library.fdb_payload_v1_builder_entry_begin.argtypes = [
        Handle,
        ctypes.c_uint32,
        ctypes.c_uint64,
        HandlePointer,
    ]
    library.fdb_payload_v1_builder_entry_begin.restype = ctypes.c_uint32
    library.fdb_payload_v1_builder_object_declare.argtypes = [
        Handle,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint64),
        HandlePointer,
    ]
    library.fdb_payload_v1_builder_object_declare.restype = ctypes.c_uint32
    library.fdb_payload_v1_builder_object_fill_begin.argtypes = [
        Handle,
        ctypes.c_uint64,
        HandlePointer,
    ]
    library.fdb_payload_v1_builder_object_fill_begin.restype = ctypes.c_uint32

    for name in (
        "fdb_payload_v1_builder_value_null",
        "fdb_payload_v1_builder_value_component_begin",
    ):
        function = getattr(library, name)
        function.argtypes = [Handle, HandlePointer]
        function.restype = ctypes.c_uint32

    for name, value_type in (
        ("fdb_payload_v1_builder_value_bool", ctypes.c_uint8),
        ("fdb_payload_v1_builder_value_u8", ctypes.c_uint8),
        ("fdb_payload_v1_builder_value_u16", ctypes.c_uint16),
        ("fdb_payload_v1_builder_value_u32", ctypes.c_uint32),
        ("fdb_payload_v1_builder_value_i32", ctypes.c_int32),
        ("fdb_payload_v1_builder_value_u8n_f64_bits", ctypes.c_uint64),
        ("fdb_payload_v1_builder_value_u16n_f64_bits", ctypes.c_uint64),
        ("fdb_payload_v1_builder_value_f32_bits", ctypes.c_uint32),
        ("fdb_payload_v1_builder_value_f64_bits", ctypes.c_uint64),
        ("fdb_payload_v1_builder_value_list_begin", ctypes.c_uint64),
        ("fdb_payload_v1_builder_value_object", ctypes.c_uint64),
        ("fdb_payload_v1_builder_value_ref", ctypes.c_uint64),
    ):
        function = getattr(library, name)
        function.argtypes = [Handle, value_type, HandlePointer]
        function.restype = ctypes.c_uint32

    for name in (
        "fdb_payload_v1_builder_value_str",
        "fdb_payload_v1_builder_value_bytes",
    ):
        function = getattr(library, name)
        function.argtypes = [
            Handle,
            BytePointer,
            ctypes.c_uint64,
            HandlePointer,
        ]
        function.restype = ctypes.c_uint32

    library.fdb_payload_v1_builder_value_wstr.argtypes = [
        Handle,
        ctypes.POINTER(ctypes.c_uint16),
        ctypes.c_uint64,
        HandlePointer,
    ]
    library.fdb_payload_v1_builder_value_wstr.restype = ctypes.c_uint32
    library.fdb_payload_v1_builder_value_fixed_run.argtypes = [
        Handle,
        ctypes.POINTER(FixedRunV1),
        HandlePointer,
    ]
    library.fdb_payload_v1_builder_value_fixed_run.restype = ctypes.c_uint32
    library.fdb_payload_v1_builder_freeze.argtypes = [
        Handle,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_builder_freeze.restype = ctypes.c_uint32

    library.fdb_payload_v1_plan_retain.argtypes = [Handle]
    library.fdb_payload_v1_plan_retain.restype = None
    library.fdb_payload_v1_plan_release.argtypes = [Handle]
    library.fdb_payload_v1_plan_release.restype = None
    library.fdb_payload_v1_plan_info.argtypes = [
        Handle,
        ctypes.POINTER(PlanInfoV2),
        HandlePointer,
    ]
    library.fdb_payload_v1_plan_info.restype = ctypes.c_uint32

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
