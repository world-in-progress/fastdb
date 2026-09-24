#!/usr/bin/env python3
"""Build and verify immutable FastDB release inventories (no publication)."""

from __future__ import annotations

import argparse
from email.parser import BytesParser
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import tomllib
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SCHEMA = "fastdb.release-manifest.v1"
FRAGMENT_SCHEMA = "fastdb.release-fragment.v1"
TARGETS = ("x86_64-unknown-linux-gnu", "aarch64-apple-darwin")
PYTHON_ABIS = ("cp310", "cp311", "cp312", "cp313", "cp314", "cp314t")
PARTS = ("core-linux", "core-macos", "wheels-linux", "wheels-macos", "sdist", "typescript")


class ReleaseError(RuntimeError):
    pass


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def source_sha() -> str:
    tracked = subprocess.check_output(["git", "diff", "--name-only", "HEAD", "-z"], cwd=ROOT).split(b"\0")
    untracked = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=ROOT).split(b"\0")
    output_roots = {"build", "dist", "wheelhouse", "release-parts", "release", "stage"}
    unexpected = [path.decode("utf-8") for path in untracked if path and Path(path.decode("utf-8")).parts[0] not in output_roots]
    if any(tracked) or unexpected:
        raise ReleaseError("Release source must be committed and clean; generated output directories do not permit tracked source changes")
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()


def version() -> str:
    python = tomllib.loads((ROOT / "pyproject.toml").read_text())["project"]["version"]
    versions = [python, json.loads((ROOT / "ts/fastdb4ts/package.json").read_text())["version"]]
    versions.extend(tomllib.loads((ROOT / f"bindings/rust/{name}/Cargo.toml").read_text())["package"]["version"] for name in ("fastdb", "fastdb-sys"))
    if any(value != python for value in versions) or re.fullmatch(r"\d+\.\d+\.\d+", python) is None:
        raise ReleaseError(f"Release package versions must match: {versions}")
    return python


