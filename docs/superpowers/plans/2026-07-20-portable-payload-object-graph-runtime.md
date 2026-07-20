# FastDB Portable Payload Object-Graph Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete FastDB P3 with one Core-owned ordinary `object_graph.v1` runtime for every V1 value kind, per-component identity pools, roots, refs, sharing and cycles, exact direct/staged backing behavior, hardened open, checked views, reachable-closure materialization, additive ABI-105, and truthful D1 closure without changing frozen P2 record meaning.

**Architecture:** A single source-topology derivation feeds both manifest facts and the profile-aware `RuntimeSchema`. The frozen record pipeline remains intact while `GraphAuthoring`, `GraphLayout`, `GraphEncoder`, graph open, graph cursors, and graph materialization plug into the same `PayloadBuilder -> BuildPlan -> PayloadOwner -> View` lifecycle, backing, access barrier, and C ABI. Graph declaration handles are temporary authoring tokens only; immutable plans and wire bytes contain dense per-component object coordinates assigned from declaration order.

**Tech Stack:** C++17 Core, C11 stable ABI, header-only C++17 RAII facade, CMake/CTest, fixed-width little-endian `fastdb.payload.bin.v1`, existing pinned JSON/JCS/SHA/numeric/text primitives, libFuzzer corpus runner, AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer where available, Emscripten/Node, Python/uv and TypeScript/WASM regression gates.

## Global Constraints

- The accepted [P3 object-graph design](../specs/2026-07-20-portable-payload-object-graph-runtime-design.md), [foundation design](../specs/2026-07-16-portable-payload-foundation-design.md), [ADR-0001](../../decisions/0001-portable-payload-core-authority.md), [Issue 0001](../../issues/0001-portable-payload-deferred-capabilities.md), [Issue 0002](../../issues/0002-portable-payload-foundation-implementation-status.md), frozen [P2 plan](2026-07-17-portable-payload-record-runtime.md), and normative [binary contract](../../../schemas/fastdb.payload.bin.v1.md) govern this work in that order for the P3 delta.
- Work from `socu/portable-payload-foundation` at or after design commit `f0aff719e81640e37ae8140cf31f0b87e759814d`, whose parent `74b50faa2013315f2db8a8dcb18dbf5551e2bb48` is the frozen P2 implementation baseline. Read every later commit; never reset, replay, or reimplement P2.
- C++ Core remains the sole parser, topology, layout, binary, graph, reachability, direct/staged, open, view, materialization, invalidation, error, and capability authority. The stable C ABI remains the only cross-language boundary.
- P3 completes `object_graph.v1` inside the existing portable-payload runtime. It does not call or extend the legacy `ObjectEngine`, create a second graph owner, or reinterpret the parent design's engine-family shorthand as implementation delegation.
- Entry-context component occurrences are identity roots. Component-field component occurrences remain inline by-value AoS records. Only `ref` introduces sharing or cycles.
- Business identity remains an ordinary declared field. Wire object IDs are dense, payload-generation-local `(component_index, object_id)` coordinates and are never promoted into a public identity domain or resource class.
- Temporary builder handles are opaque non-zero `uint64_t` authoring tokens. They are builder-local, invalid after successful freeze/release, absent from arena artifacts, plan facts, wire bytes, manifests, digests, generated artifacts, and stable error details, and never reused as wire object IDs.
- Per-component declaration order assigns wire object IDs `0..n-1`. Fill order, pointer values, hash iteration, traversal order, allocation order, and handle token values cannot affect bytes.
- Freeze rejects the first unfilled object and then the first unreachable declared object in stable component/declaration order. It never silently prunes declarations. Unused schema component declarations remain legal.
- Every successful complete graph freeze produces an exact immutable plan and direct status `ELIGIBLE`. Direct means no complete encoded image exists outside final backing; a hidden full byte vector is forbidden.
- `ALLOW_STAGING` may stage only after the final backing declines direct reservation. `REQUIRE_DIRECT` then returns `DIRECT_UNAVAILABLE/backing_declined_direct`; a direct write or commit failure rolls back and never retries through staging.
- P2 `record.v1` bytes, all 99 existing exported symbols, error meanings, validation-work counts, backing callbacks, `PayloadOwner`, access barrier, checked views, materialization, and invalidation remain frozen. Every shared-helper extraction keeps exact record characterization tests active.
- Binary major/minor remains `1.0`; profile 2 adds only region kind `OBJECT_VALUES = 8`. Header, region descriptor, and entry descriptor sizes remain 128, 56, and 40 bytes. Profile 1 remains byte-for-byte unchanged.
- Root/ref slots are schema-typed little-endian `uint64_t` object IDs controlled by existing validity bits. No second physical roots or references table is created.
- P3 adds exactly six exports and freezes exactly 105 sorted unique `fdb_payload_v1_*` symbols. Count-only checks are forbidden.
- Existing builder-options and plan-info V1 prefixes remain 88 and 104 bytes. New V2 struct revisions are 96 and 112 bytes; old callers remain accepted without any read/write beyond their declared prefix.
- No native recursion is permitted in source topology, declaration teardown, reachability, graph layout, graph encoding, graph open, graph views, closure materialization, or malformed-input validation.
- Every graph object/count/sum/product/offset/queue/marker/work/allocation boundary is checked before access or growth. Default builder `max_graph_objects` is 10,000,000, matching the existing open default.
- Until Task 8 completes, all existing public graph builder/open paths remain explicitly unavailable and graph manifests/capabilities retain their `not_evaluated` runtime claim. Tasks 1-7 exercise Core-private interfaces only; no intermediate commit exposes a partial public graph runtime.
- P4 Rust/Python/official TypeScript/WASM projections and four-target Core codegen, P5 legacy clean cut/release readiness, and the C-Two composition proof are separate later plans. They remain mandatory in the active overall goal but are not implementation work in this P3 plan.
- FastDB remains generic. Do not add CRM, route, relay, transport, lease, policy, Toodle, GIS, raw-file, object-storage, or Kubernetes semantics.
- Existing language suites are regression gates only in P3. Do not add a Rust-only, Python-only, or TypeScript-only graph parser/runtime/materializer.
- Every task follows strict RED -> GREEN -> focused gates -> relevant broad gates -> scoped local commit -> fresh read-only review -> same-reviewer re-review. Fix every Critical/Important and every correctness, portability, lifetime, determinism, ABI, format, or coverage Minor.
- Record intentional limitations in `docs/issues/` with current limit, reason, impact, owner, dependencies, and executable closure criteria. Do not defer any ordinary P3 semantic to shorten this plan.
- Do not bump a package version, push, tag, publish, claim hosted CI success, or claim release 0.2.0 without separate user authorization.

---

## Starting Point and Preflight

- [ ] Require `git status --short --branch` to show `socu/portable-payload-foundation` with a clean tracked worktree and index. Preserve unrelated user changes if the tree is not clean; stop only when they overlap a P3 file and cannot be safely separated.
- [ ] Require `git merge-base --is-ancestor 74b50faa2013315f2db8a8dcb18dbf5551e2bb48 HEAD` and `git merge-base --is-ancestor f0aff719e81640e37ae8140cf31f0b87e759814d HEAD` to succeed. Read `git log --oneline f0aff71..HEAD` and every intervening diff before trusting paths in this plan.
- [ ] Read `AGENTS.md`, both accepted designs, ADR-0001, Issues 0001/0002/0003, both prior implementation plans, `schemas/fastdb.payload.bin.v1.md`, `.superpowers/sdd/progress.md`, the retained P2 reports, current Core headers/sources, public ABI headers, CMake targets, ordered goldens, robustness corpus, and workflow definitions.
- [ ] Confirm the checked-in allowlist contains exactly 99 non-empty unique lines and that the six approved P3 symbols do not already occur:

  ```bash
  test "$(awk 'NF {n++} END {print n+0}' tests/abi/fastdb_payload_v1_symbols.txt)" = 99
  test "$(LC_ALL=C sort -u tests/abi/fastdb_payload_v1_symbols.txt | awk 'NF {n++} END {print n+0}')" = 99
  rg -n 'fdb_payload_v1_(builder_object_declare|builder_object_fill_begin|builder_value_object|builder_value_ref|view_ref_target|view_graph_identity)' \
    tests/abi/fastdb_payload_v1_symbols.txt && exit 1 || true
  ```

  Expected: both counts are 99 and the approved names have no collision.

- [ ] Run a clean native P2 baseline from a new build directory:

  ```bash
  cmake -S fastcarto -B build/p3-baseline \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-baseline --parallel
  ctest --test-dir build/p3-baseline --output-on-failure
  python3 tools/check_payload_abi_symbols.py --build-dir build/p3-baseline
  python3 tools/check_payload_binary_corpus.py --check
  ```

  Expected at `f0aff71`: 30/30 CTest cases, exact ABI-99, and the reviewed 10-seed P2 corpus pass. If later authorized commits change a count without changing meaning, record the actual clean baseline in the Task 1 brief rather than editing history.

- [ ] Run the unchanged consumer/package baseline and record exact counts and any already-owned Issue 0003/toolchain diagnostics:

  ```bash
  uv run pytest tests/python -q
  uv run python -m compileall -q python/fastdb4py tests/python
  uv build
  bash ts/build-wasm.sh
  npm --prefix ts/fastdb4ts run build
  npm run test:ts
  ```

  Expected at `f0aff71`: Python 413/413 and TypeScript/WASM 76/76, plus successful compileall/sdist/wheel. These suites do not yet prove a public graph projection.

- [ ] Confirm Issue 0002 records P2 frozen at ABI-99, P3-P5 open, hosted results pending, and no authorized push/version/tag/publication. Correct stale status text before production work if current evidence disagrees.

## Slice Boundary

### Delivered by this plan

- One shared source/runtime topology derivation consumed by manifest facts and profile-aware runtime schema.
- Builder-local object declaration/fill handles, root/ref authoring, deterministic dense IDs, forward refs, self/mutual cycles, and deterministic unreachable rejection.
- Exact profile-2 object regions, typed root/ref ID slots, canonical region/occurrence order, complete V1 values/lists/pools/nullability, and annotated ordered graph goldens.
- Exact immutable graph plans with repeatable direct/staged execution through existing heap/external backing and the complete D1 no-full-image proof.
- Hardened graph copy/external open with exact inventory, ID, partition, reachability, resource, work, overflow, and deterministic malformed-input failures.
- Profile-aware offset-only cursors, explicit ref dereference and graph identity, checked object/component/list/scalar/span views, and unchanged drain-before-release invalidation.
- Transactional reachable-closure materialization preserving sharing/cycles into detached, source-independent, densely remapped graph state.
- Manifest/capability truth, two compatible struct tails, exactly six public functions, exact ABI-105, and a thin C++17 graph facade.
- Native Debug/Release, ASan+UBSan, available TSan, binary corpus/fuzz, pure-C, C++ facade, wasm32/Node, package/language regression, docs, and independent-review evidence.
- Formal D1 correction/closure after executable evidence, while D2-D5 and hosted/release facts remain truthful.

### Explicitly not claimed by this plan

- No Rust, Python, or official TypeScript/WASM portable graph API and no payload codegen. P4 owns all four projections and all four generated artifact targets.
- No public call-db removal, `fastdb.schema.v1` removal, `ColumnEngine -> RecordEngine` clean rename, package version 0.2.0, or release readiness. P5 owns those changes.
- No C-Two outer contract parsing, artifact composition, CRM lifecycle, or downstream Rust/Python proof. That owner slice begins only after FastDB P3-P5 freeze.
- No segmented/multipart final backing, streaming/mutable graph builder, extra guaranteed platforms, native Node projection, or Go projection; D2-D5 remain in Issue 0001.
- No cross-payload object ID stability, global identity, implicit default root, orphan pruning, arbitrary graph merge, raw pointer escape, or binding-owned wire interpretation.

## File and Dependency Map

```text
spec::ResolvedSpec
    -> spec::RuntimeTopology                    # one source/storage-role derivation
    -> spec::Manifest + layout::RuntimeSchema
    -> build::ValueArena + build::GraphAuthoring
    -> layout::GraphLayout
    -> build::GraphEncoder -> build::ByteSink
    -> build::BuildPlan profile variant
    -> view::OpenCommon + view::GraphOpen
    -> view::PayloadIndex graph cursor variants
    -> view::PayloadOwner + AccessBarrier
    -> view::GraphView + view::GraphMaterialize
    -> stable C ABI + header-only C++ RAII
```

New focused modules:

