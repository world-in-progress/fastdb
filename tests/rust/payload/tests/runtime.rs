use fastdb::{
    BackingFailure, BackingObserver, BuildPlan, BuildPolicy, Builder, CompiledSpec, ExecutionMode,
    ExternalBytes, FallbackReason, MemoryBacking, OpenOptions, Payload, Profile,
    ReservationRequest, ReserveMode,
};
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;
use std::time::Duration;

fn fixture(name: &str) -> Vec<u8> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join("tests/golden/payload/v1/binary/spec");
    fs::read(root.join(name)).expect("fixture")
}

fn binary_golden(name: &str) -> Vec<u8> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join("tests/golden/payload/v1/binary/valid");
    let source = fs::read_to_string(root.join(name)).expect("binary golden");
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

fn invalid_binary_golden(name: &str) -> Vec<u8> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join("tests/golden/payload/v1/binary/invalid");
    let source = fs::read_to_string(root.join(name)).expect("invalid binary golden");
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

fn fixed_plan() -> (CompiledSpec, BuildPlan) {
    let spec = CompiledSpec::compile(&fixture("fixed-scalars.source.json")).expect("compile");
    let mut builder = Builder::create(&spec).expect("builder");
    builder.entry_begin(0, 1).unwrap().value_bool(true).unwrap();
    builder.entry_begin(1, 1).unwrap().value_u8(0xab).unwrap();
    builder
        .entry_begin(2, 3)
        .unwrap()
        .value_u16(0x1234)
        .unwrap()
        .value_null()
        .unwrap()
        .value_u16(0xffff)
        .unwrap();
    builder
        .entry_begin(3, 1)
        .unwrap()
        .value_u32(0x1234_5678)
        .unwrap();
    builder.entry_begin(4, 1).unwrap().value_i32(-2).unwrap();
    builder
        .entry_begin(5, 4)
        .unwrap()
        .value_f32_bits(0x8000_0000)
        .unwrap()
        .value_f32_bits(0x7f80_0000)
        .unwrap()
        .value_f32_bits(0xff80_0000)
        .unwrap()
        .value_f32_bits(0x7fa1_2345)
        .unwrap();
    builder
        .entry_begin(6, 4)
        .unwrap()
        .value_f64_bits(0x8000_0000_0000_0000)
        .unwrap()
        .value_f64_bits(0x7ff0_0000_0000_0000)
        .unwrap()
        .value_f64_bits(0xfff0_0000_0000_0000)
        .unwrap()
        .value_f64_bits(0x7ff0_0000_0000_0042)
        .unwrap();
    let plan = builder.freeze().expect("freeze");
    (spec, plan)
}

#[derive(Default)]
struct Observer {
    decline_direct: bool,
    fail_write: bool,
    fail_commit: bool,
    panic_commit: bool,
    writes: AtomicUsize,
    commits: AtomicUsize,
    rollbacks: AtomicUsize,
    releases: AtomicUsize,
}

impl BackingObserver for Observer {
    fn reserve(&self, request: ReservationRequest) -> Result<(), BackingFailure> {
        assert!(request.minimum_capacity > 0);
        assert!(request.alignment.is_power_of_two());
        if self.decline_direct && request.mode == ReserveMode::Direct {
            Err(BackingFailure::DirectUnavailable)
        } else {
            Ok(())
        }
    }

    fn write(&self, _offset: u64, _source: &[u8]) -> Result<(), BackingFailure> {
        self.writes.fetch_add(1, Ordering::SeqCst);
        if self.fail_write {
            Err(BackingFailure::AllocationFailed)
        } else {
            Ok(())
        }
    }

    fn commit(&self, committed: &[u8]) -> Result<(), BackingFailure> {
        assert!(!committed.is_empty());
        self.commits.fetch_add(1, Ordering::SeqCst);
        assert!(
            !self.panic_commit,
            "observer panic must stop at the C boundary"
        );
        if self.fail_commit {
            Err(BackingFailure::CommitFailed)
        } else {
            Ok(())
        }
    }

    fn rollback(&self) -> Result<(), BackingFailure> {
        self.rollbacks.fetch_add(1, Ordering::SeqCst);
        Ok(())
    }

