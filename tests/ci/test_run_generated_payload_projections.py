#!/usr/bin/env python3
"""Unit tests for generated-projection harness Core-library selection."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("run_generated_payload_projections.py")
SPEC = importlib.util.spec_from_file_location(
    "generated_projection_harness", MODULE_PATH
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PythonLibrarySelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.previous = os.environ.pop("FASTDB_PAYLOAD_LIBRARY", None)

    def tearDown(self) -> None:
        os.environ.pop("FASTDB_PAYLOAD_LIBRARY", None)
        if self.previous is not None:
            os.environ["FASTDB_PAYLOAD_LIBRARY"] = self.previous

    def test_configures_the_resolved_compiled_library_when_unset(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            library = Path(temporary, "libfastdb.test")
            library.touch()
            MODULE.configure_python_library(library.resolve())
            self.assertEqual(
                Path(os.environ["FASTDB_PAYLOAD_LIBRARY"]), library.resolve()
            )

    def test_accepts_the_same_resolved_library(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            library = Path(temporary, "libfastdb.test")
            library.touch()
            os.environ["FASTDB_PAYLOAD_LIBRARY"] = os.fspath(library)
            MODULE.configure_python_library(library.resolve())

    def test_rejects_a_different_python_library(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            compiled = Path(temporary, "compiled.test")
            python = Path(temporary, "python.test")
            compiled.touch()
            python.touch()
            os.environ["FASTDB_PAYLOAD_LIBRARY"] = os.fspath(python)
            with self.assertRaisesRegex(
                MODULE.HarnessError, "must use the same FastDB Core library"
            ):
                MODULE.configure_python_library(compiled.resolve())

    def test_rejects_a_missing_python_library(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            compiled = Path(temporary, "compiled.test")
            compiled.touch()
            os.environ["FASTDB_PAYLOAD_LIBRARY"] = os.fspath(
                Path(temporary, "missing.test")
            )
            with self.assertRaisesRegex(
                MODULE.HarnessError, "does not resolve to a native library"
            ):
                MODULE.configure_python_library(compiled.resolve())

    def test_owned_parent_views_are_released_explicitly(self) -> None:
        harness = MODULE_PATH.read_text(encoding="utf-8")
        typescript = (MODULE.TEMPLATES / "payload_codegen_smoke.ts").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("wrong_payload.entry_view(0).at(0)", harness)
        self.assertIn("with wrong_payload.entry_view(0) as wrong_entry:", harness)
        self.assertNotIn("wrongPayload.entryView(0).at(0n)", typescript)
        self.assertIn("wrongEntry.dispose();", typescript)

    def test_requires_public_hostile_codegen_matrix(self) -> None:
        self.assertEqual(
            tuple(MODULE.HOSTILE_SPEC_NAMES),
            (
                "record-all-types",
                "recursive-lists",
                "keyword-prefix-collisions",
                "shared-cyclic-graph",
            ),
        )
        self.assertTrue(callable(MODULE.run_hostile_codegen_matrix))
        harness = MODULE_PATH.read_text(encoding="utf-8")
        self.assertGreaterEqual(
            harness.count("run_hostile_codegen_matrix("),
            2,
            "hostile matrix must be defined and invoked",
        )


if __name__ == "__main__":
    unittest.main()
