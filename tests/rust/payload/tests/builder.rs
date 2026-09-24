use fastdb::{BuildPlan, Builder, BuilderOptions, CompiledSpec, FixedRun, PayloadError, PlanInfo};

const FIXED_SPEC: &[u8] =
    include_bytes!("../../../golden/payload/v1/binary/spec/fixed-scalars.source.json");
const NESTED_SPEC: &[u8] =
    include_bytes!("../../../golden/payload/v1/binary/spec/nested-lists.source.json");
const GRAPH_SPEC: &[u8] =
    include_bytes!("../../../golden/payload/v1/binary/spec/graph-all-values.source.json");
const DISCONNECTED_SPEC: &[u8] =
    include_bytes!("../../../golden/payload/v1/binary/spec/graph-disconnected-roots.source.json");

fn finish_fixed(builder: &mut Builder) -> Result<(), PayloadError> {
    builder.entry_begin(1, 1)?.value_u8(0xab)?;

    let fixed_values = [0x1234_u16, 0, u16::MAX];
    let fixed_bytes = fixed_values
        .into_iter()
        .flat_map(u16::to_ne_bytes)
        .collect::<Vec<_>>();
    let validity = [0x05_u8];
    let run = FixedRun::new(&fixed_bytes, 3, 2).with_validity(&validity, 0);
    builder.entry_begin(2, 3)?.value_fixed_run(&run)?;

    builder
        .entry_begin(3, 1)?
        .value_u32(0x1234_5678)?
        .entry_begin(4, 1)?
        .value_i32(-2)?
        .entry_begin(5, 4)?
        .value_f32_bits(0x8000_0000)?
        .value_f32_bits(0x7f80_0000)?
        .value_f32_bits(0xff80_0000)?
        .value_f32_bits(0x7fa1_2345)?
        .entry_begin(6, 4)?
        .value_f64_bits(0x8000_0000_0000_0000)?
        .value_f64_bits(0x7ff0_0000_0000_0000)?
        .value_f64_bits(0xfff0_0000_0000_0000)?
        .value_f64_bits(0x7ff0_0000_0000_0042)?;
    Ok(())
}

fn author_fixed() -> Result<BuildPlan, PayloadError> {
    let spec = CompiledSpec::compile(FIXED_SPEC)?;
    let mut builder = Builder::create(&spec)?;
    builder.entry_begin(0, 1)?.value_bool(true)?;
    finish_fixed(&mut builder)?;
    builder.freeze()
}

#[test]
fn fixed_run_and_immutable_plan_facts_are_core_owned() -> Result<(), PayloadError> {
    let plan = author_fixed()?;
    let info = plan.info()?;
    assert_eq!(info.total_bytes, 928);
    assert_eq!(info.logical_value_count, 22);
    assert_eq!(info.list_element_count, 0);
    assert_eq!(info.text_bytes, 0);
    assert_eq!(info.opaque_bytes, 0);
    assert_eq!(info.max_alignment, 8);
    assert_eq!(info.direct_build_status, 1);
    assert_eq!(info.graph_object_count, 0);

    let cloned = plan.clone();
    drop(plan);
    let concurrent = std::thread::spawn(move || cloned.info())
        .join()
        .expect("the immutable plan query thread must not panic")?;
    assert_eq!(concurrent.total_bytes, 928);
    Ok(())
}

