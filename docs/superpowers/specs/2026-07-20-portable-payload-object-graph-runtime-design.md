# FastDB Portable Payload Object-Graph Runtime Design

- **Status:** Accepted
- **Accepted:** 2026-07-20
- **Target tranche:** Portable payload P3
- **Frozen baseline:** `74b50faa2013315f2db8a8dcb18dbf5551e2bb48`
- **Parent design:** [FastDB Portable Payload Foundation Design](2026-07-16-portable-payload-foundation-design.md)
- **Authority decision:** [ADR-0001](../../decisions/0001-portable-payload-core-authority.md)
- **Current deferrals:** [Issue 0001](../../issues/0001-portable-payload-deferred-capabilities.md)
- **Implementation status:** [Issue 0002](../../issues/0002-portable-payload-foundation-implementation-status.md)
- **Current binary authority:** [`fastdb.payload.bin.v1`](../../../schemas/fastdb.payload.bin.v1.md)

## 1. Status and purpose

This document is the accepted P3 delta design for the complete ordinary
`object_graph.v1` runtime. It closes the design gaps left after the P1 compiler
and P2 `record.v1` runtime were frozen. It does not claim that P3 is already
implemented, reviewed, or release-ready.

The design defines the exact graph storage model, builder state machine,
binary profile, direct/staged planning behavior, hardened open rules, checked
views, detached materialization, resource accounting, manifest facts, and
minimal additive C ABI. The implementation must land through the task,
RED/GREEN, review, evidence, and broad-gate protocol recorded in the governing
P3-P5 goal.

The following P2 facts remain frozen:

- the C++ Core is the sole semantic and executable authority;
- the stable C ABI is the only cross-language boundary;
- `record.v1` bytes, errors, backing behavior, owner lifetime, checked-access
  barrier, and existing 99 exported symbols retain their meaning;
- `PayloadBuilder -> BuildPlan -> PayloadOwner -> View` remains the runtime
  state model;
- direct execution means that no complete encoded payload image exists outside
  the final backing before commit;
- committed payloads are immutable and invalidation drains checked accesses
  before the backing is released or reused.

Where the parent design or Issue 0001 says that dynamic graph
`REQUIRE_DIRECT` is deferred, this document records the accepted replacement
design: a complete graph is frozen into an exact immutable plan before
execution, so P3 can write it directly. That design decision alone does not
close D1. D1 remains open until the normative documents are revised and the
reviewed implementation evidence in Section 18 exists.

## 2. Decision summary

1. There is one portable-payload Core runtime, not independent record and
   graph authorities.
2. Shared lifecycle, backing, plan execution, owner, error, and checked-access
   infrastructure remains common. Record and graph have profile-specific
   layout, encoding, inventory validation, and cursor metadata inside that one
   Core.
3. `object_graph.v1` stores identity-bearing instances in one AoS object region
   per component. By-value nested components remain inline AoS values.
4. Entry-side component occurrences are graph roots. A `ref` is the only
   construct that introduces shared identity or a cycle.
5. Root and reference slots contain typed little-endian `uint64_t` object IDs.
   They do not require duplicate physical root or reference tables.
6. A graph builder declares objects before referring to them. It returns an
   opaque builder-local temporary handle that is resolved to Core-local object
   coordinates immediately when used and never enters an artifact.
7. Per-component declaration order determines dense wire object IDs. Object
   fill order does not affect bytes.
8. Freeze rejects every declared object that is not reachable from an entry
   value. It never silently prunes objects.
9. Complete freeze determines all graph sizes, offsets, object IDs, list and
   text partitions, and final length. `GraphEncoder` therefore writes the
   final backing monotonically without a complete encoded staging image.
10. Materialization copies the selected view's complete reachable closure,
    preserves sharing and cycles, and densely remaps object IDs per component
    into an independent detached graph.
11. P3 adds exactly six C exports. The public allowlist grows from 99 to 105
    symbols. Resource-reporting structs gain compatible `struct_size`-guarded
    tails without changing their frozen prefixes.

## 3. Alternatives considered

### 3.1 Shared Core pipeline with profile-specific layout and encoding

Selected.

`RuntimeSchema`, `BuildPlan`, final backing, publication, `PayloadOwner`,
`AccessBarrier`, stable errors, and the public ABI remain one pipeline.
`RecordLayout`/`RecordEncoder` continue to own the frozen record algorithm;
`GraphLayout`/`GraphEncoder` implement the graph-specific delta. Open performs
shared container checks followed by profile-specific inventory and value
validation.

This preserves reviewed P2 behavior while keeping one owner and one executable
authority.

### 3.2 Rewrite record and graph into one fully generalized layout class

Rejected for P3.

It could reduce class count, but it would rewrite large parts of the frozen
record path merely to achieve structural symmetry. The resulting regression
surface is larger than the shared behavior it would expose. Proven common
primitives may be extracted, but P3 does not replace the reviewed record
algorithm.

### 3.3 Independent graph runtime with root/reference tables and a graph owner

Rejected.

An independent builder, owner, reader, or materializer would become a second
semantic stack. Separate root and reference tables would duplicate IDs already
present at schema-known slots, add indirection, create two representations that
must agree, and complicate direct planning and hardened validation.

## 4. Authority and internal architecture

The implementation dependency direction is:

