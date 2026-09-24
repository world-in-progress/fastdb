// generated-by: fastdb.payload.codegen.v1
// payload-sha256: 92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71
// core-abi-version: 1
// generator-version: fastdb.payload.codegen.v1
// target: rust
#![allow(non_camel_case_types, non_upper_case_globals)]

pub const CANONICAL_SOURCE: &[u8] = br#"{"components":[],"entries":[],"profile":"record.v1","schema":"fastdb.payload.v1"}"#;
pub const PAYLOAD_SHA256: &str = "92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71";
pub const PAYLOAD_SHA256_BYTES: [u8; 32] = [0x92, 0xfb, 0xbc, 0x65, 0xfc, 0x79, 0xad, 0x9c, 0xa6, 0xe9, 0x63, 0x70, 0x63, 0xc8, 0xf1, 0x5a, 0x80, 0x6b, 0x40, 0xe7, 0x88, 0x53, 0x22, 0x25, 0xca, 0xfe, 0x25, 0xef, 0x17, 0xcd, 0xfc, 0x71];

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
