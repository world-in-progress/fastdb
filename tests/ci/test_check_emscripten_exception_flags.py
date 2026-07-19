#!/usr/bin/env python3
"""Focused tests for Emscripten exception/pthread structural inspection."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "check_emscripten_exception_flags.py"
SPEC = importlib.util.spec_from_file_location(
    "check_emscripten_exception_flags", MODULE_PATH
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PthreadConfigurationTests(unittest.TestCase):
    def test_accepts_an_unrelated_single_thread_command(self) -> None:
        self.assertFalse(
            MODULE.has_pthread_configuration(
                "em++ -fexceptions -o runtime.js runtime.cpp"
            )
        )

    def test_rejects_every_supported_emscripten_pthread_spelling(self) -> None:
        commands = [
            "em++ -pthread runtime.cpp",
            "em++ -sUSE_PTHREADS=1 runtime.cpp",
            "em++ -s USE_PTHREADS=1 runtime.cpp",
            "em++ -sPTHREAD_POOL_SIZE=4 runtime.cpp",
            "em++ -s PTHREAD_POOL_SIZE=4 runtime.cpp",
        ]
        for command in commands:
            with self.subTest(command=command):
                self.assertTrue(MODULE.has_pthread_configuration(command))


if __name__ == "__main__":
    unittest.main()
