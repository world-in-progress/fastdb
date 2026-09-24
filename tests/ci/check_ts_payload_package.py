#!/usr/bin/env python3
"""Verify and execute the packed fastdb4ts portable-payload subpath."""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import tempfile
from typing import Any


PACKAGE_NAME = "fastdb4ts"
PACKAGE_VERSION = "0.2.0"
REQUIRED = {
    "README.md",
    "package.json",
    "dist/LICENSE",
    "dist/THIRD_PARTY_NOTICES.txt",
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
    if (
        document.get("name") != PACKAGE_NAME
        or document.get("version") != PACKAGE_VERSION
    ):
        raise CheckError(f"package.json must identify {PACKAGE_NAME}@{PACKAGE_VERSION}")
    if document.get("private") is not False:
        raise CheckError("fastdb4ts release package must set private to false")
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
        (root / "package.json").write_text(
            json.dumps({
                "name": "fastdb-package-consumer", "private": True,
                "type": "module",
            }),
            encoding="utf-8",
        )
        installed = subprocess.run(
            [
                "npm", "install", "--offline", "--ignore-scripts",
                "--no-audit", "--no-fund", str(package.resolve()),
            ],
            cwd=root, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="strict", timeout=120,
        )
        if installed.returncode != 0:
            raise CheckError(f"npm install of packed release failed:\n{installed.stdout}")
        installed_manifest = root / "node_modules/fastdb4ts/package.json"
        check_package_json(load_json_no_duplicates(
            installed_manifest.read_text(encoding="utf-8"),
            "installed package.json",
        ))
        smoke = root / "smoke.mjs"
        smoke.write_text(
            """import assert from 'node:assert/strict';
import * as fastdb from 'fastdb4ts';
import {
  ArtifactKind,
  BuildPolicy,
  Builder,
  CodegenTarget,
  CompiledSpec,
  OpenOptions,
  Payload,
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
  '{"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"value","cardinality":"one","type":{"kind":"u8","nullable":false}}],"components":[]}',
);
const spec = CompiledSpec.compile(source);
try {
  if (spec.profile() !== Profile.RecordV1) throw new Error('profile mismatch');
  if (spec.entryCount() !== 1 || spec.componentCount() !== 0) {
    throw new Error('spec indexes mismatch');
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
  const builder = Builder.create(spec);
  let plan;
  let payload;
  let opened;
  let entry;
  let view;
  let detached;
  try {
    plan = builder.entryBegin(0, 1n).valueU8(37).freeze();
    payload = plan.execute(BuildPolicy.AllowStaging).payload;
    opened = Payload.openCopy(spec, payload.binaryBytes(), new OpenOptions());
    payload.dispose();
    entry = opened.entryView(0);
    view = entry.at(0n);
    assert.equal(view.getU8(), 37);
    detached = view.materialize();
    opened.invalidate();
    assert.throws(() => view.kind(), (error) => error.symbol === 'VIEW_INVALIDATED');
    assert.equal(detached.getU8(), 37);
  } finally {
    detached?.dispose();
    view?.dispose();
    entry?.dispose();
    opened?.dispose();
    payload?.dispose();
    plan?.dispose();
    builder.dispose();
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
            timeout=120,
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
            for filename in ("LICENSE", "THIRD_PARTY_NOTICES.txt"):
                member = archive.extractfile(f"package/dist/{filename}")
                expected = Path(__file__).resolve().parents[2] / filename
                if member is None or member.read() != expected.read_bytes():
                    raise CheckError(f"npm package does not retain the original {filename}")
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
        subprocess.SubprocessError,
    ) as error:
        print(f"TypeScript payload package check failed: {error}", file=sys.stderr)
        return 1
    print("TypeScript packed fastdb4ts/payload inventory and Wasm smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
