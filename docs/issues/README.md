# FastDB Design Issues

This directory tracks known limitations and follow-up work that are intentionally outside an accepted FastDB milestone. These files are owner-side engineering issues, not vague roadmap notes.

Each issue states:

- the exact current limit;
- why the accepted milestone does not implement it;
- what remains supported despite the limit;
- objective closure criteria.

A limitation cannot be introduced only in code or release notes. It must be recorded here before merge and linked from the governing design or ADR.

| Issue | Status | Scope |
|---|---|---|
| [0001](0001-portable-payload-deferred-capabilities.md) | Open | Capabilities deliberately deferred beyond the 0.2.0 portable payload foundation. |
