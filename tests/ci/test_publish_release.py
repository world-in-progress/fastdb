#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile
import unittest
from urllib.error import HTTPError
import zipfile
from unittest.mock import patch

import publish_release as publish
import release_artifacts as release
import test_release_artifacts


class PublicationTests(unittest.TestCase):
    def test_empty_publish_inputs_fail_before_api_or_artifact_access(self):
        for run_id, commit in (("", ""), ("", "a" * 40), ("123", "")):
            with self.subTest(run_id=run_id, source_sha=commit), patch.object(publish, "github") as api:
                with self.assertRaisesRegex(release.ReleaseError, "full source SHA and numeric artifact run ID"):
                    publish.check_ci(run_id, commit)
                api.assert_not_called()

    def test_retry_skips_only_identical_registry_bytes(self):
        record = {"name": "fastdb-0.2.0.crate", "sha256": "a" * 64}
        self.assertEqual(publish.pending_records([record], {record["name"]: record["sha256"]}), [])
        self.assertEqual(publish.pending_records([record], {}), [record])
        with self.assertRaisesRegex(release.ReleaseError, "differs"):
            publish.pending_records([record], {record["name"]: "b" * 64})
        with self.assertRaisesRegex(release.ReleaseError, "differs"):
            publish.pending_records([record], {"unrecorded.crate": "a" * 64})

    def test_accepted_npm_upload_can_become_visible_after_one_minute(self):
        record = {"name": "fastdb4ts-0.2.3.tgz", "sha256": "a" * 64}
        plan = {"ecosystem": "npm", "version": "0.2.3", "artifacts": [record]}
        responses = [{}] * 13 + [{record["name"]: record["sha256"]}]
        with patch.object(publish, "existing_hashes", side_effect=responses) as registry, \
                patch.object(publish.time, "sleep") as sleep:
            publish.verify_registry(plan)
        self.assertEqual(registry.call_count, 14)
        self.assertEqual(sleep.call_count, 13)

    def test_registry_hash_mismatch_fails_without_waiting_or_republishing(self):
        record = {"name": "fastdb4ts-0.2.3.tgz", "sha256": "a" * 64}
        plan = {"ecosystem": "npm", "version": "0.2.3", "artifacts": [record]}
        with patch.object(publish, "existing_hashes", return_value={record["name"]: "b" * 64}), \
                patch.object(publish.time, "sleep") as sleep:
            with self.assertRaisesRegex(release.ReleaseError, "differs"):
                publish.verify_registry(plan)
        sleep.assert_not_called()

    def test_version_metadata_refreshes_cached_registry_misses(self):
        with patch.object(publish, "request_json", return_value=None) as request:
            self.assertEqual(publish.existing_hashes("npm", "0.2.3"), {})
        request.assert_called_once_with("https://registry.npmjs.org/fastdb4ts/0.2.3", missing_ok=True, no_cache=True)

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
            # 30 original-platform wheels + five Windows ARM64 wheels;
            # the published sdist is excluded from upload.
            self.assertEqual(len(plan["pending"]), 35)

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

    def test_windows_and_java_proofs_are_required_before_publication(self):
        commit = "a" * 40
        successful = {"head_sha": commit, "head_branch": "main", "event": "push",
                      "conclusion": "success", "status": "completed",
                      "head_repository": {"full_name": publish.REPOSITORY}}
        # Independent inventory: a second passing Linux Python job cannot
        # substitute for either Windows job.
        counts = {"clean_cut": 1, "rust_payload": 1, "python_tests": 2, "ts_tests": 1,
                  "projection_parity": 1, "generated_projections": 1, "wasm_core": 1,
                  "native_tests": 2, "native_sanitizers": 1, "package_tests": 2, "test": 1,
                  "windows_native_tests": 1, "windows_python_tests": 1, "java_payload": 3}
        complete = [{"name": f"{name} ({index})", "conclusion": "success"}
                    for name, count in counts.items() for index in range(count)]
        jobs = complete

        def api(path):
            if path == "actions/runs/123":
                return {**successful, "id": 123, "run_attempt": 1,
                        "path": ".github/workflows/release-artifacts.yml"}
            if path.startswith("actions/workflows/tests.yml/runs?"):
                return {"workflow_runs": [{**successful, "id": 456,
                                          "path": ".github/workflows/tests.yml"}]}
            if path == "actions/runs/456/jobs?per_page=100":
                return {"jobs": jobs}
            self.fail(f"Unexpected API request: {path}")

        with patch.dict(os.environ, {"GITHUB_SHA": commit, "GITHUB_REF": "refs/heads/main"}), \
                patch.object(publish, "github", side_effect=api):
            self.assertEqual(publish.check_ci("123", commit)["proof_run_id"], 456)
            for missing in ("windows_native_tests", "windows_python_tests"):
                for result in (None, "failure", "skipped", "cancelled"):
                    with self.subTest(job=missing, result=result):
                        jobs = [dict(job) for job in complete if not job["name"].startswith(missing + " (")]
                        if result is not None:
                            jobs.append({"name": missing, "conclusion": result})
                        with self.assertRaisesRegex(release.ReleaseError, "complete successful Run Tests proof matrix"):
                            publish.check_ci("123", commit)
            jobs = [job for job in complete if job["name"] != "java_payload (2)"]
            with self.assertRaisesRegex(release.ReleaseError, "complete successful Run Tests proof matrix"):
                publish.check_ci("123", commit)
            with patch.object(publish.subprocess, "check_output", return_value="jobs:\n  java_payload:\n"):
                with self.assertRaisesRegex(release.ReleaseError, "complete successful Run Tests proof matrix"):
                    publish.check_ci("123", commit, require_workflow_source=False)
            with patch.object(publish.subprocess, "check_output", return_value="jobs:\n  test:\n"):
                self.assertEqual(publish.check_ci("123", commit, require_workflow_source=False)["proof_run_id"], 456)

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