    fn release(&self) {
        self.releases.fetch_add(1, Ordering::SeqCst);
    }
}

#[derive(Default)]
struct OverlapState {
    calls: usize,
    active: usize,
    max_active: usize,
}

#[derive(Default)]
struct OverlapObserver {
    state: Mutex<OverlapState>,
    changed: Condvar,
}

impl OverlapObserver {
    fn wait_for_first_reserve(&self) {
        let state = self.state.lock().unwrap();
        let (state, timeout) = self
            .changed
            .wait_timeout_while(state, Duration::from_secs(2), |state| state.calls == 0)
            .unwrap();
        assert!(
            !timeout.timed_out(),
            "first execution did not reach reserve"
        );
        assert_eq!(state.calls, 1);
    }

    fn max_active(&self) -> usize {
        self.state.lock().unwrap().max_active
    }
}

impl BackingObserver for OverlapObserver {
    fn reserve(&self, _request: ReservationRequest) -> Result<(), BackingFailure> {
        let mut state = self.state.lock().unwrap();
        state.calls += 1;
        state.active += 1;
        state.max_active = state.max_active.max(state.active);
        self.changed.notify_all();
        if state.calls == 1 {
            let (next, _) = self
                .changed
                .wait_timeout_while(state, Duration::from_millis(250), |state| state.calls < 2)
                .unwrap();
            state = next;
        }
        state.active -= 1;
        Ok(())
    }
}

#[test]
fn internal_heap_execution_and_payload_facts_are_core_owned() {
    let (_spec, plan) = fixed_plan();
    let info = plan.info().unwrap();
    let result = plan.execute(BuildPolicy::AllowStaging).unwrap();
    assert_eq!(result.report.mode, ExecutionMode::Direct);
    assert_eq!(result.report.fallback_reason, FallbackReason::None);
    assert_eq!(result.report.requested_bytes, info.total_bytes);
    assert_eq!(result.report.used_bytes, info.total_bytes);
    assert_eq!(result.report.staging_bytes, 0);
    assert_eq!(result.report.region_count, info.region_count);
    assert!(result.report.backing_capacity >= info.total_bytes);
    assert_eq!(result.payload.profile().unwrap(), Profile::RecordV1);
    assert_eq!(result.payload.execution_report().unwrap(), result.report);
    let expected_binary = binary_golden("fixed-scalars.bin.hex");
    assert_eq!(result.payload.binary_bytes().unwrap(), expected_binary);
    assert_eq!(expected_binary.len() as u64, info.total_bytes);

    let clone = result.payload.clone();
    drop(result.payload);
    assert_eq!(clone.binary_bytes().unwrap().len() as u64, info.total_bytes);
}

#[test]
fn caller_backing_falls_back_truthfully_and_direct_required_is_exact() {
    let (_spec, plan) = fixed_plan();
    let direct_observer = Arc::new(Observer::default());
    let direct_backing = MemoryBacking::with_observer(direct_observer.clone());
    let direct = plan
        .execute_with_backing(BuildPolicy::AllowStaging, &direct_backing)
        .unwrap();
    assert_eq!(direct.report.mode, ExecutionMode::Direct);
    assert_eq!(direct.report.fallback_reason, FallbackReason::None);
    assert_eq!(direct.report.staging_bytes, 0);
    assert_eq!(direct_observer.writes.load(Ordering::SeqCst), 0);
    assert_eq!(direct_observer.commits.load(Ordering::SeqCst), 1);
    drop(direct);
    assert_eq!(direct_observer.releases.load(Ordering::SeqCst), 1);

    let observer = Arc::new(Observer {
        decline_direct: true,
        ..Observer::default()
    });
    let backing = MemoryBacking::with_observer(observer.clone());
    let result = plan
        .execute_with_backing(BuildPolicy::AllowStaging, &backing)
        .unwrap();
    assert_eq!(result.report.mode, ExecutionMode::Staged);
    assert_eq!(
        result.report.fallback_reason,
        FallbackReason::BackingDeclinedDirect
    );
    assert_eq!(result.report.staging_bytes, result.report.used_bytes);
    assert!(observer.writes.load(Ordering::SeqCst) > 0);
    assert_eq!(observer.commits.load(Ordering::SeqCst), 1);

    let error = plan
        .execute_with_backing(BuildPolicy::RequireDirect, &backing)
        .unwrap_err();
    assert_eq!(error.code(), 2007);
    assert_eq!(error.symbol(), "DIRECT_UNAVAILABLE");
    assert_eq!(error.path(), "/backing");
    assert_eq!(
        error.details_json(),
        r#"{"reason":"backing_declined_direct"}"#
    );

    drop(result);
    assert_eq!(observer.releases.load(Ordering::SeqCst), 1);
}

