use crate::{CompiledSpec, PayloadError, check_status};
use fastdb_sys as sys;
use std::ffi::c_void;
use std::fmt;
use std::marker::PhantomData;
use std::ptr::{self, NonNull};
use std::rc::Rc;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BuilderOptions {
    pub flags: u32,
    pub max_value_nodes: u64,
    pub max_list_elements: u64,
    pub max_text_bytes: u64,
    pub max_opaque_bytes: u64,
    pub max_nesting_depth: u64,
    pub max_total_builder_bytes: u64,
    pub max_graph_objects: u64,
}

impl Default for BuilderOptions {
    fn default() -> Self {
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_builder_options_t>::uninit();
        // SAFETY: Core initializes the complete V2 prefix in this valid output.
        unsafe { sys::fdb_payload_v1_builder_options_init(raw.as_mut_ptr()) };
        // SAFETY: the initializer writes the complete current structure.
        let raw = unsafe { raw.assume_init() };
        Self {
            flags: raw.flags,
            max_value_nodes: raw.max_value_nodes,
            max_list_elements: raw.max_list_elements,
            max_text_bytes: raw.max_text_bytes,
            max_opaque_bytes: raw.max_opaque_bytes,
            max_nesting_depth: raw.max_nesting_depth,
            max_total_builder_bytes: raw.max_total_builder_bytes,
            max_graph_objects: raw.max_graph_objects,
        }
    }
}

