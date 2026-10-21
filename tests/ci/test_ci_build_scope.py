#!/usr/bin/env python3
"""Exercise build allocation and publication guards from the actual workflows."""
import fnmatch
import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]


def yaml_document(path):
    # Ruby/Psych is also used by the repository's CI contract checker. Keep
    # these tooling tests independent of Python package/native dependencies.
    return json.loads(subprocess.check_output(
        ["ruby", "-rjson", "-ryaml", "-e", "puts JSON.generate(YAML.load_file(ARGV.fetch(0)))", str(path)],
        text=True))


def path_filters(workflow, job):
    steps = workflow["jobs"][job]["steps"]
    filters = next(step["with"]["filters"] for step in steps if step.get("id") == "filter")
    return json.loads(subprocess.check_output(
        ["ruby", "-rjson", "-ryaml", "-e", "puts JSON.generate(YAML.safe_load(STDIN.read))"],
        input=filters, text=True))


def matching_scopes(filters, changed):
    return {scope for scope, patterns in filters.items()
            if any(fnmatch.fnmatchcase(path, pattern) for path in changed for pattern in patterns)}


class CIBuildScopeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tests = yaml_document(ROOT / ".github/workflows/tests.yml")
        cls.artifacts = yaml_document(ROOT / ".github/workflows/release-artifacts.yml")
        cls.publish = yaml_document(ROOT / ".github/workflows/pypi_publish.yml")
        cls.python = yaml_document(ROOT / ".github/workflows/python-distributions.yml")
        cls.test_filters = path_filters(cls.tests, "detect_changes")
        cls.artifact_filters = path_filters(cls.artifacts, "metadata")

    def test_release_tools_and_documentation_allocate_no_native_builds(self):
        for path in (
            ".github/workflows/pypi_publish.yml", ".github/workflows/python-distributions.yml",
            ".github/workflows/release-artifacts.yml", "tests/ci/python_distributions.py",
            "tests/ci/publish_release.py", "tests/ci/release_artifacts.py",
            "tests/ci/test_python_release_reuse.py", "tests/ci/test_ci_build_scope.py",
            "README.md", "AGENTS.md", "CHANGELOG.md", "docs/releases/publishing.md",
        ):
            with self.subTest(path=path):
                self.assertEqual(matching_scopes(self.test_filters, [path]), {"release"})
                self.assertEqual(matching_scopes(self.artifact_filters, [path]), set())

    def test_runtime_ci_changes_still_require_runtime_validation(self):
        for path in (".github/workflows/tests.yml", "tests/ci/check_p2_task11_quality.rb",
                     "tests/ci/test_check_payload_abi_symbols.py", "tests/ci/p4_projection_codegen_map.json"):
            with self.subTest(path=path):
                self.assertIn("workflow", matching_scopes(self.test_filters, [path]))
                self.assertEqual(matching_scopes(self.artifact_filters, [path]), set())

    def test_package_sources_and_versions_still_build_release_candidates(self):
        for path, scope in (("fastcarto/fastdb/src/payload.cpp", "core"),
                            ("python/fastdb4py/payload.py", "python"),
                            ("bindings/rust/fastdb/Cargo.toml", "rust"),
                            ("ts/fastdb4ts/package.json", "ts"), ("pyproject.toml", "core")):
            with self.subTest(path=path):
                self.assertIn(scope, matching_scopes(self.test_filters, [path]))
                self.assertEqual(matching_scopes(self.artifact_filters, [path]), {"packages"})

    def test_only_main_source_pushes_build_candidates(self):
        jobs = self.artifacts["jobs"]
        gate = jobs["metadata"]["outputs"]["build"]
        self.assertIn("github.event_name == 'push'", gate)
        self.assertIn("steps.filter.outputs.packages == 'true'", gate)
        for name in ("native", "python", "typescript", "candidate"):
            with self.subTest(job=name):
                self.assertIn("metadata", jobs[name]["needs"])
                self.assertEqual(jobs[name]["if"], "needs.metadata.outputs.build == 'true'")

    def test_pr_path_filters_can_read_changed_files(self):
        for workflow, job in ((self.tests, "detect_changes"), (self.artifacts, "metadata")):
            with self.subTest(job=job):
                self.assertEqual(workflow["jobs"][job]["permissions"]["pull-requests"], "read")

    def test_lightweight_release_checks_are_a_required_gate(self):
        jobs = self.tests["jobs"]
        self.assertEqual(jobs["release_tools"]["if"], "needs.detect_changes.outputs.release == 'true'")
        self.assertIn("release_tools", jobs["test"]["needs"])
        aggregation = next(step for step in jobs["test"]["steps"] if "RELEASE_TOOLS_RESULT" in step.get("env", {}))
        self.assertEqual(aggregation["env"]["RELEASE_TOOLS_RESULT"], "${{ needs.release_tools.result }}")
        commands = "\n".join(step.get("run", "") for step in jobs["release_tools"]["steps"])
        for script in ("test_python_release_reuse.py", "test_ci_build_scope.py", "check_p2_task11_quality.rb"):
            self.assertIn(script, commands)

    def test_publish_uses_exact_cross_run_artifact_and_never_the_builder(self):
        jobs = self.publish["jobs"]
        self.assertEqual(jobs["distributions"]["if"], "needs.source.outputs.operation == 'backfill'")
        self.assertTrue(jobs["distributions"]["with"]["backfill"])
        downloads = [step for step in jobs["prepare"]["steps"] if step.get("uses", "").startswith("actions/download-artifact@")]
        reuse = next(step for step in downloads if step["if"] == "needs.source.outputs.operation == 'publish'")
        self.assertEqual(reuse["with"]["run-id"], "${{ needs.source.outputs.artifact_run_id }}")
        self.assertEqual(reuse["with"]["artifact-ids"], "${{ needs.source.outputs.python_artifact_id }}")
        self.assertEqual(reuse["with"]["github-token"], "${{ github.token }}")
        self.assertIn("always()", jobs["prepare"]["if"])
        self.assertIn("needs.source.outputs.operation == 'publish' ||", jobs["prepare"]["if"])
        proof = next(step for step in jobs["source"]["steps"] if step.get("id") == "proof")
        self.assertIn("--require-artifact", proof["run"])

    def test_selective_plan_controls_builders_and_empty_backfill_stops(self):
        jobs = self.python["jobs"]
        self.assertEqual(jobs["wheels"]["if"], "needs.plan.outputs.pending == 'true'")
        self.assertEqual(jobs["wheels"]["strategy"]["matrix"], "${{ fromJSON(needs.plan.outputs.matrix) }}")
        wheel_build = next(step for step in jobs["wheels"]["steps"] if step.get("uses", "").startswith("pypa/cibuildwheel@"))
        self.assertEqual(wheel_build["env"]["CIBW_BUILD"], "${{ matrix.build }}")
        self.assertEqual(jobs["sdist"]["if"], "needs.plan.outputs.build_sdist == 'true'")
        self.assertIn("needs.plan.outputs.pending == 'true'", jobs["inventory"]["if"])
        self.assertIn("needs.distributions.outputs.pending == 'true'", self.publish["jobs"]["prepare"]["if"])
        condition = self.publish["jobs"]["publish"]["if"]
        self.assertIn("always()", condition)
        self.assertIn("needs.prepare.result == 'success'", condition)
        self.assertIn("needs.prepare.outputs.pending == 'true'", condition)

    def test_release_matrix_and_local_validation_share_the_same_policy(self):
        import python_distributions as distributions
        policy = self.python["env"]
        self.assertEqual(policy["CIBW_BUILD"], distributions.DEFAULT_BUILD)
        self.assertEqual(set(policy["FASTDB_WHEEL_PLATFORMS"].split()), set(distributions.DEFAULT_PLATFORMS.split()))
        runners = json.loads(policy["FASTDB_WHEEL_RUNNERS"])
        self.assertEqual(set(runners), set(distributions.DEFAULT_PLATFORMS.split()))
        self.assertEqual(runners["manylinux_aarch64"]["os"], "ubuntu-24.04-arm")
        self.assertEqual(runners["macosx_x86_64"], {"os": "macos-15", "arch": "x86_64"})
        self.assertEqual(runners["win_arm64"], {"os": "windows-11-arm", "arch": "ARM64"})
        self.assertIn("cp310-win_arm64", policy["CIBW_SKIP"].split())

    def test_windows_arm64_core_has_native_rust_compiler_and_abi_validation(self):
        job = self.artifacts["jobs"]["native"]
        targets = job["strategy"]["matrix"]["include"]
        arm = next(item for item in targets if item["target"] == "aarch64-pc-windows-msvc")
        self.assertEqual(arm["os"], "windows-11-arm")
        self.assertEqual(arm["msvc_arch"], "arm64")
        self.assertEqual(arm["part"], "core-windows-arm64")
        commands = "\n".join(step.get("run", "") for step in job["steps"] if step.get("if") == "runner.os == 'Windows'")
        for required in ("PROCESSOR_ARCHITECTURE", "rustc -vV", "CMAKE_GENERATOR=Ninja", "check_payload_abi_symbols.py", "cargo test", "check_rust_payload_package.py"):
            self.assertIn(required, commands)


if __name__ == "__main__":
    unittest.main()
