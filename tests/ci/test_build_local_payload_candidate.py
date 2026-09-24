#!/usr/bin/env python3
"""Unit tests for strict FastDB local-candidate evidence construction."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("build_local_payload_candidate.py")
SPEC = importlib.util.spec_from_file_location("local_payload_candidate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class LocalPayloadCandidateTests(unittest.TestCase):
    def test_python_compatibility_wheel_precedes_current_runtime_restore(self) -> None:
        self.assertEqual(MODULE.PYTHON_BUILD_ORDER, ("cp310", "current"))

    def test_source_snapshot_excludes_ignored_build_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = MODULE.copy_source_snapshot(
                Path(temporary) / "source"
            )
            self.assertTrue((snapshot / "pyproject.toml").is_file())
            self.assertTrue(
                (
                    snapshot
                    / "tests/ci/build_local_payload_candidate.py"
                ).is_file()
            )
            self.assertFalse((snapshot / ".git").exists())
            self.assertFalse((snapshot / ".venv").exists())
            self.assertFalse((snapshot / "build").exists())
            self.assertFalse(
                (
                    snapshot
                    / "python/fastdb4py/core/_fastdb4py.so"
                ).exists()
            )

    def test_canonical_manifest_bytes_are_stable(self) -> None:
        document = {"z": [2, 1], "a": {"b": True}}
        self.assertEqual(
            MODULE.canonical_json_bytes(document),
            b'{"a":{"b":true},"z":[2,1]}\n',
        )

    def test_relative_paths_reject_absolute_parent_and_noncanonical_forms(self) -> None:
        self.assertEqual(
            MODULE.checked_relative_path("python/current/package.whl"),
            "python/current/package.whl",
        )
        for invalid in (
            "",
            "/absolute",
            "../escape",
            "a/../escape",
            "a\\windows",
            "./a",
            "a//b",
        ):
            with self.subTest(invalid=invalid), self.assertRaises(
                MODULE.CandidateError
            ):
                MODULE.checked_relative_path(invalid)

    def test_inventory_rejects_duplicate_paths(self) -> None:
        inventory = [
            {"path": "include/a.h", "bytes": 1, "sha256": "0" * 64},
            {"path": "include/a.h", "bytes": 1, "sha256": "0" * 64},
        ]
        with self.assertRaises(MODULE.CandidateError):
            MODULE.validate_inventory(inventory, "fixture")

    def test_manifest_rejects_absolute_paths_at_any_depth(self) -> None:
        document = {
            "schema": MODULE.MANIFEST_SCHEMA,
            "source": {"repository": "fastdb", "commit": "0" * 40},
            "artifacts": [
                {
                    "path": "core",
                    "inventory": [
                        {
                            "path": "/source-machine/private/header.h",
                            "bytes": 1,
                            "sha256": "0" * 64,
                        }
                    ],
                }
            ],
        }
        with self.assertRaises(MODULE.CandidateError):
            MODULE.reject_absolute_manifest_paths(document)

    def test_build_mode_requires_absent_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            existing = Path(temporary) / "candidate"
            existing.mkdir()
            with self.assertRaises(MODULE.CandidateError):
                MODULE.require_absent_output(existing)

    def test_verification_detects_artifact_tampering(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "candidate"
            artifact = root / "fixture/payload.bin"
            artifact.parent.mkdir(parents=True)
            artifact.write_bytes(b"authority")
            provenance = MODULE.test_provenance()
            manifest = MODULE.construct_manifest(
                root,
                provenance,
                [
                    MODULE.artifact_descriptor(
                        root,
                        path="fixture/payload.bin",
                        kind="test-fixture",
                        package={"name": "fixture", "version": "0"},
                        build=MODULE.test_build_evidence(),
                    )
                ],
                enforce_candidate_set=False,
            )
            manifest_path = root / MODULE.MANIFEST_FILENAME
            manifest_path.write_bytes(MODULE.canonical_json_bytes(manifest))
            MODULE.verify_manifest(root, enforce_candidate_set=False)

            artifact.write_bytes(b"tampered")
            with self.assertRaises(MODULE.CandidateError):
                MODULE.verify_manifest(root, enforce_candidate_set=False)

    def test_verification_rejects_unmanifested_candidate_debris(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "candidate"
            artifact = root / "fixture/payload.bin"
            artifact.parent.mkdir(parents=True)
            artifact.write_bytes(b"authority")
            manifest = MODULE.construct_manifest(
                root,
                MODULE.test_provenance(),
                [
                    MODULE.artifact_descriptor(
                        root,
                        path="fixture/payload.bin",
                        kind="test-fixture",
                        package={"name": "fixture", "version": "0"},
                        build=MODULE.test_build_evidence(),
                    )
                ],
                enforce_candidate_set=False,
            )
            (root / MODULE.MANIFEST_FILENAME).write_bytes(
                MODULE.canonical_json_bytes(manifest)
            )
            (root / "unexpected.txt").write_text("debris", encoding="utf-8")
            with self.assertRaises(MODULE.CandidateError):
                MODULE.verify_manifest(root, enforce_candidate_set=False)

    def test_manifest_schema_and_abi_evidence_are_exact(self) -> None:
        allowlist = MODULE.read_abi_allowlist()
        self.assertEqual(len(allowlist), 117)
        self.assertEqual(allowlist, sorted(set(allowlist)))
        self.assertEqual(MODULE.ABI_VERSION, 1)
        self.assertEqual(
            MODULE.MANIFEST_SCHEMA,
            "fastdb.local-candidate-manifest.v1",
        )

    def test_duplicate_json_keys_are_rejected(self) -> None:
        with self.assertRaises(MODULE.CandidateError):
            MODULE.load_json_no_duplicates(
                '{"schema":"a","schema":"b"}',
                "fixture manifest",
            )

    def test_manifest_is_free_of_machine_paths_and_wall_clock_fields(self) -> None:
        document = {
            "schema": MODULE.MANIFEST_SCHEMA,
            "source": {
                "repository": "fastdb",
                "commit": "0" * 40,
                "source_date_epoch": 1,
            },
            "build": {
                "commands": ["cmake -S $SOURCE_ROOT/fastcarto"],
                "toolchains": {"cmake": "cmake version fixture"},
            },
            "artifacts": [],
        }
        MODULE.reject_absolute_manifest_paths(document)
        encoded = MODULE.canonical_json_bytes(document)
        self.assertNotIn(b"created_at", encoded)
        self.assertNotIn(b"timestamp", encoded)
        self.assertEqual(
            json.loads(encoded)["source"]["source_date_epoch"],
            1,
        )


if __name__ == "__main__":
    unittest.main()
