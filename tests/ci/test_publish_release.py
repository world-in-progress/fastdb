#!/usr/bin/env python3
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import publish_release as publish
import release_artifacts as release
import test_release_artifacts


class PublicationTests(unittest.TestCase):
    def test_retry_skips_only_identical_registry_bytes(self):
        record = {"name": "fastdb-0.2.0.crate", "sha256": "a" * 64}
        self.assertEqual(publish.pending_records([record], {record["name"]: record["sha256"]}), [])
        self.assertEqual(publish.pending_records([record], {}), [record])
        with self.assertRaisesRegex(release.ReleaseError, "differs"):
            publish.pending_records([record], {record["name"]: "b" * 64})
        with self.assertRaisesRegex(release.ReleaseError, "differs"):
            publish.pending_records([record], {"unrecorded.crate": "a" * 64})

    def test_wrong_dispatch_source_fails_before_network(self):
        with patch.dict(os.environ, {"GITHUB_SHA": "b" * 40}), patch.object(publish, "github") as api:
            with self.assertRaisesRegex(release.ReleaseError, "workflow SHA"):
                publish.check_ci("123", "a" * 40)
            api.assert_not_called()

    def test_tag_retry_uses_original_main_proofs_after_main_advances(self):
        commit = "a" * 40
        successful = {"head_sha": commit, "head_branch": "main", "event": "push", "conclusion": "success", "status": "completed", "head_repository": {"full_name": publish.REPOSITORY}}

        def api(path):
            if path == "actions/runs/123":
                return {**successful, "id": 123, "run_attempt": 1, "path": ".github/workflows/release-artifacts.yml"}
            if path.startswith("actions/workflows/tests.yml/runs?"):
                self.assertIn(f"head_sha={commit}", path)
                return {"workflow_runs": [{**successful, "id": 456, "path": ".github/workflows/tests.yml"}]}
            if path == "actions/runs/456/jobs?per_page=100":
                return {"jobs": [{"name": f"{name} ({index})", "conclusion": "success"} for name, count in publish.REQUIRED_PROOFS.items() for index in range(count)]}
            if path == "git/ref/heads/main":
                return {"object": {"sha": "b" * 40}}
            self.fail(f"Unexpected API request: {path}")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate"
            candidate.mkdir()
            document = test_release_artifacts.ReleaseInventoryTests().candidate(candidate)
            document.update({"repository": publish.REPOSITORY, "build_run_id": "123", "build_run_attempt": "1"})
            release.write_json(candidate / "release-manifest.json", document)
            published = next(record for record in document["artifacts"] if record["kind"] == "python-sdist")
            with patch.dict(os.environ, {"GITHUB_SHA": commit, "GITHUB_REF": "refs/tags/v0.2.0"}), patch.object(publish, "github", side_effect=api), patch.object(publish, "existing_hashes", return_value={published["name"]: published["sha256"]}):
                plan = publish.prepare(candidate, root / "stage", "pypi", "123", commit)
            self.assertEqual(plan["source_sha"], commit)
            self.assertEqual(plan["dispatch_ref"], "refs/tags/v0.2.0")
            self.assertNotIn(published["name"], plan["pending"])
            self.assertEqual(len(plan["pending"]), 12)

    def test_wrong_release_tag_is_rejected_before_registry_access(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate"
            candidate.mkdir()
            test_release_artifacts.ReleaseInventoryTests().candidate(candidate)
            with patch.dict(os.environ, {"GITHUB_REF": "refs/tags/v0.3.0"}), patch.object(publish, "check_ci", return_value={"artifact_run_attempt": 1}), patch.object(publish, "existing_hashes") as registry:
                with self.assertRaisesRegex(release.ReleaseError, "manifest version"):
                    publish.prepare(candidate, root / "stage", "pypi", "123", "a" * 40)
                registry.assert_not_called()

    def test_feature_branch_and_unversioned_tag_fail_before_network(self):
        for reference in ("refs/heads/feature", "refs/tags/latest", "refs/tags/v0.2.0-unreviewed"):
            with self.subTest(reference=reference), patch.dict(os.environ, {"GITHUB_SHA": "a" * 40, "GITHUB_REF": reference}), patch.object(publish, "github") as api:
                with self.assertRaisesRegex(release.ReleaseError, "versioned release tag"):
                    publish.check_ci("123", "a" * 40)
                api.assert_not_called()

    def test_pr_or_failed_artifact_run_is_not_publishable(self):
        run = {"id": 1, "head_sha": "a" * 40, "head_branch": "main", "event": "push", "conclusion": "success", "status": "completed", "path": ".github/workflows/release-artifacts.yml", "head_repository": {"full_name": publish.REPOSITORY}}
        publish.assert_source_run(run, "a" * 40, "release-artifacts.yml")
        for field, value in (("event", "pull_request"), ("conclusion", "failure"), ("head_sha", "b" * 40), ("head_branch", "release")):
            with self.subTest(field=field), self.assertRaises(release.ReleaseError):
                publish.assert_source_run({**run, field: value}, "a" * 40, "release-artifacts.yml")

    def test_downloaded_manifest_must_match_run_and_attempt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate"
            candidate.mkdir()
            document = test_release_artifacts.ReleaseInventoryTests().candidate(candidate)
            document.update({"repository": publish.REPOSITORY, "build_run_id": "123", "build_run_attempt": "1"})
            release.write_json(candidate / "release-manifest.json", document)
            with patch.dict(os.environ, {"GITHUB_REF": "refs/heads/main"}), patch.object(publish, "check_ci", return_value={"artifact_run_attempt": 2}), patch.object(publish, "existing_hashes") as registry:
                with self.assertRaisesRegex(release.ReleaseError, "run/attempt"):
                    publish.prepare(candidate, root / "stage", "pypi", "123", "a" * 40)
                registry.assert_not_called()

    def test_upload_body_retains_exact_archive_and_dependency_requirement(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "fastdb-0.2.0.crate"
            files = {
                "Cargo.toml": b'[package]\nname="fastdb"\nversion="0.2.0"\nlicense="MIT"\nreadme="README.md"\n[dependencies]\nfastdb-sys={version="=0.2.0"}\n',
                ".cargo_vcs_info.json": json.dumps({"git": {"sha1": "a" * 40}}).encode(),
                "README.md": b"Tested crate documentation",
            }
            with tarfile.open(artifact, "w:gz") as archive:
                for name, contents in files.items():
                    info = tarfile.TarInfo(f"fastdb-0.2.0/{name}")
                    info.size = len(contents)
                    archive.addfile(info, io.BytesIO(contents))
            body = publish.crate_upload_body(artifact, "a" * 40)
            size = struct.unpack_from("<I", body)[0]
            metadata = json.loads(body[4:4 + size])
            self.assertEqual(metadata["deps"][0]["version_req"], "=0.2.0")
            archive_size = struct.unpack_from("<I", body, 4 + size)[0]
            self.assertEqual(body[8 + size:], artifact.read_bytes())
            self.assertEqual(archive_size, artifact.stat().st_size)
            with self.assertRaisesRegex(release.ReleaseError, "VCS provenance"):
                publish.crate_upload_body(artifact, "b" * 40)

    def test_safe_crate_cannot_publish_before_sys_is_indexed(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "packages").mkdir()
            records = []
            for name in ("fastdb-sys", "fastdb"):
                artifact = stage / "packages" / f"{name}-0.2.0.crate"
                artifact.write_bytes(name.encode())
                records.append(release.file_record(artifact, kind="rust-crate"))
            release.write_json(stage / "publish-plan.json", {"ecosystem": "crates", "version": "0.2.0", "source_sha": "a" * 40, "artifacts": records})
            with patch.dict(os.environ, {"CARGO_REGISTRY_TOKEN": "test-only"}), patch.object(publish, "existing_hashes", return_value={}), patch.object(publish, "crate_upload_body", return_value=b"exact crate"), patch.object(publish, "request_json") as upload, patch.object(publish, "wait_for_crate_index", side_effect=release.ReleaseError("not indexed")):
                with self.assertRaisesRegex(release.ReleaseError, "not indexed"):
                    publish.publish_crates(stage)
                self.assertEqual(upload.call_count, 1)

    def test_index_wait_fails_on_missing_or_different_bytes(self):
        with patch.object(publish, "sparse_checksum", return_value=None), patch.object(publish.time, "sleep"):
            with self.assertRaisesRegex(release.ReleaseError, "did not become visible"):
                publish.wait_for_crate_index("fastdb-sys", "0.2.0", "a" * 64, attempts=2)
        with patch.object(publish, "sparse_checksum", return_value="b" * 64):
            with self.assertRaisesRegex(release.ReleaseError, "differs"):
                publish.wait_for_crate_index("fastdb-sys", "0.2.0", "a" * 64)


if __name__ == "__main__":
    unittest.main()
