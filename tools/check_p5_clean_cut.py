#!/usr/bin/env python3
"""Fail-closed repository gate for the FastDB P5 clean cut."""

from __future__ import annotations

import argparse
import ast
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from typing import Any
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parent.parent
POLICY_PATH = ROOT / "tests" / "ci" / "p5_clean_cut_policy.json"
SCHEMA = "fastdb.p5-clean-cut-policy.v1"
TOP_LEVEL_KEYS = (
    "schema",
    "native_abi_symbol_count",
    "python_version",
    "typescript_version",
    "removed_paths",
    "required_paths",
    "historical_allowlist",
    "literal_carrier_allowlist",
    "superseded_documents",
    "authority_scan_roots",
    "domain_scan_roots",
    "forbidden_authority_literals",
    "forbidden_domain_literals",
    "standalone_markers",
)

PYTHON_PORTABLE_MODULES = (
    "__init__.py",
    "_builder.py",
    "_codegen.py",
    "_error.py",
    "_ffi.py",
    "_runtime.py",
    "_spec.py",
)
TYPESCRIPT_PORTABLE_MODULES = (
    "abi.ts",
    "builder.ts",
    "codegen.ts",
    "error.ts",
    "index.ts",
    "runtime.ts",
    "spec.ts",
)
RUST_SAFE_MODULES = (
    "builder.rs",
    "codegen.rs",
    "lib.rs",
    "runtime.rs",
)
RUST_SYS_MODULES = ("lib.rs",)

PYTHON_PORTABLE_ROOT = "python/fastdb4py/payload"
TYPESCRIPT_PORTABLE_ROOT = "ts/fastdb4ts/src/payload"
RUST_SAFE_ROOT = "bindings/rust/fastdb/src"
RUST_SYS_ROOT = "bindings/rust/fastdb-sys/src"
RUST_BINDINGS_ROOT = "bindings/rust"
ABI_ALLOWLIST = "tests/abi/fastdb_payload_v1_symbols.txt"
WASM_EXPORT_GENERATOR = "tools/generate_payload_wasm_exports.py"
P4_PROOF_MAP = "tests/ci/p4_projection_codegen_map.json"
PYTHON_PACKAGE_CHECKER = "tools/check_python_package_inventory.py"
CLI_PATH = "python/fastdb4py/cli.py"
WORKFLOW_PATH = ".github/workflows/tests.yml"
ISSUE_0002 = (
    "docs/issues/0002-portable-payload-foundation-implementation-status.md"
)
ISSUE_0003 = "docs/issues/0003-legacy-swig-diagnostics.md"
ISSUE_INDEX = "docs/issues/README.md"
CURRENT_READINESS_MARKERS = {
    "README.md": "P5 local clean cut is complete at exact ABI-117.",
    "python/README.md": "P5 local clean cut is complete.",
    "fastcarto/README.md": (
        "P5 local clean cut is complete at exact ABI-117."
    ),
    "ts/fastdb4ts/README.md": "P5 local clean cut is complete.",
}
MARKDOWN_EXCLUDED_ROOTS = ("fastcarto/lib/",)

ABI_SYMBOL_PATTERN = re.compile(r"^fdb_payload_v1_[A-Za-z0-9_]+$")
INLINE_LINK_PATTERN = re.compile(r"!?\[([^\]]*)\]\(([^)\n]+)\)")
REFERENCE_LINK_PATTERN = re.compile(
    r"(?m)^[ \t]{0,3}\[[^\]]+\]:[ \t]*(\S+)"
)
HEADING_PATTERN = re.compile(r"^[ \t]{0,3}(#{1,6})[ \t]+(.+?)[ \t]*#*[ \t]*$")
EXPLICIT_ANCHOR_PATTERN = re.compile(
    r"""<(?:a|span)\b[^>]*\b(?:id|name)=["']([^"']+)["'][^>]*>""",
    re.IGNORECASE,
)
SCHEME_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


class CheckError(RuntimeError):
    """The clean-cut policy or repository cannot be checked safely."""