| Path | Single responsibility |
|---|---|
| `fastcarto/fastdb/src/payload/spec/RuntimeTopology.{hpp,cpp}` | Stable source occurrence order, context/storage role, reachability, identity-component and runtime-pool facts from one `ResolvedSpec` |
| `fastcarto/fastdb/src/payload/build/GraphAuthoring.{hpp,cpp}` | Builder-local unique handle registry, per-component declaration pools, object fill state, and iterative freeze reachability |
| `fastcarto/fastdb/src/payload/layout/LayoutFacts.hpp` | Small shared immutable descriptor/list facts extracted without changing record traversal |
| `fastcarto/fastdb/src/payload/build/ByteSink.hpp` | Existing bounded monotonic sink interface moved out of the record-named header |
| `fastcarto/fastdb/src/payload/layout/GraphLayout.{hpp,cpp}` | Exact profile-2 region/object/list/pool plan and expected validation-work facts from frozen logical graph |
| `fastcarto/fastdb/src/payload/build/GraphEncoder.{hpp,cpp}` | The one profile-2 writer from immutable `GraphLayout`/logical graph to `ByteSink` |
| `fastcarto/fastdb/src/payload/view/OpenCommon.{hpp,cpp}` | Narrow shared bounded header/directory/container checks; no profile semantics or public index publication |
| `fastcarto/fastdb/src/payload/view/GraphOpen.{hpp,cpp}` | Exact graph inventory, object/ref/value/partition/reachability validation and graph index facts |
| `fastcarto/fastdb/src/payload/view/GraphView.cpp` | Identity-object/ref cursor navigation, explicit dereference, and identity observation over the existing owner/barrier |
| `fastcarto/fastdb/src/payload/view/GraphMaterialize.cpp` | Transactional reachable-closure copy and dense per-component remapping for backed or detached graph views |

Existing files keep their ownership:

| Path | P3 responsibility |
|---|---|
| `layout/RuntimeSchema.*` | Consume shared topology, retain P2 IDs/layouts, add storage roles and identity pool metadata |
| `build/ValueArena.*` | Add artifact-safe object coordinates, object-record/root/ref tags, and immutable object pools |
| `build/PayloadBuilder.*` | Route existing typed expectation operations and graph authoring into one builder state machine |
| `layout/RecordLayout.*`, `build/RecordEncoder.*` | Preserve frozen profile-1 traversal and bytes; only narrow helper extraction is allowed |
| `build/BuildPlan.*` | Own one profile-layout variant and dispatch one shared backing/publication path |
| `view/Open.*` | Keep record open and add exact profile dispatch into `GraphOpen` |
| `view/View.*`, `view/Materialize.*` | Preserve record behavior and dispatch graph-specific cursor/closure work |
| `abi/fastdb_payload.cpp`, `include/fastdb_payload.h` | Enforce struct-prefix guards, graph public gate, six exports, error/output rules, ABI-105 |
| `include/fastdb_payload.hpp` | C-ABI-only `ObjectHandle`, graph identity, builder and view convenience |

No P3 module is created in Rust, Python, TypeScript, C-Two, Toodle, legacy call-db, `ColumnEngine`, or `ObjectEngine` paths.

## Frozen P3 Internal Interfaces

### Shared runtime topology

Task 1 produces this Core-internal contract; vector order is the existing all-source runtime type-ID order:

```cpp
namespace fastdb::payload::spec {

enum class ValueContext : std::uint8_t {
    entry_value,
    component_field,
};

enum class StorageRole : std::uint8_t {
    ordinary_value,
    list_descriptor,
    object_root_id,
    inline_component,
    reference_id,
};

struct RuntimeTypeTopology final {
    const TypeNode* source;
    ValueContext context;
    StorageRole storage_role;
    bool reachable;
};

struct RuntimeTopology final {
    std::vector<RuntimeTypeTopology> types;
    std::vector<std::uint8_t> reachable_components;
    std::vector<std::uint8_t> identity_components;
    std::uint32_t reachable_type_count;
    std::uint32_t reachable_component_count;
    std::uint32_t reachable_list_type_count;
    bool has_utf8;
    bool has_utf16le;
    bool has_bytes;
    bool has_list_items;
    bool has_objects;
    bool has_references;
    bool has_roots;
};

error::Result<RuntimeTopology> derive_runtime_topology(
    const ResolvedSpec& resolved);

}  // namespace fastdb::payload::spec
```

`types[i]` is runtime type ID `i`; `UINT32_MAX` remains reserved. Entering a list preserves context. Entering component-definition fields switches to `component_field`. Record entry components remain `inline_component`; graph entry components become `object_root_id`. A graph `ref` is always `reference_id` and makes its target identity-bearing. This derivation is iterative, visits every source node exactly once for ID order, and follows component/ref edges with visited sets only for reachability.

### Logical graph and builder handles

Task 2 extends the internal arena without storing temporary handles:

```cpp
namespace fastdb::payload::build {

using ObjectHandle = std::uint64_t;
inline constexpr ObjectHandle invalid_object_handle = UINT64_C(0);

struct ObjectCoordinate final {
    std::uint32_t component_index;
    std::uint64_t object_id;
};

enum class ValueTag : std::uint8_t {
    null_value, boolean, u8, u16, u32, i32, u8n, u16n,
    f32, f64, str, wstr, bytes, component, list, sequence,
    object_record, object_root, reference,
};

struct ValueNode final {
    std::uint32_t runtime_type_id;
    ValueTag tag;
    std::uint8_t reserved8[3];
    std::uint64_t scalar_bits_or_offset;
    std::uint64_t byte_length;
    NodeIndex first_child;
    NodeIndex next_sibling;
    std::uint64_t child_count;
    std::uint32_t object_component_index;
    std::uint32_t reserved32;
    std::uint64_t object_id;
};

class LogicalPayload final {
public:
    const std::vector<std::vector<NodeIndex>>& object_pools() const noexcept;
    std::uint64_t graph_object_count() const noexcept;
};

class PayloadBuilder final {
public:
    error::Result<ObjectHandle> declare_object(std::uint32_t component_index);
    error::Result<void> begin_object_fill(ObjectHandle object);
    error::Result<void> push_object(ObjectHandle object);
    error::Result<void> push_ref(ObjectHandle object);
};

}  // namespace fastdb::payload::build
```

An `object_record` has `runtime_type_id == UINT32_MAX`, its real component and dense declaration index in the coordinate fields, and component fields as children. `object_root` and `reference` retain their source occurrence runtime type ID plus the resolved coordinate. Every other tag uses the coordinate sentinel `UINT32_MAX/UINT64_MAX`. `LogicalPayload::object_pools()[component_index][object_id]` gives the object-record node; pools for non-identity components are empty.

Each declaration counts as one logical value node, one graph object, and 64 deterministic builder bytes. Existing entry-root, node, frame, text, wide-text, and opaque charges remain unchanged. Handle-registry allocator capacity is not a second public budget; `max_graph_objects`, `max_value_nodes`, native container capacity, and `max_total_builder_bytes` are checked before declaration mutation.

Handle tokens come from one non-zero process-wide atomic sequence with no wrap/reuse. The builder stores `token -> ObjectCoordinate`; supplying a token to another builder cannot alias one of its declarations. Exhausted handle space returns `BUILDER_RESOURCE_LIMIT` with resource `object_handles` and no token in details.

### Immutable graph layout and encoder

Tasks 3-5 produce:

```cpp
namespace fastdb::payload::layout {

struct ObjectAggregate final {
    std::uint32_t component_index;
    std::uint32_t region_index;
    std::vector<build::NodeIndex> object_nodes;
};

class GraphLayout final {
public:
    static error::Result<GraphLayout> plan(
        const RuntimeSchema& runtime_schema,
        const build::LogicalPayload& values);
    std::uint64_t total_length() const noexcept;
    std::uint64_t root_value_count() const noexcept;
    std::uint64_t graph_object_count() const noexcept;
    std::uint64_t validation_work() const noexcept;
    std::uint32_t region_count() const noexcept;
    const std::vector<RegionDescriptor>& regions() const noexcept;
    const std::vector<EntryDescriptor>& entries() const noexcept;
    const std::vector<ObjectAggregate>& object_aggregates() const noexcept;
    const std::vector<ListAggregate>& list_aggregates() const noexcept;
    const RuntimeSchema& runtime_schema() const noexcept;
};

}  // namespace fastdb::payload::layout

namespace fastdb::payload::build {

error::Result<void> encode_graph(
    const layout::GraphLayout& layout,
    const LogicalPayload& values,
    ByteSink& sink);

using ProfileLayout = std::variant<layout::RecordLayout,
                                   layout::GraphLayout>;

struct PlanInfo final {
    std::uint64_t total_bytes;
    std::uint64_t region_count;
    std::uint64_t logical_value_count;
    std::uint64_t list_element_count;
    std::uint64_t text_bytes;
    std::uint64_t opaque_bytes;
    std::uint64_t validation_work;
    std::uint32_t max_alignment;
    std::uint32_t direct_build_status;
    std::uint64_t graph_object_count;
};

}  // namespace fastdb::payload::build
```

`GraphLayout` never mutates IDs or prunes objects. Region order is entries, identity-bearing components, list nodes, then UTF-8/UTF-16LE/bytes pools. Physical occurrence order is entries first, then component pools/component IDs; object-root/ref slots never recursively pull their targets into pool/list aggregation.

### Graph index, cursors, views, and detached state

Tasks 4, 6, and 7 replace the single internal value cursor with explicit variants while preserving record behavior:

```cpp
namespace fastdb::payload::view {

struct InlineValueCursor final {
    std::uint32_t runtime_type_id;
    spec::TypeKind kind;
    std::uint64_t slot_offset;
    bool present;
};

struct IdentityObjectCursor final {
    std::uint32_t component_index;
    std::uint64_t object_id;
    bool present;
};

struct RefCursor final {
    std::uint32_t runtime_type_id;
    std::uint32_t target_component_index;
    std::uint64_t object_id;
    bool present;
};

using ValueCursor = std::variant<InlineValueCursor,
                                 IdentityObjectCursor,
                                 RefCursor>;

struct ObjectPoolMetadata final {
    std::uint32_t component_index;
    std::uint32_t region_index;
    std::uint64_t data_offset;
    std::uint64_t object_count;
    std::uint32_t stride;
    std::uint32_t alignment;
};

struct GraphIdentity final {
    std::uint32_t component_index;
    std::uint64_t object_id;
};

class PayloadIndex final {
public:
    error::Result<IdentityObjectCursor> ref_target(RefCursor cursor) const;
    error::Result<GraphIdentity> graph_identity(ValueCursor cursor) const;
};

class View final {
public:
    error::Result<View> ref_target() const;
    error::Result<GraphIdentity> graph_identity() const;
};

struct DetachedViewState final {
    build::ValueArena arena;
    std::vector<std::vector<build::NodeIndex>> object_pools;
    std::shared_ptr<const layout::RuntimeSchema> runtime_schema;
    build::NodeIndex root;
};

}  // namespace fastdb::payload::view
```

Identity-object cursors never synthesize a source runtime type ID. `ref_target` is explicit; `field` does not dereference. Backed graph operations take the same checked pin and captured generation as P2. Detached root/ref/object/list/component semantics use the same view API and contain no source pointer.

### Additive C ABI

Task 8 adds exactly the constants, typedef, struct revisions, and six functions accepted in the design:

```c
#define FDB_PAYLOAD_REGION_OBJECT_VALUES UINT32_C(8)
#define FDB_PAYLOAD_V1_INVALID_OBJECT_HANDLE UINT64_C(0)
#define FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE UINT32_C(2014)
#define FDB_PAYLOAD_E_UNREACHABLE_OBJECT UINT32_C(2015)

#define FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE UINT32_C(88)
#define FDB_PAYLOAD_V1_BUILDER_OPTIONS_V2_SIZE UINT32_C(96)
#define FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE UINT32_C(104)
#define FDB_PAYLOAD_V1_PLAN_INFO_V2_SIZE UINT32_C(112)

typedef uint64_t fdb_payload_v1_object_handle_t;

fdb_payload_v1_status_t fdb_payload_v1_builder_object_declare(
    fdb_payload_v1_builder_t*, uint32_t,
    fdb_payload_v1_object_handle_t*, fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_object_fill_begin(
    fdb_payload_v1_builder_t*, fdb_payload_v1_object_handle_t,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_object(
    fdb_payload_v1_builder_t*, fdb_payload_v1_object_handle_t,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_builder_value_ref(
    fdb_payload_v1_builder_t*, fdb_payload_v1_object_handle_t,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_ref_target(
    const fdb_payload_v1_view_t*, fdb_payload_v1_view_t**,
    fdb_payload_v1_error_t**);
fdb_payload_v1_status_t fdb_payload_v1_view_graph_identity(
    const fdb_payload_v1_view_t*, uint32_t*, uint64_t*,
    fdb_payload_v1_error_t**);
```

