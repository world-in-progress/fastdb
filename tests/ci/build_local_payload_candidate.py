#!/usr/bin/env python3
"""Build or verify the FastDB owner-repository local payload candidate."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from typing import Any
import zipfile


ROOT = Path(__file__).resolve().parents[2]
CI_ROOT = Path(__file__).resolve().parent
MANIFEST_SCHEMA = "fastdb.local-candidate-manifest.v1"
MANIFEST_FILENAME = "fastdb-local-candidate-manifest.v1.json"
ABI_VERSION = 1
ABI_SYMBOL_COUNT = 117
FASTDB_VERSION = "0.2.0"
FASTDB4TS_VERSION = "0.2.0"
EXPECTED_ARTIFACT_KINDS = {
    "core-c-abi-bundle",
    "rust-fastdb-sys-crate",
    "rust-fastdb-crate",
    "python-sdist",
    "python-current-wheel",
    "python-cp310-wheel",
    "typescript-npm-tarball",
}
PYTHON_BUILD_ORDER = ("cp310", "current")
WALL_CLOCK_KEYS = {
    "created_at",
    "generated_at",
    "timestamp",
    "wall_clock",
}
KNOWN_PLATFORMS = {
    "darwin-arm64",
    "linux-aarch64",
    "linux-x86_64",
    "win32-amd64",
}
CORE_CONFIGURE_COMMAND = (
    "cmake -S $SOURCE_ROOT/fastcarto -B $BUILD_ROOT/fastdb-core "
    "-DBUILD_TESTING=OFF -DBUILD_TOOLS=OFF -DUSE_SWIG_PYTHON=OFF "
    "-DUSE_SWIG_NODE=OFF -DUSE_SWIG_GO=OFF "
    "-DCMAKE_BUILD_TYPE=Release"
)
CORE_BUILD_COMMAND = (
    "cmake --build $BUILD_ROOT/fastdb-core --target fastdb "
    "--config Release --parallel"
)
ABI_CHECK_COMMAND = (
    "python $SOURCE_ROOT/tools/check_payload_abi_symbols.py "
    "--build-dir $BUILD_ROOT/fastdb-core"
)


class CandidateError(RuntimeError):
    """The local candidate or its retained evidence is invalid."""


def _load_ci_module(name: str, filename: str):
    path = CI_ROOT / filename
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise CandidateError(f"could not load CI helper {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUST_PACKAGE = _load_ci_module(
    "rust_payload_package_for_candidate", "check_rust_payload_package.py"
)


def run(
    command: list[str],
    *,
    cwd: Path = ROOT,
    env: dict[str, str] | None = None,
) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="strict",
    )
    if completed.returncode != 0:
        raise CandidateError(
            f"command failed with exit {completed.returncode}: "
            f"{' '.join(command)}\n{completed.stdout}"
        )
    return completed.stdout


def canonical_json_bytes(document: Any) -> bytes:
    return (
        json.dumps(
            document,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("utf-8")


def load_json_no_duplicates(source: str, label: str) -> Any:
    def object_from_pairs(
        pairs: list[tuple[str, Any]],
    ) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise CandidateError(
                    f"{label} contains duplicate JSON key {key!r}"
                )
            result[key] = value
        return result

    try:
        return json.loads(source, object_pairs_hook=object_from_pairs)
    except json.JSONDecodeError as error:
        raise CandidateError(f"{label} is invalid JSON: {error}") from error


def checked_relative_path(value: str) -> str:
    path = PurePosixPath(value)
    if (
        not value
        or "\\" in value
        or path.is_absolute()
        or ".." in path.parts
        or "." in path.parts
        or path.as_posix() != value
        or "//" in value
    ):
        raise CandidateError(
            f"candidate path must be canonical and relative: {value!r}"
        )
    return value


def sha256_bytes(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def file_record(path: Path, relative_path: str) -> dict[str, Any]:
    checked_relative_path(relative_path)
    if path.is_symlink() or not path.is_file():
        raise CandidateError(
            f"candidate inventory member must be a regular file: {path}"
        )
    contents = path.read_bytes()
    return {
        "path": relative_path,
        "bytes": len(contents),
        "sha256": sha256_bytes(contents),
    }


def validate_inventory(
    inventory: list[dict[str, Any]], label: str
) -> None:
    paths: list[str] = []
    for item in inventory:
        if set(item) != {"path", "bytes", "sha256"}:
            raise CandidateError(
                f"{label} inventory records must have exact "
                "path/bytes/sha256 fields"
            )
        path = item["path"]
        size = item["bytes"]
        digest = item["sha256"]
        if not isinstance(path, str):
            raise CandidateError(f"{label} inventory path must be a string")
        checked_relative_path(path)
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise CandidateError(
                f"{label} inventory bytes must be a non-negative integer"
            )
        if (
            not isinstance(digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", digest) is None
        ):
            raise CandidateError(
                f"{label} inventory SHA-256 must be lowercase hex"
            )
        paths.append(path)
    if len(paths) != len(set(paths)):
        raise CandidateError(
            f"{label} inventory contains duplicate relative paths"
        )
    if paths != sorted(paths):
        raise CandidateError(
            f"{label} inventory must be sorted by relative path"
        )


def _checked_archive_names(
    names: list[str],
    *,
    root: str | None,
    label: str,
) -> list[tuple[str, str]]:
    if len(names) != len(set(names)):
        raise CandidateError(f"{label} contains duplicate member names")
    result: list[tuple[str, str]] = []
    prefix = f"{root}/" if root is not None else None
    for raw in names:
        if root is not None:
            if raw == root:
                continue
            if not raw.startswith(prefix):
                raise CandidateError(
                    f"{label} contains member outside {root}/: {raw!r}"
                )
            relative = raw[len(prefix) :]
        else:
            relative = raw
        checked_relative_path(relative)
        result.append((raw, relative))
    relative_names = [relative for _, relative in result]
    if len(relative_names) != len(set(relative_names)):
        raise CandidateError(
            f"{label} contains colliding normalized member paths"
        )
    return result


def tar_inventory(
    archive_path: Path,
    *,
    root: str,
    label: str,
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    try:
        with tarfile.open(archive_path, "r:gz") as archive:
            members = archive.getmembers()
            special = [
                member.name
                for member in members
                if not (member.isfile() or member.isdir())
            ]
            if special:
                raise CandidateError(
                    f"{label} contains non-file members: {special}"
                )
            mapped = _checked_archive_names(
                [member.name for member in members],
                root=root,
                label=label,
            )
            by_name = {member.name: member for member in members}
            for raw, relative in mapped:
                member = by_name[raw]
                if member.isdir():
                    continue
                extracted = archive.extractfile(member)
                if extracted is None:
                    raise CandidateError(
                        f"{label} member has no file contents: {raw}"
                    )
                contents = extracted.read()
                records.append(
                    {
                        "path": relative,
                        "bytes": len(contents),
                        "sha256": sha256_bytes(contents),
                    }
                )
    except (tarfile.TarError, OSError) as error:
        raise CandidateError(
            f"could not inspect {label} {archive_path}: {error}"
        ) from error
    records.sort(key=lambda item: item["path"])
    validate_inventory(records, label)
    return records


def zip_inventory(
    archive_path: Path, *, label: str
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    try:
        with zipfile.ZipFile(archive_path) as archive:
            infos = archive.infolist()
            mapped = _checked_archive_names(
                [info.filename for info in infos],
                root=None,
                label=label,
            )
            by_name = {info.filename: info for info in infos}
            for raw, relative in mapped:
                info = by_name[raw]
                unix_mode = (info.external_attr >> 16) & 0xFFFF
                if unix_mode and stat.S_ISLNK(unix_mode):
                    raise CandidateError(
                        f"{label} contains symbolic link member {raw!r}"
                    )
                if info.is_dir():
                    continue
                contents = archive.read(info)
                records.append(
                    {
                        "path": relative,
                        "bytes": len(contents),
                        "sha256": sha256_bytes(contents),
                    }
                )
    except (zipfile.BadZipFile, OSError) as error:
        raise CandidateError(
            f"could not inspect {label} {archive_path}: {error}"
        ) from error
    records.sort(key=lambda item: item["path"])
    validate_inventory(records, label)
    return records


def directory_inventory(
    directory: Path, *, label: str
) -> list[dict[str, Any]]:
    if directory.is_symlink() or not directory.is_dir():
        raise CandidateError(f"{label} must be a regular directory")
    records: list[dict[str, Any]] = []
    for path in sorted(directory.rglob("*"), key=os.fspath):
        if path.is_symlink():
            raise CandidateError(
                f"{label} contains symbolic link {path.relative_to(directory)}"
            )
        if path.is_dir():
            continue
        relative = path.relative_to(directory).as_posix()
        records.append(file_record(path, relative))
    validate_inventory(records, label)
    return records


def archive_inventory(path: Path, *, label: str) -> list[dict[str, Any]]:
    if path.suffix == ".crate":
        package_root = path.name[: -len(".crate")]
        return tar_inventory(path, root=package_root, label=label)
    if path.name.endswith(".tar.gz"):
        package_root = path.name[: -len(".tar.gz")]
        return tar_inventory(path, root=package_root, label=label)
    if path.suffix == ".tgz":
        return tar_inventory(path, root="package", label=label)
    if path.suffix == ".whl":
        return zip_inventory(path, label=label)
    return [file_record(path, path.name)]


def artifact_descriptor(
    candidate_root: Path,
    *,
    path: str,
    kind: str,
    package: dict[str, str],
    build: dict[str, Any],
) -> dict[str, Any]:
    relative = checked_relative_path(path)
    artifact = candidate_root / relative
    try:
        artifact.resolve(strict=True).relative_to(
            candidate_root.resolve(strict=True)
        )
    except (FileNotFoundError, ValueError) as error:
        raise CandidateError(
            f"artifact path escapes or is missing: {relative}"
        ) from error
    if artifact.is_symlink():
        raise CandidateError(f"artifact must not be a symlink: {relative}")
    if artifact.is_dir():
        inventory = directory_inventory(artifact, label=kind)
        size = sum(item["bytes"] for item in inventory)
        digest = sha256_bytes(canonical_json_bytes(inventory))
    elif artifact.is_file():
        inventory = archive_inventory(artifact, label=kind)
        contents = artifact.read_bytes()
        size = len(contents)
        digest = sha256_bytes(contents)
    else:
        raise CandidateError(f"artifact is not a regular file/directory: {relative}")
    return {
        "kind": kind,
        "path": relative,
        "package": dict(package),
        "bytes": size,
        "sha256": digest,
        "inventory": inventory,
        "build": json.loads(canonical_json_bytes(build)),
    }


def current_platform() -> str:
    machine = platform.machine().lower()
    if sys.platform == "darwin":
        machine = "arm64" if machine in {"arm64", "aarch64"} else machine
        return f"darwin-{machine}"
    if sys.platform.startswith("linux"):
        machine = "aarch64" if machine in {"arm64", "aarch64"} else machine
        return f"linux-{machine}"
    if sys.platform == "win32":
        machine = "amd64" if machine in {"amd64", "x86_64"} else machine
        return f"win32-{machine}"
    return f"{sys.platform}-{machine}"


def build_evidence(
    commands: list[str],
    toolchains: dict[str, str],
    *,
    target: str,
) -> dict[str, Any]:
    verified = current_platform()
    evidence = {
        "commands": list(commands),
        "toolchains": dict(sorted(toolchains.items())),
        "target": target,
        "platform": verified,
        "verified_platforms": [verified],
        "unverified_platforms": sorted(KNOWN_PLATFORMS - {verified}),
    }
    reject_absolute_manifest_paths(evidence)
    return evidence


def test_build_evidence() -> dict[str, Any]:
    return build_evidence(
        ["fixture-build $SOURCE_ROOT"],
        {"fixture": "fixture 1"},
        target="fixture-target",
    )


def test_provenance() -> dict[str, Any]:
    return {
        "repository": "fastdb",
        "commit": "0" * 40,
        "source_date_epoch": 1,
        "worktree_clean": True,
        "worktree_state_sha256": sha256_bytes(b""),
    }


def read_abi_allowlist() -> list[str]:
    path = ROOT / "tests/abi/fastdb_payload_v1_symbols.txt"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CandidateError(
            f"could not read payload ABI allowlist: {error}"
        ) from error
    if (
        len(lines) != ABI_SYMBOL_COUNT
        or lines != sorted(set(lines))
        or any(
            re.fullmatch(r"fdb_payload_v1_[A-Za-z0-9_]+", line) is None
            for line in lines
        )
    ):
        raise CandidateError(
            "payload ABI allowlist must contain exact sorted 117 V1 symbols"
        )
    return lines


def abi_evidence() -> dict[str, Any]:
    symbols = read_abi_allowlist()
    return {
        "version": ABI_VERSION,
        "symbol_count": len(symbols),
        "symbols": symbols,
    }


def reject_absolute_manifest_paths(document: Any) -> None:
    checkout = os.fspath(ROOT.resolve())
    absolute_fragment = re.compile(
        r"(?:(?<=^)|(?<=[\s=]))"
        r"(?:/(?:Users|home|private|tmp|var|opt)/|[A-Za-z]:[\\/])"
    )

    def visit(value: Any, key: str | None = None) -> None:
        if key in WALL_CLOCK_KEYS:
            raise CandidateError(
                f"manifest must not retain wall-clock field {key!r}"
            )
        if isinstance(value, dict):
            for child_key, child in value.items():
                if not isinstance(child_key, str):
                    raise CandidateError("manifest object keys must be strings")
                visit(child, child_key)
        elif isinstance(value, list):
            for child in value:
                visit(child, key)
        elif isinstance(value, str):
            if key == "path" or key is not None and key.endswith("_path"):
                checked_relative_path(value)
            if checkout in value or absolute_fragment.search(value):
                raise CandidateError(
                    "manifest must not retain source-machine absolute paths"
                )

    visit(document)


def _validate_build(build: Any, label: str) -> None:
    required = {
        "commands",
        "toolchains",
        "target",
        "platform",
        "verified_platforms",
        "unverified_platforms",
    }
    if not isinstance(build, dict) or set(build) != required:
        raise CandidateError(
            f"{label} build evidence must have exact fields {sorted(required)}"
        )
    commands = build["commands"]
    if (
        not isinstance(commands, list)
        or not commands
        or any(not isinstance(command, str) or not command for command in commands)
    ):
        raise CandidateError(f"{label} build commands must be non-empty strings")
    toolchains = build["toolchains"]
    if (
        not isinstance(toolchains, dict)
        or not toolchains
        or any(
            not isinstance(key, str)
            or not key
            or not isinstance(value, str)
            or not value
            for key, value in toolchains.items()
        )
    ):
        raise CandidateError(
            f"{label} build toolchains must be a non-empty string map"
        )
    for field in ("target", "platform"):
        if not isinstance(build[field], str) or not build[field]:
            raise CandidateError(f"{label} build {field} must be a string")
    for field in ("verified_platforms", "unverified_platforms"):
        values = build[field]
        if (
            not isinstance(values, list)
            or values != sorted(set(values))
            or any(not isinstance(value, str) or not value for value in values)
        ):
            raise CandidateError(
                f"{label} build {field} must be a sorted unique string list"
            )
    if build["platform"] not in build["verified_platforms"]:
        raise CandidateError(
            f"{label} current platform must be explicitly verified"
        )
    if set(build["verified_platforms"]) & set(build["unverified_platforms"]):
        raise CandidateError(
            f"{label} verified and unverified platforms overlap"
        )


def validate_manifest(
    document: Any, *, enforce_candidate_set: bool
) -> None:
    if not isinstance(document, dict):
        raise CandidateError("candidate manifest root must be an object")
    if set(document) != {"schema", "source", "abi", "artifacts"}:
        raise CandidateError(
            "candidate manifest must have exact schema/source/abi/artifacts fields"
        )
    if document["schema"] != MANIFEST_SCHEMA:
        raise CandidateError(
            f"candidate manifest schema must be {MANIFEST_SCHEMA}"
        )
    source = document["source"]
    if not isinstance(source, dict) or set(source) != {
        "repository",
        "commit",
        "source_date_epoch",
        "worktree_clean",
        "worktree_state_sha256",
    }:
        raise CandidateError("candidate source provenance has wrong fields")
    if source["repository"] != "fastdb":
        raise CandidateError("candidate source repository must be fastdb")
    if (
        not isinstance(source["commit"], str)
        or re.fullmatch(r"[0-9a-f]{40}", source["commit"]) is None
    ):
        raise CandidateError("candidate source commit must be a full Git SHA")
    if (
        not isinstance(source["source_date_epoch"], int)
        or isinstance(source["source_date_epoch"], bool)
        or source["source_date_epoch"] < 0
    ):
        raise CandidateError(
            "candidate source_date_epoch must be a non-negative integer"
        )
    if not isinstance(source["worktree_clean"], bool):
        raise CandidateError("candidate worktree_clean must be boolean")
    if (
        not isinstance(source["worktree_state_sha256"], str)
        or re.fullmatch(
            r"[0-9a-f]{64}", source["worktree_state_sha256"]
        )
        is None
    ):
        raise CandidateError(
            "candidate worktree_state_sha256 must be lowercase SHA-256"
        )
    abi = document["abi"]
    expected_abi = abi_evidence()
    if abi != expected_abi:
        raise CandidateError(
            "candidate ABI evidence differs from exact ABI version 1 / 117 symbols"
        )
    artifacts = document["artifacts"]
    if not isinstance(artifacts, list):
        raise CandidateError("candidate artifacts must be a list")
    paths: list[str] = []
    kinds: list[str] = []
    for artifact in artifacts:
        if not isinstance(artifact, dict) or set(artifact) != {
            "kind",
            "path",
            "package",
            "bytes",
            "sha256",
            "inventory",
            "build",
            "source_commit",
        }:
            raise CandidateError("candidate artifact has wrong fields")
        kind = artifact["kind"]
        if not isinstance(kind, str) or not kind:
            raise CandidateError("candidate artifact kind must be a string")
        path = artifact["path"]
        if not isinstance(path, str):
            raise CandidateError("candidate artifact path must be a string")
        checked_relative_path(path)
        package = artifact["package"]
        if (
            not isinstance(package, dict)
            or set(package) != {"name", "version"}
            or any(
                not isinstance(value, str) or not value
                for value in package.values()
            )
        ):
            raise CandidateError(
                f"{kind} package identity must contain exact name/version"
            )
        size = artifact["bytes"]
        digest = artifact["sha256"]
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise CandidateError(f"{kind} artifact bytes are invalid")
        if (
            not isinstance(digest, str)
            or re.fullmatch(r"[0-9a-f]{64}", digest) is None
        ):
            raise CandidateError(f"{kind} artifact SHA-256 is invalid")
        inventory = artifact["inventory"]
        if not isinstance(inventory, list) or not inventory:
            raise CandidateError(f"{kind} inventory must be non-empty")
        validate_inventory(inventory, kind)
        _validate_build(artifact["build"], kind)
        if artifact["source_commit"] != source["commit"]:
            raise CandidateError(
                f"{kind} source commit differs from candidate source"
            )
        paths.append(path)
        kinds.append(kind)
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise CandidateError(
            "candidate artifact paths must be sorted and unique"
        )
    if len(kinds) != len(set(kinds)):
        raise CandidateError("candidate artifact kinds must be unique")
    if enforce_candidate_set and set(kinds) != EXPECTED_ARTIFACT_KINDS:
        raise CandidateError(
            "candidate artifact set differs from the complete owner package set; "
            f"actual={sorted(kinds)}"
        )
    reject_absolute_manifest_paths(document)


def construct_manifest(
    candidate_root: Path,
    provenance: dict[str, Any],
    artifacts: list[dict[str, Any]],
    *,
    enforce_candidate_set: bool = True,
) -> dict[str, Any]:
    source_commit = provenance.get("commit")
    normalized_artifacts: list[dict[str, Any]] = []
    for artifact in artifacts:
        normalized = json.loads(canonical_json_bytes(artifact))
        normalized["source_commit"] = source_commit
        normalized_artifacts.append(normalized)
    normalized_artifacts.sort(key=lambda item: item["path"])
    document = {
        "schema": MANIFEST_SCHEMA,
        "source": json.loads(canonical_json_bytes(provenance)),
        "abi": abi_evidence(),
        "artifacts": normalized_artifacts,
    }
    validate_manifest(
        document, enforce_candidate_set=enforce_candidate_set
    )
    for artifact in document["artifacts"]:
        if not (candidate_root / artifact["path"]).exists():
            raise CandidateError(
                f"manifest artifact is missing: {artifact['path']}"
            )
    return document


def validate_candidate_tree(
    candidate_root: Path, document: dict[str, Any]
) -> None:
    expected_files = {MANIFEST_FILENAME}
    expected_directories: set[str] = set()
    for artifact in document["artifacts"]:
        artifact_path = artifact["path"]
        artifact_on_disk = candidate_root / artifact_path
        if artifact_on_disk.is_dir():
            expected_directories.add(artifact_path)
            for item in artifact["inventory"]:
                expected_files.add(f"{artifact_path}/{item['path']}")
        else:
            expected_files.add(artifact_path)
    for filename in expected_files:
        parent = PurePosixPath(filename).parent
        while parent.as_posix() != ".":
            expected_directories.add(parent.as_posix())
            parent = parent.parent

    actual_files: set[str] = set()
    actual_directories: set[str] = set()
    for path in candidate_root.rglob("*"):
        relative = path.relative_to(candidate_root).as_posix()
        checked_relative_path(relative)
        if path.is_symlink():
            raise CandidateError(
                f"candidate tree contains symbolic link {relative!r}"
            )
        if path.is_dir():
            actual_directories.add(relative)
        elif path.is_file():
            actual_files.add(relative)
        else:
            raise CandidateError(
                f"candidate tree contains special member {relative!r}"
            )
    if actual_files != expected_files:
        raise CandidateError(
            "candidate tree files differ from the manifest artifact set; "
            f"missing={sorted(expected_files - actual_files)}, "
            f"unexpected={sorted(actual_files - expected_files)}"
        )
    if actual_directories != expected_directories:
        raise CandidateError(
            "candidate tree directories differ from the manifest artifact set; "
            f"missing={sorted(expected_directories - actual_directories)}, "
            f"unexpected={sorted(actual_directories - expected_directories)}"
        )


def verify_manifest(
    candidate_root: Path, *, enforce_candidate_set: bool = True
) -> dict[str, Any]:
    try:
        root = candidate_root.resolve(strict=True)
    except FileNotFoundError as error:
        raise CandidateError(
            f"candidate directory does not exist: {candidate_root}"
        ) from error
    if not root.is_dir():
        raise CandidateError(f"candidate path is not a directory: {root}")
    manifest_path = root / MANIFEST_FILENAME
    try:
        retained = manifest_path.read_bytes()
        source = retained.decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise CandidateError(
            f"could not read candidate manifest: {error}"
        ) from error
    document = load_json_no_duplicates(source, "candidate manifest")
    validate_manifest(
        document, enforce_candidate_set=enforce_candidate_set
    )
    if canonical_json_bytes(document) != retained:
        raise CandidateError(
            "candidate manifest is not canonical sorted compact JSON"
        )
    validate_candidate_tree(root, document)
    reconstructed: list[dict[str, Any]] = []
    for retained_artifact in document["artifacts"]:
        reconstructed.append(
            artifact_descriptor(
                root,
                path=retained_artifact["path"],
                kind=retained_artifact["kind"],
                package=retained_artifact["package"],
                build=retained_artifact["build"],
            )
        )
    rebuilt = construct_manifest(
        root,
        document["source"],
        reconstructed,
        enforce_candidate_set=enforce_candidate_set,
    )
    if canonical_json_bytes(rebuilt) != retained:
        raise CandidateError(
            "candidate manifest differs from reconstructed artifact evidence"
        )
    return document


def require_absent_output(output: Path) -> None:
    if output.exists() or output.is_symlink():
        raise CandidateError(
            f"candidate output directory must be absent: {output}"
        )


def _source_environment(source_date_epoch: int) -> dict[str, str]:
    environment = os.environ.copy()
    environment.update(
        {
            "SOURCE_DATE_EPOCH": str(source_date_epoch),
            "TZ": "UTC",
        }
    )
    return environment


def build_core_bundle(
    build_root: Path,
    bundle: Path,
    *,
    source_date_epoch: int | None = None,
) -> Path:
    if build_root.exists():
        raise CandidateError(f"Core build directory must be absent: {build_root}")
    if bundle.exists():
        raise CandidateError(f"Core bundle directory must be absent: {bundle}")
    epoch = source_date_epoch
    if epoch is None:
        epoch = int(
            run(["git", "show", "-s", "--format=%ct", "HEAD"]).strip()
        )
    environment = _source_environment(epoch)
    run(
        [
            "cmake",
            "-S",
            str(ROOT / "fastcarto"),
            "-B",
            str(build_root),
            "-DBUILD_TESTING=OFF",
            "-DBUILD_TOOLS=OFF",
            "-DUSE_SWIG_PYTHON=OFF",
            "-DUSE_SWIG_NODE=OFF",
            "-DUSE_SWIG_GO=OFF",
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        env=environment,
    )
    run(
        [
            "cmake",
            "--build",
            str(build_root),
            "--target",
            "fastdb",
            "--config",
            "Release",
            "--parallel",
        ],
        env=environment,
    )
    run(
        [
            sys.executable,
            str(ROOT / "tools/check_payload_abi_symbols.py"),
            "--build-dir",
            str(build_root),
        ],
        env=environment,
    )
    header = ROOT / "fastcarto/fastdb/include/fastdb_payload.h"
    header_source = header.read_text(encoding="utf-8")
    match = re.search(
        r"(?m)^#define FDB_PAYLOAD_V1_ABI_VERSION UINT32_C\((\d+)\)$",
        header_source,
    )
    if match is None or int(match.group(1)) != ABI_VERSION:
        raise CandidateError(
            "FastDB public header must declare payload ABI version 1"
        )
    library = RUST_PACKAGE.locate_system_library(build_root, sys.platform)
    include_dir = bundle / "include"
    library_dir = bundle / "lib"
    include_dir.mkdir(parents=True)
    library_dir.mkdir()
    shutil.copy2(header, include_dir / "fastdb_payload.h")
    relocated = library_dir / library.name
    shutil.copy2(library, relocated)
    inventory = directory_inventory(bundle, label="Core/C ABI bundle")
    expected = {
        "include/fastdb_payload.h",
        f"lib/{library.name}",
    }
    if {item["path"] for item in inventory} != expected:
        raise CandidateError(
            "Core/C ABI bundle must contain only the public payload header "
            "and platform FastDB shared library"
        )
    return relocated


def _command_version(command: list[str], label: str) -> str:
    output = run(command).strip().splitlines()
    if not output:
        raise CandidateError(f"{label} did not report a version")
    value = output[0].strip()
    if os.fspath(ROOT.resolve()) in value:
        raise CandidateError(f"{label} version retained checkout path")
    return value


def _rust_target() -> str:
    output = run(["rustc", "-vV"])
    for line in output.splitlines():
        if line.startswith("host: "):
            return line[len("host: ") :]
    raise CandidateError("rustc -vV did not report a host target")


def _python_identity(executable: Path) -> tuple[str, str]:
    try:
        resolved = executable.resolve(strict=True)
    except FileNotFoundError as error:
        raise CandidateError(
            f"Python executable does not exist: {executable}"
        ) from error
    output = run(
        [
            str(resolved),
            "-c",
            (
                "import platform,sys;"
                "print(f'{platform.python_implementation()} "
                "{sys.version_info.major}.{sys.version_info.minor}."
                "{sys.version_info.micro}');"
                "print(f'{sys.version_info.major}.{sys.version_info.minor}')"
            ),
        ]
    ).splitlines()
    if len(output) != 2:
        raise CandidateError(
            f"Python executable reported malformed identity: {resolved}"
        )
    return output[0], output[1]


def _git_worktree_state() -> tuple[bool, str]:
    status = run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"]
    )
    digest = hashlib.sha256()
    diff = subprocess.run(
        ["git", "diff", "--binary", "HEAD"],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if diff.returncode != 0:
        raise CandidateError(
            "could not capture FastDB worktree diff: "
            + diff.stderr.decode("utf-8", errors="replace")
        )
    digest.update(diff.stdout)
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if untracked.returncode != 0:
        raise CandidateError(
            "could not list FastDB untracked files: "
            + untracked.stderr.decode("utf-8", errors="replace")
        )
    for raw in sorted(item for item in untracked.stdout.split(b"\0") if item):
        relative = raw.decode("utf-8", errors="strict")
        checked_relative_path(relative)
        path = ROOT / relative
        if path.is_file() and not path.is_symlink():
            digest.update(raw)
            digest.update(b"\0")
            digest.update(path.read_bytes())
    return not bool(status), digest.hexdigest()


def source_provenance() -> dict[str, Any]:
    commit = run(["git", "rev-parse", "HEAD"]).strip()
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise CandidateError("git rev-parse did not return a full commit SHA")
    epoch_text = run(
        ["git", "show", "-s", "--format=%ct", commit]
    ).strip()
    try:
        epoch = int(epoch_text)
    except ValueError as error:
        raise CandidateError(
            f"Git commit timestamp is invalid: {epoch_text!r}"
        ) from error
    clean, state_digest = _git_worktree_state()
    return {
        "repository": "fastdb",
        "commit": commit,
        "source_date_epoch": epoch,
        "worktree_clean": clean,
        "worktree_state_sha256": state_digest,
    }


def _copy_exact(source: Path, destination: Path) -> Path:
    if destination.exists():
        raise CandidateError(
            f"artifact destination already exists: {destination}"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return destination


def copy_source_snapshot(destination: Path) -> Path:
    if destination.exists() or destination.is_symlink():
        raise CandidateError(
            f"source snapshot destination must be absent: {destination}"
        )
    raw = subprocess.run(
        [
            "git",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if raw.returncode != 0:
        raise CandidateError(
            "could not enumerate source snapshot: "
            + raw.stderr.decode("utf-8", errors="replace")
        )
    names = sorted(item for item in raw.stdout.split(b"\0") if item)
    if not names:
        raise CandidateError("source snapshot inventory is empty")
    destination.mkdir(parents=True)
    seen: set[str] = set()
    for encoded in names:
        relative = encoded.decode("utf-8", errors="strict")
        checked_relative_path(relative)
        if relative in seen:
            raise CandidateError(
                f"source snapshot contains duplicate path {relative!r}"
            )
        seen.add(relative)
        source = ROOT / relative
        target = destination / relative
        if source.is_symlink():
            raise CandidateError(
                f"source snapshot does not accept symlink {relative!r}"
            )
        if not source.is_file():
            raise CandidateError(
                f"source snapshot member is not a regular file: {relative!r}"
            )
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    return destination


def _exact_files(directory: Path, suffix: str) -> list[Path]:
    return sorted(
        (
            path
            for path in directory.iterdir()
            if path.is_file() and path.name.endswith(suffix)
        ),
        key=lambda path: path.name,
    )


def _build_python_artifacts(
    work_root: Path,
    candidate_root: Path,
    *,
    source_root: Path,
    python_current: Path,
    python_310: Path,
    environment: dict[str, str],
) -> tuple[dict[str, Path], dict[str, str]]:
    current_identity, current_minor = _python_identity(python_current)
    python_310_identity, python_310_minor = _python_identity(python_310)
    if python_310_minor != "3.10":
        raise CandidateError(
            f"--python-310 must be CPython 3.10, received {python_310_identity}"
        )
    current_dist = work_root / "python-current"
    cp310_dist = work_root / "python-cp310"
    current_dist.mkdir()
    cp310_dist.mkdir()
    commands = {
        "cp310": [
            "uv",
            "build",
            "--python",
            str(python_310.resolve(strict=True)),
            "--wheel",
            "--out-dir",
            str(cp310_dist),
            "--no-create-gitignore",
            str(source_root),
        ],
        "current": [
            "uv",
            "build",
            "--python",
            str(python_current.resolve(strict=True)),
            "--sdist",
            "--wheel",
            "--out-dir",
            str(current_dist),
            "--no-create-gitignore",
            str(source_root),
        ],
    }
    # The existing setuptools backend copies its generated extension back into
    # python/fastdb4py/core. Build the non-current compatibility wheel first so
    # the final current-runtime build restores a runnable checkout.
    logs = {
        flavor: run(commands[flavor], env=environment)
        for flavor in PYTHON_BUILD_ORDER
    }
    cp310_log = logs["cp310"]
    current_log = logs["current"]
    current_log_path = work_root / "python-current-build.log"
    cp310_log_path = work_root / "python-cp310-build.log"
    current_log_path.write_text(current_log, encoding="utf-8")
    cp310_log_path.write_text(cp310_log, encoding="utf-8")
    sdists = _exact_files(current_dist, ".tar.gz")
    current_wheels = _exact_files(current_dist, ".whl")
    cp310_wheels = _exact_files(cp310_dist, ".whl")
    if len(sdists) != 1 or len(current_wheels) != 1 or len(cp310_wheels) != 1:
        raise CandidateError(
            "Python build must produce one sdist, one current wheel, and "
            "one CPython-3.10 wheel"
        )
    run(
        [
            sys.executable,
            str(ROOT / "tools/check_python_package_inventory.py"),
            "--dist-dir",
            str(current_dist),
            "--build-log",
            str(current_log_path),
        ],
        env=environment,
    )
    cp310_check = work_root / "python-cp310-check"
    cp310_check.mkdir()
    shutil.copy2(sdists[0], cp310_check / sdists[0].name)
    shutil.copy2(cp310_wheels[0], cp310_check / cp310_wheels[0].name)
    run(
        [
            sys.executable,
            str(ROOT / "tools/check_python_package_inventory.py"),
            "--dist-dir",
            str(cp310_check),
            "--build-log",
            str(cp310_log_path),
        ],
        env=environment,
    )
    artifacts = {
        "sdist": _copy_exact(
            sdists[0], candidate_root / "python" / sdists[0].name
        ),
        "current": _copy_exact(
            current_wheels[0],
            candidate_root / "python/current" / current_wheels[0].name,
        ),
        "cp310": _copy_exact(
            cp310_wheels[0],
            candidate_root / "python/cp310" / cp310_wheels[0].name,
        ),
    }
    identities = {
        "current": current_identity,
        "current_minor": current_minor,
        "cp310": python_310_identity,
    }
    return artifacts, identities


def _build_typescript_artifact(
    work_root: Path,
    candidate_root: Path,
    *,
    source_root: Path,
    environment: dict[str, str],
) -> Path:
    package_root = source_root / "ts/fastdb4ts"
    node_modules = ROOT / "ts/fastdb4ts/node_modules"
    if not node_modules.is_dir():
        raise CandidateError(
            "TypeScript candidate build requires installed "
            "ts/fastdb4ts/node_modules"
        )
    snapshot_node_modules = package_root / "node_modules"
    if snapshot_node_modules.exists() or snapshot_node_modules.is_symlink():
        raise CandidateError(
            "source snapshot unexpectedly contains TypeScript node_modules"
        )
    snapshot_node_modules.symlink_to(
        node_modules.resolve(strict=True), target_is_directory=True
    )
    run(
        ["npm", "--prefix", str(package_root), "run", "build:wasm"],
        env=environment,
    )
    run(
        ["npm", "--prefix", str(package_root), "run", "build"],
        env=environment,
    )
    package_dir = work_root / "typescript"
    package_dir.mkdir()
    run(
        [
            "npm",
            "pack",
            str(package_root),
            "--pack-destination",
            str(package_dir),
        ],
        env=environment,
    )
    packages = _exact_files(package_dir, ".tgz")
    if len(packages) != 1:
        raise CandidateError(
            "TypeScript build must produce exactly one npm tarball"
        )
    run(
        [
            sys.executable,
            str(ROOT / "tests/ci/check_ts_payload_package.py"),
            "--package-dir",
            str(package_dir),
        ],
        env=environment,
    )
    inventory = tar_inventory(
        packages[0], root="package", label="fastdb4ts npm tarball"
    )
    if not any(item["path"].endswith(".wasm") for item in inventory):
        raise CandidateError(
            "fastdb4ts npm tarball must include the rebuilt Wasm module"
        )
    return _copy_exact(
        packages[0],
        candidate_root / "typescript" / packages[0].name,
    )


def _package_identity(name: str, version: str) -> dict[str, str]:
    return {"name": name, "version": version}


def build_candidate(
    output: Path,
    *,
    python_current: Path,
    python_310: Path,
) -> dict[str, Any]:
    require_absent_output(output)
    provenance = source_provenance()
    epoch = provenance["source_date_epoch"]
    environment = _source_environment(epoch)
    output_parent = output.parent.resolve()
    output_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="fastdb-lrc-", dir=output_parent
    ) as temporary:
        work_root = Path(temporary)
        candidate_root = work_root / "candidate"
        candidate_root.mkdir()

        core_bundle = candidate_root / "core"
        core_library = build_core_bundle(
            work_root / "fastdb-core",
            core_bundle,
            source_date_epoch=epoch,
        )

        rust_packages = RUST_PACKAGE.package_archives(
            work_root / "rust-packages",
            source_date_epoch=epoch,
        )
        rust_destinations: dict[str, Path] = {}
        for package, archive in rust_packages.items():
            rust_destinations[package] = _copy_exact(
                archive, candidate_root / "rust" / archive.name
            )
        extracted = RUST_PACKAGE.extract_package_archives(
            rust_packages, work_root / "rust-extracted"
        )
        RUST_PACKAGE.check_extracted_system_consumer(
            extracted,
            core_library,
            work_root / "rust-system-consumer",
        )

        source_snapshot = copy_source_snapshot(work_root / "source")
        python_artifacts, python_identities = _build_python_artifacts(
            work_root,
            candidate_root,
            source_root=source_snapshot,
            python_current=python_current,
            python_310=python_310,
            environment=environment,
        )
        typescript_artifact = _build_typescript_artifact(
            work_root,
            candidate_root,
            source_root=source_snapshot,
            environment=environment,
        )

        rust_target = _rust_target()
        common_toolchains = {
            "cmake": _command_version(["cmake", "--version"], "cmake"),
            "cargo": _command_version(["cargo", "--version"], "cargo"),
            "rustc": _command_version(["rustc", "--version"], "rustc"),
        }
        core_build = build_evidence(
            [
                CORE_CONFIGURE_COMMAND,
                CORE_BUILD_COMMAND,
                ABI_CHECK_COMMAND,
            ],
            {
                "cmake": common_toolchains["cmake"],
                "cxx": _command_version(["c++", "--version"], "c++"),
            },
            target=rust_target,
        )
        rust_build = build_evidence(
            [
                (
                    "python $SOURCE_ROOT/tests/ci/"
                    "build_local_payload_candidate.py "
                    "--output $OUTPUT --python-current $PYTHON_CURRENT "
                    "--python-310 $PYTHON_310"
                ),
                (
                    "cargo package --manifest-path "
                    "$SOURCE_ROOT/bindings/rust/fastdb-sys/Cargo.toml "
                    "--allow-dirty --no-verify "
                    "--target-dir $BUILD_ROOT/rust-package"
                ),
                (
                    "cargo package --manifest-path "
                    "$SOURCE_ROOT/bindings/rust/fastdb/Cargo.toml "
                    "--allow-dirty --no-verify "
                    "--target-dir $BUILD_ROOT/rust-package "
                    "--index $LOCAL_INDEX "
                    "--config source.crates-io.replace-with=fastdb-local "
                    "--config source.fastdb-local.registry=$LOCAL_INDEX"
                ),
                (
                    "cargo run --offline "
                    "--manifest-path $BUILD_ROOT/rust-system-consumer/Cargo.toml"
                ),
            ],
            {
                "cargo": common_toolchains["cargo"],
                "rustc": common_toolchains["rustc"],
            },
            target=rust_target,
        )
        python_common = {
            "uv": _command_version(["uv", "--version"], "uv"),
            "cmake": common_toolchains["cmake"],
            "cxx": _command_version(["c++", "--version"], "c++"),
            "swig": _command_version(["swig", "-version"], "swig"),
        }
        python_current_build = build_evidence(
            [
                (
                    "uv build --python $PYTHON_CURRENT --sdist --wheel "
                    "--out-dir $BUILD_ROOT/python-current "
                    "--no-create-gitignore $SOURCE_SNAPSHOT"
                )
            ],
            {**python_common, "python": python_identities["current"]},
            target=python_identities["current_minor"],
        )
        python_cp310_build = build_evidence(
            [
                (
                    "uv build --python $PYTHON_310 --wheel "
                    "--out-dir $BUILD_ROOT/python-cp310 "
                    "--no-create-gitignore $SOURCE_SNAPSHOT"
                )
            ],
            {**python_common, "python": python_identities["cp310"]},
            target="cp310",
        )
        typescript_build = build_evidence(
            [
                "npm --prefix $SOURCE_SNAPSHOT/ts/fastdb4ts run build:wasm",
                "npm --prefix $SOURCE_SNAPSHOT/ts/fastdb4ts run build",
                (
                    "npm pack $SOURCE_SNAPSHOT/ts/fastdb4ts "
                    "--pack-destination $BUILD_ROOT/typescript"
                ),
            ],
            {
                "node": _command_version(["node", "--version"], "node"),
                "npm": _command_version(["npm", "--version"], "npm"),
                "emcc": _command_version(["emcc", "--version"], "emcc"),
            },
            target="wasm32-unknown-emscripten",
        )

        artifacts = [
            artifact_descriptor(
                candidate_root,
                path="core",
                kind="core-c-abi-bundle",
                package=_package_identity("fastdb-core-c-abi", FASTDB_VERSION),
                build=core_build,
            ),
            artifact_descriptor(
                candidate_root,
                path=rust_destinations["fastdb-sys"]
                .relative_to(candidate_root)
                .as_posix(),
                kind="rust-fastdb-sys-crate",
                package=_package_identity("fastdb-sys", FASTDB_VERSION),
                build=rust_build,
            ),
            artifact_descriptor(
                candidate_root,
                path=rust_destinations["fastdb"]
                .relative_to(candidate_root)
                .as_posix(),
                kind="rust-fastdb-crate",
                package=_package_identity("fastdb", FASTDB_VERSION),
                build=rust_build,
            ),
            artifact_descriptor(
                candidate_root,
                path=python_artifacts["sdist"]
                .relative_to(candidate_root)
                .as_posix(),
                kind="python-sdist",
                package=_package_identity("fastdb4py", FASTDB_VERSION),
                build=python_current_build,
            ),
            artifact_descriptor(
                candidate_root,
                path=python_artifacts["current"]
                .relative_to(candidate_root)
                .as_posix(),
                kind="python-current-wheel",
                package=_package_identity("fastdb4py", FASTDB_VERSION),
                build=python_current_build,
            ),
            artifact_descriptor(
                candidate_root,
                path=python_artifacts["cp310"]
                .relative_to(candidate_root)
                .as_posix(),
                kind="python-cp310-wheel",
                package=_package_identity("fastdb4py", FASTDB_VERSION),
                build=python_cp310_build,
            ),
            artifact_descriptor(
                candidate_root,
                path=typescript_artifact.relative_to(candidate_root).as_posix(),
                kind="typescript-npm-tarball",
                package=_package_identity(
                    "fastdb4ts", FASTDB4TS_VERSION
                ),
                build=typescript_build,
            ),
        ]
        manifest = construct_manifest(
            candidate_root, provenance, artifacts
        )
        manifest_path = candidate_root / MANIFEST_FILENAME
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        verify_manifest(candidate_root)
        shutil.move(str(candidate_root), str(output))
    return verify_manifest(output)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path)
    mode.add_argument("--verify", type=Path)
    parser.add_argument("--python-current", type=Path)
    parser.add_argument("--python-310", type=Path)
    arguments = parser.parse_args()
    if arguments.output is not None:
        if arguments.python_current is None or arguments.python_310 is None:
            parser.error(
                "build mode requires --python-current and --python-310"
            )
    elif arguments.python_current is not None or arguments.python_310 is not None:
        parser.error(
            "--python-current/--python-310 are only valid with --output"
        )
    return arguments


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.verify is not None:
            document = verify_manifest(arguments.verify)
            print(
                "FastDB local payload candidate verified without rebuild: "
                f"{document['source']['commit']}"
            )
        else:
            document = build_candidate(
                arguments.output,
                python_current=arguments.python_current,
                python_310=arguments.python_310,
            )
            print(
                "FastDB local payload candidate built and verified: "
                f"{document['source']['commit']}"
            )
    except (
        CandidateError,
        RUST_PACKAGE.CheckError,
        OSError,
        UnicodeError,
    ) as error:
        print(
            f"FastDB local payload candidate failed: {error}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
