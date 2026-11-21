use crate::{Blob, BuildPlan, Builder, CompiledSpec, PayloadError, Profile, check_status};
use fastdb_sys as sys;
use std::alloc::{Layout, alloc_zeroed, dealloc};
use std::ffi::c_void;
use std::fmt;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::{self, NonNull};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock, PoisonError};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum BuildPolicy {
    AllowStaging = sys::FDB_PAYLOAD_BUILD_ALLOW_STAGING,
    RequireDirect = sys::FDB_PAYLOAD_BUILD_REQUIRE_DIRECT,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum ReserveMode {
    Direct = sys::FDB_PAYLOAD_RESERVE_DIRECT,
    Staged = sys::FDB_PAYLOAD_RESERVE_STAGED,
}

impl ReserveMode {
    fn from_raw(value: u32) -> Result<Self, ()> {
        match value {
            sys::FDB_PAYLOAD_RESERVE_DIRECT => Ok(Self::Direct),
            sys::FDB_PAYLOAD_RESERVE_STAGED => Ok(Self::Staged),
            _ => Err(()),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum ExecutionMode {
    Direct = sys::FDB_PAYLOAD_EXECUTION_DIRECT,
    Staged = sys::FDB_PAYLOAD_EXECUTION_STAGED,
}

impl ExecutionMode {
    fn from_raw(value: u32) -> Result<Self, PayloadError> {
        match value {
            sys::FDB_PAYLOAD_EXECUTION_DIRECT => Ok(Self::Direct),
            sys::FDB_PAYLOAD_EXECUTION_STAGED => Ok(Self::Staged),
            _ => Err(unknown_report_value("mode", value)),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum FallbackReason {
    None = sys::FDB_PAYLOAD_FALLBACK_NONE,
    PlanRequiresStaging = sys::FDB_PAYLOAD_FALLBACK_PLAN_REQUIRES_STAGING,
    BackingDeclinedDirect = sys::FDB_PAYLOAD_FALLBACK_BACKING_DECLINED_DIRECT,
}

impl FallbackReason {
    fn from_raw(value: u32) -> Result<Self, PayloadError> {
        match value {
            sys::FDB_PAYLOAD_FALLBACK_NONE => Ok(Self::None),
            sys::FDB_PAYLOAD_FALLBACK_PLAN_REQUIRES_STAGING => Ok(Self::PlanRequiresStaging),
            sys::FDB_PAYLOAD_FALLBACK_BACKING_DECLINED_DIRECT => Ok(Self::BackingDeclinedDirect),
            _ => Err(unknown_report_value("fallback_reason", value)),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ExecutionReport {
    pub mode: ExecutionMode,
    pub fallback_reason: FallbackReason,
    pub requested_bytes: u64,
    pub used_bytes: u64,
    pub staging_bytes: u64,
    pub region_count: u64,
    pub backing_capacity: u64,
}

impl ExecutionReport {
    fn from_raw(raw: &sys::fdb_payload_v1_execution_report_t) -> Result<Self, PayloadError> {
        Ok(Self {
            mode: ExecutionMode::from_raw(raw.mode)?,
            fallback_reason: FallbackReason::from_raw(raw.fallback_reason)?,
            requested_bytes: raw.requested_bytes,
            used_bytes: raw.used_bytes,
            staging_bytes: raw.staging_bytes,
            region_count: raw.region_count,
            backing_capacity: raw.backing_capacity,
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OpenOptions {
    pub flags: u32,
    pub max_total_bytes: u64,
    pub max_regions: u64,
    pub max_entries: u64,
    pub max_components: u64,
    pub max_nesting_depth: u64,
    pub max_list_elements: u64,
    pub max_graph_objects: u64,
    pub max_string_bytes: u64,
    pub max_validation_work: u64,
}

impl Default for OpenOptions {
    fn default() -> Self {
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_open_options_t>::uninit();
        // SAFETY: Core initializes the complete V1 structure.
        unsafe { sys::fdb_payload_v1_open_options_init(raw.as_mut_ptr()) };
        // SAFETY: the initializer wrote the complete current prefix.
        let raw = unsafe { raw.assume_init() };
        Self {
            flags: raw.flags,
            max_total_bytes: raw.max_total_bytes,
            max_regions: raw.max_regions,
            max_entries: raw.max_entries,
            max_components: raw.max_components,
            max_nesting_depth: raw.max_nesting_depth,
            max_list_elements: raw.max_list_elements,
            max_graph_objects: raw.max_graph_objects,
            max_string_bytes: raw.max_string_bytes,
            max_validation_work: raw.max_validation_work,
        }
    }
}

impl OpenOptions {
    fn to_raw(&self) -> sys::fdb_payload_v1_open_options_t {
        sys::fdb_payload_v1_open_options_t {
            struct_size: sys::FDB_PAYLOAD_V1_OPEN_OPTIONS_V1_SIZE,
            flags: self.flags,
            max_total_bytes: self.max_total_bytes,
            max_regions: self.max_regions,
            max_entries: self.max_entries,
            max_components: self.max_components,
            max_nesting_depth: self.max_nesting_depth,
            max_list_elements: self.max_list_elements,
            max_graph_objects: self.max_graph_objects,
            max_string_bytes: self.max_string_bytes,
            max_validation_work: self.max_validation_work,
            reserved: [0; 4],
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReservationRequest {
    pub mode: ReserveMode,
    pub minimum_capacity: u64,
    pub alignment: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BackingFailure {
    DirectUnavailable,
    AllocationFailed,
    CommitFailed,
    RollbackFailed,
    Contract,
}

impl BackingFailure {
    fn status(self) -> u32 {
        match self {
            Self::DirectUnavailable => sys::FDB_PAYLOAD_E_DIRECT_UNAVAILABLE,
            Self::AllocationFailed => sys::FDB_PAYLOAD_E_ALLOCATION_FAILED,
            Self::CommitFailed => sys::FDB_PAYLOAD_E_COMMIT_FAILED,
            Self::RollbackFailed => sys::FDB_PAYLOAD_E_ROLLBACK_FAILED,
            Self::Contract => sys::FDB_PAYLOAD_E_BACKING_CONTRACT,
        }
    }
}

pub use BackingFailure as BackingStatus;

mod backing_dispatch {
    pub struct Key;
}

/// A source of writable payload reservations.
///
/// Each execution has its own callback context. Calls through a shared backing
/// can overlap, so implementations must synchronize any shared allocator state.
pub trait Backing: Send + Sync {
    fn reserve(
        &self,
        mode: ReserveMode,
        minimum_capacity: usize,
        alignment: usize,
    ) -> Result<Box<dyn ReservedBacking>, BackingStatus>;

    // Preserve the built-in observer's range-write and rollback status hooks.
    #[doc(hidden)]
    fn memory_backing(&self, _: backing_dispatch::Key) -> Option<&MemoryBacking> {
        None
    }
}

/// Owns one unpublished writable reservation. Dropping it abandons the
/// reservation, including when commit returns an error or panics.
pub trait ReservedBacking: Send + Sync {
    fn as_mut_slice(&mut self) -> &mut [u8];

    /// Publishes the initialized prefix of the reservation. Keep its address
    /// when transferring the storage to the committed owner.
    fn commit(
        self: Box<Self>,
        used_bytes: usize,
    ) -> Result<Box<dyn CommittedBacking>, BackingStatus>;

    fn rollback(self: Box<Self>) {
        drop(self);
    }
}

/// Owns the readable storage until the last payload, view, or access releases
/// it. The slice starts at the reservation's base and covers its used prefix.
pub trait CommittedBacking: Send + Sync {
    fn as_slice(&self) -> &[u8];
}

pub trait BackingObserver: Send + Sync + 'static {
    fn reserve(&self, _request: ReservationRequest) -> Result<(), BackingFailure> {
        Ok(())
    }

    fn write(&self, _offset: u64, _source: &[u8]) -> Result<(), BackingFailure> {
        Ok(())
    }

    fn commit(&self, _committed: &[u8]) -> Result<(), BackingFailure> {
        Ok(())
    }

    fn rollback(&self) -> Result<(), BackingFailure> {
        Ok(())
    }

    fn release(&self) {}
}

struct NoopObserver;
impl BackingObserver for NoopObserver {}

struct BackingContext {
    observer: Arc<dyn BackingObserver>,
    execution: Mutex<()>,
}

impl BackingContext {
    fn reserve(&self, request: ReservationRequest) -> Result<AlignedBytes, BackingFailure> {
        self.observer.reserve(request)?;
        AlignedBytes::allocate(request.minimum_capacity, request.alignment)
    }
}

#[derive(Clone)]
pub struct MemoryBacking {
    context: Arc<BackingContext>,
}

impl MemoryBacking {
    pub fn new() -> Self {
        Self::direct()
    }

    /// Creates binding-owned aligned storage for direct or staged execution.
    ///
    /// Executions sharing a clone of this backing are serialized as required
    /// by the Core callback contract. Observer callbacks must not re-enter an
    /// execution using the same backing.
    pub fn direct() -> Self {
        Self::with_observer(Arc::new(NoopObserver))
    }

    pub fn with_observer<O>(observer: Arc<O>) -> Self
    where
        O: BackingObserver,
    {
        let observer: Arc<dyn BackingObserver> = observer;
        Self {
            context: Arc::new(BackingContext {
                observer,
                execution: Mutex::new(()),
            }),
        }
    }

    fn raw(&self) -> sys::fdb_payload_v1_backing_v1_t {
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_backing_v1_t>::uninit();
        // SAFETY: Core initializes the complete target structure.
        unsafe { sys::fdb_payload_v1_backing_init(raw.as_mut_ptr()) };
        // SAFETY: the initializer wrote the complete structure.
        let mut raw = unsafe { raw.assume_init() };
        raw.context = Arc::as_ptr(&self.context).cast_mut().cast::<c_void>();
        raw.reserve = Some(memory_reserve);
        raw.write = Some(memory_write);
        raw.commit = Some(memory_commit);
        raw.rollback = Some(memory_rollback);
        raw.retain = Some(memory_retain);
        raw.release = Some(memory_release);
        raw
    }
}

impl Default for MemoryBacking {
    fn default() -> Self {
        Self::new()
    }
}

impl Backing for MemoryBacking {
    fn reserve(
        &self,
        mode: ReserveMode,
        minimum_capacity: usize,
        alignment: usize,
    ) -> Result<Box<dyn ReservedBacking>, BackingStatus> {
        let request = ReservationRequest {
            mode,
            minimum_capacity: u64::try_from(minimum_capacity)
                .map_err(|_| BackingStatus::AllocationFailed)?,
            alignment: u32::try_from(alignment).map_err(|_| BackingStatus::AllocationFailed)?,
        };
        Ok(Box::new(MemoryReservation {
            storage: self.context.reserve(request)?,
            observer: self.context.observer.clone(),
        }))
    }

    fn memory_backing(&self, _: backing_dispatch::Key) -> Option<&MemoryBacking> {
        Some(self)
    }
}

impl fmt::Debug for MemoryBacking {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("MemoryBacking")
            .finish_non_exhaustive()
    }
}

struct AlignedBytes {
    data: NonNull<u8>,
    capacity: usize,
    layout: Layout,
}

impl AlignedBytes {
    fn allocate(capacity: u64, alignment: u32) -> Result<Self, BackingFailure> {
        let capacity = usize::try_from(capacity).map_err(|_| BackingFailure::AllocationFailed)?;
        let alignment = usize::try_from(alignment).map_err(|_| BackingFailure::AllocationFailed)?;
        let layout = Layout::from_size_align(capacity.max(1), alignment)
            .map_err(|_| BackingFailure::AllocationFailed)?;
        // SAFETY: layout is valid and nonzero-sized.
        let data = unsafe { alloc_zeroed(layout) };
        let data = NonNull::new(data).ok_or(BackingFailure::AllocationFailed)?;
        Ok(Self {
            data,
            capacity,
            layout,
        })
    }

    fn as_slice(&self, size: usize) -> &[u8] {
        // SAFETY: callers validate size <= capacity and storage remains live.
        unsafe { std::slice::from_raw_parts(self.data.as_ptr(), size) }
    }

    fn as_mut_slice(&mut self) -> &mut [u8] {
        // SAFETY: the callback contract serializes all pre-commit mutation.
        unsafe { std::slice::from_raw_parts_mut(self.data.as_ptr(), self.capacity) }
    }
}

impl Drop for AlignedBytes {
    fn drop(&mut self) {
        // SAFETY: data was allocated once with this exact layout.
        unsafe { dealloc(self.data.as_ptr(), self.layout) };
    }
}

unsafe impl Send for AlignedBytes {}
unsafe impl Sync for AlignedBytes {}

struct MemoryReservation {
    storage: AlignedBytes,
    observer: Arc<dyn BackingObserver>,
}

impl ReservedBacking for MemoryReservation {
    fn as_mut_slice(&mut self) -> &mut [u8] {
        self.storage.as_mut_slice()
    }

    fn commit(
        self: Box<Self>,
        used_bytes: usize,
    ) -> Result<Box<dyn CommittedBacking>, BackingStatus> {
        if used_bytes > self.storage.capacity {
            return Err(BackingStatus::Contract);
        }
        self.observer.commit(self.storage.as_slice(used_bytes))?;
        Ok(Box::new(CommittedMemory {
            reservation: *self,
            used_bytes,
        }))
    }

    fn rollback(self: Box<Self>) {
        let _ = self.observer.rollback();
    }
}

struct CommittedMemory {
    reservation: MemoryReservation,
    used_bytes: usize,
}

impl CommittedBacking for CommittedMemory {
    fn as_slice(&self) -> &[u8] {
        self.reservation.storage.as_slice(self.used_bytes)
    }
}

impl Drop for CommittedMemory {
    fn drop(&mut self) {
        self.reservation.observer.release();
    }
}

struct ReservationToken {
    references: AtomicUsize,
    storage: AlignedBytes,
    observer: Arc<dyn BackingObserver>,
}

unsafe extern "C" fn memory_reserve(
    context: *mut c_void,
    reserve_mode: u32,
    minimum_capacity: u64,
    alignment: u32,
    out_owner_token: *mut *mut c_void,
    out_writable_data: *mut *mut u8,
    out_capacity: *mut u64,
) -> u32 {
    callback_status(|| {
        if context.is_null()
            || out_owner_token.is_null()
            || out_writable_data.is_null()
            || out_capacity.is_null()
        {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        let Ok(mode) = ReserveMode::from_raw(reserve_mode) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        // SAFETY: context is the live BackingContext borrowed for plan_execute.
        let context = unsafe { &*context.cast::<BackingContext>() };
        let request = ReservationRequest {
            mode,
            minimum_capacity,
            alignment,
        };
        let storage = match context.reserve(request) {
            Ok(storage) => storage,
            Err(failure) => return failure.status(),
        };
        let token = Box::new(ReservationToken {
            references: AtomicUsize::new(1),
            storage,
            observer: context.observer.clone(),
        });
        let writable = if mode == ReserveMode::Direct {
            token.storage.data.as_ptr()
        } else {
            ptr::null_mut()
        };
        let token = Box::into_raw(token);
        // SAFETY: all outputs were checked and Core owns the initial token ref.
        unsafe {
            *out_owner_token = token.cast::<c_void>();
            *out_writable_data = writable;
            *out_capacity = minimum_capacity;
        }
        0
    })
}

unsafe extern "C" fn memory_write(
    _context: *mut c_void,
    owner_token: *mut c_void,
    offset: u64,
    source: *const u8,
    source_size: u64,
) -> u32 {
    callback_status(|| {
        let Some(token) = token_mut(owner_token) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        let Ok(offset_usize) = usize::try_from(offset) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        let Ok(size) = usize::try_from(source_size) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        if size != 0 && source.is_null() {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        let Some(end) = offset_usize.checked_add(size) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        if end > token.storage.capacity {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        let source = if size == 0 {
            &[]
        } else {
            // SAFETY: Core supplies a live non-empty source span for this callback.
            unsafe { std::slice::from_raw_parts(source, size) }
        };
        if let Err(failure) = token.observer.write(offset, source) {
            return failure.status();
        }
        token.storage.as_mut_slice()[offset_usize..end].copy_from_slice(source);
        0
    })
}

unsafe extern "C" fn memory_commit(
    _context: *mut c_void,
    owner_token: *mut c_void,
    used_size: u64,
    out_readable_data: *mut *const u8,
    out_readable_size: *mut u64,
) -> u32 {
    callback_status(|| {
        if out_readable_data.is_null() || out_readable_size.is_null() {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        let Some(token) = token_mut(owner_token) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        let Ok(size) = usize::try_from(used_size) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        if size > token.storage.capacity {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        if let Err(failure) = token.observer.commit(token.storage.as_slice(size)) {
            return failure.status();
        }
        // SAFETY: outputs are valid and token storage stays live until release.
        unsafe {
            *out_readable_data = token.storage.data.as_ptr();
            *out_readable_size = used_size;
        }
        0
    })
}

unsafe extern "C" fn memory_rollback(_context: *mut c_void, owner_token: *mut c_void) -> u32 {
    if owner_token.is_null() {
        return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
    }
    callback_status(|| {
        // SAFETY: rollback consumes the one uncommitted token reference.
        let token = unsafe { Box::from_raw(owner_token.cast::<ReservationToken>()) };
        token
            .observer
            .rollback()
            .map_or_else(BackingFailure::status, |()| 0)
    })
}

unsafe extern "C" fn memory_retain(_context: *mut c_void, owner_token: *mut c_void) -> u32 {
    callback_status(|| {
        let Some(token) = token_ref(owner_token) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        // Keep the edition-2024 baseline: try_update requires Rust 1.95.
        #[allow(deprecated)]
        let retained =
            token
                .references
                .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |count| {
                    count.checked_add(1)
                });
        if retained.is_err() {
            return sys::FDB_PAYLOAD_E_ALLOCATION_FAILED;
        }
        0
    })
}

unsafe extern "C" fn memory_release(_context: *mut c_void, owner_token: *mut c_void) {
    if owner_token.is_null() {
        return;
    }
    callback_result(|| {
        // SAFETY: Core calls release only for a live committed token reference.
        let token = unsafe { &*owner_token.cast::<ReservationToken>() };
        if token.references.fetch_sub(1, Ordering::AcqRel) == 1 {
            // SAFETY: this thread consumed the final token reference.
            let token = unsafe { Box::from_raw(owner_token.cast::<ReservationToken>()) };
            token.observer.release();
        }
    });
}

fn token_ref(owner_token: *mut c_void) -> Option<&'static ReservationToken> {
    // SAFETY: callers use this only during a Core callback holding a token ref.
    unsafe { owner_token.cast::<ReservationToken>().as_ref() }
}

fn token_mut(owner_token: *mut c_void) -> Option<&'static mut ReservationToken> {
    // SAFETY: Core serializes reserve/write/commit/rollback for one token.
    unsafe { owner_token.cast::<ReservationToken>().as_mut() }
}

struct BackingReservation {
    owner: Box<dyn ReservedBacking>,
    address: usize,
    capacity: usize,
}

struct BackingToken {
    reservation: Mutex<Option<BackingReservation>>,
    committed: OnceLock<Box<dyn CommittedBacking>>,
}

unsafe extern "C" fn backing_reserve<B: Backing + ?Sized>(
    context: *mut c_void,
    reserve_mode: u32,
    minimum_capacity: u64,
    alignment: u32,
    out_owner_token: *mut *mut c_void,
    out_writable_data: *mut *mut u8,
    out_capacity: *mut u64,
) -> u32 {
    callback_status(|| {
        // SAFETY: the execution context holds this reference for the call.
        let backing = unsafe { context.cast::<&B>().as_ref() };
        let (Some(backing), Ok(mode), Ok(minimum_capacity)) = (
            backing,
            ReserveMode::from_raw(reserve_mode),
            usize::try_from(minimum_capacity),
        ) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        let alignment = alignment as usize;
        if !alignment.is_power_of_two()
            || out_owner_token.is_null()
            || out_writable_data.is_null()
            || out_capacity.is_null()
        {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        let owner = match backing.reserve(mode, minimum_capacity, alignment) {
            Ok(owner) => owner,
            Err(status) => return status.status(),
        };
        let mut token = Arc::new(BackingToken {
            reservation: Mutex::new(Some(BackingReservation {
                owner,
                address: 0,
                capacity: 0,
            })),
            committed: OnceLock::new(),
        });
        let slot = Arc::get_mut(&mut token)
            .unwrap()
            .reservation
            .get_mut()
            .unwrap();
        let reservation = slot.as_mut().unwrap();
        // Borrow only after the owner is in its final location. Neither the
        // reservation nor the slice is accessed by Rust until Core is done
        // writing and calls commit or rollback.
        let slice = reservation.owner.as_mut_slice();
        let capacity = slice.len();
        let writable = slice.as_mut_ptr();
        let address = writable as usize;
        if capacity < minimum_capacity || address & (alignment - 1) != 0 {
            slot.take().unwrap().owner.rollback();
            return sys::FDB_PAYLOAD_E_DIRECT_UNAVAILABLE;
        }
        reservation.address = address;
        reservation.capacity = capacity;
        // SAFETY: Core owns one token reference and uses the writable span
        // exclusively up to commit or rollback.
        unsafe {
            *out_owner_token = Arc::into_raw(token).cast_mut().cast();
            *out_writable_data = writable;
            *out_capacity = capacity as u64;
        }
        0
    })
}

unsafe extern "C" fn backing_commit(
    _context: *mut c_void,
    owner_token: *mut c_void,
    used_size: u64,
    out_readable_data: *mut *const u8,
    out_readable_size: *mut u64,
) -> u32 {
    callback_status(|| {
        // SAFETY: Core holds the reserved token reference during commit.
        let token = unsafe { owner_token.cast::<BackingToken>().as_ref() };
        let (Some(token), Ok(size)) = (token, usize::try_from(used_size)) else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        if out_readable_data.is_null() || out_readable_size.is_null() {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        let mut slot = token
            .reservation
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        let Some(reservation) = slot.as_ref() else {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        };
        if size > reservation.capacity {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        let BackingReservation { owner, address, .. } = slot.take().unwrap();
        drop(slot);
        let committed = match owner.commit(size) {
            Ok(committed) => committed,
            Err(status) => return status.status(),
        };
        // Moving a Box into Arc can relocate inline storage. Keep the Box in
        // the Arc-owned token and take the shared borrow after publication.
        if token.committed.set(committed).is_err() {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        let readable = token.committed.get().unwrap().as_slice();
        if readable.len() < size || readable.as_ptr() as usize != address {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        // SAFETY: Core owns the token for the lifetime of the immutable span.
        unsafe {
            *out_readable_data = readable.as_ptr();
            *out_readable_size = used_size;
        }
        0
    })
}

unsafe extern "C" fn backing_rollback(_context: *mut c_void, owner_token: *mut c_void) -> u32 {
    callback_status(|| {
        if owner_token.is_null() {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        // SAFETY: Core relinquishes the initial token reference on rollback.
        let token = unsafe { Arc::from_raw(owner_token.cast::<BackingToken>()) };
        let reservation = token
            .reservation
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .take();
        if let Some(reservation) = reservation {
            reservation.owner.rollback();
        }
        0
    })
}

unsafe extern "C" fn retain_owner<T>(_context: *mut c_void, owner_token: *mut c_void) -> u32 {
    callback_status(|| {
        if owner_token.is_null() {
            return sys::FDB_PAYLOAD_E_BACKING_CONTRACT;
        }
        // SAFETY: Core holds a live committed token reference.
        unsafe { Arc::increment_strong_count(owner_token.cast::<T>()) };
        0
    })
}

unsafe extern "C" fn release_owner<T>(_context: *mut c_void, owner_token: *mut c_void) {
    if !owner_token.is_null() {
        callback_result(|| {
            // SAFETY: release relinquishes one committed token reference.
            unsafe { Arc::decrement_strong_count(owner_token.cast::<T>()) };
        });
    }
}

fn callback_status(operation: impl FnOnce() -> u32) -> u32 {
    callback_result(operation).unwrap_or(sys::FDB_PAYLOAD_E_BACKING_CONTRACT)
}

fn callback_result<T>(operation: impl FnOnce() -> T) -> Option<T> {
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(result) => Some(result),
        Err(panic) => match catch_unwind(AssertUnwindSafe(|| drop(panic))) {
            // A user-supplied panic payload can itself panic during destruction.
            Ok(()) => None,
            Err(_panic) => std::process::abort(),
        },
    }
}

struct ExternalInner {
    bytes: *const [u8],
    _owner: Arc<dyn Send + Sync>,
}

// SAFETY: the span came from one shared borrow of the heap-owned value. The
// owner remains live, is never accessed mutably, and is safe to share and drop
// on any thread.
unsafe impl Send for ExternalInner {}
unsafe impl Sync for ExternalInner {}

#[derive(Clone)]
pub struct ExternalBytes {
    inner: Arc<ExternalInner>,
}

impl ExternalBytes {
    pub fn new(bytes: Vec<u8>) -> Self {
        Self::from_owner(bytes)
    }

    /// Retains an owner and borrows its bytes without copying them.
    /// The owner is stored on the heap before borrowing, including when its
    /// bytes are stored inline. Builders, frozen plans, and open payloads
    /// (including their views) retain the owner while they need its bytes.
    pub fn from_owner<T>(owner: T) -> Self
    where
        T: AsRef<[u8]> + Send + Sync + 'static,
    {
        let owner = Arc::new(owner);
        let bytes = (*owner).as_ref() as *const [u8];
        Self {
            inner: Arc::new(ExternalInner {
                bytes,
                _owner: owner,
            }),
        }
    }

    /// Retains an existing shared byte allocation without copying it.
    pub fn from_arc(bytes: Arc<[u8]>) -> Self {
        Self::from_owner(bytes)
    }

    pub fn as_bytes(&self) -> &[u8] {
        // SAFETY: the immutable span is kept live by self.inner._owner.
        unsafe { &*self.inner.bytes }
    }

    fn owner_token(&self) -> *mut c_void {
        Arc::as_ptr(&self.inner).cast_mut().cast::<c_void>()
    }

    fn raw_backing(&self) -> sys::fdb_payload_v1_backing_v1_t {
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_backing_v1_t>::uninit();
        // SAFETY: Core initializes the complete target structure.
        unsafe { sys::fdb_payload_v1_backing_init(raw.as_mut_ptr()) };
        // SAFETY: the initializer wrote the complete structure.
        let mut raw = unsafe { raw.assume_init() };
        raw.retain = Some(retain_owner::<ExternalInner>);
        raw.release = Some(release_owner::<ExternalInner>);
        raw
    }
}

impl fmt::Debug for ExternalBytes {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ExternalBytes")
            .field("len", &self.as_bytes().len())
            .finish()
    }
}

impl Builder {
    /// Adds a bytes value without copying it into the builder.
    /// On success, the builder retains the owner. [`Builder::freeze`]
    /// transfers that hold to the build plan.
    pub fn value_external_bytes(
        &mut self,
        bytes: &ExternalBytes,
    ) -> Result<&mut Self, PayloadError> {
        let value = bytes.as_bytes();
        let size =
            u64::try_from(value.len()).map_err(|_| PayloadError::size_overflow("/value/bytes"))?;
        let backing = bytes.raw_backing();
        let mut error = ptr::null_mut();
        // SAFETY: self is live, error is a valid sink, and Core retains the
        // immutable owner token before keeping the span beyond this call.
        let status = unsafe {
            sys::fdb_payload_v1_builder_value_external_bytes(
                self.as_raw(),
                slice_pointer(value),
                size,
                &backing,
                bytes.owner_token(),
                &mut error,
            )
        };
        check_status(status, error)?;
        Ok(self)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum ViewKind {
    Sequence = sys::FDB_PAYLOAD_VIEW_SEQUENCE,
    Bool = sys::FDB_PAYLOAD_VIEW_BOOL,
    U8 = sys::FDB_PAYLOAD_VIEW_U8,
    U16 = sys::FDB_PAYLOAD_VIEW_U16,
    U32 = sys::FDB_PAYLOAD_VIEW_U32,
    I32 = sys::FDB_PAYLOAD_VIEW_I32,
    U8n = sys::FDB_PAYLOAD_VIEW_U8N,
    U16n = sys::FDB_PAYLOAD_VIEW_U16N,
    F32 = sys::FDB_PAYLOAD_VIEW_F32,
    F64 = sys::FDB_PAYLOAD_VIEW_F64,
    Str = sys::FDB_PAYLOAD_VIEW_STR,
    Wstr = sys::FDB_PAYLOAD_VIEW_WSTR,
    Bytes = sys::FDB_PAYLOAD_VIEW_BYTES,
    Component = sys::FDB_PAYLOAD_VIEW_COMPONENT,
    List = sys::FDB_PAYLOAD_VIEW_LIST,
    Ref = sys::FDB_PAYLOAD_VIEW_REF,
}

impl ViewKind {
    fn from_raw(value: u32) -> Result<Self, PayloadError> {
        match value {
            sys::FDB_PAYLOAD_VIEW_SEQUENCE => Ok(Self::Sequence),
            sys::FDB_PAYLOAD_VIEW_BOOL => Ok(Self::Bool),
            sys::FDB_PAYLOAD_VIEW_U8 => Ok(Self::U8),
            sys::FDB_PAYLOAD_VIEW_U16 => Ok(Self::U16),
            sys::FDB_PAYLOAD_VIEW_U32 => Ok(Self::U32),
            sys::FDB_PAYLOAD_VIEW_I32 => Ok(Self::I32),
            sys::FDB_PAYLOAD_VIEW_U8N => Ok(Self::U8n),
            sys::FDB_PAYLOAD_VIEW_U16N => Ok(Self::U16n),
            sys::FDB_PAYLOAD_VIEW_F32 => Ok(Self::F32),
            sys::FDB_PAYLOAD_VIEW_F64 => Ok(Self::F64),
            sys::FDB_PAYLOAD_VIEW_STR => Ok(Self::Str),
            sys::FDB_PAYLOAD_VIEW_WSTR => Ok(Self::Wstr),
            sys::FDB_PAYLOAD_VIEW_BYTES => Ok(Self::Bytes),
            sys::FDB_PAYLOAD_VIEW_COMPONENT => Ok(Self::Component),
            sys::FDB_PAYLOAD_VIEW_LIST => Ok(Self::List),
            sys::FDB_PAYLOAD_VIEW_REF => Ok(Self::Ref),
            _ => Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_UNSUPPORTED_ABI,
                "UNSUPPORTED_ABI",
                "/view/kind",
                "FastDB Core returned an unknown payload view kind",
                format!(r#"{{"actual":{value},"reason":"unknown_view_kind"}}"#),
            )),
        }
    }
}

/// Core-owned identity coordinates meaningful only within one payload owner.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct GraphIdentity {
    pub component_index: u32,
    pub object_id: u64,
}

type ViewU8Getter = unsafe extern "C" fn(
    *const sys::fdb_payload_v1_view_t,
    *mut u8,
    *mut *mut sys::fdb_payload_v1_error_t,
) -> u32;
type ViewU16Getter = unsafe extern "C" fn(
    *const sys::fdb_payload_v1_view_t,
    *mut u16,
    *mut *mut sys::fdb_payload_v1_error_t,
) -> u32;
type ViewU32Getter = unsafe extern "C" fn(
    *const sys::fdb_payload_v1_view_t,
    *mut u32,
    *mut *mut sys::fdb_payload_v1_error_t,
) -> u32;
type ViewU64Getter = unsafe extern "C" fn(
    *const sys::fdb_payload_v1_view_t,
    *mut u64,
    *mut *mut sys::fdb_payload_v1_error_t,
) -> u32;
type AccessBytesGetter = unsafe extern "C" fn(
    *const sys::fdb_payload_v1_access_t,
    *mut *const u8,
    *mut u64,
    *mut *mut sys::fdb_payload_v1_error_t,
) -> u32;

/// A retainable immutable view whose backed operations are generation checked
/// by FastDB Core. No mutation surface is projected.
pub struct View {
    raw: NonNull<sys::fdb_payload_v1_view_t>,
}

impl View {
    fn from_raw(raw: *mut sys::fdb_payload_v1_view_t) -> Result<Self, PayloadError> {
        NonNull::new(raw).map(|raw| Self { raw }).ok_or_else(|| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "",
                "FastDB Core returned success without a view",
                r#"{"reason":"missing_view_handle"}"#,
            )
        })
    }

    pub fn require_spec_sha256(&self, expected: &[u8; 32]) -> Result<(), PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self is live, expected has the required fixed length, and error is valid.
        let status = unsafe {
            sys::fdb_payload_v1_view_require_spec_sha256(
                self.raw.as_ptr(),
                expected.as_ptr(),
                &mut error,
            )
        };
        check_status(status, error)
    }

    pub fn kind(&self) -> Result<ViewKind, PayloadError> {
        let value = self.read_u32(sys::fdb_payload_v1_view_kind)?;
        ViewKind::from_raw(value)
    }

    pub fn is_null(&self) -> Result<bool, PayloadError> {
        Ok(self.read_u8(sys::fdb_payload_v1_view_is_null)? != 0)
    }

    pub fn length(&self) -> Result<u64, PayloadError> {
        self.read_u64(sys::fdb_payload_v1_view_length)
    }

    pub fn at(&self, index: u64) -> Result<Self, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and the owned-handle/error outputs are valid.
        let status =
            unsafe { sys::fdb_payload_v1_view_at(self.raw.as_ptr(), index, &mut raw, &mut error) };
        check_status(status, error)?;
        Self::from_raw(raw)
    }

    pub fn component_index(&self) -> Result<u32, PayloadError> {
        self.read_u32(sys::fdb_payload_v1_view_component_index)
    }

    pub fn field_count(&self) -> Result<u32, PayloadError> {
        self.read_u32(sys::fdb_payload_v1_view_field_count)
    }

    pub fn field(&self, index: u32) -> Result<Self, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and the owned-handle/error outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_view_field(self.raw.as_ptr(), index, &mut raw, &mut error)
        };
        check_status(status, error)?;
        Self::from_raw(raw)
    }

    /// Follows exactly one explicit Core ref edge. Ordinary field navigation
    /// never dereferences a ref implicitly.
    pub fn ref_target(&self) -> Result<Self, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and the owned-handle/error outputs are valid.
        let status =
            unsafe { sys::fdb_payload_v1_view_ref_target(self.raw.as_ptr(), &mut raw, &mut error) };
        check_status(status, error)?;
        Self::from_raw(raw)
    }

    /// Returns the exact payload-scoped Core identity pair. Equal coordinates
    /// from different payload owners do not establish cross-payload identity.
    pub fn graph_identity(&self) -> Result<GraphIdentity, PayloadError> {
        let mut component_index = 0_u32;
        let mut object_id = 0_u64;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and all scalar/error outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_view_graph_identity(
                self.raw.as_ptr(),
                &mut component_index,
                &mut object_id,
                &mut error,
            )
        };
        check_status(status, error)?;
        Ok(GraphIdentity {
            component_index,
            object_id,
        })
    }

    pub fn get_bool(&self) -> Result<bool, PayloadError> {
        Ok(self.read_u8(sys::fdb_payload_v1_view_get_bool)? != 0)
    }

    pub fn get_u8(&self) -> Result<u8, PayloadError> {
        self.read_u8(sys::fdb_payload_v1_view_get_u8)
    }

    pub fn get_u16(&self) -> Result<u16, PayloadError> {
        self.read_u16(sys::fdb_payload_v1_view_get_u16)
    }

    pub fn get_u32(&self) -> Result<u32, PayloadError> {
        self.read_u32(sys::fdb_payload_v1_view_get_u32)
    }

    pub fn get_i32(&self) -> Result<i32, PayloadError> {
        let mut value = 0_i32;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and value/error outputs are valid.
        let status =
            unsafe { sys::fdb_payload_v1_view_get_i32(self.raw.as_ptr(), &mut value, &mut error) };
        check_status(status, error)?;
        Ok(value)
    }

    pub fn get_u8n_f64_bits(&self) -> Result<u64, PayloadError> {
        self.read_u64(sys::fdb_payload_v1_view_get_u8n_f64_bits)
    }

    pub fn get_u8n(&self) -> Result<f64, PayloadError> {
        Ok(f64::from_bits(self.get_u8n_f64_bits()?))
    }

    pub fn get_u16n_f64_bits(&self) -> Result<u64, PayloadError> {
        self.read_u64(sys::fdb_payload_v1_view_get_u16n_f64_bits)
    }

    pub fn get_u16n(&self) -> Result<f64, PayloadError> {
        Ok(f64::from_bits(self.get_u16n_f64_bits()?))
    }

    pub fn get_f32_bits(&self) -> Result<u32, PayloadError> {
        self.read_u32(sys::fdb_payload_v1_view_get_f32_bits)
    }

    pub fn get_f32(&self) -> Result<f32, PayloadError> {
        Ok(f32::from_bits(self.get_f32_bits()?))
    }

    pub fn get_f64_bits(&self) -> Result<u64, PayloadError> {
        self.read_u64(sys::fdb_payload_v1_view_get_f64_bits)
    }

    pub fn get_f64(&self) -> Result<f64, PayloadError> {
        Ok(f64::from_bits(self.get_f64_bits()?))
    }

    pub fn acquire(&self) -> Result<Access, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and the unique-handle/error outputs are valid.
        let status =
            unsafe { sys::fdb_payload_v1_view_acquire(self.raw.as_ptr(), &mut raw, &mut error) };
        check_status(status, error)?;
        Access::from_raw(raw)
    }

    /// Performs exactly one Core materialization call. The returned view owns
    /// detached Core state and does not depend on this view's payload owner.
    pub fn materialize(&self) -> Result<Self, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and the owned-handle/error outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_view_materialize(self.raw.as_ptr(), &mut raw, &mut error)
        };
        check_status(status, error)?;
        Self::from_raw(raw)
    }

    fn read_u8(&self, getter: ViewU8Getter) -> Result<u8, PayloadError> {
        let mut value = 0_u8;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and value/error outputs are valid.
        let status = unsafe { getter(self.raw.as_ptr(), &mut value, &mut error) };
        check_status(status, error)?;
        Ok(value)
    }

    fn read_u16(&self, getter: ViewU16Getter) -> Result<u16, PayloadError> {
        let mut value = 0_u16;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and value/error outputs are valid.
        let status = unsafe { getter(self.raw.as_ptr(), &mut value, &mut error) };
        check_status(status, error)?;
        Ok(value)
    }

    fn read_u32(&self, getter: ViewU32Getter) -> Result<u32, PayloadError> {
        let mut value = 0_u32;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and value/error outputs are valid.
        let status = unsafe { getter(self.raw.as_ptr(), &mut value, &mut error) };
        check_status(status, error)?;
        Ok(value)
    }

    fn read_u64(&self, getter: ViewU64Getter) -> Result<u64, PayloadError> {
        let mut value = 0_u64;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and value/error outputs are valid.
        let status = unsafe { getter(self.raw.as_ptr(), &mut value, &mut error) };
        check_status(status, error)?;
        Ok(value)
    }
}

impl Clone for View {
    fn clone(&self) -> Self {
        // SAFETY: self owns a live atomically retainable view.
        unsafe { sys::fdb_payload_v1_view_retain(self.raw.as_ptr()) };
        Self { raw: self.raw }
    }
}

impl Drop for View {
    fn drop(&mut self) {
        // SAFETY: this releases exactly the view reference owned by self.
        unsafe { sys::fdb_payload_v1_view_release(self.raw.as_ptr()) };
    }
}

impl fmt::Debug for View {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("View").finish_non_exhaustive()
    }
}

unsafe impl Send for View {}

/// Unique owner of one live Core access pin. Every returned borrow is tied to
/// `&self`, so safe Rust cannot use it after this guard is dropped.
pub struct Access {
    raw: NonNull<sys::fdb_payload_v1_access_t>,
}

impl Access {
    fn from_raw(raw: *mut sys::fdb_payload_v1_access_t) -> Result<Self, PayloadError> {
        NonNull::new(raw).map(|raw| Self { raw }).ok_or_else(|| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "",
                "FastDB Core returned success without an access pin",
                r#"{"reason":"missing_access_handle"}"#,
            )
        })
    }

    /// The borrow cannot escape this unique access guard.
    ///
    /// ```compile_fail
    /// use fastdb::Access;
    ///
    /// fn invalid_escape(access: Access) -> &'static [u8] {
    ///     access.payload_bytes().unwrap()
    /// }
    /// ```
    pub fn payload_bytes(&self) -> Result<&[u8], PayloadError> {
        self.byte_span(sys::fdb_payload_v1_access_payload_bytes)
    }

    pub fn str(&self) -> Result<&str, PayloadError> {
        let bytes = self.byte_span(sys::fdb_payload_v1_access_str)?;
        std::str::from_utf8(bytes).map_err(|_| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "/access",
                "FastDB Core returned invalid UTF-8 from a validated str view",
                r#"{"reason":"invalid_core_utf8"}"#,
            )
        })
    }

    pub fn wstr(&self) -> Result<&[u16], PayloadError> {
        let mut data = ptr::null();
        let mut size = 0_u64;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and pointer/count/error outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_access_wstr(self.raw.as_ptr(), &mut data, &mut size, &mut error)
        };
        check_status(status, error)?;
        let size = checked_span_len::<u16>(size, "/access/wstr")?;
        if size == 0 {
            return Ok(&[]);
        }
        if data.is_null() || !(data as usize).is_multiple_of(std::mem::align_of::<u16>()) {
            return Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "/access/wstr",
                "FastDB Core returned invalid aligned wstr storage",
                r#"{"reason":"invalid_core_wstr_span"}"#,
            ));
        }
        // SAFETY: Core owns aligned host-endian units for the lifetime of this
        // unique access handle, and checked_span_len bounded the slice extent.
        Ok(unsafe { std::slice::from_raw_parts(data, size) })
    }

    pub fn bytes(&self) -> Result<&[u8], PayloadError> {
        self.byte_span(sys::fdb_payload_v1_access_bytes)
    }

    fn byte_span(&self, getter: AccessBytesGetter) -> Result<&[u8], PayloadError> {
        let mut data = ptr::null();
        let mut size = 0_u64;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and pointer/count/error outputs are valid.
        let status = unsafe { getter(self.raw.as_ptr(), &mut data, &mut size, &mut error) };
        check_status(status, error)?;
        let size = checked_span_len::<u8>(size, "/access")?;
        if size == 0 {
            return Ok(&[]);
        }
        if data.is_null() {
            return Err(PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "/access",
                "FastDB Core returned non-empty access storage with a null pointer",
                r#"{"reason":"invalid_core_byte_span"}"#,
            ));
        }
        // SAFETY: Core keeps this byte span live until the unique access handle
        // is released, and checked_span_len bounded the slice extent.
        Ok(unsafe { std::slice::from_raw_parts(data, size) })
    }
}

impl Drop for Access {
    fn drop(&mut self) {
        // SAFETY: this releases exactly the unique access handle owned by self.
        unsafe { sys::fdb_payload_v1_access_release(self.raw.as_ptr()) };
    }
}

impl fmt::Debug for Access {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("Access").finish_non_exhaustive()
    }
}

unsafe impl Send for Access {}

fn checked_span_len<T>(size: u64, path: &'static str) -> Result<usize, PayloadError> {
    let length = usize::try_from(size).map_err(|_| PayloadError::size_overflow(path))?;
    let bytes = length
        .checked_mul(std::mem::size_of::<T>())
        .ok_or_else(|| PayloadError::size_overflow(path))?;
    if bytes > isize::MAX as usize {
        return Err(PayloadError::size_overflow(path));
    }
    Ok(length)
}

pub struct Payload {
    raw: NonNull<sys::fdb_payload_v1_payload_t>,
}

impl Payload {
    fn from_raw(raw: *mut sys::fdb_payload_v1_payload_t) -> Result<Self, PayloadError> {
        NonNull::new(raw).map(|raw| Self { raw }).ok_or_else(|| {
            PayloadError::binding(
                sys::FDB_PAYLOAD_E_INTERNAL,
                "BINDING_CONTRACT",
                "",
                "FastDB Core returned success without a payload",
                r#"{"reason":"missing_payload_handle"}"#,
            )
        })
    }

    pub fn require_spec_sha256(&self, expected: &[u8; 32]) -> Result<(), PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self is live, expected has the required fixed length, and error is valid.
        let status = unsafe {
            sys::fdb_payload_v1_payload_require_spec_sha256(
                self.raw.as_ptr(),
                expected.as_ptr(),
                &mut error,
            )
        };
        check_status(status, error)
    }

    pub fn open_copy(
        spec: &CompiledSpec,
        bytes: &[u8],
        options: &OpenOptions,
    ) -> Result<Self, PayloadError> {
        let size = u64::try_from(bytes.len())
            .map_err(|_| PayloadError::size_overflow("/payload/bytes"))?;
        let options = options.to_raw();
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: all inputs stay live for this synchronous copying call.
        let status = unsafe {
            sys::fdb_payload_v1_payload_open_copy(
                spec.as_raw(),
                slice_pointer(bytes),
                size,
                &options,
                &mut raw,
                &mut error,
            )
        };
        check_status(status, error)?;
        Self::from_raw(raw)
    }

    pub fn open_external(
        spec: &CompiledSpec,
        bytes: &ExternalBytes,
        options: &OpenOptions,
    ) -> Result<Self, PayloadError> {
        let size = u64::try_from(bytes.as_bytes().len())
            .map_err(|_| PayloadError::size_overflow("/payload/bytes"))?;
        let backing = bytes.raw_backing();
        let options = options.to_raw();
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: Core retains the immutable owner token before this call returns.
        let status = unsafe {
            sys::fdb_payload_v1_payload_open_external(
                spec.as_raw(),
                slice_pointer(bytes.as_bytes()),
                size,
                &backing,
                bytes.owner_token(),
                &options,
                &mut raw,
                &mut error,
            )
        };
        check_status(status, error)?;
        Self::from_raw(raw)
    }

    pub fn sha256(&self) -> Result<[u8; 32], PayloadError> {
        let mut digest = [0_u8; sys::FDB_PAYLOAD_V1_SHA256_SIZE as usize];
        let mut error = ptr::null_mut();
        // SAFETY: self is live and digest/error outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_payload_sha256(self.raw.as_ptr(), digest.as_mut_ptr(), &mut error)
        };
        check_status(status, error)?;
        Ok(digest)
    }

    pub fn profile(&self) -> Result<Profile, PayloadError> {
        let mut profile = 0_u32;
        let mut error = ptr::null_mut();
        // SAFETY: self is live and outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_payload_profile(self.raw.as_ptr(), &mut profile, &mut error)
        };
        check_status(status, error)?;
        Profile::from_raw(profile)
    }

    pub fn execution_report(&self) -> Result<ExecutionReport, PayloadError> {
        let mut raw = initialized_report();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_payload_execution_report(self.raw.as_ptr(), &mut raw, &mut error)
        };
        check_status(status, error)?;
        ExecutionReport::from_raw(&raw)
    }

    pub fn binary_blob(&self) -> Result<Blob, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_payload_binary_blob(self.raw.as_ptr(), &mut raw, &mut error)
        };
        check_status(status, error)?;
        // SAFETY: success publishes one owned immutable blob.
        unsafe { Blob::from_raw(raw) }
    }

    pub fn binary_bytes(&self) -> Result<Vec<u8>, PayloadError> {
        self.binary_blob()?.to_vec()
    }

    pub fn acquire(&self) -> Result<Access, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and the unique-handle/error outputs are valid.
        let status =
            unsafe { sys::fdb_payload_v1_payload_acquire(self.raw.as_ptr(), &mut raw, &mut error) };
        check_status(status, error)?;
        Access::from_raw(raw)
    }

    pub fn entry_view(&self, entry_index: u32) -> Result<View, PayloadError> {
        let mut raw = ptr::null_mut();
        let mut error = ptr::null_mut();
        // SAFETY: self is live and the owned-handle/error outputs are valid.
        let status = unsafe {
            sys::fdb_payload_v1_payload_entry_view(
                self.raw.as_ptr(),
                entry_index,
                &mut raw,
                &mut error,
            )
        };
        check_status(status, error)?;
        View::from_raw(raw)
    }

    /// Blocks until every active access pin is released. A caller must not
    /// retain an `Access` on the same thread and then wait here for itself.
    pub fn invalidate(&self) -> Result<(), PayloadError> {
        let mut error = ptr::null_mut();
        // SAFETY: self is live; Core owns synchronization across retained aliases.
        let status =
            unsafe { sys::fdb_payload_v1_payload_invalidate(self.raw.as_ptr(), &mut error) };
        check_status(status, error)
    }
}

