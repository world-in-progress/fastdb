#!/usr/bin/env python3
"""Focused tests for the Python package quality gate."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "check_python_package_inventory.py"
SPEC = importlib.util.spec_from_file_location("check_python_package_inventory", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


EXPECTED_MESSAGES = {
    582: "Nested struct not currently supported (TileBox ignored)",
    588: "Nested class not currently supported (HandleTileAction ignored)",
    595: "Nested struct not currently supported (TakeResult ignored)",
    622: "Nested struct not currently supported (TileDataHandle ignored)",
    631: "Nested struct not currently supported (TileDbBox ignored)",
    637: "Nested struct not currently supported (TakeResult ignored)",
    206: "Setting a const char * variable may leak memory.",
}


def accepted_swig_lines() -> list[str]:
    return [
        f"/tmp/build/fastcarto/fastdb/include/fastdb.h:{line}: "
        f"Warning {code}: {EXPECTED_MESSAGES[line]}"
        for line, code in MODULE.EXPECTED_SWIG_DIAGNOSTICS
    ]


class SwigDiagnosticTests(unittest.TestCase):
    def test_accepts_exact_legacy_diagnostic_set_with_arbitrary_build_paths(self) -> None:
        MODULE.check_swig_diagnostics("\n".join(accepted_swig_lines()))

    def test_rejects_a_new_diagnostic(self) -> None:
        lines = accepted_swig_lines()
        lines.append("other.i:17: Warning 999: unexpected diagnostic")
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_swig_diagnostics("\n".join(lines))

    def test_rejects_a_missing_diagnostic(self) -> None:
        lines = accepted_swig_lines()[:-1]
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_swig_diagnostics("\n".join(lines))

    def test_rejects_a_duplicate_diagnostic(self) -> None:
        lines = accepted_swig_lines()
        lines.append(lines[0])
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_swig_diagnostics("\n".join(lines))

    def test_rejects_changed_message_at_an_accepted_location(self) -> None:
        lines = accepted_swig_lines()
        lines[0] = lines[0].replace("TileBox ignored", "DifferentType ignored")
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_swig_diagnostics("\n".join(lines))


class InventoryTests(unittest.TestCase):
    def test_rejects_top_level_build_and_dist_directories(self) -> None:
        for path in ("build/leaked.o", "dist/leaked.whl"):
            with self.subTest(path=path), self.assertRaises(MODULE.CheckError):
                MODULE.reject_debris({path}, "archive")

    def test_rejects_sdist_members_outside_the_exact_root(self) -> None:
        with self.assertRaises(MODULE.CheckError):
            MODULE.strip_sdist_root(
                ["fastdb4py-0.1.22/README.md", "outside-root.txt"],
                "fastdb4py-0.1.22",
            )

    def test_requires_exact_three_native_wheel_artifacts(self) -> None:
        exact = {
            "fastdb4py/core/_fastdb4py.so",
            "fastdb4py/core/libfastdb.so",
            "fastdb4py/core/libfastdb4py.so",
        }
        MODULE.check_wheel_native_artifacts(exact)
        for missing in exact:
            with self.subTest(missing=missing), self.assertRaises(MODULE.CheckError):
                MODULE.check_wheel_native_artifacts(exact - {missing})
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_wheel_native_artifacts(exact | {"fastdb4py/core/extra.so"})
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_wheel_native_artifacts(exact | {"other/extra.so"})


if __name__ == "__main__":
    unittest.main()
