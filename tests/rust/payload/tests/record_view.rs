use fastdb::{BuildPolicy, Builder, CompiledSpec, Payload, PayloadError, ViewKind};
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

const RECORD_SPEC: &[u8] =
    include_bytes!("../../../golden/payload/v1/spec/valid/record-all-types.source.json");

fn record_payload() -> (CompiledSpec, Payload) {
    let spec = CompiledSpec::compile(RECORD_SPEC).expect("compile record fixture");
    let mut builder = Builder::create(&spec).expect("create builder");

    builder
        .entry_begin(0, 1)
        .unwrap()
        .value_component_begin()
        .unwrap()
        .value_bool(true)
        .unwrap()
        .value_u8(0xab)
        .unwrap()
        .value_u16(0x1234)
        .unwrap()
        .value_u32(0x89ab_cdef)
        .unwrap()
        .value_i32(-42)
        .unwrap()
        .value_u8n(0.0)
        .unwrap()
        .value_u16n(1.0)
        .unwrap()
        .value_f32_bits(0x3fc0_0000)
        .unwrap()
        .value_f64_bits(0x4004_0000_0000_0000)
        .unwrap()
        .value_str("\u{feff}A\0B")
        .unwrap()
        .value_wstr(&[0xfeff, 0x0041, 0, 0xd83c, 0xdf0d, 0x03a9])
        .unwrap()
        .value_bytes(&[0, 1, 0xff])
        .unwrap()
        .value_component_begin()
        .unwrap()
        .value_list_begin(3)
        .unwrap()
        .value_list_begin(0)
        .unwrap()
        .value_null()
        .unwrap()
        .value_list_begin(3)
        .unwrap()
        .value_str("")
        .unwrap()
        .value_null()
        .unwrap()
        .value_str("tail")
        .unwrap();

    builder
        .entry_begin(1, 4)
        .unwrap()
        .value_null()
        .unwrap()
        .value_list_begin(0)
        .unwrap()
        .value_list_begin(3)
        .unwrap()
        .value_u8(0)
        .unwrap()
        .value_null()
        .unwrap()
        .value_u8(0xff)
        .unwrap()
        .value_list_begin(1)
        .unwrap()
        .value_u8(7)
        .unwrap();

    let plan = builder.freeze().expect("freeze record plan");
    let payload = plan
        .execute(BuildPolicy::AllowStaging)
        .expect("execute record plan")
        .payload;
    (spec, payload)
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
fn complete_record_views_are_core_owned_and_materialized() {
    let (_spec, payload) = record_payload();
    let binary = payload.binary_bytes().unwrap();
    {
        let access = payload.acquire().unwrap();
        assert_eq!(access.payload_bytes().unwrap(), binary.as_slice());
        let error = access.str().unwrap_err();
        require_error(
            error,
            2004,
            "TYPE_MISMATCH",
            "/access",
            "Portable payload view kind does not match the operation",
            r#"{"reason":"view_kind_mismatch"}"#,
        );
    }

    let sequence = payload.entry_view(0).unwrap();
    assert_eq!(sequence.kind().unwrap(), ViewKind::Sequence);
    assert!(!sequence.is_null().unwrap());
    assert_eq!(sequence.length().unwrap(), 1);
    let root = sequence.at(0).unwrap();
    assert_eq!(root.kind().unwrap(), ViewKind::Component);
    assert_eq!(root.component_index().unwrap(), 0);
    assert_eq!(root.field_count().unwrap(), 14);

    assert!(root.field(0).unwrap().get_bool().unwrap());
    assert_eq!(root.field(1).unwrap().get_u8().unwrap(), 0xab);
    assert_eq!(root.field(2).unwrap().get_u16().unwrap(), 0x1234);
    assert_eq!(root.field(3).unwrap().get_u32().unwrap(), 0x89ab_cdef);
    assert_eq!(root.field(4).unwrap().get_i32().unwrap(), -42);
    assert_eq!(root.field(5).unwrap().get_u8n_f64_bits().unwrap(), 0);
    assert_eq!(root.field(5).unwrap().get_u8n().unwrap(), 0.0);
    assert_eq!(
        root.field(6).unwrap().get_u16n_f64_bits().unwrap(),
        0x3ff0_0000_0000_0000
    );
    assert_eq!(root.field(6).unwrap().get_u16n().unwrap(), 1.0);
    assert_eq!(root.field(7).unwrap().get_f32_bits().unwrap(), 0x3fc0_0000);
    assert_eq!(root.field(7).unwrap().get_f32().unwrap(), 1.5);
    assert_eq!(
        root.field(8).unwrap().get_f64_bits().unwrap(),
        0x4004_0000_0000_0000
    );
    assert_eq!(root.field(8).unwrap().get_f64().unwrap(), 2.5);

    let text = root.field(9).unwrap();
    assert_eq!(text.kind().unwrap(), ViewKind::Str);
    let text_access = text.acquire().unwrap();
    drop(text);
    assert_eq!(text_access.str().unwrap(), "\u{feff}A\0B");

    let wide = root.field(10).unwrap();
    assert_eq!(wide.kind().unwrap(), ViewKind::Wstr);
    let wide_access = wide.acquire().unwrap();
    let wide_units = wide_access.wstr().unwrap();
    assert_eq!(wide_units, &[0xfeff, 0x0041, 0, 0xd83c, 0xdf0d, 0x03a9]);
    assert_eq!(
        wide_units
            .as_ptr()
            .align_offset(std::mem::align_of::<u16>()),
        0
    );

    let opaque = root.field(11).unwrap();
    assert_eq!(opaque.kind().unwrap(), ViewKind::Bytes);
    let opaque_access = opaque.acquire().unwrap();
    assert_eq!(opaque_access.bytes().unwrap(), &[0, 1, 0xff]);

    let leaf = root.field(12).unwrap();
    assert_eq!(leaf.kind().unwrap(), ViewKind::Component);
    assert_eq!(leaf.component_index().unwrap(), 1);
    assert_eq!(leaf.field_count().unwrap(), 0);

    let nested = root.field(13).unwrap();
    assert_eq!(nested.kind().unwrap(), ViewKind::List);
    assert_eq!(nested.length().unwrap(), 3);
    assert!(!nested.at(0).unwrap().is_null().unwrap());
    assert_eq!(nested.at(0).unwrap().length().unwrap(), 0);
    assert!(nested.at(1).unwrap().is_null().unwrap());
    let present = nested.at(2).unwrap();
    assert_eq!(present.length().unwrap(), 3);
    let empty_text = present.at(0).unwrap().acquire().unwrap();
    assert_eq!(empty_text.str().unwrap(), "");
    assert!(present.at(1).unwrap().is_null().unwrap());
    let tail = present.at(2).unwrap().acquire().unwrap();
    assert_eq!(tail.str().unwrap(), "tail");

    let series = payload.entry_view(1).unwrap();
    assert_eq!(series.length().unwrap(), 4);
    assert!(series.at(0).unwrap().is_null().unwrap());
    assert!(!series.at(1).unwrap().is_null().unwrap());
    assert_eq!(series.at(1).unwrap().length().unwrap(), 0);
    let values = series.at(2).unwrap();
    assert_eq!(values.length().unwrap(), 3);
    assert_eq!(values.at(0).unwrap().get_u8().unwrap(), 0);
    assert!(values.at(1).unwrap().is_null().unwrap());
    assert_eq!(values.at(2).unwrap().get_u8().unwrap(), 0xff);
    assert_eq!(series.at(3).unwrap().at(0).unwrap().get_u8().unwrap(), 7);

    require_error(
        root.get_u8().unwrap_err(),
        2004,
        "TYPE_MISMATCH",
        "/entries/0/0",
        "Portable payload view kind does not match the operation",
        r#"{"reason":"view_kind_mismatch"}"#,
    );

    for _ in 0..1_000 {
        let retained = root.clone();
        assert_eq!(retained.field_count().unwrap(), 14);
    }

    let detached = root.materialize().unwrap();
    drop(text_access);
    drop(wide_access);
    drop(opaque_access);
    drop(empty_text);
    drop(tail);
    payload.invalidate().unwrap();
    drop(payload);

    require_error(
        root.kind().unwrap_err(),
        4001,
        "VIEW_INVALIDATED",
        "/view",
        "Portable payload access barrier rejected the operation",
        r#"{"reason":"view_invalidated"}"#,
    );
    assert_eq!(detached.field(1).unwrap().get_u8().unwrap(), 0xab);
    let detached_text = detached.field(9).unwrap();
    let detached_access = detached_text.acquire().unwrap();
    assert_eq!(detached_access.str().unwrap(), "\u{feff}A\0B");
}

#[test]
fn invalidation_waits_for_the_unique_access_pin() {
    let (_spec, payload) = record_payload();
    let text = payload
        .entry_view(0)
        .unwrap()
        .at(0)
        .unwrap()
        .field(9)
        .unwrap();
    let access = text.acquire().unwrap();
    let invalidating = payload.clone();
    let (started_tx, started_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let worker = thread::spawn(move || {
        started_tx.send(()).unwrap();
        let result = invalidating.invalidate();
        done_tx.send(result).unwrap();
    });

    started_rx.recv().unwrap();
    thread::sleep(Duration::from_millis(25));
    assert!(done_rx.try_recv().is_err());
    assert_eq!(access.str().unwrap(), "\u{feff}A\0B");
    drop(access);
    done_rx
        .recv_timeout(Duration::from_secs(2))
        .expect("invalidation completes after access release")
        .unwrap();
    worker.join().unwrap();

    require_error(
        text.kind().unwrap_err(),
        4001,
        "VIEW_INVALIDATED",
        "/view",
        "Portable payload access barrier rejected the operation",
        r#"{"reason":"view_invalidated"}"#,
    );
}