impl Clone for Payload {
    fn clone(&self) -> Self {
        // SAFETY: self owns a live atomically retainable payload.
        unsafe { sys::fdb_payload_v1_payload_retain(self.raw.as_ptr()) };
        Self { raw: self.raw }
    }
}

impl Drop for Payload {
    fn drop(&mut self) {
        // SAFETY: this releases exactly the payload reference owned by self.
        unsafe { sys::fdb_payload_v1_payload_release(self.raw.as_ptr()) };
    }
}

impl fmt::Debug for Payload {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("Payload").finish_non_exhaustive()
    }
}

unsafe impl Send for Payload {}
unsafe impl Sync for Payload {}

#[derive(Debug)]
pub struct BuildResult {
    pub payload: Payload,
    pub report: ExecutionReport,
}

impl BuildPlan {
    pub fn execute(&self, policy: BuildPolicy) -> Result<BuildResult, PayloadError> {
        self.execute_raw(policy, ptr::null())
    }

    pub fn execute_with_backing<B: Backing + ?Sized>(
        &self,
        policy: BuildPolicy,
        backing: &B,
    ) -> Result<BuildResult, PayloadError> {
        if let Some(memory) = backing.memory_backing(backing_dispatch::Key) {
            let _execution = memory
                .context
                .execution
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            return self.execute_raw(policy, &memory.raw());
        }
        let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_backing_v1_t>::uninit();
        // SAFETY: Core initializes the complete backing table.
        unsafe { sys::fdb_payload_v1_backing_init(raw.as_mut_ptr()) };
        let mut raw = unsafe { raw.assume_init() };
        // Only reserve reads this borrowed context, during the synchronous
        // execution. Subsequent release and retain use the owning token.
        raw.context = ptr::from_ref(&backing).cast_mut().cast();
        raw.reserve = Some(backing_reserve::<B>);
        raw.commit = Some(backing_commit);
        raw.rollback = Some(backing_rollback);
        raw.retain = Some(retain_owner::<BackingToken>);
        raw.release = Some(release_owner::<BackingToken>);
        self.execute_raw(policy, &raw)
    }

