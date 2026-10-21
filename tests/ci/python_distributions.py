#!/usr/bin/env python3
"""Validate Python release files and prepare additive, tag-source backfills."""

from __future__ import annotations

import argparse
import ast
from datetime import datetime, timedelta, timezone
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
from urllib.parse import urlencode, urlparse
from urllib.request import Request, urlopen
import zipfile

from release_artifacts import FRAGMENT_SCHEMA, PYTHON_ABIS, WHEEL_PLATFORM_TARGETS, WHEEL_PYTHON_ABIS, ReleaseError, check_windows_binary, digest, file_record, wheel_identity, write_json


DEFAULT_BUILD = " ".join(abi + "-*" for abi in PYTHON_ABIS)
DEFAULT_PLATFORMS = " ".join(WHEEL_PLATFORM_TARGETS)


def source_commit(source: Path) -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()


def expected_slots(build: str, platforms: str) -> set[tuple[str, str]]:
    python_tags = set()
    for selector in build.split():
        if re.fullmatch(r"cp\d+t?-\*", selector) is None:
            raise ReleaseError(f"Expected an explicit CPython selector: {selector}")
        python_tags.add(selector[:-2])
    platform_tags = set(platforms.split())
    if not python_tags or not platform_tags or not platform_tags <= set(WHEEL_PLATFORM_TARGETS):
        raise ReleaseError("The expected Python/platform matrix must be nonempty and supported")
    slots = {(python, platform) for python in python_tags for platform in platform_tags
             if python in WHEEL_PYTHON_ABIS[WHEEL_PLATFORM_TARGETS[platform]]}
    if not slots:
        raise ReleaseError("No supported Python/platform slots selected")
    return slots


def check_source_platforms(source: Path, slots: set[tuple[str, str]]) -> None:
    if not any(platform == "win_arm64" for _, platform in slots):
        return
    # Old tags must remain byte-for-byte source-identical to their sdist.
    # Detect the known architecture blocker before allocating any builders;
    # environment variables cannot override an explicit CMake -A argument.
    for node in ast.walk(ast.parse((source / "setup.py").read_text())):
        if isinstance(node, (ast.List, ast.Tuple)):
            values = [item.value if isinstance(item, ast.Constant) else None for item in node.elts]
            if any(pair == ["-A", "x64"] for pair in (values[i:i + 2] for i in range(len(values) - 1))):
                raise ReleaseError(
                    f"Source {source_version(source)} forces CMake -A x64 in setup.py; Windows ARM64 "
                    "needs the architecture fix in a new release. Do not patch or move the existing tag."
                )


def public_registry(version: str) -> dict:
    from publish_release import request_json
    return request_json(f"https://pypi.org/pypi/fastdb4py/{version}/json", no_cache=True)


def source_version(source: Path) -> str:
    return tomllib.loads((source / "pyproject.toml").read_text())["project"]["version"]


def check_metadata(data: bytes, version: str, name: str) -> None:
    metadata = BytesParser().parsebytes(data)
    if metadata["Name"] != "fastdb4py" or metadata["Version"] != version:
        raise ReleaseError(f"Distribution metadata does not match source: {name}")


def inventory(directory: Path, source: Path, build: str, platforms: str) -> list[dict]:
    version = source_version(source)
    expected = expected_slots(build, platforms)
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
                if identity[1].startswith("win_"):
                    extensions = [name for name in wheel.namelist()
                                  if name.startswith("fastdb4py/core/_fastdb4py") and name.endswith(".pyd")]
                    if len(extensions) != 1:
                        raise ReleaseError(f"Windows wheel requires one native extension: {path.name}")
                    check_windows_binary(wheel.read(extensions[0]), WHEEL_PLATFORM_TARGETS[identity[1]], path.name)
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


def check_ci_source(commit: str, *, require_artifact: bool = False) -> dict:
    # The controller revision intentionally differs from an old source tag.
    from publish_release import check_ci, github
    query = urlencode({"head_sha": commit, "event": "push", "branch": "main", "status": "success", "per_page": 100})
    runs = github(f"actions/workflows/release-artifacts.yml/runs?{query}")["workflow_runs"]
    for run in runs:
        try:
            proof = check_ci(str(run["id"]), commit, require_workflow_source=False)
        except ReleaseError:
            continue
        if not require_artifact:
            return proof
        artifacts = github(f"actions/runs/{run['id']}/artifacts?per_page=100")["artifacts"]
        available = [item for item in artifacts if item["name"] == "release-part-python" and not item["expired"]]
        if len(available) != 1:
            continue
        artifact = available[0]
        if (artifact.get("workflow_run", {}).get("head_sha") != commit
                or str(artifact["workflow_run"].get("id")) != str(run["id"])
                or re.fullmatch(r"sha256:[0-9a-f]{64}", artifact.get("digest", "")) is None):
            continue
        return {**proof, "python_artifact_id": artifact["id"], "python_artifact_digest": artifact["digest"]}
    if require_artifact:
        raise ReleaseError("No complete exact-source CI run has an unexpired tested Python artifact; publication never rebuilds it")
    raise ReleaseError("No successful complete main CI evidence exists for the selected tag source")


