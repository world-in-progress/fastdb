use fastdb::{
    Backing, BackingStatus, BuildPlan, BuildPolicy, Builder, CommittedBacking, CompiledSpec,
    ExecutionMode, ExternalBytes, FallbackReason, OpenOptions, Payload, Profile, ReserveMode,
    ReservedBacking,
};
use std::sync::atomic::{AtomicUsize, Ordering::SeqCst};
use std::sync::{Arc, Mutex};

const VALUE: &[u8] = &[0, 0xff, 1, 2, 3];

fn plan(profile: Profile) -> (CompiledSpec, BuildPlan) {
    let source = match profile {
        Profile::RecordV1 => br#"{
            "schema":"fastdb.payload.v1","profile":"record.v1",
            "entries":[{"id":"value","cardinality":"many","type":{"kind":"bytes"}}],
            "components":[]
        }"#.as_slice(),
        Profile::ObjectGraphV1 => br#"{
            "schema":"fastdb.payload.v1","profile":"object_graph.v1",
            "entries":[{"id":"value","cardinality":"many","type":{"kind":"component","id":"Value"}}],
            "components":[{"id":"Value","kind":"record",
                "fields":[{"id":"bytes","type":{"kind":"bytes"}}]}]
        }"#.as_slice(),
    };
    let spec = CompiledSpec::compile(source).unwrap();
    let mut builder = Builder::create(&spec).unwrap();
    match profile {
        Profile::RecordV1 => {
            builder
                .entry_begin(0, 1)
                .unwrap()
                .value_bytes(VALUE)
                .unwrap();
        }
        Profile::ObjectGraphV1 => {
            let object = builder
                .declare_object(spec.component_index("Value").unwrap())
                .unwrap();
            builder
                .object_fill_begin(object)
                .unwrap()
                .value_bytes(VALUE)
                .unwrap();
            builder
                .entry_begin(0, 1)
                .unwrap()
                .value_object(object)
                .unwrap();
        }
    }
    (spec, builder.freeze().unwrap())
}

#[repr(align(64))]
struct InlineOwner {
    bytes: [u8; 4096],
    len: usize,
    address: Arc<AtomicUsize>,
    drops: Arc<AtomicUsize>,
    panic_on_drop: bool,
}

impl AsRef<[u8]> for InlineOwner {
    fn as_ref(&self) -> &[u8] {
        self.address.store(self.bytes.as_ptr() as usize, SeqCst);
        &self.bytes[..self.len]
    }
}

impl Drop for InlineOwner {
    fn drop(&mut self) {
        self.drops.fetch_add(1, SeqCst);
        assert!(!self.panic_on_drop, "owner destructor panic");
    }
}

#[test]
fn inline_owner_is_borrowed_in_place_and_retained_by_views_and_accesses() {
    let (spec, plan) = plan(Profile::RecordV1);
    let built = plan.execute(BuildPolicy::RequireDirect).unwrap().payload;
    let built_value = built
        .entry_view(0)
        .unwrap()
        .at(0)
        .unwrap()
        .acquire()
        .unwrap();
    let built_image = built.acquire().unwrap();
    let bytes = built_image.payload_bytes().unwrap();
    let offset = built_value.bytes().unwrap().as_ptr() as usize - bytes.as_ptr() as usize;

    for panic_on_drop in [false, true] {
        let drops = Arc::new(AtomicUsize::new(0));
        let address = Arc::new(AtomicUsize::new(0));
        let mut owner = InlineOwner {
            bytes: [0; 4096],
            len: bytes.len(),
            address: address.clone(),
            drops: drops.clone(),
            panic_on_drop,
        };
        owner.bytes[..bytes.len()].copy_from_slice(bytes);
        let external = ExternalBytes::from_owner(owner);
        let base = address.load(SeqCst);
        let payload = Payload::open_external(&spec, &external, &OpenOptions::default()).unwrap();
        drop(external);
        let view = payload.entry_view(0).unwrap().at(0).unwrap();
        let image = payload.acquire().unwrap();
        drop(payload);
        assert_eq!(drops.load(SeqCst), 0);
        assert_eq!(image.payload_bytes().unwrap().as_ptr() as usize, base);
        drop(image);
        let access = view.acquire().unwrap();
        drop(view);
        assert_eq!(drops.load(SeqCst), 0);
        assert_eq!(access.bytes().unwrap().as_ptr() as usize, base + offset);
        assert_eq!(access.bytes().unwrap(), VALUE);
        drop(access);
        assert_eq!(drops.load(SeqCst), 1);
    }
}

