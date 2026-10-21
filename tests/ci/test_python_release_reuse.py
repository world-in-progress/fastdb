#!/usr/bin/env python3
"""Local regressions for publish-without-build and selective wheel backfills."""
from contextlib import redirect_stdout
from datetime import datetime, timezone
import io
import json
from pathlib import Path
import shutil
import unittest
from unittest.mock import patch
from urllib.parse import urlparse

import python_distributions as distributions
import publish_release
from release_artifacts import FRAGMENT_SCHEMA, ReleaseError, digest
from test_python_distributions import BUILD, PLATFORMS
import test_python_distributions as fixtures


class PythonReleaseReuseTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.PythonDistributionTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root, self.source, self.dist = self.fixture.root, self.fixture.source, self.fixture.dist
        for name in ("setup.py", "MANIFEST.in", "fastcarto/CMakeLists.txt", "LICENSE", "THIRD_PARTY_NOTICES.txt"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"pass\n" if name == "setup.py" else b"original source")
        source_files = {path.relative_to(self.source).as_posix(): path.read_bytes()
                        for path in self.source.rglob("*") if path.is_file()}
        self.fixture.archive({**source_files, "PKG-INFO": b"Name: fastdb4py\nVersion: 0.2.1\n"})
        self.records = self.fixture.inventory()
        self.runners = {platform: {"os": "test-runner", "arch": "test-arch"} for platform in PLATFORMS.split()}
        self.reused = self.root / "reused"
        self.built = self.root / "built"
        self.built.mkdir()
        self.registry = self.registry_without({"cp310", "cp311"})
        self.source_patch = patch.object(distributions, "source_commit", return_value="a" * 40)
        self.source_patch.start()
        self.addCleanup(self.source_patch.stop)

    def registry_without(self, abis):
        return {"urls": [{"filename": item["name"], "size": item["bytes"],
                          "packagetype": "bdist_wheel" if item["name"].endswith(".whl") else "sdist",
                          "digests": {"sha256": item["sha256"]},
                          "upload_time_iso_8601": datetime.now(timezone.utc).isoformat(),
                          "url": "https://files.pythonhosted.org/packages/" + item["name"]}
                         for item in self.records if item.get("python_abi") not in abis]}

    def download(self, request, **kwargs):
        url = request.full_url if hasattr(request, "full_url") else request
        return io.BytesIO((self.dist / Path(urlparse(url).path).name).read_bytes())

    def plan(self, registry=None):
        with patch.object(distributions, "public_registry", return_value=registry or self.registry), \
                patch.object(distributions, "urlopen", side_effect=self.download):
            return distributions.plan_build(self.source, BUILD, PLATFORMS, self.runners, self.reused, backfill=True)

    def build_missing(self, plan):
        missing = {tuple(slot) for slot in plan["missing"]}
        for path in self.dist.glob("*.whl"):
            if distributions.wheel_identity(path.name, "0.2.1") in missing:
                shutil.copy2(path, self.built / path.name)

    def complete(self, plan):
        with redirect_stdout(io.StringIO()):
            return distributions.complete_build(plan, self.source, self.built, self.reused,
                                                self.root / "complete", BUILD, PLATFORMS)

    def test_full_ci_plan_builds_the_matrix_and_sdist_without_registry_access(self):
        with patch.object(distributions, "public_registry") as registry:
            plan = distributions.plan_build(self.source, BUILD, PLATFORMS, self.runners, self.reused)
        registry.assert_not_called()
        self.assertEqual(len(plan["missing"]), 35)
        self.assertEqual(len(plan["matrix"]["include"]), 6)
        self.assertTrue(plan["build_sdist"])
        self.assertEqual(plan["reused"], [])

    def test_full_ci_completion_produces_the_same_complete_inventory(self):
        plan = distributions.plan_build(self.source, BUILD, PLATFORMS, self.runners, self.reused)
        for item in self.records:
            shutil.copy2(self.dist / item["name"], self.built / item["name"])
        self.assertEqual(self.complete(plan), self.records)

    def test_backfill_plans_only_missing_wheels_and_reuses_the_sdist(self):
        plan = self.plan()
        self.assertEqual(len(plan["missing"]), 11)
        self.assertFalse(plan["build_sdist"])
        self.assertEqual(len(plan["reused"]), 25)
        for job in plan["matrix"]["include"]:
            abis = ["cp311"] if job["platform"] == "win_arm64" else ["cp310", "cp311"]
            self.assertEqual(job["build"].split(), [f"{abi}-{job['platform']}" for abi in abis])

    def test_one_missing_slot_allocates_only_one_platform_and_interpreter(self):
        registry = self.registry_without(set())
        registry["urls"] = [item for item in registry["urls"]
                            if item["filename"] != "fastdb4py-0.2.1-cp311-cp311-win_amd64.whl"]
        plan = self.plan(registry)
        self.assertEqual(plan["matrix"]["include"], [
            {**self.runners["win_amd64"], "platform": "win_amd64", "build": "cp311-win_amd64"}])

    def test_existing_five_platform_release_builds_only_five_arm64_wheels(self):
        registry = self.registry_without(set())
        registry["urls"] = [item for item in registry["urls"] if "win_arm64" not in item["filename"]]
        plan = self.plan(registry)
        self.assertEqual(len(plan["reused"]), 31)
        self.assertFalse(plan["build_sdist"])
        self.assertEqual(plan["matrix"]["include"], [
            {**self.runners["win_arm64"], "platform": "win_arm64",
             "build": "cp311-win_arm64 cp312-win_arm64 cp313-win_arm64 cp314-win_arm64 cp314t-win_arm64"}])
        self.build_missing(plan)
        self.assertEqual(len(self.complete(plan)), 36)

    def test_old_x64_only_source_fails_before_allocating_backfill_builders(self):
        (self.source / "setup.py").write_text("cmake_args += ['-A', 'x64']\n")
        registry = self.registry_without(set())
        registry["urls"] = [item for item in registry["urls"] if "win_arm64" not in item["filename"]]
        with patch.object(distributions, "public_registry", return_value=registry), \
                patch.object(distributions, "urlopen") as download:
            with self.assertRaisesRegex(ReleaseError, "new release.*Do not patch or move"):
                distributions.plan_build(self.source, BUILD, PLATFORMS, self.runners, self.reused, backfill=True)
        download.assert_not_called()
        self.assertFalse(self.reused.exists())

    def test_complete_release_is_a_noop_even_after_upload_window(self):
        registry = self.registry_without(set())
        for item in registry["urls"]:
            item["upload_time_iso_8601"] = "2000-01-01T00:00:00Z"
        plan = self.plan(registry)
        self.assertFalse(plan["pending"])
        self.assertEqual(plan["matrix"], {"include": []})
        self.assertFalse(plan["build_sdist"])
        self.assertEqual([path.name for path in self.reused.iterdir()], [self.fixture.sdist.name])

    def test_expired_partial_release_fails_before_downloading_or_building(self):
        for item in self.registry["urls"]:
            item["upload_time_iso_8601"] = "2000-01-01T00:00:00Z"
        with patch.object(distributions, "public_registry", return_value=self.registry), \
                patch.object(distributions, "urlopen") as download:
            with self.assertRaisesRegex(ReleaseError, "14-day"):
                distributions.plan_build(self.source, BUILD, PLATFORMS, self.runners, self.reused, backfill=True)
        download.assert_not_called()

    def test_source_mismatch_blocks_backfill(self):
        (self.source / "setup.py").write_bytes(b"pass # modified tag source\n")
        with self.assertRaisesRegex(ReleaseError, "differs from the tag"):
            self.plan()

    def test_corrupt_published_wheel_is_not_reused(self):
        item = next(item for item in self.registry["urls"] if item["filename"].endswith(".whl"))
        item["digests"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ReleaseError, "hash/size"):
            self.plan()

    def test_duplicate_or_yanked_published_slot_is_rejected(self):
        wheel = next(item for item in self.registry["urls"] if item["filename"].endswith(".whl"))
        self.registry["urls"].append(dict(wheel))
        with self.assertRaisesRegex(ReleaseError, "Duplicate published"):
            self.plan()
        self.registry["urls"].pop()
        wheel["yanked"] = True
        with self.assertRaisesRegex(ReleaseError, "yanked"):
            self.plan()

    def test_complete_inventory_combines_existing_bytes_and_only_planned_new_files(self):
        plan = self.plan()
        self.build_missing(plan)
        records = self.complete(plan)
        self.assertEqual(len(records), 36)
        for item in plan["reused"]:
            self.assertEqual(digest(self.root / "complete" / item["name"]), item["sha256"])
        with patch.object(distributions, "public_registry", return_value=self.registry):
            pending = distributions.stage(records, self.root / "complete", self.root / "upload", "0.2.1", True)
        self.assertEqual(len(pending), 11)

    def test_complete_rejects_rebuilt_existing_wheels(self):
        plan = self.plan()
        self.build_missing(plan)
        extra = self.dist / "fastdb4py-0.2.1-cp312-cp312-win_amd64.whl"
        shutil.copy2(extra, self.built / extra.name)
        with self.assertRaisesRegex(ReleaseError, "unplanned"):
            self.complete(plan)

    def test_complete_rejects_missing_build_and_reused_hash_changes(self):
        plan = self.plan()
        self.build_missing(plan)
        missing = next(self.built.iterdir())
        contents = missing.read_bytes()
        missing.unlink()
        with self.assertRaisesRegex(ReleaseError, "every planned"):
            self.complete(plan)
        missing.write_bytes(contents)
        next(self.reused.glob("*.whl")).write_bytes(b"changed after registry verification")
        with self.assertRaisesRegex(ReleaseError, "hash/size changed"):
            self.complete(plan)

    def test_changed_source_plan_is_rejected(self):
        plan = self.plan()
        plan["source_sha"] = "b" * 40
        with self.assertRaisesRegex(ReleaseError, "selected source"):
            self.complete(plan)

    def test_fragment_checks_source_inventory_duplicates_and_hashes(self):
        fragment = {"schema": FRAGMENT_SCHEMA, "part": "python", "source_sha": "a" * 40,
                    "version": "0.2.1", "artifacts": self.records}
        path = self.dist / "python.json"
        path.write_text(json.dumps(fragment))
        distributions.verify_fragment(self.dist, self.source, self.records)
        for changed in ({**fragment, "source_sha": "b" * 40},
                        {**fragment, "artifacts": self.records + [self.records[0]]},
                        {**fragment, "artifacts": [{**self.records[0], "sha256": "0" * 64}, *self.records[1:]]}):
            path.write_text(json.dumps(changed))
            with self.assertRaises(ReleaseError):
                distributions.verify_fragment(self.dist, self.source, self.records)

    def test_ci_reuse_requires_an_unexpired_exact_source_artifact(self):
        artifact = {"id": 789, "name": "release-part-python", "expired": False,
                    "digest": "sha256:" + "d" * 64, "workflow_run": {"id": 123, "head_sha": "a" * 40}}
        def api(path):
            return {"artifacts": [artifact]} if "/artifacts?" in path else {"workflow_runs": [{"id": 123}]}
        with patch.object(publish_release, "github", side_effect=api), \
                patch.object(publish_release, "check_ci", return_value={"artifact_run_id": 123}), \
                patch.object(distributions.subprocess, "run") as build:
            self.assertEqual(distributions.check_ci_source("a" * 40, require_artifact=True)["python_artifact_id"], 789)
            for field, value in (("expired", True), ("digest", ""), ("workflow_run", {"id": 123, "head_sha": "b" * 40})):
                original = artifact[field]
                artifact[field] = value
                with self.subTest(field=field), self.assertRaisesRegex(ReleaseError, "publication never rebuilds"):
                    distributions.check_ci_source("a" * 40, require_artifact=True)
                artifact[field] = original
            with patch.object(publish_release, "github", return_value={"artifacts": [], "workflow_runs": [{"id": 123}]}):
                with self.assertRaisesRegex(ReleaseError, "publication never rebuilds"):
                    distributions.check_ci_source("a" * 40, require_artifact=True)
            with patch.object(publish_release, "check_ci", side_effect=ReleaseError("failed proof")):
                with self.assertRaisesRegex(ReleaseError, "No successful complete main CI evidence"):
                    distributions.check_ci_source("a" * 40)
        build.assert_not_called()

    def test_registry_planning_refreshes_metadata(self):
        with patch.object(publish_release, "request_json", return_value=self.registry) as request:
            self.assertEqual(distributions.public_registry("0.2.1"), self.registry)
        request.assert_called_once_with("https://pypi.org/pypi/fastdb4py/0.2.1/json", no_cache=True)


if __name__ == "__main__":
    unittest.main()
