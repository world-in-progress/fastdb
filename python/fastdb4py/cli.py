"""Command-line facade over Core-owned FastDB portable-payload artifacts."""

from __future__ import annotations

import argparse
import ctypes
import errno
import os
from pathlib import Path, PurePosixPath
import shutil
import sys
import tempfile
from typing import Sequence

from fastdb4py.payload import (
    Artifact,
    ArtifactKind,
    ArtifactSet,
    CodegenTarget,
    CompiledSpec,
    PayloadError,
)


TARGETS = {
    "cpp": CodegenTarget.CPP,
    "rust": CodegenTarget.RUST,
    "python": CodegenTarget.PYTHON,
    "typescript": CodegenTarget.TYPESCRIPT,
}


def _validated_artifacts(generated: ArtifactSet) -> tuple[Artifact, ...]:
    artifacts: list[Artifact] = []
    paths: set[str] = set()
    for index in range(generated.artifact_count()):
        artifact = generated.artifact(index)
        relative_path = artifact.relative_path
        if not isinstance(relative_path, str):
            raise ValueError("Core artifact relative path is not text")
        path = PurePosixPath(relative_path)
        valid = (
            bool(relative_path)
            and "\\" not in relative_path
            and not path.is_absolute()
            and ".." not in path.parts
            and path.as_posix() == relative_path
            and relative_path != "."
        )
        if not valid:
            raise ValueError(
                f"Core artifact has an unsafe relative path: {relative_path!r}"
            )
        if relative_path in paths:
            raise ValueError(
                f"Core artifact path is duplicated: {relative_path!r}"
            )
        if artifact.kind != ArtifactKind.SOURCE:
            raise ValueError(
                f"Core artifact has unsupported kind {artifact.kind!r}: "
                f"{relative_path!r}"
            )
        paths.add(relative_path)
        artifacts.append(
            Artifact(
                relative_path=relative_path,
                kind=artifact.kind,
                bytes=bytes(artifact.bytes),
                sha256=bytes(artifact.sha256),
            )
        )
    return tuple(artifacts)


def _raise_rename_error(error_number: int, output: Path) -> None:
    raise OSError(error_number, os.strerror(error_number), output)


def _publish_new_tree(staging: Path, output: Path) -> None:
    if sys.platform == "darwin":
        rename_exclusive = ctypes.CDLL(None, use_errno=True).renamex_np
        rename_exclusive.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        rename_exclusive.restype = ctypes.c_int
        rename_excl = 0x00000004
        if (
            rename_exclusive(
                os.fsencode(staging),
                os.fsencode(output),
                rename_excl,
            )
            != 0
        ):
            _raise_rename_error(ctypes.get_errno(), output)
        return

    if sys.platform.startswith("linux"):
        library = ctypes.CDLL(None, use_errno=True)
        try:
            rename_exclusive = library.renameat2
        except AttributeError:
            _raise_rename_error(errno.ENOTSUP, output)
        rename_exclusive.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        rename_exclusive.restype = ctypes.c_int
        at_fdcwd = -100
        rename_noreplace = 1
        if (
            rename_exclusive(
                at_fdcwd,
                os.fsencode(staging),
                at_fdcwd,
                os.fsencode(output),
                rename_noreplace,
            )
            != 0
        ):
            _raise_rename_error(ctypes.get_errno(), output)
        return

    if os.name == "nt":
        staging.rename(output)
        return

    _raise_rename_error(errno.ENOTSUP, output)


def _write_new_tree(output: Path, artifacts: tuple[Artifact, ...]) -> None:
    output = Path(output)
    if os.path.lexists(output):
        raise FileExistsError(f"output path already exists: {output}")

    staging = Path(
        tempfile.mkdtemp(prefix=f".{output.name}.", dir=os.fspath(output.parent))
    )
    published = False
    try:
        for artifact in artifacts:
            destination = staging / PurePosixPath(artifact.relative_path)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(artifact.bytes)
        _publish_new_tree(staging, output)
        published = True
    finally:
        if not published and staging.exists():
            shutil.rmtree(staging)


def _run_codegen(arguments: argparse.Namespace) -> None:
    source = Path(arguments.spec).read_bytes()
    with CompiledSpec.compile(source) as spec:
        with spec.generate(TARGETS[arguments.target]) as generated:
            artifacts = _validated_artifacts(generated)
    _write_new_tree(Path(arguments.output), artifacts)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="fdb",
        description="fastdb4py command-line tools",
    )
    subparsers = parser.add_subparsers(dest="command", metavar="<command>")
    subparsers.required = True

    codegen_parser = subparsers.add_parser(
        "codegen",
        help="Write exact Core-owned portable-payload artifacts",
    )
    codegen_parser.add_argument(
        "spec",
        metavar="SPEC.json",
        help="Portable-payload specification",
    )
    codegen_parser.add_argument(
        "--target",
        choices=tuple(TARGETS),
        required=True,
        help="Core artifact target",
    )
    codegen_parser.add_argument(
        "--output",
        required=True,
        help="New output directory",
    )
    codegen_parser.set_defaults(func=_run_codegen)
    return parser


def _print_core_error(error: PayloadError) -> None:
    print("fdb: core error:", file=sys.stderr)
    print(f"  code: {error.code}", file=sys.stderr)
    print(f"  symbol: {error.symbol}", file=sys.stderr)
    print(f"  path: {error.path}", file=sys.stderr)
    print(f"  message: {error.message}", file=sys.stderr)
    print(f"  details_json: {error.details_json}", file=sys.stderr)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        arguments.func(arguments)
    except PayloadError as error:
        _print_core_error(error)
        return 1
    except Exception as error:
        print(f"fdb: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