The unchanged initializer functions write V2 sizes. New Core accepts V1 and V2 prefixes, reads/writes a tail only when fully covered, ignores larger unknown tails, and never changes the existing V1 constants. The C++ facade adds private-construction `ObjectHandle`, `GraphIdentity`, four builder methods, and two view methods only.

## Error, Ordering, and Resource Contract

| Phase | First-failure order |
|---|---|
| Builder operation | validate builder/profile/active scope, expected kind, handle membership/component, limits/capacity, then mutate |
| Freeze | incomplete active scope, missing entries, unfilled objects by component/declaration, unreachable objects by component/declaration |
| Open | options/span/header/version/profile/total/digest/reserved, directory bounds/counts, exact inventory, descriptors/data/null-zero, root/ref IDs, partitions/text, reachability/all-reached |

| Condition | Stable result |
|---|---|
| zero/forged/stale/foreign handle | `2014 INVALID_OBJECT_HANDLE`; details omit the token |
| first unreachable authored declaration | `2015 UNREACHABLE_OBJECT` at `/objects/<component-id>/<declaration-index>` |
| wrong object component or wrong root/ref operation | `2004 TYPE_MISMATCH` |
| duplicate fill or conflicting active scope | `2006 BUILDER_STATE` |
| unfilled object/field | `2002 MISSING_FIELD` |
| builder graph limit | `2013 BUILDER_RESOURCE_LIMIT`, resource `graph_objects` |
| open root/ref ID outside target pool | `3007 INVALID_REFERENCE` |
| wrong object inventory or unreachable wire object | `3009 NON_CANONICAL_BINARY` |
| open graph-object/work limit | `3008 RESOURCE_LIMIT` |
| null graph identity/ref target | `2003 UNEXPECTED_NULL` |
| graph identity on inline/non-identity or dereference non-ref | `2004 TYPE_MISMATCH` |

Graph reachability is iterative. Freeze starts from entry-side object roots and refs; open uses the same logical roots/edges over validated slots. Every first-reached object is traversed once and the final completeness scan checks every object marker. `max_nesting_depth` applies to lists/by-value containment, not graph-edge length; graph-edge growth is bounded by graph objects and validation work.

## Accepted-Spec Coverage Map

| Accepted design section | Owning task(s) | Closure evidence |
|---|---|---|
| Sections 4-5: authority, topology, storage roles | Task 1 | shared topology tests, manifest/RuntimeSchema agreement, record characterization |
| Sections 6-8: logical graph, handles, authoring, reachability | Tasks 2 and 5 | builder-state/error tests, deterministic freeze, immutable plan/publication proof |
| Section 9: normative binary profile | Tasks 3 and 4 | normative Markdown, layout/encoder/open agreement, annotated valid and invalid goldens |
| Section 10: planning, backing, exact direct | Task 5 | plan facts, range-write backing, allocation-threshold proof, direct/staged reports |
| Section 11: hardened open | Task 4 | exact inventory/order/resource/work failures and deterministic receipts |
| Section 12: checked views and lifetime | Task 6 | cursor-kind, identity/ref, span, generation, drain-before-release tests |
| Section 13: reachable-closure materialization | Task 7 | sharing/cycle preservation, dense remap, rollback, source-independence tests |
| Sections 14-16: manifest, ABI, facade | Task 8 | capability transition, V1/V2 canaries, exact ABI-105, pure-C/C++ tests |
| Section 17: error/resource table | Tasks 1-9 by owner | stable code/path/detail tests plus aggregate class map |
| Sections 18.1-18.4: validation matrix | Tasks 3-9 | goldens, allocation injection, sanitizer/wasm runs, 16-seed corpus, quality map |
| Section 19: documentation and D1 closure | Task 10 | proof map, issue/status corrections, current full-gate report |
| Sections 20-21: non-goals and acceptance | Global constraints and final checklist | clean P3 freeze with P4/P5/C-Two and release actions still open |

---

## Task 1: Derive one profile-aware runtime topology and storage-role schema

**Files:**

- Create: `fastcarto/fastdb/src/payload/spec/RuntimeTopology.hpp`
- Create: `fastcarto/fastdb/src/payload/spec/RuntimeTopology.cpp`
- Modify: `fastcarto/fastdb/src/payload/spec/Manifest.cpp`
- Modify: `fastcarto/fastdb/src/payload/layout/RuntimeSchema.hpp`
- Modify: `fastcarto/fastdb/src/payload/layout/RuntimeSchema.cpp`
- Modify: `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp`
- Create: `tests/cpp/payload/test_graph_runtime_schema.cpp`
- Modify: `tests/cpp/payload/test_record_layout.cpp`
- Modify: `tests/cpp/payload/test_compiled_spec.cpp`
- Modify: `tests/cpp/payload/test_runtime_abi.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `tests/golden/payload/v1/spec/valid/object-graph.manifest.hex`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: frozen `ResolvedSpec`, stable indexes/type traversal, current record `RuntimeSchema`, and manifest runtime facts.
- Produces: exact `RuntimeTopology`, storage role per runtime type ID, identity-component inventory, profile-aware slot layouts, one derivation used by both manifest and runtime schema, and an explicit temporary public-ABI graph gate. Public graph runtime remains unavailable.

- [ ] Write `.superpowers/sdd/p3-task-1-brief.md` with starting commit, the exact files above, the shared-topology interface, RED command, record regression gate, public-unavailable non-goal, Issue 0002 obligation, and commit subject `feat(core): derive portable graph runtime topology`.
- [ ] Add RED tests covering all of the following named shapes:

  - an entry `component`, `list<component>`, `ref`, and `list<ref>` becoming roots/reference slots under graph profile;
  - the same component type inside a component field and nested component-field list remaining inline by value;
  - one component used both as an identity pool elsewhere and inline in a field without changing field layout;
  - a ref-only target becoming identity-bearing;
  - a by-value-only component not receiving an object pool;
  - an unused source component retaining all-source runtime IDs but no reachable layout/pool;
  - shared refs, self/mutual ref cycles, and a 20,000-component ref cycle terminating iteratively;
  - record profile preserving every existing runtime ID, reachable component, slot stride/alignment, list inventory, and manifest byte.

- [ ] Configure and run the focused RED target:

  ```bash
  cmake -S fastcarto -B build/p3-task1 \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-task1 \
    --target fastdb_payload_test_graph_runtime_schema --parallel
  ctest --test-dir build/p3-task1 \
    -R '^payload\.graph_runtime_schema$' --output-on-failure
  ```

  Expected RED: `RuntimeTopology`, graph storage roles, and the target do not exist.

- [ ] Implement `derive_runtime_topology` with one stable all-source occurrence pass and a separate iterative reachability pass. Use explicit work items carrying `ValueContext`; do not recursively expand component definitions during ID assignment and do not let an unordered container define output order.
- [ ] Make `RuntimeSchema::compile` consume `RuntimeTopology::types` directly. Add `storage_role(runtime_type_id)`, `component_identity_bearing(component_index)`, and identity-component iteration. Keep existing record `require_record_runtime` behavior available to record-only callers.
- [ ] Because Core-private schema compilation is now graph-aware before the public runtime is complete, add an explicit temporary `object_graph.v1` rejection in `fdb_payload_v1_builder_create` using the existing stable graph-unavailable result. Pin `builder_create`, `payload_open_copy`, and `payload_open_external` as unavailable in `test_runtime_abi.cpp`; Tasks 2-7 may not weaken these public assertions.
- [ ] Replace `Manifest.cpp`'s independent reachable-component/object/root/ref derivation with `RuntimeTopology`. Keep object-graph runtime status `not_evaluated`, operations `compile,query`, direct reason `runtime_slice_not_implemented`, and codegen targets empty; only already-reported topology/pool facts may become more accurate.
- [ ] Run `payload.graph_runtime_schema`, `payload.record_layout`, `payload.compiled_spec`, every manifest golden, the complete native suite (the 30 frozen baseline cases plus the new graph target), and ASan+UBSan. Require exact record manifest/binary behavior and no recursive teardown failure; record the actual total instead of hard-coding future cumulative task counts.
- [ ] Update Issue 0002 with the shared topology/storage-role seam and state that graph authoring, binary, plan, open, views, materialization, ABI, and public capability remain absent.
- [ ] Run `git diff --check`, inspect only intended files, and commit:

  ```bash
  git add fastcarto/fastdb/src/payload/spec/RuntimeTopology.* \
    fastcarto/fastdb/src/payload/spec/Manifest.cpp \
    fastcarto/fastdb/src/payload/layout/RuntimeSchema.* \
    fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp \
    tests/cpp/payload/test_graph_runtime_schema.cpp \
    tests/cpp/payload/test_record_layout.cpp \
    tests/cpp/payload/test_compiled_spec.cpp \
    tests/cpp/payload/test_runtime_abi.cpp tests/cpp/CMakeLists.txt \
    tests/golden/payload/v1/spec/valid/object-graph.manifest.hex \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): derive portable graph runtime topology"
  ```

## Task 2: Add builder-local object declarations, fills, roots, refs, and freeze reachability

**Files:**

- Create: `fastcarto/fastdb/src/payload/build/GraphAuthoring.hpp`
- Create: `fastcarto/fastdb/src/payload/build/GraphAuthoring.cpp`
- Modify: `fastcarto/fastdb/src/payload/build/ValueArena.hpp`
- Modify: `fastcarto/fastdb/src/payload/build/ValueArena.cpp`
- Modify: `fastcarto/fastdb/src/payload/build/PayloadBuilder.hpp`
- Modify: `fastcarto/fastdb/src/payload/build/PayloadBuilder.cpp`
- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Modify: `fastcarto/fastdb/src/payload/error/Error.cpp`
- Create: `tests/cpp/payload/test_graph_builder.cpp`
- Modify: `tests/cpp/payload/test_payload_builder.cpp`
- Modify: `tests/cpp/payload/test_runtime_abi.cpp`
- Modify: `tests/cpp/payload/test_error.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: Task 1 runtime storage roles and the existing expectation-stack/arena transaction rules.
- Produces: internal `ObjectHandle`, object record/root/ref arena nodes, per-component declaration pools, exactly-once fills, iterative deterministic freeze reachability, codes 2014/2015, and a complete immutable logical graph. Public C ABI graph operations remain gated unavailable.

- [ ] Freeze `.superpowers/sdd/p3-task-2-brief.md` with `feat(core): add portable graph value authoring`, allowed files, exact internal signatures, RED command, ABI-unavailable regression, limits, error ordering, and no binary/plan/public-operation non-goals.
- [ ] Add RED builder tests for:

  - declare -> root -> fill, declarations interleaved across components, and dense per-component coordinates;
  - declare A/B before fill, forward refs, self cycle, mutual cycle, shared target, and multiple explicitly rooted disconnected graphs;
  - the same declarations filled in different orders yielding identical logical coordinates/arena trees;
  - nullable root/ref versus present object zero;
  - entry-side nested lists of object roots/refs and component-field inline component/list behavior;
  - wrong component, non-identity component, zero/forged/stale/foreign-builder handle, duplicate fill, nested conflicting scope, missing fields, unfilled object, missing entry, and orphan declaration;
  - concurrent builders never receiving the same token, plus a test-only allocator seam at the final non-zero token proving terminal exhaustion returns resource `object_handles` without wrap, reuse, mutation, or token disclosure;
  - exact failure order and `/objects/<component-id>/<declaration-index>` paths without token values;
  - graph objects counted against 10,000,000 default, explicit `max_graph_objects`, `max_value_nodes`, native capacity, and 64-byte logical charge before mutation;
  - injected allocation failure during object node, pool, handle registry, fill frame, refs, reachability markers/queue, and freeze publication with unchanged retryable builder state;
  - a 50,000-object chain/cycle and teardown without native recursion;
  - public `builder_create`, `payload_open_*`, manifest, and capabilities still reporting graph runtime unavailable because Task 8 has not exposed it.

- [ ] Run RED:

  ```bash
  cmake -S fastcarto -B build/p3-task2 \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-task2 \
    --target fastdb_payload_test_graph_builder --parallel
  ctest --test-dir build/p3-task2 \
    -R '^payload\.graph_builder$' --output-on-failure
  ```

  Expected RED: graph authoring types/methods and stable errors are absent.