#[test]
fn arc_and_vec_keep_the_original_allocation() {
    let (spec, plan) = plan(Profile::RecordV1);
    let built = plan.execute(BuildPolicy::RequireDirect).unwrap().payload;
    let image = built.acquire().unwrap();
    let bytes = image.payload_bytes().unwrap();
    let value = built
        .entry_view(0)
        .unwrap()
        .at(0)
        .unwrap()
        .acquire()
        .unwrap();
    let offset = value.bytes().unwrap().as_ptr() as usize - bytes.as_ptr() as usize;
    let mut vec = Vec::with_capacity(bytes.len() * 4);
    vec.extend_from_slice(bytes);
    assert!(vec.capacity() > vec.len());
    let vec_ptr = vec.as_ptr();
    let external = ExternalBytes::new(vec);
    assert_eq!(external.as_bytes().as_ptr(), vec_ptr);
    let payload = Payload::open_external(&spec, &external, &OpenOptions::default()).unwrap();
    assert_eq!(
        payload.acquire().unwrap().payload_bytes().unwrap().as_ptr(),
        vec_ptr
    );

    let arc: Arc<[u8]> = bytes.into();
    let weak = Arc::downgrade(&arc);
    let base = arc.as_ptr();
    let external = ExternalBytes::from_arc(arc);
    assert_eq!(external.as_bytes().as_ptr(), base);
    let payload = Payload::open_external(&spec, &external, &OpenOptions::default()).unwrap();
    drop(external);
    let view = payload.entry_view(0).unwrap().at(0).unwrap();
    let access = view.acquire().unwrap();
    let image = payload.acquire().unwrap();
    drop(payload);
    assert_eq!(weak.strong_count(), 1);
    assert_eq!(image.payload_bytes().unwrap().as_ptr(), base);
    assert_eq!(access.bytes().unwrap().as_ptr(), base.wrapping_add(offset));
    drop(image);
    drop(access);
    assert_eq!(weak.strong_count(), 1);
    drop(view);
    assert_eq!(weak.strong_count(), 0);
}

#[derive(Clone, Copy, Debug)]
enum Fault {
    None,
    Short,
    Misaligned,
    Decline,
    Commit,
    PanicCommit,
    ShortCommitted,
}

#[derive(Default)]
struct SlabState {
    buffer: Mutex<Option<Vec<u8>>>,
    modes: Mutex<Vec<ReserveMode>>,
    abandoned: AtomicUsize,
    rolled_back: AtomicUsize,
    released: AtomicUsize,
}

struct Slab {
    state: Arc<SlabState>,
    offset: usize,
    capacity: usize,
    fault: Fault,
}

impl Slab {
    fn new(plan: &BuildPlan, fault: Fault) -> (Self, usize) {
        let info = plan.info().unwrap();
        let alignment = info.max_alignment as usize;
        let capacity = info.total_bytes as usize + 16;
        let buffer = vec![0xcc; capacity + alignment];
        let offset = buffer.as_ptr().align_offset(alignment);
        let address = buffer.as_ptr() as usize + offset;
        let state = Arc::new(SlabState {
            buffer: Mutex::new(Some(buffer)),
            ..SlabState::default()
        });
        (
            Self {
                state,
                offset,
                capacity,
                fault,
            },
            address,
        )
    }
}

impl Backing for Slab {
    fn reserve(
        &self,
        mode: ReserveMode,
        minimum_capacity: usize,
        alignment: usize,
    ) -> Result<Box<dyn ReservedBacking>, BackingStatus> {
        self.state.modes.lock().unwrap().push(mode);
        let (offset, capacity) = if mode == ReserveMode::Direct {
            match self.fault {
                Fault::Decline => return Err(BackingStatus::DirectUnavailable),
                Fault::Short => (self.offset, minimum_capacity - 1),
                Fault::Misaligned => {
                    assert!(alignment > 1);
                    (self.offset + 1, self.capacity)
                }
                _ => (self.offset, self.capacity),
            }
        } else {
            (self.offset, self.capacity)
        };
        let buffer = self
            .state
            .buffer
            .lock()
            .unwrap()
            .take()
            .ok_or(BackingStatus::AllocationFailed)?;
        Ok(Box::new(ReservedSlab {
            buffer: Some(buffer),
            state: self.state.clone(),
            offset,
            capacity,
            fault: self.fault,
        }))
    }
}

struct ReservedSlab {
    buffer: Option<Vec<u8>>,
    state: Arc<SlabState>,
    offset: usize,
    capacity: usize,
    fault: Fault,
}

impl ReservedBacking for ReservedSlab {
    fn as_mut_slice(&mut self) -> &mut [u8] {
        &mut self.buffer.as_mut().unwrap()[self.offset..self.offset + self.capacity]
    }

    fn commit(
        mut self: Box<Self>,
        used: usize,
    ) -> Result<Box<dyn CommittedBacking>, BackingStatus> {
        assert!(!matches!(self.fault, Fault::PanicCommit), "commit panic");
        if matches!(self.fault, Fault::Commit) {
            return Err(BackingStatus::CommitFailed);
        }
        let len = if matches!(self.fault, Fault::ShortCommitted) {
            used - 1
        } else {
            used
        };
        Ok(Box::new(CommittedSlab {
            buffer: self.buffer.take().unwrap(),
            state: self.state.clone(),
            offset: self.offset,
            len,
        }))
    }

    fn rollback(self: Box<Self>) {
        self.state.rolled_back.fetch_add(1, SeqCst);
    }
}

impl Drop for ReservedSlab {
    fn drop(&mut self) {
        if let Some(buffer) = self.buffer.take() {
            self.state.abandoned.fetch_add(1, SeqCst);
            *self.state.buffer.lock().unwrap() = Some(buffer);
        }
    }
}

