#!/usr/bin/env python3
"""Validate Python release files and prepare additive, tag-source backfills."""

from __future__ import annotations

import argparse
from email.parser import BytesParser
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import tomllib
from urllib.parse import urlencode
from urllib.request import urlopen
import zipfile

from release_artifacts import ReleaseError, digest, file_record, wheel_identity, write_json


def source_version(source: Path) -> str:
    return tomllib.loads((source / "pyproject.toml").read_text())["project"]["version"]


def check_metadata(data: bytes, version: str, name: str) -> None:
    metadata = BytesParser().parsebytes(data)
    if metadata["Name"] != "fastdb4py" or metadata["Version"] != version:
        raise ReleaseError(f"Distribution metadata does not match source: {name}")


def inventory(directory: Path, source: Path, build: str, platforms: str) -> list[dict]:
    version = source_version(source)
    python_tags = set()
    for selector in build.split():
        if re.fullmatch(r"cp\d+t?-\*", selector) is None:
            raise ReleaseError(f"Expected an explicit CPython selector: {selector}")
        python_tags.add(selector[:-2])
    platform_tags = set(platforms.split())
    if not python_tags or not platform_tags:
        raise ReleaseError("The expected Python/platform matrix must not be empty")
    expected = {(python, platform) for python in python_tags for platform in platform_tags}
    seen = set()
    records = []
    sdist_name = f"fastdb4py-{version}.tar.gz"
    found_sdist = False
    for path in sorted(directory.iterdir()):
        if path.is_symlink() or not path.is_file():
            raise ReleaseError(f"Distribution must be a regular file: {path.name}")
        # The source-bound release fragment is outside the publishable inventory.
        if path.name == "python.json":
            continue
        if path.suffix == ".whl":
            identity = wheel_identity(path.name, version)
            if identity in seen or identity not in expected:
                raise ReleaseError(f"Duplicate or unexpected wheel slot {identity}: {path.name}")
            with zipfile.ZipFile(path) as wheel:
                metadata = [name for name in wheel.namelist() if name.endswith(".dist-info/METADATA")]
                if len(metadata) != 1:
                    raise ReleaseError(f"Wheel must have exactly one METADATA: {path.name}")
                check_metadata(wheel.read(metadata[0]), version, path.name)
                if not any(name.startswith("fastdb4py/core/_fastdb4py") and name.endswith((".so", ".pyd")) for name in wheel.namelist()):
                    raise ReleaseError(f"Wheel omits its native extension: {path.name}")
            seen.add(identity)
            record = file_record(path, kind="python-wheel")
            record.update(python_abi=identity[0], platform=identity[1])
        elif path.name == sdist_name:
            with tarfile.open(path) as archive:
                metadata = archive.extractfile(f"fastdb4py-{version}/PKG-INFO")
                if metadata is None:
                    raise ReleaseError("sdist omits PKG-INFO")
                check_metadata(metadata.read(), version, path.name)
            found_sdist = True
            record = file_record(path, kind="python-sdist")
        else:
            raise ReleaseError(f"Unexpected distribution file: {path.name}")
        records.append(record)
        print(f"{record['name']}  {record['bytes']} bytes  sha256:{record['sha256']}")
    missing = sorted(expected - seen)
    if missing or not found_sdist:
        raise ReleaseError(f"Incomplete Python distributions; missing wheels: {missing}; missing sdist: {not found_sdist}")
    print(f"Complete: {len(seen)} wheels + 1 sdist")
    return records


def compare_sdist(source: Path, sdist: Path) -> dict:
    """Compare all archived source bytes; allow only setuptools-generated extras."""
    version = source_version(source)
    generated = {"PKG-INFO", "setup.cfg"}
    generated.update("python/fastdb4py.egg-info/" + name for name in (
        "PKG-INFO", "SOURCES.txt", "dependency_links.txt", "entry_points.txt",
        "not-zip-safe", "requires.txt", "top_level.txt",
    ))
    matched = []
    with tarfile.open(sdist) as archive:
        names = set()
        for member in archive:
            if member.isdir():
                continue
            path = PurePosixPath(member.name)
            if not member.isfile() or path.parts[0] != f"fastdb4py-{version}" or ".." in path.parts:
                raise ReleaseError(f"Unsafe sdist member: {member.name}")
            relative = path.relative_to(f"fastdb4py-{version}").as_posix()
            if relative in names:
                raise ReleaseError(f"Duplicate sdist member: {relative}")
            names.add(relative)
            data = archive.extractfile(member).read()
            original = source / relative
            if original.is_file():
                if original.read_bytes() != data:
                    raise ReleaseError(f"Published sdist differs from the tag: {relative}")
                matched.append(relative)
            elif relative not in generated:
                raise ReleaseError(f"Published sdist has source absent from the tag: {relative}")
            if relative == "PKG-INFO":
                check_metadata(data, version, sdist.name)
    required = {"pyproject.toml", "setup.py", "MANIFEST.in", "fastcarto/CMakeLists.txt", "LICENSE", "THIRD_PARTY_NOTICES.txt"}
    if not required <= set(matched):
        raise ReleaseError(f"Published sdist omits required source: {required - set(matched)}")
    return {"sdist": sdist.name, "bytes": sdist.stat().st_size, "sha256": digest(sdist), "matched_source_files": len(matched)}