- [ ] Extend `ValueNode` and `LogicalPayload` exactly as frozen above. Set coordinate sentinels on every record node and retain source occurrence runtime IDs only for root/ref leaves. Keep the existing 64-byte logical-node budget and count every declaration once.
- [ ] Implement `GraphAuthoring` with a non-zero global atomic token allocator, builder-owned token map, per-component node pools, filled markers, and immediate token-to-coordinate resolution. Never copy a token into `ValueNode`, `LogicalPayload`, an error, or a report.
- [ ] Extend the expectation stack with an object-record parent. `begin_object_fill` opens the target component's field sequence and auto-closes after the final field; declarations and another entry/fill are rejected while any scope is active.
- [ ] Implement `push_object` only for `object_root_id` expectations and `push_ref` only for `reference_id`. Validate target component equality before reserving/publishing a node. Existing `begin_component` remains inline-only.
- [ ] Implement freeze checks and iterative closure exactly in design Sections 7.5/8.2. A failed logical freeze preserves the complete builder. A successful freeze moves pools and coordinates into `LogicalPayload`, destroys the token map, and seals the builder.
- [ ] Add header constants `2014/2015` and error symbols only. Do not add the six functions, handle typedef, region kind, or struct tails yet. Preserve exact existing C header layouts and ABI-99.
- [ ] Run focused GREEN, record builder regression, runtime unavailable ABI regression, error table, Debug, ASan+UBSan, and available TSan for independent builders. Require exact ABI-99.
- [ ] Update Issue 0002 with the Core-private logical graph authoring seam and all still-open binary/plan/open/view/materialization/public gaps.
- [ ] Run `git diff --check` and commit:

  ```bash
  git add fastcarto/fastdb/src/payload/build/GraphAuthoring.* \
    fastcarto/fastdb/src/payload/build/ValueArena.* \
    fastcarto/fastdb/src/payload/build/PayloadBuilder.* \
    fastcarto/fastdb/include/fastdb_payload.h \
    fastcarto/fastdb/src/payload/error/Error.cpp \
    tests/cpp/payload/test_graph_builder.cpp \
    tests/cpp/payload/test_payload_builder.cpp \
    tests/cpp/payload/test_runtime_abi.cpp tests/cpp/payload/test_error.cpp \
    tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): add portable graph value authoring"
  ```

## Task 3: Freeze profile-2 wire layout with first annotated graph goldens

**Files:**

- Modify: `schemas/fastdb.payload.bin.v1.md`
- Modify: `schemas/README.md`
- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Modify: `fastcarto/fastdb/src/payload/layout/BinaryFormat.hpp`
- Create: `fastcarto/fastdb/src/payload/layout/LayoutFacts.hpp`
- Modify: `fastcarto/fastdb/src/payload/layout/RecordLayout.hpp`
- Modify: `fastcarto/fastdb/src/payload/layout/RecordLayout.cpp`
- Create: `fastcarto/fastdb/src/payload/layout/GraphLayout.hpp`
- Create: `fastcarto/fastdb/src/payload/layout/GraphLayout.cpp`
- Create: `fastcarto/fastdb/src/payload/build/ByteSink.hpp`
- Modify: `fastcarto/fastdb/src/payload/build/RecordEncoder.hpp`
- Modify: `fastcarto/fastdb/src/payload/build/RecordEncoder.cpp`
- Create: `fastcarto/fastdb/src/payload/build/GraphEncoder.hpp`
- Create: `fastcarto/fastdb/src/payload/build/GraphEncoder.cpp`
- Create: `fastcarto/fastdb/src/payload/view/OpenCommon.hpp`
- Create: `fastcarto/fastdb/src/payload/view/OpenCommon.cpp`
- Create: `fastcarto/fastdb/src/payload/view/GraphOpen.hpp`
- Create: `fastcarto/fastdb/src/payload/view/GraphOpen.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/Open.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/Open.cpp`
- Modify: `tests/cpp/payload/GoldenCorpus.hpp`
- Modify: `tests/cpp/payload/GoldenCorpus.cpp`
- Create: `tests/cpp/payload/test_graph_binary.cpp`
- Modify: `tests/cpp/payload/test_record_binary.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `tests/golden/payload/v1/binary/index.json`
- Create: `tests/golden/payload/v1/binary/spec/graph-empty.source.json`
- Create: `tests/golden/payload/v1/binary/spec/graph-single-root.source.json`
- Create: `tests/golden/payload/v1/binary/spec/graph-shared-cycle.source.json`
- Create: `tests/golden/payload/v1/binary/valid/graph-empty.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/graph-empty.sha256`
- Create: `tests/golden/payload/v1/binary/valid/graph-empty.layout.md`
- Create: `tests/golden/payload/v1/binary/valid/graph-single-root.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/graph-single-root.sha256`
- Create: `tests/golden/payload/v1/binary/valid/graph-single-root.layout.md`
- Create: `tests/golden/payload/v1/binary/valid/graph-shared-cycle.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/graph-shared-cycle.sha256`
- Create: `tests/golden/payload/v1/binary/valid/graph-shared-cycle.layout.md`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: immutable logical graph, profile-aware runtime schema, existing checked math/wire primitives, record slot semantics, and bounded sink.
- Produces: complete normative profile-2 byte contract, `OBJECT_VALUES = 8`, shared layout fact types/sink interface, initial exact `GraphLayout`/`GraphEncoder`, initial strict graph open for fixed graph values, and three hand-audited graph goldens. No public plan/open path is enabled.

- [ ] Freeze `.superpowers/sdd/p3-task-3-brief.md` with commit subject `feat(core): freeze portable graph binary profile`, exact normative tables, first three scenarios, record-byte regression hashes, public-unavailable non-goal, and one atomic wire-doc/encoder/open/golden deliverable.
- [ ] Add RED tests for:

  - profile 2 in the unchanged 128-byte header and root-value count retaining its P2 meaning;
  - region kind 8 with exact owner component, `UINT32_MAX` runtime type sentinel, count/stride/alignment/length, including required zero-count identity pools;
  - 8-byte root/ref slots, object ID zero present versus null, typed target component, and no physical root/reference table;
  - exact entry -> object pool -> list -> variable-pool directory order and shared zero-length boundaries;
  - object records using stable component field order, immediate validity, inline nested component layout, zero padding, and no object-level validity bitmap;
  - declaration order affecting IDs while fill order does not affect bytes;
  - repeated encode/open and annotated hex/SHA receipt agreement for empty, one root, shared ref plus self cycle;
  - every existing record golden retaining identical bytes, SHA, error, and validation work after helper extraction.

- [ ] Run RED:

  ```bash
  cmake -S fastcarto -B build/p3-task3 \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-task3 \
    --target fastdb_payload_test_graph_binary --parallel
  ctest --test-dir build/p3-task3 \
    -R '^payload\.graph_binary$' --output-on-failure
  ```

  Expected RED: profile-2 binary rules, graph layout/encoder/open, and graph fixtures do not exist.

- [ ] Extend `fastdb.payload.bin.v1.md` with the complete design Section 9 contract in this commit: profile value, region kind, every descriptor field, root/ref slots, inventory/order, occurrence traversal, IDs, validity/zero/padding, all V1 slot rules, overflow, resource work, open ordering, errors, and closed forward compatibility. Replace the record-only header profile rule with an exact profile-dispatched rule without weakening profile 1.
- [ ] Move only `DescriptorFact`/`ListAggregate` to `LayoutFacts.hpp` and `ByteSink` to `ByteSink.hpp`. Run record characterization immediately; do not generalize or rewrite `RecordLayout`/`RecordEncoder` traversal.
- [ ] Add `RegionKind::object_values` and its exact region rule. Keep the existing numeric values 1-7 and every profile-1 `region_rule` result unchanged.
- [ ] Implement `GraphLayout::plan` initially for empty/fixed scalar/object/component/root/ref/list-free graph shapes. Build exact directories and object aggregates from frozen object pools; never discover IDs or prune during layout.
- [ ] Implement `GraphEncoder` header/directories/object/root/ref/fixed slots in strictly ascending offsets with bounded local buffers. It writes each object record in component/object-ID order and never follows a root/ref target while aggregating physical data.
- [ ] Extract only bounded container header/directory validation into `OpenCommon`. Preserve exact record validation order, errors, paths, details, and work. Implement `open_graph` for the same initial fixed graph slice and publish no index until all supported checks succeed.
- [ ] Extend the ordered golden index so every graph success explicitly names source, scenario, hex, independent SHA-256, and `.layout.md` receipt. Each receipt lists header fields, directory indexes, aligned offsets, object pool coordinates, root/ref slot bytes, padding spans, and manual SHA command; tests do not generate expected bytes.
- [ ] Hand-review all three hex files against the normative tables, compute SHA independently from decoded hex, then require Core output to match. Do not use production encoder output to rewrite expectations during the test.
- [ ] Add the public region macro only; public builder/open remains unavailable and symbol allowlist remains exactly 99.
- [ ] Update Issue 0002: normative graph bytes and the initial Core-private fixed graph slice exist, while lists/variable pools, complete hardened open, plans/backing, views, materialization, ABI, and D1 evidence remain open.
- [ ] Run graph binary, every record layout/binary/open test, ordered golden validation, Debug, Release, ASan+UBSan, schema packaging/link checks, Markdown relative links, and exact ABI-99.
- [ ] Commit the normative document, encoder/open slice, and goldens atomically:

  ```bash
  git add schemas/fastdb.payload.bin.v1.md schemas/README.md \
    fastcarto/fastdb/include/fastdb_payload.h \
    fastcarto/fastdb/src/payload/layout/BinaryFormat.hpp \
    fastcarto/fastdb/src/payload/layout/LayoutFacts.hpp \
    fastcarto/fastdb/src/payload/layout/RecordLayout.* \
    fastcarto/fastdb/src/payload/layout/GraphLayout.* \
    fastcarto/fastdb/src/payload/build/ByteSink.hpp \
    fastcarto/fastdb/src/payload/build/RecordEncoder.* \
    fastcarto/fastdb/src/payload/build/GraphEncoder.* \
    fastcarto/fastdb/src/payload/view/OpenCommon.* \
    fastcarto/fastdb/src/payload/view/GraphOpen.* \
    fastcarto/fastdb/src/payload/view/Open.* \
    tests/cpp/payload/GoldenCorpus.* \
    tests/cpp/payload/test_graph_binary.cpp \
    tests/cpp/payload/test_record_binary.cpp tests/cpp/CMakeLists.txt \
    tests/golden/payload/v1/binary/index.json \
    tests/golden/payload/v1/binary/spec/graph-{empty,single-root,shared-cycle}.source.json \
    tests/golden/payload/v1/binary/valid/graph-{empty,single-root,shared-cycle}.{bin.hex,sha256,layout.md} \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): freeze portable graph binary profile"
  ```

## Task 4: Complete graph layout, encoding, hardened open, and all V1 values

**Files:**

- Modify: `fastcarto/fastdb/src/payload/layout/GraphLayout.hpp`
- Modify: `fastcarto/fastdb/src/payload/layout/GraphLayout.cpp`
- Modify: `fastcarto/fastdb/src/payload/build/GraphEncoder.hpp`
- Modify: `fastcarto/fastdb/src/payload/build/GraphEncoder.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/Open.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/Open.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/GraphOpen.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/GraphOpen.cpp`
- Modify: `tests/cpp/payload/GoldenCorpus.hpp`
- Modify: `tests/cpp/payload/GoldenCorpus.cpp`
- Modify: `tests/cpp/payload/test_graph_binary.cpp`
- Modify: `tests/cpp/payload/test_record_binary.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `tests/golden/payload/v1/binary/index.json`
- Create: `tests/golden/payload/v1/binary/spec/graph-all-values.source.json`
- Create: `tests/golden/payload/v1/binary/spec/graph-nested-lists.source.json`
- Create: `tests/golden/payload/v1/binary/spec/graph-disconnected-roots.source.json`
- Create: `tests/golden/payload/v1/binary/spec/graph-null-empty.source.json`
- Create: `tests/golden/payload/v1/binary/valid/graph-all-values.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/graph-all-values.sha256`
- Create: `tests/golden/payload/v1/binary/valid/graph-all-values.layout.md`
- Create: `tests/golden/payload/v1/binary/valid/graph-nested-lists.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/graph-nested-lists.sha256`
- Create: `tests/golden/payload/v1/binary/valid/graph-nested-lists.layout.md`
- Create: `tests/golden/payload/v1/binary/valid/graph-disconnected-roots.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/graph-disconnected-roots.sha256`
- Create: `tests/golden/payload/v1/binary/valid/graph-disconnected-roots.layout.md`
- Create: `tests/golden/payload/v1/binary/valid/graph-null-empty.bin.hex`
- Create: `tests/golden/payload/v1/binary/valid/graph-null-empty.sha256`
- Create: `tests/golden/payload/v1/binary/valid/graph-null-empty.layout.md`
- Create: `tests/golden/payload/v1/binary/invalid/graph-object-owner.bin.hex`
- Create: `tests/golden/payload/v1/binary/invalid/graph-object-owner.error.json`
- Create: `tests/golden/payload/v1/binary/invalid/graph-object-stride.bin.hex`
- Create: `tests/golden/payload/v1/binary/invalid/graph-object-stride.error.json`
- Create: `tests/golden/payload/v1/binary/invalid/graph-root-id.bin.hex`
- Create: `tests/golden/payload/v1/binary/invalid/graph-root-id.error.json`
- Create: `tests/golden/payload/v1/binary/invalid/graph-ref-id.bin.hex`
- Create: `tests/golden/payload/v1/binary/invalid/graph-ref-id.error.json`
- Create: `tests/golden/payload/v1/binary/invalid/graph-unreachable.bin.hex`
- Create: `tests/golden/payload/v1/binary/invalid/graph-unreachable.error.json`
- Create: `tests/golden/payload/v1/binary/invalid/graph-null-slot.bin.hex`
- Create: `tests/golden/payload/v1/binary/invalid/graph-null-slot.error.json`
- Create: `tests/golden/payload/v1/binary/invalid/graph-region-order.bin.hex`
- Create: `tests/golden/payload/v1/binary/invalid/graph-region-order.error.json`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: Task 3 exact binary contract and initial graph pipeline.
- Produces: complete profile-2 layout/encoding/open for all scalars, normalized values, `str`, `wstr`, `bytes`, recursive lists, inline components, roots, refs, nullability, sharing, cycles, complete graph index metadata, exact resource work, and deterministic malformed failures. Public graph runtime remains gated.

