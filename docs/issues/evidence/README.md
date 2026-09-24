# FastDB Local-Candidate Evidence

This directory retains small, reviewable evidence for the FastDB-owned part of the C-Two Phase 0B local release candidate. It does not contain package archives, native libraries, generated projects, build trees, or a published release.

[`fastdb-local-candidate-manifest.v1.json`](fastdb-local-candidate-manifest.v1.json) is the exact canonical manifest produced from FastDB implementation commit `7eb74734926bd8fe911229eee9744a6dd8172487`. Its SHA-256 is `9a1c7c83dca16237257dcc50d5917f10d032e02ffceae1278ae331d101b69d32`. The manifest contains only FastDB-owned artifacts and records their complete archive inventories, build commands, source commit, platform, toolchains, byte counts, and SHA-256 values.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| Core/C ABI bundle | 2,314,708 | `eaea8ff354da069957fefbed07aab268f6e343091b7400d4d12be9ec515fff58` |
| CPython 3.10 wheel | 1,132,548 | `1d8ff0e41a719cdb26d8ceeec6f0b5629ac6bcf6ed8a52b477007f39e815e4b4` |
| Current-interpreter wheel | 1,132,719 | `bbf45f30faa5e80fedf6f0523efb77443db43e35385eb12f5c2867214dabebdd` |
| Python sdist | 1,067,800 | `421852e7dd23be321c66b1dbf0675962f5332e37a74cde805d21c183da679fa9` |
| `fastdb` Rust crate | 18,008 | `43f0915631a63fe40711a3945fcbf1c5489c4db4536888dc79acc229b661ca9e` |
| `fastdb-sys` Rust crate | 7,999 | `a57bce3c3564356dcbabc1c0e3905d62eb859147fc35279c33b4f151fa460367` |
| `fastdb4ts` npm tarball | 639,738 | `219fcda8ae71ff97a8ddc0cf11a1edf6c2d7299b5371c32195f5bbd781080a9a` |

The artifacts retain source metadata `fastdb4py==0.1.22`, `fastdb==0.1.22`, `fastdb-sys==0.1.22`, and `fastdb4ts==0.0.3`. Those versions were not republished. Local candidate identity is therefore the source commit plus the exact manifest and artifact hashes, never the reused version string alone.

C-Two implementation commit `bf6f5c950959bcd2723cf3c7bfe772c9ee91dc02` consumed this exact candidate into its own manifest and passed isolated Rust, Python 3.10/current, and Node consumers, the 18-row Rust/Python direct/relay matrix, and the 12-row generated TypeScript Node matrix. That downstream evidence proves a local owner-boundary handoff; it does not turn these files into official FastDB packages or hosted-platform evidence.