impl BuilderOptions {
    fn to_raw(&self) -> sys::fdb_payload_v1_builder_options_t {
        sys::fdb_payload_v1_builder_options_t {
            struct_size: sys::FDB_PAYLOAD_V1_BUILDER_OPTIONS_V2_SIZE,
            flags: self.flags,
            max_value_nodes: self.max_value_nodes,
            max_list_elements: self.max_list_elements,
            max_text_bytes: self.max_text_bytes,
            max_opaque_bytes: self.max_opaque_bytes,
            max_nesting_depth: self.max_nesting_depth,
            max_total_builder_bytes: self.max_total_builder_bytes,
            reserved: [0; 4],
            max_graph_objects: self.max_graph_objects,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct FixedRun<'a> {
    data: &'a [u8],
    count: u64,
    stride_bytes: u64,
    validity: Option<&'a [u8]>,
    validity_bit_offset: u64,
}

impl<'a> FixedRun<'a> {
    pub fn new(data: &'a [u8], count: u64, stride_bytes: u64) -> Self {
        Self {
            data,
            count,
            stride_bytes,
            validity: None,
            validity_bit_offset: 0,
        }
    }

    pub fn with_validity(mut self, validity: &'a [u8], bit_offset: u64) -> Self {
        self.validity = Some(validity);
        self.validity_bit_offset = bit_offset;
        self
    }

    fn to_raw(self) -> Result<sys::fdb_payload_v1_fixed_run_v1_t, PayloadError> {
        let data_byte_length = u64::try_from(self.data.len())
            .map_err(|_| PayloadError::size_overflow("/fixed_run/data"))?;
        let (validity, validity_byte_length) = match self.validity {
            Some(bytes) => (
                bytes.as_ptr(),
                u64::try_from(bytes.len())
                    .map_err(|_| PayloadError::size_overflow("/fixed_run/validity"))?,
            ),
            None => (ptr::null(), 0),
        };
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_fixed_run_v1_t>::uninit();
        // SAFETY: Core initializes the complete V1 structure in this valid output.
        unsafe { sys::fdb_payload_v1_fixed_run_init(raw.as_mut_ptr()) };
        // SAFETY: the initializer writes the complete current structure.
        let mut raw = unsafe { raw.assume_init() };
        raw.data = if self.data.is_empty() {
            ptr::null()
        } else {
            self.data.as_ptr().cast::<c_void>()
        };
        raw.data_byte_length = data_byte_length;
        raw.count = self.count;
        raw.stride_bytes = self.stride_bytes;
        raw.validity = validity;
        raw.validity_byte_length = validity_byte_length;
        raw.validity_bit_offset = self.validity_bit_offset;
        Ok(raw)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ObjectHandle {
    raw: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PlanInfo {
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
    pub graph_object_count: u64,
}

pub struct Builder {
    raw: NonNull<sys::fdb_payload_v1_builder_t>,
    // The Core contract makes builders unique and thread-confined.
    _thread_confined: PhantomData<Rc<()>>,
}

impl Builder {
    pub fn create(spec: &CompiledSpec) -> Result<Self, PayloadError> {
        Self::create_raw(spec, ptr::null())
    }

    pub fn create_with_options(
        spec: &CompiledSpec,
        options: &BuilderOptions,
    ) -> Result<Self, PayloadError> {
        let raw_options = options.to_raw();
        Self::create_raw(spec, &raw_options)
    }

    fn create_raw(
        spec: &CompiledSpec,
        options: *const sys::fdb_payload_v1_builder_options_t,
    ) -> Result<Self, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: the spec is live for the call, options is null or points to a
        // complete local V2 structure, and both output locations are valid.
        let status = unsafe {
            sys::fdb_payload_v1_builder_create(spec.as_raw(), options, &mut raw, &mut error)
        };
        check_status(status, error)?;
        NonNull::new(raw)
            .map(|raw| Self {
                raw,
                _thread_confined: PhantomData,
            })
            .ok_or_else(|| {
                PayloadError::binding(
                    sys::FDB_PAYLOAD_E_INTERNAL,
                    "BINDING_CONTRACT",
                    "",
                    "FastDB Core returned success without a builder",
                    r#"{"reason":"missing_builder_handle"}"#,
                )
            })
    }

    pub fn entry_begin(
        &mut self,
        entry_index: u32,
        value_count: u64,
    ) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status = unsafe {
            sys::fdb_payload_v1_builder_entry_begin(
                self.raw.as_ptr(),
                entry_index,
                value_count,
                &mut error,
            )
        };
        self.complete(status, error)
    }

    pub fn declare_object(&mut self, component_index: u32) -> Result<ObjectHandle, PayloadError> {
        let mut object = sys::FDB_PAYLOAD_V1_INVALID_OBJECT_HANDLE;
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_builder_object_declare(
                self.raw.as_ptr(),
                component_index,
                &mut object,
                &mut error,
            )
        };
        check_status(status, error)?;
        if object == sys::FDB_PAYLOAD_V1_INVALID_OBJECT_HANDLE {
            return Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "/object",
                "FastDB Core returned success without an object handle",
                r#"{"reason":"missing_object_handle"}"#,
            ));
        }
        Ok(ObjectHandle { raw: object })
    }

    pub fn object_fill_begin(&mut self, object: ObjectHandle) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique builder; Core validates the opaque token.
        let status = unsafe {
            sys::fdb_payload_v1_builder_object_fill_begin(self.raw.as_ptr(), object.raw, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_null(&mut self) -> Result<&mut Self, PayloadError> {
        self.call_unary(sys::fdb_payload_v1_builder_value_null)
    }

    pub fn value_bool(&mut self, value: bool) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_bool(self.raw.as_ptr(), u8::from(value), &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_u8(&mut self, value: u8) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status =
            unsafe { sys::fdb_payload_v1_builder_value_u8(self.raw.as_ptr(), value, &mut error) };
        self.complete(status, error)
    }

    pub fn value_u16(&mut self, value: u16) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status =
            unsafe { sys::fdb_payload_v1_builder_value_u16(self.raw.as_ptr(), value, &mut error) };
        self.complete(status, error)
    }

    pub fn value_u32(&mut self, value: u32) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status =
            unsafe { sys::fdb_payload_v1_builder_value_u32(self.raw.as_ptr(), value, &mut error) };
        self.complete(status, error)
    }

