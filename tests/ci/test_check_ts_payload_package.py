#!/usr/bin/env python3
"""Unit tests for the TypeScript payload package checker."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("check_ts_payload_package.py")
SPEC = importlib.util.spec_from_file_location("ts_payload_package", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class TypeScriptPayloadPackageTests(unittest.TestCase):
    def test_requires_payload_inventory_and_rejects_debris(self) -> None:
        MODULE.check_inventory(set(MODULE.REQUIRED))
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_inventory(set(MODULE.REQUIRED) - {"dist/payload/index.js"})
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_inventory(set(MODULE.REQUIRED) | {"src/private.ts"})
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_inventory(set(MODULE.REQUIRED) | {"../escape"})

    def test_requires_exact_payload_subpath_and_module_package(self) -> None:
        accepted = {
            "type": "module",
            "files": ["dist", "README.md"],
            "exports": {
                "./payload": {
                    "types": "./dist/payload/index.d.ts",
                    "import": "./dist/payload/index.js",
                }
            },
        }
        MODULE.check_package_json(accepted)
        for changed in (
            {**accepted, "type": "commonjs"},
            {**accepted, "files": ["dist"]},
            {**accepted, "exports": {}},
        ):
            with self.assertRaises(MODULE.CheckError):
                MODULE.check_package_json(changed)
        with self.assertRaises(MODULE.CheckError):
            MODULE.load_json_no_duplicates(
                '{"type":"module","type":"commonjs"}', "package.json"
            )

    def test_requires_one_clean_tarball_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(MODULE.CheckError):
                MODULE.exact_package(root)
            package = root / "fastdb4ts.tgz"
            package.write_bytes(b"fixture")
            self.assertEqual(MODULE.exact_package(root), package.resolve())
            (root / "extra.txt").write_text("debris", encoding="utf-8")
            with self.assertRaises(MODULE.CheckError):
                MODULE.exact_package(root)

    def test_rejects_duplicate_or_outside_tar_members(self) -> None:
        self.assertEqual(
            MODULE.strip_root(["package", "package/package.json"]),
            {"package.json"},
        )
        with self.assertRaises(MODULE.CheckError):
            MODULE.strip_root(["package/a", "package/a"])
        with self.assertRaises(MODULE.CheckError):
            MODULE.strip_root(["outside"])
        for unsafe in (
            "package/../escape",
            "package/..\\escape",
            "package/./file",
            "package/double//file",
        ):
            with self.subTest(unsafe=unsafe):
                with self.assertRaises(MODULE.CheckError):
                    MODULE.strip_root([unsafe])

    def test_rejects_links_and_other_special_tar_members(self) -> None:
        regular = tarfile.TarInfo("package/file")
        regular.size = 0
        directory = tarfile.TarInfo("package/dir")
        directory.type = tarfile.DIRTYPE
        MODULE.reject_special_members([regular, directory])

        link = tarfile.TarInfo("package/link")
        link.type = tarfile.SYMTYPE
        link.linkname = "file"
        with self.assertRaises(MODULE.CheckError):
            MODULE.reject_special_members([regular, link])


if __name__ == "__main__":
    unittest.main()
