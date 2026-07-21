//! Mechanical declarations for the stable FastDB portable-payload C ABI.

#![allow(non_camel_case_types)]

use std::ffi::c_void;

pub const FDB_PAYLOAD_V1_ABI_VERSION: u32 = 1;
pub const FDB_PAYLOAD_V1_SHA256_SIZE: u32 = 32;
pub const FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE: u32 = 80;
pub const FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE: u32 = 72;
pub const FDB_PAYLOAD_V1_BUILDER_OPTIONS_V2_SIZE: u32 = 96;
pub const FDB_PAYLOAD_V1_FIXED_RUN_V1_SIZE: u32 = 96;
pub const FDB_PAYLOAD_V1_PLAN_INFO_V2_SIZE: u32 = 112;
pub const FDB_PAYLOAD_V1_INVALID_OBJECT_HANDLE: u64 = 0;

pub const FDB_PAYLOAD_PROFILE_RECORD_V1: u32 = 1;
pub const FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1: u32 = 2;

pub const FDB_PAYLOAD_E_UNSUPPORTED_ABI: u32 = 7002;
pub const FDB_PAYLOAD_E_INTERNAL: u32 = 9001;

pub type fdb_payload_v1_status_t = u32;
pub type fdb_payload_v1_profile_t = u32;

#[repr(C)]
pub struct fdb_payload_v1_spec_t {
    _private: [u8; 0],
    _marker: std::marker::PhantomData<(*mut u8, std::marker::PhantomPinned)>,
}

#[repr(C)]
pub struct fdb_payload_v1_blob_t {
    _private: [u8; 0],
    _marker: std::marker::PhantomData<(*mut u8, std::marker::PhantomPinned)>,
}

#[repr(C)]
pub struct fdb_payload_v1_error_t {
    _private: [u8; 0],
    _marker: std::marker::PhantomData<(*mut u8, std::marker::PhantomPinned)>,
}

#[repr(C)]
pub struct fdb_payload_v1_builder_t {
    _private: [u8; 0],
    _marker: std::marker::PhantomData<(*mut u8, std::marker::PhantomPinned)>,
}

