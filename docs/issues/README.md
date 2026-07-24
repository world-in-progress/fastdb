# FastDB Design Issues

This directory tracks both known limitations intentionally outside an accepted FastDB milestone and temporary implementation gaps inside an accepted milestone. These files are owner-side engineering issues, not vague roadmap notes.

Each issue states:

- the exact current limit;
- why the current implementation or accepted milestone does not implement it yet;
- what remains supported despite the limit;
- objective closure criteria.

A limitation cannot be introduced only in code or release notes. It must be recorded here before merge and linked from the governing design or ADR.

| Issue | Status | Scope |
|---|---|---|
| [0001](0001-portable-payload-deferred-capabilities.md) | Open (D1 closed; D2-D5 open) | D1 graph direct construction is locally closed with executable P3 evidence; segmented backing, streaming authoring, additional guaranteed platforms, and native Node/Go projections remain deferred. |
| [0002](0002-portable-payload-foundation-implementation-status.md) | Open (P1-P5 and C-Two local handoff proven; hosted/official release pending) | Frozen P2/P3 traceability and historical ABI-105, locally complete exact ABI-117 projections/codegen/hostile-output evidence, local P5 clean-cut readiness, retained FastDB package manifest plus C-Two isolated-consumer/matrix handoff, and remaining hosted/official-release evidence. |
| [0003](0003-legacy-swig-diagnostics.md) | Closed | P5 Task 4 removed the seven-warning legacy SWIG surface and installed a zero-warning package gate; hosted execution remains tracked by Issue 0002. |

Small retained local-candidate manifests live under [`evidence/`](evidence/README.md). They are evidence records, not package distribution.