- [ ] Freeze `.superpowers/sdd/p3-task-4-brief.md` with commit subject `feat(core): complete portable graph binary runtime`, the complete semantic/malformed matrix, exact open order, resource accounting, record regressions, and no public owner/view/ABI claim.
- [ ] Add RED valid cases covering every V1 kind by exact native name (`bool,u8,u16,u32,i32,u8n,u16n,f32,f64,str,wstr,bytes,component,ref,list`), one/many, nullable present/null, empty values, nested lists, lists under entries/objects/inline components, shared variable values without deduplication, forward/shared/self/mutual refs, object zero, multiple pools, and explicit disconnected roots.
- [ ] Add RED deterministic byte cases proving one physical occurrence order: entries/value indexes, component indexes, object IDs, stable fields/list items; root/ref targets are never traversed in place. Repeated builds and fill-order variants match; intentionally changed declaration order produces the correspondingly changed documented IDs/bytes.
- [ ] Add RED malformed cases for every graph-changed field class:

  - wrong profile and graph-only region under record profile;
  - missing/duplicate/reordered/unknown/extra object regions;
  - wrong object owner, type sentinel, count, stride, alignment, byte length, offset, flags, or reserved bytes;
  - non-zero object/inter-region/final/null-slot/validity-tail padding;
  - root/ref ID equal to or above target count and present ID into empty target pool;
  - wrong root/ref target component implied by schema;
  - list partition, UTF-8, UTF-16LE, normalized numeric, Boolean/NaN corruption inside object records;
  - structurally valid but unreachable object;
  - `max_graph_objects`, graph sum/product/native capacity, nesting, string, list, and validation-work boundaries;
  - repeat open returning byte-identical stable diagnostics.

- [ ] Run focused RED against the new scenario/test names:

  ```bash
  cmake -S fastcarto -B build/p3-task4 \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-task4 \
    --target fastdb_payload_test_graph_binary --parallel
  ctest --test-dir build/p3-task4 \
    -R '^payload\.graph_binary$' --output-on-failure
  ```

  Expected RED: Task 3 rejects or cannot build the variable/list/malformed cases because the complete graph traversal/open logic is absent.
- [ ] Complete `GraphLayout` list aggregates, descriptor facts, variable-pool partitions, object/list/null work facts, and checked exact length. Reuse P2 quantization/text helpers; do not introduce a graph-specific numeric or string codec.
- [ ] Complete `GraphEncoder` for inline components, recursive lists, variable descriptors/pools, validity, canonical NaNs/normalized codes, and all padding. Keep writes monotonic and bounded; object/ref slots emit coordinates only.
- [ ] Extend `PayloadIndex` with object-pool metadata and the explicit `InlineValueCursor`, `IdentityObjectCursor`, and `RefCursor` types. Record open emits only inline cursors. Graph open validates all bytes before publishing object/list/pool cursor facts.
- [ ] Implement graph open in the exact seven-stage order. Apply `max_graph_objects` before marker/queue allocation; structurally inspect every object, then perform iterative reachability and fail the first unmarked object in component/object order.
- [ ] Charge structural work exactly as P2 plus the reachability revisit and final object-marker scan. Assert `GraphLayout::validation_work()` equals eager Core-built open; lazy text differences retain the existing P2 meaning.
- [ ] Add the four complete graph golden scenarios and selected checked-in invalid receipts. Each valid graph case has a human-audited layout receipt; each invalid case pins status, symbol, path, message class, and JCS details.
- [ ] Run focused graph binary/open tests, every record binary/open/view regression, allocation-failure sweeps for layout/open markers/queues/index publication, 50,000-object iterative cycles, Debug, Release, ASan+UBSan, and exact ABI-99.
- [ ] Update Issue 0002 with complete Core-private binary/open facts and remaining plan/backing/view/materialization/public ABI/D1 gaps.
- [ ] Run JSON duplicate-key parsing for all new fixtures, relative-link checks, `git diff --check`, and commit:

  ```bash
  git add fastcarto/fastdb/src/payload/layout/GraphLayout.* \
    fastcarto/fastdb/src/payload/build/GraphEncoder.* \
    fastcarto/fastdb/src/payload/view/Open.* \
    fastcarto/fastdb/src/payload/view/GraphOpen.* \
    tests/cpp/payload/GoldenCorpus.* \
    tests/cpp/payload/test_graph_binary.cpp \
    tests/cpp/payload/test_record_binary.cpp tests/cpp/CMakeLists.txt \
    tests/golden/payload/v1/binary/index.json \
    tests/golden/payload/v1/binary/spec/graph-{all-values,nested-lists,disconnected-roots,null-empty}.source.json \
    tests/golden/payload/v1/binary/valid/graph-{all-values,nested-lists,disconnected-roots,null-empty}.{bin.hex,sha256,layout.md} \
    tests/golden/payload/v1/binary/invalid/graph-{object-owner,object-stride,root-id,ref-id,unreachable,null-slot,region-order}.{bin.hex,error.json} \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): complete portable graph binary runtime"
  ```

## Task 5: Integrate graph BuildPlan, final backing, publication, and real direct proof

**Files:**

- Modify: `fastcarto/fastdb/src/payload/build/BuildPlan.hpp`
- Modify: `fastcarto/fastdb/src/payload/build/BuildPlan.cpp`
- Modify: `fastcarto/fastdb/src/payload/build/PayloadBuilder.hpp`
- Modify: `fastcarto/fastdb/src/payload/build/PayloadBuilder.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/Open.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/Open.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/PayloadOwner.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/PayloadOwner.cpp`
- Modify: `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp`
- Modify: `fastcarto/fastdb/src/payload/backing/HeapBacking.hpp`
- Modify: `fastcarto/fastdb/src/payload/backing/HeapBacking.cpp`
- Modify: `fastcarto/fastdb/CMakeLists.txt`
- Create: `tests/cpp/payload/BackingTestSupport.hpp`
- Modify: `tests/cpp/payload/test_payload_backing.cpp`
- Create: `tests/cpp/payload/test_graph_backing.cpp`
- Modify: `tests/cpp/payload/test_payload_open.cpp`
- Modify: `tests/cpp/payload/test_runtime_abi.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: complete immutable graph layout/encoder/open, existing backing callbacks/state machine, and immutable owner publication.
- Produces: `ProfileLayout` BuildPlan, graph plan facts including object count, repeatable heap/external direct/staged execution, graph owner publication through the one reader, and all eight D1 proof facts at Core-private level. Public C ABI graph creation/open remains gated until Task 8.

- [ ] Freeze `.superpowers/sdd/p3-task-5-brief.md` with `feat(core): execute portable graph build plans`, exact profile variant, `PlanInfo` tail fact, backing transition table, D1 proof matrix, no C ABI exposure, and all P2 callback/lifetime regressions.
- [ ] Extract the existing fake backing recorder into `BackingTestSupport.hpp` without changing any assertion, then add RED graph cases for:

  - immutable plan facts, `graph_object_count`, exact total/regions/work, source mutation independence, repeat and concurrent distinct-context execution;
  - Core heap, stable writable span, and range-write-only final backing;
  - every graph plan direct-eligible with reason `graph_layout_exact_after_freeze` at the later manifest seam;
  - direct reserve decline -> staged only under `ALLOW_STAGING`, exact report `STAGED/BACKING_DECLINED_DIRECT/staging_bytes=total_bytes`;
  - direct reserve decline -> `REQUIRE_DIRECT` failure with no staged reserve;
  - direct write/commit failure -> exactly one rollback and no staging retry;
  - direct/staged/Core heap byte identity for cycles, sharing, lists, `str`, `wstr`, and bytes;
  - committed-image validation through graph open before owner publication and release-not-rollback on post-commit failure;
  - exact report `DIRECT/NONE/staging_bytes=0`, one reserve, monotonic non-overlapping writes, and complete final-length coverage.

- [ ] Add the no-full-image executable proof using a large variable-width graph and a range-write-only backing. After plan creation, enable a test allocation guard that rejects any single allocation at least half `total_bytes` while allowing bounded index/queue metadata. Require direct execution to succeed; a deliberate test-only full-image allocation of `total_bytes` must fail under the same guard.
- [ ] Add a Core-private heap-backing reserve observer compiled only under a `BUILD_TESTING`-scoped private definition in `fastcarto/fastdb/CMakeLists.txt`. It is declared only in the internal header, adds no public header or stable ABI symbol and no production branch, and records reserve mode/count for the D1 test. Require zero heap/staged reserve calls during the direct proof.
- [ ] Run focused RED:

  ```bash
  cmake -S fastcarto -B build/p3-task5 \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-task5 \
    --target fastdb_payload_test_graph_backing \
      fastdb_payload_test_payload_open \
      fastdb_payload_test_runtime_abi --parallel
  ctest --test-dir build/p3-task5 \
    -R '^payload\.(graph_backing|payload_open|runtime_abi)$' \
    --output-on-failure
  ```

  Expected RED: `BuildPlan` stores only `RecordLayout`, graph `freeze_plan`/publication fails, and D1 direct assertions cannot pass; the public-unavailable ABI assertions remain GREEN.
- [ ] Replace the record-only member with `ProfileLayout`. `BuildPlan::create` compiles one runtime schema and selects exactly one layout by `CompiledSpec.profile()`; `execute` visits the layout to choose the single encoder and the exact reader/publication options.
- [ ] Extend internal `PlanInfo` with graph object count. For record plans it is zero and every existing plan fact remains exact. All graph plans report direct eligible.
- [ ] Add `open_payload` profile dispatch while keeping `open_record`/`open_graph` focused validators. Remove `PayloadOwner`'s record-only internal restriction so Core-private graph execution can publish through the one owner. Preserve explicit graph-unavailable guards in public `fdb_payload_v1_builder_create`, `fdb_payload_v1_payload_open_copy`, and `fdb_payload_v1_payload_open_external`; extend `test_runtime_abi.cpp` so the internal dispatcher change cannot expose a partial public graph path before Task 8.
- [ ] Preserve the shared backing state machine unchanged. Staged graph execution uses the same heap sink plus one copy; direct uses only the final sink. Do not introduce graph callbacks, fixup callbacks, reserve growth, or a second owner.
- [ ] Run focused graph backing/open, complete record backing/open, concurrent execution, allocation failures after reserve/commit/publication, Debug, Release, ASan+UBSan, available TSan, and exact ABI-99.
- [ ] Perform the required source audit: `GraphEncoder` contains no complete-image byte buffer; `BuildPlan` cannot route a direct graph plan through heap staging; reports are derived from actual reservation path. Record file/line evidence in `.superpowers/sdd/p3-task-5-report.md`.
- [ ] Update Issue 0002 with Core-private direct proof facts but keep D1 open because public ABI, views/materialization, robustness, and independent final evidence are incomplete.
- [ ] Run `git diff --check` and commit:

  ```bash
  git add fastcarto/fastdb/src/payload/build/BuildPlan.* \
    fastcarto/fastdb/src/payload/build/PayloadBuilder.* \
    fastcarto/fastdb/src/payload/view/Open.* \
    fastcarto/fastdb/src/payload/view/PayloadOwner.* \
    fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp \
    fastcarto/fastdb/src/payload/backing/HeapBacking.* \
    fastcarto/fastdb/CMakeLists.txt \
    tests/cpp/payload/BackingTestSupport.hpp \
    tests/cpp/payload/test_payload_backing.cpp \
    tests/cpp/payload/test_graph_backing.cpp \
    tests/cpp/payload/test_payload_open.cpp \
    tests/cpp/payload/test_runtime_abi.cpp tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): execute portable graph build plans"
  ```

## Task 6: Add checked identity-object and ref views on the existing lifetime barrier

**Files:**

- Modify: `fastcarto/fastdb/src/payload/view/Open.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/Open.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/GraphOpen.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/GraphOpen.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/View.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/View.cpp`
- Create: `fastcarto/fastdb/src/payload/view/GraphView.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/PayloadOwner.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/PayloadOwner.cpp`
- Create: `tests/cpp/payload/test_graph_view.cpp`
- Modify: `tests/cpp/payload/test_checked_view.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: validated graph index/object-pool facts, one owner, one access barrier, and the exact cursor variants.
- Produces: checked graph entry/object/ref/list/component/scalar/span navigation, explicit `ref_target`, scoped `graph_identity`, generation capture, concurrent read-only behavior, and unchanged drain-before-release invalidation. Materialization and public graph ABI remain unavailable.

