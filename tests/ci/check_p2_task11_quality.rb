#!/usr/bin/env ruby
# frozen_string_literal: true

require "pathname"
require "json"
require "yaml"

ROOT = Pathname.new(__dir__).join("../..").expand_path
WORKFLOW = ROOT.join(".github/workflows/tests.yml")
ISSUE = ROOT.join("docs/issues/0002-portable-payload-foundation-implementation-status.md")
MALFORMED_CLASS_MAP = ROOT.join("tests/ci/p2_malformed_class_map.json")

class QualityError < StandardError; end

JOBS = {
  "python_tests" => %w[core python workflow],
  "ts_tests" => %w[core ts workflow],
  "native_tests" => %w[core workflow],
  "native_sanitizers" => %w[core workflow],
  "wasm_core" => %w[core workflow],
  "package_tests" => %w[core python workflow]
}.freeze

# Later phases may add independently validated jobs to the repository-wide
# aggregate, but this historical gate must still fail closed over the exact
# current inventory rather than silently accepting arbitrary extra jobs.
POST_P2_JOBS = {
  "rust_payload" => %w[core rust workflow],
  "projection_parity" => %w[core python rust ts workflow],
  "generated_projections" => %w[core python rust ts workflow]
}.freeze
POST_P2_AGGREGATE_JOBS = POST_P2_JOBS.keys.freeze

MALFORMED_CLASSES = %w[
  header-identity-version-profile-length-digest
  truncation
  directory-arithmetic
  region-overlap-gap-misalignment-order
  descriptor-size-flags-reserved
  validity-length-tail-null-storage
  boolean-and-nan-canonicality
  string-and-list-partitions
  utf8-and-utf16
  counts-limits-and-work
  allocation-failure
  backing-callbacks
  stale-generation
  active-access-drain
].freeze

def require_quality(condition, message)
  raise QualityError, message unless condition
end

def reject_duplicate_yaml_keys(node, label, path = "$")
  case node
  when Psych::Nodes::Mapping
    seen = {}
    node.children.each_slice(2) do |key_node, value_node|
      require_quality(key_node.is_a?(Psych::Nodes::Scalar),
                      "#{label} has a non-scalar YAML key at #{path}")
      key = key_node.value
      require_quality(!seen.key?(key),
                      "#{label} contains duplicate YAML key #{key.inspect} at #{path}")
      seen[key] = true
      reject_duplicate_yaml_keys(value_node, label, "#{path}/#{key}")
    end
  when Psych::Nodes::Sequence
    node.children.each_with_index do |child, index|
      reject_duplicate_yaml_keys(child, label, "#{path}/#{index}")
    end
  when Psych::Nodes::Stream, Psych::Nodes::Document
    node.children.each do |child|
      reject_duplicate_yaml_keys(child, label, path)
    end
  end
end

def self_test_yaml_duplicates
  reject_duplicate_yaml_keys(Psych.parse("root:\n  child: value\n"),
                             "self-test")
  begin
    reject_duplicate_yaml_keys(Psych.parse("root:\n  same: 1\n  same: 2\n"),
                               "self-test")
  rescue QualityError
    return
  end
  raise QualityError, "duplicate YAML self-test was accepted"
end

def expected_to_run?(job, scopes)
  JOBS.fetch(job).any? { |scope| scopes.fetch(scope) }
end

def validate_results(values)
  scopes = %w[core python ts workflow].to_h do |scope|
    value = values.fetch("#{scope.upcase}_SCOPE")
    require_quality(%w[true false].include?(value),
                    "#{scope} scope has unexpected value: #{value.inspect}")
    [scope, value == "true"]
  end
  require_quality(values.fetch("DETECT_RESULT") == "success",
                  "detect_changes did not succeed")

  JOBS.each_key do |job|
    expected = expected_to_run?(job, scopes) ? "success" : "skipped"
    actual = values.fetch("#{job.upcase}_RESULT")
    require_quality(actual == expected,
                    "#{job} result #{actual.inspect} does not match #{expected.inspect}")
  end
end

def require_results_rejected(values, description)
  begin
    validate_results(values)
  rescue KeyError, QualityError
    return
  end
  raise QualityError, "aggregate accepted #{description}"