#[test]
fn nested_component_lists_preserve_null_and_empty_shapes() -> Result<(), PayloadError> {
    let spec = CompiledSpec::compile(NESTED_SPEC)?;
    let mut builder = Builder::create(&spec)?;
    let wide = [0x0041_u16, 0xd83d, 0xde03];
    let opaque = [0x00_u8, 0xff, 0x41];

    builder
        .entry_begin(0, 2)?
        .value_component_begin()?
        .value_list_begin(3)?
        .value_null()?
        .value_list_begin(0)?
        .value_list_begin(3)?
        .value_str("")?
        .value_null()?
        .value_str("alpha")?
        .value_null()?
        .value_list_begin(0)?
        .value_component_begin()?
        .value_list_begin(0)?
        .value_list_begin(3)?
        .value_null()?
        .value_wstr(&[])?
        .value_wstr(&wide)?
        .value_list_begin(2)?
        .value_null()?
        .value_bytes(&opaque)?
        .entry_begin(1, 3)?
        .value_null()?
        .value_list_begin(0)?
        .value_list_begin(3)?
        .value_u16(7)?
        .value_null()?
        .value_u16(9)?
        .entry_begin(2, 1)?
        .value_list_begin(3)?
        .value_null()?
        .value_list_begin(0)?
        .value_list_begin(3)?
        .value_str("")?
        .value_null()?
        .value_str("tail")?;

    let info = builder.freeze()?.info()?;
    assert_eq!(info.total_bytes, 1960);
    assert_eq!(info.list_element_count, 20);
    assert_eq!(info.graph_object_count, 0);
    Ok(())
}

#[test]
fn graph_authoring_preserves_forward_self_and_mutual_refs() -> Result<(), PayloadError> {
    let spec = CompiledSpec::compile(GRAPH_SPEC)?;
    let node_component = spec.component_index("Node")?;
    let asset_component = spec.component_index("Asset")?;
    let mut builder = Builder::create(&spec)?;
    let node = builder.declare_object(node_component)?;
    let asset = builder.declare_object(asset_component)?;
    let wide = [0x0041_u16, 0xd83d, 0xde00];
    let opaque = [0x00_u8, 0xff, 0x7e];

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
        .value_wstr(&wide)?
        .value_bytes(&opaque)?
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

    let info = builder.freeze()?.info()?;
    assert_eq!(
        info,
        PlanInfo {
            flags: 0,
            total_bytes: 1304,
            region_count: 13,
            logical_value_count: 40,
            list_element_count: 3,
            text_bytes: 14,
            opaque_bytes: 3,
            validation_work: 146,
            max_alignment: 8,
            direct_build_status: 1,
            graph_object_count: 2,
        }
    );
    Ok(())
}

#[test]
fn disconnected_objects_are_explicit_roots() -> Result<(), PayloadError> {
    let spec = CompiledSpec::compile(DISCONNECTED_SPEC)?;
    let node_component = spec.component_index("Node")?;
    let mut builder = Builder::create(&spec)?;
    let first = builder.declare_object(node_component)?;
    let second = builder.declare_object(node_component)?;
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
    let info = builder.freeze()?.info()?;
    assert_eq!(info.total_bytes, 328);
    assert_eq!(info.graph_object_count, 2);
    Ok(())
}

#[test]
fn core_type_error_is_preserved_and_builder_mutation_is_retryable() -> Result<(), PayloadError> {
    let spec = CompiledSpec::compile(FIXED_SPEC)?;
    let mut builder = Builder::create(&spec)?;
    builder.entry_begin(0, 1)?;
    let error = builder
        .value_u8(1)
        .expect_err("the Core must reject u8 for a bool entry");
    assert_eq!(error.code(), 2004);
    assert_eq!(error.symbol(), "TYPE_MISMATCH");
    builder.value_bool(true)?;
    finish_fixed(&mut builder)?;
    assert_eq!(builder.freeze()?.info()?.total_bytes, 928);
    Ok(())
}

#[test]
fn core_owned_builder_limits_are_projected_without_binding_semantics() -> Result<(), PayloadError> {
    let spec = CompiledSpec::compile(DISCONNECTED_SPEC)?;
    let component = spec.component_index("Node")?;
    let options = BuilderOptions {
        max_graph_objects: 1,
        ..BuilderOptions::default()
    };
    let mut builder = Builder::create_with_options(&spec, &options)?;

    let _first = builder.declare_object(component)?;
    let error = builder
        .declare_object(component)
        .expect_err("the Core must enforce max_graph_objects");
    assert_eq!(error.code(), 2013);
    assert_eq!(error.symbol(), "BUILDER_RESOURCE_LIMIT");
    assert_eq!(error.path(), "/objects/Node/1");
    Ok(())
}
