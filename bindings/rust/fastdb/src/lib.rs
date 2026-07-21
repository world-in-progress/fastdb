//! Safe Rust projection of the FastDB portable-payload Core.

mod builder;

pub use builder::{BuildPlan, Builder, BuilderOptions, FixedRun, ObjectHandle, PlanInfo};

use fastdb_sys as sys;
use std::error::Error;
use std::fmt;
use std::ptr::{self, NonNull};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Profile {
    RecordV1,
    ObjectGraphV1,
}

impl Profile {
    fn from_raw(value: u32) -> Result<Self, PayloadError> {
        match value {
            sys::FDB_PAYLOAD_PROFILE_RECORD_V1 => Ok(Self::RecordV1),
            sys::FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1 => Ok(Self::ObjectGraphV1),
            _ => Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_UNSUPPORTED_ABI,
                "UNSUPPORTED_ABI",
                "/profile",
                "FastDB Core returned an unknown payload profile",
                format!(r#"{{"actual":{value},"reason":"unknown_profile"}}"#),
            )),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Capabilities {
    pub profile: Profile,
    pub semantic_flags: u64,
    pub operation_flags: u64,
    pub codegen_target_flags: u64,
    pub direct_build_status: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PayloadError {
    code: u32,
    symbol: String,
    path: String,
    message: String,
    details_json: String,
}

impl PayloadError {
    pub fn code(&self) -> u32 {
        self.code
    }

    pub fn symbol(&self) -> &str {
        &self.symbol
    }

    pub fn path(&self) -> &str {
        &self.path
    }

    pub fn message(&self) -> &str {
        &self.message
    }

    pub fn details_json(&self) -> &str {
        &self.details_json
    }

    pub(crate) fn binding(
        code: u32,
        symbol: impl Into<String>,
        path: impl Into<String>,
        message: impl Into<String>,
        details_json: impl Into<String>,
    ) -> Self {
        Self {
            code,
            symbol: symbol.into(),
            path: path.into(),
            message: message.into(),
            details_json: details_json.into(),
        }
    }

    unsafe fn from_raw(status: u32, raw: *mut sys::fdb_payload_v1_error_t) -> Self {
        if raw.is_null() {
            return Self::binding(
                if status == 0 {
                    sys::FDB_PAYLOAD_E_INTERNAL
                } else {
                    status
                },
                "BINDING_CONTRACT",
                "",
                "FastDB Core returned a failure without an owned error",
                r#"{"reason":"missing_error_handle"}"#,
            );
        }

        unsafe fn copy_field(
            raw: *const sys::fdb_payload_v1_error_t,
            accessor: unsafe extern "C" fn(
                *const sys::fdb_payload_v1_error_t,
                *mut *const u8,
                *mut u64,
            ),
        ) -> String {
            let mut data = ptr::null();
            let mut size = 0_u64;
            // SAFETY: raw is an owned live Core error and the outputs are valid.
            unsafe { accessor(raw, &mut data, &mut size) };
            let Ok(size) = usize::try_from(size) else {
                return "<FastDB error field exceeds the host address space>".to_owned();
            };
            if size == 0 {
                return String::new();
            }
            if data.is_null() {
                return "<FastDB error field has null storage>".to_owned();
            }
            // SAFETY: Core keeps the field alive until the owned error is released.
            let bytes = unsafe { std::slice::from_raw_parts(data, size) };
            String::from_utf8_lossy(bytes).into_owned()
        }

        // SAFETY: raw is non-null and remains owned until the final release below.
        let code = unsafe { sys::fdb_payload_v1_error_code(raw) };
        // SAFETY: each accessor only borrows from the same live error handle.
        let symbol = unsafe { copy_field(raw, sys::fdb_payload_v1_error_symbol) };
        let path = unsafe { copy_field(raw, sys::fdb_payload_v1_error_path) };
        let message = unsafe { copy_field(raw, sys::fdb_payload_v1_error_message) };
        let details_json = unsafe { copy_field(raw, sys::fdb_payload_v1_error_details_json) };
        // SAFETY: this consumes the one error reference returned by Core.
        unsafe { sys::fdb_payload_v1_error_release(raw) };
        Self {
            code,
            symbol,
            path,
            message,
            details_json,
        }
    }

    pub(crate) fn size_overflow(path: &'static str) -> Self {
        Self::binding(
            sys::FDB_PAYLOAD_E_INTERNAL,
            "BINDING_SIZE_OVERFLOW",
            path,
            "A host value cannot be represented by the FastDB ABI",
            r#"{"reason":"host_size_overflow"}"#,
        )
    }
}

impl fmt::Display for PayloadError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{} ({}) at {}: {}",
            self.symbol, self.code, self.path, self.message
        )
    }
}

