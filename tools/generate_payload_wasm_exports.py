#!/usr/bin/env python3
"""Generate deterministic Emscripten exports from the reviewed payload ABI."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import sys
import tempfile


SYMBOL_PATTERN = re.compile(r"^fdb_payload_v1_[A-Za-z0-9_]+$")
EXPECTED_ABI_SYMBOLS = 105


class GenerationError(RuntimeError):
    """An actionable export-generation failure."""


def parse_arguments() -> argparse.Namespace:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--allowlist",
        type=Path,
        default=root / "tests" / "abi" / "fastdb_payload_v1_symbols.txt",
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def read_symbols(path: Path) -> list[str]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise GenerationError(f"cannot read UTF-8 ABI allowlist {path}: {error}") from error
    symbols = text.splitlines()
    if not symbols or any(not symbol for symbol in symbols):
        raise GenerationError("ABI allowlist must be non-empty with no blank lines")
    malformed = [symbol for symbol in symbols if not SYMBOL_PATTERN.fullmatch(symbol)]
    if malformed:
        raise GenerationError(f"ABI allowlist has malformed symbol: {malformed[0]!r}")
    expected_order = sorted(set(symbols))
    if symbols != expected_order:
        raise GenerationError("ABI allowlist must be sorted and unique")
    if len(symbols) != EXPECTED_ABI_SYMBOLS:
        raise GenerationError(
            "reviewed portable payload ABI must contain exactly "
            f"{EXPECTED_ABI_SYMBOLS} symbols, found {len(symbols)}"
        )
    return symbols


def render(symbols: list[str]) -> str:
    exports = ["_malloc", "_free", *[f"_{symbol}" for symbol in symbols]]
    # CMake's SHELL link-option expansion treats spaces as argument separators,
    # so the Python-literal list must be compact as well as deterministic.
    return "[" + ",".join(repr(export) for export in exports) + "]\n"


def write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{path.name}.",
            dir=path.parent,
            delete=False,
        ) as temporary:
            temporary.write(content)
            temporary_name = temporary.name
        os.replace(temporary_name, path)
        temporary_name = None
    finally:
        if temporary_name is not None:
            try:
                Path(temporary_name).unlink()
            except FileNotFoundError:
                pass


def main() -> int:
    arguments = parse_arguments()
    try:
        symbols = read_symbols(arguments.allowlist)
        write_atomic(arguments.output, render(symbols))
    except GenerationError as error:
        print(f"payload Wasm export generation failed: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"payload Wasm export generation failed: {error}", file=sys.stderr)
        return 1
    print(
        f"generated {EXPECTED_ABI_SYMBOLS} payload ABI exports plus malloc/free: "
        f"{arguments.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