- [ ] Freeze `.superpowers/sdd/p3-task-6-brief.md` with `feat(core): add checked portable graph views`, exact cursor/interface signatures, view error table, lifetime matrix, record regressions, and no materialization/public ABI claim.
- [ ] Add RED backed-view tests for:

  - entry sequence -> identity root; root/list item -> identity object; object fields -> inline values or refs;
  - identity object kind/component/field count/field traversal without a synthetic runtime type ID;
  - inline component kind/fields but no graph identity;
  - ref kind, explicit target, shared target identities, self/mutual cycles, and no implicit dereference through `field`;
  - `graph_identity` on identity object and ref returning exact coordinates; object zero; IDs scoped to one payload;
  - null applicable view -> `UNEXPECTED_NULL`; inline/other identity and non-ref dereference -> `TYPE_MISMATCH`;
  - nested lists of roots/refs, all scalar bits, `str`/`wstr`/bytes access, null/empty, normalized values, and by-value components inside objects;
  - every child retaining owner/generation, concurrent independent view handles, invalidation blocking new pins, waiting for active pins, advancing generation, and releasing backing exactly once;
  - record cursor/view results, errors, access spans, and invalidation remaining exact.

- [ ] Run RED:

  ```bash
  cmake -S fastcarto -B build/p3-task6 \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-task6 \
    --target fastdb_payload_test_graph_view --parallel
  ctest --test-dir build/p3-task6 \
    -R '^payload\.graph_view$' --output-on-failure
  ```

  Expected RED: current `ValueCursor` cannot represent identity objects/refs and `View` has no ref/identity operations.

- [ ] Install the explicit cursor variant in `PayloadIndex`. Update every record method to construct/require `InlineValueCursor`; do not encode cursor role in impossible runtime IDs or overloaded offsets.
- [ ] Add checked object-pool coordinate-to-offset arithmetic and component field traversal. Every backed call obtains a short pin before reading owner bytes and releases it before publishing the child view.
- [ ] Implement `GraphView.cpp` for identity/ref-specific methods and narrow dispatch hooks in `View.cpp`. `field` accepts inline or identity components only; `ref_target` alone dereferences.
- [ ] Keep the same `PayloadOwnerState`, `AccessBarrier`, generation, and `Access` types. Do not add graph locks, ref-counted object nodes, cached backing pointers, or an unpinned raw span.
- [ ] Run graph/record checked view suites, active-pin drain tests, 32-thread read-only graph traversal, injected child-view/access allocation failures, Debug, Release, ASan+UBSan, and focused TSan.
- [ ] Update Issue 0002 with Core-private graph view/lifetime facts and remaining closure/materialization/public ABI/robustness gaps.
- [ ] Run `git diff --check` and commit:

  ```bash
  git add fastcarto/fastdb/src/payload/view/Open.* \
    fastcarto/fastdb/src/payload/view/GraphOpen.* \
    fastcarto/fastdb/src/payload/view/View.* \
    fastcarto/fastdb/src/payload/view/GraphView.cpp \
    fastcarto/fastdb/src/payload/view/PayloadOwner.* \
    tests/cpp/payload/test_graph_view.cpp \
    tests/cpp/payload/test_checked_view.cpp tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): add checked portable graph views"
  ```

## Task 7: Materialize selected graph views as independent reachable closures

**Files:**

- Modify: `fastcarto/fastdb/src/payload/build/ValueArena.hpp`
- Modify: `fastcarto/fastdb/src/payload/build/ValueArena.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/View.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/View.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/GraphView.cpp`
- Modify: `fastcarto/fastdb/src/payload/view/Materialize.hpp`
- Modify: `fastcarto/fastdb/src/payload/view/Materialize.cpp`
- Create: `fastcarto/fastdb/src/payload/view/GraphMaterialize.cpp`
- Create: `tests/cpp/payload/test_graph_materialize.cpp`
- Modify: `tests/cpp/payload/test_checked_view.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: backed/detached cursor variants, checked access pins, runtime schema, graph object pools, and flat value arena.
- Produces: transactional source-independent detached graph state, selected-view reachable closure, sharing/cycle preservation, deterministic dense per-component remap, repeated detached materialization, and survival after source invalidation/release.

- [ ] Freeze `.superpowers/sdd/p3-task-7-brief.md` with `feat(core): materialize portable graph closures`, exact closure/remap algorithm, allocation/lifetime failure matrix, record materialization regression, and public ABI still gated.
- [ ] Add RED materialization tests for:

  - an entry sequence containing multiple roots and its union closure;
  - one identity-object subview excluding unrelated rooted objects;
  - a ref subview remaining a ref root and including target closure;
  - an inline component subview including objects reached through its refs;
  - shared refs remaining shared, self/mutual cycles remaining cycles, and disconnected non-selected graphs omitted;
  - ascending source IDs remapped to dense target IDs independently per component;
  - target IDs permitted to differ while values/identity equality inside target remain correct;
  - all scalar/list/text/wide/bytes/null/empty/by-value data copied exactly;
  - source invalidation/release during and after materialization, one full source pin, no backing dependency in published detached state;
  - materializing an already-detached graph creating a second independent graph;
  - allocation failure at discovery map, queue, per-component sort/remap, node/byte/object-pool copy, ref rewrite, and final publication with no partial view/leak;
  - unchanged P2 record subtree materialization and metrics.

- [ ] Run RED:

  ```bash
  cmake -S fastcarto -B build/p3-task7 \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-task7 \
    --target fastdb_payload_test_graph_materialize --parallel
  ctest --test-dir build/p3-task7 \
    -R '^payload\.graph_materialize$' --output-on-failure
  ```

  Expected RED: detached state has no object pools and current materialization cannot preserve/refollow graph identity.

- [ ] Extend `DetachedViewState` with per-component object pools. Keep record detached pools empty and preserve every existing record node/path/metric behavior.
- [ ] Implement `GraphMaterialize.cpp` as one transaction under one source pin:

  ```text
  discover selected root tree and referenced (component, source_id) pairs
  visit each discovered object once with an explicit queue
  sort reachable source IDs ascending inside each component
  assign target IDs 0..n-1
  copy root tree and object records into temporary detached arena/pools
  rewrite every object-root/ref coordinate through the remap
  validate counts/coordinates, then publish one immutable detached state
  ```

- [ ] Use checked native-size/count arithmetic before every map/vector growth. Do not publish a detached `View` until all copied nodes, byte spans, pools, and rewrites validate.
- [ ] Preserve root cursor kind: identity object remains identity object, ref remains ref, list/component/scalar remains its selected kind. Do not convert a ref subview into its target object.
- [ ] Run graph closure, graph view/invalidation, record materialization, deep/wide iterative closure, allocation-failure balance, Debug, Release, ASan+UBSan, and available TSan.
- [ ] Update Issue 0002: ordinary graph Core semantics are internally complete, but public ABI/capability, wasm/fuzz/ABI-105, final review, and D1 formal closure remain open.
- [ ] Run `git diff --check` and commit:

  ```bash
  git add fastcarto/fastdb/src/payload/build/ValueArena.* \
    fastcarto/fastdb/src/payload/view/View.* \
    fastcarto/fastdb/src/payload/view/GraphView.cpp \
    fastcarto/fastdb/src/payload/view/Materialize.* \
    fastcarto/fastdb/src/payload/view/GraphMaterialize.cpp \
    tests/cpp/payload/test_graph_materialize.cpp \
    tests/cpp/payload/test_checked_view.cpp tests/cpp/CMakeLists.txt \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): materialize portable graph closures"
  ```

## Task 8: Expose complete graph runtime through additive ABI-105 and thin C++ facade

**Files:**

- Modify: `fastcarto/fastdb/include/fastdb_payload.h`
- Modify: `fastcarto/fastdb/include/fastdb_payload.hpp`
- Modify: `fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp`
- Modify: `fastcarto/fastdb/src/payload/spec/Manifest.cpp`
- Modify: `schemas/fastdb.payload.manifest.v1.schema.json`
- Modify: `fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc`
- Modify: `tests/abi/fastdb_payload_v1_symbols.txt`
- Modify: `tests/cpp/payload/test_c_header_smoke.c`
- Modify: `tests/cpp/payload/test_runtime_abi.cpp`
- Modify: `tests/cpp/payload/test_runtime_cpp_facade.cpp`
- Modify: `tests/cpp/payload/test_compiled_spec.cpp`
- Modify: `tests/cpp/payload/test_error.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `tests/golden/payload/v1/spec/valid/object-graph.manifest.hex`
- Modify: `tests/wasm/payload_runtime_harness.cpp`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: complete reviewed Core graph authoring/plan/open/view/materialization/lifetime path.
- Produces: two struct-tail revisions, six exact C exports, public graph build/open/view/materialize/invalidate, exact manifest/capability truth, thin C++ `ObjectHandle`/`GraphIdentity`, pure-C layout/source compatibility, and exact ABI-105. P4 language projections remain absent.

- [ ] Freeze `.superpowers/sdd/p3-task-8-brief.md` with `feat(core): expose portable graph runtime ABI`, the exact header excerpt in this plan, V1/V2 canary matrix, output clearing/error rules, manifest transition, public behavior matrix, ABI-105 allowlist, and no language binding/codegen work.
- [ ] Add RED pure-C/runtime ABI tests for:

  - exact constants, typedef width, `sizeof` 96/112, old V1 constant values 88/104, and initializers writing V2 sizes;
  - old 88-byte builder options using default graph limit without an out-of-bounds read, new 96-byte tail zero/default/custom behavior, partial 89-95-byte tail ignored, and larger canary tail preserved;
  - old 104-byte plan info receiving the complete old prefix only, new 112-byte graph count, partial 105-111-byte tail untouched, larger canary preserved;
  - reserved/flags validation inside covered V1 prefixes and no validation/read of unknown larger tails;
  - every new function's null handle/output/error-sink, output clearing, allocation failure, exception containment, owned error/status equality, handle validity, and no token leakage;
  - old 99 functions retaining declarations/signatures/record behavior;
  - one public C graph exercising all values, cycles/sharing, direct/staged, open copy/external, ref identity, materialization, invalidation, and balanced release.

- [ ] Add RED C++ facade tests for private `ObjectHandle` construction, move/copy value behavior, `declare_object`, `begin_object_fill`, `push_object`, `push_ref`, `ref_target`, `graph_identity`, errors, and byte/report identity with raw C calls. Ensure facade source contains no topology/layout/wire/reachability/materialization implementation.
- [ ] Add RED manifest/capability tests requiring graph runtime `available`, layout `object_pool_aos`, exact required pools from shared topology, operations `compile,query,build,open,view,materialize,invalidate`, direct `eligible/graph_layout_exact_after_freeze`, and no codegen targets.
- [ ] Run focused RED:

  ```bash
  cmake -S fastcarto -B build/p3-task8 \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-task8 \
    --target fastdb_payload_test_c_header_smoke \
      fastdb_payload_test_runtime_abi \
      fastdb_payload_test_runtime_cpp_facade \
      fastdb_payload_test_compiled_spec --parallel
  ctest --test-dir build/p3-task8 \
    -R '^payload\.(c_header_smoke|runtime_abi|runtime_cpp_facade|compiled_spec)$' \
    --output-on-failure
  ```

  Expected RED: public graph calls are absent or gated, struct sizes are old, graph manifest is `not_evaluated`, and allowlist remains 99.