impl Error for PayloadError {}

pub struct Blob {
    raw: NonNull<sys::fdb_payload_v1_blob_t>,
}

impl Blob {
    unsafe fn from_raw(raw: *mut sys::fdb_payload_v1_blob_t) -> Result<Self, PayloadError> {
        NonNull::new(raw).map(|raw| Self { raw }).ok_or_else(|| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "",
                "FastDB Core returned success without a blob",
                r#"{"reason":"missing_blob_handle"}"#,
            )
        })
    }

    pub fn as_bytes(&self) -> Result<&[u8], PayloadError> {
        // SAFETY: self owns a live immutable blob handle.
        let size = unsafe { sys::fdb_payload_v1_blob_size(self.raw.as_ptr()) };
        let size = usize::try_from(size).map_err(|_| PayloadError::size_overflow("/blob"))?;
        // SAFETY: self keeps the Core blob storage alive for the returned borrow.
        let data = unsafe { sys::fdb_payload_v1_blob_data(self.raw.as_ptr()) };
        if size == 0 {
            return Ok(&[]);
        }
        if data.is_null() {
            return Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "/blob",
                "FastDB Core returned a non-empty blob with null storage",
                r#"{"reason":"null_blob_storage"}"#,
            ));
        }
        // SAFETY: Core reports a live byte span of exactly size bytes.
        Ok(unsafe { std::slice::from_raw_parts(data, size) })
    }

    pub fn to_vec(&self) -> Result<Vec<u8>, PayloadError> {
        Ok(self.as_bytes()?.to_vec())
    }
}

impl Clone for Blob {
    fn clone(&self) -> Self {
        // SAFETY: self owns one live reference and Core blobs are retainable.
        unsafe { sys::fdb_payload_v1_blob_retain(self.raw.as_ptr()) };
        Self { raw: self.raw }
    }
}

impl Drop for Blob {
    fn drop(&mut self) {
        // SAFETY: this releases exactly the reference owned by self.
        unsafe { sys::fdb_payload_v1_blob_release(self.raw.as_ptr()) };
    }
}

// Core blobs are immutable and their retain/release operations are thread-safe.
unsafe impl Send for Blob {}
unsafe impl Sync for Blob {}

pub struct CompiledSpec {
    raw: NonNull<sys::fdb_payload_v1_spec_t>,
}

