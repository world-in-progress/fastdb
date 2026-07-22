#!/usr/bin/env python3
"""Fail-closed quality gate for P4 projection parity and codegen readiness."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[2]
PROOF_MAP = ROOT / "tests/ci/p4_projection_codegen_map.json"
ABI_ALLOWLIST = ROOT / "tests/abi/fastdb_payload_v1_symbols.txt"
ISSUE = ROOT / "docs/issues/0002-portable-payload-foundation-implementation-status.md"

SCHEMA = "fastdb.payload.p4-projection-codegen-map.v1"
LANGUAGES = ("cpp", "rust", "python", "typescript")
RUNTIME_IDS = (
    "canonical",
    "binary",
    "logical-value",
    "error",
    "lifetime",
    "direct-staged",
)
OBSERVATIONS = {
    "canonical": (
        "canonical-json",
        "manifest-json",
        "sha256",
        "profile-capabilities-stable-indexes",
    ),
    "binary": (
        "record-deterministic-bytes",
        "graph-deterministic-bytes",
    ),
    "logical-value": (
        "all-v1-scalars",
        "normalized-values",
        "str-wstr-bytes",
        "nested-component-recursive-list",
        "null-empty-cardinality",
        "graph-refs-identity-sharing-cycles",
    ),
    "error": (
        "five-core-fields",
        "compile-builder-open-type-backing-invalidation",
    ),
    "lifetime": (
        "clone-retain-release",
        "copy-external-ownership",
        "checked-access-invalidation",
        "detached-materialization",
        "failure-cleanup",
    ),
    "direct-staged": (
        "internal-heap-direct",
        "host-direct",
        "forced-staged-fallback",
        "direct-required-rejection",
        "truthful-execution-report",
    ),
}
PACKAGE_IDS = (
    "rust-source",
    "rust-system",
    "python-3.10-wheel",
    "typescript-wasm-package",
)
WORKFLOW_JOBS = ("rust_payload", "projection_parity")
WORKFLOW_QUALITY_PATHS = (
    "README.md",
    "docs/issues/0002-portable-payload-foundation-implementation-status.md",
)
CODEGEN_TARGETS = ("cpp", "rust", "python", "typescript")

TOP_LEVEL_KEYS = (
    "schema",
    "authority",
    "abi",
    "runtime_receipts",
    "package_boundaries",
    "workflow",
    "codegen",
)
RUNTIME_KEYS = ("id", "status", "fixtures", "observations", "proofs")
PROOF_KEYS = ("language", "cases")
PROOF_CASE_KEYS = ("file", "test", "markers")
PACKAGE_KEYS = ("id", "status", "proofs", "workflow_markers")
PACKAGE_PROOF_KEYS = ("file", "marker")

ISSUE_MARKERS = (
    "#### P4 Task 6 local evidence",
    "exactly six ordered runtime",
    "Hosted Task 6 execution remains pending",
    "P4 Task 7",
    "same-agent",
)
DOCUMENTATION_MARKERS = {
    "README.md": (
        "P4 Tasks 1-6",
        "Core-owned four-language code generation remains open",
        "definitions until an authorized hosted run exists",
    ),
    "bindings/rust/fastdb-sys/README.md": (
        "C++ Core",
        "FASTDB_PAYLOAD_LINK_MODE",
        "system",
    ),
    "bindings/rust/fastdb/README.md": (
        "C++ Core",
        "projection",
        "codegen",
    ),
    "python/README.md": ("fastdb4py.payload", "Python 3.10", "C++ Core"),
    "ts/fastdb4ts/README.md": (
        "fastdb4ts/payload",
        "WebAssembly",
        "codegen",
    ),
}


class QualityError(RuntimeError):
    """The checked repository does not satisfy the frozen P4 contract."""


def load_json_no_duplicates(source: str, label: str) -> Any:
    def object_from_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise QualityError(f"{label} contains duplicate JSON key {key!r}")
            result[key] = value
        return result

    return json.loads(source, object_pairs_hook=object_from_pairs)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise QualityError(message)


def exact_keys(value: dict[str, Any], expected: tuple[str, ...], label: str) -> None:
    require(tuple(value) == expected, f"{label} keys must be exact and ordered")


def non_empty_strings(value: Any, label: str) -> None:
    require(isinstance(value, list) and bool(value), f"{label} must be non-empty")
    require(
        all(isinstance(item, str) and bool(item) for item in value),
        f"{label} must contain only non-empty strings",
    )
    require(len(value) == len(set(value)), f"{label} must be unique")


def check_repository_relative_path(value: Any, label: str) -> str:
    require(isinstance(value, str) and bool(value), f"{label} must be non-empty")
    path = PurePosixPath(value)
    require(
        not path.is_absolute()
        and "\\" not in value
        and ".." not in path.parts
        and path.as_posix() == value,
        f"{label} must be a normalized repository-relative path",
    )
    return value


def repository_file(relative: str, label: str) -> Path:
    check_repository_relative_path(relative, label)
    try:
        resolved = (ROOT / relative).resolve(strict=True)
    except OSError as error:
        raise QualityError(f"cannot resolve {label} {relative}: {error}") from error
    require(
        resolved.is_relative_to(ROOT) and resolved.is_file(),
        f"{label} must resolve to a regular file inside the repository: {relative}",
    )
    return resolved


def check_map(document: dict[str, Any]) -> None:
    exact_keys(document, TOP_LEVEL_KEYS, "P4 proof map")
    require(document["schema"] == SCHEMA, "unexpected P4 proof-map schema")
    require(
        document["authority"]
        == {
            "semantic_authority": "cpp-core",
            "projection_rule": "abi-only-no-binding-semantics",
        },
        "P4 authority boundary must remain exact",
    )
    require(
        document["abi"]
        == {
            "version": 1,
            "symbol_count": 105,
            "allowlist": "tests/abi/fastdb_payload_v1_symbols.txt",
            "status": "frozen-p3",
        },
        "P4 runtime map must freeze the reviewed ABI-105",
    )

    receipts = document["runtime_receipts"]
    require(isinstance(receipts, list), "runtime receipts must be an array")
    require(
        tuple(row.get("id") for row in receipts) == RUNTIME_IDS,
        "runtime receipts must be exact, unique, and ordered",
    )
    for row in receipts:
        receipt_id = row["id"]
        exact_keys(row, RUNTIME_KEYS, f"runtime receipt {receipt_id}")
        require(
            row["status"] == "closed-task-6",
            f"runtime receipt {receipt_id} must be closed by Task 6",
        )
        non_empty_strings(row["fixtures"], f"{receipt_id} fixtures")
        for fixture in row["fixtures"]:
            check_repository_relative_path(fixture, f"{receipt_id} fixture")
        require(
            tuple(row["observations"]) == OBSERVATIONS[receipt_id],
            f"{receipt_id} observations must be exact and ordered",
        )
        proofs = row["proofs"]
        require(isinstance(proofs, list), f"{receipt_id} proofs must be an array")
        require(
            tuple(proof.get("language") for proof in proofs) == LANGUAGES,
            f"{receipt_id} proofs must cover each language exactly and in order",
        )
        for proof in proofs:
            language = proof["language"]
            exact_keys(proof, PROOF_KEYS, f"{receipt_id}/{language} proof")
            cases = proof["cases"]
            require(
                isinstance(cases, list) and bool(cases),
                f"{receipt_id}/{language} proof cases must be non-empty",
            )
            seen_cases = set()
            for case in cases:
                exact_keys(
                    case, PROOF_CASE_KEYS, f"{receipt_id}/{language} proof case"
                )
                require(
                    isinstance(case["file"], str) and bool(case["file"]),
                    f"{receipt_id}/{language} proof file must be non-empty",
                )
                check_repository_relative_path(
                    case["file"], f"{receipt_id}/{language} proof file"
                )
                require(
                    isinstance(case["test"], str) and bool(case["test"]),
                    f"{receipt_id}/{language} proof test must be non-empty",
                )
                non_empty_strings(
                    case["markers"], f"{receipt_id}/{language} case markers"
                )
                identity = (case["file"], case["test"])
                require(
                    identity not in seen_cases,
                    f"{receipt_id}/{language} proof case is duplicated",
                )
                seen_cases.add(identity)

    packages = document["package_boundaries"]
    require(isinstance(packages, list), "package boundaries must be an array")
    require(
        tuple(row.get("id") for row in packages) == PACKAGE_IDS,
        "package boundaries must be exact, unique, and ordered",
    )
    for row in packages:
        package_id = row["id"]
        exact_keys(row, PACKAGE_KEYS, f"package boundary {package_id}")
        require(
            row["status"] == "local-executable",
            f"package boundary {package_id} must be locally executable",
        )
        proofs = row["proofs"]
        require(isinstance(proofs, list) and proofs, f"{package_id} needs proof")
        seen = set()
        for proof in proofs:
            exact_keys(proof, PACKAGE_PROOF_KEYS, f"{package_id} package proof")
            pair = (proof["file"], proof["marker"])
            require(
                all(isinstance(item, str) and item for item in pair),
                f"{package_id} package proof must be non-empty",
            )
            require(pair not in seen, f"{package_id} package proof is duplicated")
            check_repository_relative_path(
                proof["file"], f"{package_id} package proof file"
            )
            seen.add(pair)
        non_empty_strings(
            row["workflow_markers"], f"{package_id} workflow markers"
        )

    workflow = document["workflow"]
    require(
        workflow
        == {
            "file": ".github/workflows/tests.yml",
            "jobs": list(WORKFLOW_JOBS),
            "definition_status": "definitions-only",
            "hosted_status": "pending",
        },
        "workflow truth must keep hosted execution pending",
    )

    codegen = document["codegen"]
    require(isinstance(codegen, list), "codegen rows must be an array")
    require(
        tuple(row.get("target") for row in codegen) == CODEGEN_TARGETS,
        "codegen targets must be exact, unique, and ordered",
    )
    for row in codegen:
        require(
            tuple(row) == ("target", "status", "proofs"),
            "codegen row keys must be exact and ordered",
        )
        require(row["status"] == "open", "all Task 6 codegen rows must stay open")
        require(row["proofs"] == [], "open codegen rows cannot claim proof")


def test_pattern(language: str, name: str) -> re.Pattern[str]:
    escaped = re.escape(name)
    if language == "cpp":
        return re.compile(rf"\b(?:int|bool|void)\s+{escaped}\s*\(")
    if language == "rust":
        return re.compile(rf"#\[test\][\s\S]{{0,160}}\bfn\s+{escaped}\s*\(")
    if language == "python":
        return re.compile(rf"^def\s+{escaped}\s*\(", re.MULTILINE)
    if language == "typescript":
        return re.compile(rf"\btest\(\s*(['\"]){escaped}\1")
    raise QualityError(f"unknown proof language {language!r}")


def proof_region(source: str, language: str, name: str) -> str:
    """Return the named executable proof, not unrelated text in its file."""

    match = test_pattern(language, name).search(source)
    require(match is not None, f"{language} proof test is absent: {name}")
    terminators = {
        "cpp": re.compile(
            r"^\s*(?:int|bool|void)\s+[A-Za-z_][A-Za-z0-9_]*\s*\(",
            re.MULTILINE,
        ),
        "rust": re.compile(r"^fn\s+[A-Za-z_][A-Za-z0-9_]*\s*\(", re.MULTILINE),
        "python": re.compile(r"^def\s+[A-Za-z_][A-Za-z0-9_]*\s*\(", re.MULTILINE),
        "typescript": re.compile(r"^test\s*\(", re.MULTILINE),
    }
    next_test = terminators[language].search(source, match.end())
    return source[match.start() : next_test.start() if next_test else len(source)]


def default_reader(relative: str) -> str:
    path = repository_file(relative, "proof source")
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise QualityError(f"cannot read proof source {relative}: {error}") from error


def check_proof_sources(
    document: dict[str, Any], reader: Callable[[str], str] = default_reader
) -> None:
    for row in document["runtime_receipts"]:
        receipt_id = row["id"]
        for fixture in row["fixtures"]:
            repository_file(fixture, f"{receipt_id} fixture")
        for proof in row["proofs"]:
            language = proof["language"]
            for case in proof["cases"]:
                source = reader(case["file"])
                name = case["test"]
                region = proof_region(source, language, name)
                if language == "cpp":
                    require(
                        len(re.findall(rf"\b{re.escape(name)}\s*\(", source)) >= 2,
                        f"{receipt_id}/cpp proof is defined but not executed: {name}",
                    )
                for marker in case["markers"]:
                    require(
                        marker in region,
                        f"{receipt_id}/{language} proof marker is absent from "
                        f"{name}: {marker}",
                    )


def check_package_sources(
    document: dict[str, Any], reader: Callable[[str], str] = default_reader
) -> None:
    for row in document["package_boundaries"]:
        for proof in row["proofs"]:
            source = reader(proof["file"])
            require(
                proof["marker"] in source,
                f"{row['id']} package marker is absent: {proof['marker']}",
            )


def check_workflow(document: dict[str, Any], source: str) -> None:
    for job in WORKFLOW_JOBS:
        require(
            re.search(rf"^  {re.escape(job)}:\s*$", source, re.MULTILINE)
            is not None,
            f"workflow job definition is absent: {job}",
        )
    for path in WORKFLOW_QUALITY_PATHS:
        require(
            f"- '{path}'" in source,
            f"workflow path filter omits Task 6 authority document: {path}",
        )
    for row in document["package_boundaries"]:
        for marker in row["workflow_markers"]:
            require(marker in source, f"workflow marker is absent: {marker}")
    require(
        re.search(
            r"check_p4_projection_codegen_quality\.py[\s\\]*--check-repository",
            source,
        )
        is not None,
        "workflow does not execute the Task 6 repository quality gate",
    )
    require(
        re.search(
            r"check_p4_projection_codegen_quality\.py[\s\\]*--validate-results",
            source,
        )
        is not None,
        "workflow aggregate does not execute the Task 6 result validator",
    )


def check_abi_allowlist(source: str) -> None:
    symbols = source.splitlines()
    require(len(symbols) == 105, "P4 runtime ABI allowlist must remain exactly 105")
    require(symbols == sorted(set(symbols)), "ABI allowlist must be sorted and unique")
    require(
        all(re.fullmatch(r"fdb_payload_v1_[A-Za-z0-9_]+", item) for item in symbols),
        "ABI allowlist contains an invalid symbol",
    )


def check_issue_truth(source: str) -> None:
    for marker in ISSUE_MARKERS:
        require(marker in source, f"Issue 0002 omits Task 6 truth: {marker}")
    false_claims = (
        "Hosted Task 6 execution passed",
        "Hosted projection parity passed",
        "Task 6 independent review",
    )
    for claim in false_claims:
        require(claim not in source, f"Issue 0002 makes a false claim: {claim}")


def check_documentation(reader: Callable[[str], str] = default_reader) -> None:
    for relative, markers in DOCUMENTATION_MARKERS.items():
        source = reader(relative)
        for marker in markers:
            require(marker in source, f"{relative} omits package truth: {marker}")


def check_workflow_results(environment: dict[str, str]) -> None:
    scopes = {}
    for name in ("CORE", "RUST", "PYTHON", "TS", "WORKFLOW"):
        key = f"{name}_SCOPE"
        value = environment.get(key)
        require(value in {"true", "false"}, f"{key} must be true or false")
        scopes[name] = value == "true"

    expectations = {
        "RUST_PAYLOAD_RESULT": scopes["CORE"] or scopes["RUST"] or scopes["WORKFLOW"],
        "PROJECTION_PARITY_RESULT": any(scopes.values()),
    }
    for key, required in expectations.items():
        result = environment.get(key)
        expected = "success" if required else "skipped"
        require(
            result == expected,
            f"{key} must be {expected} for the detected paths, received {result!r}",
        )


def check_repository() -> None:
    try:
        document = load_json_no_duplicates(
            PROOF_MAP.read_text(encoding="utf-8"), "P4 proof map"
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise QualityError(f"cannot load P4 projection/codegen map: {error}") from error
    require(isinstance(document, dict), "P4 proof map root must be an object")
    check_map(document)
    check_proof_sources(document)
    check_package_sources(document)
    workflow_source = default_reader(document["workflow"]["file"])
    check_workflow(document, workflow_source)
    check_abi_allowlist(ABI_ALLOWLIST.read_text(encoding="utf-8"))
    check_issue_truth(ISSUE.read_text(encoding="utf-8"))
    check_documentation()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check-repository", action="store_true")
    mode.add_argument("--validate-results", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.check_repository:
            check_repository()
        else:
            check_workflow_results(dict(os.environ))
    except (QualityError, OSError, UnicodeError) as error:
        print(f"P4 projection/codegen quality check failed: {error}", file=sys.stderr)
        return 1
    print("P4 projection/codegen quality check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
