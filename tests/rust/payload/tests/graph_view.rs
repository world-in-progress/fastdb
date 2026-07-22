use fastdb::{
    BuildPolicy, Builder, CompiledSpec, GraphIdentity, Payload, PayloadError, View, ViewKind,
};
use std::thread;

const GRAPH_SPEC: &[u8] =
    include_bytes!("../../../golden/payload/v1/binary/spec/graph-all-values.source.json");
const DISCONNECTED_SPEC: &[u8] =
    include_bytes!("../../../golden/payload/v1/binary/spec/graph-disconnected-roots.source.json");

struct GraphFixture {
    payload: Payload,
    node_index: u32,
    inline_index: u32,
    asset_index: u32,
}

fn graph_payload() -> Result<GraphFixture, PayloadError> {
    let spec = CompiledSpec::compile(GRAPH_SPEC)?;
    let node_index = spec.component_index("Node")?;
    let inline_index = spec.component_index("Inline")?;
    let asset_index = spec.component_index("Asset")?;
    let mut builder = Builder::create(&spec)?;
    let node = builder.declare_object(node_index)?;
    let asset = builder.declare_object(asset_index)?;

    builder
        .object_fill_begin(node)?
        .value_bool(true)?
        .value_u8(0x12)?
        .value_u16(0x3456)?
        .value_u32(0x789a_bcde)?
        .value_i32(-1_234_567)?
        .value_u8n_bits(0x3fe0_0000_0000_0000)?
        .value_u16n_bits(0)?
        .value_f32_bits(0x7fa1_2345)?
        .value_f64_bits(0xfff8_0000_0000_1234)?
        .value_str("same")?
        .value_wstr(&[0x0041, 0xd83d, 0xde00])?
        .value_bytes(&[0x00, 0xff, 0x7e])?
        .value_component_begin()?
        .value_null()?
        .value_u16(0xbeef)?
        .value_list_begin(3)?
        .value_f32_bits(0x8000_0000)?
        .value_null()?
        .value_f32_bits(0xff80_0001)?
        .value_ref(node)?
        .value_ref(asset)?
        .object_fill_begin(asset)?
        .value_str("same")?
        .value_ref(node)?
        .entry_begin(0, 1)?
        .value_object(node)?
        .entry_begin(1, 1)?
        .value_object(asset)?
        .entry_begin(2, 2)?
        .value_ref(node)?
        .value_null()?
        .entry_begin(3, 3)?
        .value_u8n_bits(0)?
        .value_u8n_bits(0x3fe0_0000_0000_0000)?
        .value_u8n_bits(0x3ff0_0000_0000_0000)?
        .entry_begin(4, 3)?
        .value_u16n_bits(0xbff0_0000_0000_0000)?
        .value_u16n_bits(0)?
        .value_u16n_bits(0x3ff0_0000_0000_0000)?;

    let payload = builder
        .freeze()?
        .execute(BuildPolicy::AllowStaging)?
        .payload;
    Ok(GraphFixture {
        payload,
        node_index,
        inline_index,
        asset_index,
    })
}

fn root(payload: &Payload, entry_index: u32) -> Result<View, PayloadError> {
    payload.entry_view(entry_index)?.at(0)
}

fn require_error(
    error: PayloadError,
    code: u32,
    symbol: &str,
    path: &str,
    message: &str,
    details: &str,
) {
    assert_eq!(error.code(), code);
    assert_eq!(error.symbol(), symbol);
    assert_eq!(error.path(), path);
    assert_eq!(error.message(), message);
    assert_eq!(error.details_json(), details);
}