impl CompiledSpec {
    pub fn compile(source: &[u8]) -> Result<Self, PayloadError> {
        // SAFETY: this function has no pointer arguments.
        let abi_version = unsafe { sys::fdb_payload_v1_abi_version() };
        if abi_version != sys::FDB_PAYLOAD_V1_ABI_VERSION {
            return Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_UNSUPPORTED_ABI,
                "UNSUPPORTED_ABI",
                "/abi_version",
                "The loaded FastDB payload ABI is incompatible",
                format!(
                    r#"{{"actual":{abi_version},"expected":{}}}"#,
                    sys::FDB_PAYLOAD_V1_ABI_VERSION
                ),
            ));
        }
        let source_size =
            u64::try_from(source.len()).map_err(|_| PayloadError::size_overflow("/source"))?;
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: source is borrowed for the call and both output locations are valid.
        let status = unsafe {
            sys::fdb_payload_v1_spec_compile_json(
                source.as_ptr(),
                source_size,
                ptr::null(),
                &mut raw,
                &mut error,
            )
        };
        check_status(status, error)?;
        NonNull::new(raw).map(|raw| Self { raw }).ok_or_else(|| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "",
                "FastDB Core returned success without a compiled spec",
                r#"{"reason":"missing_spec_handle"}"#,
            )
        })
    }

    pub fn canonical_blob(&self) -> Result<Blob, PayloadError> {
        self.query_blob(|spec, out, error| unsafe {
            sys::fdb_payload_v1_spec_canonical_json(spec, out, error)
        })
    }

    pub fn canonical_json(&self) -> Result<Vec<u8>, PayloadError> {
        self.canonical_blob()?.to_vec()
    }

    pub fn manifest_blob(&self) -> Result<Blob, PayloadError> {
        self.query_blob(|spec, out, error| unsafe {
            sys::fdb_payload_v1_spec_manifest_json(spec, out, error)
        })
    }

    pub fn manifest_json(&self) -> Result<Vec<u8>, PayloadError> {
        self.manifest_blob()?.to_vec()
    }

    pub fn sha256(&self) -> Result<[u8; 32], PayloadError> {
        let mut digest = [0_u8; sys::FDB_PAYLOAD_V1_SHA256_SIZE as usize];
        let mut error = ptr::null_mut();
        // SAFETY: self is live, digest has the ABI-required length, and error is valid.
        let status = unsafe {
            sys::fdb_payload_v1_spec_sha256(self.raw.as_ptr(), digest.as_mut_ptr(), &mut error)
        };
        check_status(status, error)?;
        Ok(digest)
    }

    pub fn profile(&self) -> Result<Profile, PayloadError> {
        let mut profile = 0_u32;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and both output locations are valid.
        let status = unsafe {
            sys::fdb_payload_v1_spec_profile(self.raw.as_ptr(), &mut profile, &mut error)
        };
        check_status(status, error)?;
        Profile::from_raw(profile)
    }

    pub fn capabilities(&self) -> Result<Capabilities, PayloadError> {
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_capabilities_t>::uninit();
        // SAFETY: Core initializes the full V1 prefix in this valid output location.
        unsafe { sys::fdb_payload_v1_capabilities_init(raw.as_mut_ptr()) };
        // SAFETY: the init function initialized the complete V1 structure.
        let mut raw = unsafe { raw.assume_init() };
        let mut error = ptr::null_mut();
        // SAFETY: self is live and both output locations are valid.
        let status = unsafe {
            sys::fdb_payload_v1_spec_capabilities(self.raw.as_ptr(), &mut raw, &mut error)
        };
        check_status(status, error)?;
        Ok(Capabilities {
            profile: Profile::from_raw(raw.profile)?,
            semantic_flags: raw.semantic_flags,
            operation_flags: raw.operation_flags,
            codegen_target_flags: raw.codegen_target_flags,
            direct_build_status: raw.direct_build_status,
        })
    }

    pub fn entry_count(&self) -> Result<u32, PayloadError> {
        self.query_u32(|out, error| unsafe {
            sys::fdb_payload_v1_spec_entry_count(self.raw.as_ptr(), out, error)
        })
    }

    pub fn entry_id(&self, index: u32) -> Result<String, PayloadError> {
        let blob = self.query_blob(|spec, out, error| unsafe {
            sys::fdb_payload_v1_spec_entry_id(spec, index, out, error)
        })?;
        string_from_blob(blob, "/entries/id")
    }

    pub fn entry_index(&self, id: &str) -> Result<u32, PayloadError> {
        self.query_id(id, |data, size, out, error| unsafe {
            sys::fdb_payload_v1_spec_entry_index(self.raw.as_ptr(), data, size, out, error)
        })
    }

    pub fn component_count(&self) -> Result<u32, PayloadError> {
        self.query_u32(|out, error| unsafe {
            sys::fdb_payload_v1_spec_component_count(self.raw.as_ptr(), out, error)
        })
    }

    pub fn component_id(&self, index: u32) -> Result<String, PayloadError> {
        let blob = self.query_blob(|spec, out, error| unsafe {
            sys::fdb_payload_v1_spec_component_id(spec, index, out, error)
        })?;
        string_from_blob(blob, "/components/id")
    }

    pub fn component_index(&self, id: &str) -> Result<u32, PayloadError> {
        self.query_id(id, |data, size, out, error| unsafe {
            sys::fdb_payload_v1_spec_component_index(self.raw.as_ptr(), data, size, out, error)
        })
    }

    pub fn component_field_count(&self, component_index: u32) -> Result<u32, PayloadError> {
        self.query_u32(|out, error| unsafe {
            sys::fdb_payload_v1_spec_component_field_count(
                self.raw.as_ptr(),
                component_index,
                out,
                error,
            )
        })
    }

    pub fn component_field_id(
        &self,
        component_index: u32,
        field_index: u32,
    ) -> Result<String, PayloadError> {
        let blob = self.query_blob(|spec, out, error| unsafe {
            sys::fdb_payload_v1_spec_component_field_id(
                spec,
                component_index,
                field_index,
                out,
                error,
            )
        })?;
        string_from_blob(blob, "/components/fields/id")
    }

    pub fn component_field_index(
        &self,
        component_index: u32,
        id: &str,
    ) -> Result<u32, PayloadError> {
        self.query_id(id, |data, size, out, error| unsafe {
            sys::fdb_payload_v1_spec_component_field_index(
                self.raw.as_ptr(),
                component_index,
                data,
                size,
                out,
                error,
            )
        })
    }

    fn query_blob<F>(&self, call: F) -> Result<Blob, PayloadError>
    where
        F: FnOnce(
            *const sys::fdb_payload_v1_spec_t,
            *mut *mut sys::fdb_payload_v1_blob_t,
            *mut *mut sys::fdb_payload_v1_error_t,
        ) -> u32,
    {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        let status = call(self.raw.as_ptr(), &mut raw, &mut error);
        check_status(status, error)?;
        // SAFETY: a successful Core blob query publishes one owned handle.
        unsafe { Blob::from_raw(raw) }
    }

    fn query_u32<F>(&self, call: F) -> Result<u32, PayloadError>
    where
        F: FnOnce(*mut u32, *mut *mut sys::fdb_payload_v1_error_t) -> u32,
    {
        let mut value = 0_u32;
        let mut error = ptr::null_mut();
        let status = call(&mut value, &mut error);
        check_status(status, error)?;
        Ok(value)
    }

    fn query_id<F>(&self, id: &str, call: F) -> Result<u32, PayloadError>
    where
        F: FnOnce(*const u8, u64, *mut u32, *mut *mut sys::fdb_payload_v1_error_t) -> u32,
    {
        let size = u64::try_from(id.len()).map_err(|_| PayloadError::size_overflow("/id"))?;
        let mut value = 0_u32;
        let mut error = ptr::null_mut();
        let status = call(id.as_ptr(), size, &mut value, &mut error);
        check_status(status, error)?;
        Ok(value)
    }

    pub(crate) fn as_raw(&self) -> *mut sys::fdb_payload_v1_spec_t {
        self.raw.as_ptr()
    }
}