def check_backfill_window(version: str, *, now: datetime | None = None, registry: dict | None = None) -> dict:
    if registry is None:
        registry = public_registry(version)
    try:
        uploads = [datetime.fromisoformat(item["upload_time_iso_8601"].replace("Z", "+00:00")) for item in registry["urls"]]
        if not uploads or any(value.tzinfo is None for value in uploads):
            raise ValueError("Missing timezone-aware timestamps")
    except (KeyError, TypeError, ValueError) as error:
        raise ReleaseError("Cannot check PyPI backfill age: valid upload timestamps are required") from error
    first = min(uploads).astimezone(timezone.utc)
    # PyPI exposes surviving file times, not release creation time. An old
    # file proves expiry; recent files cannot prove a release is still open.
    # https://blog.pypi.org/posts/2026-07-22-releases-now-reject-new-files-after-14-days/
    deadline = first + timedelta(days=14)
    if (now or datetime.now(timezone.utc)) > deadline:
        raise ReleaseError(
            f"PyPI's 14-day upload window is closed for fastdb4py {version} "
            f"(first existing file: {first.isoformat()}). Publish a new version; "
            "skip-existing cannot extend the upload window."
        )
    return {"version": version, "first_existing_upload": first.isoformat(), "observed_upload_deadline": deadline.isoformat()}


def stage(records: list[dict], directory: Path, output: Path, version: str, backfill: bool, *, registry: dict | None = None) -> list[dict]:
    existing = set()
    if backfill:
        registry = public_registry(version) if registry is None else registry
        existing = {item["filename"] for item in registry["urls"]}
    pending = [item for item in records if item["name"] not in existing]
    if backfill and pending:
        check_backfill_window(version, registry=registry)
    output.mkdir(parents=True, exist_ok=False)
    for item in pending:
        shutil.copy2(directory / item["name"], output / item["name"])
    return pending


def published_file(item: dict, output: Path) -> Path:
    name = item["filename"]
    url = urlparse(item["url"])
    checksum = item["digests"]["sha256"]
    if (PurePosixPath(name).name != name or "\\" in name or name in ("", ".", "..")
            or url.scheme != "https" or url.hostname != "files.pythonhosted.org" or url.username
            or re.fullmatch(r"[0-9a-f]{64}", checksum) is None):
        raise ReleaseError("Invalid published distribution filename, URL or hash")
    with urlopen(Request(item["url"], headers={"Cache-Control": "no-cache"}), timeout=60) as response:
        data = response.read()
    path = output / name
    path.write_bytes(data)
    if digest(path) != checksum or ("size" in item and len(data) != item["size"]):
        raise ReleaseError(f"Published distribution does not match its PyPI hash/size: {name}")
    return path


def compare_published_sdist(source: Path, registry: dict | None = None) -> dict:
    version = source_version(source)
    registry = public_registry(version) if registry is None else registry
    sdists = [item for item in registry["urls"] if item["packagetype"] == "sdist"]
    if len(sdists) != 1:
        raise ReleaseError("Backfill requires one existing PyPI sdist")
    item = sdists[0]
    with tempfile.TemporaryDirectory(prefix="fastdb-published-source-") as temporary:
        path = published_file(item, Path(temporary))
        return compare_sdist(source, path)


