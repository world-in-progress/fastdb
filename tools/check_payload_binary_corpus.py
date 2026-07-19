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
    ("valid-empty.bin", "valid-empty"),
    ("valid-fixed.bin", "valid-fixed"),
    ("valid-text.bin", "valid-text"),
    ("valid-list.bin", "valid-list"),
    ("malformed-magic.bin", "malformed-magic"),
    ("malformed-length.bin", "malformed-length"),
    ("malformed-offset.bin", "malformed-offset"),
    ("malformed-validity.bin", "malformed-validity"),
    ("malformed-text.bin", "malformed-text"),
    ("malformed-list.bin", "malformed-list"),
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
    actual_cases = tuple((case.get("name"), case.get("class")) for case in cases)
    if actual_cases != EXPECTED_CASES:
        raise CheckError(
            "binary corpus name/class order does not match the reviewed inventory"
        )
    return cases


def decoded_source(case: dict[str, object]) -> bytearray:
    relative = case.get("source_hex")
    if not isinstance(relative, str):
        raise CheckError(f"{case.get('name')}: source_hex must be a string")
    path = ROOT / relative
    try:
        return bytearray.fromhex("".join(path.read_text(encoding="ascii").split()))
    except (OSError, ValueError) as error:
        raise CheckError(f"{case.get('name')}: cannot decode {relative}: {error}") from error


def expected_bytes(case: dict[str, object]) -> bytes:
    data = decoded_source(case)
    mutation = case.get("mutation")
    if mutation is not None:
        if not isinstance(mutation, dict):
            raise CheckError(f"{case.get('name')}: mutation must be an object")
        offset = mutation.get("offset")
        before = mutation.get("before")
        after = mutation.get("after")
        if not isinstance(offset, int) or offset < 0 or offset >= len(data):
            raise CheckError(f"{case.get('name')}: mutation offset is out of range")
        try:
            before_bytes = bytes.fromhex(before) if isinstance(before, str) else b""
            after_bytes = bytes.fromhex(after) if isinstance(after, str) else b""
        except ValueError as error:
            raise CheckError(f"{case.get('name')}: mutation bytes are not hex") from error
        if len(before_bytes) != 1 or len(after_bytes) != 1:
            raise CheckError(f"{case.get('name')}: mutation must replace exactly one byte")
        if data[offset] != before_bytes[0]:
            raise CheckError(
                f"{case.get('name')}: reviewed source byte at {offset} changed"
            )
        data[offset] = after_bytes[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != case.get("sha256"):
        raise CheckError(
            f"{case.get('name')}: recipe digest {digest} does not match manifest"
        )
    return bytes(data)


def verify(cases: list[dict[str, object]]) -> None:
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
        expected = expected_bytes(case)
        if actual != expected:
            raise CheckError(f"{path.name}: bytes do not match the reviewed recipe")


def materialize(cases: list[dict[str, object]]) -> None:
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
        (CORPUS / str(case["name"])).write_bytes(expected_bytes(case))


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