```text
CompiledSpec
    -> shared runtime-topology derivation
    -> RuntimeSchema
    -> profile plan
         record.v1       -> RecordLayout / RecordEncoder
         object_graph.v1 -> GraphLayout  / GraphEncoder
    -> shared BuildPlan execution
    -> shared Backing reservation / commit / rollback
    -> profile-aware hardened open
    -> shared PayloadOwner / AccessBarrier
    -> profile-aware checked View / detached materialization
    -> stable C ABI / thin C++ RAII facade
```

The C++ Core owns every box. A profile-specific class is an internal algorithm,
not another authority. No Rust, Python, TypeScript, C-Two, or Toodle code may
parse graph wire bytes, assign object IDs, validate references, compute graph
layout, or materialize graph closures independently.

The graph slice does not call or extend the legacy `ObjectEngine`. Portable
payload semantics flow through the new Core contract. P5 may retain the
standalone engine name where it remains a genuinely separate FastDB API, but
it cannot become a second implementation of this binary profile.

The parent design's `object_graph.v1 <-> ObjectEngine` line names the semantic
engine family, not an implementation delegation. For the portable-payload
path, this delta supersedes any reading that would require the legacy
`ObjectEngine` to encode, open, or validate profile 2.

## 5. Runtime topology and storage roles

### 5.1 Contexts

Runtime topology is derived from the resolved source model using two traversal
contexts:

- **entry-value context:** the type occurrence is in an entry type tree,
  possibly below one or more `list.items` nodes;
- **component-field context:** the type occurrence is a component field or is
  below that field's `list.items` chain.

List traversal preserves its current context. Entering the fields of any
component definition always switches to component-field context.

### 5.2 Storage roles

Every reachable runtime type occurrence receives one storage role:

| Source occurrence | Context | Storage role | Slot |
|---|---|---|---:|
| scalar or variable value | either | ordinary value | existing P2 slot |
| `list` | either | list descriptor | 16 bytes |
| `component` | entry-value | object root ID | 8 bytes |
| `component` | component-field | inline component | component stride |
| `ref` | either | reference ID | 8 bytes |

An entry may itself be a `ref`, or contain refs below lists. Such refs are
entry-originating reachability edges even though they are not component root
occurrences.

### 5.3 Reachability and identity-bearing components

Topology derivation starts from every entry type and follows:

- `list.items`;
- component targets and their fields;
- ref targets and their fields.

By-value component containment is already a finite DAG by P1 validation.
Reference edges may cycle, so component discovery is iterative and visited-set
bounded.

A reachable component is **identity-bearing** when at least one reachable type
occurrence either:

- names it as an entry-value component; or
- names it as a `ref` target.

A component may be identity-bearing in one occurrence and appear inline by
value in another occurrence. Identity-bearing status determines whether a
component has an object region; it never changes the layout of that
component's fields.

Unused source component declarations retain their P1 stable indexes and
runtime type IDs where applicable, but do not create runtime component layouts
or object regions merely because they exist in the source document.

### 5.4 One shared derivation

Reachable components, identity-bearing components, root presence, reference
presence, and required storage classes are derived once by a Core-internal
topology analyzer. `RuntimeSchema` and manifest/capability generation consume
that result. They must not maintain separate algorithms that can disagree
about object-region inventory.

## 6. Logical object model

### 6.1 Objects, values, and identity

Each identity-bearing component has a logical object pool. A declared object
is a non-null instance of that component. Nullability belongs to root/ref slots
that may or may not identify an object; the pool itself contains no null
objects.

Business or domain identity remains an ordinary declared field. A wire object
ID is only a location in one component pool of one payload generation. There
is no public `identity_domain`, global object ID, graph resource class, or
cross-payload identity promise.

By-value nested components are independent values. Two structurally equal
by-value components are not shared. Two refs share only when they resolve to
the same `(component_index, object_id)`.

### 6.2 Roots

Every non-null entry-value component occurrence is a root. This includes
components below arbitrarily nested entry-side lists and values of a
cardinality-many entry.

Every non-null ref reached while walking an entry value tree also introduces
its target into the reachable closure. This permits an entry whose terminal
type is `ref` or `list<ref>`.

Multiple disconnected graphs are valid only when every connected component is
made reachable by one or more entry values. There is no implicit default root
and no hidden root registry.

### 6.3 References

A non-null ref targets exactly the component named by its resolved source type.
It may point forward to an object that is filled later, point to its containing
object, participate in a mutual cycle, or share a target with any number of
other refs.

Raw pointers, language references, transport handles, builder handles, and
business IDs are never reference encodings.

## 7. Builder contract

### 7.1 Temporary object handles

The C ABI represents a temporary handle as an opaque non-zero `uint64_t` token.
Zero is permanently invalid. The token is:

- valid only with the builder that issued it;
- copyable as an authoring token but not retainable;
- invalid after builder freeze or release;
- rejected when forged, stale, or supplied to another builder;
- never persisted in `ValueArena`, `BuildPlan`, wire bytes, manifests,
  generated artifacts, digests, or stable error details.

When a handle is supplied, the Core resolves it immediately to
`(component_index, declaration_index)` and stores only those Core-local facts.
The implementation must associate sufficient builder identity with a token to
reject accidental numeric reuse across builders. The particular token
generation mechanism is not observable and cannot affect output bytes.

### 7.2 Object declaration

`object_declare(component_index)` is legal only when:

- the builder is not frozen;
- no entry or object-fill authoring scope is active;
- the profile is `object_graph.v1`;
- the index names a reachable identity-bearing component;
- the new total does not exceed `max_graph_objects`, `max_value_nodes`, native
  capacity, or `max_total_builder_bytes`.