end

def self_test_results
  scope_names = %w[core python ts workflow]
  result_values = %w[success skipped failure cancelled neutral timed_out
                     action_required stale startup_failure]
  (0...(1 << scope_names.length)).each do |bits|
    scopes = scope_names.each_with_index.to_h do |name, index|
      [name, (bits & (1 << index)).positive?]
    end
    values = {"DETECT_RESULT" => "success"}
    scopes.each { |name, active| values["#{name.upcase}_SCOPE"] = active.to_s }
    JOBS.each_key do |job|
      values["#{job.upcase}_RESULT"] =
        expected_to_run?(job, scopes) ? "success" : "skipped"
    end
    validate_results(values)

    JOBS.each_key do |job|
      key = "#{job.upcase}_RESULT"
      (result_values - [values.fetch(key)]).each do |wrong_result|
        wrong = values.merge(key => wrong_result)
        require_results_rejected(
          wrong,
          "#{job} result #{wrong_result.inspect} for scope bits #{bits}"
        )
      end
    end
  end


  baseline = {
    "DETECT_RESULT" => "success",
    "CORE_SCOPE" => "false",
    "PYTHON_SCOPE" => "false",
    "TS_SCOPE" => "false",
    "WORKFLOW_SCOPE" => "false"
  }
  JOBS.each_key { |job| baseline["#{job.upcase}_RESULT"] = "skipped" }
  (result_values - ["success"]).each do |wrong_result|
    require_results_rejected(
      baseline.merge("DETECT_RESULT" => wrong_result),
      "detect_changes result #{wrong_result.inspect}"
    )
  end
  %w[CORE_SCOPE PYTHON_SCOPE TS_SCOPE WORKFLOW_SCOPE].each do |scope|
    %w[TRUE FALSE unknown].each do |wrong_value|
      require_results_rejected(
        baseline.merge(scope => wrong_value),
        "#{scope} value #{wrong_value.inspect}"
      )
    end
  end
  self_test_yaml_duplicates
end

def expression_scopes(job)
  expression = job.fetch("if")
  expression.scan(/needs\.detect_changes\.outputs\.([a-z_]+)/).flatten.uniq.sort
end

def step_run(job, name)
  step = job.fetch("steps").find { |candidate| candidate["name"] == name }
  require_quality(!step.nil?, "missing step #{name.inspect}")
  step.fetch("run")
end

