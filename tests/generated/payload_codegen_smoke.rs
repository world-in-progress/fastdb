mod generated;

use fastdb::{BuildPolicy, Builder, CompiledSpec, PayloadError};

const OTHER_SOURCE: &[u8] = br#"{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"other","cardinality":"one","type":{"kind":"component","id":"Other"}}],"components":[{"id":"Other","kind":"record","fields":[{"id":"different","type":{"kind":"u8"}}]}]}"#;

fn require_mismatch(error: PayloadError, path: &str) {
    assert_eq!(error.code(), 3006);
    assert_eq!(error.symbol(), "DIGEST_MISMATCH");
    assert_eq!(error.path(), path);
    assert!(
        error
            .details_json()
            .contains("\"reason\":\"spec_digest_mismatch\"")
    );
}

fn main() -> Result<(), PayloadError> {
    let wrong_spec = CompiledSpec::compile(OTHER_SOURCE)?;
    let mut wrong_builder = Builder::create(&wrong_spec)?;
    let builder_error =
        generated::fdb_rust_id_726f6f74_builder_entry_begin(
            &mut wrong_builder,
            1,
        )
        .expect_err("generated builder helper accepted a foreign spec");
    require_mismatch(builder_error, "/builder/spec_sha256");
    wrong_builder
        .entry_begin(0, 1)?
        .value_component_begin()?
        .value_u8(9)?;
    let wrong_payload = wrong_builder
        .freeze()?
        .execute(BuildPolicy::AllowStaging)?
        .payload;
    let payload_error =
        generated::fdb_rust_id_726f6f74_from_payload(&wrong_payload)
            .err()
            .expect("generated payload factory accepted a foreign spec");
    require_mismatch(payload_error, "/payload/spec_sha256");
    let wrong_view = wrong_payload.entry_view(0)?.at(0)?;
    let view_error =
        generated::FdbRustType_fdb_rust_id_4974656d_View::try_from_view(
            wrong_view,
        )
        .err()
        .expect("generated component constructor accepted a foreign spec");
    require_mismatch(view_error, "/view/spec_sha256");

    let spec = generated::compile_spec()?;
    let mut builder = Builder::create(&spec)?;
    generated::fdb_rust_id_726f6f74_builder_entry_begin(&mut builder, 1)?
        .value_component_begin()?
        .value_u8(7)?;
    let payload = builder
        .freeze()?
        .execute(BuildPolicy::AllowStaging)?
        .payload;
    let entry = generated::fdb_rust_id_726f6f74_from_payload(&payload)?;
    let component =
        generated::FdbRustType_fdb_rust_id_4974656d_View::try_from_view(
            entry.at(0)?,
        )?
        .expect("generated component index");
    assert_eq!(component.fdb_rust_id_76616c7565_value()?, 7);
    Ok(())
}