- [ ] Append `max_graph_objects` and `graph_object_count` after existing `reserved[4]`, define V2 size constants, and update initializers/parsers/writers with byte-coverage guards before every tail access. Do not repurpose a reserved word or change V1 constant values.
- [ ] Implement the six ABI functions through existing guarded status/publication helpers. `object_declare` clears to zero; `ref_target` clears its handle; `graph_identity` clears both numbers; all publish only after complete Core success.
- [ ] Remove the explicit public graph-unavailable gates only after all six functions and old-prefix tests are GREEN. Existing builder/open functions then dispatch to the same complete Core graph path.
- [ ] Implement `ObjectHandle` with private raw constructor and no retain/release, plus a plain `GraphIdentity` return value in `fastdb_payload.hpp`. Every method performs one C ABI call and fixed-width conversion only.
- [ ] Change manifest schema/generator/goldens atomically with executable capability. Canonical payload JSON/digest fixtures must remain unchanged; only manifest/capability bytes may change.
- [ ] Append exactly the six approved names to the sorted allowlist and run native plus wasm symbol diff. Require 105 unique exact names; reject an extra/missing symbol even when count is 105.
- [ ] Run pure-C C11 header/link on arm64/x86_64/wasm32, raw C ABI, C++ facade, graph/record complete suites, Debug, Release, ASan+UBSan, available TSan, embedded-schema regeneration, manifest goldens, Core wasm harness, and exact native/wasm ABI-105.
- [ ] Update Issue 0002 with public P3 runtime/ABI facts while stating robustness/fuzz/full gates/final review/D1 closure and all P4-P5 work remain open.
- [ ] Run `git diff --check`, inspect generated schema diff, and commit:

  ```bash
  git add fastcarto/fastdb/include/fastdb_payload.h \
    fastcarto/fastdb/include/fastdb_payload.hpp \
    fastcarto/fastdb/src/payload/abi/fastdb_payload.cpp \
    fastcarto/fastdb/src/payload/spec/Manifest.cpp \
    fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc \
    schemas/fastdb.payload.manifest.v1.schema.json \
    tests/abi/fastdb_payload_v1_symbols.txt \
    tests/cpp/payload/test_c_header_smoke.c \
    tests/cpp/payload/test_runtime_abi.cpp \
    tests/cpp/payload/test_runtime_cpp_facade.cpp \
    tests/cpp/payload/test_compiled_spec.cpp \
    tests/cpp/payload/test_error.cpp tests/cpp/CMakeLists.txt \
    tests/golden/payload/v1/spec/valid/object-graph.manifest.hex \
    tests/wasm/payload_runtime_harness.cpp \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "feat(core): expose portable graph runtime ABI"
  ```

## Task 9: Harden graph robustness, wasm, ABI, CI, and package gates

**Files:**

