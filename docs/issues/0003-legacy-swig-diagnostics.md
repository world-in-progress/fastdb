# Issue 0003: Legacy SWIG Diagnostics

- **Status:** Closed
- **Opened:** 2026-07-17
- **Owner:** FastDB legacy Python binding / P5 clean cut
- **Related status issue:** [Issue 0002](0002-portable-payload-foundation-implementation-status.md)
- **Governing decision:** [ADR-0001](../decisions/0001-portable-payload-core-authority.md)

## Historical limit

Before P5 Task 4, building the 0.1.x `fastdb4py` wheel succeeded but SWIG
emitted exactly these diagnostics from
`fastcarto/fastdb/include/fastdb.h`:

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

## Historical behavior and impact

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
package build earlier.

## P5 Task 0 selected resolution

The accepted
[P5 clean-cut design](../superpowers/specs/2026-07-23-portable-payload-clean-cut-design.md)
and
[implementation plan](../superpowers/plans/2026-07-23-portable-payload-clean-cut.md)
choose the following owner-correct cleanup:

- remove the scratch/final-backing classes and SWIG wrappers that exist only
  for the obsolete call-db path;
- keep the native tile APIs available to C++ while excluding their already
  unsupported nested declarations from SWIG parsing;
- keep the internal `utf8_view_t` sequence bridge while preventing SWIG from
  generating a writable setter for its borrowed `const char *` member; and
- replace the exact-seven-warning allowance with a zero-warning package gate
  that rejects any matched SWIG diagnostic.

At Task 0 this was a design decision rather than closure evidence: the source
was unchanged at starting commit
`9d86c171eda1fe107c3519ce040ca2ec417167f9`, and the issue remained open until
the clean package proof below passed.

## P5 Task 4 closure evidence

P5 Task 4 closes the issue without adding a replacement binding-owned
allocator, compatibility alias, or warning allowlist:

- `TileBoxTake` and `FastVectorTileDb` remain compiled public C++ APIs, but
  their nested declarations are outside SWIG parsing;
- the internal `utf8_view_t` sequence bridge remains, while
  `utf8_view_t::data` is intentionally not settable from Python because it is
  a borrowed `const char *`;
- the orphaned scratch/final-backing classes and
  `FastVectorDbBuild::postToFinalBacking` are removed from the native and
  Python surfaces rather than hidden behind deprecated wrappers; and
- `check_swig_diagnostics` now accepts only zero matched SWIG diagnostics.
  Each of the former seven warnings and an arbitrary new warning has a
  negative checker regression.

The fresh package command completed with zero matched SWIG diagnostics. The
real package inventory gate passed and found exactly these three native wheel
members:

```text
fastdb4py/core/_fastdb4py.so
fastdb4py/core/libfastdb.dylib
fastdb4py/core/libfastdb4py.dylib
```

The resulting local artifacts were:

```text
d60df86e14f9514f9e6aaae666d6bac072fbbe459dfcedef026c04397a844f9a  fastdb4py-0.1.22-cp314-cp314t-macosx_26_0_arm64.whl
de2a8f95376719017d18e75edf52adef7ab4c7c42a8902baf06c24fb3054e424  fastdb4py-0.1.22.tar.gz
```

The complete Python source suite passes 338 tests, compileall succeeds, and
the package-checker unit suite passes 16 tests. These are local macOS arm64
results. No hosted Linux/Windows package result, version bump, tag,
publication, or release is implied.

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

All criteria are satisfied locally by the P5 Task 4 evidence above. No
replacement typemap or writable borrowed-pointer API was introduced, so the
conditional ownership/lifetime-test criterion does not apply. Hosted
execution remains pending and is tracked by Issue 0002 rather than keeping
this source-level SWIG cleanup open.

The scoped implementation is commit `ad3e4d6`; the frozen descriptor-padding
proof correction is `099a890`. Issue 0002 records the exact-range same-agent
review and its non-independent status.