impl Clone for CompiledSpec {
    fn clone(&self) -> Self {
        // SAFETY: self owns a live immutable, retainable Core spec.
        unsafe { sys::fdb_payload_v1_spec_retain(self.raw.as_ptr()) };
        Self { raw: self.raw }
    }
}

impl Drop for CompiledSpec {
    fn drop(&mut self) {
        // SAFETY: this releases exactly the reference owned by self.
        unsafe { sys::fdb_payload_v1_spec_release(self.raw.as_ptr()) };
    }
}

impl fmt::Debug for CompiledSpec {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("CompiledSpec")
            .finish_non_exhaustive()
    }
}

// Core compiled specs are immutable; their queries and reference count are
// explicitly thread-safe in the stable C contract.
unsafe impl Send for CompiledSpec {}
unsafe impl Sync for CompiledSpec {}

pub(crate) fn check_status(
    status: u32,
    raw_error: *mut sys::fdb_payload_v1_error_t,
) -> Result<(), PayloadError> {
    if status == 0 {
        if !raw_error.is_null() {
            // SAFETY: even a contract-violating non-null output is still an owned Core error.
            unsafe { sys::fdb_payload_v1_error_release(raw_error) };
            return Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "",
                "FastDB Core returned success with an error handle",
                r#"{"reason":"unexpected_error_handle"}"#,
            ));
        }
        Ok(())
    } else {
        // SAFETY: the stable ABI returns one owned error on every failure.
        Err(unsafe { PayloadError::from_raw(status, raw_error) })
    }
}

fn string_from_blob(blob: Blob, path: &'static str) -> Result<String, PayloadError> {
    String::from_utf8(blob.to_vec()?).map_err(|_| {
        PayloadError::binding(
            sys::FDB_PAYLOAD_E_INTERNAL,
            "BINDING_INVALID_UTF8",
            path,
            "FastDB Core returned a non-UTF-8 identifier",
            r#"{"reason":"invalid_utf8_identifier"}"#,
        )
    })
}
