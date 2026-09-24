#!/usr/bin/env python3
"""Focused tests for the reviewed binary-open corpus gate."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "check_payload_binary_corpus.py"
SPEC = importlib.util.spec_from_file_location(
    "check_payload_binary_corpus", MODULE_PATH
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class BinaryCorpusGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.original_manifest = MODULE.MANIFEST
        self.original_corpus = MODULE.CORPUS

    def tearDown(self) -> None:
        MODULE.MANIFEST = self.original_manifest
        MODULE.CORPUS = self.original_corpus

    def test_reviewed_inventory_has_exact_p3_sixteen_seed_order(self) -> None:
        cases = MODULE.load_manifest()
        self.assertEqual(len(cases), 16)
        self.assertEqual(
            [case["name"] for case in cases[-6:]],
            [
                "valid-graph-cycle.bin",
                "valid-graph-variable.bin",
                "valid-graph-null.bin",
                "malformed-graph-object-region.bin",
                "malformed-graph-reference.bin",
                "malformed-graph-unreachable.bin",
            ],
        )

    def test_rejects_a_name_class_mapping_change(self) -> None:
        document = json.loads(self.original_manifest.read_text(encoding="utf-8"))
        document["cases"][0]["class"], document["cases"][1]["class"] = (
            document["cases"][1]["class"],
            document["cases"][0]["class"],
        )
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "binary-corpus.json"
            manifest.write_text(json.dumps(document), encoding="utf-8")
            MODULE.MANIFEST = manifest
            with self.assertRaises(MODULE.CheckError):
                MODULE.load_manifest()

    def test_rejects_an_expected_result_change(self) -> None:
        document = json.loads(self.original_manifest.read_text(encoding="utf-8"))
        document["cases"][4]["expected"]["path"] = "/different/path"
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "binary-corpus.json"
            manifest.write_text(json.dumps(document), encoding="utf-8")
            MODULE.MANIFEST = manifest
            with self.assertRaises(MODULE.CheckError):
                MODULE.load_manifest()

    def test_rejects_non_file_debris_in_the_corpus_directory(self) -> None:
        cases = MODULE.load_manifest()
        with tempfile.TemporaryDirectory() as directory:
            corpus = Path(directory) / "binary-corpus"
            shutil.copytree(self.original_corpus, corpus)
            (corpus / "unreviewed-directory").mkdir()
            MODULE.CORPUS = corpus
            with self.assertRaises(MODULE.CheckError):
                MODULE.verify(cases)


if __name__ == "__main__":
    unittest.main()
