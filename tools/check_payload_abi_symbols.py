#!/usr/bin/env python3
"""Verify the exact exported FastDB portable-payload V1 C ABI."""

from __future__ import annotations

import argparse
import difflib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


SYMBOL_PREFIX = "fdb_payload_v1_"
SYMBOL_PATTERN = re.compile(r"^fdb_payload_v1_[A-Za-z0-9_]+$")
WINDOWS_LIBRARY_NAME = "fastdb.dll"
DUMPBIN_EXPORTS_HEADER = re.compile(
    r"Section contains the following exports for\b"
)
# dumpbin /exports rows: "<ordinal> <hint> <RVA> <name>"; exports by ordinal
# without a name, section headers, and the Summary table never match.
DUMPBIN_EXPORT_ROW = re.compile(
    r"^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)$"
)
ALLOWLIST = (
    Path(__file__).resolve().parent.parent
    / "tests"
    / "abi"
    / "fastdb_payload_v1_symbols.txt"
)


class CheckError(RuntimeError):
    """An actionable ABI-check failure."""


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="CMake build directory containing the FastDB shared library",
    )
    parser.add_argument(
        "--wasm-build-dir",
        type=Path,
        help="Emscripten CMake build directory containing the C ABI object",
    )
    arguments = parser.parse_args()
    if (arguments.build_dir is None) == (arguments.wasm_build_dir is None):
        parser.error("provide exactly one of --build-dir or --wasm-build-dir")
    return arguments


def platform_configuration() -> tuple[str, list[str], bool]:
    if sys.platform == "win32":
        raise CheckError(
            "Windows uses dumpbin export extraction, not the nm platform "
            "configuration"
        )
    if sys.platform == "darwin":
        return "libfastdb.dylib", ["nm", "-gU"], True
    if sys.platform.startswith("linux"):
        return "libfastdb.so", ["nm", "-D", "--defined-only"], False
    raise CheckError(
        "unsupported platform for payload ABI inspection: "
        f"{sys.platform!r}; expected Darwin, Linux, or Windows"
    )


def run_symbol_tool(command: list[str], library: Path) -> str:
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
        )
    except OSError as error:
        raise CheckError(
            f"symbol inspection tool could not run: {command[0]!r}: {error}"
        ) from error
    except UnicodeError as error:
        raise CheckError(
            f"symbol inspection output was not valid UTF-8 for {library}"
        ) from error

    if result.returncode != 0:
        diagnostic = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise CheckError(
            f"symbol inspection failed with exit {result.returncode}: "
            f"{' '.join(command)}\n{diagnostic}"
        )
    return result.stdout


def parse_dumpbin_exports(text: str) -> list[str]:
    """Extract exported symbol names from dumpbin /exports output."""
    lines = text.splitlines()
    headers = [
        index
        for index, line in enumerate(lines)
        if DUMPBIN_EXPORTS_HEADER.search(line)
    ]
    if len(headers) != 1:
        raise CheckError(
            "dumpbin output must contain exactly one export section header, "
            f"found {len(headers)}; refusing to accept ambiguous output"
        )
    names: list[str] = []
    for line in lines[headers[0] + 1 :]:
        row = DUMPBIN_EXPORT_ROW.match(line)
        if row is not None:
            names.append(row.group(1))
        elif SYMBOL_PREFIX in line:
            raise CheckError(
                "dumpbin emitted a payload-named export line that the row "
                f"parser rejects (forwards/decorations are not accepted): {line!r}"
            )
    return names


def dumpbin_exported_payload_symbols(library: Path) -> list[str]:
    dumpbin = shutil.which("dumpbin") or shutil.which("dumpbin.exe")
    if dumpbin is None:
        raise CheckError(
            "dumpbin is required for Windows payload ABI inspection; run "
            "from an MSVC developer environment (for example "
            "ilammy/msvc-dev-cmd) so dumpbin is on PATH"
        )
    stdout = run_symbol_tool([dumpbin, "/exports", os.fspath(library)], library)
    symbols = {
        name for name in parse_dumpbin_exports(stdout) if name.startswith(SYMBOL_PREFIX)
    }
    return sorted(symbols)


def resolved_build_directory(path: Path) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except FileNotFoundError as error:
        raise CheckError(f"build directory does not exist: {path}") from error
    if not resolved.is_dir():
        raise CheckError(f"build path is not a directory: {resolved}")
    return resolved