Successful declarations append to that component's declaration sequence and
return one handle. Declarations for different components may be interleaved;
wire ID assignment is independently dense within each component.

### 7.3 Object fill

`object_fill_begin(handle)` begins the ordinary component-field expectation
for one declared object. It is legal only when no other top-level authoring
scope is active and the object has not already been filled. Every component
field must be authored exactly once in stable field order using the existing
typed value operations.

The existing component-begin operation always authors an inline by-value
component. It cannot substitute for an identity root or ref.

An object is filled exactly once. P3 does not add mutable replacement or
deletion to the append-only complete builder. A caller that wants a different
already-completed logical graph releases and rebuilds it; a future incremental
editing or streaming contract remains D3.

### 7.4 Root and ref values

Two distinct leaf operations consume handles:

- `value_object(handle)` is legal only for an expected component occurrence
  whose storage role is object root ID;
- `value_ref(handle)` is legal only for an expected `ref` occurrence.

The handle's component must equal the expected component target. `value_null`
continues to author a null root or ref only when that occurrence is nullable.

The builder may alternate complete entry scopes, declarations, and complete
object-fill scopes in any order. Only one scope is active at a time. Forward
references mean that an object may be declared and referenced before it is
filled; references to undeclared symbolic objects are not accepted.

### 7.5 Deterministic builder errors

Operations validate all inputs and resource growth before mutating state.
Handle lookup failure never publishes the token in an error. Stable logical
object paths use:

```text
/objects/<component-id>/<per-component-declaration-index>
```

Freeze checks failures in this order:

1. an active incomplete entry/list/component/object-fill scope;
2. missing entries by stable entry index;
3. unfilled objects by stable component index then declaration index;
4. unreachable objects by stable component index then declaration index.

This ordering is part of the deterministic error contract.

## 8. Freeze and immutable planning

### 8.1 Dense object IDs

For each identity-bearing component, freeze maps declaration index `i` to wire
object ID `i`. This is a deliberate semantic ordering, not graph-isomorphism
canonicalization. Fill order, arena insertion order, hash-table order, pointer
address, and traversal order cannot affect IDs or bytes.

The plan retains Core-local component and object coordinates only. Temporary
handle tokens are discarded from the executable state.

### 8.2 Reachability validation

Freeze computes reachability iteratively:

1. walk every authored entry value tree without following object fields;
2. enqueue each present object-root ID and each present entry-originating ref;
3. dequeue each not-yet-visited object once;
4. walk its component fields, recursively through by-value components and
   lists with an explicit stack;
5. enqueue each present ref target;
6. after the queue drains, scan object pools by component/declaration order and
   reject the first unvisited declaration.

Every declared object must be reachable. P3 never silently prunes an object or
rewrites the user's declaration order. This runtime rule does not reject unused
component definitions in the source schema.

### 8.3 Exact plan

Successful freeze publishes an immutable, repeatable `BuildPlan` that owns:

- the complete normalized logical entry values;
- the complete object records grouped by component and dense ID;
- exact component, root, ref, list, string, wide-string, and bytes slot facts;
- exact canonical list and variable-pool partitions;
- every region descriptor, offset, length, stride, and alignment;
- total final bytes and validation-work requirements;
- graph object count and direct-build eligibility.

If layout construction fails after logical freeze, the C ABI publishes no plan
and restores the complete builder-owned graph state so the existing retryable
freeze contract remains true. Restoration includes object tables, object-fill
roots, entry roots, and arena state.

## 9. `fastdb.payload.bin.v1` graph profile

### 9.1 Version and profile

P3 keeps binary major/minor `1.0`. The fixed container and descriptor sizes
remain 128, 56, and 40 bytes. The header profile is exactly:

```text
FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1 = 2
```

This is not an ambiguous extension of record bytes. The source and public ABI
already reserve an explicit `object_graph.v1` profile; pre-P3 readers reject
that profile before interpreting record layout. P3 defines the previously
unimplemented profile-specific inventory. `record.v1` profile `1` retains
byte-for-byte and error-for-error meaning.

Header offset 96 remains the checked sum of entry value counts. Its historical
name `root value count` does not mean “number of graph objects” and does not
change for profile 2.

### 9.2 New region kind

P3 adds one region kind:

```text
FDB_PAYLOAD_REGION_OBJECT_VALUES = 8
```

There is exactly one `OBJECT_VALUES` region for every reachable
identity-bearing component, including when its runtime object count is zero.

| Descriptor field | `OBJECT_VALUES` rule |
|---|---|
| `kind` | `8` |
| `flags` | `0` |
| `owner_index` | stable component index |
| `runtime_type_id` | `UINT32_MAX` sentinel |
| `data_offset` | smallest aligned offset after the prior region |
| `byte_length` | checked `object_count * component_stride` |
| `element_count` | object count |
| `stride` | component AoS stride |
| `alignment` | component alignment, exactly `1`, `2`, `4`, or `8` |
| `reserved` | `0` |

Object ID `i` is stored at byte offset
`data_offset + i * component_stride`. The record at that location follows the
same immediate-field validity, stable-field order, nested by-value component,
alignment, null-zero, unused-bit, and padding-zero rules as P2 inline
components.

Object pools have no object-level validity bitmap. A declared object exists;
nullable roots and refs decide whether a slot identifies one.

### 9.3 Root and reference slots

Every identity component occurrence in entry-value context and every `ref`
uses this slot:

