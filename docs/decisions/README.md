# FastDB Architecture Decisions

Architecture decisions define FastDB-owned contracts and remain authoritative until superseded by a later accepted decision.

| ADR | Status | Decision |
|---|---|---|
| [0001](0001-portable-payload-core-authority.md) | Accepted | Make the C++ Core the sole portable-payload authority and replace the public call-db path in 0.2.0. |

## Status meanings

- **Proposed** — under review and not yet authoritative.
- **Accepted** — normative for new design and implementation work.
- **Superseded** — retained for history; the replacing ADR is linked.
- **Rejected** — considered but not adopted.

Changing an accepted schema, identity, ABI, lifetime, backing, or repository-ownership boundary requires a new ADR.