def write_json(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def file_record(path: Path, *, kind: str, target: str | None = None) -> dict:
    return {"name": path.name, "kind": kind, "target": target, "bytes": path.stat().st_size, "sha256": digest(path)}


def make_core(build_dir: Path, output: Path, target: str) -> None:
    from check_rust_payload_package import locate_system_library

    expected_machine = "arm64" if target == TARGETS[1] else "x86_64"
    if platform.machine().lower() != expected_machine:
        raise ReleaseError(f"Target {target} requires a native {expected_machine} builder")
    commit = source_sha()
    marker = json.loads((build_dir / "release-build-source.json").read_text())
    if marker != {"source_sha": commit, "source_root": str(ROOT.resolve())}:
        raise ReleaseError("Core build directory was not initialized from this exact committed source")
    output.mkdir(parents=True, exist_ok=True)
    library = locate_system_library(build_dir)
    release_version = version()
    with tempfile.TemporaryDirectory(prefix="fastdb-core-release-") as directory:
        bundle = Path(directory)
        (bundle / "include").mkdir()
        (bundle / "lib").mkdir()
        for header in ("fastdb_payload.h", "fastdb_payload.hpp"):
            shutil.copy2(ROOT / "fastcarto/fastdb/include" / header, bundle / "include" / header)
        shutil.copy2(library, bundle / "lib" / library.name)
        shutil.copy2(ROOT / "LICENSE", bundle / "LICENSE")
        for dependency in ("yyjson", "double-conversion", "picosha2"):
            destination = bundle / "licenses" / dependency
            destination.mkdir(parents=True)
            for filename in ("LICENSE", "UPSTREAM.md"):
                shutil.copy2(ROOT / "fastcarto/lib" / dependency / filename, destination / filename)
        # Preserve the original embedded notices of the legacy libraries linked
        # into libfastdb, with exact source paths and an immutable source URL.
        for dependency in ("gaiageo", "clipper"):
            source_root = ROOT / "fastcarto/lib" / dependency
            notices = []
            for source in sorted(source_root.rglob("*")):
                if source.is_file() and source.suffix in {".c", ".cpp", ".h", ".hpp", ".inl"}:
                    contents = source.read_text(encoding="utf-8", errors="replace")
                    comment = re.search(r"/\*.*?\*/", contents, re.DOTALL)
                    if comment and re.search(r"copyright|license", comment.group(), re.IGNORECASE):
                        notices.append(source.relative_to(ROOT).as_posix() + "\n" + comment.group())
            if not notices:
                raise ReleaseError(f"Missing vendored license notices: {dependency}")
            destination = bundle / "licenses" / dependency
            destination.mkdir(parents=True)
            (destination / "NOTICES.txt").write_text("\n\n".join(notices) + "\n", encoding="utf-8")
            (destination / "UPSTREAM.txt").write_text(f"Retained source: https://github.com/world-in-progress/fastdb/tree/{source_sha()}/fastcarto/lib/{dependency}\n", encoding="utf-8")
        members = [file for file in sorted(bundle.rglob("*")) if file.is_file()]
        manifest = {
            "schema": "fastdb.core-bundle.v1", "version": release_version,
            "source_sha": source_sha(), "target": target,
            "platform": platform.platform(), "libc": list(platform.libc_ver()),
            "macos_deployment_target": os.environ.get("MACOSX_DEPLOYMENT_TARGET") if target == TARGETS[1] else None,
            "compiler": subprocess.check_output(["c++", "--version"], text=True).strip(),
            "link_mode": "system", "system_lib_dir": "<absolute bundle path>/lib",
            "abi": "fdb_payload_v1", "abi_symbol_count": 117,
            "files": [{"path": file.relative_to(bundle).as_posix(), "bytes": file.stat().st_size, "sha256": digest(file)} for file in members],
        }
        write_json(bundle / "manifest.json", manifest)
        destination = output / f"fastdb-core-{release_version}-{target}.tar.gz"
        with tarfile.open(destination, "w:gz") as archive:
            for member in sorted(bundle.rglob("*")):
                archive.add(member, arcname=member.relative_to(bundle).as_posix(), recursive=False)


def inspect_wheel(path: Path, release_version: str) -> str:
    # Check repaired wheel identity separately from the source-wheel inventory gate.
    # auditwheel/delocate may legitimately add dependency libraries to this archive.
    with zipfile.ZipFile(path) as archive:
        metadata = [name for name in archive.namelist() if name.endswith(".dist-info/METADATA")]
        if len(metadata) != 1:
            raise ReleaseError(f"Invalid wheel metadata: {path.name}")
        info = BytesParser().parsebytes(archive.read(metadata[0]))
        if info["Name"] != "fastdb4py" or info["Version"] != release_version:
            raise ReleaseError(f"Unexpected wheel identity: {path.name}")
        if not any(name.startswith("fastdb4py/payload/") for name in archive.namelist()):
            raise ReleaseError(f"Wheel omits the portable payload API: {path.name}")
    pieces = path.stem.split("-")
    return "cp314t" if pieces[-2] == "cp314t" else pieces[-3]


def record_part(output: Path, part: str, target: str | None) -> None:
    release_version = version()
    records = []
    for path in sorted(output.iterdir()):
        if not path.is_file() or path.suffix == ".json":
            continue
        if path.name.endswith(".whl"):
            abi = inspect_wheel(path, release_version)
            platform_tag = path.stem.split("-")[-1]
            if part == "wheels-linux" and ("manylinux" not in platform_tag or not platform_tag.endswith("x86_64")):
                raise ReleaseError(f"Linux release wheel is not manylinux x86_64: {path.name}")
            if part == "wheels-macos" and (not platform_tag.startswith("macosx_") or not platform_tag.endswith("arm64")):
                raise ReleaseError(f"macOS release wheel is not arm64: {path.name}")
            item = file_record(path, kind="python-wheel", target=target)
            item["python_abi"] = abi
        elif path.name.startswith("fastdb-core-") and path.name.endswith(".tar.gz"):
            item = file_record(path, kind="core-bundle", target=target)
        elif path.name.endswith(".crate"):
            item = file_record(path, kind="rust-crate")
        elif path.name == f"fastdb4py-{release_version}.tar.gz":
            item = file_record(path, kind="python-sdist")
        elif path.name == f"fastdb4ts-{release_version}.tgz":
            item = file_record(path, kind="typescript-package", target="wasm32")
        else:
            raise ReleaseError(f"Unexpected release artifact: {path.name}")
        records.append(item)
    if not records:
        raise ReleaseError(f"Release part is empty: {part}")
    write_json(output / f"{part}.json", {
        "schema": FRAGMENT_SCHEMA, "part": part, "version": release_version,
        "source_sha": source_sha(), "artifacts": records,
    })


def check_complete(records: list[dict], release_version: str) -> None:
    expected_fixed = {
        *(f"fastdb-core-{release_version}-{target}.tar.gz" for target in TARGETS),
        f"fastdb-sys-{release_version}.crate", f"fastdb-{release_version}.crate",
        f"fastdb4py-{release_version}.tar.gz", f"fastdb4ts-{release_version}.tgz",
    }
    fixed = {item["name"] for item in records if item["kind"] != "python-wheel"}
    if fixed != expected_fixed:
        raise ReleaseError(f"Incomplete fixed release artifacts: {fixed ^ expected_fixed}")
    for target in TARGETS:
        wheels = [item for item in records if item["kind"] == "python-wheel" and item["target"] == target]
        if sorted(item["python_abi"] for item in wheels) != sorted(PYTHON_ABIS):
            raise ReleaseError(f"Incomplete Python ABI wheel set for {target}")


def assemble(input_dir: Path, output: Path) -> None:
    release_version = version()
    commit = source_sha()
    output.mkdir(parents=True, exist_ok=True)
    parts = {}
    records = []
    for path in sorted(input_dir.rglob("*.json")):
        document = json.loads(path.read_text())
        if document.get("schema") != FRAGMENT_SCHEMA:
            raise ReleaseError(f"Unknown fragment: {path}")
        part = document["part"]
        if part in parts or part not in PARTS:
            raise ReleaseError(f"Duplicate or unknown release part: {part}")
        if document["source_sha"] != commit or document["version"] != release_version:
            raise ReleaseError(f"Mixed source/version release part: {part}")
        parts[part] = digest(path)
        for item in document["artifacts"]:
            filename = item["name"]
            if PurePosixPath(filename).name != filename or "\\" in filename:
                raise ReleaseError("Artifact name must be a filename")
            source = path.parent / filename
            if source.is_symlink() or digest(source) != item["sha256"] or source.stat().st_size != item["bytes"]:
                raise ReleaseError(f"Artifact content mismatch: {filename}")
            if (output / filename).exists():
                raise ReleaseError(f"Duplicate artifact: {filename}")
            shutil.copy2(source, output / filename)
            records.append(item)
    if set(parts) != set(PARTS):
        raise ReleaseError(f"Incomplete release parts: {set(parts) ^ set(PARTS)}")
    check_complete(records, release_version)
    write_json(output / "release-manifest.json", {
        "schema": SCHEMA, "version": release_version, "source_sha": commit,
        "repository": os.environ.get("GITHUB_REPOSITORY", "world-in-progress/fastdb"),
        "build_run_id": os.environ.get("GITHUB_RUN_ID"),
        "build_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "parts": parts, "artifacts": sorted(records, key=lambda item: item["name"]),
        "reproducibility": "Artifact identity is exact; independent native byte reproducibility is not claimed.",
    })


def verify(directory: Path, commit: str) -> dict:
    document = json.loads((directory / "release-manifest.json").read_text())
    if document.get("schema") != SCHEMA or document.get("source_sha") != commit:
        raise ReleaseError("Release manifest schema or source SHA mismatch")
    records = document["artifacts"]
    names = [item["name"] for item in records]
    if len(set(names)) != len(names):
        raise ReleaseError("Release manifest contains duplicate artifacts")
    for item in records:
        filename = item["name"]
        if PurePosixPath(filename).name != filename or "\\" in filename:
            raise ReleaseError("Artifact name must be a filename")
        artifact = directory / filename
        if artifact.is_symlink() or digest(artifact) != item["sha256"] or artifact.stat().st_size != item["bytes"]:
            raise ReleaseError(f"Release artifact mismatch: {filename}")
    check_complete(records, document["version"])
    if {path.name for path in directory.iterdir()} != {*names, "release-manifest.json"}:
        raise ReleaseError("Release directory contains unrecorded files")
    return document


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("version")
    start = sub.add_parser("start-native")
    start.add_argument("--build-dir", type=Path, required=True)
    core = sub.add_parser("core")
    core.add_argument("--build-dir", type=Path, required=True)
    core.add_argument("--output", type=Path, required=True)
    core.add_argument("--target", choices=TARGETS, required=True)
    part = sub.add_parser("record")
    part.add_argument("--output", type=Path, required=True)
    part.add_argument("--part", choices=PARTS, required=True)
    part.add_argument("--target", choices=TARGETS)
    assembly = sub.add_parser("assemble")
    assembly.add_argument("--input", type=Path, required=True)
    assembly.add_argument("--output", type=Path, required=True)
    check = sub.add_parser("verify")
    check.add_argument("--directory", type=Path, required=True)
    check.add_argument("--source-sha", required=True)
    args = parser.parse_args()
    if args.command == "version":
        source_sha()
        print(version())
    elif args.command == "start-native":
        commit = source_sha()
        args.build_dir.mkdir(parents=True, exist_ok=False)
        write_json(args.build_dir / "release-build-source.json", {"source_sha": commit, "source_root": str(ROOT.resolve())})
    elif args.command == "core":
        make_core(args.build_dir, args.output, args.target)
    elif args.command == "record":
        record_part(args.output, args.part, args.target)
    elif args.command == "assemble":
        assemble(args.input, args.output)
    else:
        verify(args.directory, args.source_sha)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ReleaseError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
