# fastdb Schema And C-Two Provider Architecture

> **Status:** Vision / implementation guide
> **Date:** 2026-05-18
> **Scope:** Define how fastdb should expose a neutral schema and codec provider surface that C-Two can consume without C-Two understanding fastdb internals.

## Decision Summary

fastdb should expose `fastdb.schema.v1` and provider-owned codec adapters; it should not become C-Two's CRM IDL and C-Two should not import fastdb core, Python bindings, TypeScript bindings, or schema parsers. C-Two should only receive opaque `CodecRef` data such as codec id, version, schema hash, and buffer capabilities.

`ColumnEngine` and `ObjectEngine` remain separate capability profiles. `ColumnEngine` is the columnar/batch profile and rejects REF fields. `ObjectEngine` is the object-graph profile and owns REF traversal, dependency ordering, and graph-oriented database construction. A schema can be valid while only some engine profiles are eligible.

`FastSerializer` is legacy. It can stay for compatibility and migration tests while users still depend on it, but new C-Two integration should not use its hybrid blob/buffer protocol as the foundation. New provider work should start from neutral schema export plus explicit columnar/object-graph codec profiles.

## Current Implementation Status

This document defines the target shape for the fastdb / C-Two integration. Today, fastdb has `@feature` classes, `LayerSchema`, `ColumnEngine`, `ObjectEngine`, Python/TypeScript bindings, legacy `FastSerializer`, `fastdb.schema.v1` export, strict portable schema validation, engine capability reports, opaque codec ref helpers, a dependency-neutral `FastdbCodecProvider` candidate/adapter pilot, and a fastdb-owned optional `CTwoFastdbCodecProvider` / `install_c_two_provider()` wrapper that imports C-Two only when a user explicitly installs/registers it. It still does not make C-Two import fastdb, it does not make `FastSerializer` the provider foundation, and it does not yet generate TypeScript payload codec helpers from c-two contract descriptors.

## Neutral Schema

`fastdb.schema.v1` should be a semantic descriptor derived from `@feature` classes and `LayerSchema`, but it must not serialize `LayerSchema` directly. `LayerSchema` currently mixes field semantics with runtime push plans, compiled function caches, numpy helper arrays, and table accessor state; these are implementation details and should not affect portable schema identity.

A descriptor should include:

- schema version
- feature identity, including module-qualified name when available
- stable field order
- field names
- symbolic field kinds such as `u8`, `u16`, `u32`, `i32`, `f32`, `f64`, `str`, `wstr`, `bytes`, `ref`, and `list`
- list element kind
- ref target identity for `ref` and `list[ref]`
- optional semantic annotations such as WKB geometry when a later layer introduces them

The descriptor should not use raw C++ enum numbers as public semantics. Python and TypeScript currently expose values that can diverge from C++ storage enum values, so the portable contract must use symbolic kinds and reserve native enum values for backend storage hints only.

## Capability Profiles

Schema export and engine eligibility should be separate artifacts. A feature graph can have a stable semantic schema even when a specific engine cannot build it.

`columnar.v1` eligibility should report:

- whether all fields are supported by `ColumnEngine`
- unsupported REF or `list[ref]` fields
- unsupported fixed-table variable-length fields such as `bytes` or `wstr` where applicable
- whether `STR` can use the current string-column path

`object_graph.v1` eligibility should report:

- whether REF and `list[ref]` targets resolve to `@feature` classes
- whether class-level circular REF dependencies are present
- whether schema construction requires unresolved forward references
- whether all field kinds can be represented by current object-graph construction

Capability diagnostics can change as engines improve, so they should not be folded into the semantic schema hash unless a profile-specific codec identity explicitly depends on them.

## Strict Export Mode

Portable schema export should be stricter than current Python runtime convenience. In strict export mode:

- REF targets must be explicit `@feature` classes.
- `list[ref]` targets must resolve to explicit `@feature` classes.
- unresolved forward refs fail export with a clear diagnostic.
- arbitrary annotated classes must not be silently treated as REF.
- duplicate feature identities must fail unless the caller provides an explicit namespace or aliasing strategy.

This tightening should first apply to schema/export/provider paths. Runtime engines can keep compatibility where needed until a later migration removes permissive behavior.

## C-Two Provider Shape

The fastdb C-Two provider should answer type-to-codec queries and return opaque codec refs. It should not require C-Two core to call fastdb schema APIs directly.

Conceptually:

```python
class FastdbCodecProvider:
    def candidates_for_type(self, typ, context):
        ...
```

For a columnar-eligible feature, the provider can return:

```json
{
  "id": "org.fastdb.columnar",
  "version": "1",
  "schema": "fastdb.schema.v1",
  "schema_sha256": "...",
  "capabilities": ["bytes", "buffer-view"]
}
```

For an object graph, it can return:

```json
{
  "id": "org.fastdb.object-graph",
  "version": "1",
  "schema": "fastdb.schema.v1",
  "schema_sha256": "...",
  "capabilities": ["bytes"]
}
```

The provider package owns encode/decode/from_buffer adapters. C-Two owns adapter invocation and transport. This keeps c-two neutral while allowing fastdb to evolve storage and schema internals.

Because C-Two expects concrete transfer adapters at runtime, fastdb should expose a small optional wrapper such as `install_c_two_provider()` that turns dependency-neutral candidates into C-Two `@transferable(codec_ref=...)` adapter classes when C-Two is present. That wrapper belongs in fastdb or a fastdb-owned integration package, not in C-Two core.

## Provider Codegen Integration

C-Two codegen and fastdb codegen should compose through descriptors rather than shared runtime imports. C-Two should read `c-two.contract.v1` and generate the RPC contract skeleton, typed call surface, route identity constants, and codec requirement declarations. fastdb should read the `fastdb.schema.v1` descriptors referenced by those codec requirements and generate provider-owned TypeScript payload helpers where the schema and engine profile are supported.

This means fastdb codegen should not parse CRM methods as a service IDL and should not decide route names, relay behavior, retries, or contract compatibility. Its job is narrower: map `fastdb.schema.v1` and codec ids such as `org.fastdb.columnar` or `org.fastdb.object-graph` to TypeScript feature/schema declarations, encode/decode helper placeholders, and clear unsupported diagnostics for schemas that cannot be represented by the TypeScript/WASM binding.

The generated artifacts should be deterministic and hash-aware. If a C-Two descriptor says a payload requires `schema_sha256 = abc...`, the fastdb helper should either generate or select helpers for exactly that schema hash, or fail with a diagnostic that names the missing codec id and schema hash. It should not fall back to a similarly named feature class or a runtime cache.

## Implementation Order

1. Add neutral schema descriptor dataclasses or plain dict builders in `fastdb4py`, with canonical JSON and hash tests.
2. Add strict schema export validation that rejects unresolved refs, non-`@feature` refs, and ambiguous identities.
3. Add engine capability analysis for ColumnEngine and ObjectEngine without changing engine behavior.
4. Update docs and examples to position FastSerializer as legacy.
5. Add a provider prototype that emits codec refs and adapters in fastdb-owned code.
6. Let C-Two consume only the provider output and adapter hooks; do not add fastdb imports to C-Two core.
7. Add provider-owned codegen helpers that consume `fastdb.schema.v1` descriptors referenced by C-Two codec requirements and emit TypeScript payload helpers or explicit unsupported stubs.

## Non-Goals

Do not make fastdb the CRM definition language.

Do not expose raw `LayerSchema` as the portable schema.

Do not make `FastSerializer` the new C-Two provider protocol.

Do not require C-Two to understand ColumnEngine, ObjectEngine, feature refs, or native enum values.
