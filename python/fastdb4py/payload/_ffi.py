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
OPEN_OPTIONS_SIZE = 112
PLAN_INFO_SIZE = 112
EXECUTION_REPORT_SIZE = 72
CODEGEN_OPTIONS_SIZE = 48
BACKING_SIZE = 96

Handle = ctypes.c_void_p
HandlePointer = ctypes.POINTER(Handle)
BytePointer = ctypes.POINTER(ctypes.c_uint8)
BytePointerPointer = ctypes.POINTER(BytePointer)
U16Pointer = ctypes.POINTER(ctypes.c_uint16)
U16PointerPointer = ctypes.POINTER(U16Pointer)
VoidPointerPointer = ctypes.POINTER(ctypes.c_void_p)

BackingReserveCallback = ctypes.CFUNCTYPE(
    ctypes.c_uint32,
    ctypes.c_void_p,
    ctypes.c_uint32,
    ctypes.c_uint64,
    ctypes.c_uint32,
    VoidPointerPointer,
    BytePointerPointer,
    ctypes.POINTER(ctypes.c_uint64),
)
BackingWriteCallback = ctypes.CFUNCTYPE(
    ctypes.c_uint32,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_uint64,
    BytePointer,
    ctypes.c_uint64,
)
BackingCommitCallback = ctypes.CFUNCTYPE(
    ctypes.c_uint32,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_uint64,
    BytePointerPointer,
    ctypes.POINTER(ctypes.c_uint64),
)
BackingRollbackCallback = ctypes.CFUNCTYPE(
    ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p
)
BackingRetainCallback = ctypes.CFUNCTYPE(
    ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p
)
BackingReleaseCallback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)


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