def resolve_tag(tag: str, root: Path) -> dict:
    if re.fullmatch(r"v\d+\.\d+\.\d+", tag) is None:
        raise ReleaseError("An existing version tag such as v0.2.1 is required")
    commit = subprocess.check_output(["git", "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}"], cwd=root, text=True).strip()
    subprocess.run(["git", "merge-base", "--is-ancestor", commit, "origin/main"], cwd=root, check=True)
    project = tomllib.loads(subprocess.check_output(["git", "show", f"{commit}:pyproject.toml"], cwd=root, text=True))
    if tag != "v" + project["project"]["version"]:
        raise ReleaseError("The selected tag does not match its package version")
    return {"source_sha": commit, "version": tag[1:], "tag": tag}


def check_ci_source(commit: str) -> dict:
    # The controller revision intentionally differs from an old source tag.
    from publish_release import check_ci, github
    query = urlencode({"head_sha": commit, "event": "push", "branch": "main", "status": "success", "per_page": 100})
    runs = github(f"actions/workflows/release-artifacts.yml/runs?{query}")["workflow_runs"]
    for run in runs:
        try:
            return check_ci(str(run["id"]), commit, require_workflow_source=False)
        except ReleaseError:
            continue
    raise ReleaseError("No successful complete main CI evidence exists for the selected tag source")


def stage(records: list[dict], directory: Path, output: Path, version: str, backfill: bool) -> list[dict]:
    from publish_release import request_json
    existing = set()
    if backfill:
        registry = request_json(f"https://pypi.org/pypi/fastdb4py/{version}/json")
        existing = {item["filename"] for item in registry["urls"]}
    pending = [item for item in records if item["name"] not in existing]
    output.mkdir(parents=True, exist_ok=False)
    for item in pending:
        shutil.copy2(directory / item["name"], output / item["name"])
    return pending


def compare_published_sdist(source: Path) -> dict:
    from publish_release import request_json
    version = source_version(source)
    registry = request_json(f"https://pypi.org/pypi/fastdb4py/{version}/json")
    sdists = [item for item in registry["urls"] if item["packagetype"] == "sdist"]
    if len(sdists) != 1:
        raise ReleaseError("Backfill requires one existing PyPI sdist")
    item = sdists[0]
    with tempfile.TemporaryDirectory(prefix="fastdb-published-source-") as temporary:
        path = Path(temporary) / f"fastdb4py-{version}.tar.gz"
        with urlopen(item["url"], timeout=60) as response:
            path.write_bytes(response.read())
        if digest(path) != item["digests"]["sha256"]:
            raise ReleaseError("Published sdist does not match its PyPI SHA-256")
        return compare_sdist(source, path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    resolve = sub.add_parser("resolve")
    resolve.add_argument("--tag", required=True)
    resolve.add_argument("--root", type=Path, default=Path.cwd())
    proof = sub.add_parser("check-ci")
    proof.add_argument("--source-sha", required=True)
    compare = sub.add_parser("compare-sdist")
    compare.add_argument("--source", type=Path, required=True)
    compare.add_argument("--sdist", type=Path, required=True)
    compare.add_argument("--output", type=Path, required=True)
    for command in ("inventory", "stage"):
        check = sub.add_parser(command)
        check.add_argument("--directory", type=Path, required=True)
        check.add_argument("--source", type=Path, required=True)
        check.add_argument("--build", required=True)
        check.add_argument("--platforms", required=True)
        if command == "stage":
            check.add_argument("--output", type=Path, required=True)
            check.add_argument("--backfill", action="store_true")
    args = parser.parse_args()
    if args.command == "resolve":
        result = resolve_tag(args.tag, args.root)
        if path := os.environ.get("GITHUB_OUTPUT"):
            with open(path, "a", encoding="utf-8") as stream:
                for key, value in result.items():
                    stream.write(f"{key}={value}\n")
        print(json.dumps(result))
    elif args.command == "check-ci":
        print(json.dumps(check_ci_source(args.source_sha)))
    elif args.command == "compare-sdist":
        result = compare_sdist(args.source, args.sdist)
        write_json(args.output, result)
        print(json.dumps(result))
    else:
        records = inventory(args.directory, args.source, args.build, args.platforms)
        if args.command == "stage":
            fragment = json.loads((args.directory / "python.json").read_text())
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=args.source, text=True).strip()
            if fragment.get("source_sha") != commit or fragment.get("version") != source_version(args.source) or fragment.get("part") != "python":
                raise ReleaseError("Python fragment does not match the selected source")
            recorded = {item["name"]: (item["bytes"], item["sha256"]) for item in fragment["artifacts"]}
            if recorded != {item["name"]: (item["bytes"], item["sha256"]) for item in records}:
                raise ReleaseError("Python files differ from the tested source-bound fragment")
            if args.backfill:
                print(json.dumps(compare_published_sdist(args.source)))
            pending = stage(records, args.directory, args.output, source_version(args.source), args.backfill)
            if path := os.environ.get("GITHUB_OUTPUT"):
                with open(path, "a", encoding="utf-8") as stream:
                    stream.write(f"pending={'true' if pending else 'false'}\n")
            print(f"Upload only {len(pending)} files: {[item['name'] for item in pending]}")


if __name__ == "__main__":
    try:
        main()
    except (ReleaseError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
