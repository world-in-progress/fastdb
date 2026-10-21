# Windows ARM64 preparation (2026-10-21)

This preserves the local preparation evidence collected before 0.2.3.
The native Windows ARM64 gates subsequently passed for source
`abba785ef2a7d3fbee1bbc608393a3e6c7b042c4`: 40 C++ tests, 117 C ABI exports,
native Rust source/package/system linkage, and five installed wheels with
five read/write smoke checks and 170 payload tests. See the
[0.2.3 release record](0.2.3.md) for hosted CI and publication evidence.
The immutable `v0.2.2` tag remains unchanged. Statements below about pending
execution describe the preparation stage.

## Pending build and validation

`setup.py` selects Visual Studio `x64`, `ARM64` or `Win32` from the target
Python platform; an explicit wheel target takes precedence over the host.
Ninja receives no unsupported `-A` argument and uses the developer environment.
Unknown Windows targets fail before CMake is invoked.

`python-distributions.yml` adds native `windows-11-arm` with `CIBW_ARCHS=ARM64`.
The existing five platforms retain six ABIs; Windows ARM64 adds `cp311`,
`cp312`, `cp313`, `cp314`, `cp314t`. cibuildwheel 3.4.0 also lists an ARM64
3.10 interpreter, but NumPy's Windows ARM64 wheels start in the Python 3.11+
release line. The explicit `cp310-win_arm64` exclusion prevents promising
an installation that would compile NumPy on an ordinary user's machine.
Windows builds require binary NumPy dependencies.

Each new wheel must pass the installed-wheel read/write smoke and payload
suite. A complete inventory now requires 35 wheels plus one sdist. Missing
Windows ARM64 slots allocate only one native runner for those five wheels;
existing files and the sdist are reused. The publication path continues to
download tested exact-source CI artifacts without rebuilding.

The Release Artifacts native matrix also adds an
`aarch64-pc-windows-msvc` Core SDK bundle. Its job requires native Windows
and Rust host architectures, configures the ARM64 MSVC environment and Ninja,
runs C++ tests, inspects the C ABI exports, tests Rust source linkage and
isolated packaged/system linkage, and retains the matching DLL/import library.
The release manifest requires this Core archive and every new wheel. PE machine
checks independently reject x64 binaries mislabeled as ARM64.

## Why 0.2.2 cannot take these wheels from its original source

PyPI's [14-day rule](https://blog.pypi.org/posts/2026-07-22-releases-now-reject-new-files-after-14-days/)
does not block the version by age as of this record. The first surviving file
was uploaded at `2026-10-16T12:59:52.517967Z`; its observed deadline is
**2026-10-30 20:59:52 +08:00**. PyPI exposes no definitive release-open API.

The original tag is `6be54b2aea078d4ba283f33d30289b447c32750a`. Its `setup.py:78`
unconditionally appends `['-A', 'x64']`. CMake's explicit generator architecture
[overrides an environment default](https://cmake.org/cmake/help/latest/envvar/CMAKE_GENERATOR_PLATFORM.html), so the ARM64 Python target cannot use this
build script to select ARM64. A source fix is necessary; patching the build
source while labeling it as the original tag would break source identity.

The local preflight checked all 756 archived tag files and compared 309
published sdist source files. The sdist is 1,063,812 bytes with SHA-256
`44766d92153deba407b8e53c31f5889a67fbe64cc599d7b4587bf917ea40ab49`.
PyPI has 30 wheels plus that sdist; exactly five ARM64 slots are missing under
the pending policy. The planner rejects the old source before archive
downloads or build allocation, with a message requiring a new release.
Evidence is in the ignored `build/windows-arm64-validation/0.2.2-preflight.json`.

The architecture fix must ship in a new version (normally 0.2.3). No version
bump, tag movement, push, Actions dispatch or upload is part of this preparation.
The existing trusted publisher remains `pypi_publish.yml` with no environment.

## Local checks and remaining gate

The local Python suite passed 344 tests. Another 141 targeted release,
inventory, Windows-build, ABI-symbol, workflow-scope and policy unit tests
passed, together with `actionlint`, P2/P4 gates, compileall and whitespace
checks. A local macOS sdist/wheel build passed after explicitly adding the
installed SDK's libc++ headers through command-local `CXXFLAGS`; the active
Command Line Tools initially could not compile even `#include <algorithm>`.
No global compiler configuration or license settings were changed.
The installed local macOS wheel passed all 34 payload tests. Version coherence
and the P5 repository policy passed in a clean temporary Git snapshot; the
working checkout remains uncommitted. An auxiliary read-only audit failed to
launch (`harness-error`) and is not counted as review or test evidence.

Regressions exercise target-Python architecture selection, Ninja behavior,
binary machine mismatches, all new missing-slot failures, five-wheel-only
backfill planning, no sdist rebuild, and old-source rejection before builds.
Workflow syntax and existing CI quality gates are checked locally.

Native Windows ARM64 compilation, DLL loading, installed-wheel payload tests
and Rust execution still require the new native runner. Their success is the
remaining support gate, not implied by local command simulation.

References: [cibuildwheel Windows ARM64](https://cibuildwheel.pypa.io/en/v3.4.0/platforms/#windows-arm64-builds),
[NumPy 2.3 release](https://numpy.org/doc/2.3/release/2.3.0-notes.html),
[GitHub ARM64 runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
