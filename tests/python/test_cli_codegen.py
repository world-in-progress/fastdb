from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys

import pytest

from fastdb4py import cli
from fastdb4py.payload import (
    Artifact,
    ArtifactKind,
    CodegenTarget,
    CompiledSpec,
)


SPEC = (
    b'{"schema":"fastdb.payload.v1","profile":"record.v1",'
    b'"entries":[],"components":[]}'
)
TARGETS = {
    "cpp": CodegenTarget.CPP,
    "rust": CodegenTarget.RUST,
    "python": CodegenTarget.PYTHON,
    "typescript": CodegenTarget.TYPESCRIPT,
}


class _ArtifactSet:
    """Complete test double for the two ArtifactSet operations the CLI consumes."""

    def __init__(self, artifacts: tuple[Artifact, ...]) -> None:
        self._artifacts = artifacts

    def artifact_count(self) -> int:
        return len(self._artifacts)

    def artifact(self, index: int) -> Artifact:
        return self._artifacts[index]


def _artifact(relative_path: str, *, kind: object = ArtifactKind.SOURCE) -> Artifact:
    content = f"artifact:{relative_path}".encode()
    return Artifact(
        relative_path=relative_path,
        kind=kind,  # type: ignore[arg-type]
        bytes=content,
        sha256=hashlib.sha256(content).digest(),
    )


def _run_codegen(
    spec_path: Path, target: str, output: Path
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            "-m",
            "fastdb4py.cli",
            "codegen",
            os.fspath(spec_path),
            "--target",
            target,
            "--output",
            os.fspath(output),
        ],
        check=False,
        capture_output=True,
        text=True,
    )


@pytest.mark.parametrize(("target_name", "target"), TARGETS.items())
def test_codegen_cli_emits_the_exact_core_artifacts(
    tmp_path: Path, target_name: str, target: CodegenTarget
) -> None:
    spec_path = tmp_path / "spec.json"
    output = tmp_path / "generated"
    spec_path.write_bytes(SPEC)

    completed = _run_codegen(spec_path, target_name, output)

    assert completed.returncode == 0, completed.stderr
    with CompiledSpec.compile(SPEC) as spec:
        with spec.generate(target) as generated:
            expected = tuple(
                generated.artifact(index)
                for index in range(generated.artifact_count())
            )
    actual = {
        path.relative_to(output).as_posix(): path.read_bytes()
        for path in output.rglob("*")
        if path.is_file()
    }
    assert actual == {
        artifact.relative_path: artifact.bytes for artifact in expected
    }


def test_codegen_target_map_is_the_direct_core_target_map() -> None:
    assert cli.TARGETS == TARGETS


@pytest.mark.parametrize(
    "relative_path",
    [
        "",
        ".",
        "/absolute.py",
        r"nested\windows.py",
        "../escape.py",
        "nested/../escape.py",
        "./noncanonical.py",
        "nested//noncanonical.py",
        "C:/windows-absolute.py",
        "C:windows-drive-relative.py",
        "nul\x00byte.py",
    ],
)
def test_validated_artifacts_rejects_unsafe_paths(relative_path: str) -> None:
    generated = _ArtifactSet((_artifact(relative_path),))

    with pytest.raises(ValueError):
        cli._validated_artifacts(generated)


def test_validated_artifacts_rejects_duplicate_paths() -> None:
    generated = _ArtifactSet((_artifact("same.py"), _artifact("same.py")))

    with pytest.raises(ValueError):
        cli._validated_artifacts(generated)


def test_validated_artifacts_rejects_file_directory_prefix_conflicts() -> None:
    generated = _ArtifactSet(
        (_artifact("collision"), _artifact("collision/nested.py"))
    )

    with pytest.raises(ValueError):
        cli._validated_artifacts(generated)


def test_validated_artifacts_rejects_non_source_kinds() -> None:
    generated = _ArtifactSet((_artifact("generated.py", kind=999),))

    with pytest.raises(ValueError):
        cli._validated_artifacts(generated)