| Field | Rule |
|---|---|
| width | 8 bytes |
| alignment | 8 |
| present value | little-endian `uint64_t object_id` |
| valid range | `[0, target_object_count)` |
| null value | controlled by the existing validity bit; all 8 slot bytes zero |

Object ID zero is valid. A nullable present reference to object zero and a null
reference therefore have identical slot bytes but distinct validity bits.

The target component pool is fixed by the compiled type occurrence. The wire
does not store another component tag in each slot.

Roots and refs are explicit typed ID slots. There are no additional physical
`ROOTS` or `REFERENCES` regions. Such tables would duplicate slot data and
create two facts that could disagree. Manifest names `roots` and `references`
as logical storage classes, not physical region kinds.

This exact slot contract supersedes the parent design's generic “reference
tables” wording for profile 2. The required explicit reference data is the
schema-typed ID slot plus its validity bit; it is not a second physical table.

### 9.4 Canonical region inventory

For profile 2, the region directory contains exactly:

1. each entry by stable index: nullable validity first, then values;
2. each identity-bearing component by stable component index: one
   `OBJECT_VALUES` region;
3. each reachable list node by ascending runtime type ID: nullable-item
   validity first, then items;
4. UTF-8, UTF-16LE, and bytes pools in that order when reachable.

Required zero-length regions remain present. Adjacent empty regions may share
the same aligned offset under the existing P2 rule. Unknown, duplicate,
missing, reordered, aliased, or extra regions are non-canonical.

### 9.5 Canonical occurrence traversal

List aggregation and variable-pool bytes use one finite physical occurrence
order:

1. entries by stable entry index and value index;
2. then identity-bearing components by stable component index;
3. then objects in that component by dense object ID;
4. within an entry value or object record, fields and list items are walked
   depth-first in stable order with an explicit stack.

An object-root or ref slot is emitted but its target is not traversed during
physical aggregation. Every object record is traversed later exactly once in
its object-region order. Cycles and sharing therefore cannot alter list or
variable-pool ordering.

This same occurrence order governs layout planning, encoding, structural open
validation, deterministic goldens, and complete-payload materialization.

### 9.6 Canonicality, overflow, and forward compatibility

All P2 checked arithmetic, partition, null-zero, Boolean, numeric, text,
alignment, inter-region padding, final padding, and exact-length rules apply to
profile 2. Additionally:

- every root/ref ID load is bounded and little-endian;
- every object count, sum, product, offset, marker allocation, and queue growth
  is checked before use;
- unused high validity bits and all padding remain zero;
- a null root/ref has a zero slot;
- an object region's component, count, stride, alignment, and order are exact;
- every object in an accepted binary is reachable from entry values.

Unknown header flags, region flags, profile values, or region kinds fail
closed. A future graph format that needs different ID width, separate tables,
segmented regions, or changed component layout requires a new accepted profile
or binary version; readers never infer an extension from spare bytes.

## 10. Direct and staged execution

### 10.1 One profile plan in one BuildPlan

`BuildPlan` owns an internal profile-plan variant. The frozen
`RecordLayout`/`RecordEncoder` remains one variant and the new
`GraphLayout`/`GraphEncoder` is the other. Final-backing selection,
reservation, write sinks, commit, rollback, publication, and execution report
remain shared.

`GraphEncoder` consumes only immutable graph plan facts and writes a `ByteSink`
at the exact next offset. It does not build or return a complete byte vector.
Small bounded metadata scratch is allowed; a second complete encoded image is
not.

### 10.2 Direct truth

Every successfully frozen P3 graph plan has direct status `ELIGIBLE` because
all final sizes and fixups are exact before execution.

For a successful direct execution:

- the backing receives one direct reserve;
- every write is monotonic, non-overlapping, and collectively covers exactly
  `total_bytes`;
- no staged reserve or Core heap-image reserve occurs;
- commit publishes exactly `total_bytes`;
- the report has mode `DIRECT`, fallback `NONE`, and `staging_bytes = 0`.

If direct reserve is declined:

- `REQUIRE_DIRECT` returns `DIRECT_UNAVAILABLE` with reason
  `backing_declined_direct` and performs no staged reserve;
- `ALLOW_STAGING` may encode once into a Core-owned heap backing and copy the
  complete image into the caller's staged reservation;
- the successful report has mode `STAGED`, fallback
  `BACKING_DECLINED_DIRECT`, and `staging_bytes = total_bytes`.

Failure after a successful direct reserve rolls back. It never retries through
staging, because that would hide a write/commit failure rather than represent
a backing that declined direct construction.

### 10.3 D1 correction

D1's current rationale conflates mutable authoring with immutable plan
execution. V1 already requires complete logical authoring before freeze. Once
freeze succeeds, cyclic reachability does not prevent exact layout: object
declarations, IDs, fields, lists, strings, byte spans, roots, and ref targets
are all finite and known.

The complete logical builder state is not an encoded staging image. D3 still
accurately records that authoring memory grows with the complete logical
payload and that V1 is not a streaming builder.

D1 may be marked closed only after Section 18's direct-path proof and review
are committed. Until then, manifest and implementation-status documents must
not claim graph direct support merely because this design is accepted.

## 11. Hardened graph open

### 11.1 Dispatch and shared container validation

Public open dispatches from the provided `CompiledSpec.profile`. It verifies
that the header profile matches that exact profile. The implementation may
extract narrow shared header/directory readers, but must preserve every frozen
record error, path, detail, and validation-work result.

The graph path performs validation in this deterministic order:

