# Publishing tested artifacts

`pypi_publish.yml` retains the existing trusted publisher identity: GitHub
`world-in-progress/fastdb`, workflow `pypi_publish.yml`, no environment.
Version tags are immutable.

## Normal publication

`release-artifacts.yml` builds the release candidate once on a package-changing
push to `main`. PRs and changes limited to documentation or release tooling
run tooling checks without allocating native, Python-wheel or WASM builders.
`tests.yml` independently applies its runtime validation scopes; release
tooling changes use the required `release_tools` gate. Changes to `tests.yml`
or runtime quality gates still require runtime validation.

`Run Tests` requires native Core/CTest/ABI validation on Linux x86_64 and
macOS arm64 (`native_tests`, two successful jobs), plus Windows x64
(`windows_native_tests`, one successful job on `windows-2022`). The Windows
job uses MSVC/Ninja and puts the built `fastdb.dll` directory on `PATH` before
CTest. Full Python tests require CPython 3.12 on Linux (`python_tests`) and
Windows x64 (`windows_python_tests`); the experimental Linux 3.13t job cannot
substitute for either required proof. The Windows jobs follow the same path
scopes as their existing native/Python counterparts and feed the required
`test` aggregate. Windows ARM64 remains covered by release builds.

A version-tag push to `pypi_publish.yml` locates successful Release Artifacts
and complete Run Tests proofs for that exact source SHA. It downloads the
unexpired `release-part-python` artifact by run ID and artifact ID. Before
upload, it checks the fragment's source/version, every file's size and SHA-256,
and the complete wheel/sdist inventory. The publish path has no build job.
Missing or expired artifacts cause failure; a metadata-only green run cannot
replace the candidate or a complete runtime proof. Release preparation must
produce both proofs and the candidate before tagging.

Manual retry uses the current controller from `main`, an existing matching tag,
and `operation=publish`. It performs the same artifact checks and does not
rebuild. npm and Rust already download their tested Release Artifacts archives
and retain their existing workflow inputs.

## Missing-wheel backfill

Dispatch `pypi_publish.yml` from `main` with `operation=backfill` and an existing
`tag`, for example `v0.2.2`. This is the manual default. The planner:

1. Resolves the tag and its complete exact-source CI proofs.
2. Refreshes public PyPI metadata and computes missing ABI/platform slots.
3. Rejects an expired partial release before downloading files or starting
   builders. A complete release can finish as a no-op even after expiry.
4. Verifies the published sdist against the tag, and downloads existing
   required files with PyPI size/SHA-256 verification.
5. Allocates only platforms with missing slots and passes their exact
   cibuildwheel identifiers. It never rebuilds the sdist.
6. Rejects missing planned files, extra builds of existing slots, changed
   reusable files, or an incomplete combined inventory. It refreshes PyPI
   metadata before staging only still-missing filenames with `skip-existing`.

The pending policy is six ABIs (`cp310`, `cp311`, `cp312`, `cp313`, `cp314`,
`cp314t`) on the existing five platforms (macOS arm64/x86_64,
manylinux_2_28 x86_64/aarch64, Windows amd64), plus the five Python 3.11+
ABIs on Windows ARM64. NumPy's Windows ARM64 wheels require Python 3.11+;
`cp310-win_arm64` is explicitly excluded. All 35 wheels and one sdist are
required. Both normal and
backfill builds use `python-distributions.yml`. Linux aarch64 uses the native
`ubuntu-24.04-arm` runner. macOS Intel cross-compiles on `macos-15` arm64 and
runs installed-wheel checks through Rosetta. Each newly built wheel runs the
read/write smoke check and payload tests through cibuildwheel.

Windows ARM64 uses native `windows-11-arm` for wheel execution. The same
runner also builds and tests the native Core and Rust consumers, retains a
matching Core SDK bundle and checks ABI exports. Binary PE machine checks
reject x64 extensions or DLLs labeled as ARM64. These pending jobs have not
yet established native Windows ARM64 support; see the
[preparation and 0.2.2 blocker record](windows-arm64.md).

## Local validation of artifact reuse (2026-10-17)

No push, Actions dispatch, upload, version change or tag change was performed
for this workflow update. Workflow syntax and CI contract checks run locally,
along with 124 targeted unit tests (39 release, 17 distribution, 9 scope,
18 projection-contract and 41 clean-cut tests). `actionlint`, the P2/P4 quality
gates, and whitespace checks passed. Release-version and P5 repository checks
use a clean temporary Git snapshot of the pending changes because they require
committed source; the working checkout remains uncommitted. The scope regressions exercise real YAML
filters, PR/main build guards, cross-run artifact wiring, required lightweight
checks, selective identifiers and empty-backfill guards.

An offline replay used cached real 0.2.2 files: the original 20 wheels plus
sdist and the ten cp310/cp311 wheels. All 756 files in the cached source archive
matched immutable `v0.2.2` (`6be54b2aea078d4ba283f33d30289b447c32750a`);
309 published sdist source files matched that source. The planner reused 21
files, selected exactly ten wheels, requested no sdist build, and the combined
31-file inventory passed. Staging selected exactly ten files. A subsequent
complete-registry snapshot allocated zero builders and zero upload files.
This replay exercised planning, hash checks, inventory and staging with zero
network requests, compiler invocations or uploads; hosted execution remains
unrun. Local evidence is in `build/ci-release-reuse/offline-replay.json` and
`offline-replay.log` (ignored build outputs).
