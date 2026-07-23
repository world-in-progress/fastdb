#!/usr/bin/env python3
"""Generate every public projection through Core and execute it in a clean tree."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
from types import ModuleType
from typing import Callable

from fastdb4py.payload import (
    ArtifactKind,
    BuildPolicy,
    Builder,
    CodegenTarget,
    CompiledSpec,
    PayloadError,
    View,
)


ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = ROOT / "tests/generated"
SPEC_A = (
    b'{"schema":"fastdb.payload.v1","profile":"record.v1",'
    b'"entries":[{"id":"root","cardinality":"one",'
    b'"type":{"kind":"component","id":"Item"}}],'
    b'"components":[{"id":"Item","kind":"record","fields":['
    b'{"id":"value","type":{"kind":"u8"}}]}]}'
)
SPEC_B = (
    b'{"schema":"fastdb.payload.v1","profile":"record.v1",'
    b'"entries":[{"id":"other","cardinality":"one",'
    b'"type":{"kind":"component","id":"Other"}}],'
    b'"components":[{"id":"Other","kind":"record","fields":['
    b'{"id":"different","type":{"kind":"u8"}}]}]}'
)
TARGETS = (
    (CodegenTarget.CPP, ".hpp"),
    (CodegenTarget.RUST, ".rs"),
    (CodegenTarget.PYTHON, ".py"),
    (CodegenTarget.TYPESCRIPT, ".ts"),
)
HOSTILE_SPEC_NAMES = (
    "record-all-types",
    "recursive-lists",
    "keyword-prefix-collisions",
    "shared-cyclic-graph",
)
KEYWORD_PREFIX_COLLISION_SPEC = (
    b'{"schema":"fastdb.payload.v1","profile":"object_graph.v1",'
    b'"entries":[{"id":"class","cardinality":"one",'
    b'"type":{"kind":"component","id":"type"}},'
    b'{"id":"fdb_cpp_id_636c617373","cardinality":"many",'
    b'"type":{"kind":"ref","target":"type","nullable":true}}],'
    b'"components":[{"id":"type","kind":"record","fields":['
    b'{"id":"match","type":{"kind":"u32"}},'
    b'{"id":"interface","type":{"kind":"ref","target":"type",'
    b'"nullable":true}},'
    b'{"id":"fdb_ts_id_696e74657266616365","type":{"kind":"list",'
    b'"nullable":true,"items":{"kind":"ref","target":"type"}}}]}]}'
)


class HarnessError(RuntimeError):
    """A generated projection did not compile, import, or execute."""


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--library", type=Path)
    return parser.parse_args()


def run(command: list[str], *, cwd: Path, environment: dict[str, str]) -> None:
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        raise HarnessError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def native_library(arguments: argparse.Namespace) -> Path:
    if arguments.library is not None:
        candidate = arguments.library.resolve(strict=True)
        if not candidate.is_file():
            raise HarnessError(f"--library is not a regular file: {candidate}")
        return candidate
    filename = {
        "darwin": "libfastdb.dylib",
        "linux": "libfastdb.so",
        "win32": "fastdb.dll",
    }.get(sys.platform)
    if filename is None:
        raise HarnessError(f"unsupported platform: {sys.platform}")
    build_dir = arguments.build_dir.resolve(strict=True)
    candidates = sorted(
        {item.resolve() for item in build_dir.rglob(filename) if item.is_file()}
    )
    if len(candidates) != 1:
        listing = "\n".join(f"  - {item}" for item in candidates) or "  (none)"
        raise HarnessError(
            "expected exactly one built FastDB shared library; candidates:\n"
            + listing
        )
    return candidates[0]


def configure_python_library(library: Path) -> None:
    configured = os.environ.get("FASTDB_PAYLOAD_LIBRARY")
    if configured:
        try:
            python_library = Path(configured).expanduser().resolve(strict=True)
        except OSError as error:
            raise HarnessError(
                "FASTDB_PAYLOAD_LIBRARY does not resolve to a native library: "
                f"{configured}"
            ) from error
        if not python_library.is_file():
            raise HarnessError(
                "FASTDB_PAYLOAD_LIBRARY is not a regular file: "
                f"{python_library}"
            )
        if python_library != library:
            raise HarnessError(
                "Python and compiled generated projections must use the same "
                "FastDB Core library: "
                f"python={python_library}, compiled={library}"
            )
    else:
        os.environ["FASTDB_PAYLOAD_LIBRARY"] = os.fspath(library)


def runtime_environment(library: Path) -> dict[str, str]:
    environment = dict(os.environ)
    variable = {
        "darwin": "DYLD_LIBRARY_PATH",
        "linux": "LD_LIBRARY_PATH",
        "win32": "PATH",
    }[sys.platform]
    previous = environment.get(variable)
    environment[variable] = (
        os.fspath(library.parent)
        if not previous
        else os.pathsep.join((os.fspath(library.parent), previous))
    )
    return environment


def generate_artifacts_for_source(
    directory: Path, source: bytes
) -> tuple[str, dict[str, Path]]:
    artifacts: dict[str, Path] = {}
    with CompiledSpec.compile(source) as spec:
        digest = spec.sha256().hex()
        for target, suffix in TARGETS:
            generated = spec.generate(target)
            try:
                if generated.artifact_count() != 1:
                    raise HarnessError(f"{target.name} did not produce one artifact")
                artifact = generated.artifact(0)
            finally:
                generated.close()
            relative = Path(artifact.relative_path)
            if (
                artifact.kind is not ArtifactKind.SOURCE
                or relative.is_absolute()
                or relative.name != artifact.relative_path
                or relative.suffix != suffix
            ):
                raise HarnessError(
                    f"{target.name} produced an invalid artifact: "
                    f"{artifact.relative_path}"
                )
            if hashlib.sha256(artifact.bytes).digest() != artifact.sha256:
                raise HarnessError(f"{target.name} artifact hash does not match")
            path = directory / relative
            path.write_bytes(artifact.bytes)
            artifacts[suffix] = path
    if set(artifacts) != {suffix for _, suffix in TARGETS}:
        raise HarnessError("Core did not publish the exact four-target artifact set")
    return digest, artifacts


def generate_artifacts(directory: Path) -> tuple[str, dict[str, Path]]:
    return generate_artifacts_for_source(directory, SPEC_A)


def hostile_spec_sources() -> tuple[tuple[str, bytes], ...]:
    sources = (
        (
            "record-all-types",
            ROOT
            / "tests/golden/payload/v1/spec/valid/"
            "record-all-types.source.json",
        ),
        (
            "recursive-lists",
            ROOT
            / "tests/golden/payload/v1/binary/spec/"
            "nested-lists.source.json",
        ),
        (
            "shared-cyclic-graph",
            ROOT
            / "tests/golden/payload/v1/binary/spec/"
            "graph-shared-cycle.source.json",
        ),
    )
    loaded = {name: path.read_bytes() for name, path in sources}
    loaded["keyword-prefix-collisions"] = KEYWORD_PREFIX_COLLISION_SPEC
    return tuple((name, loaded[name]) for name in HOSTILE_SPEC_NAMES)


def expect_digest_mismatch(callback: Callable[[], object], path: str) -> None:
    try:
        callback()
    except PayloadError as error:
        if (
            error.code != 3006
            or error.symbol != "DIGEST_MISMATCH"
            or error.path != path
            or '"reason":"spec_digest_mismatch"' not in error.details_json
        ):
            raise HarnessError(
                f"generated Python guard returned the wrong error: {error!r}"
            ) from error
    else:
        raise HarnessError(f"generated Python guard accepted a foreign handle: {path}")


def load_generated_python(path: Path) -> ModuleType:
    name = f"fastdb_generated_{path.stem}"
    module_spec = importlib.util.spec_from_file_location(name, path)
    if module_spec is None or module_spec.loader is None:
        raise HarnessError(f"cannot create an import spec for {path}")
    module = importlib.util.module_from_spec(module_spec)
    sys.modules[name] = module
    try:
        module_spec.loader.exec_module(module)
    finally:
        sys.modules.pop(name, None)
    return module


def run_python_projection(path: Path) -> None:
    generated = load_generated_python(path)
    spec_a = generated.compile_spec()
    spec_b = CompiledSpec.compile(SPEC_B)
    wrong_builder = Builder.create(spec_b)
    owned: list[object] = []
    try:
        expect_digest_mismatch(
            lambda: generated.fdb_python_id_726f6f74_builder_entry_begin(
                wrong_builder, 1
            ),
            "/builder/spec_sha256",
        )
        wrong_plan = (
            wrong_builder.entry_begin(0, 1)
            .value_component_begin()
            .value_u8(9)
            .freeze()
        )
        owned.append(wrong_plan)
        wrong_builder.close()
        wrong_payload = wrong_plan.execute(BuildPolicy.ALLOW_STAGING).payload
        owned.append(wrong_payload)
        expect_digest_mismatch(
            lambda: generated.fdb_python_id_726f6f74_from_payload(wrong_payload),
            "/payload/spec_sha256",
        )
        with wrong_payload.entry_view(0) as wrong_entry:
            wrong_view = wrong_entry.at(0)
        owned.append(wrong_view)
        expect_digest_mismatch(
            lambda: generated.FdbPythonType_fdb_python_id_4974656d_View.try_from_view(
                wrong_view
            ),
            "/view/spec_sha256",
        )

        builder = Builder.create(spec_a)
        owned.append(builder)
        generated.fdb_python_id_726f6f74_builder_entry_begin(
            builder, 1
        ).value_component_begin().value_u8(7)
        plan = builder.freeze()
        owned.append(plan)
        builder.close()
        payload = plan.execute(BuildPolicy.ALLOW_STAGING).payload
        owned.append(payload)
        entry = generated.fdb_python_id_726f6f74_from_payload(payload)
        owned.append(entry)
        component_view = entry.at(0)
        try:
            component = (
                generated.FdbPythonType_fdb_python_id_4974656d_View.try_from_view(
                    component_view
                )
            )
        finally:
            component_view.close()
        if component is None:
            raise HarnessError("generated Python component projection was absent")
        owned.append(component)
        original_close = View.close
        original_del = View.__del__
        original_get_u8 = View.get_u8
        captured_finalizers: list[View] = []
        close_calls = 0

        def counting_close(view: View) -> None:
            nonlocal close_calls
            close_calls += 1
            original_close(view)

        def capture_finalizer(view: View) -> None:
            captured_finalizers.append(view)

        View.close = counting_close
        View.__del__ = capture_finalizer
        try:
            if component.fdb_python_id_76616c7565_value() != 7:
                raise HarnessError(
                    "generated Python projection returned the wrong value"
                )
            if close_calls != 1:
                raise HarnessError(
                    "generated Python scalar helper did not explicitly close "
                    "its temporary View"
                )

            def fail_get_u8(_view: View) -> int:
                raise RuntimeError("injected generated getter failure")

            View.get_u8 = fail_get_u8
            try:
                component.fdb_python_id_76616c7565_value()
            except RuntimeError as error:
                if str(error) != "injected generated getter failure":
                    raise
            else:
                raise HarnessError("generated Python scalar helper hid getter failure")
            if close_calls != 2:
                raise HarnessError(
                    "generated Python scalar helper did not explicitly close "
                    "its temporary View after getter failure"
                )
        finally:
            View.get_u8 = original_get_u8
            View.__del__ = original_del
            View.close = original_close
            for captured in captured_finalizers:
                original_close(captured)
            captured_finalizers.clear()
    finally:
        for handle in reversed(owned):
            close = getattr(handle, "close", None)
            if close is not None:
                close()
        wrong_builder.close()
        spec_b.close()
        spec_a.close()


def template(name: str, replacements: dict[str, str] | None = None) -> str:
    content = (TEMPLATES / name).read_text(encoding="utf-8")
    for marker, value in (replacements or {}).items():
        content = content.replace(marker, value)
    return content


def run_cpp_projection(
    directory: Path,
    digest: str,
    artifact: Path,
    library: Path,
    environment: dict[str, str],
) -> None:
    if sys.platform == "win32":
        raise HarnessError("Windows generated-C++ linking is not implemented")
    compiler_name = os.environ.get("CXX", "c++")
    compiler = shutil.which(compiler_name)
    if compiler is None:
        raise HarnessError(f"C++ compiler is unavailable: {compiler_name}")
    source = directory / "cpp-smoke.cpp"
    executable = directory / "cpp-smoke"
    source.write_text(
        template(
            "payload_codegen_smoke.cpp",
            {"@HEADER@": artifact.name, "@DIGEST@": digest},
        ),
        encoding="utf-8",
        newline="\n",
    )
    run(
        [
            compiler,
            "-std=c++17",
            "-pthread",
            "-I",
            os.fspath(ROOT / "fastcarto/fastdb/include"),
            "-I",
            os.fspath(directory),
            os.fspath(source),
            "-L",
            os.fspath(library.parent),
            "-lfastdb",
            f"-Wl,-rpath,{library.parent}",
            "-o",
            os.fspath(executable),
        ],
        cwd=directory,
        environment=environment,
    )
    run([os.fspath(executable)], cwd=directory, environment=environment)


def run_rust_projection(
    directory: Path,
    artifact: Path,
    library: Path,
    environment: dict[str, str],
) -> None:
    cargo = shutil.which("cargo")
    if cargo is None:
        raise HarnessError("cargo is unavailable")
    project = directory / "rust"
    source_dir = project / "src"
    source_dir.mkdir(parents=True)
    shutil.copy2(artifact, source_dir / "generated.rs")
    (source_dir / "main.rs").write_text(
        template("payload_codegen_smoke.rs"), encoding="utf-8", newline="\n"
    )
    crate_path = os.fspath(ROOT / "bindings/rust/fastdb").replace("\\", "\\\\")
    (project / "Cargo.toml").write_text(
        textwrap.dedent(
            f"""
            [package]
            name = "fastdb-generated-projection-smoke"
            version = "0.0.0"
            edition = "2024"
            publish = false

            [dependencies]
            fastdb = {{ path = {crate_path!r} }}
            """
        ).lstrip(),
        encoding="utf-8",
        newline="\n",
    )
    rust_environment = dict(environment)
    rust_environment["FASTDB_PAYLOAD_LINK_MODE"] = "system"
    rust_environment["FASTDB_PAYLOAD_SYSTEM_LIB_DIR"] = os.fspath(library.parent)
    run(
        [
            cargo,
            "run",
            "--quiet",
            "--offline",
            "--manifest-path",
            os.fspath(project / "Cargo.toml"),
            "--target-dir",
            os.fspath(project / "target"),
        ],
        cwd=project,
        environment=rust_environment,
    )


def run_typescript_projection(
    directory: Path,
    artifact: Path,
    environment: dict[str, str],
) -> None:
    node = shutil.which("node")
    tsc_name = "tsc.cmd" if sys.platform == "win32" else "tsc"
    tsc = ROOT / "ts/fastdb4ts/node_modules/.bin" / tsc_name
    package = ROOT / "ts/fastdb4ts"
    if node is None:
        raise HarnessError("node is unavailable")
    if not tsc.is_file():
        raise HarnessError(f"TypeScript compiler is unavailable: {tsc}")
    if not (package / "dist/payload/index.js").is_file():
        raise HarnessError("fastdb4ts must be built before this harness")
    project = directory / "typescript"
    project.mkdir()
    shutil.copy2(artifact, project / "generated.ts")
    (project / "smoke.ts").write_text(
        template("payload_codegen_smoke.ts"), encoding="utf-8", newline="\n"
    )
    (project / "package.json").write_text(
        '{"name":"fastdb-generated-projection-smoke","private":true,"type":"module"}\n',
        encoding="utf-8",
        newline="\n",
    )
    (project / "tsconfig.json").write_text(
        template("payload_codegen_tsconfig.json"),
        encoding="utf-8",
        newline="\n",
    )
    installed = project / "node_modules/fastdb4ts"
    installed.mkdir(parents=True)
    shutil.copy2(package / "package.json", installed / "package.json")
    shutil.copytree(package / "dist", installed / "dist")
    run(
        [os.fspath(tsc), "-p", os.fspath(project / "tsconfig.json")],
        cwd=project,
        environment=environment,
    )
    run(
        [node, os.fspath(project / "dist/smoke.js")],
        cwd=project,
        environment=environment,
    )


def run_hostile_cpp_matrix(
    directory: Path,
    cases: tuple[tuple[str, str, dict[str, Path]], ...],
    library: Path,
    environment: dict[str, str],
) -> None:
    if sys.platform == "win32":
        raise HarnessError("Windows generated-C++ linking is not implemented")
    compiler_name = os.environ.get("CXX", "c++")
    compiler = shutil.which(compiler_name)
    if compiler is None:
        raise HarnessError(f"C++ compiler is unavailable: {compiler_name}")
    source = directory / "hostile-cpp.cpp"
    executable = directory / "hostile-cpp"
    lines = [f'#include "{artifacts[".hpp"].name}"' for _, _, artifacts in cases]
    lines.extend(("", "int main() {"))
    for index, (_, digest, _) in enumerate(cases):
        lines.append(
            f"    auto spec_{index} = "
            f"fastdb_payload_{digest}::compile_spec();"
        )
        lines.append(f"    static_cast<void>(spec_{index});")
    lines.extend(("    return 0;", "}", ""))
    source.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            "-I",
            os.fspath(ROOT / "fastcarto/fastdb/include"),
            "-I",
            os.fspath(directory / "artifacts"),
            os.fspath(source),
            "-L",
            os.fspath(library.parent),
            "-lfastdb",
            f"-Wl,-rpath,{library.parent}",
            "-o",
            os.fspath(executable),
        ],
        cwd=directory,
        environment=environment,
    )
    run([os.fspath(executable)], cwd=directory, environment=environment)


def run_hostile_rust_matrix(
    directory: Path,
    cases: tuple[tuple[str, str, dict[str, Path]], ...],
    library: Path,
    environment: dict[str, str],
) -> None:
    cargo = shutil.which("cargo")
    if cargo is None:
        raise HarnessError("cargo is unavailable")
    project = directory / "hostile-rust"
    source_dir = project / "src"
    source_dir.mkdir(parents=True)
    main_lines: list[str] = []
    for index, (_, _, artifacts) in enumerate(cases):
        generated_name = f"generated_{index}.rs"
        shutil.copy2(artifacts[".rs"], source_dir / generated_name)
        main_lines.append(f'#[path = "{generated_name}"]')
        main_lines.append(f"mod generated_{index};")
    main_lines.extend(("", "fn main() -> Result<(), fastdb::PayloadError> {"))
    for index, _ in enumerate(cases):
        main_lines.append(
            f"    let spec_{index} = generated_{index}::compile_spec()?;"
        )
        main_lines.append(f"    drop(spec_{index});")
    main_lines.extend(("    Ok(())", "}", ""))
    (source_dir / "main.rs").write_text(
        "\n".join(main_lines), encoding="utf-8", newline="\n"
    )
    crate_path = os.fspath(ROOT / "bindings/rust/fastdb").replace("\\", "\\\\")
    (project / "Cargo.toml").write_text(
        textwrap.dedent(
            f"""
            [package]
            name = "fastdb-hostile-generated-projection-smoke"
            version = "0.0.0"
            edition = "2024"
            publish = false

            [dependencies]
            fastdb = {{ path = {crate_path!r} }}
            """
        ).lstrip(),
        encoding="utf-8",
        newline="\n",
    )
    rust_environment = dict(environment)
    rust_environment["FASTDB_PAYLOAD_LINK_MODE"] = "system"
    rust_environment["FASTDB_PAYLOAD_SYSTEM_LIB_DIR"] = os.fspath(library.parent)
    run(
        [
            cargo,
            "run",
            "--quiet",
            "--offline",
            "--manifest-path",
            os.fspath(project / "Cargo.toml"),
            "--target-dir",
            os.fspath(project / "target"),
        ],
        cwd=project,
        environment=rust_environment,
    )


def run_hostile_python_matrix(
    cases: tuple[tuple[str, str, dict[str, Path]], ...],
) -> None:
    for name, digest, artifacts in cases:
        generated = load_generated_python(artifacts[".py"])
        spec = generated.compile_spec()
        try:
            if spec.sha256().hex() != digest:
                raise HarnessError(
                    f"{name} generated Python compiled the wrong specification"
                )
        finally:
            spec.close()


def run_hostile_typescript_matrix(
    directory: Path,
    cases: tuple[tuple[str, str, dict[str, Path]], ...],
    environment: dict[str, str],
) -> None:
    node = shutil.which("node")
    tsc_name = "tsc.cmd" if sys.platform == "win32" else "tsc"
    tsc = ROOT / "ts/fastdb4ts/node_modules/.bin" / tsc_name
    package = ROOT / "ts/fastdb4ts"
    if node is None:
        raise HarnessError("node is unavailable")
    if not tsc.is_file():
        raise HarnessError(f"TypeScript compiler is unavailable: {tsc}")
    if not (package / "dist/payload/index.js").is_file():
        raise HarnessError("fastdb4ts must be built before this harness")

    project = directory / "hostile-typescript"
    project.mkdir()
    smoke_lines: list[str] = [
        "import { initPayload } from 'fastdb4ts/payload';"
    ]
    for index, (_, _, artifacts) in enumerate(cases):
        generated_name = f"generated_{index}.ts"
        shutil.copy2(artifacts[".ts"], project / generated_name)
        smoke_lines.append(
            f"import * as generated{index} from './generated_{index}.js';"
        )
    smoke_lines.extend(("", "await initPayload();"))
    for index, (_, digest, _) in enumerate(cases):
        smoke_lines.append(
            f"if (generated{index}.PAYLOAD_SHA256 !== '{digest}') "
            "{ throw new Error('generated digest mismatch'); }"
        )
        smoke_lines.append(f"const spec{index} = generated{index}.compileSpec();")
        smoke_lines.append(f"spec{index}.dispose();")
    smoke_lines.append("")
    (project / "smoke.ts").write_text(
        "\n".join(smoke_lines), encoding="utf-8", newline="\n"
    )
    (project / "package.json").write_text(
        '{"name":"fastdb-hostile-generated-projection-smoke",'
        '"private":true,"type":"module"}\n',
        encoding="utf-8",
        newline="\n",
    )
    (project / "tsconfig.json").write_text(
        template("payload_codegen_tsconfig.json"),
        encoding="utf-8",
        newline="\n",
    )
    installed = project / "node_modules/fastdb4ts"
    installed.mkdir(parents=True)
    shutil.copy2(package / "package.json", installed / "package.json")
    shutil.copytree(package / "dist", installed / "dist")
    run(
        [os.fspath(tsc), "-p", os.fspath(project / "tsconfig.json")],
        cwd=project,
        environment=environment,
    )
    run(
        [node, os.fspath(project / "dist/smoke.js")],
        cwd=project,
        environment=environment,
    )


def run_hostile_codegen_matrix(
    directory: Path,
    library: Path,
    environment: dict[str, str],
) -> None:
    matrix = directory / "hostile-matrix"
    artifacts_directory = matrix / "artifacts"
    artifacts_directory.mkdir(parents=True)
    cases: list[tuple[str, str, dict[str, Path]]] = []
    digests: set[str] = set()
    for name, source in hostile_spec_sources():
        digest, artifacts = generate_artifacts_for_source(
            artifacts_directory, source
        )
        if digest in digests:
            raise HarnessError(f"hostile specification digest is duplicated: {name}")
        digests.add(digest)
        cases.append((name, digest, artifacts))
    frozen_cases = tuple(cases)
    if tuple(name for name, _, _ in frozen_cases) != HOSTILE_SPEC_NAMES:
        raise HarnessError("hostile specification matrix is incomplete or reordered")
    run_hostile_cpp_matrix(matrix, frozen_cases, library, environment)
    run_hostile_rust_matrix(matrix, frozen_cases, library, environment)
    run_hostile_python_matrix(frozen_cases)
    run_hostile_typescript_matrix(matrix, frozen_cases, environment)


def main() -> int:
    arguments = parse_arguments()
    try:
        library = native_library(arguments)
        configure_python_library(library)
        environment = runtime_environment(library)
        with tempfile.TemporaryDirectory(
            prefix="fastdb-generated-projections-"
        ) as temporary:
            directory = Path(temporary)
            digest, artifacts = generate_artifacts(directory)
            run_python_projection(artifacts[".py"])
            run_cpp_projection(
                directory, digest, artifacts[".hpp"], library, environment
            )
            run_rust_projection(
                directory, artifacts[".rs"], library, environment
            )
            run_typescript_projection(directory, artifacts[".ts"], environment)
            run_hostile_codegen_matrix(directory, library, environment)
    except (HarnessError, OSError, PayloadError) as error:
        print(f"generated payload projection harness failed: {error}", file=sys.stderr)
        return 1
    print(
        "generated payload projection harness passed: "
        "Core ABI -> C++/Rust/Python/TypeScript compile/import/runtime plus "
        "four-shape hostile matrix"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
