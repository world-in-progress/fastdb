#!/usr/bin/env python3
"""Tests for deterministic Wasm payload export generation."""

from __future__ import annotations

import ast
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "tools" / "generate_payload_wasm_exports.py"
ALLOWLIST = ROOT / "tests" / "abi" / "fastdb_payload_v1_symbols.txt"


def run_generator(allowlist: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(GENERATOR),
            "--allowlist",
            str(allowlist),
            "--output",
            str(output),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
    )


def test_exact_deterministic_export_inventory() -> None:
    symbols = ALLOWLIST.read_text(encoding="utf-8").splitlines()
    with tempfile.TemporaryDirectory() as temporary_directory:
        output = Path(temporary_directory) / "payload-exports.txt"
        first = run_generator(ALLOWLIST, output)
        assert first.returncode == 0, first.stderr
        first_bytes = output.read_bytes()
        exports = ast.literal_eval(first_bytes.decode("utf-8"))
        assert exports == ["_malloc", "_free", *[f"_{symbol}" for symbol in symbols]]
        assert len(exports) == 107

        second = run_generator(ALLOWLIST, output)
        assert second.returncode == 0, second.stderr
        assert output.read_bytes() == first_bytes


def test_invalid_allowlist_fails_without_replacing_output() -> None:
    with tempfile.TemporaryDirectory() as temporary_directory:
        temporary = Path(temporary_directory)
        allowlist = temporary / "symbols.txt"
        output = temporary / "payload-exports.txt"
        allowlist.write_text(
            "fdb_payload_v1_spec_retain\nfdb_payload_v1_abi_version\n",
            encoding="utf-8",
        )
        output.write_text("sentinel\n", encoding="utf-8")

        result = run_generator(allowlist, output)
        assert result.returncode != 0
        assert "sorted and unique" in result.stderr
        assert output.read_text(encoding="utf-8") == "sentinel\n"


if __name__ == "__main__":
    test_exact_deterministic_export_inventory()
    test_invalid_allowlist_fails_without_replacing_output()
    print("payload Wasm export generator tests passed: 2")
