#!/usr/bin/env python3
"""Exercise Rust payload source inventory and relocated system linking."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
RUST_ROOT = ROOT / "bindings/rust"
PACKAGE_REQUIRED = {
    "fastdb-sys": {
        ".cargo_vcs_info.json",
        "Cargo.lock",
        "Cargo.toml",
        "Cargo.toml.orig",
        "README.md",
        "build.rs",
        "src/lib.rs",
    },
    "fastdb": {
        ".cargo_vcs_info.json",
        "Cargo.lock",
        "Cargo.toml",
        "Cargo.toml.orig",
        "README.md",
        "src/builder.rs",
        "src/lib.rs",
        "src/runtime.rs",
    },
}
FORBIDDEN_PARTS = {"target", "fastcarto", ".git", "tests", "build"}


class CheckError(RuntimeError):
    """The Rust package/link boundary is incomplete."""


def run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> str:
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
    output = run(
        [
            "cargo",
            "package",
            "--manifest-path",
            str(RUST_ROOT / package / "Cargo.toml"),
            "--allow-dirty",
            "--no-verify",
            "--list",
        ],
        cwd=ROOT,
    )
    return {line for line in output.splitlines() if line and not line.startswith("warning:")}


def check_inventory(package: str, names: set[str]) -> None:
    required = PACKAGE_REQUIRED[package]
    missing = sorted(required - names)
    if missing:
        raise CheckError(f"{package} package inventory is missing {missing}")
    debris = sorted(
        name
        for name in names
        if Path(name).is_absolute()
        or ".." in Path(name).parts
        or any(part in FORBIDDEN_PARTS for part in Path(name).parts)
    )
    if debris:
        raise CheckError(f"{package} package inventory contains debris {debris}")


def locate_system_library(build_dir: Path, platform: str = sys.platform) -> Path:
    names = {
        "darwin": ("libfastdb.dylib",),
        "linux": ("libfastdb.so",),
        "win32": ("fastdb.lib",),
    }
    key = "linux" if platform.startswith("linux") else platform
    if key not in names:
        raise CheckError(f"unsupported Rust system-link test platform: {platform}")
    try:
        root = build_dir.resolve(strict=True)
    except FileNotFoundError as error:
        raise CheckError(f"native build directory does not exist: {build_dir}") from error
    candidates = sorted(
        (
            candidate.resolve(strict=True)
            for name in names[key]
            for candidate in root.rglob(name)
            if candidate.is_file()
        ),
        key=os.fspath,
    )
    if len(candidates) != 1:
        raise CheckError(
            f"expected exactly one system FastDB library under {root}, "
            f"found {len(candidates)}: {[str(item) for item in candidates]}"
        )
    return candidates[0]


def copy_crate(source: Path, destination: Path) -> None:
    destination.mkdir(parents=True)
    shutil.copy2(source / "Cargo.toml", destination / "Cargo.toml")
    if (source / "build.rs").is_file():
        shutil.copy2(source / "build.rs", destination / "build.rs")
    shutil.copytree(source / "src", destination / "src")


def write_consumer(root: Path) -> Path:
    consumer = root / "consumer"
    (consumer / "src").mkdir(parents=True)
    (consumer / "Cargo.toml").write_text(
        """[package]
name = "fastdb-system-link-smoke"
version = "0.0.0"
edition = "2024"
publish = false

[workspace]

[dependencies]
fastdb = { path = "../bindings/rust/fastdb" }
""",
        encoding="utf-8",
    )
    (consumer / "src/main.rs").write_text(
        r'''use fastdb::{CompiledSpec, Profile};

fn main() {
    let source = br#"{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]}"#;
    let spec = CompiledSpec::compile(source).expect("system Core compile");
    assert_eq!(spec.profile().expect("profile"), Profile::RecordV1);
    assert_eq!(spec.entry_count().expect("entry count"), 0);
    assert_eq!(spec.component_count().expect("component count"), 0);
    assert_eq!(spec.sha256().expect("digest").len(), 32);
}
''',
        encoding="utf-8",
    )
    return consumer


def check_relocated_system_consumer(library: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="fastdb-rust-system-") as temporary:
        root = Path(temporary)
        bindings = root / "bindings/rust"
        copy_crate(RUST_ROOT / "fastdb-sys", bindings / "fastdb-sys")
        copy_crate(RUST_ROOT / "fastdb", bindings / "fastdb")
        consumer = write_consumer(root)
        environment = os.environ.copy()
        environment.update(
            {
                "CARGO_TARGET_DIR": str(root / "target"),
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
        run(
            ["cargo", "run", "--offline", "--manifest-path", str(consumer / "Cargo.toml")],
            cwd=root,
            env=environment,
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        for package in PACKAGE_REQUIRED:
            check_inventory(package, package_inventory(package))
        library = locate_system_library(arguments.build_dir)
        check_relocated_system_consumer(library)
    except (CheckError, OSError, UnicodeError) as error:
        print(f"Rust payload package check failed: {error}", file=sys.stderr)
        return 1
    print("Rust payload source inventory and relocated system link check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