struct CommittedSlab {
    buffer: Vec<u8>,
    state: Arc<SlabState>,
    offset: usize,
    len: usize,
}

impl CommittedBacking for CommittedSlab {
    fn as_slice(&self) -> &[u8] {
        &self.buffer[self.offset..self.offset + self.len]
    }
}

impl Drop for CommittedSlab {
    fn drop(&mut self) {
        self.state.released.fetch_add(1, SeqCst);
    }
}

#[test]
fn direct_execution_uses_preallocated_storage_for_both_profiles() {
    for profile in [Profile::RecordV1, Profile::ObjectGraphV1] {
        let (_, plan) = plan(profile);
        let expected = plan
            .execute(BuildPolicy::RequireDirect)
            .unwrap()
            .payload
            .binary_bytes()
            .unwrap();
        let (slab, address) = Slab::new(&plan, Fault::None);
        let backing: &dyn Backing = &slab;
        let result = plan
            .execute_with_backing(BuildPolicy::RequireDirect, backing)
            .unwrap();
        assert_eq!(result.report.mode, ExecutionMode::Direct);
        assert_eq!(result.report.staging_bytes, 0);
        assert_eq!(result.report.backing_capacity, slab.capacity as u64);
        assert_eq!(*slab.state.modes.lock().unwrap(), [ReserveMode::Direct]);
        let view = result.payload.entry_view(0).unwrap();
        let access = result.payload.acquire().unwrap();
        let state = slab.state.clone();
        drop(slab);
        drop(result);
        assert_eq!(state.released.load(SeqCst), 0);
        assert_eq!(access.payload_bytes().unwrap().as_ptr() as usize, address);
        assert_eq!(access.payload_bytes().unwrap(), expected);
        drop(access);
        assert_eq!(state.released.load(SeqCst), 0);
        std::thread::spawn(move || drop(view)).join().unwrap();
        assert_eq!(state.released.load(SeqCst), 1);
        assert_eq!(state.abandoned.load(SeqCst), 0);
    }
}

#[test]
fn direct_declines_roll_back_and_stage_only_when_allowed() {
    for profile in [Profile::RecordV1, Profile::ObjectGraphV1] {
        let (_, plan) = plan(profile);
        let expected = plan
            .execute(BuildPolicy::RequireDirect)
            .unwrap()
            .payload
            .binary_bytes()
            .unwrap();
        for fault in [Fault::Short, Fault::Misaligned, Fault::Decline] {
            let (slab, address) = Slab::new(&plan, fault);
            let error = plan
                .execute_with_backing(BuildPolicy::RequireDirect, &slab)
                .unwrap_err();
            assert_eq!(error.symbol(), "DIRECT_UNAVAILABLE");
            let rollbacks = usize::from(!matches!(fault, Fault::Decline));
            assert_eq!(slab.state.rolled_back.load(SeqCst), rollbacks);
            assert_eq!(slab.state.abandoned.load(SeqCst), rollbacks);
            assert_eq!(slab.state.released.load(SeqCst), 0);

            let result = plan
                .execute_with_backing(BuildPolicy::AllowStaging, &slab)
                .unwrap();
            assert_eq!(result.report.mode, ExecutionMode::Staged);
            assert_eq!(
                result.report.fallback_reason,
                FallbackReason::BackingDeclinedDirect
            );
            assert_eq!(result.report.staging_bytes, result.report.used_bytes);
            assert_eq!(
                *slab.state.modes.lock().unwrap(),
                [
                    ReserveMode::Direct,
                    ReserveMode::Direct,
                    ReserveMode::Staged
                ]
            );
            let access = result.payload.acquire().unwrap();
            assert_eq!(access.payload_bytes().unwrap().as_ptr() as usize, address);
            assert_eq!(access.payload_bytes().unwrap(), expected);
            drop(access);
            drop(result);
            assert_eq!(slab.state.rolled_back.load(SeqCst), rollbacks * 2);
            assert_eq!(slab.state.released.load(SeqCst), 1);
        }
    }
}

#[test]
fn failed_commit_abandons_the_reservation_and_invalid_publication_releases_once() {
    for profile in [Profile::RecordV1, Profile::ObjectGraphV1] {
        let (_, plan) = plan(profile);
        for (fault, symbol, abandoned, released) in [
            (Fault::Commit, "COMMIT_FAILED", 1, 0),
            (Fault::PanicCommit, "BACKING_CONTRACT", 1, 0),
            (Fault::ShortCommitted, "BACKING_CONTRACT", 0, 1),
        ] {
            let (slab, _) = Slab::new(&plan, fault);
            let error = plan
                .execute_with_backing(BuildPolicy::RequireDirect, &slab)
                .unwrap_err();
            assert_eq!(error.symbol(), symbol);
            assert_eq!(slab.state.abandoned.load(SeqCst), abandoned);
            assert_eq!(slab.state.released.load(SeqCst), released);
            assert_eq!(slab.state.rolled_back.load(SeqCst), 0);
        }
    }
}
