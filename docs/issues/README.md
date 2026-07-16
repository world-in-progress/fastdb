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
| [0001](0001-portable-payload-deferred-capabilities.md) | Open | Capabilities deliberately deferred beyond the 0.2.0 portable payload foundation. |
| [0002](0002-portable-payload-foundation-implementation-status.md) | Open (P1 in progress) | Current P1-P5 implementation gaps inside the non-deferrable 0.2.0 portable payload foundation; vendored primitives and the native test seam are present, but no portable payload API ships yet. |