def _object_without_duplicates(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CheckError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def load_json_no_duplicates(source: str, label: str) -> Any:
    try:
        return json.loads(source, object_pairs_hook=_object_without_duplicates)
    except CheckError as error:
        raise CheckError(f"{label}: {error}") from error
    except json.JSONDecodeError as error:
        raise CheckError(
            f"{label} is not valid JSON at line {error.lineno}, "
            f"column {error.colno}: {error.msg}"
        ) from error


def load_policy(path: Path) -> dict[str, object]:
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise CheckError(f"cannot read UTF-8 clean-cut policy {path}: {error}") from error
    document = load_json_no_duplicates(source, os.fspath(path))
    if not isinstance(document, dict):
        raise CheckError("clean-cut policy must be a JSON object")
    return document


def _normalized_repository_path(
    value: object,
    label: str,
    *,
    allow_dot: bool = False,
) -> tuple[str | None, str | None]:
    if not isinstance(value, str) or not value:
        return None, f"{label} must be a non-empty string"
    path = PurePosixPath(value)
    wildcard = any(character in value for character in "*?[]")
    valid = (
        not wildcard
        and not path.is_absolute()
        and "\\" not in value
        and ".." not in path.parts
        and "//" not in value
        and path.as_posix() == value
        and (allow_dot or value != ".")
    )
    if not valid:
        return (
            None,
            f"{label} must be an exact normalized repository-relative path",
        )
    return value, None


def _resolve_inside(
    root: Path,
    relative: str,
    label: str,
    *,
    regular_file: bool | None,
) -> tuple[Path | None, str | None]:
    candidate = root / PurePosixPath(relative)
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as error:
        return None, f"{label} does not resolve: {relative}: {error}"
    try:
        resolved.relative_to(root)
    except ValueError:
        return None, f"{label} escapes the repository: {relative}"
    current = root
    for component in PurePosixPath(relative).parts:
        current /= component
        if current.is_symlink():
            return (
                None,
                f"{label} must not contain a symlink component: {relative}",
            )
    if regular_file is True and not resolved.is_file():
        return None, f"{label} must be a regular file: {relative}"
    if regular_file is False and not (resolved.is_file() or resolved.is_dir()):
        return None, f"{label} must be a file or directory: {relative}"
    return resolved, None


def _validate_string_list(
    policy: dict[str, object],
    key: str,
    violations: list[str],
    *,
    paths: bool = False,
    allow_dot: bool = False,
) -> list[str]:
    value = policy.get(key)
    if not isinstance(value, list) or not value:
        violations.append(f"policy {key} must be a non-empty array")
        return []
    if not all(isinstance(item, str) and item for item in value):
        violations.append(f"policy {key} must contain only non-empty strings")
        return []
    strings = [str(item) for item in value]
    if len(strings) != len(set(strings)):
        violations.append(f"policy {key} entries must be unique")
    if paths:
        for index, item in enumerate(strings):
            _, error = _normalized_repository_path(
                item,
                f"policy {key}[{index}]",
                allow_dot=allow_dot,
            )
            if error is not None:
                violations.append(error)
    else:
        for index, item in enumerate(strings):
            try:
                item.encode("ascii")
            except UnicodeEncodeError:
                violations.append(
                    f"policy {key}[{index}] must be an ASCII literal"
                )
    return strings


def validate_policy(root: Path, policy: dict[str, object]) -> list[str]:
    """Validate the exact policy shape without inspecting repository content."""

    violations: list[str] = []
    try:
        root = root.resolve(strict=True)
    except OSError as error:
        return [f"repository root does not resolve: {error}"]
    if not root.is_dir():
        return [f"repository root is not a directory: {root}"]

    if tuple(policy) != TOP_LEVEL_KEYS:
        violations.append("policy keys must be exact and ordered")
        return sorted(set(violations))
    if policy["schema"] != SCHEMA:
        violations.append(f"policy schema must be {SCHEMA!r}")
    if policy["native_abi_symbol_count"] != 117:
        violations.append("policy native ABI symbol count must remain exactly 117")
    if policy["python_version"] != "0.1.22":
        violations.append("policy Python package version must remain 0.1.22")
    if policy["typescript_version"] != "0.0.3":
        violations.append("policy TypeScript package version must remain 0.0.3")

    removed = _validate_string_list(
        policy, "removed_paths", violations, paths=True
    )
    required = _validate_string_list(
        policy, "required_paths", violations, paths=True
    )
    historical = _validate_string_list(
        policy, "historical_allowlist", violations, paths=True
    )
    carriers = _validate_string_list(
        policy, "literal_carrier_allowlist", violations, paths=True
    )
    superseded = _validate_string_list(
        policy, "superseded_documents", violations, paths=True
    )
    authority_roots = _validate_string_list(
        policy,
        "authority_scan_roots",
        violations,
        paths=True,
        allow_dot=True,
    )
    domain_roots = _validate_string_list(
        policy,
        "domain_scan_roots",
        violations,
        paths=True,
        allow_dot=True,
    )
    _validate_string_list(
        policy, "forbidden_authority_literals", violations
    )
    _validate_string_list(policy, "forbidden_domain_literals", violations)

    if len(carriers) != 1:
        violations.append(
            "policy literal_carrier_allowlist must contain exactly one file"
        )
    if set(carriers) & set(historical):
        violations.append(
            "policy literal carrier must not be classified as historical"
        )
    if not set(superseded) <= set(historical):
        violations.append(
            "policy superseded_documents must be an exact subset of "
            "historical_allowlist"
        )
    if set(removed) & set(required):
        violations.append("policy removed_paths and required_paths must be disjoint")

    for label, paths, regular_file in (
        ("required path", required, True),
        ("historical allowlist entry", historical, True),
        ("literal carrier", carriers, True),
        ("superseded document", superseded, True),
        ("authority scan root", authority_roots, False),
        ("domain scan root", domain_roots, False),
    ):
        for relative in paths:
            _, error = _resolve_inside(
                root,
                relative,
                label,
                regular_file=regular_file,
            )
            if error is not None:
                violations.append(error)

    for relative in removed:
        if os.path.lexists(root / PurePosixPath(relative)):
            violations.append(f"removed path is present: {relative}")

    markers = policy["standalone_markers"]
    if not isinstance(markers, dict) or not markers:
        violations.append("policy standalone_markers must be a non-empty object")
    else:
        for relative, marker in markers.items():
            normalized, error = _normalized_repository_path(
                relative,
                "policy standalone marker path",
            )
            if error is not None:
                violations.append(error)
                continue
            if not isinstance(marker, str) or not marker:
                violations.append(
                    f"standalone marker for {relative!r} must be non-empty text"
                )
                continue
            try:
                marker.encode("ascii")
            except UnicodeEncodeError:
                violations.append(
                    f"standalone marker for {relative!r} must be ASCII"
                )
            if normalized is not None:
                _, resolve_error = _resolve_inside(
                    root,
                    normalized,
                    "standalone marker file",
                    regular_file=True,
                )
                if resolve_error is not None:
                    violations.append(resolve_error)

    return sorted(set(violations))


def _git_inventory(root: Path) -> tuple[list[str], list[str]]:
    try:
        completed = subprocess.run(
            [
                "git",
                "-C",
                os.fspath(root),
                "ls-files",
                "-z",
                "--cached",
                "--others",
                "--exclude-standard",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        return [], [f"cannot execute Git inventory: {error}"]
    if completed.returncode != 0:
        diagnostic = completed.stderr.decode("utf-8", errors="replace").strip()
        return [], [
            f"Git inventory failed with exit {completed.returncode}: "
            f"{diagnostic or 'no diagnostic'}"
        ]

    violations: list[str] = []
    inventory: list[str] = []
    for raw in completed.stdout.split(b"\0"):
        if not raw:
            continue
        try:
            relative = raw.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            violations.append(
                "Git inventory contains a path that is not valid UTF-8"
            )
            continue
        normalized, error = _normalized_repository_path(
            relative,
            "Git inventory path",
        )
        if error is not None or normalized is None:
            violations.append(error or f"invalid Git inventory path: {relative!r}")
            continue
        candidate = root / PurePosixPath(normalized)
        try:
            resolved = candidate.resolve(strict=True)
        except OSError as resolve_error:
            violations.append(
                f"Git inventory path does not resolve: {relative}: {resolve_error}"
            )
            continue
        try:
            resolved.relative_to(root)
        except ValueError:
            violations.append(f"Git inventory path escapes repository: {relative}")
            continue
        if candidate.is_symlink():
            violations.append(f"Git inventory path must not be a symlink: {relative}")
            continue
        if not resolved.is_file():
            violations.append(
                f"Git inventory path must be a regular file: {relative}"
            )
            continue
        inventory.append(normalized)
    return sorted(set(inventory)), sorted(set(violations))


def _check_git_cleanliness(root: Path) -> list[str]:
    try:
        completed = subprocess.run(
            [
                "git",
                "-C",
                os.fspath(root),
                "status",
                "--porcelain=v1",
                "-z",
                "--untracked-files=all",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        return [f"cannot execute Git cleanliness check: {error}"]
    if completed.returncode != 0:
        diagnostic = completed.stderr.decode(
            "utf-8", errors="replace"
        ).strip()
        return [
            f"Git cleanliness check failed with exit {completed.returncode}: "
            f"{diagnostic or 'no diagnostic'}"
        ]
    if completed.stdout:
        return [
            "repository worktree/index must be clean, including "
            "non-ignored untracked files"
        ]
    return []


def _path_is_within(relative: str, root: str) -> bool:
    if root == ".":
        return True
    return relative == root or relative.startswith(root + "/")


def _read_bytes(root: Path, relative: str) -> tuple[bytes | None, str | None]:
    try:
        return (root / PurePosixPath(relative)).read_bytes(), None
    except OSError as error:
        return None, f"cannot read repository file {relative}: {error}"


def _read_text(root: Path, relative: str) -> tuple[str | None, str | None]:
    data, error = _read_bytes(root, relative)
    if error is not None or data is None:
        return None, error
    try:
        return data.decode("utf-8", errors="strict"), None
    except UnicodeDecodeError as decode_error:
        return None, f"repository text file is not UTF-8: {relative}: {decode_error}"


def _check_literal_boundaries(
    root: Path,
    inventory: list[str],
    policy: dict[str, object],
) -> list[str]:
    violations: list[str] = []
    historical = set(policy["historical_allowlist"])
    carriers = set(policy["literal_carrier_allowlist"])
    authority_roots = list(policy["authority_scan_roots"])
    domain_roots = list(policy["domain_scan_roots"])
    authority_literals = [
        literal.encode("ascii")
        for literal in policy["forbidden_authority_literals"]
    ]
    domain_literals = [
        literal.encode("ascii")
        for literal in policy["forbidden_domain_literals"]
    ]

    for relative in inventory:
        if relative in carriers:
            continue
        data, error = _read_bytes(root, relative)
        if error is not None or data is None:
            violations.append(error or f"cannot read {relative}")
            continue
        if (
            relative not in historical
            and any(_path_is_within(relative, value) for value in authority_roots)
        ):
            for literal in authority_literals:
                if literal in data:
                    violations.append(
                        f"{relative}: forbidden authority literal "
                        f"{literal.decode('ascii')!r}"
                    )
        if (
            relative not in historical
            and any(_path_is_within(relative, value) for value in domain_roots)
        ):
            for literal in domain_literals:
                if literal in data:
                    violations.append(
                        f"{relative}: forbidden downstream-domain literal "
                        f"{literal.decode('ascii')!r}"
                    )
    return violations


def _check_standalone_markers(
    root: Path,
    policy: dict[str, object],
) -> list[str]:
    violations: list[str] = []
    for relative, marker in policy["standalone_markers"].items():
        text, error = _read_text(root, relative)
        if error is not None or text is None:
            violations.append(error or f"cannot read standalone file {relative}")
        elif marker not in text:
            violations.append(
                f"{relative}: required standalone marker is missing: {marker!r}"
            )
    return violations


def _strip_fenced_blocks(source: str) -> str:
    output: list[str] = []
    fence: str | None = None
    for line in source.splitlines(keepends=True):
        match = re.match(r"^[ \t]{0,3}(`{3,}|~{3,})", line)
        if match is not None:
            marker = match.group(1)
            if fence is None:
                fence = marker[0]
            elif marker[0] == fence:
                fence = None
            output.append("\n" if line.endswith("\n") else "")
        elif fence is None:
            output.append(line)
        else:
            output.append("\n" if line.endswith("\n") else "")
    return "".join(output)


def _strip_inline_code(source: str) -> str:
    """Blank CommonMark code spans so code-shaped calls are not links."""

    output: list[str] = []
    index = 0
    while index < len(source):
        if source[index] != "`":
            output.append(source[index])
            index += 1
            continue
        end_of_run = index
        while end_of_run < len(source) and source[end_of_run] == "`":
            end_of_run += 1
        marker = source[index:end_of_run]
        closing = source.find(marker, end_of_run)
        if closing < 0:
            output.append(marker)
            index = end_of_run
            continue
        span_end = closing + len(marker)
        output.append(
            "".join(
                "\n" if character == "\n" else " "
                for character in source[index:span_end]
            )
        )
        index = span_end
    return "".join(output)


def _heading_slug(value: str) -> str:
    value = re.sub(r"<[^>]+>", "", value)
    value = re.sub(r"[^\w\- ]", "", value.casefold(), flags=re.UNICODE)
    value = re.sub(r"[ \t]+", "-", value.strip())
    return value


def _markdown_anchors(source: str) -> set[str]:
    stripped = _strip_fenced_blocks(source)
    anchors = {
        unquote(match.group(1)).casefold()
        for match in EXPLICIT_ANCHOR_PATTERN.finditer(stripped)
    }
    counts: dict[str, int] = {}
    for line in stripped.splitlines():
        match = HEADING_PATTERN.match(line)
        if match is None:
            continue
        base = _heading_slug(match.group(2))
        if not base:
            continue
        count = counts.get(base, 0)
        counts[base] = count + 1
        anchors.add(base if count == 0 else f"{base}-{count}")
    return anchors


def _link_target(value: str) -> str:
    value = value.strip()
    if value.startswith("<") and ">" in value:
        return value[1 : value.index(">")]
    if any(character.isspace() for character in value):
        return value.split(None, 1)[0]
    return value


def _markdown_links(source: str) -> list[tuple[str, str]]:
    stripped = _strip_inline_code(_strip_fenced_blocks(source))
    links = [
        (match.group(1), _link_target(match.group(2)))
        for match in INLINE_LINK_PATTERN.finditer(stripped)
    ]
    links.extend(
        ("", _link_target(match.group(1)))
        for match in REFERENCE_LINK_PATTERN.finditer(stripped)
    )
    return links


def _check_one_markdown_link(
    root: Path,
    source_relative: str,
    target: str,
) -> list[str]:
    if (
        not target
        or target.startswith("//")
        or SCHEME_PATTERN.match(target) is not None
    ):
        return []
    decoded = unquote(target)
    path_part, separator, anchor = decoded.partition("#")
    path_part = path_part.split("?", 1)[0]
    if path_part.startswith("/") or "\\" in path_part:
        return [
            f"{source_relative}: Markdown target must be relative: {target!r}"
        ]

    source_path = root / PurePosixPath(source_relative)
    destination = source_path if not path_part else source_path.parent / path_part
    try:
        resolved = destination.resolve(strict=True)
    except OSError:
        return [
            f"{source_relative}: Markdown target does not exist: {target!r}"
        ]
    try:
        resolved.relative_to(root)
    except ValueError:
        return [
            f"{source_relative}: Markdown target escapes repository: {target!r}"
        ]
    if destination.is_symlink():
        return [
            f"{source_relative}: Markdown target must not be a symlink: {target!r}"
        ]
    if resolved.is_dir():
        if separator and anchor:
            return [
                f"{source_relative}: Markdown anchor cannot target a directory: "
                f"{target!r}"
            ]
        return []
    if not resolved.is_file():
        return [
            f"{source_relative}: Markdown target is not a file: {target!r}"
        ]
    if not separator or not anchor or resolved.suffix.casefold() != ".md":
        return []

    try:
        destination_source = resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        return [
            f"{source_relative}: cannot inspect Markdown anchor {target!r}: {error}"
        ]
    normalized_anchor = unquote(anchor).casefold()
    if normalized_anchor not in _markdown_anchors(destination_source):
        return [
            f"{source_relative}: Markdown anchor does not exist: {target!r}"
        ]
    return []


def _check_markdown(
    root: Path,
    inventory: list[str],
    policy: dict[str, object],
) -> list[str]:
    violations: list[str] = []
    markdown = [
        relative
        for relative in inventory
        if relative.casefold().endswith(".md")
        and not any(
            relative.startswith(prefix)
            for prefix in MARKDOWN_EXCLUDED_ROOTS
        )
    ]
    sources: dict[str, str] = {}
    for relative in markdown:
        source, error = _read_text(root, relative)
        if error is not None or source is None:
            violations.append(error or f"cannot read Markdown file {relative}")
            continue
        sources[relative] = source
        for _, target in _markdown_links(source):
            violations.extend(_check_one_markdown_link(root, relative, target))

    for relative in policy["superseded_documents"]:
        source = sources.get(relative)
        if source is None:
            text, error = _read_text(root, relative)
            if error is not None or text is None:
                violations.append(
                    error or f"cannot read superseded document {relative}"
                )
                continue
            source = text
        visible_prefix = "\n".join(source.splitlines()[:24]).casefold()
        if "superseded" not in visible_prefix:
            violations.append(
                f"{relative}: superseded document lacks a visible "
                "superseded status near the top"
            )
        accepted_link = False
        for label, target in _markdown_links(source):
            candidate = f"{label} {target}".casefold()
            if "accepted" in candidate or "portable-payload" in candidate:
                accepted_link = True
                break
        if not accepted_link:
            violations.append(
                f"{relative}: superseded document lacks an accepted-design link"
            )
    return violations


def _project_version(source: str) -> tuple[str | None, str | None]:
    section: str | None = None
    versions: list[str] = []
    for line in source.splitlines():
        section_match = re.match(r"^[ \t]*\[([^\]]+)\][ \t]*$", line)
        if section_match is not None:
            section = section_match.group(1)
            continue
        if section != "project":
            continue
        version_match = re.match(
            r"""^[ \t]*version[ \t]*=[ \t]*["']([^"']+)["'][ \t]*(?:#.*)?$""",
            line,
        )
        if version_match is not None:
            versions.append(version_match.group(1))
    if len(versions) != 1:
        return None, "pyproject [project] must contain exactly one literal version"
    return versions[0], None


def _check_versions(
    root: Path,
    policy: dict[str, object],
) -> list[str]:
    violations: list[str] = []
    source, error = _read_text(root, "pyproject.toml")
    if error is not None or source is None:
        violations.append(error or "cannot read pyproject.toml")
    else:
        version, version_error = _project_version(source)
        if version_error is not None:
            violations.append(version_error)
        elif version != policy["python_version"]:
            violations.append(
                "Python package version does not match the clean-cut policy: "
                f"{version!r}"
            )

    package_source, error = _read_text(root, "ts/fastdb4ts/package.json")
    if error is not None or package_source is None:
        violations.append(error or "cannot read TypeScript package metadata")
    else:
        try:
            package = load_json_no_duplicates(
                package_source, "TypeScript package metadata"
            )
        except CheckError as load_error:
            violations.append(str(load_error))
        else:
            if (
                not isinstance(package, dict)
                or package.get("version") != policy["typescript_version"]
            ):
                violations.append(
                    "TypeScript package version does not match the clean-cut "
                    f"policy: {package.get('version') if isinstance(package, dict) else None!r}"
                )
    return violations


def _integer_assignment(source: str, name: str) -> int | None:
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return None
    values: list[int] = []
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Assign, ast.AnnAssign)):
            continue
        targets = node.targets if isinstance(node, ast.Assign) else [node.target]
        value = node.value
        if not isinstance(value, ast.Constant) or not isinstance(value.value, int):
            continue
        if any(
            isinstance(target, ast.Name) and target.id == name
            for target in targets
        ):
            values.append(value.value)
    return values[0] if len(values) == 1 else None


def _check_abi(
    root: Path,
    policy: dict[str, object],
) -> list[str]:
    violations: list[str] = []
    expected_count = int(policy["native_abi_symbol_count"])
    allowlist_source, error = _read_text(root, ABI_ALLOWLIST)
    symbols: list[str] = []
    if error is not None or allowlist_source is None:
        violations.append(error or f"cannot read {ABI_ALLOWLIST}")
    else:
        symbols = allowlist_source.splitlines()
        if (
            len(symbols) != expected_count
            or symbols != sorted(set(symbols))
            or any(ABI_SYMBOL_PATTERN.fullmatch(symbol) is None for symbol in symbols)
        ):
            violations.append(
                "native ABI allowlist must contain exactly "
                f"{expected_count} sorted unique valid symbols"
            )

    generator, error = _read_text(root, WASM_EXPORT_GENERATOR)
    if error is not None or generator is None:
        violations.append(error or f"cannot read {WASM_EXPORT_GENERATOR}")
    else:
        generator_count = _integer_assignment(
            generator, "EXPECTED_ABI_SYMBOLS"
        )
        if (
            generator_count != expected_count
            or Path(ABI_ALLOWLIST).name not in generator
        ):
            violations.append(
                "Wasm ABI export generator must derive the exact policy count "
                "from the reviewed native allowlist"
            )

    proof_source, error = _read_text(root, P4_PROOF_MAP)
    if error is not None or proof_source is None:
        violations.append(error or f"cannot read {P4_PROOF_MAP}")
    else:
        try:
            proof = load_json_no_duplicates(proof_source, "P4 proof map")
        except CheckError as load_error:
            violations.append(str(load_error))
        else:
            abi = proof.get("abi") if isinstance(proof, dict) else None
            if (
                not isinstance(abi, dict)
                or abi.get("symbol_count") != expected_count
                or abi.get("allowlist") != ABI_ALLOWLIST
            ):
                violations.append(
                    "P4 proof-map ABI must match the exact reviewed native "
                    "allowlist and policy count"
                )
    return violations


def _direct_module_inventory(
    inventory: list[str],
    root: str,
) -> set[str]:
    prefix = root + "/"
    return {
        relative[len(prefix) :]
        for relative in inventory
        if relative.startswith(prefix)
    }


def _check_binding_authority(
    root: Path,
    inventory: list[str],
) -> list[str]:
    violations: list[str] = []
    actual_python = _direct_module_inventory(inventory, PYTHON_PORTABLE_ROOT)
    expected_python = set(PYTHON_PORTABLE_MODULES)
    if actual_python != expected_python:
        violations.append(
            "Python portable module inventory must remain exact; "
            f"missing={sorted(expected_python - actual_python)!r}, "
            f"unexpected={sorted(actual_python - expected_python)!r}"
        )
    actual_typescript = _direct_module_inventory(
        inventory, TYPESCRIPT_PORTABLE_ROOT
    )
    expected_typescript = set(TYPESCRIPT_PORTABLE_MODULES)
    if actual_typescript != expected_typescript:
        violations.append(
            "TypeScript portable module inventory must remain exact; "
            f"missing={sorted(expected_typescript - actual_typescript)!r}, "
            f"unexpected={sorted(actual_typescript - expected_typescript)!r}"
        )
    actual_rust_safe = _direct_module_inventory(inventory, RUST_SAFE_ROOT)
    expected_rust_safe = set(RUST_SAFE_MODULES)
    if actual_rust_safe != expected_rust_safe:
        violations.append(
            "Rust safe portable module inventory must remain exact; "
            f"missing={sorted(expected_rust_safe - actual_rust_safe)!r}, "
            f"unexpected={sorted(actual_rust_safe - expected_rust_safe)!r}"
        )
    actual_rust_sys = _direct_module_inventory(inventory, RUST_SYS_ROOT)
    expected_rust_sys = set(RUST_SYS_MODULES)
    if actual_rust_sys != expected_rust_sys:
        violations.append(
            "Rust raw portable module inventory must remain exact; "
            f"missing={sorted(expected_rust_sys - actual_rust_sys)!r}, "
            f"unexpected={sorted(actual_rust_sys - expected_rust_sys)!r}"
        )

    python_patterns = (
        re.compile(rb"\bimport[ \t]+hashlib\b"),
        re.compile(rb"\bfrom[ \t]+hashlib\b"),
        re.compile(rb"\bjson[ \t]*\.[ \t]*(?:loads|dumps)[ \t]*\("),
        re.compile(rb"\borjson[ \t]*\.[ \t]*(?:loads|dumps)[ \t]*\("),
        re.compile(
            rb"\b(?:def|class)[ \t]+(?:"
            rb"[A-Za-z0-9_]*(?:render|renderer)[A-Za-z0-9_]*|"
            rb"(?:emit|generate)_(?:source|cpp|rust|python|typescript)"
            rb")\b",
            re.IGNORECASE,
        ),
        re.compile(rb"\b(?:jinja2|mako)\b"),
        re.compile(rb"\bfrom[ \t]+string[ \t]+import[ \t]+Template\b"),
    )
    typescript_patterns = (
        re.compile(rb"\bJSON[ \t]*\.[ \t]*(?:parse|stringify)[ \t]*\("),
        re.compile(rb"\bcreateHash[ \t]*\("),
        re.compile(rb"\bcrypto[ \t]*\.[ \t]*subtle\b"),
        re.compile(rb"\bfrom[ \t]+[\"'](?:node:)?crypto[\"']"),
        re.compile(
            rb"\b(?:function|class|const|let)[ \t]+(?:"
            rb"[A-Za-z0-9_$]*(?:render|renderer)[A-Za-z0-9_$]*|"
            rb"(?:emit|generate)(?:Source|Cpp|Rust|Python|TypeScript)"
            rb")\b",
            re.IGNORECASE,
        ),
        re.compile(rb"\b(?:handlebars|mustache|ejs)\b", re.IGNORECASE),
    )
    rust_patterns = (
        re.compile(
            rb"\b(?:serde_json|sha2|jsonschema|minijinja|tera)\b",
            re.IGNORECASE,
        ),
        re.compile(
            rb"\b(?:fn|struct|enum)[ \t]+(?:"
            rb"[A-Za-z0-9_]*(?:render|renderer)[A-Za-z0-9_]*|"
            rb"(?:emit|generate)_(?:source|cpp|rust|python|typescript)"
            rb")\b",
            re.IGNORECASE,
        ),
    )
    for relative in sorted(
        path
        for path in inventory
        if path.startswith(PYTHON_PORTABLE_ROOT + "/")
    ):
        data, error = _read_bytes(root, relative)
        if error is not None or data is None:
            violations.append(error or f"cannot read {relative}")
        elif any(pattern.search(data) for pattern in python_patterns):
            violations.append(
                f"{relative}: binding-side semantic parser or digest "
                "or renderer implementation is forbidden"
            )
    for relative in sorted(
        path
        for path in inventory
        if path.startswith(TYPESCRIPT_PORTABLE_ROOT + "/")
    ):
        data, error = _read_bytes(root, relative)
        if error is not None or data is None:
            violations.append(error or f"cannot read {relative}")
        elif any(pattern.search(data) for pattern in typescript_patterns):
            violations.append(
                f"{relative}: binding-side semantic parser or digest "
                "or renderer implementation is forbidden"
            )
    for relative in sorted(
        path
        for path in inventory
        if path.startswith(RUST_BINDINGS_ROOT + "/")
    ):
        data, error = _read_bytes(root, relative)
        if error is not None or data is None:
            violations.append(error or f"cannot read {relative}")
        elif any(pattern.search(data) for pattern in rust_patterns):
            violations.append(
                f"{relative}: binding-side semantic parser or digest "
                "or renderer implementation is forbidden"
            )
    return violations


def _check_cli(root: Path) -> list[str]:
    source, error = _read_text(root, CLI_PATH)
    if error is not None or source is None:
        return [error or f"cannot read {CLI_PATH}"]
    violations: list[str] = []
    required = (
        "from fastdb4py.payload import",
        "CompiledSpec.compile",
        "spec.generate",
        "_validated_artifacts",
        "_write_new_tree",
        'open("xb")',
    )
    for marker in required:
        if marker not in source:
            violations.append(f"{CLI_PATH}: CLI is missing required marker {marker!r}")
    forbidden = (
        "fastdb4py.codegen",
        "fastdb4py.schema",
        "import json",
        "from json",
    )
    for marker in forbidden:
        if marker in source:
            violations.append(
                f"{CLI_PATH}: CLI must remain a Core-only artifact facade; "
                f"forbidden import or generator marker {marker!r}"
            )
    if 'open("wb")' in source or "write_text(" in source:
        violations.append(
            f"{CLI_PATH}: CLI publication must use exclusive new-file writes"
        )
    return violations


def _check_swig_boundary(
    root: Path,
    inventory: list[str],
    policy: dict[str, object],
) -> list[str]:
    violations: list[str] = []
    source, error = _read_text(root, PYTHON_PACKAGE_CHECKER)
    if error is not None or source is None:
        return [error or f"cannot read {PYTHON_PACKAGE_CHECKER}"]
    allowance_markers = (
        "SWIG_" + "ALLOWLIST",
        "ALLOWED_" + "SWIG",
        "EXPECTED_" + "SWIG_DIAGNOSTICS",
        "IGNORED_" + "SWIG",
    )
    for marker in allowance_markers:
        if marker.casefold() in source.casefold():
            violations.append(
                f"{PYTHON_PACKAGE_CHECKER}: SWIG diagnostic allowance "
                f"is forbidden: {marker!r}"
            )
    if "check_swig_diagnostics" not in source:
        violations.append(
            f"{PYTHON_PACKAGE_CHECKER}: zero-diagnostic SWIG check is missing"
        )

    historical_diagnostic_literals = (
        "Warning " + "325:",
        "Warning " + "451:",
    )
    excluded = set(policy["literal_carrier_allowlist"])
    for relative in inventory:
        if relative in excluded:
            continue
        data, read_error = _read_bytes(root, relative)
        if read_error is not None or data is None:
            continue
        for literal in historical_diagnostic_literals:
            if literal.encode("ascii") in data:
                violations.append(
                    f"{relative}: historical SWIG diagnostic literal must be "
                    "constructed from reviewed fragments"
                )
    return violations


def _check_issues(root: Path) -> list[str]:
    violations: list[str] = []
    issue2, error = _read_text(root, ISSUE_0002)
    if error is not None or issue2 is None:
        violations.append(error or f"cannot read {ISSUE_0002}")
    else:
        issue2_plain = issue2.replace("*", "")
        if re.search(
            r"(?im)^[ \t]*(?:-[ \t]*)?Status:[ \t]*Open[ \t]*$",
            issue2_plain,
        ) is None:
            violations.append("Issue 0002 must remain Open")
        required = (
            "#### P5 Task 6 local evidence",
            "P5 Task 7",
            "pending",
            "Hosted",
            "version",
            "push",
            "tag",
            "publication",
            "release",
            "C-" + "Two",
        )
        for marker in required:
            if marker.casefold() not in issue2.casefold():
                violations.append(
                    f"Issue 0002 is missing required Task 6 truth marker "
                    f"{marker!r}"
                )

    issue3, error = _read_text(root, ISSUE_0003)
    if error is not None or issue3 is None:
        violations.append(error or f"cannot read {ISSUE_0003}")
    else:
        issue3_plain = issue3.replace("*", "")
        statuses = re.findall(
            r"(?im)^[ \t]*(?:-[ \t]*)?Status:[ \t]*(Open|Closed)[ \t]*$",
            issue3_plain,
        )
        required: tuple[str, ...]
        if statuses == ["Closed"]:
            required = (
                "P5 Task 4",
                "zero matched SWIG diagnostics",
                "Hosted",
                "pending",
            )
        elif statuses == ["Open"]:
            required = (
                "Hosted",
                "pending",
            )
            if re.search(
                r"(?is)(?:"
                r"pending[^.\n]{0,120}clean[- ]package evidence|"
                r"clean[- ]package evidence[^.\n]{0,120}pending"
                r")",
                issue3,
            ) is None:
                violations.append(
                    "Issue 0003 Open state must explicitly mark clean-package "
                    "evidence pending"
                )
        else:
            required = ()
            violations.append(
                "Issue 0003 must have exactly one truthful Open or Closed state"
            )
        for marker in required:
            if marker.casefold() not in issue3.casefold():
                violations.append(
                    f"Issue 0003 is missing required {statuses[0].lower()} "
                    f"evidence marker {marker!r}"
                )

    issue_index, error = _read_text(root, ISSUE_INDEX)
    if error is not None or issue_index is None:
        violations.append(error or f"cannot read {ISSUE_INDEX}")
    else:
        index_markers = (
            "| [0002](0002-portable-payload-foundation-implementation-status.md) "
            "| Open (P1-P5 locally frozen; hosted/release/C-"
            "Two evidence pending) |",
            "| [0003](0003-legacy-swig-diagnostics.md) | Closed |",
        )
        for marker in index_markers:
            if marker not in issue_index:
                violations.append(
                    f"{ISSUE_INDEX}: Issue index is missing exact current "
                    f"status {marker!r}"
                )
    return violations


def _check_current_readiness_docs(root: Path) -> list[str]:
    violations: list[str] = []
    for relative, marker in CURRENT_READINESS_MARKERS.items():
        source, error = _read_text(root, relative)
        if error is not None or source is None:
            violations.append(error or f"cannot read {relative}")
        elif marker not in source:
            violations.append(
                f"{relative}: current readiness marker is missing: {marker!r}"
            )
    return violations


def _workflow_job_body(source: str, name: str) -> str | None:
    lines = source.splitlines()
    starts = [
        index
        for index, line in enumerate(lines)
        if re.fullmatch(rf"  {re.escape(name)}:[ \t]*", line) is not None
    ]
    if len(starts) != 1:
        return None
    start = starts[0]
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if re.fullmatch(r"  [A-Za-z0-9_-]+:[ \t]*", lines[index]) is not None:
            end = index
            break
    return "\n".join(lines[start + 1 : end])


def _check_workflow(root: Path) -> list[str]:
    source, error = _read_text(root, WORKFLOW_PATH)
    if error is not None or source is None:
        return [error or f"cannot read {WORKFLOW_PATH}"]
    violations: list[str] = []
    body = _workflow_job_body(source, "clean_cut")
    if body is None:
        return [
            f"{WORKFLOW_PATH}: workflow must contain exactly one clean_cut job"
        ]
    if re.search(
        r"(?m)^[ \t]+(?:if|needs|continue-on-error):",
        body,
    ) is not None:
        violations.append(
            f"{WORKFLOW_PATH}: clean_cut must be an unconditional standalone job"
        )
    required_commands = (
        "python3 tests/ci/test_check_p5_clean_cut.py",
        "python3 tools/check_p5_clean_cut.py --check",
    )
    for command in required_commands:
        if re.search(
            rf"(?m)^[ \t]+(?:-[ \t]+run:[ \t]+)?"
            rf"{re.escape(command)}[ \t]*$",
            body,
        ) is None:
            violations.append(
                f"{WORKFLOW_PATH}: clean_cut job is missing command "
                f"{command!r}"
            )
    run_starts = [
        index
        for index, line in enumerate(body.splitlines())
        if re.fullmatch(r"        run:[ \t]*\|[ \t]*", line) is not None
    ]
    script_lines: list[str] = []
    if len(run_starts) == 1:
        lines = body.splitlines()
        for line in lines[run_starts[0] + 1 :]:
            if line.strip() and len(line) - len(line.lstrip()) <= 8:
                break
            if line.strip():
                script_lines.append(line.strip())
    if len(run_starts) != 1 or tuple(script_lines) != required_commands:
        violations.append(
            f"{WORKFLOW_PATH}: clean_cut must use the exact fail-fast script"
        )
    shells = [
        line.strip()
        for line in body.splitlines()
        if re.match(r"^[ \t]+shell:", line) is not None
    ]
    if shells != ["shell: bash"]:
        violations.append(
            f"{WORKFLOW_PATH}: clean_cut must use the exact fail-fast shell"
        )
    if "uses: actions/checkout@" not in body:
        violations.append(
            f"{WORKFLOW_PATH}: clean_cut job must check out the repository"
        )
    return violations


def check_repository(
    root: Path,
    policy: dict[str, object],
) -> list[str]:
    """Return stable sorted violations for the policy-governed repository."""

    try:
        resolved_root = root.resolve(strict=True)
    except OSError as error:
        return [f"repository root does not resolve: {error}"]
    policy_violations = validate_policy(resolved_root, policy)
    if policy_violations:
        return policy_violations

    inventory, inventory_violations = _git_inventory(resolved_root)
    violations = list(inventory_violations)
    violations.extend(_check_git_cleanliness(resolved_root))
    inventory_set = set(inventory)
    for relative in policy["required_paths"]:
        if relative not in inventory_set:
            violations.append(
                f"required path is outside the governed Git inventory: {relative}"
            )
    for relative in policy["literal_carrier_allowlist"]:
        if relative not in inventory_set:
            violations.append(
                f"literal carrier is outside the governed Git inventory: {relative}"
            )

    violations.extend(
        _check_literal_boundaries(resolved_root, inventory, policy)
    )
    violations.extend(_check_standalone_markers(resolved_root, policy))
    violations.extend(_check_markdown(resolved_root, inventory, policy))
    violations.extend(_check_versions(resolved_root, policy))
    violations.extend(_check_abi(resolved_root, policy))
    violations.extend(
        _check_binding_authority(resolved_root, inventory)
    )
    violations.extend(_check_cli(resolved_root))
    violations.extend(
        _check_swig_boundary(resolved_root, inventory, policy)
    )
    violations.extend(_check_issues(resolved_root))
    violations.extend(_check_current_readiness_docs(resolved_root))
    violations.extend(_check_workflow(resolved_root))
    return sorted(set(violations))


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="check the repository and fail on any violation",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=ROOT,
        help="repository root (defaults to this checkout)",
    )
    parser.add_argument(
        "--policy",
        type=Path,
        help="policy path (defaults beneath the selected repository root)",
    )
    arguments = parser.parse_args()
    if not arguments.check:
        parser.error("--check is required")
    return arguments


def main() -> int:
    arguments = parse_arguments()
    try:
        root = arguments.root.resolve(strict=True)
        policy_path = (
            arguments.policy
            if arguments.policy is not None
            else root / "tests" / "ci" / "p5_clean_cut_policy.json"
        )
        policy = load_policy(policy_path)
        violations = check_repository(root, policy)
    except CheckError as error:
        print(f"P5 clean-cut check failed: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"P5 clean-cut check failed: {error}", file=sys.stderr)
        return 1

    if violations:
        print(
            f"P5 clean-cut check failed with {len(violations)} violation(s):",
            file=sys.stderr,
        )
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        return 1
    print("P5 clean-cut check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
