use crate::{Blob, CompiledSpec, PayloadError, check_status};
use fastdb_sys as sys;
use std::ptr::{self, NonNull};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u64)]
pub enum CodegenTarget {
    Cpp = sys::FDB_PAYLOAD_CODEGEN_TARGET_CPP,
    Rust = sys::FDB_PAYLOAD_CODEGEN_TARGET_RUST,
    Python = sys::FDB_PAYLOAD_CODEGEN_TARGET_PYTHON,
    TypeScript = sys::FDB_PAYLOAD_CODEGEN_TARGET_TYPESCRIPT,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum ArtifactKind {
    Source = sys::FDB_PAYLOAD_ARTIFACT_SOURCE,
}

impl ArtifactKind {
    fn from_raw(value: u32) -> Result<Self, PayloadError> {
        match value {
            sys::FDB_PAYLOAD_ARTIFACT_SOURCE => Ok(Self::Source),
            _ => Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_UNSUPPORTED_ABI,
                "UNSUPPORTED_ABI",
                "/codegen/artifacts/kind",
                "FastDB Core returned an unknown artifact kind",
                format!(r#"{{"actual":{value},"reason":"unknown_artifact_kind"}}"#),
            )),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CodegenOptions {
    pub flags: u32,
    pub max_artifacts: u64,
    pub max_total_bytes: u64,
}

impl Default for CodegenOptions {
    fn default() -> Self {
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_codegen_options_t>::uninit();
        // SAFETY: Core initializes the complete V1 options prefix.
        unsafe { sys::fdb_payload_v1_codegen_options_init(raw.as_mut_ptr()) };
        // SAFETY: the initializer wrote the complete current structure.
        let raw = unsafe { raw.assume_init() };
        Self {
            flags: raw.flags,
            max_artifacts: raw.max_artifacts,
            max_total_bytes: raw.max_total_bytes,
        }
    }
}

impl CodegenOptions {
    fn to_raw(&self) -> sys::fdb_payload_v1_codegen_options_t {
        sys::fdb_payload_v1_codegen_options_t {
            struct_size: sys::FDB_PAYLOAD_V1_CODEGEN_OPTIONS_V1_SIZE,
            flags: self.flags,
            max_artifacts: self.max_artifacts,
            max_total_bytes: self.max_total_bytes,
            reserved: [0; 3],
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Artifact {
    pub relative_path: String,
    pub kind: ArtifactKind,
    pub bytes: Vec<u8>,
    pub sha256: [u8; 32],
}

#[derive(Debug)]
pub struct ArtifactSet {
    raw: NonNull<sys::fdb_payload_v1_codegen_result_t>,
}

impl ArtifactSet {
    fn from_raw(raw: *mut sys::fdb_payload_v1_codegen_result_t) -> Result<Self, PayloadError> {
        NonNull::new(raw).map(|raw| Self { raw }).ok_or_else(|| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "/codegen",
                "FastDB Core returned success without an artifact set",
                r#"{"reason":"missing_codegen_result"}"#,
            )
        })
    }

    pub fn len(&self) -> Result<u64, PayloadError> {
        let mut count = 0_u64;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and both outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_codegen_result_artifact_count(
                self.raw.as_ptr(),
                &mut count,
                &mut error,
            )
        };
        check_status(status, error)?;
        Ok(count)
    }

    pub fn is_empty(&self) -> Result<bool, PayloadError> {
        Ok(self.len()? == 0)
    }

    pub fn artifact(&self, index: u64) -> Result<Artifact, PayloadError> {
        let relative_path = self.query_blob(index, |result, index, output, error| unsafe {
            sys::fdb_payload_v1_codegen_result_artifact_relative_path(result, index, output, error)
        })?;
        let relative_path = String::from_utf8(relative_path.to_vec()?).map_err(|_| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_INVALID_UTF8",
                "/codegen/artifacts/relative_path",
                "FastDB Core returned a non-UTF-8 artifact path",
                r#"{"reason":"invalid_utf8_artifact_path"}"#,
            )
        })?;

        let mut kind = 0_u32;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and both outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_codegen_result_artifact_kind(
                self.raw.as_ptr(),
                index,
                &mut kind,
                &mut error,
            )
        };
        check_status(status, error)?;
        let kind = ArtifactKind::from_raw(kind)?;

