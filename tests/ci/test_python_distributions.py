#!/usr/bin/env python3
from __future__ import annotations

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import re
import shlex
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import python_distributions as distributions
from release_artifacts import ReleaseError


BUILD = "cp312-* cp313-* cp314-* cp314t-*"
PLATFORMS = "macosx_arm64 macosx_x86_64 manylinux_x86_64 manylinux_aarch64 win_amd64"


class PythonDistributionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "pyproject.toml").write_text('[project]\nname="fastdb4py"\nversion="0.2.1"\n')
        self.dist = self.root / "dist"
        self.dist.mkdir()
        for platform in PLATFORMS.split():
            tag = platform
            if platform.startswith("macosx_"):
                tag = platform.replace("macosx_", "macosx_11_0_", 1)
            elif platform.startswith("manylinux_"):
                tag = platform.replace("manylinux_", "manylinux_2_28_", 1)
            for abi in ("cp312", "cp313", "cp314", "cp314t"):
                name = f"fastdb4py-0.2.1-{abi.removesuffix('t')}-{abi}-{tag}.whl"
                self.wheel(self.dist / name)
        self.sdist = self.dist / "fastdb4py-0.2.1.tar.gz"
        self.archive({"PKG-INFO": b"Name: fastdb4py\nVersion: 0.2.1\n"})

    def wheel(self, path, *, metadata_version="0.2.1"):
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("fastdb4py-0.2.1.dist-info/METADATA", f"Name: fastdb4py\nVersion: {metadata_version}\n")
            archive.writestr("fastdb4py/core/_fastdb4py.so", b"native fixture")

    def archive(self, members):
        with tarfile.open(self.sdist, "w:gz") as archive:
            for name, contents in members.items():
                info = tarfile.TarInfo("fastdb4py-0.2.1/" + name)
                info.size = len(contents)
                archive.addfile(info, io.BytesIO(contents))

    def inventory(self):
        with redirect_stdout(io.StringIO()):
            return distributions.inventory(self.dist, self.source, BUILD, PLATFORMS)

    def test_complete_inventory_and_every_missing_wheel_slot(self):
        self.assertEqual(len(self.inventory()), 21)
        for path in sorted(self.dist.glob("*.whl")):
            with self.subTest(missing=path.name):
                data = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(ReleaseError, "Incomplete Python distributions"):
                    self.inventory()
                path.write_bytes(data)

    def test_missing_sdist_blocks_publication(self):
        self.sdist.unlink()
        with self.assertRaisesRegex(ReleaseError, "missing sdist: True"):
            self.inventory()

    def test_compressed_auditwheel_tags_keep_one_slot(self):
        path = self.dist / "fastdb4py-0.2.1-cp312-cp312-manylinux_2_28_aarch64.whl"
        path.rename(path.with_name(path.name.replace("manylinux_2_28_", "manylinux_2_24_aarch64.manylinux_2_28_")))
        self.assertEqual(len(self.inventory()), 21)

    def test_wrong_abi_platform_version_and_duplicate_slot_are_rejected(self):
        for name in (
            "fastdb4py-0.2.1-cp314t-cp314t-win_amd64.whl",
            "fastdb4py-0.2.1-cp314-cp313-win_amd64.whl",
            "fastdb4py-0.2.1-cp312-cp312-manylinux_2_35_aarch64.whl",
            "fastdb4py-0.2.1-cp312-cp312-manylinux_2_24_aarch64.whl",
            "fastdb4py-0.2.1-cp312-cp312-macosx_27_0_x86_64.whl",
            "fastdb4py-0.2.1-cp312-cp312-macosx_11_0_arm64.macosx_11_0_x86_64.whl",
            "fastdb4py-0.2.2-cp312-cp312-win_amd64.whl",
            "fastdb4py-0.2.1-cp312-cp312-macosx_10_15_x86_64.whl",
            "fastdb4py-0.2.1-cp311-cp311-win_amd64.whl",
        ):
            with self.subTest(name=name):
                path = self.dist / name
                self.wheel(path)
                with self.assertRaises(ReleaseError):
                    self.inventory()
                path.unlink()

    def test_metadata_and_unknown_files_fail_closed(self):
        path = self.dist / "fastdb4py-0.2.1-cp312-cp312-win_amd64.whl"
        self.wheel(path, metadata_version="0.2.2")
        with self.assertRaisesRegex(ReleaseError, "metadata"):
            self.inventory()
        self.wheel(path)
        (self.dist / "unexpected.txt").write_text("unreviewed")
        with self.assertRaisesRegex(ReleaseError, "Unexpected distribution"):
            self.inventory()

    def test_backfill_stages_only_eight_missing_names_and_leaves_existing_bytes(self):
        records = self.inventory()
        missing = [item for item in records if item.get("platform") in {"manylinux_aarch64", "macosx_x86_64"}]
        missing_names = {item["name"] for item in missing}
        existing = [{"filename": item["name"], "digests": {"sha256": "old, independently built bytes"}} for item in records if item["name"] not in missing_names]
        # Historical 3.10/3.11 files remain published, outside the new matrix.
        existing.append({"filename": "fastdb4py-0.2.1-cp310-cp310-win_amd64.whl"})
        with patch("publish_release.request_json", return_value={"urls": existing}):
            pending = distributions.stage(records, self.dist, self.root / "upload", "0.2.1", True)
        self.assertEqual(len(pending), 8)
        self.assertEqual({path.name for path in (self.root / "upload").iterdir()}, missing_names)
        for item in pending:
            self.assertEqual((self.root / "upload" / item["name"]).read_bytes(), (self.dist / item["name"]).read_bytes())
        with patch("publish_release.request_json") as registry:
            self.assertEqual(len(distributions.stage(records, self.dist, self.root / "normal", "0.2.1", False)), 21)
            registry.assert_not_called()

    def test_published_source_comparison_rejects_changed_source_and_extra_code(self):
        required = ("setup.py", "MANIFEST.in", "fastcarto/CMakeLists.txt", "LICENSE", "THIRD_PARTY_NOTICES.txt")
        for name in required:
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"original source")
        members = {path.relative_to(self.source).as_posix(): path.read_bytes() for path in self.source.rglob("*") if path.is_file()}
        members["PKG-INFO"] = b"Name: fastdb4py\nVersion: 0.2.1\n"
        self.archive(members)
        self.assertEqual(distributions.compare_sdist(self.source, self.sdist)["matched_source_files"], 6)
        self.archive({**members, "setup.py": b"changed source"})
        with self.assertRaisesRegex(ReleaseError, "differs from the tag"):
            distributions.compare_sdist(self.source, self.sdist)
        self.archive({**members, "extra.py": b"unexpected source"})
        with self.assertRaisesRegex(ReleaseError, "absent from the tag"):
            distributions.compare_sdist(self.source, self.sdist)

    def test_empty_matrix_is_rejected(self):
        for build, platforms in (("", PLATFORMS), (BUILD, ""), ("cp3*", PLATFORMS)):
            with self.assertRaises(ReleaseError):
                distributions.inventory(self.dist, self.source, build, platforms)

    def test_new_controller_checks_old_source_proofs_without_changing_default_policy(self):
        commit = "a" * 40
        with patch("publish_release.github", return_value={"workflow_runs": [{"id": 123}]}), patch("publish_release.check_ci", return_value={"source_sha": commit}) as proof:
            with patch.dict(os.environ, {"GITHUB_SHA": "b" * 40}):
                self.assertEqual(distributions.check_ci_source(commit), {"source_sha": commit})
            proof.assert_called_once_with("123", commit, require_workflow_source=False)

    def test_tag_resolution_rejects_branches_and_does_not_run_them(self):
        for tag in ("main", "v0.2.1; echo unsafe", "v0.2.1/other", "v0.2.1-rc1", ""):
            with self.subTest(tag=tag), patch.object(distributions.subprocess, "check_output") as git:
                with self.assertRaises(ReleaseError):
                    distributions.resolve_tag(tag, self.source)
                git.assert_not_called()

    def test_wheel_tests_use_the_selected_package_checkout(self):
        # cibuildwheel's {project} is the caller cwd. In a two-checkout build
        # only {package} points at the selected tag and contains its tests.
        workflow = Path(__file__).resolve().parents[2] / ".github/workflows/python-distributions.yml"
        template = re.search(r"CIBW_TEST_COMMAND: >-\n(.*?)\n  FASTDB_WHEEL_PLATFORMS:", workflow.read_text(), re.DOTALL)[1]
        command = " ".join(line.strip() for line in template.splitlines())
        command = command.replace("{project}", str(self.root)).replace("{package}", str(self.source))
        arguments = shlex.split(command)
        actual = Path(arguments[arguments.index("pytest") + 1])
        selected_tests = self.source / "tests/python/payload"
        selected_tests.mkdir(parents=True)
        self.assertEqual(actual, selected_tests)
        self.assertTrue(actual.is_dir())


if __name__ == "__main__":
    unittest.main()
