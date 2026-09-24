#!/usr/bin/env python3
"""Real extracted-package probes for the fastdb-sys ownership boundary."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CI_ROOT = Path(__file__).resolve().parent


def load_module(name: str, filename: str):
    path = CI_ROOT / filename
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUST_PACKAGE = load_module(
    "rust_payload_package_for_modes", "check_rust_payload_package.py"
)
CANDIDATE = load_module(
    "local_payload_candidate_for_modes", "build_local_payload_candidate.py"
)


class FastdbSysPackagedModeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(
            prefix="fastdb-packaged-modes-"
        )
        cls.root = Path(cls.temporary.name)
        cls.archives = RUST_PACKAGE.package_archives(cls.root / "packages")
        cls.extracted = RUST_PACKAGE.extract_package_archives(
            cls.archives, cls.root / "extracted"
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_packaged_source_mode_fails_with_precise_boundary(self) -> None:
        consumer = RUST_PACKAGE.write_sys_consumer(
            self.root / "source-consumer",
            self.extracted["fastdb-sys"],
        )
        environment = os.environ.copy()
        environment.update(
            {
                "CARGO_TARGET_DIR": str(self.root / "source-target"),
                "FASTDB_PAYLOAD_LINK_MODE": "source",
            }
        )
        completed = subprocess.run(
            [
                "cargo",
                "check",
                "--offline",
                "--manifest-path",
                str(consumer / "Cargo.toml"),
            ],
            cwd=self.root,
            env=environment,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="strict",
        )
        self.assertNotEqual(completed.returncode, 0, completed.stdout)
        self.assertIn(
            "FASTDB_PAYLOAD_LINK_MODE=source is checkout-only",
            completed.stdout,
        )
        self.assertIn(
            "FASTDB_PAYLOAD_LINK_MODE=system",
            completed.stdout,
        )
        self.assertIn("FASTDB_PAYLOAD_SYSTEM_LIB_DIR", completed.stdout)

    def test_packaged_system_mode_links_extracted_core_outside_checkout(self) -> None:
        build_root = self.root / "native-build"
        bundle = self.root / "core-bundle"
        CANDIDATE.build_core_bundle(build_root, bundle)
        library = RUST_PACKAGE.locate_system_library(bundle / "lib", sys.platform)
        RUST_PACKAGE.check_extracted_system_consumer(
            self.extracted,
            library,
            self.root / "system-consumer",
        )


if __name__ == "__main__":
    unittest.main()
