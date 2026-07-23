#!/usr/bin/env python3
"""Verify exact portable-payload contents and debris exclusions in Python builds."""

from __future__ import annotations

import argparse
from email import policy
from email.parser import BytesParser
from pathlib import Path, PurePosixPath
import re
import tarfile
import zipfile


class CheckError(RuntimeError):
    pass


SDIST_REQUIRED = {
    "fastcarto/fastdb/include/fastdb_payload.h",
    "fastcarto/fastdb/include/fastdb_payload.hpp",
    "fastcarto/fastdb/src/payload/codegen/Artifact.cpp",
    "fastcarto/fastdb/src/payload/codegen/Artifact.hpp",
    "fastcarto/fastdb/src/payload/codegen/Generator.cpp",
    "fastcarto/fastdb/src/payload/codegen/Generator.hpp",
    "fastcarto/fastdb/src/payload/codegen/Identifier.cpp",
    "fastcarto/fastdb/src/payload/codegen/Identifier.hpp",
    "fastcarto/fastdb/src/payload/codegen/RenderCpp.cpp",
    "fastcarto/fastdb/src/payload/codegen/RenderPython.cpp",
    "fastcarto/fastdb/src/payload/codegen/Renderer.hpp",
    "fastcarto/fastdb/src/payload/codegen/RenderRust.cpp",
    "fastcarto/fastdb/src/payload/codegen/RenderTypeScript.cpp",
    "fastcarto/fastdb/src/payload/build/GraphAuthoring.cpp",
    "fastcarto/fastdb/src/payload/build/GraphAuthoring.hpp",
    "fastcarto/fastdb/src/payload/build/GraphEncoder.cpp",
    "fastcarto/fastdb/src/payload/build/GraphEncoder.hpp",
    "fastcarto/fastdb/src/payload/layout/GraphLayout.cpp",
    "fastcarto/fastdb/src/payload/layout/GraphLayout.hpp",
    "fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc",
    "fastcarto/fastdb/src/payload/spec/Manifest.cpp",
    "fastcarto/fastdb/src/payload/view/GraphMaterialize.cpp",
    "fastcarto/fastdb/src/payload/view/GraphOpen.cpp",
    "fastcarto/fastdb/src/payload/view/GraphOpen.hpp",
    "fastcarto/fastdb/src/payload/view/GraphView.cpp",
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
    "python/fastdb4py/payload/__init__.py",
    "python/fastdb4py/payload/_builder.py",
    "python/fastdb4py/payload/_codegen.py",
    "python/fastdb4py/payload/_error.py",
    "python/fastdb4py/payload/_ffi.py",
    "python/fastdb4py/payload/_runtime.py",
    "python/fastdb4py/payload/_spec.py",
}
WHEEL_REQUIRED = {
    "fastdb4py/payload/__init__.py",
    "fastdb4py/payload/_builder.py",
    "fastdb4py/payload/_codegen.py",
    "fastdb4py/payload/_error.py",
    "fastdb4py/payload/_ffi.py",
    "fastdb4py/payload/_runtime.py",
    "fastdb4py/payload/_spec.py",
}
WHEEL_FORBIDDEN = {
    "fastdb4py/call_db.py",
    "fastdb4py/schema.py",
    "fastdb4py/require.py",
    "fastdb4py/allocator.py",
    "fastdb4py/codegen/__init__.py",
    "fastdb4py/codegen/ts_gen.py",
}
SDIST_FORBIDDEN = {f"python/{path}" for path in WHEEL_FORBIDDEN}
FORBIDDEN_PARTS = {
    "__pycache__",
    ".pytest_cache",
    ".mypy_cache",
    ".ruff_cache",
    "runtime-corpus",
    "CMakeFiles",
}
SWIG_DIAGNOSTIC = re.compile(
    r"^\s*(?P<source>.+?):(?P<line>\d+): Warning (?P<code>\d+): "
    r"(?P<message>.*)$",
    re.MULTILINE,
)
NATIVE_ARTIFACT = re.compile(r"\.(?:so(?:\.\d+)*|dylib|dll|pyd)$")
NORMALIZED_NAME_SEPARATOR = re.compile(r"[-_.]+")
DIST_AUXILIARY_FILES = {".gitignore"}


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
            or (path.parts and path.parts[0] in {"build", "dist", "tests"})
            or path.name == ".DS_Store"
            or path.suffix in {".pyc", ".pyo"}
            or path.is_absolute()
            or ".." in path.parts
        ):
            bad.append(name)
    if bad:
        raise CheckError(f"{label} contains forbidden debris: {bad}")


def require_members(names: set[str], required: set[str], label: str) -> None:
    missing = sorted(required - names)
    if missing:
        raise CheckError(f"{label} is missing required portable-payload files: {missing}")


def reject_members(names: set[str], forbidden: set[str], label: str) -> None:
    present = sorted(names & forbidden)
    if present:
        raise CheckError(f"{label} contains removed Python authority: {present}")


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
    actual = [
        (
            Path(match.group("source").replace("\\", "/")).name,
            int(match.group("line")),
            int(match.group("code")),
            match.group("message"),
        )
        for match in SWIG_DIAGNOSTIC.finditer(build_log)
    ]
    if actual:
        raise CheckError(
            "Python package build must contain zero SWIG diagnostics; "
            f"found={actual}"
        )


def canonical_distribution_name(name: str) -> str:
    return NORMALIZED_NAME_SEPARATOR.sub("-", name).lower()