- Modify: `tests/fuzz/payload/fuzz_payload_open.cpp`
- Modify: `tests/fuzz/payload/run_payload_open_corpus.cpp`
- Modify: `tests/fuzz/payload/binary-corpus.json`
- Create: `tests/fuzz/payload/binary-corpus/valid-graph-cycle.bin`
- Create: `tests/fuzz/payload/binary-corpus/valid-graph-variable.bin`
- Create: `tests/fuzz/payload/binary-corpus/valid-graph-null.bin`
- Create: `tests/fuzz/payload/binary-corpus/malformed-graph-object-region.bin`
- Create: `tests/fuzz/payload/binary-corpus/malformed-graph-reference.bin`
- Create: `tests/fuzz/payload/binary-corpus/malformed-graph-unreachable.bin`
- Modify: `tools/check_payload_binary_corpus.py`
- Modify: `tests/ci/test_check_payload_binary_corpus.py`
- Create: `tests/ci/p3_malformed_class_map.json`
- Create: `tests/ci/check_p3_runtime_quality.rb`
- Create: `tests/ci/test_check_p3_runtime_quality.rb`
- Modify: `tests/cpp/payload/test_graph_binary.cpp`
- Modify: `tests/cpp/payload/test_graph_backing.cpp`
- Modify: `tests/cpp/payload/test_graph_view.cpp`
- Modify: `tests/cpp/payload/test_graph_materialize.cpp`
- Modify: `tests/cpp/payload/test_runtime_abi.cpp`
- Modify: `tests/wasm/payload_runtime_harness.cpp`
- Modify: `tests/cpp/CMakeLists.txt`
- Modify: `.github/workflows/tests.yml`
- Modify: `tools/check_python_package_inventory.py`
- Modify: `tests/ci/test_check_python_package_inventory.py`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`

**Interfaces:**

- Consumes: complete public ABI-105 graph runtime and existing repository-owned robustness/package/workflow gates.
- Produces: one graph-aware public fuzz/open traversal, exactly 16 reviewed seeds, executable P3 malformed/proof map, Core wasm32/Node graph coverage, exact native/wasm ABI-105 CI, package inventory truth, and complete post-fix local evidence required before D1 closure.

- [ ] Freeze `.superpowers/sdd/p3-task-9-brief.md` with `test(core): harden portable graph runtime`, exact 16-seed inventory, malformed class map, fuzz/wasm/ABI/package/CI commands, local-versus-hosted wording, P2 regression, and no D1 status change before review-clean evidence.
- [ ] Add the new corpus/map/quality tests first and capture RED:

  ```bash
  python3 tests/ci/test_check_payload_binary_corpus.py
  python3 tools/check_payload_binary_corpus.py --check
  ruby tests/ci/test_check_p3_runtime_quality.rb
  ruby tests/ci/check_p3_runtime_quality.rb --check-repository
  ```

  Expected RED: the old checker/CMake contract still requires 10 seeds, the six graph seeds and class map are absent or incomplete, and the new quality checker rejects ABI-105/P3 proof expectations that are not yet wired into robustness gates.
- [ ] Extend the fuzz harness with one fixed comprehensive graph matching spec in addition to the existing record matching/mismatch specs. On successful graph open, traverse bounded sequences/lists/components/refs with a visited `(component,object)` set, compare shared identities, acquire spans, materialize, invalidate, and traverse the detached result. It must never predict acceptance or parse wire bytes.
- [ ] Add exactly six reviewed graph seeds so the tracked corpus total is 16:

  | Seed | Required class |
  |---|---|
  | `valid-graph-cycle.bin` | valid sharing/self/mutual cycle |
  | `valid-graph-variable.bin` | valid lists/str/wstr/bytes |
  | `valid-graph-null.bin` | valid null/empty/object-zero distinctions |
  | `malformed-graph-object-region.bin` | wrong object descriptor/inventory |
  | `malformed-graph-reference.bin` | out-of-range typed ref/root ID |
  | `malformed-graph-unreachable.bin` | structurally valid unreachable object |

  Each seed has a fixed SHA-256 and exact expected success or status/path in `binary-corpus.json`. The deterministic runner executes all 16 before the fuzz entrypoint.

- [ ] Extend `fuzz_payload_open` failure equality to compare status, code, symbol, path, message, and canonical details over repeated graph opens. Balance every payload/view/access/blob/error and bound graph traversal by `max_graph_objects`/work.
- [ ] Add `p3_malformed_class_map.json` mapping every design Section 18.2-18.4 class to an exact test/golden/seed: profile/inventory/descriptors, padding/null, IDs/reachability, values/lists/text, limits/work, allocation, backing direct proof, view generation/drain, materialization closure, ABI prefix/output, and wasm exception containment.
- [ ] Add an executable P3 quality checker that rejects a missing class, duplicate class owner, absent named test/fixture, ABI count other than 105, corpus count other than 16, stale graph-unavailable manifest, missing D1 proof report fields, or forbidden public ownership/type terms. Unit-test every rejection branch.
- [ ] Extend the Core wasm32/Node harness to build/open/traverse/materialize/invalidate the `graph-all-values` golden, assert exact identity/cycle behavior, exercise injected allocation failure, and run direct range-write output. Extend the public single-thread ABI harness to the six functions and exact old/new struct prefixes.
- [ ] Extend workflow native/sanitizer/Emscripten jobs with graph targets, exact ABI-105, 16-seed corpus, P3 quality checker, and source/package inventory. Keep current standard Linux x86-64/macOS arm64 runners and path-aware aggregate semantics; workflow definitions are not hosted passes.
- [ ] Update package inventory expectations for the new Core sources, normative graph binary text, manifest schema/embed, and public headers. Assert that test goldens/corpus and build/cache/runtime-fuzz debris remain outside the distribution. Do not change `MANIFEST.in`, `pyproject.toml`, or the project version because their current recursive Core/schema rules already cover the intended production files.
- [ ] Run the repository-owned deterministic corpus and this coverage-guided binary-open smoke from a new build:

  ```bash
  cmake -S fastcarto -B build/p3-task9-fuzz \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF \
    -DFASTDB_BUILD_FUZZERS=ON -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  cmake --build build/p3-task9-fuzz \
    --target fastdb_payload_test_binary_open_corpus fuzz_payload_open --parallel
  ctest --test-dir build/p3-task9-fuzz \
    -R '^payload\.(binary_open_corpus|binary_fuzz_corpus)$' \
    --output-on-failure
  cmake -E make_directory \
    build/p3-task9-fuzz/binary-runtime-corpus \
    build/p3-task9-fuzz/parser-artifacts
  ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    build/p3-task9-fuzz/tests/cpp/fuzz_payload_open \
      build/p3-task9-fuzz/binary-runtime-corpus \
      tests/fuzz/payload/binary-corpus \
      -runs=1000 -max_len=1048576 \
      -artifact_prefix=build/p3-task9-fuzz/parser-artifacts/
  test -z "$(find build/p3-task9-fuzz/parser-artifacts \
    -type f -print -quit)"
  ```

  On the currently recorded macOS toolchain, report the exact libFuzzer/LeakSanitizer availability and run all other hard-fail instrumented gates; `detect_leaks=0` is not LeakSanitizer evidence, and an unavailable or adjusted schedule is never relabeled as a default pass.
- [ ] Run the complete clean local P3 gate from new directories after the last Task 9 implementation/review fix:

  ```bash
  tools/vendor_portable_payload_deps.sh --check
  python3 tools/generate_embedded_payload_schemas.py --check
  python3 tests/ci/test_check_payload_binary_corpus.py
  python3 tools/check_payload_binary_corpus.py --check
  ruby tests/ci/check_p3_runtime_quality.rb --check-repository

  cmake -S fastcarto -B build/p3-final-debug \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/p3-final-debug --parallel
  ctest --test-dir build/p3-final-debug --output-on-failure
  python3 tools/check_payload_abi_symbols.py --build-dir build/p3-final-debug

  cmake -S fastcarto -B build/p3-final-release \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build build/p3-final-release --parallel
  ctest --test-dir build/p3-final-release --output-on-failure

  cmake -S fastcarto -B build/p3-final-sanitize \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF \
    -DFASTDB_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  cmake --build build/p3-final-sanitize --parallel
  ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ctest --test-dir build/p3-final-sanitize --output-on-failure

  emcmake cmake -S fastcarto -B build/p3-final-wasm \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build build/p3-final-wasm --parallel
  node build/p3-final-wasm/wasm-tests/payload_runtime_harness.js
  node build/p3-final-wasm/wasm-tests/payload_runtime_abi_single_thread.js \
    --single-thread-injected-failure
  python3 tools/check_payload_abi_symbols.py \
    --wasm-build-dir build/p3-final-wasm

  uv run pytest tests/python -q
  uv run python -m compileall -q python/fastdb4py tests/python
  python3 tests/ci/test_check_python_package_inventory.py
  set -o pipefail
  uv build --out-dir build/p3-package-dist \
    2>&1 | tee build/p3-package-build.log
  python3 tools/check_python_package_inventory.py \
    --dist-dir build/p3-package-dist \
    --build-log build/p3-package-build.log
  bash ts/build-wasm.sh
  npm --prefix ts/fastdb4ts run build
  npm run test:ts
  ```

  Expected: every available command passes with exact counts recorded; native and applicable wasm allowlists are exactly 105; all graph/record goldens and 16 seeds pass; sanitizer retained-log scan is clean; consumer regressions/package builds pass. `detect_leaks=0` on local Apple ASan is explicitly not LeakSanitizer evidence. Hosted outcomes remain pending.

- [ ] Probe and, when supported, run focused ThreadSanitizer graph plan/view/invalidation tests in a separate build:

  ```bash
  cmake -S fastcarto -B build/p3-final-tsan \
    -DBUILD_TESTING=ON -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
    -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread' \
    -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=thread'
  cmake --build build/p3-final-tsan \
    --target fastdb_payload_test_graph_backing \
      fastdb_payload_test_graph_view \
      fastdb_payload_test_checked_view --parallel
  TSAN_OPTIONS=halt_on_error=1 \
    ctest --test-dir build/p3-final-tsan \
      -R '^payload\.(graph_backing|graph_view|checked_view)$' \
      --output-on-failure
  ```

  Record compiler/runtime probe output and exact results. If the platform cannot configure, link, or run TSan, preserve that exact limitation in Issue 0002 and do not convert it into a pass.
- [ ] Inspect sdist/wheel inventories and all generated/embedded diffs. Run pure-C arm64/x86_64/wasm32 header/link checks, C++ facade, exact symbol diff, JSON/YAML duplicate-safe parsing, Markdown links/anchors, forbidden ownership/legacy/type scans, `git diff --check`, and clean tracked status.
- [ ] Update Issue 0002 with actual local commands/counts/artifacts/toolchain limits, keep hosted/P4-P5/release open, and do not yet mark D1 closed until Task 9 independent review is clean.
- [ ] Commit:

  ```bash
  git add tests/fuzz/payload/fuzz_payload_open.cpp \
    tests/fuzz/payload/run_payload_open_corpus.cpp \
    tests/fuzz/payload/binary-corpus.json \
    tests/fuzz/payload/binary-corpus/valid-graph-{cycle,variable,null}.bin \
    tests/fuzz/payload/binary-corpus/malformed-graph-{object-region,reference,unreachable}.bin \
    tools/check_payload_binary_corpus.py \
    tests/ci/test_check_payload_binary_corpus.py \
    tests/ci/p3_malformed_class_map.json \
    tests/ci/check_p3_runtime_quality.rb \
    tests/ci/test_check_p3_runtime_quality.rb \
    tests/cpp/payload/test_graph_binary.cpp \
    tests/cpp/payload/test_graph_backing.cpp \
    tests/cpp/payload/test_graph_view.cpp \
    tests/cpp/payload/test_graph_materialize.cpp \
    tests/cpp/payload/test_runtime_abi.cpp \
    tests/wasm/payload_runtime_harness.cpp tests/cpp/CMakeLists.txt \
    tools/check_python_package_inventory.py \
    tests/ci/test_check_python_package_inventory.py \
    .github/workflows/tests.yml \
    docs/issues/0002-portable-payload-foundation-implementation-status.md
  git commit -m "test(core): harden portable graph runtime"
  ```

- [ ] Send the frozen Task 9 commit/diff/report to a fresh reviewer for accepted-spec compliance and code quality. Fix every material finding, rerun all affected broad gates, commit `fix(core): address portable graph runtime review`, and return to the same reviewer until it reports 0 Critical / 0 Important / 0 material Minor.

## Task 10: Close P3 and D1 truthfully, then obtain final independent review

**Files:**

- Modify: `docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md`
- Modify: `docs/superpowers/specs/2026-07-20-portable-payload-object-graph-runtime-design.md`
- Modify: `docs/issues/0001-portable-payload-deferred-capabilities.md`
- Modify: `docs/issues/0002-portable-payload-foundation-implementation-status.md`
- Modify: `docs/issues/README.md`
- Modify: `README.md`
- Modify: `fastcarto/README.md`
- Modify: `schemas/README.md`
- Modify: `CHANGELOG.md`
- Modify: `tests/ci/p3_malformed_class_map.json`

**Interfaces:**

- Consumes: review-clean Tasks 1-9, complete fresh local P3 gate, exact ABI-105, direct proof, source audit, and actual toolchain/hosted state.
- Produces: formal D1 supersession/closure with evidence, exact P3 proof map/status, truthful public docs, final read-only P3 review, clean P3 freeze, and a precise boundary for the later P4 plan. It does not mark the overall P3-P5/C-Two goal complete.

- [ ] Freeze `.superpowers/sdd/p3-task-10-brief.md` with `docs: record portable payload P3 closure`, docs-only allowed files, exact evidence required for D1, no production-code fix hidden in docs, full-gate rerun, final reviewer, and explicit hosted/version/push/tag/publish non-goals.
- [ ] Require every Task 1-9 progress entry and retained report to contain starting/implementation/fix commit, RED/GREEN/broad commands, platform/compiler, counts, artifacts, limitations, and same-reviewer final result. Correct evidence omissions before editing normative status.
- [ ] Update the parent design's graph-direct text and implementation order so it no longer says dynamic graph direct is deferred. Retain historical rationale and link to the accepted P3 delta; do not rewrite P2 or pretend the old fake-direct concern was invalid.
- [ ] Mark Issue 0001 D1 closed with exact implementation commits, named direct test, range-write proof, allocation-threshold proof, execution reports, platform/toolchain, source-audit locations, and reviewer result. Keep D2-D5 open with their existing limits/rationale/impact/dependencies/closure criteria.
- [ ] Update the P3 design implementation status from design-only to locally implemented/reviewed only after evidence exists. Keep hosted and release operations explicitly pending.
- [ ] Rewrite Issue 0002's current summary and proof map to state P1/P2/P3 local freeze, exact ABI-105, ordinary graph support, D1 closure, P4/P5/C-Two remaining, local toolchain limits, and no hosted/release claim. Do not close Issue 0002.
- [ ] Update README/fastcarto/schema/changelog truth: Core and C/C++ graph runtime is locally implemented; Rust/Python/official TypeScript projections and codegen are not; legacy public surfaces still exist pending P5; version remains unchanged; local readiness is not release 0.2.0.
- [ ] Trace every P3 design Section 4-21 and active-goal Stage B bullet to one implementation symbol and one exact test/golden/report in the proof map. Require named coverage for all types, roots/refs, cycles/sharing, direct/staged, malformed IDs/inventory, work/limits, views/lifetime, closure materialization, ABI prefixes/symbols, wasm, and record non-regression.
- [ ] Run the complete Task 9 gate again after the last review fix, not only docs checks. Record new exact counts rather than copying earlier output.
- [ ] Run final documentation checks:

  ```bash
  python3 tools/generate_embedded_payload_schemas.py --check
  python3 tools/check_payload_binary_corpus.py --check
  ruby tests/ci/check_p3_runtime_quality.rb --check-repository
  rg -n 'TB[D]|TO[D]O|FIXM[E]|implement[[:space:]]+later|similar[[:space:]]+to[[:space:]]+Task|appropriate[[:space:]]+error[[:space:]]+handling|write[[:space:]]+tests[[:space:]]+for[[:space:]]+the[[:space:]]+above|fill[[:space:]]+in' \
    docs/superpowers/plans/2026-07-20-portable-payload-object-graph-runtime.md \
    docs/superpowers/specs/2026-07-20-portable-payload-object-graph-runtime-design.md \
    schemas/fastdb.payload.bin.v1.md docs/issues README.md fastcarto/README.md
  git diff --check
  git status --short
  ```

  Expected: generators/corpus/quality pass, placeholder scan has no match, diff check passes, and only the intended Task 10 docs/proof-map files are modified before commit.

- [ ] Commit truthful closure docs:

  ```bash
  git add docs/superpowers/specs/2026-07-16-portable-payload-foundation-design.md \
    docs/superpowers/specs/2026-07-20-portable-payload-object-graph-runtime-design.md \
    docs/issues/0001-portable-payload-deferred-capabilities.md \
    docs/issues/0002-portable-payload-foundation-implementation-status.md \
    docs/issues/README.md README.md fastcarto/README.md schemas/README.md \
    CHANGELOG.md \
    tests/ci/p3_malformed_class_map.json
  git commit -m "docs: record portable payload P3 closure"
  ```

- [ ] Freeze the complete P3 range from `f0aff71` through Task 10, all reports, proof map, current full-gate report, and D1 diff. Send it to a fresh read-only final P3 reviewer for both accepted design compliance and code quality.
- [ ] Fix every Critical/Important and every material Minor. Production findings return to the owning Task 1-9 layer and trigger relevant focused plus full gates; documentation findings remain Task 10. If a finding invalidates any D1 proof or public capability claim, first reopen/correct the tracked D1/status text in the same scoped fix, then close it again only after restored evidence and re-review. Commit scoped fixes and return all of them to the same final reviewer until it reports 0/0/0 material findings.
- [ ] After final review clean, rerun exact ABI-105, complete native/sanitizer/wasm/consumer/package/docs gates affected by the final fix and require a clean tracked worktree/index. Update the retained P3 closure report with final commit IDs/results without inventing hosted evidence.
- [ ] Stop P3 only at a clean local freeze. Do not mark the overall goal complete; next write and approve the P4 four-language projection/Core-codegen design and implementation plan from the frozen actual ABI-105.

---

## Per-Task Implementation and Review Protocol

For every Task 1-10:

1. Write `.superpowers/sdd/p3-task-<n>-brief.md` before RED. It freezes exact starting commit, allowed files, consumed/produced interfaces, RED command/expected failure, focused/broad gates, docs obligation, non-goals, and commit subject.
2. Only one implementation worker may modify the worktree at a time. No reviewer edits files.
3. Capture real RED before production changes. A compile failure must be the named missing capability, not an unrelated broken baseline.
4. Implement the smallest complete task contract, run focused GREEN and relevant broad gates, update Issue 0002 truth, and make one scoped local commit.
5. Preserve `.superpowers/sdd/p3-task-<n>-report.md` with exact commands, platform/compiler, counts, artifacts, allocations/sanitizers, limitations, implementation/fix commits, and review result. Reports remain ignored operational evidence unless a tracked proof map explicitly names them.
6. Freeze commit/diff/report and send it to a fresh read-only reviewer for both specification compliance and code quality.
7. Fix every Critical/Important. Fix any Minor involving correctness, portability, lifetime, determinism, ABI, binary format, resource accounting, direct/staged truth, or coverage; otherwise record it in the exact owner issue with closure criteria.
8. Return fixes to the same reviewer. Do not switch reviewers to evade a finding. Mark the ledger task complete only after same-reviewer re-review is clean.
9. After the final fix, rerun every affected broad gate. Focused GREEN alone never closes a task.
10. Never push, tag, publish, bump versions, or convert workflow definitions into hosted pass claims.

## Final P3 Review Checklist

- [ ] One C++ Core topology/runtime owns both profiles; no legacy `ObjectEngine`, binding, or C-Two code parses/encodes/materializes profile 2.
- [ ] RuntimeSchema and manifest consume one topology derivation and agree on reachability, identity components, roots, refs, lists, and required pools.
- [ ] Handles are builder-local/artifact-free; declaration order alone assigns dense IDs; fill order is byte-neutral; unreachable declarations fail.
- [ ] Normative Markdown, `BinaryFormat`, `GraphLayout`, `GraphEncoder`, `GraphOpen`, goldens, and malformed receipts agree on every profile-2 byte and failure order.
- [ ] Profile 1 goldens, validation work, errors, owner/lifetime behavior, and all 99 old symbol meanings are unchanged.
- [ ] Direct graph proof includes one range-write final backing, no staged/heap reserve, monotonic full coverage, allocation threshold, report truth, staged byte identity, failure injection, and source audit.
- [ ] Graph open rejects malformed inventory/IDs/padding/values/partitions/reachability before index publication and charges limits/work before allocation/read.
- [ ] Cursor variants never fake a runtime type ID; ref dereference is explicit; backed operations pin; invalidation drains before release.
- [ ] Materialization copies only selected reachable closure, preserves sharing/cycles, densely remaps per component, publishes transactionally, and survives source invalidation.
- [ ] Manifest is available only after executable completion; codegen targets remain empty; payload canonical identity is unchanged.
- [ ] V1/V2 struct-prefix canaries and six functions satisfy output/error/exception rules; exact native/applicable wasm allowlist is 105 unique symbols.
- [ ] Complete local Debug/Release/sanitizer/TSan-available/corpus/fuzz/C/C++/wasm/package/language/docs gates pass after final fixes; unavailable tools are recorded, not claimed.
- [ ] D1 is closed only with actual proof; D2-D5, P4/P5/C-Two, hosted CI, version, push, tag, publish, and release remain truthful.
- [ ] Worktree/index is clean and contains no build output, cache, runtime fuzz corpus, generated payload image, secret, or unexplained user change.

## P3 Completion Definition

P3 is locally complete only when a C and C++ caller can compile any valid `object_graph.v1` source; declare/fill objects; author roots, refs, all V1 values, lists, nulls, sharing, forward/self/mutual cycles, and explicit disconnected roots; freeze a deterministic exact plan; execute byte-identical direct/staged payloads through heap or caller backing; open the exact hardened profile-2 binary; traverse checked identity/ref/component/list/value views; materialize an independent reachable closure; invalidate with active-access drain; and release every handle/backing without leaks or races.

The normative layout and annotated ordered goldens must match exact bytes. Every malformed changed field must fail deterministically. The direct proof must rule out a complete hidden image. Manifest facts must be executable, the public C ABI must contain exactly 105 reviewed symbols with old prefixes intact, P2 record behavior must remain frozen, all available local gates must pass after final fixes, D1 must be formally and evidentially closed, and independent review must have no unresolved material finding.

P3 completion does not claim P4 cross-language portable projection/codegen, P5 clean cut/release readiness, C-Two composition, version 0.2.0, hosted CI, push, tag, publish, release, Toodle raw-file sharing, or any application-domain capability.
