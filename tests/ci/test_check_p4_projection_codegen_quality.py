#!/usr/bin/env python3
"""Unit tests for the P4 projection/codegen quality gate."""

from __future__ import annotations

import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("check_p4_projection_codegen_quality.py")
SPEC = importlib.util.spec_from_file_location("p4_projection_quality", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def proof(language: str) -> dict[str, object]:
    names = {
        "cpp": ["cpp_receipt"],
        "rust": ["rust_receipt"],
        "python": ["test_python_receipt"],
        "typescript": ["typescript receipt"],
    }
    return {
        "language": language,
        "cases": [
            {
                "file": f"{language}.source",
                "test": names[language][0],
                "markers": [f"{language}-marker"],
            }
        ],
    }


def codegen_proofs(target: str) -> list[dict[str, object]]:
    return [
        {
            "id": proof_id,
            "file": f"{target}-{proof_id}.source",
            "language": "python",
            "test": f"test_{target}_{proof_id.replace('-', '_')}",
            "markers": [f"{target}-{proof_id}-marker"],
        }
        for proof_id in MODULE.CODEGEN_PROOF_IDS
    ]


def closure_requirements() -> list[dict[str, object]]:
    return [
        {
            "id": requirement_id,
            "status": "closed",
            "proofs": [
                {
                    "file": f"closure-{requirement_id}.source",
                    "language": "python",
                    "test": f"check_{requirement_id.replace('-', '_')}",
                    "markers": [f"{requirement_id}-marker"],
                }
            ],
        }
        for requirement_id in MODULE.CLOSURE_REQUIREMENT_IDS
    ]


def valid_document() -> dict[str, object]:
    return {
        "schema": MODULE.SCHEMA,
        "authority": {
            "semantic_authority": "cpp-core",
            "projection_rule": "abi-only-no-binding-semantics",
        },
        "abi": {
            "version": 1,
            "symbol_count": 117,
            "allowlist": "tests/abi/fastdb_payload_v1_symbols.txt",
            "status": "frozen-p4",
        },
        "runtime_receipts": [
            {
                "id": receipt_id,
                "status": "closed-task-6",
                "fixtures": [
                    "tests/golden/payload/v1/spec/valid/record-all-types.source.json"
                ],
                "observations": list(MODULE.OBSERVATIONS[receipt_id]),
                "proofs": [proof(language) for language in MODULE.LANGUAGES],
            }
            for receipt_id in MODULE.RUNTIME_IDS
        ],
        "package_boundaries": [
            {
                "id": package_id,
                "status": "local-executable",
                "proofs": [
                    {"file": f"{package_id}.source", "marker": "package-marker"}
                ],
                "workflow_markers": [f"run-{package_id}"],
            }
            for package_id in MODULE.PACKAGE_IDS
        ],
        "workflow": {
            "file": ".github/workflows/tests.yml",
            "jobs": list(MODULE.WORKFLOW_JOBS),
            "definition_status": "definitions-only",
            "hosted_status": "pending",
        },
        "codegen": [
            {
                "target": target,
                "status": "closed-p4",
                "proofs": codegen_proofs(target),
            }
            for target in MODULE.CODEGEN_TARGETS
        ],
        "closure": {
            "status": "local-complete-task-9",
            "requirements": closure_requirements(),
            "review": "primary-agent-not-independent",
            "hosted_status": "pending",
            "p5_status": "open-clean-cut",
        },
    }


class ProjectionMapTests(unittest.TestCase):
    def test_requires_final_p4_closure_contract(self) -> None:
        self.assertEqual(
            MODULE.SCHEMA,
            "fastdb.payload.p4-projection-codegen-map.v2",
        )
        self.assertEqual(
            MODULE.CLOSURE_REQUIREMENT_IDS,
            (
                "core-authority",
                "runtime-projection-parity",
                "generated-target-execution",
                "artifact-determinism-provenance-hash",
                "abi-manifest-truth",
                "package-workflow",
                "hostile-codegen-robustness",
                "documentation-handoff",
            ),
        )
        self.assertIn("closure", MODULE.TOP_LEVEL_KEYS)

    def test_rejects_duplicate_json_keys(self) -> None:
        self.assertEqual(
            MODULE.load_json_no_duplicates('{"first":1,"second":2}', "fixture"),
            {"first": 1, "second": 2},
        )
        with self.assertRaises(MODULE.QualityError):
            MODULE.load_json_no_duplicates('{"same":1,"same":2}', "fixture")

    def test_accepts_exact_ordered_contract(self) -> None:
        MODULE.check_map(valid_document())

    def test_rejects_missing_duplicate_or_reordered_runtime_receipts(self) -> None:
        for mutate in (
            lambda rows: rows.pop(),
            lambda rows: rows.__setitem__(1, copy.deepcopy(rows[0])),
            lambda rows: rows.__setitem__(slice(0, 2), list(reversed(rows[:2]))),
        ):
            with self.subTest(mutate=mutate):
                document = valid_document()
                mutate(document["runtime_receipts"])
                with self.assertRaises(MODULE.QualityError):
                    MODULE.check_map(document)

    def test_rejects_noncanonical_or_escaping_repository_paths(self) -> None:
        for invalid in (
            "/absolute/source.py",
            "../outside.py",
            "tests/../outside.py",
            "tests\\outside.py",
            "tests//outside.py",
        ):
            document = valid_document()
            document["runtime_receipts"][0]["proofs"][0]["cases"][0][
                "file"
            ] = invalid
            with self.subTest(invalid=invalid):
                with self.assertRaises(MODULE.QualityError):
                    MODULE.check_map(document)

    def test_rejects_repository_symlink_escaping_root(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            repository = temporary_root / "repository"
            repository.mkdir()
            inside = repository / "inside.source"
            outside = temporary_root / "outside.source"
            inside.write_text("inside", encoding="utf-8")
            outside.write_text("outside", encoding="utf-8")
            (repository / "escape.source").symlink_to(outside)

            previous_root = MODULE.ROOT
            MODULE.ROOT = repository.resolve()
            try:
                self.assertEqual(
                    MODULE.repository_file("inside.source", "inside proof"),
                    inside.resolve(),
                )
                with self.assertRaises(MODULE.QualityError):
                    MODULE.repository_file("escape.source", "escaping proof")
            finally:
                MODULE.ROOT = previous_root

    def test_rejects_missing_duplicate_or_reordered_language_evidence(self) -> None:
        document = valid_document()
        proofs = document["runtime_receipts"][0]["proofs"]
        MODULE.check_map(document)
        for invalid in (
            proofs[:-1],
            [proofs[0], proofs[0], proofs[2], proofs[3]],
            [proofs[1], proofs[0], proofs[2], proofs[3]],
        ):
            changed = copy.deepcopy(document)
            changed["runtime_receipts"][0]["proofs"] = invalid
            with self.assertRaises(MODULE.QualityError):
                MODULE.check_map(changed)

        changed = copy.deepcopy(document)
        cases = changed["runtime_receipts"][0]["proofs"][0]["cases"]
        cases.append(copy.deepcopy(cases[0]))
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_map(changed)

    def test_rejects_missing_or_unexecutable_source_evidence(self) -> None:
        document = valid_document()
        sources = {
            "cpp.source": "int cpp_receipt() { /* cpp-marker */ return 0; }\nint main() { return cpp_receipt(); }",
            "rust.source": "#[test]\nfn rust_receipt() { /* rust-marker */ }",
            "python.source": "def test_python_receipt():\n    pass  # python-marker",
            "typescript.source": "test('typescript receipt', () => { /* typescript-marker */ });",
        }
        for package_id in MODULE.PACKAGE_IDS:
            sources[f"{package_id}.source"] = "package-marker"
        reader = sources.__getitem__
        MODULE.check_proof_sources(document, reader)
        MODULE.check_package_sources(document, reader)

        sources["rust.source"] = "rust-marker"
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_proof_sources(document, reader)

    def test_rejects_marker_from_an_unrelated_test(self) -> None:
        for language, source in {
            "cpp": "int cpp_receipt() { return 0; }\nint test_other() { /* cpp-marker */ return 0; }\nint main() { return cpp_receipt(); }",
            "rust": "#[test]\nfn rust_receipt() {}\n#[test]\nfn other() { /* rust-marker */ }",
            "python": "def test_python_receipt():\n    pass\ndef test_other():\n    pass  # python-marker",
            "typescript": "test('typescript receipt', () => {});\ntest('other', () => { /* typescript-marker */ });",
        }.items():
            document = valid_document()
            readers = {
                f"{candidate}.source": (
                    source
                    if candidate == language
                    else {
                        "cpp": "int cpp_receipt() { /* cpp-marker */ return 0; }\nint main() { return cpp_receipt(); }",
                        "rust": "#[test]\nfn rust_receipt() { /* rust-marker */ }",
                        "python": "def test_python_receipt():\n    pass  # python-marker",
                        "typescript": "test('typescript receipt', () => { /* typescript-marker */ });",
                    }[candidate]
                )
                for candidate in MODULE.LANGUAGES
            }
            for package_id in MODULE.PACKAGE_IDS:
                readers[f"{package_id}.source"] = "package-marker"
            with self.subTest(language=language):
                with self.assertRaises(MODULE.QualityError):
                    MODULE.check_proof_sources(document, readers.__getitem__)

    def test_rejects_incomplete_reordered_or_fabricated_codegen_rows(self) -> None:
        for mutate in (
            lambda rows: rows[0].__setitem__("status", "open"),
            lambda rows: rows[0]["proofs"].pop(),
            lambda rows: rows[0]["proofs"][0].__setitem__("id", "invented"),
            lambda rows: rows.__setitem__(slice(0, 2), list(reversed(rows[:2]))),
        ):
            document = valid_document()
            mutate(document["codegen"])
            with self.assertRaises(MODULE.QualityError):
                MODULE.check_map(document)

    def test_codegen_sources_are_executable_and_marker_scoped(self) -> None:
        document = valid_document()
        sources: dict[str, str] = {}
        for row in document["codegen"]:
            for proof in row["proofs"]:
                name = proof["test"]
                marker = proof["markers"][0]
                invocation = (
                    f"\n{name}()\n"
                    if proof["id"] in {"generated-execution", "hostile-execution"}
                    else ""
                )
                sources[proof["file"]] = (
                    f"def {name}():\n    pass  # {marker}{invocation}"
                )
        MODULE.check_codegen_sources(document, sources.__getitem__)

        first = document["codegen"][0]["proofs"][0]
        sources[first["file"]] = f"def {first['test']}():\n    pass"
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_codegen_sources(document, sources.__getitem__)

    def test_closure_sources_are_scoped_and_invoked(self) -> None:
        document = valid_document()
        sources: dict[str, str] = {}
        for requirement in document["closure"]["requirements"]:
            proof = requirement["proofs"][0]
            name = proof["test"]
            marker = proof["markers"][0]
            sources[proof["file"]] = (
                f"def {name}():\n    pass  # {marker}\n\n{name}()\n"
            )
        MODULE.check_closure_sources(document, sources.__getitem__)

        first = document["closure"]["requirements"][0]["proofs"][0]
        sources[first["file"]] = f"def {first['test']}():\n    pass\n"
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_closure_sources(document, sources.__getitem__)

        sources[first["file"]] = (
            f"def {first['test']}():\n    pass  # {first['markers'][0]}\n"
        )
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_closure_sources(document, sources.__getitem__)

    def test_rejects_incomplete_reordered_or_false_closure(self) -> None:
        for mutate in (
            lambda closure: closure["requirements"].pop(),
            lambda closure: closure["requirements"].__setitem__(
                slice(0, 2),
                list(reversed(closure["requirements"][:2])),
            ),
            lambda closure: closure.__setitem__("review", "independent"),
            lambda closure: closure.__setitem__("hosted_status", "passed"),
            lambda closure: closure.__setitem__("p5_status", "complete"),
        ):
            document = valid_document()
            mutate(document["closure"])
            with self.assertRaises(MODULE.QualityError):
                MODULE.check_map(document)

    def test_rejects_incomplete_package_or_workflow_inventory(self) -> None:
        document = valid_document()
        document["package_boundaries"].pop()
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_map(document)

        document = valid_document()
        workflow = "  rust_payload:\n  projection_parity:\n"
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_workflow(document, workflow)

        workflow = "\n".join(
            [
                "  rust_payload:",
                "  projection_parity:",
                "  generated_projections:",
                *(f"- '{path}'" for path in MODULE.WORKFLOW_QUALITY_PATHS),
                *(
                    marker
                    for row in document["package_boundaries"]
                    for marker in row["workflow_markers"]
                ),
                "check_p4_projection_codegen_quality.py --check-repository",
                "check_p4_projection_codegen_quality.py --validate-results",
                "test_run_generated_payload_projections.py",
                "run_generated_payload_projections.py",
                "FASTDB_PAYLOAD_LIBRARY",
                "npm --prefix ts/fastdb4ts run build:wasm",
                "fastdb_payload_test_codegen_c_abi.js",
                "fastdb_payload_test_cpp_facade.js",
                "simple and hostile generated projections",
            ]
        )
        MODULE.check_workflow(document, workflow)
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_workflow(
                document,
                workflow.replace(
                    f"- '{MODULE.WORKFLOW_QUALITY_PATHS[0]}'", ""
                ),
            )
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_workflow(
                document,
                workflow.replace(
                    "check_p4_projection_codegen_quality.py --validate-results",
                    "missing aggregate validator",
                ),
            )

    def test_validates_path_aware_workflow_results(self) -> None:
        scope_names = ("CORE", "RUST", "PYTHON", "TS", "WORKFLOW")
        for bits in range(1 << len(scope_names)):
            active = {
                name: bool(bits & (1 << index))
                for index, name in enumerate(scope_names)
            }
            environment = {
                f"{name}_SCOPE": str(enabled).lower()
                for name, enabled in active.items()
            }
            environment["RUST_PAYLOAD_RESULT"] = (
                "success"
                if active["CORE"] or active["RUST"] or active["WORKFLOW"]
                else "skipped"
            )
            environment["PROJECTION_PARITY_RESULT"] = (
                "success" if any(active.values()) else "skipped"
            )
            environment["GENERATED_PROJECTIONS_RESULT"] = (
                "success" if any(active.values()) else "skipped"
            )
            MODULE.check_workflow_results(environment)

            for key in (
                "RUST_PAYLOAD_RESULT",
                "PROJECTION_PARITY_RESULT",
                "GENERATED_PROJECTIONS_RESULT",
            ):
                wrong = dict(environment)
                wrong[key] = (
                    "skipped" if environment[key] == "success" else "success"
                )
                with self.assertRaises(MODULE.QualityError):
                    MODULE.check_workflow_results(wrong)

        invalid_scope = dict(environment)
        invalid_scope["CORE_SCOPE"] = "unknown"
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_workflow_results(invalid_scope)

    def test_rejects_false_hosted_or_independent_claims(self) -> None:
        document = valid_document()
        document["workflow"]["hosted_status"] = "passed"
        with self.assertRaises(MODULE.QualityError):
            MODULE.check_map(document)

        accepted = "\n".join(MODULE.ISSUE_MARKERS)
        MODULE.check_issue_truth(accepted)
        for false_claim in (
            "Hosted Task 6 execution passed",
            "Hosted projection parity passed",
            "Task 6 independent review",
        ):
            with self.assertRaises(MODULE.QualityError):
                MODULE.check_issue_truth(f"{accepted}\n{false_claim}")

    def test_current_documentation_accepts_truthful_p5_closure(self) -> None:
        sources = {
            relative: "\n".join(markers)
            for relative, markers in MODULE.DOCUMENTATION_MARKERS.items()
        }
        sources["README.md"] = sources["README.md"].replace(
            "P5 clean cut remains open",
            "P5 local clean cut is complete at exact ABI-117.",
        )
        self.assertNotIn("P5 clean cut remains open", sources["README.md"])
        self.assertIn(
            "P5 local clean cut is complete at exact ABI-117.",
            sources["README.md"],
        )

        MODULE.check_documentation(sources.__getitem__)

    def test_rejects_abi_count_order_or_shape_drift(self) -> None:
        exact = [f"fdb_payload_v1_symbol_{index:03d}" for index in range(117)]
        MODULE.check_abi_allowlist("\n".join(exact))
        for invalid in (exact[:-1], list(reversed(exact)), exact[:-1] + ["bad"]):
            with self.assertRaises(MODULE.QualityError):
                MODULE.check_abi_allowlist("\n".join(invalid))


if __name__ == "__main__":
    unittest.main()