class MavenPublicationTests(unittest.TestCase):
    version = "0.4.0"
    names = ("fastdb-java-0.4.0.jar", "fastdb-java-0.4.0-sources.jar",
             "fastdb-java-0.4.0-javadoc.jar", "fastdb-java-0.4.0.pom")
    deployment_id = "28570f16-da32-4c14-bd2e-c1acc0782365"
    prefix = "io/github/world-in-progress/fastdb-java/0.4.0/"

    def write_stage(self, stage):
        packages = stage / "packages"
        packages.mkdir()
        artifacts = []
        for name, kind in zip(self.names, ("maven-jar", "maven-sources", "maven-javadoc", "maven-pom")):
            path = packages / name
            path.write_bytes(name.encode())
            artifacts.append(release.file_record(path, kind=kind))
        plan = {"ecosystem": "maven", "version": self.version, "source_sha": "a" * 40, "artifacts": artifacts, "pending": list(self.names)}
        release.write_json(stage / "publish-plan.json", plan)
        return plan

    def test_maven_stages_the_full_coordinate_or_waits_for_visibility(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / "candidate"
            candidate.mkdir()
            document = test_release_artifacts.ReleaseInventoryTests().candidate(candidate)
            document.update(repository=publish.REPOSITORY, build_run_id="123", build_run_attempt="1")
            release.write_json(candidate / "release-manifest.json", document)
            records = publish.select_records(document, "maven")
            self.assertEqual(len(records), 4)
            first = records[0]
            with patch.dict(os.environ, {"GITHUB_REF": "refs/heads/main"}), \
                    patch.object(publish, "check_ci", return_value={"artifact_run_attempt": 1}), \
                    patch.object(publish, "existing_hashes", side_effect=[{}, {first["name"]: first["sha256"]},
                                                                        {first["name"]: "0" * 64}]):
                stage = root / "initial"
                plan = publish.prepare(candidate, stage, "maven", "123", "a" * 40)
                self.assertEqual(set(plan["pending"]), {record["name"] for record in records})
                for name in plan["pending"]:
                    self.assertEqual((stage / "packages" / name).read_bytes(), (candidate / name).read_bytes())
                retry = publish.prepare(candidate, root / "retry", "maven", "123", "a" * 40)
                self.assertEqual(retry["pending"], [])
                self.assertEqual(retry["artifacts"], records)
                with self.assertRaisesRegex(release.ReleaseError, "differs"):
                    publish.prepare(candidate, root / "mismatch", "maven", "123", "a" * 40)

    def test_maven_downloads_public_bytes_and_only_ignores_404(self):
        def open_artifact(request, *, timeout):
            filename = request.full_url.split("/")[-1]
            self.assertEqual(request.full_url, "https://repo1.maven.org/maven2/" + self.prefix + filename)
            self.assertEqual(request.get_header("User-agent"), publish.MAVEN_USER_AGENT)
            self.assertEqual(request.get_header("Cache-control"), "no-cache")
            if filename == self.names[-1]:
                raise HTTPError(request.full_url, missing_code, "missing", {}, None)
            return io.BytesIO(filename.encode())

        missing_code = 404
        with patch.object(publish, "urlopen", side_effect=open_artifact):
            hashes = publish.existing_hashes("maven", self.version)
            self.assertEqual(hashes, {name: hashlib.sha256(name.encode()).hexdigest() for name in self.names[:-1]})
            missing_code = 503
            with self.assertRaisesRegex(release.ReleaseError, "HTTP 503"):
                publish.existing_hashes("maven", self.version)

    def test_maven_waits_for_all_public_bytes_and_rejects_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            plan = self.write_stage(Path(directory))
            complete = {record["name"]: record["sha256"] for record in plan["artifacts"]}
            with patch.object(publish, "existing_hashes", side_effect=[{}] * 120 + [complete]) as registry, \
                    patch.object(publish.time, "sleep"):
                publish.verify_registry(plan)
                self.assertEqual(registry.call_count, 121)
            with patch.object(publish, "existing_hashes", return_value={**complete, self.names[0]: "1" * 64}), \
                    patch.object(publish.time, "sleep") as sleep:
                with self.assertRaisesRegex(release.ReleaseError, "differs"):
                    publish.verify_registry(plan)
                sleep.assert_not_called()

    def test_maven_rejects_changed_file_before_signing_or_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            self.write_stage(stage)
            path = stage / "packages" / self.names[0]
            path.write_bytes(b"x" * len(path.name))
            with patch.object(publish.subprocess, "run") as gpg, patch.object(publish, "urlopen") as api:
                with self.assertRaisesRegex(release.ReleaseError, "Staged Maven artifact changed"):
                    publish.publish_maven(stage)
                path.unlink()
                with self.assertRaisesRegex(release.ReleaseError, "do not match the release inventory"):
                    publish.publish_maven(stage)
                gpg.assert_not_called()
                api.assert_not_called()

    def test_maven_uploads_signed_bundle_and_polls_portal(self):
        def gpg(args, *, input, env, capture_output):
            self.assertEqual(args[:4], ["gpg", "--no-options", "--batch", "--yes"])
            home = Path(env["GNUPGHOME"])
            self.assertEqual(home.stat().st_mode & 0o777, 0o700)
            self.assertNotIn("MAVEN_GPG_PRIVATE_KEY", env)
            if args[4] == "--import":
                self.assertEqual(input, b"private key")
            else:
                self.assertEqual(args[4:10], ["--armor", "--detach-sign", "--pinentry-mode", "loopback",
                                               "--passphrase-fd", "0"])
                self.assertEqual(input, b"passphrase\n")
                path = Path(args[-1])
                Path(args[-2]).write_bytes(b"detached signature " + path.read_bytes())
            return subprocess.CompletedProcess(args, 0)

        def api(request, *, timeout):
            self.assertEqual(request.get_method(), "POST")
            self.assertEqual(request.get_header("User-agent"), publish.MAVEN_USER_AGENT)
            self.assertEqual(request.get_header("Authorization"), "Bearer dXNlcjpwYXNz")
            if request.full_url == "https://central.sonatype.com/api/v1/publisher/upload?publishingType=AUTOMATIC":
                boundary = request.get_header("Content-type").split("boundary=", 1)[1].encode()
                headers, body = request.data.split(b"\r\n\r\n", 1)
                self.assertTrue(headers.startswith(b"--" + boundary))
                self.assertIn(b'name="bundle"; filename="fastdb-java.zip"', headers)
                self.assertIn(b"Content-Type: application/octet-stream", headers)
                with zipfile.ZipFile(io.BytesIO(body.removesuffix(b"\r\n--" + boundary + b"--\r\n"))) as bundle:
                    self.assertEqual(len(bundle.namelist()), 16)
                    for name in self.names:
                        member = self.prefix + name
                        artifact = name.encode()
                        self.assertEqual(bundle.read(member), artifact)
                        self.assertEqual(bundle.read(member + ".md5").decode(), hashlib.md5(artifact, usedforsecurity=False).hexdigest())
                        self.assertEqual(bundle.read(member + ".sha1").decode(), hashlib.sha1(artifact, usedforsecurity=False).hexdigest())
                        self.assertEqual(bundle.read(member + ".asc"), b"detached signature " + artifact)
                return io.BytesIO(self.deployment_id.encode())
            self.assertEqual(request.full_url, "https://central.sonatype.com/api/v1/publisher/status?id=" + self.deployment_id)
            return io.BytesIO(json.dumps({"deploymentState": next(states)}).encode())

        states = iter(["PENDING", "VALIDATING", "PUBLISHING", "PUBLISHED"])
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            self.write_stage(stage)
            with patch.dict(os.environ, {"MAVEN_CENTRAL_USERNAME": "user", "MAVEN_CENTRAL_PASSWORD": "pass",
                                         "MAVEN_GPG_PRIVATE_KEY": "private key", "MAVEN_GPG_PASSPHRASE": "passphrase"}), \
                    patch.object(publish.subprocess, "run", side_effect=gpg) as signer, \
                    patch.object(publish, "urlopen", side_effect=api) as portal, \
                    patch.object(publish.time, "sleep") as sleep, patch("builtins.print"):
                publish.publish_maven(stage)
            self.assertEqual(signer.call_count, 5)
            self.assertEqual(portal.call_count, 5)
            self.assertEqual(sleep.call_count, 3)
            self.assertFalse(Path(signer.call_args.kwargs["env"]["GNUPGHOME"]).exists())
            recorded = json.loads((stage / "deployment.json").read_text())
            self.assertEqual(recorded["deployment_id"], self.deployment_id)
            self.assertFalse(recorded["resumed"])

    def test_maven_poll_retries_transient_503_and_can_resume_existing_deployment(self):
        responses = iter([
            HTTPError("https://central.sonatype.com/api/v1/publisher/status", 503, "unavailable", {}, io.BytesIO(b"busy")),
            io.BytesIO(json.dumps({"deploymentState": "PUBLISHING"}).encode()),
            io.BytesIO(json.dumps({"deploymentState": "PUBLISHED"}).encode()),
        ])

        def api(request, *, timeout):
            self.assertEqual(request.full_url, "https://central.sonatype.com/api/v1/publisher/status?id=" + self.deployment_id)
            item = next(responses)
            if isinstance(item, Exception):
                raise item
            return item

        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            self.write_stage(stage)
            with patch.dict(os.environ, {"MAVEN_CENTRAL_USERNAME": "user", "MAVEN_CENTRAL_PASSWORD": "pass"}, clear=True), \
                    patch.object(publish.subprocess, "run") as signer, \
                    patch.object(publish, "urlopen", side_effect=api) as portal, \
                    patch.object(publish.time, "sleep") as sleep, patch("builtins.print"):
                publish.publish_maven(stage, deployment_id=self.deployment_id)
            signer.assert_not_called()
            self.assertEqual(portal.call_count, 3)
            self.assertEqual(sleep.call_count, 2)
            recorded = json.loads((stage / "deployment.json").read_text())
            self.assertEqual(recorded["deployment_id"], self.deployment_id)
            self.assertTrue(recorded["resumed"])

    def test_maven_failed_deployment_returns_all_details_without_waiting(self):
        status = {"deploymentState": "FAILED", "errors": {"first": ["bad POM"], "second": ["bad signature"]}}
        with patch.object(publish, "central_post", return_value=json.dumps(status).encode()) as api, \
                patch.object(publish.time, "sleep") as sleep:
            with self.assertRaisesRegex(release.ReleaseError, "bad POM.*bad signature"):
                publish.wait_for_maven(self.deployment_id, "test")
            api.assert_called_once()
            sleep.assert_not_called()


if __name__ == "__main__":
    unittest.main()