    pub fn value_i32(&mut self, value: i32) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status =
            unsafe { sys::fdb_payload_v1_builder_value_i32(self.raw.as_ptr(), value, &mut error) };
        self.complete(status, error)
    }

    pub fn value_u8n(&mut self, value: f64) -> Result<&mut Self, PayloadError> {
        self.value_u8n_bits(value.to_bits())
    }

    pub fn value_u8n_bits(&mut self, bits: u64) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_u8n_f64_bits(self.raw.as_ptr(), bits, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_u16n(&mut self, value: f64) -> Result<&mut Self, PayloadError> {
        self.value_u16n_bits(value.to_bits())
    }

    pub fn value_u16n_bits(&mut self, bits: u64) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_u16n_f64_bits(self.raw.as_ptr(), bits, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_f32(&mut self, value: f32) -> Result<&mut Self, PayloadError> {
        self.value_f32_bits(value.to_bits())
    }

    pub fn value_f32_bits(&mut self, bits: u32) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_f32_bits(self.raw.as_ptr(), bits, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_f64(&mut self, value: f64) -> Result<&mut Self, PayloadError> {
        self.value_f64_bits(value.to_bits())
    }

    pub fn value_f64_bits(&mut self, bits: u64) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_f64_bits(self.raw.as_ptr(), bits, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_str(&mut self, value: &str) -> Result<&mut Self, PayloadError> {
        self.value_str_bytes(value.as_bytes())
    }

    pub fn value_str_bytes(&mut self, value: &[u8]) -> Result<&mut Self, PayloadError> {
        let size =
            u64::try_from(value.len()).map_err(|_| PayloadError::size_overflow("/value/str"))?;
        let mut error = ptr::null_mut();
        // SAFETY: value is borrowed for this synchronous call and error is valid.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_str(
                self.raw.as_ptr(),
                slice_pointer(value),
                size,
                &mut error,
            )
        };
        self.complete(status, error)
    }

    pub fn value_wstr(&mut self, value: &[u16]) -> Result<&mut Self, PayloadError> {
        let size =
            u64::try_from(value.len()).map_err(|_| PayloadError::size_overflow("/value/wstr"))?;
        let data = if value.is_empty() {
            ptr::null()
        } else {
            value.as_ptr()
        };
        let mut error = ptr::null_mut();
        // SAFETY: aligned u16 units are borrowed for this synchronous call.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_wstr(self.raw.as_ptr(), data, size, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_wstr_str(&mut self, value: &str) -> Result<&mut Self, PayloadError> {
        let units = value.encode_utf16().collect::<Vec<_>>();
        self.value_wstr(&units)
    }

    pub fn value_bytes(&mut self, value: &[u8]) -> Result<&mut Self, PayloadError> {
        let size =
            u64::try_from(value.len()).map_err(|_| PayloadError::size_overflow("/value/bytes"))?;
        let mut error = ptr::null_mut();
        // SAFETY: value is borrowed for this synchronous call and error is valid.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_bytes(
                self.raw.as_ptr(),
                slice_pointer(value),
                size,
                &mut error,
            )
        };
        self.complete(status, error)
    }

    pub fn value_fixed_run(&mut self, run: &FixedRun<'_>) -> Result<&mut Self, PayloadError> {
        let raw = run.to_raw()?;
        let mut error = ptr::null_mut();
        // SAFETY: raw and its borrowed spans stay live for this synchronous call.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_fixed_run(self.raw.as_ptr(), &raw, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_component_begin(&mut self) -> Result<&mut Self, PayloadError> {
        self.call_unary(sys::fdb_payload_v1_builder_value_component_begin)
    }

    pub fn value_list_begin(&mut self, item_count: u64) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_list_begin(self.raw.as_ptr(), item_count, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_object(&mut self, object: ObjectHandle) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: Core validates the opaque builder-local object token.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_object(self.raw.as_ptr(), object.raw, &mut error)
        };
        self.complete(status, error)
    }

    pub fn value_ref(&mut self, object: ObjectHandle) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: Core validates the opaque builder-local object token.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_ref(self.raw.as_ptr(), object.raw, &mut error)
        };
        self.complete(status, error)
    }

    pub fn freeze(&mut self) -> Result<BuildPlan, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique builder and both outputs are valid.
        let status =
            unsafe { sys::fdb_payload_v1_builder_freeze(self.raw.as_ptr(), &mut raw, &mut error) };
        check_status(status, error)?;
        BuildPlan::from_raw(raw)
    }

    fn call_unary(
        &mut self,
        operation: unsafe extern "C" fn(
            *mut sys::fdb_payload_v1_builder_t,
            *mut *mut sys::fdb_payload_v1_error_t,
        ) -> u32,
    ) -> Result<&mut Self, PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self owns the unique live builder and error is a valid sink.
        let status = unsafe { operation(self.raw.as_ptr(), &mut error) };
        self.complete(status, error)
    }

    fn complete(
        &mut self,
        status: u32,
        error: *mut sys::fdb_payload_v1_error_t,
    ) -> Result<&mut Self, PayloadError> {
        check_status(status, error)?;
        Ok(self)
    }
}

