#!/usr/bin/env python3
"""Adversarial unit tests for the P5 clean-cut repository gate."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "check_p5_clean_cut.py"
SPEC = importlib.util.spec_from_file_location("check_p5_clean_cut", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

POLICY_PATH = ROOT / "tests" / "ci" / "p5_clean_cut_policy.json"
PRODUCTION_POLICY = MODULE.load_policy(POLICY_PATH)
OBSOLETE_AUTHORITY = str(PRODUCTION_POLICY["forbidden_authority_literals"][0])
DOWNSTREAM_DOMAIN = str(PRODUCTION_POLICY["forbidden_domain_literals"][0])
STANDALONE_MARKER = "Standalone boundary marker."
RUST_SAFE_MODULES = ("builder.rs", "codegen.rs", "lib.rs", "runtime.rs")
RUST_SYS_MODULES = ("lib.rs",)


def write(root: Path, relative: str, content: str | bytes = "") -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, bytes):
        path.write_bytes(content)
    else:
        path.write_text(content, encoding="utf-8")
    return path


def commit_repository(root: Path) -> None:
    subprocess.run(
        ["git", "-C", str(root), "add", "-A"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "-c",
            "user.name=FastDB P5 Fixture",
            "-c",
            "user.email=fastdb-p5-fixture@example.invalid",
            "commit",
            "--no-gpg-sign",
            "-q",
            "-m",
            "fixture state",
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def valid_policy() -> dict[str, object]:
    return {
        "schema": "fastdb.p5-clean-cut-policy.v1",
        "native_abi_symbol_count": 117,
        "python_version": "0.2.1",
        "typescript_version": "0.2.1",
        "removed_paths": list(PRODUCTION_POLICY["removed_paths"]),
        "required_paths": [
            "src/standalone.py",
            "tools/check_p5_clean_cut.py",
            "tests/ci/test_check_p5_clean_cut.py",
        ],
        "historical_allowlist": ["docs/history.md"],
        "literal_carrier_allowlist": ["tests/ci/policy.json"],
        "superseded_documents": ["docs/history.md"],
        "authority_scan_roots": ["."],
        "domain_scan_roots": ["src", "bindings/rust"],
        "forbidden_authority_literals": list(
            PRODUCTION_POLICY["forbidden_authority_literals"]
        ),
        "forbidden_domain_literals": list(
            PRODUCTION_POLICY["forbidden_domain_literals"]
        ),
        "standalone_markers": {
            "src/standalone.py": STANDALONE_MARKER,
        },
    }


def abi_symbols(count: int = 117) -> str:
    return "".join(
        f"fdb_payload_v1_fixture_{index:03d}\n" for index in range(count)
    )


def create_repository(root: Path) -> dict[str, object]:
    subprocess.run(
        ["git", "init", "-q", str(root)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    write(root, ".gitignore", "ignored/\n")
    policy = valid_policy()
    write(
        root,
        "tests/ci/policy.json",
        json.dumps(policy, indent=2) + "\n",
    )
    write(root, "src/standalone.py", f'"""{STANDALONE_MARKER}"""\n')
    write(root, "tools/check_p5_clean_cut.py", "# fixture gate\n")
    write(root, "tests/ci/test_check_p5_clean_cut.py", "# fixture tests\n")

    write(
        root,
        "docs/accepted.md",
        "# Accepted design\n\nThe current contract is authoritative.\n",
    )
    write(
        root,
        "docs/current.md",
        "# Current guide\n\nSee [the accepted design](accepted.md#accepted-design).\n",
    )
    write(
        root,
        "docs/history.md",
        "# Superseded historical record\n\n"
        "> Superseded. Retained as historical evidence; see the "
        "[accepted design](accepted.md#accepted-design).\n",
    )
    write(
        root,
        "README.md",
        "# Fixture\n\n"
        "See [the current guide](docs/current.md#current-guide).\n",
    )
    write(
        root,
        "python/README.md",
        "# Python binding\n\nP5 local clean cut is complete.\n",
    )
    write(
        root,
        "fastcarto/README.md",
        "# Native core\n\nP5 local clean cut is complete at exact ABI-117.\n",
    )
    write(
        root,
        "ts/fastdb4ts/README.md",
        "# TypeScript binding\n\nP5 local clean cut is complete.\n",
    )

    write(
        root,
        "pyproject.toml",
        '[project]\nname = "fixture"\nversion = "0.2.1"\n',
    )
    write(
        root,
        "ts/fastdb4ts/package.json",
        '{"name":"fixture","version":"0.2.1"}\n',
    )
    write(root, "tests/abi/fastdb_payload_v1_symbols.txt", abi_symbols())
    write(
        root,
        "tools/generate_payload_wasm_exports.py",
        "EXPECTED_ABI_SYMBOLS = 117\n"
        'ALLOWLIST = "tests/abi/fastdb_payload_v1_symbols.txt"\n',
    )
    write(
        root,
        "tests/ci/p4_projection_codegen_map.json",
        '{"abi":{"symbol_count":117,'
        '"allowlist":"tests/abi/fastdb_payload_v1_symbols.txt"}}\n',
    )

    write(
        root,
        "python/fastdb4py/cli.py",
        "from fastdb4py.payload import CompiledSpec\n"
        "def run(source, target, output):\n"
        "    with CompiledSpec.compile(source) as spec:\n"
        "        with spec.generate(target) as generated:\n"
        "            artifacts = _validated_artifacts(generated)\n"
        "    _write_new_tree(output, artifacts)\n"
        "def _validated_artifacts(generated):\n"
        "    return generated\n"
        "def _write_new_tree(output, artifacts):\n"
        "    with output.open(\"xb\") as stream:\n"
        "        stream.write(artifacts)\n",
    )
    write(
        root,
        "tools/check_python_package_inventory.py",
        "def check_swig_diagnostics(output):\n"
        "    if 'Warning ' in output:\n"
        "        raise RuntimeError(output)\n",
    )
    write(
        root,
        ".github/workflows/tests.yml",
        "jobs:\n"
        "  clean_cut:\n"
        "    runs-on: ubuntu-latest\n"
        "    steps:\n"
        "      - uses: actions/checkout@v6\n"
        "      - name: Enforce the clean cut\n"
        "        shell: bash\n"
        "        run: |\n"
        "          python3 tests/ci/test_check_p5_clean_cut.py\n"
        "          python3 tools/check_p5_clean_cut.py --check\n",
    )
    write(
        root,
        "docs/issues/0002-portable-payload-foundation-implementation-status.md",
        "# Portable payload implementation status\n\n"
        "Status: Open\n\n"
        "#### P5 Task 6 local evidence\n\n"
        "P5 Task 7 is locally complete.\n\n"
        "**P5 local clean cut:** Complete\n\n"
        "The **P5 local clean cut is complete** at exact ABI-117.\n\n"
        "Hosted execution remains pending. Version change, push, tag, "
        "publication, release, and downstream C-" "Two composition remain "
        "pending.\n",
    )
    write(
        root,
        "docs/issues/0003-legacy-swig-diagnostics.md",
        "# Binding diagnostics\n\n"
        "Status: Closed\n\n"
        "P5 Task 4 closed this issue with zero matched SWIG diagnostics. "
        "Hosted execution remains pending.\n",
    )
    write(
        root,
        "docs/issues/README.md",
        "# Issue index\n\n"
        "| Issue | Status |\n"
        "|---|---|\n"
        "| [0002](0002-portable-payload-foundation-implementation-status.md) "
        "| Open (P1-P5 locally frozen; hosted/release evidence pending) |\n"
        "| [0003](0003-legacy-swig-diagnostics.md) | Closed |\n",
    )

    for relative in MODULE.PYTHON_PORTABLE_MODULES:
        write(root, f"python/fastdb4py/payload/{relative}", "# projection\n")
    for relative in MODULE.TYPESCRIPT_PORTABLE_MODULES:
        write(root, f"ts/fastdb4ts/src/payload/{relative}", "// projection\n")
    for relative in RUST_SAFE_MODULES:
        write(root, f"bindings/rust/fastdb/src/{relative}", "// projection\n")
    for relative in RUST_SYS_MODULES:
        write(root, f"bindings/rust/fastdb-sys/src/{relative}", "// projection\n")
    commit_repository(root)
    return policy


class RepositoryFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "repository"
        self.root.mkdir()
        self.policy = create_repository(self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def violations(
        self, policy: dict[str, object] | None = None
    ) -> list[str]:
        return MODULE.check_repository(
            self.root,
            self.policy if policy is None else policy,
        )

    def assert_rejected(self, fragment: str) -> None:
        violations = self.violations()
        self.assertTrue(
            any(fragment in violation for violation in violations),
            "\n".join(violations),
        )


class PolicyTests(RepositoryFixture):
    def test_clean_fixture_passes(self) -> None:
        self.assertEqual(self.violations(), [])

    def test_load_policy_rejects_duplicate_json_keys(self) -> None:
        path = write(
            self.root,
            "duplicate.json",
            '{"schema":"first","schema":"second"}',
        )
        with self.assertRaises(MODULE.CheckError):
            MODULE.load_policy(path)

    def test_rejects_unknown_or_reordered_policy_keys(self) -> None:
        unknown = copy.deepcopy(self.policy)
        unknown["unexpected"] = True
        self.assertTrue(MODULE.check_repository(self.root, unknown))

        reordered = {
            key: self.policy[key]
            for key in reversed(tuple(self.policy))
        }
        self.assertTrue(MODULE.check_repository(self.root, reordered))

    def test_rejects_broad_missing_or_escaping_allowlist_entries(self) -> None:
        for invalid in ("docs", "docs/*", "missing.md", "../outside.md"):
            changed = copy.deepcopy(self.policy)
            changed["historical_allowlist"] = [invalid]
            with self.subTest(invalid=invalid):
                self.assertTrue(MODULE.check_repository(self.root, changed))

    def test_rejects_symlink_escape(self) -> None:
        outside = Path(self.temporary.name) / "outside.py"
        outside.write_text("outside\n", encoding="utf-8")
        (self.root / "src/standalone.py").unlink()
        (self.root / "src/standalone.py").symlink_to(outside)
        self.assert_rejected("escapes")

    def test_rejects_intermediate_symlink_policy_path(self) -> None:
        (self.root / "linked-docs").symlink_to(self.root / "docs")
        changed = copy.deepcopy(self.policy)
        changed["historical_allowlist"] = ["linked-docs/history.md"]
        changed["superseded_documents"] = ["linked-docs/history.md"]
        violations = self.violations(changed)
        self.assertTrue(
            any(
                "historical allowlist entry must not contain a symlink component"
                in violation
                for violation in violations
            ),
            "\n".join(violations),
        )

    def test_production_policy_contract_is_exact_and_valid(self) -> None:
        policy = PRODUCTION_POLICY
        self.assertEqual(policy["schema"], "fastdb.p5-clean-cut-policy.v1")
        self.assertEqual(policy["native_abi_symbol_count"], 117)
        self.assertEqual(policy["python_version"], "0.2.1")
        self.assertEqual(policy["typescript_version"], "0.2.1")
        self.assertEqual(len(policy["removed_paths"]), 14)
        self.assertEqual(len(policy["required_paths"]), 10)
        self.assertEqual(len(policy["historical_allowlist"]), 27)
        self.assertEqual(len(policy["literal_carrier_allowlist"]), 1)
        self.assertEqual(len(policy["superseded_documents"]), 14)
        self.assertEqual(len(policy["forbidden_authority_literals"]), 26)
        self.assertEqual(len(policy["forbidden_domain_literals"]), 9)
        self.assertEqual(len(policy["standalone_markers"]), 8)
        self.assertTrue(
            {
                "bindings",
                "examples",
                "go",
                "schemas",
                "tools",
                "fastcarto/README.md",
            }.issubset(set(policy["domain_scan_roots"]))
        )
        self.assertEqual(
            MODULE.validate_policy(ROOT, policy),
            [],
        )


class InventoryAndAuthorityTests(RepositoryFixture):
    def test_rejects_dirty_worktree_index_and_untracked_files(self) -> None:
        write(self.root, "README.md", "# Locally modified fixture\n")
        self.assert_rejected("worktree/index")

        subprocess.run(
            ["git", "-C", str(self.root), "add", "README.md"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assert_rejected("worktree/index")

        commit_repository(self.root)
        write(self.root, "untracked-clean-content.txt", "local only\n")
        self.assert_rejected("worktree/index")

    def test_rejects_each_removed_path(self) -> None:
        for relative in self.policy["removed_paths"]:
            with self.subTest(relative=relative):
                path = write(self.root, str(relative), "# returned surface\n")
                self.assert_rejected("removed path")
                path.unlink()

    def test_scans_untracked_nonignored_files_as_raw_bytes(self) -> None:
        write(
            self.root,
            "untracked.bin",
            b"\xffprefix-" + OBSOLETE_AUTHORITY.encode("ascii") + b"-suffix",
        )
        self.assert_rejected("forbidden authority literal")

    def test_ignores_git_ignored_build_output(self) -> None:
        write(
            self.root,
            "ignored/output.txt",
            OBSOLETE_AUTHORITY,
        )
        self.assertEqual(self.violations(), [])

    def test_rejects_each_current_authority_literal(self) -> None:
        for literal in self.policy["forbidden_authority_literals"]:
            with self.subTest(literal=literal):
                path = write(self.root, "notes.bin", str(literal).encode("ascii"))
                self.assert_rejected("forbidden authority literal")
                path.unlink()

    def test_rejects_each_downstream_domain_literal(self) -> None:
        for literal in self.policy["forbidden_domain_literals"]:
            with self.subTest(literal=literal):
                path = write(
                    self.root,
                    "src/domain.bin",
                    str(literal).encode("ascii"),
                )
                self.assert_rejected("forbidden downstream-domain literal")
                path.unlink()

    def test_policy_is_the_only_current_literal_carrier(self) -> None:
        changed = copy.deepcopy(self.policy)
        changed["literal_carrier_allowlist"] = [
            "tests/ci/policy.json",
            "README.md",
        ]
        self.assertTrue(MODULE.check_repository(self.root, changed))

    def test_literal_carrier_path_is_policy_driven(self) -> None:
        changed = copy.deepcopy(self.policy)
        diagnostic = "Warning " + "325:"
        changed["forbidden_authority_literals"][0] = diagnostic
        changed["literal_carrier_allowlist"] = ["config/policy.json"]
        (self.root / "tests/ci/policy.json").unlink()
        write(
            self.root,
            "config/policy.json",
            json.dumps(changed, indent=2) + "\n",
        )
        commit_repository(self.root)
        self.assertEqual(self.violations(changed), [])

    def test_rejects_missing_standalone_marker(self) -> None:
        write(self.root, "src/standalone.py", "# marker removed\n")
        self.assert_rejected("standalone marker")

    def test_rejects_binding_side_semantic_duplicate(self) -> None:
        write(
            self.root,
            "python/fastdb4py/payload/profile.py",
            "import hashlib\n",
        )
        self.assert_rejected("portable module inventory")
        (self.root / "python/fastdb4py/payload/profile.py").unlink()
        write(
            self.root,
            "ts/fastdb4ts/src/payload/spec.ts",
            "const parsed = JSON.parse(source);\n",
        )
        self.assert_rejected("binding-side semantic")

    def test_rejects_binding_side_renderer(self) -> None:
        write(
            self.root,
            "python/fastdb4py/payload/_codegen.py",
            "def render_typescript(model):\n"
            "    return str(model)\n",
        )
        self.assert_rejected("binding-side semantic")

    def test_rejects_rust_binding_side_parser(self) -> None:
        write(
            self.root,
            "bindings/rust/fastdb/src/lib.rs",
            "use serde_json::Value;\n",
        )
        self.assert_rejected("binding-side semantic")

    def test_rejects_downstream_domain_in_rust_binding(self) -> None:
        write(
            self.root,
            "bindings/rust/fastdb/src/domain.rs",
            DOWNSTREAM_DOMAIN,
        )
        self.assert_rejected("forbidden downstream-domain literal")

    def test_violations_are_stably_sorted(self) -> None:
        write(self.root, "z.txt", OBSOLETE_AUTHORITY)
        write(self.root, "a.txt", OBSOLETE_AUTHORITY)
        violations = self.violations()
        self.assertEqual(violations, sorted(set(violations)))


class GovernanceSurfaceTests(RepositoryFixture):
    def test_rejects_active_looking_superseded_document(self) -> None:
        write(self.root, "docs/history.md", "# Current proposal\n")
        violations = self.violations()
        self.assertTrue(
            any("superseded document" in item for item in violations),
            "\n".join(violations),
        )

    def test_rejects_python_or_typescript_version_drift(self) -> None:
        write(
            self.root,
            "pyproject.toml",
            '[project]\nname = "fixture"\nversion = "9.9.9"\n',
        )
        self.assert_rejected("Python package version")
        write(
            self.root,
            "pyproject.toml",
            '[project]\nname = "fixture"\nversion = "0.2.1"\n',
        )
        write(
            self.root,
            "ts/fastdb4ts/package.json",
            '{"name":"fixture","version":"9.9.9"}\n',
        )
        self.assert_rejected("TypeScript package version")

    def test_rejects_native_wasm_or_proof_map_abi_drift(self) -> None:
        write(
            self.root,
            "tests/abi/fastdb_payload_v1_symbols.txt",
            abi_symbols(116),
        )
        self.assert_rejected("native ABI")
        write(
            self.root,
            "tests/abi/fastdb_payload_v1_symbols.txt",
            abi_symbols(),
        )
        write(
            self.root,
            "tools/generate_payload_wasm_exports.py",
            "EXPECTED_ABI_SYMBOLS = 116\n"
            'ALLOWLIST = "tests/abi/fastdb_payload_v1_symbols.txt"\n',
        )
        self.assert_rejected("Wasm ABI")
        write(
            self.root,
            "tools/generate_payload_wasm_exports.py",
            "EXPECTED_ABI_SYMBOLS = 117\n"
            'ALLOWLIST = "tests/abi/fastdb_payload_v1_symbols.txt"\n',
        )
        write(
            self.root,
            "tests/ci/p4_projection_codegen_map.json",
            '{"abi":{"symbol_count":116,'
            '"allowlist":"tests/abi/fastdb_payload_v1_symbols.txt"}}\n',
        )
        self.assert_rejected("P4 proof-map ABI")

    def test_rejects_cli_side_generation_or_unsafe_publication(self) -> None:
        cli = self.root / "python/fastdb4py/cli.py"
        cli.write_text(
            cli.read_text(encoding="utf-8").replace(
                "from fastdb4py.payload import CompiledSpec",
                "from fastdb4py.codegen import CompiledSpec",
            ),
            encoding="utf-8",
        )
        self.assert_rejected("CLI")
        create_repository_content = (
            "from fastdb4py.payload import CompiledSpec\n"
            "def run(source, target, output):\n"
            "    with CompiledSpec.compile(source) as spec:\n"
            "        with spec.generate(target) as generated:\n"
            "            artifacts = _validated_artifacts(generated)\n"
            "    _write_new_tree(output, artifacts)\n"
            "def _validated_artifacts(generated): return generated\n"
            "def _write_new_tree(output, artifacts):\n"
            "    with output.open(\"wb\") as stream: stream.write(artifacts)\n"
        )
        write(self.root, "python/fastdb4py/cli.py", create_repository_content)
        self.assert_rejected("exclusive")

    def test_rejects_new_binding_diagnostic_allowance(self) -> None:
        write(
            self.root,
            "tools/check_python_package_inventory.py",
            "SWIG_" + "ALLOWLIST = {'warning'}\n",
        )
        self.assert_rejected("SWIG diagnostic allowance")

    def test_rejects_issue_state_drift(self) -> None:
        issue = self.root / (
            "docs/issues/0002-portable-payload-foundation-"
            "implementation-status.md"
        )
        issue.write_text(
            issue.read_text(encoding="utf-8").replace(
                "Status: Open", "Status: Closed"
            ),
            encoding="utf-8",
        )
        self.assert_rejected("Issue 0002")
        issue.write_text(
            "# Status\n\nStatus: Open\n\nP5 Task 7 remains pending.\n",
            encoding="utf-8",
        )
        self.assert_rejected("Task 6")

    def test_rejects_missing_local_closure_marker(self) -> None:
        issue = self.root / (
            "docs/issues/0002-portable-payload-foundation-"
            "implementation-status.md"
        )
        issue.write_text(
            issue.read_text(encoding="utf-8").replace(
                "**P5 local clean cut:** Complete",
                "P5 local closure remains pending",
            ),
            encoding="utf-8",
        )
        self.assert_rejected("local clean cut")

    def test_rejects_stale_issue_index_status(self) -> None:
        index = self.root / "docs/issues/README.md"
        index.write_text(
            index.read_text(encoding="utf-8").replace(
                "| [0003](0003-legacy-swig-diagnostics.md) | Closed |",
                "| [0003](0003-legacy-swig-diagnostics.md) | Open |",
            ),
            encoding="utf-8",
        )
        self.assert_rejected("Issue index")

    def test_rejects_stale_current_readiness_document(self) -> None:
        readme = self.root / MODULE.ISSUE_0002
        readme.write_text(
            readme.read_text(encoding="utf-8").replace(
                "The **P5 local clean cut is complete** at exact ABI-117.",
                "P5 remains open.",
            ),
            encoding="utf-8",
        )
        self.assert_rejected("current readiness marker")

    def test_accepts_truthful_open_issue_0003(self) -> None:
        write(
            self.root,
            "docs/issues/0003-legacy-swig-diagnostics.md",
            "# Binding diagnostics\n\n"
            "Status: Open\n\n"
            "Clean package evidence remains pending. "
            "Hosted execution remains pending.\n",
        )
        commit_repository(self.root)
        self.assertEqual(self.violations(), [])

    def test_rejects_open_issue_0003_without_pending_evidence(self) -> None:
        write(
            self.root,
            "docs/issues/0003-legacy-swig-diagnostics.md",
            "# Binding diagnostics\n\n"
            "Status: Open\n\n"
            "Hosted execution remains pending.\n",
        )
        self.assert_rejected("Issue 0003")

    def test_rejects_open_issue_0003_with_completed_clean_package_claim(self) -> None:
        write(
            self.root,
            "docs/issues/0003-legacy-swig-diagnostics.md",
            "# Binding diagnostics\n\n"
            "Status: Open\n\n"
            "Clean package evidence is complete. "
            "Hosted execution remains pending.\n",
        )
        self.assert_rejected("Issue 0003")

    def test_rejects_missing_markdown_target_or_anchor(self) -> None:
        current = self.root / "docs/current.md"
        current.write_text(
            "# Current guide\n\n[missing](absent.md)\n",
            encoding="utf-8",
        )
        self.assert_rejected("Markdown target")
        current.write_text(
            "# Current guide\n\n[missing](accepted.md#absent-heading)\n",
            encoding="utf-8",
        )
        self.assert_rejected("Markdown anchor")

    def test_rejects_missing_link_outside_docs_and_package_readmes(self) -> None:
        write(
            self.root,
            "bindings/rust/README.md",
            "# Rust projection\n\n[missing](absent.md)\n",
        )
        self.assert_rejected("Markdown target")

    def test_markdown_link_scan_ignores_inline_code_calls(self) -> None:
        write(
            self.root,
            "docs/current.md",
            "# Current guide\n\n"
            "The expression `_builder[n](*items)` is code, not a link. "
            "See [accepted](accepted.md#accepted-design).\n",
        )
        commit_repository(self.root)
        self.assertEqual(self.violations(), [])

    def test_rejects_missing_workflow_integration(self) -> None:
        write(self.root, ".github/workflows/tests.yml", "steps: []\n")
        self.assert_rejected("workflow")

    def test_rejects_conditional_clean_cut_workflow_job(self) -> None:
        workflow = self.root / ".github/workflows/tests.yml"
        workflow.write_text(
            workflow.read_text(encoding="utf-8").replace(
                "    runs-on: ubuntu-latest",
                "    if: false\n    runs-on: ubuntu-latest",
            ),
            encoding="utf-8",
        )
        self.assert_rejected("unconditional")

    def test_rejects_non_failing_clean_cut_command(self) -> None:
        workflow = self.root / ".github/workflows/tests.yml"
        workflow.write_text(
            workflow.read_text(encoding="utf-8").replace(
                "python3 tools/check_p5_clean_cut.py --check",
                "python3 tools/check_p5_clean_cut.py --check || true",
            ),
            encoding="utf-8",
        )
        self.assert_rejected("missing command")

    def test_rejects_non_fail_fast_clean_cut_script(self) -> None:
        workflow = self.root / ".github/workflows/tests.yml"
        workflow.write_text(
            workflow.read_text(encoding="utf-8").replace(
                "        run: |\n",
                "        run: |\n          set +e\n",
            ),
            encoding="utf-8",
        )
        self.assert_rejected("exact fail-fast script")

    def test_rejects_non_fail_fast_clean_cut_shell(self) -> None:
        workflow = self.root / ".github/workflows/tests.yml"
        workflow.write_text(
            workflow.read_text(encoding="utf-8").replace(
                "        shell: bash\n",
                "        shell: bash {0} || true\n",
            ),
            encoding="utf-8",
        )
        self.assert_rejected("exact fail-fast shell")


if __name__ == "__main__":
    unittest.main()