def plan_build(source: Path, build: str, platforms: str, runners: dict, reused: Path, *, backfill: bool = False) -> dict:
    """Choose exact missing slots before allocating any build runners."""
    version = source_version(source)
    expected = expected_slots(build, platforms)
    if set(runners) != set(platforms.split()):
        raise ReleaseError("Runner declarations do not match the wheel platforms")
    plan = {"schema": "fastdb.python-build-plan.v1", "source_sha": source_commit(source), "version": version,
            "build": build, "platforms": platforms, "backfill": backfill, "reused": []}
    missing = set(expected)
    if backfill:
        registry = public_registry(version)
        sdists = [item for item in registry["urls"] if item.get("packagetype") == "sdist"]
        if len(sdists) != 1 or sdists[0]["filename"] != f"fastdb4py-{version}.tar.gz":
            raise ReleaseError("Backfill requires the published source distribution")
        selected = {}
        for item in registry["urls"]:
            if not item["filename"].endswith(".whl"):
                continue
            try:
                identity = wheel_identity(item["filename"], version)
            except ReleaseError:
                continue  # Preserve historical files outside the current policy.
            if identity in expected:
                if identity in selected:
                    raise ReleaseError(f"Duplicate published wheel slot: {identity}")
                if item.get("yanked"):
                    raise ReleaseError(f"Required published wheel is yanked: {item['filename']}")
                selected[identity] = item
        missing -= selected.keys()
        if missing:
            check_backfill_window(version, registry=registry)
        check_source_platforms(source, missing)
        reused.mkdir(parents=True, exist_ok=False)
        sdist = published_file(sdists[0], reused)
        plan["sdist_comparison"] = compare_sdist(source, sdist)
        # A complete release needs no wheel downloads, builds or upload jobs.
        if missing:
            plan["reused"].append(file_record(sdist, kind="python-sdist"))
            for item in sorted(selected.values(), key=lambda item: item["filename"]):
                plan["reused"].append(file_record(published_file(item, reused), kind="python-wheel"))
    if not backfill:
        check_source_platforms(source, missing)
    plan["missing"] = [list(slot) for slot in sorted(missing)]
    plan["build_sdist"] = not backfill
    plan["pending"] = bool(missing) or not backfill
    matrix = []
    for platform in platforms.split():
        selectors = [f"{abi}-{platform}" for abi, candidate in sorted(missing) if candidate == platform]
        if selectors:
            matrix.append({**runners[platform], "platform": platform, "build": " ".join(selectors)})
    plan["matrix"] = {"include": matrix}
    return plan


def complete_build(plan: dict, source: Path, built: Path, reused: Path, output: Path,
                   build: str, platforms: str) -> list[dict]:
    """Merge verified existing bytes with exactly the wheels that were planned."""
    if (plan.get("schema") != "fastdb.python-build-plan.v1" or plan.get("source_sha") != source_commit(source)
            or plan.get("version") != source_version(source) or plan.get("build") != build
            or plan.get("platforms") != platforms or not plan.get("pending")):
        raise ReleaseError("Build plan does not match the selected source and release policy")
    expected = {tuple(slot) for slot in plan["missing"]}
    if not expected <= expected_slots(build, platforms):
        raise ReleaseError("Build plan contains unexpected wheel slots")
    seen = set()
    sdist_found = False
    built_files = sorted(built.iterdir())
    for path in built_files:
        if not path.is_file() or path.is_symlink():
            raise ReleaseError(f"Built distribution must be a regular file: {path.name}")
        if path.suffix == ".whl":
            identity = wheel_identity(path.name, plan["version"])
            if identity not in expected or identity in seen:
                raise ReleaseError(f"Built an unplanned or duplicate wheel: {path.name}")
            seen.add(identity)
        elif path.name == f"fastdb4py-{plan['version']}.tar.gz" and plan["build_sdist"]:
            sdist_found = True
        else:
            raise ReleaseError(f"Unplanned built distribution: {path.name}")
    if seen != expected or sdist_found != plan["build_sdist"]:
        raise ReleaseError("Build did not produce every planned wheel/sdist")
    retained = {item["name"]: item for item in plan["reused"]}
    reused_files = sorted(reused.iterdir()) if reused.exists() else []
    if len(retained) != len(plan["reused"]) or {path.name for path in reused_files} != set(retained):
        raise ReleaseError("Reused distributions differ from the registry snapshot")
    for path in reused_files:
        item = retained[path.name]
        if (not path.is_file() or path.is_symlink() or path.stat().st_size != item["bytes"]
                or digest(path) != item["sha256"]):
            raise ReleaseError(f"Reused distribution hash/size changed: {path.name}")
    output.mkdir(parents=True, exist_ok=False)
    for path in [*reused_files, *built_files]:
        if (output / path.name).exists():
            raise ReleaseError(f"Built a distribution that should have been reused: {path.name}")
        shutil.copy2(path, output / path.name)
    return inventory(output, source, build, platforms)


