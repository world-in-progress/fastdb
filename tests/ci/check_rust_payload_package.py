#!/usr/bin/env python3
"""Exercise packaged Rust payload inventories and relocated system linking."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[2]
RUST_ROOT = ROOT / "bindings/rust"
PACKAGE_VERSION = "0.2.0"
PACKAGE_REQUIRED = {
    "fastdb-sys": {
        ".cargo_vcs_info.json",
        "Cargo.lock",
        "Cargo.toml",
        "Cargo.toml.orig",
        "LICENSE",
        "README.md",
        "build.rs",
        "src/lib.rs",
    },
    "fastdb": {
        ".cargo_vcs_info.json",
        "Cargo.lock",
        "Cargo.toml",
        "Cargo.toml.orig",
        "LICENSE",
        "README.md",
        "src/builder.rs",
        "src/codegen.rs",
        "src/lib.rs",
        "src/runtime.rs",
    },
}
FORBIDDEN_PARTS = {
    "target",
    "fastcarto",
    ".git",
    ".github",
    "tests",
    "build",
    "__pycache__",
    ".pytest_cache",
}


class CheckError(RuntimeError):
    """The Rust package/link boundary is incomplete."""


def run(
    command: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
    )
    if completed.returncode != 0:
        rendered = " ".join(command)
        raise CheckError(
            f"command failed with exit {completed.returncode}: {rendered}\n"
            f"{completed.stdout}{completed.stderr}"
        )
    return completed.stdout


def package_inventory(package: str) -> set[str]:
    with tempfile.TemporaryDirectory(
        prefix="fastdb-rust-inventory-"
    ) as temporary:
        archives = package_archives(Path(temporary) / "packages")
        return check_archive(package, archives[package])


def check_inventory(package: str, names: set[str]) -> None:
    try:
        required = PACKAGE_REQUIRED[package]
    except KeyError as error:
        raise CheckError(f"unknown Rust payload package {package!r}") from error
    missing = sorted(required - names)
    if missing:
        raise CheckError(f"{package} package inventory is missing {missing}")
    debris = sorted(
        name
        for name in names
        if (
            PurePosixPath(name).is_absolute()
            or "\\" in name
            or ".." in PurePosixPath(name).parts
            or any(
                part in FORBIDDEN_PARTS for part in PurePosixPath(name).parts
            )
            or PurePosixPath(name).name == ".DS_Store"
            or PurePosixPath(name).suffix
            in {".pyc", ".pyo", ".o", ".a", ".so", ".dylib"}
        )
    )
    if debris:
        raise CheckError(f"{package} package inventory contains debris {debris}")


def strip_archive_root(names: list[str], root: str) -> set[str]:
    if len(names) != len(set(names)):
        raise CheckError("Rust crate archive contains duplicate member names")
    expected_prefix = f"{root}/"
    outside = sorted(
        name
        for name in names
        if name != root and not name.startswith(expected_prefix)
    )
    if outside:
        raise CheckError(
            f"Rust crate archive contains members outside {root}/: {outside}"
        )
    relative_names = [
        name[len(expected_prefix) :]
        for name in names
        if name.startswith(expected_prefix)
    ]
    invalid: list[str] = []
    canonical_names: list[str] = []
    for name in relative_names:
        path = PurePosixPath(name)
        canonical = path.as_posix()
        if (
            not name
            or "\\" in name
            or path.is_absolute()
            or ".." in path.parts
            or "." in path.parts
            or canonical != name
            or "//" in name
        ):
            invalid.append(name)
        canonical_names.append(canonical)
    if invalid:
        raise CheckError(
            f"Rust crate archive contains non-canonical paths: {invalid}"
        )
    if len(canonical_names) != len(set(canonical_names)):
        raise CheckError(
            "Rust crate archive contains colliding normalized paths"
        )
    return set(canonical_names)


def reject_special_members(members: list[tarfile.TarInfo]) -> None:
    special = [
        member.name
        for member in members
        if not (member.isfile() or member.isdir())
    ]
    if special:
        raise CheckError(
            f"Rust crate archive contains non-file members: {special}"
        )


def check_normalized_manifest(package: str, contents: bytes) -> None:
    try:
        source = contents.decode("utf-8")
    except UnicodeDecodeError as error:
        raise CheckError(
            f"{package} normalized Cargo.toml is not UTF-8"
        ) from error
    if not re.search(
        rf"(?m)^name\s*=\s*\"{re.escape(package)}\"\s*$", source
    ):
        raise CheckError(
            f"{package} normalized Cargo.toml has the wrong package name"
        )
    if not re.search(
        rf"(?m)^version\s*=\s*\"{re.escape(PACKAGE_VERSION)}\"\s*$", source
    ):
        raise CheckError(
            f"{package} normalized Cargo.toml has the wrong package version"
        )
    if re.search(r"(?m)^publish\s*=\s*(?:false|\[\s*\])\s*$", source):
        raise CheckError(f"{package} archive disables registry publication")
    if package == "fastdb":
        dependency = re.search(
            r"(?ms)^\[dependencies\.fastdb-sys\]\s*$"
            r"(?P<body>.*?)(?=^\[|\Z)",
            source,
        )
        if dependency is None:
            raise CheckError(
                "fastdb normalized Cargo.toml is missing fastdb-sys dependency"
            )
        body = dependency.group("body")
        if not re.search(
            rf"(?m)^version\s*=\s*\"={re.escape(PACKAGE_VERSION)}\"\s*$",
            body,
        ):
            raise CheckError(
                "fastdb packaged dependency must require "
                f"fastdb-sys ={PACKAGE_VERSION}"
            )
        if re.search(r"(?m)^path\s*=", body):
            raise CheckError(
                "fastdb normalized packaged dependency must not retain a "
                "sibling checkout path"
            )


def check_archive(package: str, archive_path: Path) -> set[str]:
    expected_name = f"{package}-{PACKAGE_VERSION}.crate"
    if archive_path.name != expected_name:
        raise CheckError(
            f"{package} archive must be named {expected_name}, "
            f"received {archive_path.name}"
        )
    root = f"{package}-{PACKAGE_VERSION}"
    try:
        with tarfile.open(archive_path, "r:gz") as archive:
            members = archive.getmembers()
            reject_special_members(members)
            names = strip_archive_root(
                [member.name for member in members], root
            )
            check_inventory(package, names)
            manifest = archive.extractfile(f"{root}/Cargo.toml")
            if manifest is None:
                raise CheckError(
                    f"{package} archive is missing Cargo.toml contents"
                )
            check_normalized_manifest(package, manifest.read())
    except (tarfile.TarError, OSError) as error:
        raise CheckError(
            f"could not inspect {package} archive {archive_path}: {error}"
        ) from error
    return names


def package_archives(
    destination: Path,
    *,
    source_date_epoch: int | None = None,
) -> dict[str, Path]:
    if destination.exists():
        raise CheckError(
            f"Rust package destination must be absent: {destination}"
        )
    destination.mkdir(parents=True)
    environment = os.environ.copy()
    if source_date_epoch is not None:
        environment.update(
            {
                "SOURCE_DATE_EPOCH": str(source_date_epoch),
                "TZ": "UTC",
            }
        )
    target_dir = destination / ".cargo-target"
    result: dict[str, Path] = {}
    try:
        for package in ("fastdb-sys",):
            run(
                [
                    "cargo",
                    "package",
                    "--manifest-path",
                    str(RUST_ROOT / package / "Cargo.toml"),
                    "--allow-dirty",
                    "--no-verify",
                    "--target-dir",
                    str(target_dir),
                ],
                cwd=ROOT,
                env=environment,
            )
            produced = target_dir / "package" / (
                f"{package}-{PACKAGE_VERSION}.crate"
            )
            if not produced.is_file():
                raise CheckError(
                    f"cargo did not produce expected archive {produced}"
                )
            destination_archive = destination / produced.name
            shutil.copy2(produced, destination_archive)
            check_archive(package, destination_archive)
            result[package] = destination_archive

        index = destination / ".local-index"
        _write_local_index(index, result["fastdb-sys"])
        run(
            [
                "cargo",
                "package",
                "--manifest-path",
                str(RUST_ROOT / "fastdb" / "Cargo.toml"),
                "--allow-dirty",
                "--no-verify",
                "--target-dir",
                str(target_dir),
                "--index",
                index.resolve(strict=True).as_uri(),
                "--config",
                'source.crates-io.replace-with="fastdb-local"',
                "--config",
                (
                    'source.fastdb-local.registry="'
                    + index.resolve(strict=True).as_uri()
                    + '"'
                ),
            ],
            cwd=ROOT,
            env=environment,
        )
        package = "fastdb"
        produced = target_dir / "package" / (
            f"{package}-{PACKAGE_VERSION}.crate"
        )
        if not produced.is_file():
            raise CheckError(
                f"cargo did not produce expected archive {produced}"
            )
        destination_archive = destination / produced.name
        shutil.copy2(produced, destination_archive)
        check_archive(package, destination_archive)
        result[package] = destination_archive
    finally:
        shutil.rmtree(target_dir, ignore_errors=True)
        shutil.rmtree(destination / ".local-index", ignore_errors=True)
    return result


def _index_relative_path(package: str) -> Path:
    lowered = package.lower()
    if len(lowered) == 1:
        return Path("1") / lowered
    if len(lowered) == 2:
        return Path("2") / lowered
    if len(lowered) == 3:
        return Path("3") / lowered[0] / lowered
    return Path(lowered[:2]) / lowered[2:4] / lowered


def _write_local_index(index: Path, sys_archive: Path) -> None:
    if index.exists():
        raise CheckError(f"temporary Cargo index must be absent: {index}")
    index.mkdir(parents=True)
    download = index / "crates"
    download.mkdir()
    (index / "config.json").write_text(
        json.dumps(
            {
                "dl": download.resolve(strict=True).as_uri(),
                "api": None,
            },
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    entry = index / _index_relative_path("fastdb-sys")
    entry.parent.mkdir(parents=True)
    checksum = hashlib.sha256(sys_archive.read_bytes()).hexdigest()
    entry.write_text(
        json.dumps(
            {
                "name": "fastdb-sys",
                "vers": PACKAGE_VERSION,
                "deps": [],
                "cksum": checksum,
                "features": {},
                "yanked": False,
                "links": "fastdb_payload_v1",
            },
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    git_environment = os.environ.copy()
    git_environment.update(
        {
            "GIT_AUTHOR_NAME": "FastDB local candidate",
            "GIT_AUTHOR_EMAIL": "local-candidate@invalid",
            "GIT_COMMITTER_NAME": "FastDB local candidate",
            "GIT_COMMITTER_EMAIL": "local-candidate@invalid",
            "GIT_AUTHOR_DATE": "@1 +0000",
            "GIT_COMMITTER_DATE": "@1 +0000",
        }
    )
    run(["git", "init", "-q"], cwd=index, env=git_environment)
    run(["git", "add", "."], cwd=index, env=git_environment)
    run(
        ["git", "commit", "-q", "-m", "local index"],
        cwd=index,
        env=git_environment,
    )


def extract_package_archives(
    archives: dict[str, Path], destination: Path
) -> dict[str, Path]:
    if destination.exists():
        raise CheckError(
            f"Rust extraction destination must be absent: {destination}"
        )
    destination.mkdir(parents=True)
    extracted: dict[str, Path] = {}
    for package in PACKAGE_REQUIRED:
        try:
            archive_path = archives[package]
        except KeyError as error:
            raise CheckError(
                f"Rust archive set is missing {package}"
            ) from error
        check_archive(package, archive_path)
        root = f"{package}-{PACKAGE_VERSION}"
        with tarfile.open(archive_path, "r:gz") as archive:
            archive.extractall(destination)
        crate_root = destination / root
        if not crate_root.is_dir():
            raise CheckError(
                f"Rust archive did not extract expected root {root}"
            )
        extracted[package] = crate_root
    return extracted


def locate_system_library(build_dir: Path, platform: str = sys.platform) -> Path:
    names = {
        "darwin": ("libfastdb.dylib",),
        "linux": ("libfastdb.so",),
        "win32": ("fastdb.lib",),
    }
    key = "linux" if platform.startswith("linux") else platform
    if key not in names:
        raise CheckError(
            f"unsupported Rust system-link test platform: {platform}"
        )
    try:
        root = build_dir.resolve(strict=True)
    except FileNotFoundError as error:
        raise CheckError(
            f"native build directory does not exist: {build_dir}"
        ) from error
    candidates = sorted(
        (
            candidate.resolve(strict=True)
            for name in names[key]
            for candidate in root.rglob(name)
            if candidate.is_file() and not candidate.is_symlink()
        ),
        key=os.fspath,
    )
    if len(candidates) != 1:
        raise CheckError(
            f"expected exactly one system FastDB library under {root}, "
            f"found {len(candidates)}: {[str(item) for item in candidates]}"
        )
    return candidates[0]


def _toml_path(path: Path) -> str:
    return json.dumps(os.fspath(path.resolve(strict=True)))


def write_sys_consumer(root: Path, sys_crate: Path) -> Path:
    consumer = root
    (consumer / "src").mkdir(parents=True)
    (consumer / "Cargo.toml").write_text(
        f"""[package]