impl Drop for Builder {
    fn drop(&mut self) {
        // SAFETY: this releases exactly the unique builder owned by self.
        unsafe { sys::fdb_payload_v1_builder_release(self.raw.as_ptr()) };
    }
}

impl fmt::Debug for Builder {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("Builder").finish_non_exhaustive()
    }
}

pub struct BuildPlan {
    raw: NonNull<sys::fdb_payload_v1_plan_t>,
}

impl BuildPlan {
    fn from_raw(raw: *mut sys::fdb_payload_v1_plan_t) -> Result<Self, PayloadError> {
        NonNull::new(raw).map(|raw| Self { raw }).ok_or_else(|| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "",
                "FastDB Core returned success without a build plan",
                r#"{"reason":"missing_plan_handle"}"#,
            )
        })
    }

    pub fn info(&self) -> Result<PlanInfo, PayloadError> {
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_plan_info_t>::uninit();
        // SAFETY: Core initializes the complete V2 prefix in this valid output.
        unsafe { sys::fdb_payload_v1_plan_info_init(raw.as_mut_ptr()) };
        // SAFETY: the initializer writes the complete current structure.
        let mut raw = unsafe { raw.assume_init() };
        let mut error = ptr::null_mut();
        // SAFETY: self owns a live immutable plan and outputs are valid.
        let status =
            unsafe { sys::fdb_payload_v1_plan_info(self.raw.as_ptr(), &mut raw, &mut error) };
        check_status(status, error)?;
        Ok(PlanInfo {
            flags: raw.flags,
            total_bytes: raw.total_bytes,
            region_count: raw.region_count,
            logical_value_count: raw.logical_value_count,
            list_element_count: raw.list_element_count,
            text_bytes: raw.text_bytes,
            opaque_bytes: raw.opaque_bytes,
            validation_work: raw.validation_work,
            max_alignment: raw.max_alignment,
            direct_build_status: raw.direct_build_status,
            graph_object_count: raw.graph_object_count,
        })
    }

    pub(crate) fn as_raw(&self) -> *mut sys::fdb_payload_v1_plan_t {
        self.raw.as_ptr()
    }
}

impl Clone for BuildPlan {
    fn clone(&self) -> Self {
        // SAFETY: self owns a live immutable, atomically retainable plan.
        unsafe { sys::fdb_payload_v1_plan_retain(self.raw.as_ptr()) };
        Self { raw: self.raw }
    }
}

impl Drop for BuildPlan {
    fn drop(&mut self) {
        // SAFETY: this releases exactly the plan reference owned by self.
        unsafe { sys::fdb_payload_v1_plan_release(self.raw.as_ptr()) };
    }
}

impl fmt::Debug for BuildPlan {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("BuildPlan").finish_non_exhaustive()
    }
}

// Core plans are immutable and retain/release/info are explicitly thread-safe.
unsafe impl Send for BuildPlan {}
unsafe impl Sync for BuildPlan {}

fn slice_pointer(value: &[u8]) -> *const u8 {
    if value.is_empty() {
        ptr::null()
    } else {
        value.as_ptr()
    }
}
