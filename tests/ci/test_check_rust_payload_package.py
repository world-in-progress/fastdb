#!/usr/bin/env python3
"""Unit tests for the Rust payload package/link checker."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("check_rust_payload_package.py")
SPEC = importlib.util.spec_from_file_location("rust_payload_package", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RustPayloadPackageTests(unittest.TestCase):
    def test_requires_complete_inventory_and_rejects_debris(self) -> None:
        for package, required in MODULE.PACKAGE_REQUIRED.items():
            MODULE.check_inventory(package, set(required))
            with self.assertRaises(MODULE.CheckError):
                MODULE.check_inventory(package, set(required) - {next(iter(required))})
            with self.assertRaises(MODULE.CheckError):
                MODULE.check_inventory(package, set(required) | {"target/debug/file"})

    def test_locates_exact_platform_library(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / "nested/libfastdb.dylib"
            library.parent.mkdir()
            library.write_bytes(b"fixture")
            self.assertEqual(
                MODULE.locate_system_library(root, "darwin"), library.resolve()
            )
            duplicate = root / "other/libfastdb.dylib"
            duplicate.parent.mkdir()
            duplicate.write_bytes(b"fixture")
            with self.assertRaises(MODULE.CheckError):
                MODULE.locate_system_library(root, "darwin")

    def test_rejects_missing_or_unsupported_library_roots(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(MODULE.CheckError):
                MODULE.locate_system_library(root, "darwin")
            with self.assertRaises(MODULE.CheckError):
                MODULE.locate_system_library(root, "plan9")


if __name__ == "__main__":
    unittest.main()