        let bytes = self
            .query_blob(index, |result, index, output, error| unsafe {
                sys::fdb_payload_v1_codegen_result_artifact_bytes(result, index, output, error)
            })?
            .to_vec()?;
        let mut sha256 = [0_u8; sys::FDB_PAYLOAD_V1_SHA256_SIZE as usize];
        let mut error = ptr::null_mut();
        // SAFETY: self is live and the fixed-size digest/error outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_codegen_result_artifact_sha256(
                self.raw.as_ptr(),
                index,
                sha256.as_mut_ptr(),
                &mut error,
            )
        };
        check_status(status, error)?;
        Ok(Artifact {
            relative_path,
            kind,
            bytes,
            sha256,
        })
    }

    fn query_blob<F>(&self, index: u64, call: F) -> Result<Blob, PayloadError>
    where
        F: FnOnce(
            *const sys::fdb_payload_v1_codegen_result_t,
            u64,
            *mut *mut sys::fdb_payload_v1_blob_t,
            *mut *mut sys::fdb_payload_v1_error_t,
        ) -> u32,
    {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        let status = call(self.raw.as_ptr(), index, &mut raw, &mut error);
        check_status(status, error)?;
        // SAFETY: success publishes one owned immutable blob.
        unsafe { Blob::from_raw(raw) }
    }
}

impl Clone for ArtifactSet {
    fn clone(&self) -> Self {
        // SAFETY: self owns a live immutable, retainable Core result.
        unsafe { sys::fdb_payload_v1_codegen_result_retain(self.raw.as_ptr()) };
        Self { raw: self.raw }
    }
}

impl Drop for ArtifactSet {
    fn drop(&mut self) {
        // SAFETY: this releases exactly the reference owned by self.
        unsafe { sys::fdb_payload_v1_codegen_result_release(self.raw.as_ptr()) };
    }
}

// Core artifact sets are immutable and their query/reference-count operations
// are explicitly thread-safe in the stable C contract.
unsafe impl Send for ArtifactSet {}
unsafe impl Sync for ArtifactSet {}

impl CompiledSpec {
    pub fn generate(
        &self,
        target: CodegenTarget,
        options: &CodegenOptions,
    ) -> Result<ArtifactSet, PayloadError> {
        let options = options.to_raw();
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self/options stay live for the synchronous call and outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_spec_codegen(
                self.as_raw(),
                target as u64,
                &options,
                &mut raw,
                &mut error,
            )
        };
        check_status(status, error)?;
        ArtifactSet::from_raw(raw)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{BuildPolicy, Builder};
    use std::sync::Arc;

    const EMPTY_SPEC: &[u8] =
        br#"{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]}"#;
    const SPEC_A: &[u8] = br#"{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"value","cardinality":"one","type":{"kind":"u8"}}],"components":[]}"#;
    const SPEC_B: &[u8] = br#"{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"other","cardinality":"one","type":{"kind":"u8"}}],"components":[]}"#;

    fn lower_hex(bytes: &[u8]) -> String {
        const HEX: &[u8; 16] = b"0123456789abcdef";
        let mut output = String::with_capacity(bytes.len() * 2);
        for byte in bytes {
            output.push(char::from(HEX[usize::from(byte >> 4)]));
            output.push(char::from(HEX[usize::from(byte & 0x0f)]));
        }
        output
    }