#[test]
fn graph_identity_refs_cycles_and_materialized_closure_are_core_owned() -> Result<(), PayloadError>
{
    let fixture = graph_payload()?;
    let payload = fixture.payload;
    let root = root(&payload, 0)?;
    let root_identity = GraphIdentity {
        component_index: fixture.node_index,
        object_id: 0,
    };
    assert_eq!(root.graph_identity()?, root_identity);

    let inline = root.field(12)?;
    assert_eq!(inline.component_index()?, fixture.inline_index);
    require_error(
        inline.graph_identity().unwrap_err(),
        2004,
        "TYPE_MISMATCH",
        "/entries/0/0/fields/12",
        "Portable payload view kind does not match the operation",
        r#"{"reason":"view_kind_mismatch"}"#,
    );

    let self_ref = root.field(14)?;
    assert_eq!(self_ref.kind()?, ViewKind::Ref);
    assert_eq!(self_ref.graph_identity()?, root_identity);
    require_error(
        self_ref.field(0).unwrap_err(),
        2004,
        "TYPE_MISMATCH",
        "/entries/0/0/fields/14",
        "Portable payload view kind does not match the operation",
        r#"{"reason":"view_kind_mismatch"}"#,
    );
    let self_target = self_ref.ref_target()?;
    assert_eq!(self_target.graph_identity()?, root_identity);

    let asset_ref = root.field(15)?;
    let asset_identity = GraphIdentity {
        component_index: fixture.asset_index,
        object_id: 0,
    };
    assert_eq!(asset_ref.graph_identity()?, asset_identity);
    let asset = asset_ref.ref_target()?;
    assert_eq!(asset.graph_identity()?, asset_identity);
    let owner_ref = asset.field(1)?;
    assert_eq!(owner_ref.graph_identity()?, root_identity);
    assert_eq!(owner_ref.ref_target()?.graph_identity()?, root_identity);

    let shared_ref = payload.entry_view(2)?.at(0)?;
    assert_eq!(shared_ref.graph_identity()?, root_identity);
    assert_eq!(shared_ref.ref_target()?.graph_identity()?, root_identity);
    let null_ref = payload.entry_view(2)?.at(1)?;
    assert!(null_ref.is_null()?);
    require_error(
        null_ref.ref_target().unwrap_err(),
        2003,
        "UNEXPECTED_NULL",
        "/entries/2/1",
        "Portable payload view is null",
        r#"{"reason":"unexpected_null"}"#,
    );
    require_error(
        null_ref.graph_identity().unwrap_err(),
        2003,
        "UNEXPECTED_NULL",
        "/entries/2/1",
        "Portable payload view is null",
        r#"{"reason":"unexpected_null"}"#,
    );

    let mut workers = Vec::new();
    for _ in 0..4 {
        let worker_root = root.clone();
        let worker_ref = asset_ref.clone();
        workers.push(thread::spawn(move || -> Result<(), PayloadError> {
            for _ in 0..250 {
                assert_eq!(worker_root.graph_identity()?.object_id, 0);
                assert_eq!(worker_ref.ref_target()?.graph_identity()?.object_id, 0);
            }
            Ok(())
        }));
    }
    for worker in workers {
        worker.join().expect("graph view worker panicked")?;
    }

    let detached = root.materialize()?;
    assert_eq!(detached.graph_identity()?, root_identity);
    payload.invalidate()?;
    require_error(
        root.graph_identity().unwrap_err(),
        4001,
        "VIEW_INVALIDATED",
        "/view",
        "Portable payload access barrier rejected the operation",
        r#"{"reason":"view_invalidated"}"#,
    );
    require_error(
        asset_ref.ref_target().unwrap_err(),
        4001,
        "VIEW_INVALIDATED",
        "/view",
        "Portable payload access barrier rejected the operation",
        r#"{"reason":"view_invalidated"}"#,
    );
    drop(payload);

    let detached_self = detached.field(14)?;
    assert_eq!(detached_self.graph_identity()?, root_identity);
    assert_eq!(detached_self.ref_target()?.graph_identity()?, root_identity);
    let detached_asset = detached.field(15)?.ref_target()?;
    assert_eq!(detached_asset.graph_identity()?, asset_identity);
    assert_eq!(
        detached_asset.field(1)?.ref_target()?.graph_identity()?,
        root_identity
    );
    Ok(())
}

#[test]
fn disconnected_roots_have_distinct_payload_scoped_identities() -> Result<(), PayloadError> {
    let spec = CompiledSpec::compile(DISCONNECTED_SPEC)?;
    let component = spec.component_index("Node")?;
    let mut builder = Builder::create(&spec)?;
    let first = builder.declare_object(component)?;
    let second = builder.declare_object(component)?;
    builder
        .object_fill_begin(first)?
        .value_u32(11)?
        .value_null()?
        .object_fill_begin(second)?
        .value_u32(22)?
        .value_null()?
        .entry_begin(0, 2)?
        .value_object(first)?
        .value_object(second)?;
    let payload = builder
        .freeze()?
        .execute(BuildPolicy::AllowStaging)?
        .payload;
    let roots = payload.entry_view(0)?;
    let first = roots.at(0)?;
    let second = roots.at(1)?;
    assert_eq!(
        first.graph_identity()?,
        GraphIdentity {
            component_index: component,
            object_id: 0,
        }
    );
    assert_eq!(
        second.graph_identity()?,
        GraphIdentity {
            component_index: component,
            object_id: 1,
        }
    );
    assert_eq!(first.field(0)?.get_u32()?, 11);
    assert_eq!(second.field(0)?.get_u32()?, 22);

    let independent = graph_payload()?;
    assert_eq!(
        root(&independent.payload, 0)?.graph_identity()?.object_id,
        0
    );
    payload.invalidate()?;
    assert_eq!(
        root(&independent.payload, 0)?.field(3)?.get_u32()?,
        0x789a_bcde
    );
    Ok(())
}