class OpenOptionsV1(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("max_total_bytes", ctypes.c_uint64),
        ("max_regions", ctypes.c_uint64),
        ("max_entries", ctypes.c_uint64),
        ("max_components", ctypes.c_uint64),
        ("max_nesting_depth", ctypes.c_uint64),
        ("max_list_elements", ctypes.c_uint64),
        ("max_graph_objects", ctypes.c_uint64),
        ("max_string_bytes", ctypes.c_uint64),
        ("max_validation_work", ctypes.c_uint64),
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


class ExecutionReportV1(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("mode", ctypes.c_uint32),
        ("fallback_reason", ctypes.c_uint32),
        ("reserved32", ctypes.c_uint32),
        ("requested_bytes", ctypes.c_uint64),
        ("used_bytes", ctypes.c_uint64),
        ("staging_bytes", ctypes.c_uint64),
        ("region_count", ctypes.c_uint64),
        ("backing_capacity", ctypes.c_uint64),
        ("reserved64", ctypes.c_uint64 * 2),
    ]


class CodegenOptionsV1(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("max_artifacts", ctypes.c_uint64),
        ("max_total_bytes", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 3),
    ]


class BackingV1(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("context", ctypes.c_void_p),
        ("reserve", BackingReserveCallback),
        ("write", BackingWriteCallback),
        ("commit", BackingCommitCallback),
        ("rollback", BackingRollbackCallback),
        ("retain", BackingRetainCallback),
        ("release", BackingReleaseCallback),
        ("reserved", ctypes.c_uint64 * 4),
    ]


if ctypes.sizeof(BuilderOptionsV2) != BUILDER_OPTIONS_SIZE:
    raise ImportError("FastDB builder-options ABI layout mismatch")
if ctypes.sizeof(FixedRunV1) != FIXED_RUN_SIZE:
    raise ImportError("FastDB fixed-run ABI layout mismatch")
if ctypes.sizeof(OpenOptionsV1) != OPEN_OPTIONS_SIZE:
    raise ImportError("FastDB open-options ABI layout mismatch")
if ctypes.sizeof(PlanInfoV2) != PLAN_INFO_SIZE:
    raise ImportError("FastDB plan-info ABI layout mismatch")
if ctypes.sizeof(ExecutionReportV1) != EXECUTION_REPORT_SIZE:
    raise ImportError("FastDB execution-report ABI layout mismatch")
if ctypes.sizeof(CodegenOptionsV1) != CODEGEN_OPTIONS_SIZE:
    raise ImportError("FastDB codegen-options ABI layout mismatch")
if ctypes.sizeof(BackingV1) != BACKING_SIZE:
    raise ImportError("FastDB backing ABI layout mismatch")


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
    library.fdb_payload_v1_open_options_init.argtypes = [
        ctypes.POINTER(OpenOptionsV1)
    ]
    library.fdb_payload_v1_open_options_init.restype = None
    library.fdb_payload_v1_plan_info_init.argtypes = [
        ctypes.POINTER(PlanInfoV2)
    ]
    library.fdb_payload_v1_plan_info_init.restype = None
    library.fdb_payload_v1_execution_report_init.argtypes = [
        ctypes.POINTER(ExecutionReportV1)
    ]
    library.fdb_payload_v1_execution_report_init.restype = None
    library.fdb_payload_v1_codegen_options_init.argtypes = [
        ctypes.POINTER(CodegenOptionsV1)
    ]
    library.fdb_payload_v1_codegen_options_init.restype = None
    library.fdb_payload_v1_backing_init.argtypes = [ctypes.POINTER(BackingV1)]
    library.fdb_payload_v1_backing_init.restype = None

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
    library.fdb_payload_v1_spec_codegen.argtypes = [
        Handle,
        ctypes.c_uint64,
        ctypes.POINTER(CodegenOptionsV1),
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_spec_codegen.restype = ctypes.c_uint32
    library.fdb_payload_v1_codegen_result_retain.argtypes = [Handle]
    library.fdb_payload_v1_codegen_result_retain.restype = None
    library.fdb_payload_v1_codegen_result_release.argtypes = [Handle]
    library.fdb_payload_v1_codegen_result_release.restype = None
    library.fdb_payload_v1_codegen_result_artifact_count.argtypes = [
        Handle,
        ctypes.POINTER(ctypes.c_uint64),
        HandlePointer,
    ]
    library.fdb_payload_v1_codegen_result_artifact_count.restype = ctypes.c_uint32
    for name in (
        "fdb_payload_v1_codegen_result_artifact_relative_path",
        "fdb_payload_v1_codegen_result_artifact_bytes",
    ):
        function = getattr(library, name)
        function.argtypes = [
            Handle,
            ctypes.c_uint64,
            HandlePointer,
            HandlePointer,
        ]
        function.restype = ctypes.c_uint32
    library.fdb_payload_v1_codegen_result_artifact_kind.argtypes = [
        Handle,
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_uint32),
        HandlePointer,
    ]
    library.fdb_payload_v1_codegen_result_artifact_kind.restype = ctypes.c_uint32
    library.fdb_payload_v1_codegen_result_artifact_sha256.argtypes = [
        Handle,
        ctypes.c_uint64,
        BytePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_codegen_result_artifact_sha256.restype = ctypes.c_uint32

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
    library.fdb_payload_v1_builder_require_spec_sha256.argtypes = [
        Handle,
        BytePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_builder_require_spec_sha256.restype = ctypes.c_uint32
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
    library.fdb_payload_v1_plan_execute.argtypes = [
        Handle,
        ctypes.c_uint32,
        ctypes.POINTER(BackingV1),
        HandlePointer,
        ctypes.POINTER(ExecutionReportV1),
        HandlePointer,
    ]
    library.fdb_payload_v1_plan_execute.restype = ctypes.c_uint32

    library.fdb_payload_v1_payload_open_copy.argtypes = [
        Handle,
        BytePointer,
        ctypes.c_uint64,
        ctypes.POINTER(OpenOptionsV1),
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_open_copy.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_open_external.argtypes = [
        Handle,
        BytePointer,
        ctypes.c_uint64,
        ctypes.POINTER(BackingV1),
        ctypes.c_void_p,
        ctypes.POINTER(OpenOptionsV1),
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_open_external.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_retain.argtypes = [Handle]
    library.fdb_payload_v1_payload_retain.restype = None
    library.fdb_payload_v1_payload_release.argtypes = [Handle]
    library.fdb_payload_v1_payload_release.restype = None
    library.fdb_payload_v1_payload_require_spec_sha256.argtypes = [
        Handle,
        BytePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_require_spec_sha256.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_sha256.argtypes = [
        Handle,
        BytePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_sha256.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_profile.argtypes = [
        Handle,
        ctypes.POINTER(ctypes.c_uint32),
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_profile.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_execution_report.argtypes = [
        Handle,
        ctypes.POINTER(ExecutionReportV1),
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_execution_report.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_binary_blob.argtypes = [
        Handle,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_binary_blob.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_acquire.argtypes = [
        Handle,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_acquire.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_entry_view.argtypes = [
        Handle,
        ctypes.c_uint32,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_payload_entry_view.restype = ctypes.c_uint32
    library.fdb_payload_v1_payload_invalidate.argtypes = [Handle, HandlePointer]
    library.fdb_payload_v1_payload_invalidate.restype = ctypes.c_uint32

    library.fdb_payload_v1_view_retain.argtypes = [Handle]
    library.fdb_payload_v1_view_retain.restype = None
    library.fdb_payload_v1_view_release.argtypes = [Handle]
    library.fdb_payload_v1_view_release.restype = None
    library.fdb_payload_v1_view_require_spec_sha256.argtypes = [
        Handle,
        BytePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_view_require_spec_sha256.restype = ctypes.c_uint32
    for name, value_type in (
        ("fdb_payload_v1_view_kind", ctypes.c_uint32),
        ("fdb_payload_v1_view_is_null", ctypes.c_uint8),
        ("fdb_payload_v1_view_length", ctypes.c_uint64),
        ("fdb_payload_v1_view_component_index", ctypes.c_uint32),
        ("fdb_payload_v1_view_field_count", ctypes.c_uint32),
        ("fdb_payload_v1_view_get_bool", ctypes.c_uint8),
        ("fdb_payload_v1_view_get_u8", ctypes.c_uint8),
        ("fdb_payload_v1_view_get_u16", ctypes.c_uint16),
        ("fdb_payload_v1_view_get_u32", ctypes.c_uint32),
        ("fdb_payload_v1_view_get_i32", ctypes.c_int32),
        ("fdb_payload_v1_view_get_u8n_f64_bits", ctypes.c_uint64),
        ("fdb_payload_v1_view_get_u16n_f64_bits", ctypes.c_uint64),
        ("fdb_payload_v1_view_get_f32_bits", ctypes.c_uint32),
        ("fdb_payload_v1_view_get_f64_bits", ctypes.c_uint64),
    ):
        function = getattr(library, name)
        function.argtypes = [Handle, ctypes.POINTER(value_type), HandlePointer]
        function.restype = ctypes.c_uint32

    library.fdb_payload_v1_view_at.argtypes = [
        Handle,
        ctypes.c_uint64,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_view_at.restype = ctypes.c_uint32
    library.fdb_payload_v1_view_field.argtypes = [
        Handle,
        ctypes.c_uint32,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_view_field.restype = ctypes.c_uint32
    library.fdb_payload_v1_view_ref_target.argtypes = [
        Handle,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_view_ref_target.restype = ctypes.c_uint32
    library.fdb_payload_v1_view_graph_identity.argtypes = [
        Handle,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint64),
        HandlePointer,
    ]
    library.fdb_payload_v1_view_graph_identity.restype = ctypes.c_uint32
    library.fdb_payload_v1_view_acquire.argtypes = [
        Handle,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_view_acquire.restype = ctypes.c_uint32
    library.fdb_payload_v1_view_materialize.argtypes = [
        Handle,
        HandlePointer,
        HandlePointer,
    ]
    library.fdb_payload_v1_view_materialize.restype = ctypes.c_uint32

    library.fdb_payload_v1_access_release.argtypes = [Handle]
    library.fdb_payload_v1_access_release.restype = None
    for name in (
        "fdb_payload_v1_access_payload_bytes",
        "fdb_payload_v1_access_str",
        "fdb_payload_v1_access_bytes",
    ):
        function = getattr(library, name)
        function.argtypes = [
            Handle,
            BytePointerPointer,
            ctypes.POINTER(ctypes.c_uint64),
            HandlePointer,
        ]
        function.restype = ctypes.c_uint32
    library.fdb_payload_v1_access_wstr.argtypes = [
        Handle,
        U16PointerPointer,
        ctypes.POINTER(ctypes.c_uint64),
        HandlePointer,
    ]
    library.fdb_payload_v1_access_wstr.restype = ctypes.c_uint32

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