1. options, supplied span, header, version, profile, total, digest, and header
   reserved bytes;
2. checked directory ranges, exact counts, descriptor sizes, directory
   padding, and total bounds;
3. exact entry, object, list, and variable-pool region inventory;
4. descriptor fields, offsets, alignment, padding, validity, component slots,
   numeric values, and null-zero rules;
5. every present root/ref ID against its schema-selected target pool;
6. exact list and variable-pool partitions plus eager text validation when
   requested;
7. graph reachability and the final all-objects-reached check.

An index is published only after every phase succeeds.

### 11.2 Object and reference validation

Open derives the identity-component inventory from the same runtime topology
used by the manifest. It sums object counts with checked arithmetic and applies
`max_graph_objects` before allocating object markers or queues.

Structural validation examines every object record, including one that later
proves unreachable. Present root/ref IDs outside the target pool fail with
`INVALID_REFERENCE`. A malformed object region or wrong canonical inventory
fails with `NON_CANONICAL_BINARY`.

Reachability uses the same roots and edges as freeze. It is iterative, marks
`(component_index, object_id)`, and traverses each reached object once. The
first unmarked object in component/object-ID order fails with
`NON_CANONICAL_BINARY` and reason `unreachable_object`.

### 11.3 Resource accounting

Existing P2 work charges remain unchanged. Graph structural validation treats
identity-root and ref slots as ordinary logical slots and treats each object
record plus its fields/by-value descendants as inspected logical values.

The reachability phase additionally charges:

- one work unit for every logical slot revisited while walking entry trees and
  each first-reached object record tree;
- one work unit for every object marker checked in the final completeness
  scan.

The charge is applied before the corresponding read, queue growth, or marker
access. `GraphLayout` computes the exact expected work for Core-built images;
open independently accumulates and must reach the same result. Reference-chain
length is controlled by graph-object and validation-work limits, not by native
recursion. `max_nesting_depth` continues to govern structural list/by-value
nesting rather than graph-edge depth.

## 12. Profile-aware index and checked views

### 12.1 Cursor model

A graph-capable index must not pretend that every component object is an inline
slot associated with a source `runtime_type_id`. It uses explicit internal
cursor variants:

- **sequence cursor:** stable entry sequence;
- **inline-value cursor:** runtime type ID, slot location, kind, and presence;
- **identity-object cursor:** component index, object ID, and presence;
- **ref cursor:** ref runtime type ID, target component index, object ID, and
  presence.

An object obtained by dereferencing a ref has an identity-object cursor. It
does not synthesize a component `runtime_type_id` that never existed in the
source tree.

The index stores object-region metadata by component index: region/base,
object count, stride, and alignment. Checked address calculations use the
existing bounded-load rules.

### 12.2 View behavior

- An identity-object cursor reports kind `COMPONENT` and supports
  `component_index`, `field_count`, and `field`.
- An inline by-value component also reports kind `COMPONENT` and supports the
  same field traversal, but has no graph identity.
- A ref cursor reports kind `REF`. `field` never implicitly dereferences it.
- `ref_target` explicitly returns the target identity-object view.
- `graph_identity` returns `(component_index, object_id)` for a present
  identity-object or ref view.
- `graph_identity` on an inline component or other kind is `TYPE_MISMATCH`.
- `ref_target` on a non-ref is `TYPE_MISMATCH`.
- Either operation on a null applicable view is `UNEXPECTED_NULL`.

Object IDs are observable so callers can compare sharing inside one payload or
detached graph. They are scoped to that payload generation and component. They
are not stable across materialization, rebuild, transport, or another payload.

List item navigation consults the item's runtime storage role. It returns an
identity-object, ref, or inline-value cursor as appropriate.

### 12.3 Lifetime

Every backed navigation or identity query takes the same short checked access
pin as P2. Child views retain the same owner and captured generation. No graph
view exposes a backing pointer. Invalidation prevents new pins, waits for live
pins to drain, advances the generation, and then releases the backing exactly
as in P2.

## 13. Reachable-closure materialization

### 13.1 Detached graph state

Detached materialization extends the current tree arena into one immutable
state containing:

- a detached value arena for the selected root tree and by-value data;
- per-component detached object records;
- an explicit detached root cursor;
- the shared immutable runtime schema.

It is not a new payload owner, binary reader, or backing. It has no dependency
on the source backing after publication.

### 13.2 Algorithm

Materialization holds one source access pin for the complete transaction:

1. walk the selected view's root tree iteratively;
2. discover all referenced identity objects, tracking
   `(component_index, source_object_id)` to preserve sharing and terminate
   cycles;
3. for each component, sort the reachable source IDs in ascending order;
4. assign dense target IDs `0..n-1` in that order;
5. allocate and copy the detached root tree and selected object records,
   rewriting every root/ref coordinate to the target ID;
6. validate internal counts and publish the detached state only after the
   entire copy succeeds.

Any allocation or validation failure publishes no partial view. Materializing
an already-detached graph performs the same independent transactional copy.

### 13.3 Required semantics

- Shared source refs to one object remain shared in the result.
- Self and mutual cycles remain cycles.
- A component or list subview copies only objects reachable from that subview.
- A ref subview remains a ref root and includes its target closure.
- A by-value component subview includes closures reached through refs in its
  fields.
- Target object IDs are dense per component and may differ from source IDs.
- The result's values, strings, bytes, refs, and identity observations remain
  usable after source invalidation and release.

## 14. Manifest and capability truth