    fn execute_raw(
        &self,
        policy: BuildPolicy,
        backing: *const sys::fdb_payload_v1_backing_v1_t,
    ) -> Result<BuildResult, PayloadError> {
        let mut payload = ptr::null_mut();
        let mut report = initialized_report();
        let mut error = ptr::null_mut();
        // SAFETY: self and optional backing stay live for the synchronous call.
        let status = unsafe {
            sys::fdb_payload_v1_plan_execute(
                self.as_raw(),
                policy as u32,
                backing,
                &mut payload,
                &mut report,
                &mut error,
            )
        };
        check_status(status, error)?;
        let payload = Payload::from_raw(payload)?;
        let report = ExecutionReport::from_raw(&report)?;
        Ok(BuildResult { payload, report })
    }
}

fn initialized_report() -> sys::fdb_payload_v1_execution_report_t {
    let mut raw = std::mem::MaybeUninit::<sys::fdb_payload_v1_execution_report_t>::uninit();
    // SAFETY: Core initializes the complete V1 report.
    unsafe { sys::fdb_payload_v1_execution_report_init(raw.as_mut_ptr()) };
    // SAFETY: the initializer wrote the complete current structure.
    unsafe { raw.assume_init() }
}

fn slice_pointer(bytes: &[u8]) -> *const u8 {
    if bytes.is_empty() {
        ptr::null()
    } else {
        bytes.as_ptr()
    }
}

fn unknown_report_value(field: &'static str, value: u32) -> PayloadError {
    PayloadError::binding(
        sys::FDB_PAYLOAD_E_UNSUPPORTED_ABI,
        "UNSUPPORTED_ABI",
        format!("/execution_report/{field}"),
        "FastDB Core returned an unknown execution report value",
        format!(r#"{{"actual":{value},"reason":"unknown_report_value"}}"#),
    )
}
