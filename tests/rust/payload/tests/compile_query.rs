use fastdb::{Capabilities, CompiledSpec, PayloadError, Profile};

const SOURCE: &[u8] =
    include_bytes!("../../../golden/payload/v1/spec/valid/record-all-types.source.json");
const CANONICAL_HEX: &str =
    include_str!("../../../golden/payload/v1/spec/valid/record-all-types.canonical.hex");
const MANIFEST_HEX: &str =
    include_str!("../../../golden/payload/v1/spec/valid/record-all-types.manifest.hex");
const DIGEST_HEX: &str =
    include_str!("../../../golden/payload/v1/spec/valid/record-all-types.sha256");
const INVALID_SOURCE: &[u8] =
    include_bytes!("../../../golden/payload/v1/spec/invalid/bad-kind.source.json");

fn decode_hex(source: &str) -> Vec<u8> {
    let source = source.trim();
    assert_eq!(source.len() % 2, 0);
    source
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let text = std::str::from_utf8(pair).expect("golden hex is ASCII");
            u8::from_str_radix(text, 16).expect("golden hex is valid")
        })
        .collect()
}

fn lower_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        output.push(char::from(HEX[usize::from(byte >> 4)]));
        output.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    output
}

#[test]
fn compile_and_query_record_all_types_through_core() -> Result<(), PayloadError> {
    let spec = CompiledSpec::compile(SOURCE)?;
    let cloned = spec.clone();

    assert_eq!(spec.canonical_json()?, decode_hex(CANONICAL_HEX));
    assert_eq!(spec.manifest_json()?, decode_hex(MANIFEST_HEX));
    assert_eq!(lower_hex(&spec.sha256()?), DIGEST_HEX.trim());
    assert_eq!(spec.profile()?, Profile::RecordV1);
    assert_eq!(
        spec.capabilities()?,
        Capabilities {
            profile: Profile::RecordV1,
            semantic_flags: 0x1b,
            operation_flags: 0xff,
            codegen_target_flags: 0x0f,
            direct_build_status: 1,
        }
    );

    assert_eq!(spec.entry_count()?, 2);
    assert_eq!(spec.entry_id(0)?, "single");
    assert_eq!(spec.entry_id(1)?, "series");
    assert_eq!(spec.entry_index("series")?, 1);
    assert_eq!(spec.component_count()?, 2);
    assert_eq!(spec.component_id(0)?, "AllTypes");
    assert_eq!(spec.component_id(1)?, "Leaf");
    assert_eq!(spec.component_index("Leaf")?, 1);
    assert_eq!(spec.component_field_count(0)?, 14);
    assert_eq!(spec.component_field_id(0, 9)?, "str_value");
    assert_eq!(spec.component_field_index(0, "list_value")?, 13);
    assert_eq!(cloned.sha256()?, spec.sha256()?);
    Ok(())
}

#[test]
fn compile_error_preserves_every_core_field() {
    let error = CompiledSpec::compile(INVALID_SOURCE)
        .expect_err("the invalid Core golden must be rejected");
    assert_eq!(error.code(), 1005);
    assert_eq!(error.symbol(), "INVALID_TYPE");
    assert_eq!(error.path(), "/entries/0/type/items/kind");
    assert_eq!(error.message(), "Payload type kind is invalid");
    assert_eq!(
        error.details_json(),
        r#"{"actual":"text","reason":"invalid_type_kind"}"#
    );
}