After P3 implementation is complete, reviewed, and enabled, a graph manifest
reports:

- `runtime.status = "available"`;
- `runtime.layout_model = "object_pool_aos"`;
- `required_pools` in the existing order: `utf8`, `utf16le`, `bytes`,
  `list_items`, `objects`, `references`, `roots`, including only required
  logical classes;
- `objects` only when the reachable topology has an identity-bearing
  component, not merely a by-value component;
- `references` only when a reachable ref occurrence exists;
- `roots` when an entry terminal occurrence is a component/ref through any
  list chain;
- operations `compile`, `query`, `build`, `open`, `view`, `materialize`, and
  `invalidate`;
- direct status `eligible` with reason
  `graph_layout_exact_after_freeze`;
- an empty codegen target list until P4 actually lands each target.

The manifest schema and graph golden change with these executable capability
facts. The canonical `fastdb.payload.v1` bytes and its SHA-256 digest do not
change, because runtime capability is not payload identity.

Design acceptance alone does not change the current manifest. Capability
claims change in the same reviewed task that supplies their executable proof.

## 15. Additive C ABI

### 15.1 Constants and type

P3 adds:

```c
#define FDB_PAYLOAD_REGION_OBJECT_VALUES UINT32_C(8)
#define FDB_PAYLOAD_V1_INVALID_OBJECT_HANDLE UINT64_C(0)
#define FDB_PAYLOAD_E_INVALID_OBJECT_HANDLE UINT32_C(2014)
#define FDB_PAYLOAD_E_UNREACHABLE_OBJECT UINT32_C(2015)

typedef uint64_t fdb_payload_v1_object_handle_t;
```

The handle typedef is an authoring token, not a retainable ABI object and not a
wire object ID.

### 15.2 Versioned struct tails

The frozen prefixes remain accepted unchanged:

- builder options prefix: 88 bytes;
- plan info prefix: 104 bytes.

The current public structs append one fully covered `uint64_t` tail after the
existing reserved prefix:

```c
/* Existing fields and reserved[4] remain at their frozen offsets. */
uint64_t max_graph_objects;  /* builder options current size: 96 */

/* Existing fields and reserved[4] remain at their frozen offsets. */
uint64_t graph_object_count; /* plan info current size: 112 */
```

The existing constants remain frozen and two struct-revision constants are
added exactly as follows:

```c
#define FDB_PAYLOAD_V1_BUILDER_OPTIONS_V1_SIZE UINT32_C(88)
#define FDB_PAYLOAD_V1_BUILDER_OPTIONS_V2_SIZE UINT32_C(96)
#define FDB_PAYLOAD_V1_PLAN_INFO_V1_SIZE UINT32_C(104)
#define FDB_PAYLOAD_V1_PLAN_INFO_V2_SIZE UINT32_C(112)
```

The unchanged initializer functions write the corresponding `V2_SIZE`. This
is a compatible struct-prefix revision inside C ABI V1, not a payload binary,
ABI-family, or release-version change. New Core accepts the old complete V1
prefix and supplies the default graph-object limit when the tail is absent or
zero. It reads or writes a tail only when `struct_size` fully covers it and
ignores a larger unknown tail. Existing flags and fully covered reserved words
remain zero.

Old callers therefore continue to build record payloads and may build graphs
with the Core default. Old plan-info callers receive the complete old prefix;
new callers can observe the graph object count without a new exported query.

### 15.3 Six new functions

P3 adds exactly these exports:

```c
fdb_payload_v1_status_t fdb_payload_v1_builder_object_declare(
    fdb_payload_v1_builder_t* builder,
    uint32_t component_index,
    fdb_payload_v1_object_handle_t* out_object,
    fdb_payload_v1_error_t** out_error);

fdb_payload_v1_status_t fdb_payload_v1_builder_object_fill_begin(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_object_handle_t object,
    fdb_payload_v1_error_t** out_error);

fdb_payload_v1_status_t fdb_payload_v1_builder_value_object(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_object_handle_t object,
    fdb_payload_v1_error_t** out_error);

fdb_payload_v1_status_t fdb_payload_v1_builder_value_ref(
    fdb_payload_v1_builder_t* builder,
    fdb_payload_v1_object_handle_t object,
    fdb_payload_v1_error_t** out_error);

fdb_payload_v1_status_t fdb_payload_v1_view_ref_target(
    const fdb_payload_v1_view_t* view,
    fdb_payload_v1_view_t** out_target,
    fdb_payload_v1_error_t** out_error);

fdb_payload_v1_status_t fdb_payload_v1_view_graph_identity(
    const fdb_payload_v1_view_t* view,
    uint32_t* out_component_index,
    uint64_t* out_object_id,
    fdb_payload_v1_error_t** out_error);
```

All existing C ABI rules apply. A fallible call requires `out_error`, clears
every present value output before ordinary validation, publishes outputs only
on complete success, catches every C++ exception, and returns an owned stable
error on failure. `object_declare` clears its output to the invalid token;
`ref_target` clears its handle; `graph_identity` clears both numeric outputs.

The exact public symbol inventory grows from 99 to 105. Count-only checks are
forbidden; native and applicable wasm builds diff sorted unique symbols against
the checked-in 105-line allowlist.

### 15.4 Thin C++ facade

The header-only C++17 facade adds:

- an `ObjectHandle` value whose constructor from the raw token is private;
- `Builder::declare_object`;
- `Builder::begin_object_fill`;
- `Builder::push_object` and `Builder::push_ref`;
- `View::ref_target`;
- a small returned graph-identity value containing component index and object
  ID.

