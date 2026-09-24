#!/usr/bin/env python3
"""Verify the public source-build Emscripten exception option contract."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex


class CheckError(RuntimeError):
    pass


def has_pthread_configuration(command: str) -> bool:
    """Return whether an Emscripten command configures pthread support."""
    tokens = shlex.split(command)
    for index, token in enumerate(tokens):
        if token == "-pthread":
            return True
        if token.startswith(("-sUSE_PTHREADS", "-sPTHREAD_POOL_SIZE")):
            return True
        if token == "-s" and index + 1 < len(tokens):
            if tokens[index + 1].startswith(("USE_PTHREADS", "PTHREAD_POOL_SIZE")):
                return True
    return False


def command_rows(build_dir: Path) -> list[dict[str, str]]:
    path = build_dir / "compile_commands.json"
    if not path.is_file():
        raise CheckError(f"missing compile command database: {path}")
    rows = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(rows, list):
        raise CheckError("compile_commands.json root is not an array")
    return rows


def commands_for(rows: list[dict[str, str]], suffix: str, marker: str = "") -> list[str]:
    commands = [
        row["command"]
        for row in rows
        if row.get("file", "").endswith(suffix)
        and (not marker or marker in row.get("command", ""))
    ]
    if not commands:
        raise CheckError(f"no compile command for {suffix} {marker}".rstrip())
    return commands


def link_command(build_dir: Path, target: str) -> str:
    matches = sorted(
        build_dir.rglob(f"CMakeFiles/{target}.dir/link.txt"), key=lambda p: str(p)
    )
    if len(matches) != 1:
        raise CheckError(f"expected one link command for {target}, found {len(matches)}")
    command = matches[0].read_text(encoding="utf-8")
    response_files = sorted(matches[0].parent.glob("*.rsp"), key=lambda p: str(p))
    return "\n".join(
        [command, *(path.read_text(encoding="utf-8") for path in response_files)]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    build_dir = args.build_dir.resolve(strict=True)
    rows = command_rows(build_dir)

    core = commands_for(rows, "/src/payload/abi/fastdb_payload.cpp")
    pure_c = commands_for(rows, "/tests/cpp/payload/test_c_header_smoke.c")
    facade = commands_for(rows, "/tests/cpp/payload/test_runtime_cpp_facade.cpp")
    runtime = commands_for(
        rows,
        "/tests/cpp/payload/test_runtime_abi.cpp",
        "wasm_runtime_abi_single_thread",
    )
    if not all("-fexceptions" in command for command in core):
        raise CheckError("Core C++ catch sites are missing -fexceptions")
    if any("-fexceptions" in command for command in pure_c):
        raise CheckError("the pure-C consumer compile is polluted by -fexceptions")
    if not all("-fexceptions" in command for command in [*facade, *runtime]):
        raise CheckError("a public C++ consumer compile is missing -fexceptions")
    if any(has_pthread_configuration(command) for command in runtime):
        raise CheckError("the single-thread runtime ABI compile configures pthreads")

    targets = [
        "fastdb_payload_test_c_header_smoke",
        "fastdb_payload_test_runtime_cpp_facade",
        "fastdb_payload_wasm_runtime_abi_single_thread",
    ]
    for target in targets:
        link = link_command(build_dir, target)
        if "-fexceptions" not in link:
            raise CheckError(f"{target} final link is missing -fexceptions")
        if "libfastdb.a" not in link:
            raise CheckError(f"{target} does not consume the public fastdb target")
    if has_pthread_configuration(
        link_command(build_dir, "fastdb_payload_wasm_runtime_abi_single_thread")
    ):
        raise CheckError("the single-thread runtime ABI final link configures pthreads")

    print("Emscripten exception propagation check passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckError, FileNotFoundError, json.JSONDecodeError) as error:
        print(f"Emscripten exception propagation check failed: {error}")
        raise SystemExit(1) from error
