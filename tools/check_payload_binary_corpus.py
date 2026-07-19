#!/usr/bin/env python3
"""Materialize or verify the reviewed portable-payload binary seed corpus."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tests/fuzz/payload/binary-corpus.json"
CORPUS = ROOT / "tests/fuzz/payload/binary-corpus"
EXPECTED_CASES = (
    ("valid-empty.bin", "valid-empty", 0, ""),
    ("valid-fixed.bin", "valid-fixed", 0, ""),
    ("valid-text.bin", "valid-text", 0, ""),
    ("valid-list.bin", "valid-list", 0, ""),
    ("malformed-magic.bin", "malformed-magic", 3001, "/binary/header/magic"),
    (
        "malformed-length.bin",
        "malformed-length",
        3009,
        "/binary/header/total_length",
    ),
    (
        "malformed-offset.bin",
        "malformed-offset",
        3005,
        "/binary/regions/0/data_offset",
    ),
    (
        "malformed-validity.bin",
        "malformed-validity",
        3009,
        "/binary/regions/1/data",
    ),
    (
        "malformed-text.bin",
        "malformed-text",
        2010,
        "/entries/records/0/scalars/j_str",
    ),
    (
        "malformed-list.bin",
        "malformed-list",
        3004,
        "/entries/records/0/bools",
    ),
)


class CheckError(RuntimeError):
    pass


def load_manifest() -> list[dict[str, object]]:
    document = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if document.get("schema") != "fastdb.payload.binary-fuzz-corpus.v1":
        raise CheckError("unexpected binary corpus manifest schema")
    cases = document.get("cases")
    if not isinstance(cases, list):
        raise CheckError("binary corpus cases must be an array")
    if not all(isinstance(case, dict) for case in cases):
        raise CheckError("every binary corpus case must be an object")
    actual_cases = tuple(
        (
            case.get("name"),
            case.get("class"),
            case.get("expected", {}).get("status")
            if isinstance(case.get("expected"), dict)
            else None,
            case.get("expected", {}).get("path")
            if isinstance(case.get("expected"), dict)
            else None,
        )
        for case in cases
    )
    if actual_cases != EXPECTED_CASES:
        raise CheckError(
            "binary corpus name/class order does not match the reviewed inventory"
        )
    return cases


def source_bytes(
    case: dict[str, object], resolved: dict[str, bytes]
) -> bytearray:
    relative = case.get("source_hex")
    source_seed = case.get("source_seed")
    if isinstance(relative, str) == isinstance(source_seed, str):
        raise CheckError(
            f"{case.get('name')}: exactly one of source_hex/source_seed is required"
        )
    if isinstance(source_seed, str):
        if source_seed not in resolved:
            raise CheckError(
                f"{case.get('name')}: source_seed must name an earlier reviewed case"
            )
        return bytearray(resolved[source_seed])
    path = ROOT / str(relative)
    try:
        return bytearray.fromhex("".join(path.read_text(encoding="ascii").split()))
    except (OSError, ValueError) as error:
        raise CheckError(f"{case.get('name')}: cannot decode {relative}: {error}") from error


def expected_bytes(case: dict[str, object], resolved: dict[str, bytes]) -> bytes:
    data = source_bytes(case, resolved)
    patches = case.get("patches", [])
    if not isinstance(patches, list):
        raise CheckError(f"{case.get('name')}: patches must be an array")
    for patch in patches:
        if not isinstance(patch, dict):
            raise CheckError(f"{case.get('name')}: every patch must be an object")
        offset = patch.get("offset")
        before = patch.get("before")
        after = patch.get("after")
        if not isinstance(offset, int) or offset < 0:
            raise CheckError(f"{case.get('name')}: patch offset is invalid")
        try:
            before_bytes = bytes.fromhex(before) if isinstance(before, str) else b""
            after_bytes = bytes.fromhex(after) if isinstance(after, str) else b""
        except ValueError as error:
            raise CheckError(f"{case.get('name')}: patch bytes are not hex") from error
        if not before_bytes or len(before_bytes) != len(after_bytes):
            raise CheckError(
                f"{case.get('name')}: patch before/after lengths must match and be nonzero"
            )
        end = offset + len(before_bytes)
        if end > len(data):
            raise CheckError(f"{case.get('name')}: patch range is out of bounds")
        if data[offset:end] != before_bytes:
            raise CheckError(
                f"{case.get('name')}: reviewed source bytes at {offset} changed"
            )
        data[offset:end] = after_bytes
    digest = hashlib.sha256(data).hexdigest()
    if digest != case.get("sha256"):
        raise CheckError(
            f"{case.get('name')}: recipe digest {digest} does not match manifest"
        )
    return bytes(data)


def resolve_cases(cases: list[dict[str, object]]) -> dict[str, bytes]:
    resolved: dict[str, bytes] = {}
    for case in cases:
        name = str(case["name"])
        if name in resolved:
            raise CheckError(f"duplicate binary corpus case: {name}")
        resolved[name] = expected_bytes(case, resolved)
    return resolved


def verify(cases: list[dict[str, object]]) -> None:
    resolved = resolve_cases(cases)
    if not CORPUS.is_dir():
        raise CheckError(f"missing reviewed binary corpus directory: {CORPUS}")
    entries = sorted(CORPUS.iterdir(), key=lambda path: path.name)
    non_files = [
        path.name for path in entries if path.is_symlink() or not path.is_file()
    ]
    if non_files:
        raise CheckError(f"binary corpus contains non-file debris: {non_files}")
    actual_names = [path.name for path in entries]
    expected_names = sorted(str(case["name"]) for case in cases)
    if actual_names != expected_names:
        raise CheckError(
            f"binary corpus inventory mismatch: expected {expected_names}, got {actual_names}"
        )
    for case in cases:
        path = CORPUS / str(case["name"])
        actual = path.read_bytes()
        expected = resolved[str(case["name"])]
        if actual != expected:
            raise CheckError(f"{path.name}: bytes do not match the reviewed recipe")


def materialize(cases: list[dict[str, object]]) -> None:
    resolved = resolve_cases(cases)
    CORPUS.mkdir(parents=True, exist_ok=True)
    expected_names = {str(case["name"]) for case in cases}
    for existing in CORPUS.iterdir():
        if (
            existing.is_symlink()
            or not existing.is_file()
            or existing.name not in expected_names
        ):
            raise CheckError(f"refusing to replace unreviewed corpus entry: {existing}")
    for case in cases:
        name = str(case["name"])
        (CORPUS / name).write_bytes(resolved[name])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--materialize", action="store_true")
    arguments = parser.parse_args()
    cases = load_manifest()
    if arguments.materialize:
        materialize(cases)
    verify(cases)
    print(f"Portable payload binary corpus check passed: {len(cases)} reviewed seeds")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckError, OSError, json.JSONDecodeError) as error:
        print(f"Portable payload binary corpus check failed: {error}")
        raise SystemExit(1) from error