@pytest.mark.parametrize("existing_kind", ["file", "directory", "broken-symlink"])
def test_write_new_tree_rejects_every_existing_output_root(
    tmp_path: Path, existing_kind: str
) -> None:
    output = tmp_path / "generated"
    if existing_kind == "file":
        output.write_bytes(b"caller-owned")
    elif existing_kind == "directory":
        output.mkdir()
        (output / "caller-owned").write_bytes(b"keep")
    else:
        output.symlink_to(tmp_path / "missing-target", target_is_directory=True)

    with pytest.raises(FileExistsError):
        cli._write_new_tree(output, (_artifact("generated.py"),))

    assert os.path.lexists(output)
    if existing_kind == "file":
        assert output.read_bytes() == b"caller-owned"
    elif existing_kind == "directory":
        assert (output / "caller-owned").read_bytes() == b"keep"
    else:
        assert output.is_symlink()


def test_write_new_tree_removes_its_staging_tree_after_a_real_write_failure(
    tmp_path: Path,
) -> None:
    output = tmp_path / "generated"
    artifacts = (_artifact("collision"), _artifact("collision/nested.py"))
    before = tuple(tmp_path.iterdir())

    with pytest.raises(OSError):
        cli._write_new_tree(output, artifacts)

    assert tuple(tmp_path.iterdir()) == before
    assert not os.path.lexists(output)


def test_write_new_tree_never_collapses_distinct_paths_on_a_casefolding_fs(
    tmp_path: Path,
) -> None:
    probe = tmp_path / "CaseProbe"
    probe.write_bytes(b"probe")
    casefolding = (tmp_path / "caseprobe").exists()
    probe.unlink()
    if not casefolding:
        pytest.skip("temporary filesystem is case-sensitive")

    output = tmp_path / "generated"
    artifacts = (_artifact("Case.py"), _artifact("case.py"))
    before = tuple(tmp_path.iterdir())

    with pytest.raises(FileExistsError):
        cli._write_new_tree(output, artifacts)

    assert tuple(tmp_path.iterdir()) == before
    assert not os.path.lexists(output)


def test_exclusive_tree_publish_never_replaces_an_existing_empty_directory(
    tmp_path: Path,
) -> None:
    staging = tmp_path / "staging"
    staging.mkdir()
    (staging / "artifact.py").write_bytes(b"generated")
    output = tmp_path / "generated"
    output.mkdir()
    output_identity = output.stat().st_ino

    with pytest.raises(FileExistsError):
        cli._publish_new_tree(staging, output)

    assert output.stat().st_ino == output_identity
    assert tuple(output.iterdir()) == ()
    assert (staging / "artifact.py").read_bytes() == b"generated"


def test_codegen_cli_preserves_every_core_error_field(tmp_path: Path) -> None:
    spec_path = tmp_path / "invalid.json"
    spec_path.write_bytes(b"{}")

    completed = _run_codegen(spec_path, "python", tmp_path / "generated")

    assert completed.returncode == 1
    assert "code: 1005" in completed.stderr
    assert "symbol: INVALID_TYPE" in completed.stderr
    assert "path: /schema" in completed.stderr
    assert "message: Payload object is missing required field" in completed.stderr
    assert (
        'details_json: {"field":"schema","reason":"missing_field"}'
        in completed.stderr
    )


def test_codegen_cli_filesystem_error_does_not_invent_core_fields(
    tmp_path: Path,
) -> None:
    spec_path = tmp_path / "spec.json"
    spec_path.write_bytes(SPEC)
    output = tmp_path / "missing-parent" / "generated"

    completed = _run_codegen(spec_path, "python", output)

    assert completed.returncode == 1
    assert "fdb: error:" in completed.stderr
    assert "code:" not in completed.stderr
    assert "symbol:" not in completed.stderr
    assert "details_json:" not in completed.stderr