def verify_fragment(directory: Path, source: Path, records: list[dict]) -> None:
    fragment = json.loads((directory / "python.json").read_text())
    if (fragment.get("schema") != FRAGMENT_SCHEMA or fragment.get("source_sha") != source_commit(source)
            or fragment.get("version") != source_version(source) or fragment.get("part") != "python"):
        raise ReleaseError("Python fragment does not match the selected source")
    recorded = {item["name"]: (item["bytes"], item["sha256"]) for item in fragment["artifacts"]}
    if (len(recorded) != len(fragment["artifacts"])
            or recorded != {item["name"]: (item["bytes"], item["sha256"]) for item in records}):
        raise ReleaseError("Python files differ from the tested source-bound fragment")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    resolve = sub.add_parser("resolve")
    resolve.add_argument("--tag", required=True)
    resolve.add_argument("--root", type=Path, default=Path.cwd())
    proof = sub.add_parser("check-ci")
    proof.add_argument("--source-sha", required=True)
    proof.add_argument("--require-artifact", action="store_true")
    window = sub.add_parser("check-backfill")
    window.add_argument("--version", required=True)
    compare = sub.add_parser("compare-sdist")
    compare.add_argument("--source", type=Path, required=True)
    compare.add_argument("--sdist", type=Path, required=True)
    compare.add_argument("--output", type=Path, required=True)
    plan = sub.add_parser("plan")
    plan.add_argument("--source", type=Path, required=True)
    plan.add_argument("--build", required=True)
    plan.add_argument("--platforms", required=True)
    plan.add_argument("--runners", required=True)
    plan.add_argument("--reused", type=Path, required=True)
    plan.add_argument("--output", type=Path, required=True)
    plan.add_argument("--backfill", action="store_true")
    complete = sub.add_parser("complete")
    complete.add_argument("--plan", type=Path, required=True)
    complete.add_argument("--source", type=Path, required=True)
    complete.add_argument("--built", type=Path, required=True)
    complete.add_argument("--reused", type=Path, required=True)
    complete.add_argument("--output", type=Path, required=True)
    complete.add_argument("--build", required=True)
    complete.add_argument("--platforms", required=True)
    for command in ("inventory", "stage"):
        check = sub.add_parser(command)
        check.add_argument("--directory", type=Path, required=True)
        check.add_argument("--source", type=Path, required=True)
        check.add_argument("--build", default=DEFAULT_BUILD)
        check.add_argument("--platforms", default=DEFAULT_PLATFORMS)
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
        result = check_ci_source(args.source_sha, require_artifact=args.require_artifact)
        if path := os.environ.get("GITHUB_OUTPUT"):
            with open(path, "a", encoding="utf-8") as stream:
                for key, value in result.items():
                    stream.write(f"{key}={value}\n")
        print(json.dumps(result))
    elif args.command == "check-backfill":
        print(json.dumps(check_backfill_window(args.version)))
    elif args.command == "compare-sdist":
        result = compare_sdist(args.source, args.sdist)
        write_json(args.output, result)
        print(json.dumps(result))
    elif args.command == "plan":
        result = plan_build(args.source, args.build, args.platforms, json.loads(args.runners), args.reused,
                            backfill=args.backfill)
        write_json(args.output, result)
        if path := os.environ.get("GITHUB_OUTPUT"):
            with open(path, "a", encoding="utf-8") as stream:
                for key in ("pending", "build_sdist"):
                    stream.write(f"{key}={str(result[key]).lower()}\n")
                stream.write(f"reused={str(bool(result['reused'])).lower()}\n")
                stream.write("matrix=" + json.dumps(result["matrix"], separators=(",", ":")) + "\n")
        print(json.dumps({key: result[key] for key in ("missing", "matrix", "pending", "build_sdist")}))
    elif args.command == "complete":
        complete_build(json.loads(args.plan.read_text()), args.source, args.built, args.reused, args.output,
                       args.build, args.platforms)
    else:
        records = inventory(args.directory, args.source, args.build, args.platforms)
        if args.command == "stage":
            verify_fragment(args.directory, args.source, records)
            registry = None
            if args.backfill:
                registry = public_registry(source_version(args.source))
                sdists = [item for item in registry["urls"] if item.get("packagetype") == "sdist"]
                local_sdist = args.directory / f"fastdb4py-{source_version(args.source)}.tar.gz"
                if len(sdists) != 1 or digest(local_sdist) != sdists[0]["digests"]["sha256"]:
                    raise ReleaseError("Published source distribution changed since backfill planning")
                print(json.dumps(compare_sdist(args.source, local_sdist)))
            pending = stage(records, args.directory, args.output, source_version(args.source), args.backfill, registry=registry)
            if path := os.environ.get("GITHUB_OUTPUT"):
                with open(path, "a", encoding="utf-8") as stream:
                    stream.write(f"pending={'true' if pending else 'false'}\n")
            print(f"Upload only {len(pending)} files: {[item['name'] for item in pending]}")


if __name__ == "__main__":
    try:
        main()
    except (ReleaseError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
