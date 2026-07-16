# ADR-0001: Portable Payload Core Authority and 0.2.0 Clean Cut

- **Status:** Accepted
- **Date:** 2026-07-16
- **Design:** [FastDB Portable Payload Foundation Design](../superpowers/specs/2026-07-16-portable-payload-foundation-design.md)
- **Deferrals:** [Issue 0001](../issues/0001-portable-payload-deferred-capabilities.md)

## Context

FastDB currently exposes useful storage, view-lifetime, final-backing, Python, and TypeScript/WASM mechanics, but its cross-runtime integration path is split across Python-owned `fastdb.schema.v1`, duplicated Python/TypeScript call-db semantics, and native storage APIs that were not designed as a stable cross-language payload ABI. The public name `ColumnEngine` also implies a SoA/Arrow-like physical layout even though the engine stores AoS records and provides field access by striding over them.

C-Two needs FastDB payloads inside a larger CRM contract, but C-Two must not become FastDB's schema compiler or give one language SDK extra payload powers. Keeping the existing call-db path would leave schema, canonical identity, layout, errors, and capability behavior distributed across repositories and languages.

FastDB is still 0.x. Preserving the wrong public abstraction would create a longer-lived compatibility burden and make later correction harder.

## Decision

FastDB will replace its public call-db integration path with `fastdb.payload.v1` for the 0.2.0 release.

The C++ Core is the sole authority for:

- strict source parsing and validation;
- explicit `record.v1` and `object_graph.v1` profile rules;
- the native type algebra and explicit nullability;
- normalization, RFC 8785 canonical JSON, and SHA-256 identity;
- resolved manifests and capability reports;
- deterministic `fastdb.payload.bin.v1` layout and validation;
- builder, plan, backing, payload owner, view, materialization, and invalidation semantics;
- stable structured errors;
- payload-only cross-language code generation.

Native and WebAssembly consumers cross a stable C ABI with versioned `fdb_payload_v1_*` symbols, opaque handles, exact-width types, explicit retain/release, owned errors, and versioned callback tables. The public C++ API is a RAII facade over this ABI rather than a competing C++ class ABI.

Rust, Python, and TypeScript/WASM project the Core contract. They do not implement independent parsers, canonicalization, digest, profile validation, layout planning, or binary readers.

C-Two's `c-two.contract.v2` is a super-schema that contains the FastDB payload document. C-Two parses and assembles the outer document, delegates the nested FastDB value to the FastDB library, and composes returned FastDB codegen artifacts through the single `c3` entry point. FastDB contains no CRM method, route, relay, transport, lease, policy, or lifecycle semantics.

The 0.2.0 release makes a clean cut:

- remove public call-db schema/runtime/codegen surfaces;
- remove `fastdb.schema.v1` as portable authority;
- remove `columnar.v1` naming;
- rename `ColumnEngine` to `RecordEngine` without an alias;
- retain useful low-level mechanics only after refactoring them behind generic payload concepts;
- retain `FastSerializer` only as a clearly separate legacy standalone serializer;
- provide no compatibility parser, dual digest, or deprecated alias.

## Alternatives considered

### Keep call-db and incrementally wrap it

Rejected. It preserves a transport-shaped abstraction inside FastDB, retains duplicated language semantics, and makes the eventual owner boundary harder to clean.

### Pass JSON through a stateless C function on every operation

Rejected. Repeated parsing obscures ownership and lifetime, increases overhead, and cannot express a coherent builder/plan/backing/view state machine.

### Stabilize the existing C++ class API

Rejected. C++ ABI stability depends on compiler and standard-library details and does not provide a sound Rust/Python/WASM boundary.

### Let each language implement the payload specification

Rejected. Independent normalization, digest, error, and layout implementations will drift and can assign different identities or values to the same document.

### Put enhanced FastDB behavior in the C-Two Rust SDK

Rejected. It would give Rust a hidden capability superset, leave Python behind, and invert the ownership boundary. Generic payload capability belongs in FastDB Core and every supported binding.

## Consequences

### Positive

- One canonical identity and binary meaning across languages.
- A clean FastDB/C-Two boundary that can support Toodle without coupling FastDB to Toodle.
- Explicit lifetime and final-backing truth rather than transport-specific wrappers.
- A correct `RecordEngine` name for AoS plus strided access.
- Bindings become ergonomic projections instead of semantic authorities.
- Intentional limitations are visible and actionable in the owner repository.

### Costs

- The change is intentionally breaking for current call-db and `ColumnEngine` consumers.
- The C++ Core must gain strict JSON/JCS/digest/codegen/error capabilities that currently live elsewhere or do not exist.
- Cross-language golden fixtures and ABI/lifetime tests become mandatory release work.
- C-Two must migrate to a wrapped FastDB sub-spec and remove any duplicated FastDB semantics after the Core contract closes.

## Compliance

An implementation conforms only if it satisfies the success criteria and clean-cut gates in the accepted design. A partial implementation may exist on the development branch, but no 0.2.0 release may advertise the portable payload foundation while a non-deferred criterion is missing.
