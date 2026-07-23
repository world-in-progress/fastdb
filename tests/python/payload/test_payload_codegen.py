import hashlib
from concurrent.futures import ThreadPoolExecutor

import pytest

from fastdb4py.payload import (
    ArtifactKind,
    BuildPolicy,
    Builder,
    CodegenOptions,
    CodegenTarget,
    CompiledSpec,
    PayloadError,
)


SPEC_A = (
    b'{"schema":"fastdb.payload.v1","profile":"record.v1",'
    b'"entries":[{"id":"value","cardinality":"one",'
    b'"type":{"kind":"u8"}}],"components":[]}'
)
SPEC_B = (
    b'{"schema":"fastdb.payload.v1","profile":"record.v1",'
    b'"entries":[{"id":"other","cardinality":"one",'
    b'"type":{"kind":"u8"}}],"components":[]}'
)


def test_core_codegen_projects_all_targets_and_owned_artifacts() -> None:
    suffixes = {
        CodegenTarget.CPP: ".hpp",
        CodegenTarget.RUST: ".rs",
        CodegenTarget.PYTHON: ".py",
        CodegenTarget.TYPESCRIPT: ".ts",
    }
    with CompiledSpec.compile(SPEC_A) as spec:
        for target, suffix in suffixes.items():
            generated = spec.generate(target)
            assert generated.artifact_count() == 1
            clone = generated.clone()
            artifact = clone.artifact(0)
            clone.close()
            generated.close()
            assert artifact.kind is ArtifactKind.SOURCE
            assert artifact.relative_path.endswith(suffix)
            assert artifact.bytes
            assert artifact.sha256 == hashlib.sha256(artifact.bytes).digest()

        with pytest.raises(PayloadError) as limited:
            spec.generate(CodegenTarget.CPP, CodegenOptions(max_artifacts=0))
        assert limited.value.code == 6003
        assert limited.value.path == "/codegen/limits/max_artifacts"


@pytest.mark.parametrize("invalid_options", [False, 0, object()])
def test_core_codegen_rejects_non_options(invalid_options: object) -> None:
    with CompiledSpec.compile(SPEC_A) as spec:
        with pytest.raises(TypeError, match="options must be a CodegenOptions"):
            spec.generate(CodegenTarget.CPP, invalid_options)  # type: ignore[arg-type]


def test_core_codegen_revalidates_mutated_options_before_ffi() -> None:
    options = CodegenOptions()
    options.max_artifacts = -1
    with CompiledSpec.compile(SPEC_A) as spec:
        with pytest.raises(
            ValueError,
            match="max_artifacts must be an unsigned 64-bit integer",
        ):
            spec.generate(CodegenTarget.CPP, options)


def test_artifact_set_clone_and_concurrent_queries_own_native_lifetimes() -> None:
    with CompiledSpec.compile(SPEC_A) as spec:
        original = spec.generate(CodegenTarget.PYTHON)
    clone = original.clone()
    original.close()
    original.close()

    def observe(_: int) -> tuple[str, ArtifactKind, bytes, bytes]:
        artifact = clone.artifact(0)
        return (
            artifact.relative_path,
            artifact.kind,
            artifact.bytes,
            artifact.sha256,
        )

    try:
        with ThreadPoolExecutor(max_workers=8) as executor:
            observations = list(executor.map(observe, range(64)))
        assert all(
            observation == observations[0] for observation in observations
        )
    finally:
        clone.close()
        clone.close()

    with pytest.raises(PayloadError, match="ArtifactSet is closed"):
        clone.artifact_count()


def test_core_provenance_guards_reject_same_indexes_from_another_spec() -> None:
    with (
        CompiledSpec.compile(SPEC_A) as spec_a,
        CompiledSpec.compile(SPEC_B) as spec_b,
    ):
        digest_a = spec_a.sha256()
        digest_b = spec_b.sha256()
        builder = Builder.create(spec_a)
        builder.require_spec_sha256(digest_a)
        with pytest.raises(PayloadError) as builder_error:
            builder.require_spec_sha256(digest_b)
        assert builder_error.value.code == 3006
        assert builder_error.value.path == "/builder/spec_sha256"

        plan = builder.entry_begin(0, 1).value_u8(7).freeze()
        builder.close()
        result = plan.execute(BuildPolicy.ALLOW_STAGING)
        payload = result.payload
        payload.require_spec_sha256(digest_a)
        with pytest.raises(PayloadError) as payload_error:
            payload.require_spec_sha256(digest_b)
        assert payload_error.value.code == 3006
        assert payload_error.value.path == "/payload/spec_sha256"

        view = payload.entry_view(0)
        detached = view.materialize()
        for candidate in (view, detached):
            candidate.require_spec_sha256(digest_a)
            with pytest.raises(PayloadError) as view_error:
                candidate.require_spec_sha256(digest_b)
            assert view_error.value.code == 3006
            assert view_error.value.path == "/view/spec_sha256"

        detached.close()
        view.close()
        payload.close()
        plan.close()
