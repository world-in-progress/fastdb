#!/usr/bin/env python3
"""Verify exact portable-payload contents and debris exclusions in Python builds."""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path, PurePosixPath
import re
import tarfile
import tomllib
import zipfile


class CheckError(RuntimeError):
    pass


SDIST_REQUIRED = {
    "fastcarto/fastdb/include/fastdb_payload.h",
    "fastcarto/fastdb/include/fastdb_payload.hpp",
    "fastcarto/lib/double-conversion/UPSTREAM.md",
    "fastcarto/lib/double-conversion/LICENSE",
    "fastcarto/lib/double-conversion/double-conversion/double-conversion.h",
    "fastcarto/lib/picosha2/UPSTREAM.md",
    "fastcarto/lib/picosha2/LICENSE",
    "fastcarto/lib/picosha2/picosha2.h",
    "fastcarto/lib/yyjson/UPSTREAM.md",
    "fastcarto/lib/yyjson/LICENSE",
    "fastcarto/lib/yyjson/src/yyjson.c",
    "fastcarto/lib/yyjson/src/yyjson.h",
    "schemas/fastdb.payload.bin.v1.md",
    "schemas/fastdb.payload.manifest.v1.schema.json",
    "schemas/fastdb.payload.v1.schema.json",
    "schemas/fastdb.payload.v1.schema.sha256",
}
FORBIDDEN_PARTS = {
    "__pycache__",
    ".pytest_cache",
    ".mypy_cache",
    ".ruff_cache",
    "runtime-corpus",
    "CMakeFiles",
}
EXPECTED_SWIG_DIAGNOSTICS = (
    (582, 325),
    (588, 325),
    (595, 325),
    (622, 325),
    (631, 325),
    (637, 325),
    (206, 451),
)
EXPECTED_SWIG_MESSAGES = {
    582: "Nested struct not currently supported (TileBox ignored)",
    588: "Nested class not currently supported (HandleTileAction ignored)",
    595: "Nested struct not currently supported (TakeResult ignored)",
    622: "Nested struct not currently supported (TileDataHandle ignored)",
    631: "Nested struct not currently supported (TileDbBox ignored)",
    637: "Nested struct not currently supported (TakeResult ignored)",
    206: "Setting a const char * variable may leak memory.",
}
SWIG_DIAGNOSTIC = re.compile(
    r"^\s*(?P<source>.+?):(?P<line>\d+): Warning (?P<code>\d+): "
    r"(?P<message>.*)$",
    re.MULTILINE,
)
NATIVE_ARTIFACT = re.compile(r"\.(?:so(?:\.\d+)*|dylib|dll|pyd)$")


def strip_sdist_root(names: list[str], root: str) -> set[str]:
    prefix = f"{root}/"
    outside = sorted(name for name in names if name != root and not name.startswith(prefix))
    if outside:
        raise CheckError(
            f"sdist contains members outside its exact {root!r} root: {outside}"
        )
    return {name[len(prefix) :] for name in names if name.startswith(prefix)}


def reject_debris(names: set[str], label: str) -> None:
    bad = []
    for name in sorted(names):
        path = PurePosixPath(name)
        if (
            any(part in FORBIDDEN_PARTS for part in path.parts)
            or (path.parts and path.parts[0] in {"build", "dist"})
            or path.name == ".DS_Store"
            or path.suffix in {".pyc", ".pyo"}
            or path.is_absolute()
            or ".." in path.parts
        ):
            bad.append(name)
    if bad:
        raise CheckError(f"{label} contains forbidden debris: {bad}")


def check_wheel_native_artifacts(names: set[str]) -> None:
    native = {name for name in names if NATIVE_ARTIFACT.search(name)}
    python_extension = {
        name
        for name in native
        if PurePosixPath(name).name.startswith("_fastdb4py.")
    }
    native_core = {
        name
        for name in native
        if PurePosixPath(name).name.startswith("libfastdb.")
        or PurePosixPath(name).name == "fastdb.dll"
    }
    native_binding = {
        name
        for name in native
        if PurePosixPath(name).name.startswith("libfastdb4py.")
        or PurePosixPath(name).name == "fastdb4py.dll"
    }
    expected = python_extension | native_core | native_binding
    if (
        len(python_extension) != 1
        or len(native_core) != 1
        or len(native_binding) != 1
        or native != expected
    ):
        raise CheckError(
            "wheel must contain exactly one Python extension, one native FastDB "
            "library, and one native binding library; "
            f"found {sorted(native)}"
        )


def check_swig_diagnostics(build_log: str) -> None:
    actual = Counter(
        (
            Path(match.group("source").replace("\\", "/")).name,
            int(match.group("line")),
            int(match.group("code")),
            match.group("message"),
        )
        for match in SWIG_DIAGNOSTIC.finditer(build_log)
    )
    expected = Counter(
        ("fastdb.h", line, code, EXPECTED_SWIG_MESSAGES[line])
        for line, code in EXPECTED_SWIG_DIAGNOSTICS
    )
    if actual != expected:
        missing = list((expected - actual).elements())
        unexpected = list((actual - expected).elements())
        raise CheckError(
            "SWIG diagnostics differ from Issue 0003; "
            f"missing={missing}, unexpected={unexpected}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist-dir", required=True, type=Path)
    parser.add_argument("--build-log", required=True, type=Path)
    parser.add_argument(
        "--pyproject", type=Path, default=Path(__file__).resolve().parent.parent / "pyproject.toml"
    )
    args = parser.parse_args()
    check_swig_diagnostics(args.build_log.read_text(encoding="utf-8"))
    metadata = tomllib.loads(args.pyproject.read_text(encoding="utf-8"))["project"]
    normalized = metadata["name"].replace("-", "_")
    version = metadata["version"]
    dist = args.dist_dir.resolve(strict=True)
    sdists = sorted(dist.glob(f"{normalized}-{version}.tar.gz"))
    wheels = sorted(dist.glob(f"{normalized}-{version}-*.whl"))
    if len(sdists) != 1 or len(wheels) != 1:
        raise CheckError(
            f"expected one sdist and one wheel for {normalized}-{version}; "
            f"found {len(sdists)} and {len(wheels)}"
        )

    with tarfile.open(sdists[0], "r:gz") as archive:
        raw_names = archive.getnames()
    root = f"{normalized}-{version}"
    sdist_names = strip_sdist_root(raw_names, root)
    missing = sorted(SDIST_REQUIRED - sdist_names)
    if missing:
        raise CheckError(f"sdist is missing required portable-payload files: {missing}")
    reject_debris(sdist_names, "sdist")

    with zipfile.ZipFile(wheels[0]) as archive:
        wheel_names = set(archive.namelist())
    check_wheel_native_artifacts(wheel_names)
    reject_debris(wheel_names, "wheel")

    print(
        "Python package diagnostics and inventory check passed: "
        f"{sdists[0].name}, {wheels[0].name}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckError, OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"Python package inventory check failed: {error}")
        raise SystemExit(1) from error