It performs only C ABI calls, checked fixed-width conversions, RAII ownership,
and copied-error translation. It contains no topology, layout, reachability,
binary, or materialization algorithm.

## 16. Stable errors

P3 reserves two builder codes:

| Code | Symbol | Use |
|---:|---|---|
| 2014 | `INVALID_OBJECT_HANDLE` | zero, forged, stale, foreign-builder, or otherwise unknown temporary handle |
| 2015 | `UNREACHABLE_OBJECT` | first completely authored declaration not reachable at freeze |

Existing codes retain these graph uses:

| Condition | Code | Stable reason |
|---|---:|---|
| declare non-identity component or use handle at wrong expected component/kind | `TYPE_MISMATCH` 2004 | operation-specific kind/component mismatch |
| duplicate fill or operation while another top-level scope is active | `BUILDER_STATE` 2006 | `object_already_filled` or active-scope reason |
| incomplete/unfilled object fields | `MISSING_FIELD` 2002 | `field_not_authored` or `object_not_filled` |
| builder object limit | `BUILDER_RESOURCE_LIMIT` 2013 | resource `graph_objects` |
| root/ref ID outside target pool on open | `INVALID_REFERENCE` 3007 | `root_object_id_out_of_range` or `reference_object_id_out_of_range` |
| malformed object inventory or unreachable binary object | `NON_CANONICAL_BINARY` 3009 | exact inventory reason or `unreachable_object` |
| open object limit or reachability work limit | `RESOURCE_LIMIT` 3008 | resource `graph_objects` or `validation_work` |
| null identity/ref query | `UNEXPECTED_NULL` 2003 | `unexpected_null` |
| graph identity on by-value/non-identity view, or dereference of non-ref | `TYPE_MISMATCH` 2004 | `view_kind_mismatch` |

Builder errors never contain temporary token values. Binary diagnostics may
contain wire component indexes and object IDs because those are stable facts of
the rejected image. The first failure is deterministic under Sections 7.5 and
11.1.

## 17. Implementation boundaries

The expected internal separation is:

- a shared resolved-topology analyzer consumed by manifest and runtime schema;
- profile-aware `RuntimeSchema` slot/storage-role metadata;
- graph object state added to the existing logical payload arena;
- `GraphLayout` for exact immutable planning;
- `GraphEncoder` for monotonic `ByteSink` output;
- a narrow shared container reader plus profile-specific record/graph open
  validation;
- object-pool metadata and cursor variants in the existing payload index/view
  layer;
- graph closure state in detached materialization;
- additive C ABI implementation and header-only C++ facade methods.

The precise file split may follow existing reviewable-size conventions, but
these boundaries cannot collapse into one giant graph class or duplicate the
same semantics in ABI/binding code. No empty Rust crate, Python package,
TypeScript module, codegen target, or C-Two adapter is created during P3.

P2 record characterization, exact binary goldens, error details, validation
work, and ABI tests remain active regression gates throughout any shared-helper
extraction.

## 18. Verification and D1 closure evidence

### 18.1 Deterministic semantic matrix

The checked graph corpus includes at least:

- one root and one component pool;
- multiple identity-bearing component pools;
- shared references;
- forward refs, self cycles, and mutual cycles;
- nullable/non-null roots and refs, including valid object ID zero;
- cardinality-one and cardinality-many entries;
- entry-side nested lists of objects and refs;
- component-field by-value components and lists of by-value components;
- all V1 scalar kinds, normalized integers, `str`, `wstr`, `bytes`, recursive
  lists, empty values, and nullability;
- multiple disconnected graphs with explicit roots;
- a source schema containing unused components;
- orphan declarations rejected at freeze;
- two builds with the same declarations and different fill order producing
  identical bytes;
- intentionally different declaration orders producing their correspondingly
  different, deterministic object-ID order;
- repeat execution and direct/staged byte identity.

Ordered binary goldens include hand-reviewable byte annotations, independent
SHA-256 files, exact manifests, and an index that prevents unreviewed corpus
drift.

### 18.2 Malformed input

Graph malformed tests cover every changed field class:

- profile mismatch and graph-only region in record profile;
- missing, duplicate, reordered, unknown, or extra object regions;
- wrong owner component, runtime sentinel, count, stride, alignment, byte
  length, offset, flags, or reserved bytes;
- non-zero inter-region, component, null-slot, validity-tail, and final padding;
- root/ref ID just at and beyond the target object count;
- empty target pools with present IDs;
- corrupted list partitions reached through object records;
- malformed UTF-8/UTF-16LE reached through graph objects;
- object-count, native-capacity, validation-work, and nesting-limit failures;
- structurally valid but unreachable object records.

Every malformed fixture asserts exact status, symbol, path, message class, and
canonical details. Repeated open produces identical diagnostics.

### 18.3 Builder, allocation, and lifecycle failures

Allocation-failure injection walks declaration, handle bookkeeping, object
fill, freeze reachability, graph layout, direct encode, publication open,
object/ref view creation, and detached materialization. Each failure proves:

- no partial public output;
- no leaked handle, reservation, access pin, owner, or detached state;
- exactly-once rollback/release where applicable;
- retry behavior consistent with the existing builder/backing contract.

Checked views prove shared identity, explicit ref dereference, null behavior,
generation capture, concurrent read-only use, drain-before-release
invalidation, and post-invalidation materialized graph survival. Focused TSan
coverage is required where the current platform/toolchain supports it.

