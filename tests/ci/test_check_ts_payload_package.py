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
    def test_rejects_every_removed_root_declaration_marker(self) -> None:
        removed = {
            "call-db",
            "decodeFastdbCallDb",
            "decodeFastdbFeature",
            "encodeFastdbCallDb",
            "encodeFastdbFeature",
            "FastdbCallDbArrayItem",
            "FastdbCallDbArrayView",
            "FastdbCallDbBinding",
            "FastdbCallDbColumnView",
            "FastdbCallDbFeatureDependency",
            "FastdbCallDbScalarField",
            "FastdbCallDbTable",
            "FastdbCallDbTableView",
            "FastdbCallDbView",
            "FastdbFeatureCodecBinding",
            "viewFastdbCallDb",
        }
        self.assertEqual(
            getattr(MODULE, "REMOVED_ROOT_DECLARATION_MARKERS", set()),
            removed,
        )
        check = getattr(MODULE, "check_root_declarations", lambda _source: None)
        check("export { Feature, ORM, FastSerializer };")
        for marker in removed:
            with self.subTest(marker=marker), self.assertRaises(MODULE.CheckError):
                check(f"export type {{ {marker} }};")

    def test_forbids_generated_call_db_runtime_and_declarations(self) -> None:
        forbidden = {"dist/call-db.js", "dist/call-db.d.ts"}
        self.assertEqual(MODULE.FORBIDDEN, forbidden)
        for path in forbidden:
            with self.subTest(path=path), self.assertRaises(MODULE.CheckError):
                MODULE.check_inventory(set(MODULE.REQUIRED) | {path})

    def test_requires_payload_inventory_and_rejects_debris(self) -> None:
        self.assertIn("dist/index.js", MODULE.REQUIRED)
        self.assertIn("dist/index.d.ts", MODULE.REQUIRED)
        MODULE.check_inventory(set(MODULE.REQUIRED))
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_inventory(set(MODULE.REQUIRED) - {"dist/payload/index.js"})
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_inventory(set(MODULE.REQUIRED) | {"src/private.ts"})
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_inventory(set(MODULE.REQUIRED) | {"../escape"})

    def test_requires_exact_payload_subpath_and_module_package(self) -> None:
        root_export = {
            "types": "./dist/index.d.ts",
            "import": "./dist/index.js",
        }
        payload_export = {
            "types": "./dist/payload/index.d.ts",
            "import": "./dist/payload/index.js",
        }
        accepted = {
            "type": "module",
            "files": ["dist", "README.md"],
            "exports": {
                ".": root_export,
                "./payload": payload_export,
            },
        }
        MODULE.check_package_json(accepted)
        for changed in (
            {**accepted, "type": "commonjs"},
            {**accepted, "files": ["dist"]},
            {**accepted, "exports": {}},
            {**accepted, "exports": {"./payload": payload_export}},
            {**accepted, "exports": {".": root_export}},
            {
                **accepted,
                "exports": {
                    ".": root_export,
                    "./payload": payload_export,
                    "./call-db": root_export,
                },
            },
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
