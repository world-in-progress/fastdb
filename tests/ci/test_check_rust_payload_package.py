#!/usr/bin/env python3
"""Unit tests for the Rust payload package/link checker."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tarfile
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

    def test_archive_paths_are_rooted_canonical_and_unique(self) -> None:
        root = "fastdb-sys-0.1.22"
        self.assertEqual(
            MODULE.strip_archive_root(
                [
                    root,
                    f"{root}/Cargo.toml",
                    f"{root}/src/lib.rs",
                ],
                root,
            ),
            {"Cargo.toml", "src/lib.rs"},
        )
        for names in (
            [f"{root}/Cargo.toml", f"{root}/Cargo.toml"],
            ["outside/Cargo.toml"],
            [f"{root}/../escape"],
            [f"{root}/..\\escape"],
            [f"{root}/./Cargo.toml"],
            [f"{root}/double//Cargo.toml"],
            ["/absolute/Cargo.toml"],
        ):
            with self.subTest(names=names), self.assertRaises(MODULE.CheckError):
                MODULE.strip_archive_root(names, root)

    def test_archive_rejects_links_and_other_special_members(self) -> None:
        regular = tarfile.TarInfo("fastdb-0.1.22/src/lib.rs")
        regular.size = 0
        directory = tarfile.TarInfo("fastdb-0.1.22/src")
        directory.type = tarfile.DIRTYPE
        MODULE.reject_special_members([regular, directory])

        link = tarfile.TarInfo("fastdb-0.1.22/src/link.rs")
        link.type = tarfile.SYMTYPE
        link.linkname = "lib.rs"
        with self.assertRaises(MODULE.CheckError):
            MODULE.reject_special_members([regular, link])

    def test_package_inventory_requires_license_and_build_seam(self) -> None:
        self.assertIn("LICENSE", MODULE.PACKAGE_REQUIRED["fastdb-sys"])
        self.assertIn("LICENSE", MODULE.PACKAGE_REQUIRED["fastdb"])
        self.assertIn("build.rs", MODULE.PACKAGE_REQUIRED["fastdb-sys"])
        self.assertNotIn("build.rs", MODULE.PACKAGE_REQUIRED["fastdb"])

    def test_safe_manifest_requires_versioned_sys_dependency(self) -> None:
        MODULE.check_normalized_manifest(
            "fastdb",
            b"""[package]
name = "fastdb"
version = "0.1.22"

[dependencies.fastdb-sys]
version = "0.1.22"
""",
        )
        for invalid in (
            b"""[package]
name = "fastdb"
version = "0.1.22"

[dependencies.fastdb-sys]
path = "../fastdb-sys"
""",
            b"""[package]
name = "fastdb"
version = "0.1.22"

[dependencies.fastdb-sys]
version = "0.1.21"
""",
        ):
            with self.assertRaises(MODULE.CheckError):
                MODULE.check_normalized_manifest("fastdb", invalid)


if __name__ == "__main__":
    unittest.main()