### 18.4 Direct-path proof required to close D1

A large graph with variable-width data executes through a caller-owned,
range-write-only final backing under `REQUIRE_DIRECT`. The proof must show all
of the following together:

1. exactly one `DIRECT` reserve and no `STAGED` reserve;
2. no invocation of the Core heap-backing reserve path;
3. monotonically increasing, non-overlapping writes covering the final length;
4. an allocation-failure threshold during execution that would reject a
   second complete encoded image while permitting bounded plan/open metadata;
5. report mode `DIRECT`, fallback `NONE`, and `staging_bytes = 0`;
6. byte identity with an allowed staged execution of the same immutable plan;
7. source review confirming that `GraphEncoder` writes `ByteSink` directly and
   contains no full-image byte buffer;
8. cycles, sharing, lists, `str`, `wstr`, bytes, and injected backing failures
   on the direct path.

Execution-report assertions alone are insufficient.

### 18.5 Complete gates

The final P3 task reruns, after the last review fix:

- clean native Debug and Release build/test;
- ASan+UBSan with retained-log zero-diagnostic inspection;
- focused ThreadSanitizer when available;
- record and graph binary robustness/fuzz corpus, with a graph matching spec;
- pure-C header/link checks on supported native and wasm targets;
- exact native and applicable wasm ABI-105 symbol diff;
- C++ RAII graph facade tests;
- Core wasm32/Node graph runtime and injected-failure paths;
- unchanged Python and TypeScript/WASM regression suites even though their P3
  portable graph projection is not yet public;
- JSON schema parsing, manifest/golden regeneration checks, package inventory,
  Markdown relative links, forbidden ownership terms, and `git diff --check`;
- a clean tracked worktree with no build, cache, or runtime fuzz artifacts.

P3 then receives a fresh read-only completion review. Every Critical,
Important, and correctness/portability/lifetime/determinism/ABI/format/coverage
Minor is fixed and returned to the same reviewer until the result is zero.

## 19. Documentation transition and D1 closure

Design acceptance authorizes implementation planning but does not change
shipped capability. The implementation plan must sequence documentation truth
as follows:

1. add the exact graph profile to the normative binary Markdown and add its
   RED goldens/tests before production encoding;
2. implement and review the Core/runtime/ABI slice while Issue 0002 describes
   remaining gaps accurately;
3. after executable graph direct proof exists, revise the parent design's D1
   statement and mark Issue 0001 D1 closed with commit, test, report, platform,
   and reviewer evidence;
4. change manifest/capability status only in the task that makes those facts
   executable;
5. record local and hosted evidence separately. Hosted CI remains pending
   until push is separately authorized.

The D1 history is retained. It is corrected, not deleted: the old concern
about fake direct output was valid, while the assumption that complete freeze
could not know graph size is superseded.

## 20. Scope and remaining limits

P3 completes ordinary `object_graph.v1`; none of its required semantics may be
moved to a deferred issue. In particular, cycles, sharing, all V1 values,
direct/staged execution, hardened open, checked views, reachable-closure
materialization, invalidation, the C ABI, the C++ facade, WASM Core proof, and
resource limits are P3 work.

The following existing Issue 0001 limits remain after P3:

- D2: segmented or multipart final backing;
- D3: incremental/streaming builder and mutable graph editing;
- D4: guaranteed platforms beyond Linux x86-64, macOS arm64, and wasm32;
- D5: native Node.js and Go portable projections.

P3 does not implement Rust, Python, or official TypeScript/WASM portable
projections or code generation; those are non-deferrable P4 work after the
P3 Core/ABI freeze. It does not perform the P5 legacy clean cut or the C-Two
composition slice. It adds no CRM, route, relay, transport, lease, policy,
Toodle, GIS, raw-file, or object-storage semantics.

The contiguous complete-builder model is intentional V1 behavior and is
already recorded by D2/D3 with current limit, rationale, impact, owner,
dependencies, and closure criteria. This design introduces no additional known
P3 limitation requiring a new issue.

## 21. P3 acceptance criteria

P3 is complete only when current repository evidence proves all of the
following:

1. `object_graph.v1` compiles into one Core runtime topology and supports every
   V1 value kind, roots, refs, sharing, and cycles.
2. Builder handles are builder-local and artifact-free; declaration order is
   semantic and fill order is byte-neutral.
3. Freeze rejects unfilled and unreachable objects deterministically.
4. Binary profile 2, object regions, ID slots, canonical ordering, zero rules,
   and hardened open match the normative Markdown and ordered goldens.
5. Exact direct planning and monotonic final-backing execution satisfy every
   D1 proof item; staged fallback remains truthful.
6. Checked identity/ref/component/list views and invalidation reuse the P2
   owner/barrier semantics.
7. Detached materialization preserves sharing/cycles and survives source
   invalidation with dense per-component remapping.
8. Manifest and capabilities report only implemented facts.
9. The C ABI is additive, old prefixes remain valid, and the exact allowlist is
   105 symbols on every applicable build.
10. The C++ facade remains a thin C ABI projection.
11. P2 record bytes, errors, validation work, lifetime, and its original 99
    symbols retain their frozen meaning.
12. All focused and broad gates pass after final fixes, and independent review
    has no unresolved Critical, Important, or material Minor finding.
13. Issue 0001 D1 and Issue 0002 state implementation, local evidence, hosted
    status, and remaining limitations exactly.
14. No version bump, push, tag, publish, or release claim occurs without a
    separate user authorization.
