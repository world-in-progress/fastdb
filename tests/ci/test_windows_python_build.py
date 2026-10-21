#!/usr/bin/env python3
"""Check the actual setup.py configure command for Windows Python targets."""
import os
import multiprocessing  # Load the host backend before simulating sys.platform.
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

from setuptools import Distribution


ROOT = Path(__file__).resolve().parents[2]


class WindowsPythonBuildTests(unittest.TestCase):
    def configure(self, target, generator="", *, detected="win-amd64"):
        with patch("setuptools.setup"):
            setup = runpy.run_path(str(ROOT / "setup.py"))
        with tempfile.TemporaryDirectory() as temporary:
            build = setup["CMakeBuild"](Distribution())
            build.build_temp = str(Path(temporary) / "build")
            build.build_lib = str(Path(temporary) / "lib")
            build.plat_name = target
            extension = setup["CMakeExtension"]("fastdb4py.core._fastdb4py", str(ROOT))
            with patch.object(build, "get_ext_fullpath", return_value=str(Path(temporary) / "module.pyd")), \
                    patch.object(build, "copy_tree"), patch.object(setup["sys"], "platform", "win32"), \
                    patch.object(setup["sysconfig"], "get_platform", return_value=detected), \
                    patch.dict(os.environ, {"CMAKE_GENERATOR": generator}), \
                    patch("subprocess.check_call") as invoke:
                build.build_extension(extension)
            return invoke.call_args_list[0].args[0]

    def test_visual_studio_matches_python_target_architecture(self):
        for target, expected in (("win-amd64", "x64"), ("win-arm64", "ARM64"), ("win32", "Win32")):
            with self.subTest(target=target):
                command = self.configure(target, "Visual Studio 17 2022")
                self.assertEqual(command[command.index("-A") + 1], expected)

    def test_explicit_wheel_target_overrides_host_python(self):
        command = self.configure("win_arm64", detected="win-amd64")
        self.assertEqual(command[command.index("-A") + 1], "ARM64")

    def test_native_arm64_python_is_used_without_an_explicit_target(self):
        command = self.configure(None, detected="win-arm64")
        self.assertEqual(command[command.index("-A") + 1], "ARM64")

    def test_ninja_uses_developer_environment_without_generator_platform(self):
        for generator in ("Ninja", "Ninja Multi-Config"):
            with self.subTest(generator=generator):
                self.assertNotIn("-A", self.configure("win-arm64", generator))

    def test_unknown_architecture_fails_before_invoking_cmake(self):
        with self.assertRaisesRegex(RuntimeError, "Unsupported Windows Python platform"):
            self.configure("win-unknown")


if __name__ == "__main__":
    unittest.main()
