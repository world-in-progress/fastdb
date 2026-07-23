#!/usr/bin/env python3
"""Verify and execute the packed fastdb4ts portable-payload subpath."""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import tempfile
from typing import Any


REQUIRED = {
    "README.md",
    "package.json",
    "dist/index.d.ts",
    "dist/index.js",
    "dist/payload/abi.d.ts",
    "dist/payload/abi.js",
    "dist/payload/builder.d.ts",
    "dist/payload/builder.js",
    "dist/payload/codegen.d.ts",
    "dist/payload/codegen.js",
    "dist/payload/error.d.ts",
    "dist/payload/error.js",
    "dist/payload/index.d.ts",
    "dist/payload/index.js",
    "dist/payload/runtime.d.ts",
    "dist/payload/runtime.js",
    "dist/payload/spec.d.ts",
    "dist/payload/spec.js",
    "dist/wasm-loader.d.ts",
    "dist/wasm-loader.js",
    "dist/wasm/fastdb4ts.d.ts",
    "dist/wasm/fastdb4ts.js",
    "dist/wasm/fastdb4ts.wasm",
}
REMOVED_SUBPATH = "call" + "-db"
REMOVED_BINDING_STEM = "Fastdb" + "Call" + "Db"
FORBIDDEN = {
    f"dist/{REMOVED_SUBPATH}.d.ts",
    f"dist/{REMOVED_SUBPATH}.js",
}
REMOVED_ROOT_DECLARATION_MARKERS = {
    REMOVED_SUBPATH,
    "decode" + REMOVED_BINDING_STEM,
    "decodeFastdbFeature",
    "encode" + REMOVED_BINDING_STEM,
    "encodeFastdbFeature",
    REMOVED_BINDING_STEM + "ArrayItem",
    REMOVED_BINDING_STEM + "ArrayView",
    REMOVED_BINDING_STEM + "Binding",
    REMOVED_BINDING_STEM + "ColumnView",
    REMOVED_BINDING_STEM + "FeatureDependency",
    REMOVED_BINDING_STEM + "ScalarField",
    REMOVED_BINDING_STEM + "Table",
    REMOVED_BINDING_STEM + "TableView",
    REMOVED_BINDING_STEM + "View",
    "FastdbFeatureCodecBinding",
    "view" + REMOVED_BINDING_STEM,
}
FORBIDDEN_PARTS = {
    "node_modules",
    "src",
    "tests",
    ".git",
    ".pytest_cache",
    "__pycache__",
}


class CheckError(RuntimeError):
    """The npm payload package is incomplete or unsafe to consume."""


def load_json_no_duplicates(source: str, label: str) -> Any:
    def object_from_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise CheckError(f"{label} contains duplicate JSON key {key!r}")
            result[key] = value
        return result

    return json.loads(source, object_pairs_hook=object_from_pairs)


def exact_package(package_dir: Path) -> Path:
    try:
        entries = sorted(package_dir.resolve(strict=True).iterdir())
    except (FileNotFoundError, NotADirectoryError) as error:
        raise CheckError(f"package directory is unavailable: {package_dir}") from error
    packages = [
        path
        for path in entries
        if path.is_file() and not path.is_symlink() and path.suffix == ".tgz"
    ]
    unexpected = [path.name for path in entries if path not in packages]
    if len(packages) != 1 or unexpected:
        raise CheckError(
            "package directory must contain exactly one npm tarball; "
            f"packages={len(packages)}, unexpected={unexpected}"
        )
    return packages[0]


def strip_root(names: list[str]) -> set[str]:
    if len(names) != len(set(names)):
        raise CheckError("npm tarball contains duplicate member names")
    prefix = "package/"
    outside = [
        name
        for name in names
        if name != "package" and not name.startswith(prefix)
    ]
    if outside:
        raise CheckError(f"npm tarball contains members outside package/: {outside}")
    relative_names = [
        name[len(prefix) :] for name in names if name.startswith(prefix)
    ]
    invalid = []
    canonical_names = []
    for name in relative_names:
        path = PurePosixPath(name)
        canonical = path.as_posix()
        if (
            not name
            or "\\" in name
            or path.is_absolute()
            or ".." in path.parts
            or canonical != name
        ):
            invalid.append(name)
        canonical_names.append(canonical)
    if invalid:
        raise CheckError(f"npm tarball contains non-canonical paths: {invalid}")
    if len(canonical_names) != len(set(canonical_names)):
        raise CheckError("npm tarball contains colliding normalized paths")
    return set(canonical_names)


def check_inventory(names: set[str]) -> None:
    missing = sorted(REQUIRED - names)
    if missing:
        raise CheckError(f"npm package is missing payload artifacts: {missing}")
    forbidden = sorted(FORBIDDEN & names)
    if forbidden:
        raise CheckError(f"npm package contains removed authority artifacts: {forbidden}")
    debris = []
    for name in sorted(names):
        path = PurePosixPath(name)
        if (
            path.is_absolute()
            or "\\" in name
            or ".." in path.parts
            or any(part in FORBIDDEN_PARTS for part in path.parts)
            or path.suffix in {".tsbuildinfo", ".pyc", ".pyo"}
            or path.name == ".DS_Store"
        ):
            debris.append(name)
    if debris:
        raise CheckError(f"npm package contains forbidden debris: {debris}")