    fn require_digest_mismatch(
        error: &PayloadError,
        path: &str,
        actual: &[u8; 32],
        expected: &[u8; 32],
    ) {
        assert_eq!(error.code(), sys::FDB_PAYLOAD_E_DIGEST_MISMATCH);
        assert_eq!(error.symbol(), "DIGEST_MISMATCH");
        assert_eq!(error.path(), path);
        assert_eq!(
            error.message(),
            "Portable payload spec digest does not match"
        );
        assert_eq!(
            error.details_json(),
            format!(
                r#"{{"actual":"{}","expected":"{}","reason":"spec_digest_mismatch"}}"#,
                lower_hex(actual),
                lower_hex(expected)
            )
        );
    }

    #[test]
    fn core_codegen_projects_owned_artifacts_and_limits() {
        let spec = CompiledSpec::compile(EMPTY_SPEC).expect("compile");
        for target in [
            CodegenTarget::Cpp,
            CodegenTarget::Rust,
            CodegenTarget::Python,
            CodegenTarget::TypeScript,
        ] {
            let set = spec
                .generate(target, &CodegenOptions::default())
                .expect("generate");
            assert_eq!(set.len().expect("count"), 1);
            let artifact = set.artifact(0).expect("artifact");
            drop(set);
            assert_eq!(artifact.kind, ArtifactKind::Source);
            assert!(!artifact.relative_path.is_empty());
            assert!(!artifact.bytes.is_empty());
            assert!(artifact.sha256.iter().any(|byte| *byte != 0));
        }

        let shared = Arc::new(
            spec.generate(CodegenTarget::Rust, &CodegenOptions::default())
                .expect("shared generation"),
        );
        std::thread::scope(|scope| {
            for _ in 0..8 {
                let local = Arc::clone(&shared);
                scope.spawn(move || {
                    for _ in 0..32 {
                        assert_eq!(local.len().expect("shared count"), 1);
                        let artifact = local.artifact(0).expect("shared artifact");
                        assert_eq!(artifact.kind, ArtifactKind::Source);
                        assert!(!artifact.bytes.is_empty());
                    }
                });
            }
        });

        let options = CodegenOptions {
            max_artifacts: 0,
            ..CodegenOptions::default()
        };
        let error = spec
            .generate(CodegenTarget::Cpp, &options)
            .expect_err("zero inventory must fail");
        assert_eq!(error.code(), sys::FDB_PAYLOAD_E_GENERATOR_FAILED);
        assert_eq!(error.path(), "/codegen/limits/max_artifacts");
    }

    #[test]
    fn core_provenance_guards_reject_same_indexes_from_another_spec() {
        let spec_a = CompiledSpec::compile(SPEC_A).expect("compile spec A");
        let spec_b = CompiledSpec::compile(SPEC_B).expect("compile spec B");
        let digest_a = spec_a.sha256().expect("digest A");
        let digest_b = spec_b.sha256().expect("digest B");

        let mut builder = Builder::create(&spec_a).expect("builder");
        builder
            .require_spec_sha256(&digest_a)
            .expect("own builder digest");
        let builder_error = builder
            .require_spec_sha256(&digest_b)
            .expect_err("foreign builder digest");
        require_digest_mismatch(&builder_error, "/builder/spec_sha256", &digest_a, &digest_b);

        builder
            .entry_begin(0, 1)
            .expect("entry")
            .value_u8(7)
            .expect("value");
        let payload = builder
            .freeze()
            .expect("freeze")
            .execute(BuildPolicy::AllowStaging)
            .expect("execute")
            .payload;
        payload
            .require_spec_sha256(&digest_a)
            .expect("own payload digest");
        let payload_error = payload
            .require_spec_sha256(&digest_b)
            .expect_err("foreign payload digest");
        require_digest_mismatch(&payload_error, "/payload/spec_sha256", &digest_a, &digest_b);

        let view = payload.entry_view(0).expect("entry view");
        let detached = view.materialize().expect("materialize");
        for candidate in [&view, &detached] {
            candidate
                .require_spec_sha256(&digest_a)
                .expect("own view digest");
            let view_error = candidate
                .require_spec_sha256(&digest_b)
                .expect_err("foreign view digest");
            require_digest_mismatch(&view_error, "/view/spec_sha256", &digest_a, &digest_b);
        }
    }
}
