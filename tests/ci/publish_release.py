#!/usr/bin/env python3
"""Publish tested archives only after exact-source CI and registry hash checks."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tarfile
import time
import tomllib
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from release_artifacts import ReleaseError, digest, verify, write_json


REPOSITORY = "world-in-progress/fastdb"
REQUIRED_PROOFS = {
    "clean_cut": 1, "rust_payload": 1, "python_tests": 1, "ts_tests": 1,
    "projection_parity": 1, "generated_projections": 1, "wasm_core": 1,
    "native_tests": 2, "native_sanitizers": 1, "package_tests": 2, "test": 1,
}


def request_json(url: str, *, token: str | None = None, missing_ok: bool = False, data: bytes | None = None) -> dict | None:
    headers = {"User-Agent": "fastdb-release/0.2.0 (https://github.com/world-in-progress/fastdb)", "Accept": "application/json"}
    if token:
        headers["Authorization"] = token if data is not None else f"Bearer {token}"
    if data is not None:
        headers["Content-Type"] = "application/octet-stream"
    try:
        with urlopen(Request(url, data=data, headers=headers, method="PUT" if data is not None else "GET"), timeout=60) as response:
            result = json.load(response)
    except HTTPError as error:
        if missing_ok and error.code == 404:
            return None
        raise ReleaseError(f"Registry/API request failed with HTTP {error.code}: {url}") from error
    if result.get("errors"):
        raise ReleaseError(f"Registry/API rejected request: {result['errors']}")
    return result


def github(path: str) -> dict:
    token = os.environ.get("GH_TOKEN")
    if not token:
        raise ReleaseError("GH_TOKEN is required to verify source-bound CI evidence")
    result = request_json(f"https://api.github.com/repos/{REPOSITORY}/{path}", token=token)
    assert result is not None
    return result


def assert_source_run(run: dict, commit: str, workflow: str) -> None:
    if not (
        run.get("head_sha") == commit and run.get("head_branch") == "main"
        and run.get("event") == "push" and run.get("conclusion") == "success"
        and run.get("status") == "completed"
        and run.get("path") == f".github/workflows/{workflow}"
        and run.get("head_repository", {}).get("full_name") == REPOSITORY
    ):
        raise ReleaseError(f"Run {run.get('id')} is not a successful exact-source main run of {workflow}")


def check_ci(run_id: str, commit: str) -> dict:
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None or not run_id.isdigit():
        raise ReleaseError("A full source SHA and numeric artifact run ID are required")
    if os.environ.get("GITHUB_SHA") is not None and os.environ["GITHUB_SHA"] != commit:
        raise ReleaseError("Publication workflow SHA must equal the artifact source SHA for registry provenance")
    run = github(f"actions/runs/{run_id}")
    assert_source_run(run, commit, "release-artifacts.yml")
    query = urlencode({"head_sha": commit, "event": "push", "branch": "main", "per_page": 100})
    proofs = github(f"actions/workflows/tests.yml/runs?{query}")["workflow_runs"]
    for proof in proofs:
        try:
            assert_source_run(proof, commit, "tests.yml")
        except ReleaseError:
            continue
        jobs = github(f"actions/runs/{proof['id']}/jobs?per_page=100")["jobs"]
        completed = {}
        for job in jobs:
            if job["conclusion"] == "success":
                name = job["name"].split(" (", 1)[0]
                completed[name] = completed.get(name, 0) + 1
        if all(completed.get(name, 0) >= count for name, count in REQUIRED_PROOFS.items()):
            return {"artifact_run_id": run["id"], "artifact_run_attempt": run["run_attempt"], "proof_run_id": proof["id"], "source_sha": commit}
    raise ReleaseError("No complete successful Run Tests proof matrix exists for this exact main SHA")


def select_records(document: dict, ecosystem: str) -> list[dict]:
    kinds = {"pypi": {"python-wheel", "python-sdist"}, "npm": {"typescript-package"}, "crates": {"rust-crate"}}
    return [record for record in document["artifacts"] if record["kind"] in kinds[ecosystem]]


def existing_hashes(ecosystem: str, version: str) -> dict[str, str]:
    if ecosystem == "pypi":
        result = request_json(f"https://pypi.org/pypi/fastdb4py/{version}/json", missing_ok=True)
        return {} if result is None else {item["filename"]: item["digests"]["sha256"] for item in result["urls"]}
    if ecosystem == "npm":
        result = request_json(f"https://registry.npmjs.org/fastdb4ts/{version}", missing_ok=True)
        if result is None:
            return {}
        # npm exposes SHA-512 integrity; compare the actual published tarball SHA-256.
        import hashlib
        url = f"https://registry.npmjs.org/fastdb4ts/-/fastdb4ts-{version}.tgz"
        with urlopen(url, timeout=60) as response:
            checksum = hashlib.sha256(response.read()).hexdigest()
        return {f"fastdb4ts-{version}.tgz": checksum}
    result = {}
    for name in ("fastdb-sys", "fastdb"):
        package = request_json(f"https://crates.io/api/v1/crates/{name}/{version}", missing_ok=True)
        if package is not None:
            if package["version"].get("yanked"):
                raise ReleaseError(f"Published crate is yanked: {name} {version}")
            result[f"{name}-{version}.crate"] = package["version"]["checksum"]
    return result


def pending_records(records: list[dict], existing: dict[str, str]) -> list[dict]:
    expected = {record["name"]: record["sha256"] for record in records}
    for name, checksum in existing.items():
        if expected.get(name) != checksum:
            raise ReleaseError(f"Published registry identity differs from tested artifact: {name}")
    return [record for record in records if record["name"] not in existing]


def sparse_checksum(name: str, version: str) -> str | None:
    # Both supported package names have at least four characters.
    url = f"https://index.crates.io/{name[:2]}/{name[2:4]}/{name}"
    try:
        with urlopen(Request(url, headers={"User-Agent": "fastdb-release/0.2.0"}), timeout=60) as response:
            rows = response.read().decode("utf-8").splitlines()
    except HTTPError as error:
        if error.code == 404:
            return None
        raise ReleaseError(f"Cargo index request failed with HTTP {error.code}") from error
    for row in rows:
        entry = json.loads(row)
        if entry["vers"] == version:
            if entry.get("yanked"):
                raise ReleaseError(f"Cargo index entry is yanked: {name} {version}")
            return entry["cksum"]
    return None


def wait_for_crate_index(name: str, version: str, checksum: str, *, attempts: int = 24) -> None:
    for attempt in range(attempts):
        visible = sparse_checksum(name, version)
        if visible == checksum:
            return
        if visible is not None:
            raise ReleaseError(f"Cargo index checksum differs from the tested archive: {name}")
        if attempt + 1 < attempts:
            time.sleep(5)
    raise ReleaseError(f"Published crate did not become visible in the Cargo index: {name}")


def prepare(directory: Path, output: Path, ecosystem: str, run_id: str, commit: str) -> dict:
    ci = check_ci(run_id, commit)
    document = verify(directory, commit)
    if document.get("repository") != REPOSITORY or str(document.get("build_run_id")) != run_id or str(document.get("build_run_attempt")) != str(ci["artifact_run_attempt"]):
        raise ReleaseError("Downloaded manifest does not belong to the verified artifact run/attempt")
    records = select_records(document, ecosystem)
    pending = pending_records(records, existing_hashes(ecosystem, document["version"]))
    output.mkdir(parents=True, exist_ok=False)
    packages = output / "packages"
    packages.mkdir()
    for record in pending:
        shutil.copy2(directory / record["name"], packages / record["name"])
    plan = {"schema": "fastdb.registry-publish-plan.v1", "ecosystem": ecosystem, "version": document["version"], "source_sha": commit, "manifest_sha256": digest(directory / "release-manifest.json"), "ci": ci, "artifacts": records, "pending": [record["name"] for record in pending]}
    write_json(output / "publish-plan.json", plan)
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as stream:
            stream.write(f"pending={'true' if pending else 'false'}\n")
    return plan


def crate_upload_body(path: Path, commit: str) -> bytes:
    """Use Cargo's documented registry API to upload the verified .crate unchanged."""
    with tarfile.open(path, "r:gz") as archive:
        root = path.name.removesuffix(".crate")

        def read(relative: str) -> bytes:
            stream = archive.extractfile(f"{root}/{relative}")
            if stream is None:
                raise ReleaseError(f"Crate is missing {relative}")
            return stream.read()

        manifest = tomllib.loads(read("Cargo.toml").decode("utf-8"))
        vcs = json.loads(read(".cargo_vcs_info.json"))
        if vcs.get("git", {}).get("sha1") != commit or vcs.get("git", {}).get("dirty", False):
            raise ReleaseError(f"Crate VCS provenance does not match the tested source: {path.name}")
        package = manifest["package"]
        metadata = {"name": package["name"], "vers": package["version"], "deps": [], "features": manifest.get("features", {}), "authors": package.get("authors", []), "keywords": package.get("keywords", []), "categories": package.get("categories", []), "badges": manifest.get("badges", {})}
        for field in ("description", "documentation", "homepage", "license", "repository", "links"):
            metadata[field] = package.get(field)
        metadata["license_file"] = package.get("license-file")
        metadata["rust_version"] = package.get("rust-version")
        readme = package.get("readme")
        metadata["readme_file"] = readme if isinstance(readme, str) else None
        metadata["readme"] = read(readme).decode("utf-8") if isinstance(readme, str) else None
        for target, group in [(None, manifest), *manifest.get("target", {}).items()]:
            for key, kind in (("dependencies", "normal"), ("build-dependencies", "build"), ("dev-dependencies", "dev")):
                for alias, dependency in group.get(key, {}).items():
                    dependency = {"version": dependency} if isinstance(dependency, str) else dependency
                    if "path" in dependency or "git" in dependency:
                        raise ReleaseError("Published crates must have registry-only dependencies")
                    metadata["deps"].append({"name": dependency.get("package", alias), "version_req": dependency["version"], "features": dependency.get("features", []), "optional": dependency.get("optional", False), "default_features": dependency.get("default-features", True), "target": target, "kind": kind, "registry": dependency.get("registry-index"), "explicit_name_in_toml": alias if "package" in dependency else None})
    metadata_bytes = json.dumps(metadata, separators=(",", ":")).encode("utf-8")
    archive_bytes = path.read_bytes()
    return struct.pack("<I", len(metadata_bytes)) + metadata_bytes + struct.pack("<I", len(archive_bytes)) + archive_bytes