#[repr(C)]
pub struct fdb_payload_v1_plan_t {
    _private: [u8; 0],
    _marker: std::marker::PhantomData<(*mut u8, std::marker::PhantomPinned)>,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fdb_payload_v1_compile_options_t {
    pub struct_size: u32,
    pub flags: u32,
    pub max_source_bytes: u64,
    pub max_json_values: u64,
    pub max_nesting_depth: u32,
    pub max_entries: u32,
    pub max_components: u32,
    pub max_fields_per_component: u32,
    pub max_total_fields: u64,
    pub reserved: [u64; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fdb_payload_v1_capabilities_t {
    pub struct_size: u32,
    pub profile: u32,
    pub semantic_flags: u64,
    pub operation_flags: u64,
    pub codegen_target_flags: u64,
    pub direct_build_status: u32,
    pub reserved32: u32,
    pub reserved64: [u64; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fdb_payload_v1_builder_options_t {
    pub struct_size: u32,
    pub flags: u32,
    pub max_value_nodes: u64,
    pub max_list_elements: u64,
    pub max_text_bytes: u64,
    pub max_opaque_bytes: u64,
    pub max_nesting_depth: u64,
    pub max_total_builder_bytes: u64,
    pub reserved: [u64; 4],
    pub max_graph_objects: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fdb_payload_v1_fixed_run_v1_t {
    pub struct_size: u32,
    pub flags: u32,
    pub data: *const c_void,
    pub data_byte_length: u64,
    pub count: u64,
    pub stride_bytes: u64,
    pub validity: *const u8,
    pub validity_byte_length: u64,
    pub validity_bit_offset: u64,
    pub reserved: [u64; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct fdb_payload_v1_plan_info_t {
    pub struct_size: u32,
    pub flags: u32,
    pub total_bytes: u64,
    pub region_count: u64,
    pub logical_value_count: u64,
    pub list_element_count: u64,
    pub text_bytes: u64,
    pub opaque_bytes: u64,
    pub validation_work: u64,
    pub max_alignment: u32,
    pub direct_build_status: u32,
    pub reserved: [u64; 4],
    pub graph_object_count: u64,
}

unsafe extern "C" {
    pub fn fdb_payload_v1_abi_version() -> u32;
    pub fn fdb_payload_v1_compile_options_init(options: *mut fdb_payload_v1_compile_options_t);
    pub fn fdb_payload_v1_capabilities_init(capabilities: *mut fdb_payload_v1_capabilities_t);
    pub fn fdb_payload_v1_builder_options_init(options: *mut fdb_payload_v1_builder_options_t);
    pub fn fdb_payload_v1_fixed_run_init(run: *mut fdb_payload_v1_fixed_run_v1_t);
    pub fn fdb_payload_v1_plan_info_init(info: *mut fdb_payload_v1_plan_info_t);

    pub fn fdb_payload_v1_spec_compile_json(
        source: *const u8,
        source_size: u64,
        options: *const fdb_payload_v1_compile_options_t,
        out_spec: *mut *mut fdb_payload_v1_spec_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_retain(spec: *mut fdb_payload_v1_spec_t);
    pub fn fdb_payload_v1_spec_release(spec: *mut fdb_payload_v1_spec_t);
    pub fn fdb_payload_v1_spec_canonical_json(
        spec: *const fdb_payload_v1_spec_t,
        out_blob: *mut *mut fdb_payload_v1_blob_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_sha256(
        spec: *const fdb_payload_v1_spec_t,
        out_digest: *mut u8,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_manifest_json(
        spec: *const fdb_payload_v1_spec_t,
        out_blob: *mut *mut fdb_payload_v1_blob_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_profile(
        spec: *const fdb_payload_v1_spec_t,
        out_profile: *mut fdb_payload_v1_profile_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_capabilities(
        spec: *const fdb_payload_v1_spec_t,
        out_capabilities: *mut fdb_payload_v1_capabilities_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_entry_count(
        spec: *const fdb_payload_v1_spec_t,
        out_count: *mut u32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_entry_id(
        spec: *const fdb_payload_v1_spec_t,
        entry_index: u32,
        out_id: *mut *mut fdb_payload_v1_blob_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_entry_index(
        spec: *const fdb_payload_v1_spec_t,
        id: *const u8,
        id_size: u64,
        out_entry_index: *mut u32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_component_count(
        spec: *const fdb_payload_v1_spec_t,
        out_count: *mut u32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_component_id(
        spec: *const fdb_payload_v1_spec_t,
        component_index: u32,
        out_id: *mut *mut fdb_payload_v1_blob_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_component_index(
        spec: *const fdb_payload_v1_spec_t,
        id: *const u8,
        id_size: u64,
        out_component_index: *mut u32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_component_field_count(
        spec: *const fdb_payload_v1_spec_t,
        component_index: u32,
        out_count: *mut u32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_component_field_id(
        spec: *const fdb_payload_v1_spec_t,
        component_index: u32,
        field_index: u32,
        out_id: *mut *mut fdb_payload_v1_blob_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_spec_component_field_index(
        spec: *const fdb_payload_v1_spec_t,
        component_index: u32,
        id: *const u8,
        id_size: u64,
        out_field_index: *mut u32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;

    pub fn fdb_payload_v1_builder_create(
        spec: *const fdb_payload_v1_spec_t,
        options: *const fdb_payload_v1_builder_options_t,
        out_builder: *mut *mut fdb_payload_v1_builder_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_release(builder: *mut fdb_payload_v1_builder_t);
    pub fn fdb_payload_v1_builder_entry_begin(
        builder: *mut fdb_payload_v1_builder_t,
        entry_index: u32,
        value_count: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_object_declare(
        builder: *mut fdb_payload_v1_builder_t,
        component_index: u32,
        out_object: *mut u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_object_fill_begin(
        builder: *mut fdb_payload_v1_builder_t,
        object: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_null(
        builder: *mut fdb_payload_v1_builder_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_bool(
        builder: *mut fdb_payload_v1_builder_t,
        value: u8,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_u8(
        builder: *mut fdb_payload_v1_builder_t,
        value: u8,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_u16(
        builder: *mut fdb_payload_v1_builder_t,
        value: u16,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_u32(
        builder: *mut fdb_payload_v1_builder_t,
        value: u32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_i32(
        builder: *mut fdb_payload_v1_builder_t,
        value: i32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_u8n_f64_bits(
        builder: *mut fdb_payload_v1_builder_t,
        value: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_u16n_f64_bits(
        builder: *mut fdb_payload_v1_builder_t,
        value: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_f32_bits(
        builder: *mut fdb_payload_v1_builder_t,
        value: u32,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_f64_bits(
        builder: *mut fdb_payload_v1_builder_t,
        value: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_str(
        builder: *mut fdb_payload_v1_builder_t,
        value: *const u8,
        value_size: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_wstr(
        builder: *mut fdb_payload_v1_builder_t,
        value: *const u16,
        value_size: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_bytes(
        builder: *mut fdb_payload_v1_builder_t,
        value: *const u8,
        value_size: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_fixed_run(
        builder: *mut fdb_payload_v1_builder_t,
        run: *const fdb_payload_v1_fixed_run_v1_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_component_begin(
        builder: *mut fdb_payload_v1_builder_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_list_begin(
        builder: *mut fdb_payload_v1_builder_t,
        item_count: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_object(
        builder: *mut fdb_payload_v1_builder_t,
        object: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_value_ref(
        builder: *mut fdb_payload_v1_builder_t,
        object: u64,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;
    pub fn fdb_payload_v1_builder_freeze(
        builder: *mut fdb_payload_v1_builder_t,
        out_plan: *mut *mut fdb_payload_v1_plan_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;

    pub fn fdb_payload_v1_plan_retain(plan: *mut fdb_payload_v1_plan_t);
    pub fn fdb_payload_v1_plan_release(plan: *mut fdb_payload_v1_plan_t);
    pub fn fdb_payload_v1_plan_info(
        plan: *const fdb_payload_v1_plan_t,
        out_info: *mut fdb_payload_v1_plan_info_t,
        out_error: *mut *mut fdb_payload_v1_error_t,
    ) -> fdb_payload_v1_status_t;

    pub fn fdb_payload_v1_blob_retain(blob: *mut fdb_payload_v1_blob_t);
    pub fn fdb_payload_v1_blob_release(blob: *mut fdb_payload_v1_blob_t);
    pub fn fdb_payload_v1_blob_data(blob: *const fdb_payload_v1_blob_t) -> *const u8;
    pub fn fdb_payload_v1_blob_size(blob: *const fdb_payload_v1_blob_t) -> u64;

    pub fn fdb_payload_v1_error_retain(error: *mut fdb_payload_v1_error_t);
    pub fn fdb_payload_v1_error_release(error: *mut fdb_payload_v1_error_t);
    pub fn fdb_payload_v1_error_code(error: *const fdb_payload_v1_error_t) -> u32;
    pub fn fdb_payload_v1_error_symbol(
        error: *const fdb_payload_v1_error_t,
        out_data: *mut *const u8,
        out_size: *mut u64,
    );
    pub fn fdb_payload_v1_error_path(
        error: *const fdb_payload_v1_error_t,
        out_data: *mut *const u8,
        out_size: *mut u64,
    );
    pub fn fdb_payload_v1_error_message(
        error: *const fdb_payload_v1_error_t,
        out_data: *mut *const u8,
        out_size: *mut u64,
    );
    pub fn fdb_payload_v1_error_details_json(
        error: *const fdb_payload_v1_error_t,
        out_data: *mut *const u8,
        out_size: *mut u64,
    );
}

const _: () = {
    assert!(std::mem::size_of::<fdb_payload_v1_compile_options_t>() == 80);
    assert!(std::mem::size_of::<fdb_payload_v1_capabilities_t>() == 72);
    assert!(std::mem::size_of::<fdb_payload_v1_builder_options_t>() == 96);
    assert!(std::mem::size_of::<fdb_payload_v1_fixed_run_v1_t>() == 96);
    assert!(std::mem::size_of::<fdb_payload_v1_plan_info_t>() == 112);
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn raw_prefix_layout_and_core_abi_match() {
        assert_eq!(
            size_of::<fdb_payload_v1_compile_options_t>(),
            FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE as usize
        );
        assert_eq!(
            size_of::<fdb_payload_v1_capabilities_t>(),
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE as usize
        );
        assert_eq!(
            size_of::<fdb_payload_v1_builder_options_t>(),
            FDB_PAYLOAD_V1_BUILDER_OPTIONS_V2_SIZE as usize
        );
        assert_eq!(
            size_of::<fdb_payload_v1_fixed_run_v1_t>(),
            FDB_PAYLOAD_V1_FIXED_RUN_V1_SIZE as usize
        );
        assert_eq!(
            size_of::<fdb_payload_v1_plan_info_t>(),
            FDB_PAYLOAD_V1_PLAN_INFO_V2_SIZE as usize
        );
        // SAFETY: this function has no arguments and returns a value.
        assert_eq!(
            unsafe { fdb_payload_v1_abi_version() },
            FDB_PAYLOAD_V1_ABI_VERSION
        );
    }
}