name = "fastdb-sys-packaged-mode-smoke"
version = "0.0.0"
edition = "2024"
publish = false

[workspace]

[dependencies]
fastdb-sys = {{ path = {_toml_path(sys_crate)} }}
""",
        encoding="utf-8",
    )
    (consumer / "src/main.rs").write_text(
        """fn main() {
    let version = unsafe { fastdb_sys::fdb_payload_v1_abi_version() };
    assert_eq!(version, 1);
}
""",
        encoding="utf-8",
    )
    return consumer


def write_consumer(
    root: Path,
    fastdb_crate: Path,
    sys_crate: Path,
) -> Path:
    consumer = root
    (consumer / "src").mkdir(parents=True)
    (consumer / "Cargo.toml").write_text(
        f"""[package]
name = "fastdb-system-link-smoke"
version = "0.0.0"
edition = "2024"
publish = false

[workspace]

[dependencies]
fastdb = {{ path = {_toml_path(fastdb_crate)} }}

[patch.crates-io]
fastdb-sys = {{ path = {_toml_path(sys_crate)} }}
""",
        encoding="utf-8",
    )
    (consumer / "src/main.rs").write_text(
        r'''use fastdb::{
    ArtifactKind, BuildPolicy, Builder, CodegenOptions, CodegenTarget,
    CompiledSpec, OpenOptions, Payload, Profile,
};

fn main() {
    let source = br#"{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"value","cardinality":"one","type":{"kind":"u8","nullable":false}}],"components":[]}"#;
    let spec = CompiledSpec::compile(source).expect("system Core compile");
    assert_eq!(spec.profile().expect("profile"), Profile::RecordV1);
    assert_eq!(spec.entry_count().expect("entry count"), 1);
    assert_eq!(spec.component_count().expect("component count"), 0);
    assert_eq!(spec.sha256().expect("digest").len(), 32);
    let generated = spec
        .generate(CodegenTarget::Rust, &CodegenOptions::default())
        .expect("system Core codegen");
    assert_eq!(generated.len().expect("artifact count"), 1);
    let artifact = generated.artifact(0).expect("artifact");
    assert_eq!(artifact.kind, ArtifactKind::Source);
    assert!(artifact.relative_path.ends_with(".rs"));
    assert!(!artifact.bytes.is_empty());
    assert!(artifact.sha256.iter().any(|byte| *byte != 0));

    let mut builder = Builder::create(&spec).expect("system Core builder");
    builder.entry_begin(0, 1).expect("entry").value_u8(37).expect("value");
    let plan = builder.freeze().expect("freeze");
    let payload = plan.execute(BuildPolicy::AllowStaging).expect("execute").payload;
    let bytes = payload.binary_bytes().expect("binary bytes");
    let opened = Payload::open_copy(&spec, &bytes, &OpenOptions::default()).expect("open copy");
    drop(payload);
    let view = opened.entry_view(0).expect("entry view").at(0).expect("value view");
    assert_eq!(view.get_u8().expect("checked value"), 37);
    let detached = view.materialize().expect("materialize");
    opened.invalidate().expect("invalidate");
    assert_eq!(view.kind().expect_err("checked view must reject stale use").symbol(), "VIEW_INVALIDATED");
    assert_eq!(detached.get_u8().expect("detached value survives"), 37);
    println!("FastDB 0.2.0 packaged system consumer: compile/codegen/build/open/view/materialize/invalidate passed");
}
''',
        encoding="utf-8",
    )
    return consumer


def _loader_environment(library: Path, target_dir: Path) -> dict[str, str]:
    environment = os.environ.copy()
    for key in (
        "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS",
        "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER",
    ):
        environment.pop(key, None)
    environment.update(
        {
            "CARGO_HOME": str(target_dir.parent / "cargo-home"),
            "CARGO_TARGET_DIR": str(target_dir),
            "FASTDB_PAYLOAD_LINK_MODE": "system",
            "FASTDB_PAYLOAD_SYSTEM_LIB_DIR": str(library.parent),
        }
    )
    if sys.platform == "darwin":
        loader_variable = "DYLD_LIBRARY_PATH"
    elif sys.platform == "win32":
        loader_variable = "PATH"
    else:
        loader_variable = "LD_LIBRARY_PATH"
    existing = environment.get(loader_variable)
    environment[loader_variable] = (
        str(library.parent)
        if not existing
        else f"{library.parent}{os.pathsep}{existing}"
    )
    return environment


def check_extracted_system_consumer(
    extracted: dict[str, Path],
    library: Path,
    root: Path,
) -> None:
    for label, path in {**extracted, "library": library}.items():
        resolved = path.resolve(strict=True)
        try:
            resolved.relative_to(ROOT.resolve(strict=True))
        except ValueError:
            pass
        else:
            raise CheckError(
                f"{label} system-consumer input must be outside the checkout: "
                f"{resolved}"
            )
    consumer = write_consumer(
        root,
        extracted["fastdb"],
        extracted["fastdb-sys"],
    )
    run(
        [
            "cargo",
            "run",
            "--offline",
            "--manifest-path",
            str(consumer / "Cargo.toml"),
        ],
        cwd=root,
        env=_loader_environment(library, root / "target"),
    )


def windows_runtime_dll(
    import_library: Path, platform: str = sys.platform
) -> Path | None:
    """Return the runtime DLL that must ship beside the Windows import library.

    Rust system linkage compiles and links against fastdb.lib, but the
    packaged consumer's test binary loads fastdb.dll at run time; relocating
    only the import library would produce a package that cannot execute.
    """
    if platform != "win32":
        return None
    if import_library.suffix.lower() != ".lib":
        raise CheckError(
            "Windows system link requires the fastdb.lib import library, "
            f"received {import_library.name}"
        )
    runtime = import_library.with_suffix(".dll")
    if not runtime.is_file():
        raise CheckError(
            f"Windows import library is missing its runtime DLL: {runtime}"
        )
    return runtime


def check_relocated_system_consumer(
    library: Path, package_dir: Path | None = None,
) -> None:
    with tempfile.TemporaryDirectory(
        prefix="fastdb-rust-system-"
    ) as temporary:
        root = Path(temporary)
        destination = package_dir if package_dir is not None else root / "packages"
        archives = package_archives(destination)
        extracted = extract_package_archives(
            archives, root / "extracted"
        )
        bundle = root / "bundle/lib"
        bundle.mkdir(parents=True)
        relocated = bundle / library.name
        shutil.copy2(library, relocated)
        runtime = windows_runtime_dll(library)
        if runtime is not None:
            shutil.copy2(runtime, bundle / runtime.name)
        check_extracted_system_consumer(
            extracted, relocated, root / "consumer"
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument(
        "--package-dir", type=Path,
        help="Retain the exact validated crate archives in this new directory.",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        library = locate_system_library(arguments.build_dir)
        check_relocated_system_consumer(library, arguments.package_dir)
    except (CheckError, OSError, UnicodeError) as error:
        print(f"Rust payload package check failed: {error}", file=sys.stderr)
        return 1
    print("Rust payload archives and relocated system link check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