#[test]
fn callback_failures_rollback_once_without_unwinding() {
    let (_spec, plan) = fixed_plan();
    for (fail_write, fail_commit, panic_commit, expected) in [
        (true, false, false, 5002_u32),
        (false, true, false, 5003_u32),
        (false, false, true, 5001_u32),
    ] {
        let observer = Arc::new(Observer {
            decline_direct: fail_write,
            fail_write,
            fail_commit,
            panic_commit,
            ..Observer::default()
        });
        let backing = MemoryBacking::with_observer(observer.clone());
        let error = plan
            .execute_with_backing(BuildPolicy::AllowStaging, &backing)
            .unwrap_err();
        assert_eq!(error.code(), expected);
        assert_eq!(observer.rollbacks.load(Ordering::SeqCst), 1);
        assert_eq!(observer.releases.load(Ordering::SeqCst), 0);
    }
}

#[test]
fn copied_and_external_open_have_explicit_independent_lifetimes() {
    let (spec, plan) = fixed_plan();
    let built = plan.execute(BuildPolicy::AllowStaging).unwrap();
    let original = built.payload.binary_bytes().unwrap();

    let mut source = original.clone();
    let copied = Payload::open_copy(&spec, &source, &OpenOptions::default()).unwrap();
    source.fill(0xff);
    assert_eq!(copied.binary_bytes().unwrap(), original);

    let external = ExternalBytes::new(original.clone());
    let opened = Payload::open_external(&spec, &external, &OpenOptions::default()).unwrap();
    drop(external);
    assert_eq!(opened.binary_bytes().unwrap(), original);
    let error = opened.execution_report().unwrap_err();
    assert_eq!(error.code(), 2008);
    assert_eq!(
        error.details_json(),
        r#"{"reason":"opened_payload_has_no_execution_report"}"#
    );
}

#[test]
fn invalid_open_preserves_every_core_field() {
    let spec = CompiledSpec::compile(&fixture("empty.source.json")).expect("compile empty spec");
    let source = invalid_binary_golden("header-magic.bin.hex");
    let error = Payload::open_copy(&spec, &source, &OpenOptions::default())
        .expect_err("the invalid Core binary golden must be rejected");
    assert_eq!(error.code(), 3001);
    assert_eq!(error.symbol(), "INVALID_MAGIC");
    assert_eq!(error.path(), "/binary/header/magic");
    assert_eq!(error.message(), "Portable payload magic is invalid");
    assert_eq!(
        error.details_json(),
        r#"{"actual":"4744425041593100","expected":"4644425041593100","reason":"invalid_magic"}"#
    );
}

#[test]
fn shared_safe_backing_serializes_complete_plan_executions() {
    let (_spec, plan) = fixed_plan();
    let observer = Arc::new(OverlapObserver::default());
    let backing = MemoryBacking::with_observer(observer.clone());

    let first_plan = plan.clone();
    let first_backing = backing.clone();
    let first = thread::spawn(move || {
        first_plan
            .execute_with_backing(BuildPolicy::AllowStaging, &first_backing)
            .unwrap()
    });
    observer.wait_for_first_reserve();
    let second = thread::spawn(move || {
        plan.execute_with_backing(BuildPolicy::AllowStaging, &backing)
            .unwrap()
    });

    drop(first.join().unwrap());
    drop(second.join().unwrap());
    assert_eq!(observer.max_active(), 1);
}