def check_root_declarations(source: str) -> None:
    removed = sorted(
        marker for marker in REMOVED_ROOT_DECLARATION_MARKERS if marker in source
    )
    if removed:
        raise CheckError(
            "root declarations contain removed authority markers: "
            f"{removed}"
        )


def reject_special_members(members: list[tarfile.TarInfo]) -> None:
    special = [
        member.name
        for member in members
        if not (member.isfile() or member.isdir())
    ]
    if special:
        raise CheckError(f"npm tarball contains non-file members: {special}")


def check_package_json(document: Any) -> None:
    if not isinstance(document, dict):
        raise CheckError("package.json root must be an object")
    expected_root = {
        "types": "./dist/index.d.ts",
        "import": "./dist/index.js",
    }
    expected_payload = {
        "types": "./dist/payload/index.d.ts",
        "import": "./dist/payload/index.js",
    }
    expected_exports = {
        ".": expected_root,
        "./payload": expected_payload,
    }
    exports = document.get("exports")
    if not isinstance(exports, dict) or exports != expected_exports:
        raise CheckError("package.json must export the exact root and ./payload subpaths")
    files = document.get("files")
    if not isinstance(files, list) or "dist" not in files or "README.md" not in files:
        raise CheckError("package.json files must include dist and README.md")
    if document.get("type") != "module":
        raise CheckError("fastdb4ts payload package must remain an ES module")


def run_smoke(package: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="fastdb-ts-package-") as temporary:
        root = Path(temporary)
        with tarfile.open(package, "r:gz") as archive:
            members = archive.getmembers()
            names = [member.name for member in members]
            check_inventory(strip_root(names))
            reject_special_members(members)
            archive.extractall(root)
        node_modules = root / "node_modules"
        node_modules.mkdir()
        shutil.move(str(root / "package"), str(node_modules / "fastdb4ts"))
        smoke = root / "smoke.mjs"
        smoke.write_text(
            """import * as fastdb from 'fastdb4ts';
import {
  ArtifactKind,
  CodegenTarget,
  CompiledSpec,
  Profile,
  initPayload,
} from 'fastdb4ts/payload';

const removedBindingStem = 'Fastdb' + 'Call' + 'Db';
for (const name of [
  'encode' + removedBindingStem,
  'decode' + removedBindingStem,
  'view' + removedBindingStem,
  'encodeFastdbFeature',
  'decodeFastdbFeature',
]) {
  if (name in fastdb) throw new Error(`removed root export present: ${name}`);
}
for (const name of ['Feature', 'ORM', 'FastSerializer']) {
  if (typeof fastdb[name] !== 'function') {
    throw new Error(`retained root export missing: ${name}`);
  }
}

await initPayload();
const source = new TextEncoder().encode(
  '{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[],"components":[]}',
);
const spec = CompiledSpec.compile(source);
try {
  if (spec.profile() !== Profile.RecordV1) throw new Error('profile mismatch');
  if (spec.entryCount() !== 0 || spec.componentCount() !== 0) {
    throw new Error('empty spec indexes mismatch');
  }
  if (spec.sha256().byteLength !== 32) throw new Error('digest mismatch');
  const generated = spec.generate(CodegenTarget.TypeScript);
  try {
    if (generated.size() !== 1n) throw new Error('artifact count mismatch');
    const artifact = generated.artifact(0n);
    if (
      artifact.kind !== ArtifactKind.Source ||
      !artifact.relativePath.endsWith('.ts') ||
      artifact.bytes.byteLength === 0 ||
      artifact.sha256.byteLength !== 32
    ) {
      throw new Error('artifact mismatch');
    }
  } finally {
    generated.dispose();
  }
} finally {
  spec.dispose();
}
""",
            encoding="utf-8",
        )
        completed = subprocess.run(
            ["node", str(smoke)],
            cwd=root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="strict",
        )
        if completed.returncode != 0:
            raise CheckError(
                f"packed fastdb4ts/payload smoke failed with exit "
                f"{completed.returncode}:\n{completed.stdout}"
            )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        package = exact_package(arguments.package_dir)
        with tarfile.open(package, "r:gz") as archive:
            members = archive.getmembers()
            reject_special_members(members)
            names = strip_root([member.name for member in members])
            check_inventory(names)
            package_json = archive.extractfile("package/package.json")
            if package_json is None:
                raise CheckError("npm package is missing package.json contents")
            check_package_json(
                load_json_no_duplicates(
                    package_json.read().decode("utf-8"), "package.json"
                )
            )
            root_declarations = archive.extractfile("package/dist/index.d.ts")
            if root_declarations is None:
                raise CheckError("npm package is missing root declaration contents")
            check_root_declarations(
                root_declarations.read().decode("utf-8")
            )
        run_smoke(package)
    except (
        CheckError,
        OSError,
        UnicodeError,
        json.JSONDecodeError,
        tarfile.TarError,
    ) as error:
        print(f"TypeScript payload package check failed: {error}", file=sys.stderr)
        return 1
    print("TypeScript packed fastdb4ts/payload inventory and Wasm smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