def check_workflow
  reject_duplicate_yaml_keys(Psych.parse_file(WORKFLOW), "workflow")
  workflow = YAML.load_file(WORKFLOW)
  jobs = workflow.fetch("jobs")
  detect = jobs.fetch("detect_changes")
  require_quality(detect.fetch("outputs").keys.sort ==
                    %w[core python rust ts workflow],
                  "detect_changes outputs are not exact")
  filter_step = detect.fetch("steps").find { |step| step["id"] == "filter" }
  require_quality(!filter_step.nil?, "detect_changes lacks its path filter")
  filter_source = filter_step.fetch("with").fetch("filters")
  reject_duplicate_yaml_keys(Psych.parse(filter_source), "workflow filters")
  filters = YAML.safe_load(filter_source)
  {
    "core" => %w[tools/check_emscripten_exception_flags.py
                 tools/check_payload_binary_corpus.py],
    "python" => %w[tools/check_python_package_inventory.py],
    "rust" => %w[bindings/rust/** tests/rust/**],
    "workflow" => %w[tests/ci/**]
  }.each do |scope, required_paths|
    missing_paths = required_paths - filters.fetch(scope)
    require_quality(missing_paths.empty?,
                    "#{scope} scope omits gate paths #{missing_paths.inspect}")
  end

  JOBS.each do |name, scopes|
    require_quality(jobs.key?(name), "missing workflow job #{name}")
    require_quality(expression_scopes(jobs.fetch(name)) == scopes.sort,
                    "#{name} path scope does not match #{scopes.sort.inspect}")
  end
  POST_P2_JOBS.each do |name, scopes|
    require_quality(jobs.key?(name), "missing post-P2 workflow job #{name}")
    require_quality(expression_scopes(jobs.fetch(name)) == scopes.sort,
                    "#{name} path scope does not match #{scopes.sort.inspect}")
  end

  rust_payload = jobs.fetch("rust_payload")
  require_quality(rust_payload.fetch("runs-on") == "ubuntu-24.04",
                  "rust_payload must use ubuntu-24.04")
  rust_source = step_run(rust_payload, "Run Rust source-link projection tests")
  require_quality(rust_source.include?(
                    "cargo test --manifest-path bindings/rust/Cargo.toml") &&
                  rust_source.include?("--workspace --all-features"),
                  "rust_payload lacks the full source-link projection suite")
  rust_lints = step_run(rust_payload, "Check Rust formatting and warnings")
  require_quality(rust_lints.include?(
                    "cargo fmt --manifest-path bindings/rust/Cargo.toml") &&
                  rust_lints.include?(
                    "cargo clippy --manifest-path bindings/rust/Cargo.toml") &&
                  rust_lints.include?("--workspace --all-targets --all-features") &&
                  rust_lints.include?("-D warnings"),
                  "rust_payload lacks the complete format/clippy gates")
  rust_configure = step_run(
    rust_payload, "Configure shared FastDB for relocated system linking"
  )
  require_quality(rust_configure.include?(
                    "cmake -S fastcarto -B build/rust-system") &&
                  rust_configure.include?("-DBUILD_TESTING=OFF") &&
                  rust_configure.include?("-DBUILD_TOOLS=OFF") &&
                  rust_configure.include?("-DCMAKE_BUILD_TYPE=Release"),
                  "rust_payload lacks the bounded shared-Core configuration")
  rust_system = step_run(
    rust_payload, "Build and link relocated Rust consumer to the system Core"
  )
  require_quality(rust_system.include?(
                    "cmake --build build/rust-system --target fastdb") &&
                  rust_system.include?(
                    "python3 tests/ci/test_check_rust_payload_package.py") &&
                  rust_system.include?(
                    "python3 tests/ci/check_rust_payload_package.py") &&
                  rust_system.include?("--build-dir build/rust-system"),
                  "rust_payload lacks the tested relocated system-link proof")

  projection = jobs.fetch("projection_parity")
  require_quality(projection.fetch("runs-on") == "ubuntu-24.04",
                  "projection_parity must use ubuntu-24.04")
  projection_gate = step_run(
    projection, "Validate ordered projection and closed-codegen map"
  )
  require_quality(projection_gate.include?(
                    "python3 tests/ci/test_check_p4_projection_codegen_quality.py") &&
                  projection_gate.include?(
                    "python3 tests/ci/check_p4_projection_codegen_quality.py") &&
                  projection_gate.include?("--check-repository"),
                  "projection_parity lacks the tested repository quality gate")

  native = jobs.fetch("native_tests")
  matrix = native.fetch("strategy").fetch("matrix").fetch("include")
  require_quality(matrix == [
                    {"os" => "ubuntu-24.04", "expected_arch" => "x86_64"},
                    {"os" => "macos-15", "expected_arch" => "arm64"}
                  ], "native matrix must be exact Linux x86-64/macOS arm64")
  require_quality(step_run(native, "Verify portable payload ABI symbols")
                    .include?("check_payload_abi_symbols.py"),
                  "native job must run the executable ABI gate")
  corpus_gate = step_run(native, "Verify reviewed binary-open corpus")
  require_quality(corpus_gate.include?("test_check_payload_binary_corpus.py") &&
                  corpus_gate.include?("tools/check_payload_binary_corpus.py --check"),
                  "native job must run the tested reviewed binary corpus gate")

  sanitizer = jobs.fetch("native_sanitizers")
  require_quality(sanitizer.fetch("runs-on") == "ubuntu-24.04",
                  "native_sanitizers must use ubuntu-24.04")
  require_quality(sanitizer.fetch("env") == {
                    "ASAN_OPTIONS" =>
                      "halt_on_error=1:abort_on_error=1:detect_leaks=1",
                    "UBSAN_OPTIONS" => "halt_on_error=1:print_stacktrace=1"
                  }, "native_sanitizers must hard-fail with leak detection")
  sanitizer_smoke = step_run(sanitizer,
                             "Run portable payload parser quality smokes")
  %w[fuzz_payload_spec_compile fuzz_payload_open
     tests/fuzz/payload/corpus tests/fuzz/payload/binary-corpus].each do |item|
    require_quality(sanitizer_smoke.include?(item),
                    "sanitizer parser quality smoke omits #{item}")
  end
  require_quality(sanitizer_smoke.scan("-runs=1000").length == 2 &&
                  sanitizer_smoke.scan("-max_len=1048576").length == 2 &&
                  sanitizer_smoke.scan("-artifact_prefix=").length == 2,
                  "both parser quality smokes must pin runs, size, and artifacts")
  sanitizer_audit = step_run(sanitizer,
                             "Reject sanitizer diagnostics and artifacts")
  require_quality(sanitizer_audit.include?("runtime error:") &&
                  sanitizer_audit.include?("ERROR: (AddressSanitizer|LeakSanitizer)") &&
                  sanitizer_audit.include?("SUMMARY: (AddressSanitizer|") &&
                  sanitizer_audit.include?("parser-artifacts"),
                  "sanitizer job lacks retained-log/artifact rejection")

  wasm = jobs.fetch("wasm_core")
  require_quality(wasm.fetch("runs-on") == "ubuntu-24.04",
                  "wasm_core must use ubuntu-24.04")
  wasm_build = step_run(wasm, "Build portable payload WASM proofs")
  %w[fastdb_payload_wasm_runtime_harness
     fastdb_payload_test_c_header_smoke
     fastdb_payload_test_runtime_cpp_facade
     fastdb_payload_wasm_runtime_abi_single_thread].each do |target|
    require_quality(wasm_build.include?(target),
                    "wasm_core does not build #{target}")
  end
  require_quality(step_run(wasm, "Run portable payload WASM proofs")
                  .include?("--single-thread-injected-failure"),
                  "wasm_core lacks the single-thread injected-failure mode")
  exception_gate = step_run(wasm, "Verify Emscripten exception propagation")
  require_quality(exception_gate.include?("test_check_emscripten_exception_flags.py") &&
                  exception_gate.include?("tools/check_emscripten_exception_flags.py"),
                  "wasm_core lacks tested structural exception flag inspection")
  require_quality(step_run(wasm, "Verify portable payload WASM ABI symbols")
                  .include?("check_payload_abi_symbols.py --wasm-build-dir"),
                  "wasm_core lacks the exact WASM ABI gate")

  package = jobs.fetch("package_tests")
  require_quality(package.fetch("runs-on") == "ubuntu-24.04",
                  "package_tests must use ubuntu-24.04")
  package_strategy = package.fetch("strategy")
  require_quality(package_strategy.fetch("fail-fast") == false &&
                  package_strategy.fetch("matrix").fetch("python-version") ==
                    %w[3.10 3.12],
                  "package_tests must cover exact Python 3.10/3.12 matrix")
  package_build = step_run(package, "Build Python sdist and wheel")
  require_quality(package_build.include?("set -o pipefail") &&
                  package_build.include?("tee build/package-build.log"),
                  "package job must preserve the build diagnostics")
  package_verify = step_run(package, "Verify Python package inventories")
  require_quality(package_verify.include?("check_python_package_inventory.py") &&
                  package_verify.include?("--build-log build/package-build.log"),
                  "package job lacks the executable exact inventory gate")
  require_quality(package_verify.include?("test_check_python_package_inventory.py"),
                  "package job lacks focused package-gate tests")
  installed_wheel = step_run(package,
                             "Run payload suite from the installed wheel")
  require_quality(installed_wheel.include?(
                    'wheels=(build/package-dist/*.whl)') &&
                  installed_wheel.include?('test "${#wheels[@]}" -eq 1') &&
                  installed_wheel.include?("uv run --isolated --no-project") &&
                  installed_wheel.include?('--python "${{ matrix.python-version }}"') &&
                  installed_wheel.include?('--with "${wheels[0]}" --with pytest') &&
                  installed_wheel.include?(
                    "python -m pytest tests/python/payload -q"),
                  "package job lacks the exact installed-wheel payload suite")

  ts_package = step_run(jobs.fetch("ts_tests"),
                        "Verify packed portable-payload subpath")
  require_quality(ts_package.include?(
                    "python3 tests/ci/test_check_ts_payload_package.py") &&
                  ts_package.include?("npm pack ./ts/fastdb4ts") &&
                  ts_package.include?("--pack-destination build/ts-package") &&
                  ts_package.include?(
                    "python3 tests/ci/check_ts_payload_package.py") &&
                  ts_package.include?("--package-dir build/ts-package"),
                  "ts_tests lacks the tested packed payload-package proof")

  aggregate = jobs.fetch("test")
  required_needs = ["detect_changes", *JOBS.keys,
                    *POST_P2_AGGREGATE_JOBS].sort
  require_quality(Array(aggregate.fetch("needs")).sort == required_needs,
                  "aggregate needs are not exact")
  aggregate_run = step_run(aggregate, "Validate required test results")
  aggregate_step = aggregate.fetch("steps").find do |step|
    step["name"] == "Validate required test results"
  end
  aggregate_env = aggregate_step.fetch("env")
  {
    "RUST_SCOPE" => "${{ needs.detect_changes.outputs.rust }}",
    "RUST_PAYLOAD_RESULT" => "${{ needs.rust_payload.result }}",
    "PROJECTION_PARITY_RESULT" => "${{ needs.projection_parity.result }}"
  }.each do |name, expected|
    require_quality(aggregate_env.fetch(name) == expected,
                    "aggregate #{name} wiring is not exact")
  end
  require_quality(aggregate_run.include?(
                    "check_p4_projection_codegen_quality.py") &&
                  aggregate_run.include?("--validate-results"),
                  "aggregate does not use the P4 result validator")
  require_quality(aggregate_run.include?("check_p2_task11_quality.rb --validate-results"),
                  "aggregate does not use the tested result validator")

  allowlist = ROOT.join("tests/abi/fastdb_payload_v1_symbols.txt")
                  .read.lines(chomp: true)
  p3_additions = %w[
    fdb_payload_v1_builder_object_declare
    fdb_payload_v1_builder_object_fill_begin
    fdb_payload_v1_builder_value_object
    fdb_payload_v1_builder_value_ref
    fdb_payload_v1_view_graph_identity
    fdb_payload_v1_view_ref_target
  ]
  p4_additions = %w[
    fdb_payload_v1_builder_require_spec_sha256
    fdb_payload_v1_codegen_options_init
    fdb_payload_v1_codegen_result_artifact_bytes
    fdb_payload_v1_codegen_result_artifact_count
    fdb_payload_v1_codegen_result_artifact_kind
    fdb_payload_v1_codegen_result_artifact_relative_path
    fdb_payload_v1_codegen_result_artifact_sha256
    fdb_payload_v1_codegen_result_release
    fdb_payload_v1_codegen_result_retain
    fdb_payload_v1_payload_require_spec_sha256
    fdb_payload_v1_spec_codegen
    fdb_payload_v1_view_require_spec_sha256
  ]
  p2_symbols = allowlist - p3_additions - p4_additions
  require_quality(allowlist.length == 117 &&
                  allowlist == allowlist.uniq.sort &&
                  (p3_additions - allowlist).empty? &&
                  (p4_additions - allowlist).empty? &&
                  p2_symbols.length == 99,
                  "portable payload ABI allowlist must preserve the 99 P2 symbols plus the exact P3 and P4 additive families")
end

def check_documentation
  issue = ISSUE.read
  required = [
    "## P2 requirement-to-test traceability",
    "https://emscripten.org/docs/porting/exceptions.html",
    "single-thread WebAssembly runtime-ABI",
    "Hosted results remain pending",
    "Object-graph runtime remains P3",
    "exact seven-diagnostic SWIG baseline",
    "Task 11's complete fresh local gate is green",
    "### Hosted workflow evidence",
    "| Accepted requirement | Implementation authority | Exact proof |"
  ]
  missing = required.reject { |text| issue.include?(text) }
  require_quality(missing.empty?,
                  "Issue 0002 lacks Task 11 truth: #{missing.join(', ')}")
  proof_names = %w[
    test_runtime_ids_reachability_and_component_layout
    test_builder_consumes_the_same_runtime_schema_ids
    test_record_batch_components_and_lists
    test_fixed_runs_and_transactional_failures
    test_region_matrix_zero_boundaries_and_partition_rules
    test_open_preflights_static_spec_limits_and_known_work
    test_lazy_selected_span_work_boundary
    test_manifest_indexes_facts_and_capabilities
    test_repeatable_concurrent_and_reentrant_execution
    test_checked_view_access_materialize_and_barrier_abi
  ]
  missing_proofs = proof_names.reject { |name| issue.include?(name) }
  require_quality(missing_proofs.empty?,
                  "Issue 0002 omits exact P2 proofs: #{missing_proofs.join(', ')}")
  require_quality(issue.include?("detect_leaks=0") &&
                  issue.include?("detect_leaks=1"),
                  "Issue 0002 must distinguish local macOS and hosted Linux leak gates")
  stale_guidance =
    "public consumer guidance, proof\nmapping, and final independent review remain open"
  require_quality(!issue.include?(stale_guidance),
                  "Issue 0002 retains the stale Task 11 guidance/proof status")

  readme = ROOT.join("README.md").read
  core_readme = ROOT.join("fastcarto/README.md").read
  schemas = ROOT.join("schemas/README.md").read
  changelog = ROOT.join("CHANGELOG.md").read
  index = ROOT.join("docs/issues/README.md").read
  compact = ->(text) { text.gsub(/\s+/, " ") }
  require_quality(compact.call(readme).include?("exactly 99 `fdb_payload_v1_*` exports"),
                  "root README does not state the exact P2 ABI")
  require_quality(compact.call(core_readme).include?("Emscripten exception model"),
                  "Core README lacks source-build exception guidance")
  require_quality(compact.call(schemas).include?("P2 requirement-to-test traceability"),
                  "schema README does not link the P2 traceability map")
  require_quality(compact.call(changelog).include?("P2 record binary/runtime/lifetime"),
                  "changelog lacks the P2 local implementation entry")
  issue_row = index.lines.find { |line| line.include?("[0002]") }
  require_quality(!issue_row.nil? && issue_row.include?("Open") &&
                  issue_row.include?("P2") &&
                  issue_row.match?(/locally (?:complete\/)?frozen/) &&
                  issue_row.include?("hosted evidence pending"),
                  "issue index overstates or omits the frozen P2/local-hosted state")
end

def check_malformed_class_map
  document = JSON.parse(MALFORMED_CLASS_MAP.read)
  require_quality(document.fetch("schema") ==
                    "fastdb.payload.p2-malformed-class-map.v1",
                  "unexpected malformed-class map schema")
  rows = document.fetch("classes")
  classes = rows.map { |row| row.fetch("class") }
  require_quality(classes == MALFORMED_CLASSES,
                  "malformed-class map does not match the mandatory order")
  rows.each do |row|
    proofs = row.fetch("proofs")
    require_quality(!proofs.empty?, "#{row.fetch('class')} has no exact proof")
    proofs.each do |proof|
      relative = proof.fetch("file")
      test = proof.fetch("test")
      source = ROOT.join(relative).read
      require_quality(source.match?(/\bint\s+#{Regexp.escape(test)}\s*\(/),
                      "#{relative} does not define #{test}")
      require_quality(source.scan(/\b#{Regexp.escape(test)}\s*\(/).length >= 2,
                      "#{relative} does not execute #{test} from its test entry")
    end
  end
end

def check_repository
  check_workflow
  check_documentation
  check_malformed_class_map
  self_test_results
end

begin
  case ARGV.fetch(0, "--check-repository")
  when "--check-repository"
    check_repository
  when "--self-test-results"
    self_test_results
  when "--validate-results"
    validate_results(ENV)
  else
    raise QualityError, "unknown mode #{ARGV[0].inspect}"
  end
rescue KeyError, QualityError => error
  warn "P2 Task 11 quality check failed: #{error.message}"
  exit 1
end

puts "P2 Task 11 quality check passed"
