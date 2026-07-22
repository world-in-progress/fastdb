// generated-by: fastdb.payload.codegen.v1
// payload-sha256: 92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71
// core-abi-version: 1
// generator-version: fastdb.payload.codegen.v1
// target: rust
#![allow(non_camel_case_types, non_upper_case_globals)]

pub const CANONICAL_SOURCE: &[u8] = br#"{"components":[],"entries":[],"profile":"record.v1","schema":"fastdb.payload.v1"}"#;
pub const PAYLOAD_SHA256: &str = "92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71";

pub fn compile_spec() -> Result<fastdb::CompiledSpec, fastdb::PayloadError> {
    fastdb::CompiledSpec::compile(CANONICAL_SOURCE)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct IdMetadata {
    pub symbol: &'static str,
    pub original_id: &'static str,
    pub stable_index: u32,
}

pub const ENTRIES: &[IdMetadata] = &[
];

pub const COMPONENTS: &[IdMetadata] = &[
];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FieldMetadata {
    pub symbol: &'static str,
    pub original_id: &'static str,
    pub component_index: u32,
    pub field_index: u32,
}

pub const FIELDS: &[FieldMetadata] = &[
];