def verify_registry(plan: dict, *, attempts: int = 12) -> None:
    for attempt in range(attempts):
        pending = pending_records(plan["artifacts"], existing_hashes(plan["ecosystem"], plan["version"]))
        if not pending:
            if plan["ecosystem"] == "crates":
                for record in plan["artifacts"]:
                    name = record["name"].removesuffix(f"-{plan['version']}.crate")
                    wait_for_crate_index(name, plan["version"], record["sha256"])
            return
        if attempt + 1 < attempts:
            time.sleep(5)
    raise ReleaseError(f"Registry is missing tested artifacts: {[item['name'] for item in pending]}")


def publish_crates(stage: Path) -> None:
    plan = json.loads((stage / "publish-plan.json").read_text())
    if plan["ecosystem"] != "crates":
        raise ReleaseError("Expected a crates publication plan")
    token = os.environ.get("CARGO_REGISTRY_TOKEN")
    if not token:
        raise ReleaseError("CARGO_REGISTRY_TOKEN is required")
    for name in ("fastdb-sys", "fastdb"):
        filename = f"{name}-{plan['version']}.crate"
        records = [record for record in plan["artifacts"] if record["name"] == filename]
        existing = {key: value for key, value in existing_hashes("crates", plan["version"]).items() if key == filename}
        if not pending_records(records, existing):
            wait_for_crate_index(name, plan["version"], records[0]["sha256"])
            continue
        artifact = stage / "packages" / filename
        if digest(artifact) != records[0]["sha256"]:
            raise ReleaseError(f"Staged crate changed: {filename}")
        request_json("https://crates.io/api/v1/crates/new", token=token, data=crate_upload_body(artifact, plan["source_sha"]))
        # Wait for this crate before uploading its dependent package.
        wait_for_crate_index(name, plan["version"], records[0]["sha256"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check-ci")
    check.add_argument("--run-id", required=True)
    check.add_argument("--source-sha", required=True)
    stage = sub.add_parser("prepare")
    stage.add_argument("--directory", type=Path, required=True)
    stage.add_argument("--output", type=Path, required=True)
    stage.add_argument("--ecosystem", choices=("pypi", "npm", "crates"), required=True)
    stage.add_argument("--run-id", required=True)
    stage.add_argument("--source-sha", required=True)
    for command in ("publish-crates", "verify-registry"):
        item = sub.add_parser(command)
        item.add_argument("--stage", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "check-ci":
        print(json.dumps(check_ci(args.run_id, args.source_sha), sort_keys=True))
    elif args.command == "prepare":
        prepare(args.directory, args.output, args.ecosystem, args.run_id, args.source_sha)
    elif args.command == "publish-crates":
        publish_crates(args.stage)
    else:
        plan = json.loads((args.stage / "publish-plan.json").read_text())
        verify_registry(plan)
        write_json(args.stage / "published.json", {**plan, "status": "published-and-hash-verified"})
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ReleaseError, OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
