# Issue 0003: Legacy SWIG Diagnostics

- **Status:** Open
- **Opened:** 2026-07-17
- **Owner:** FastDB legacy Python binding / P5 clean cut
- **Related status issue:** [Issue 0002](0002-portable-payload-foundation-implementation-status.md)
- **Governing decision:** [ADR-0001](../decisions/0001-portable-payload-core-authority.md)

## Current limit

Building the current 0.1.x `fastdb4py` wheel succeeds, but SWIG emits exactly
these diagnostics from `fastcarto/fastdb/include/fastdb.h`:

- SWIG Warning 325 at line 582 for nested `TileBox`;
- SWIG Warning 325 at line 588 for nested `HandleTileAction`;
- SWIG Warning 325 at line 595 for nested `TakeResult`;
- SWIG Warning 325 at line 622 for nested `TileDataHandle`;
- SWIG Warning 325 at line 631 for nested `TileDbBox`;
- SWIG Warning 325 at line 637 for the second nested `TakeResult`; and
- SWIG Warning 451 at line 206 for a settable `const char *` member whose
  ownership SWIG cannot determine.

No new SWIG warning joins this accepted set implicitly. A build that emits a
different warning, a warning at a different location, or an additional instance
is a regression until reviewed and recorded here.

## Reason

The warnings originate in the legacy 0.1.x SWIG input surface. SWIG does not
project the nested declarations named above, and the generated setter cannot
prove ownership for the character pointer. The P1 portable-payload API is a
separate pure-C surface in `fastdb_payload.h`; it is not added to the legacy
SWIG interface and does not depend on these ignored declarations.

## Supported behavior and impact

- The current Python sdist and wheel still build, and the wheel contains the
  linked native FastDB library.
- The P1 `fastdb.payload.v1` compiler/query Core and its C/C++ API are
  unaffected by these diagnostics.
- The warnings remain noisy and can conceal a newly introduced binding
  diagnostic, so package evidence must compare the emitted set exactly rather
  than treating arbitrary SWIG output as expected.
- This issue does not authorize extending the legacy call-db or
  `ColumnEngine` API. Those surfaces remain migration inputs for the planned
  0.2.0 clean cut.

## Owner and dependencies

FastDB owns the cleanup. It belongs with the legacy Python binding and the P5
clean-cut work unless one of the listed warnings begins blocking a required
package build earlier. Resolution depends on deciding whether each legacy
declaration is removed with the obsolete surface or retained through an
explicit, ownership-correct SWIG typemap/API shape.

## Closure criteria

Close this issue only when all of the following are true:

- the P5 public-surface decision has removed each obsolete declaration or made
  its intended Python projection explicit;
- SWIG emits none of the seven diagnostics above during a clean wheel build;
- the replacement typemap/API, if any, has ownership and lifetime tests;
- Python tests, compileall, sdist/wheel builds, and package inventory checks
  pass from a clean tree; and
- CI rejects every newly introduced SWIG diagnostic rather than silently
  expanding an allowlist.
