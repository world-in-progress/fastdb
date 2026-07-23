#!/usr/bin/env python3
"""Focused tests for the Python package quality gate."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "check_python_package_inventory.py"
SPEC = importlib.util.spec_from_file_location("check_python_package_inventory", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


FORMER_SWIG_LINES = (
    "/tmp/build/fastdb.h:582: Warning " + "325: "
    "Nested struct not currently supported (TileBox ignored)",
    "/tmp/build/fastdb.h:588: Warning " + "325: "
    "Nested class not currently supported (HandleTileAction ignored)",
    "/tmp/build/fastdb.h:595: Warning " + "325: "
    "Nested struct not currently supported (TakeResult ignored)",
    "/tmp/build/fastdb.h:622: Warning " + "325: "
    "Nested struct not currently supported (TileDataHandle ignored)",
    "/tmp/build/fastdb.h:631: Warning " + "325: "
    "Nested struct not currently supported (TileDbBox ignored)",
    "/tmp/build/fastdb.h:637: Warning " + "325: "
    "Nested struct not currently supported (TakeResult ignored)",
    "/tmp/build/fastdb.h:206: Warning " + "451: "
    "Setting a const char * variable may leak memory.",
)


class SwigDiagnosticTests(unittest.TestCase):
    def test_accepts_only_zero_swig_diagnostics(self) -> None:
        try:
            MODULE.check_swig_diagnostics("")
            MODULE.check_swig_diagnostics("ordinary compiler output\n")
        except MODULE.CheckError as error:
            self.fail(f"zero SWIG diagnostics must pass: {error}")

    def test_rejects_each_former_swig_diagnostic(self) -> None:
        for line in FORMER_SWIG_LINES:
            with self.subTest(line=line), self.assertRaises(MODULE.CheckError):
                MODULE.check_swig_diagnostics(line)

    def test_rejects_any_new_matched_swig_diagnostic(self) -> None:
        with self.assertRaises(MODULE.CheckError):
            MODULE.check_swig_diagnostics(
                r"C:\build\other.i:17: Warning 999: unexpected diagnostic"
            )


class InventoryTests(unittest.TestCase):
    def test_requires_every_retained_standalone_python_module(self) -> None:
        retained = {
            "fastdb4py/__init__.py",
            "fastdb4py/cli.py",
            "fastdb4py/column_view.py",
            "fastdb4py/decorator.py",
            "fastdb4py/feature/__init__.py",
            "fastdb4py/layout.py",
            "fastdb4py/materialize.py",
            "fastdb4py/object_engine.py",
            "fastdb4py/orm/__init__.py",
            "fastdb4py/orm/table.py",
            "fastdb4py/push.py",
            "fastdb4py/push_compiler.py",
            "fastdb4py/reader.py",
            "fastdb4py/record_engine.py",
            "fastdb4py/registry.py",
            "fastdb4py/serializer.py",
            "fastdb4py/string_column.py",
            "fastdb4py/type.py",
            "fastdb4py/view_owner.py",
        }
        self.assertTrue(retained <= MODULE.WHEEL_REQUIRED)
        self.assertTrue(
            {f"python/{path}" for path in retained} <= MODULE.SDIST_REQUIRED
        )
        with self.assertRaises(MODULE.CheckError):
            MODULE.require_members(
                set(MODULE.WHEEL_REQUIRED) - {"fastdb4py/record_engine.py"},
                set(MODULE.WHEEL_REQUIRED),
                "wheel",
            )

    def test_forbids_removed_python_authority_in_sdist_and_wheel(self) -> None:
        wheel_forbidden = {
            "fastdb4py/" + "call" + "_db.py",
            "fastdb4py/schema.py",
            "fastdb4py/require.py",
            "fastdb4py/allocator.py",
            "fastdb4py/codegen/__init__.py",
            "fastdb4py/codegen/ts_gen.py",
        }
        sdist_forbidden = {
            f"python/{path}" for path in wheel_forbidden
        }
        self.assertEqual(MODULE.WHEEL_FORBIDDEN, wheel_forbidden)
        self.assertEqual(MODULE.SDIST_FORBIDDEN, sdist_forbidden)
        with self.assertRaises(MODULE.CheckError):
            MODULE.reject_members(
                {
                    "fastdb4py/__init__.py",
                    "fastdb4py/" + "call" + "_db.py",
                },
                MODULE.WHEEL_FORBIDDEN,
                "wheel",
            )
        with self.assertRaises(MODULE.CheckError):
            MODULE.reject_members(
                {"python/fastdb4py/schema.py"},
                MODULE.SDIST_FORBIDDEN,
                "sdist",
            )

    def test_requires_task8_codegen_projection_in_sdist_and_wheel(self) -> None:
        self.assertIn("python/fastdb4py/payload/_codegen.py", MODULE.SDIST_REQUIRED)
        self.assertIn("fastdb4py/payload/_codegen.py", MODULE.WHEEL_REQUIRED)

    def test_requires_task3_runtime_projection_in_sdist_and_wheel(self) -> None:
        self.assertIn("python/fastdb4py/payload/_runtime.py", MODULE.SDIST_REQUIRED)
        self.assertIn("fastdb4py/payload/_runtime.py", MODULE.WHEEL_REQUIRED)

    def test_requires_task2_builder_projection_in_sdist_and_wheel(self) -> None:
        self.assertIn("python/fastdb4py/payload/_builder.py", MODULE.SDIST_REQUIRED)
        self.assertIn("fastdb4py/payload/_builder.py", MODULE.WHEEL_REQUIRED)

    def test_requires_task1_payload_projection_in_sdist_and_wheel(self) -> None:
        sdist_required = {
            "python/fastdb4py/payload/__init__.py",
            "python/fastdb4py/payload/_error.py",
            "python/fastdb4py/payload/_ffi.py",
            "python/fastdb4py/payload/_spec.py",
        }
        wheel_required = {
            "fastdb4py/payload/__init__.py",
            "fastdb4py/payload/_error.py",
            "fastdb4py/payload/_ffi.py",
            "fastdb4py/payload/_spec.py",
        }
        self.assertTrue(sdist_required <= MODULE.SDIST_REQUIRED)
        self.assertTrue(wheel_required <= MODULE.WHEEL_REQUIRED)
        with self.assertRaises(MODULE.CheckError):
            MODULE.require_members(set(), wheel_required, "wheel")

    def test_requires_the_complete_p3_graph_core_inventory(self) -> None:
        required = {
            "fastcarto/fastdb/src/payload/build/GraphAuthoring.cpp",
            "fastcarto/fastdb/src/payload/build/GraphAuthoring.hpp",
            "fastcarto/fastdb/src/payload/build/GraphEncoder.cpp",
            "fastcarto/fastdb/src/payload/build/GraphEncoder.hpp",
            "fastcarto/fastdb/src/payload/layout/GraphLayout.cpp",
            "fastcarto/fastdb/src/payload/layout/GraphLayout.hpp",
            "fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc",
            "fastcarto/fastdb/src/payload/spec/Manifest.cpp",
            "fastcarto/fastdb/src/payload/view/GraphMaterialize.cpp",
            "fastcarto/fastdb/src/payload/view/GraphOpen.cpp",
            "fastcarto/fastdb/src/payload/view/GraphOpen.hpp",
            "fastcarto/fastdb/src/payload/view/GraphView.cpp",
        }
        self.assertTrue(required <= MODULE.SDIST_REQUIRED)

    def test_cli_help_starts_on_supported_python_3_10(self) -> None:
        python310 = shutil.which("python3.10")
        if python310 is None:
            self.skipTest("python3.10 is not installed")
        completed = subprocess.run(
            [python310, str(MODULE_PATH), "--help"],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_distribution_identity_normalizes_only_name_separators(self) -> None:
        self.assertEqual(
            MODULE.canonical_distribution_name("FastDB_4.py"), "fastdb-4-py"
        )

    def test_metadata_identity_rejects_duplicate_name(self) -> None:
        with self.assertRaises(MODULE.CheckError):
            MODULE.metadata_identity(
                b"Name: first\nName: second\nVersion: 1\n\n", "fixture"
            )

    def test_artifact_identity_rejects_name_or_version_drift(self) -> None:
        with self.assertRaises(MODULE.CheckError):
            MODULE.artifact_identity(
                Path("fastdb4py-0.1.22.tar.gz"),
                Path("other-0.1.22-py3-none-any.whl"),
            )
        with self.assertRaises(MODULE.CheckError):
            MODULE.artifact_identity(
                Path("fastdb4py-0.1.22.tar.gz"),
                Path("fastdb4py-0.1.23-py3-none-any.whl"),
            )

    def test_rejects_top_level_build_and_dist_directories(self) -> None:
        for path in (
            "build/leaked.o",
            "dist/leaked.whl",
            "tests/golden/payload.bin",
            "tests/fuzz/payload/corpus/seed",
        ):
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