def metadata_identity(contents: bytes, label: str) -> tuple[str, str]:
    metadata = BytesParser(policy=policy.compat32).parsebytes(contents)
    names = metadata.get_all("Name", [])
    versions = metadata.get_all("Version", [])
    if len(names) != 1 or len(versions) != 1 or not names[0] or not versions[0]:
        raise CheckError(f"{label} must contain exactly one Name and Version")
    return names[0], versions[0]


def artifact_identity(
    sdist: Path, wheel: Path
) -> tuple[str, str, str, str]:
    sdist_suffix = ".tar.gz"
    if not sdist.name.endswith(sdist_suffix):
        raise CheckError(f"unsupported sdist filename: {sdist.name}")
    sdist_root = sdist.name[: -len(sdist_suffix)]
    sdist_name, separator, sdist_version = sdist_root.rpartition("-")
    if not separator or not sdist_name or not sdist_version:
        raise CheckError(f"sdist filename has no exact name/version: {sdist.name}")

    if wheel.suffix != ".whl":
        raise CheckError(f"unsupported wheel filename: {wheel.name}")
    wheel_parts = wheel.stem.split("-")
    if len(wheel_parts) not in {5, 6}:
        raise CheckError(f"wheel filename has invalid tag fields: {wheel.name}")
    wheel_name, wheel_version = wheel_parts[:2]
    if (
        canonical_distribution_name(sdist_name)
        != canonical_distribution_name(wheel_name)
        or sdist_version != wheel_version
    ):
        raise CheckError(
            "sdist/wheel filename identities differ: "
            f"{sdist_name}-{sdist_version} vs {wheel_name}-{wheel_version}"
        )
    return sdist_root, sdist_name, sdist_version, wheel_name


def exact_dist_artifacts(dist: Path) -> tuple[Path, Path]:
    entries = sorted(dist.iterdir(), key=lambda path: path.name)
    invalid = [
        path.name for path in entries if path.is_symlink() or not path.is_file()
    ]
    sdists = [path for path in entries if path.name.endswith(".tar.gz")]
    wheels = [path for path in entries if path.suffix == ".whl"]
    expected = set(sdists + wheels)
    unexpected = [
        path.name
        for path in entries
        if path not in expected and path.name not in DIST_AUXILIARY_FILES
    ]
    if invalid or len(sdists) != 1 or len(wheels) != 1 or unexpected:
        raise CheckError(
            "dist directory must contain exactly one regular sdist and wheel; "
            f"invalid={invalid}, unexpected={unexpected}, "
            f"sdists={len(sdists)}, wheels={len(wheels)}"
        )
    return sdists[0], wheels[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist-dir", required=True, type=Path)
    parser.add_argument("--build-log", required=True, type=Path)
    args = parser.parse_args()
    check_swig_diagnostics(args.build_log.read_text(encoding="utf-8"))
    dist = args.dist_dir.resolve(strict=True)
    sdist, wheel = exact_dist_artifacts(dist)
    root, filename_name, version, _ = artifact_identity(sdist, wheel)

    with tarfile.open(sdist, "r:gz") as archive:
        raw_names = archive.getnames()
        if len(raw_names) != len(set(raw_names)):
            raise CheckError("sdist contains duplicate member names")
        package_info = archive.extractfile(f"{root}/PKG-INFO")
        if package_info is None:
            raise CheckError("sdist is missing its root PKG-INFO")
        sdist_metadata = metadata_identity(package_info.read(), "sdist PKG-INFO")
    sdist_names = strip_sdist_root(raw_names, root)
    require_members(sdist_names, SDIST_REQUIRED, "sdist")
    reject_members(sdist_names, SDIST_FORBIDDEN, "sdist")
    reject_debris(sdist_names, "sdist")

    with zipfile.ZipFile(wheel) as archive:
        raw_wheel_names = archive.namelist()
        if len(raw_wheel_names) != len(set(raw_wheel_names)):
            raise CheckError("wheel contains duplicate member names")
        wheel_names = set(raw_wheel_names)
        metadata_paths = sorted(
            name
            for name in wheel_names
            if PurePosixPath(name).parts[-1:] == ("METADATA",)
            and len(PurePosixPath(name).parts) == 2
            and PurePosixPath(name).parts[0].endswith(".dist-info")
        )
        if len(metadata_paths) != 1:
            raise CheckError(
                "wheel must contain exactly one top-level dist-info/METADATA"
            )
        wheel_metadata = metadata_identity(
            archive.read(metadata_paths[0]), "wheel METADATA"
        )

    expected_identity = (canonical_distribution_name(filename_name), version)
    actual_identities = {
        "sdist": (canonical_distribution_name(sdist_metadata[0]), sdist_metadata[1]),
        "wheel": (canonical_distribution_name(wheel_metadata[0]), wheel_metadata[1]),
    }
    for label, identity in actual_identities.items():
        if identity != expected_identity:
            raise CheckError(
                f"{label} metadata identity {identity} differs from filename "
                f"identity {expected_identity}"
            )
    check_wheel_native_artifacts(wheel_names)
    require_members(wheel_names, WHEEL_REQUIRED, "wheel")
    reject_members(wheel_names, WHEEL_FORBIDDEN, "wheel")
    reject_debris(wheel_names, "wheel")

    print(
        "Python package diagnostics and inventory check passed: "
        f"{sdist.name}, {wheel.name}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckError, OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"Python package inventory check failed: {error}")
        raise SystemExit(1) from error