def locate_library(build_directory: Path, library_name: str) -> Path:
    candidates: list[Path] = []
    for candidate in build_directory.rglob(library_name):
        try:
            resolved = candidate.resolve(strict=True)
        except FileNotFoundError:
            continue
        try:
            resolved.relative_to(build_directory)
        except ValueError as error:
            raise CheckError(
                "FastDB shared-library candidate escapes the build directory: "
                f"{candidate} -> {resolved}"
            ) from error
        if resolved.is_file():
            candidates.append(candidate)

    candidates = sorted(set(candidates), key=lambda value: os.fspath(value))
    if not candidates:
        raise CheckError(
            f"no {library_name} found beneath build directory "
            f"{build_directory}; build the native fastdb shared library first"
        )
    if len(candidates) != 1:
        listing = "\n".join(f"  - {candidate}" for candidate in candidates)
        raise CheckError(
            f"ambiguous {library_name} beneath build directory "
            f"{build_directory}; found {len(candidates)} candidates:\n{listing}"
        )
    return candidates[0]


def read_allowlist(path: Path) -> list[str]:
    if not path.is_file():
        raise CheckError(
            f"payload ABI symbol allowlist is missing: {path}; "
            "create the reviewed exact symbol inventory before accepting ABI drift"
        )
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise CheckError(f"payload ABI allowlist is not valid UTF-8: {path}") from error

    lines = text.splitlines()
    if not lines:
        raise CheckError(f"payload ABI allowlist is empty: {path}")
    if any(not line for line in lines):
        raise CheckError(f"payload ABI allowlist contains a blank line: {path}")
    malformed = [line for line in lines if SYMBOL_PATTERN.fullmatch(line) is None]
    if malformed:
        listing = "\n".join(f"  - {line!r}" for line in malformed)
        raise CheckError(
            f"payload ABI allowlist contains malformed symbol lines:\n{listing}"
        )
    expected_order = sorted(set(lines))
    if lines != expected_order:
        raise CheckError(
            "payload ABI allowlist must be sorted and unique; exact correction:\n"
            + "\n".join(expected_order)
        )
    return lines


def exported_payload_symbols(
    library: Path,
    nm_command: list[str],
    strip_platform_underscore: bool,
) -> list[str]:
    stdout = run_symbol_tool([*nm_command, os.fspath(library)], library)

    symbols: set[str] = set()
    for line in stdout.splitlines():
        fields = line.split()
        if not fields:
            continue
        symbol = fields[-1]
        if strip_platform_underscore and symbol.startswith("_"):
            symbol = symbol[1:]
        if symbol.startswith(SYMBOL_PREFIX):
            symbols.add(symbol)
    return sorted(symbols)


def exact_diff(expected: list[str], actual: list[str], library: Path) -> str:
    removed = sorted(set(expected) - set(actual))
    added = sorted(set(actual) - set(expected))
    summary: list[str] = []
    if removed:
        summary.append("removed exports:\n" + "\n".join(f"  - {x}" for x in removed))
    if added:
        summary.append("added exports:\n" + "\n".join(f"  + {x}" for x in added))
    diff = "\n".join(
        difflib.unified_diff(
            expected,
            actual,
            fromfile=os.fspath(ALLOWLIST),
            tofile=os.fspath(library),
            lineterm="",
        )
    )
    return "\n".join([*summary, "exact symbol diff:", diff])


def main() -> int:
    try:
        arguments = parse_arguments()
        expected = read_allowlist(ALLOWLIST)
        if len(expected) != 117:
            raise CheckError(
                f"reviewed portable payload ABI must contain exactly 117 symbols, found {len(expected)}"
            )
        if arguments.wasm_build_dir is not None:
            build_directory = resolved_build_directory(arguments.wasm_build_dir)
            candidates = sorted(
                build_directory.rglob("fastdb_payload.cpp.o"),
                key=lambda value: os.fspath(value),
            )
            if len(candidates) != 1:
                raise CheckError(
                    "expected exactly one Emscripten fastdb_payload.cpp.o, "
                    f"found {len(candidates)}"
                )
            emnm = shutil.which("emnm")
            if emnm is None:
                raise CheckError("emnm is required for wasm ABI inspection")
            library = candidates[0]
            actual = exported_payload_symbols(
                library, [emnm, "--defined-only"], False
            )
        else:
            build_directory = resolved_build_directory(arguments.build_dir)
            if sys.platform == "win32":
                library = locate_library(build_directory, WINDOWS_LIBRARY_NAME)
                actual = dumpbin_exported_payload_symbols(library)
            else:
                library_name, nm_command, strip_platform_underscore = (
                    platform_configuration()
                )
                library = locate_library(build_directory, library_name)
                actual = exported_payload_symbols(
                    library, nm_command, strip_platform_underscore
                )
        if actual != expected:
            raise CheckError(
                "portable payload ABI symbol set differs from the reviewed allowlist\n"
                + exact_diff(expected, actual, library)
            )
    except CheckError as error:
        print(f"payload ABI symbol check failed: {error}", file=sys.stderr)
        return 1

    print(
        f"payload ABI symbol check passed: {len(expected)} exact symbols in {library}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
