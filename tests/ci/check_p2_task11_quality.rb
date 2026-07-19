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
  workflow = YAML.load_file(WORKFLOW)
  jobs = workflow.fetch("jobs")
  detect = jobs.fetch("detect_changes")
  filter_step = detect.fetch("steps").find { |step| step["id"] == "filter" }
  require_quality(!filter_step.nil?, "detect_changes lacks its path filter")
  filters = YAML.safe_load(filter_step.fetch("with").fetch("filters"))
  {
    "core" => %w[tools/check_emscripten_exception_flags.py
                 tools/check_payload_binary_corpus.py],
    "python" => %w[tools/check_python_package_inventory.py],
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

  aggregate = jobs.fetch("test")
  required_needs = ["detect_changes", *JOBS.keys].sort
  require_quality(Array(aggregate.fetch("needs")).sort == required_needs,
                  "aggregate needs are not exact")
  aggregate_run = step_run(aggregate, "Validate required test results")
  require_quality(aggregate_run.include?("check_p2_task11_quality.rb --validate-results"),
                  "aggregate does not use the tested result validator")

  allowlist = ROOT.join("tests/abi/fastdb_payload_v1_symbols.txt")
                  .read.lines(chomp: true)
  require_quality(allowlist.length == 99 && allowlist == allowlist.uniq.sort,
                  "portable payload ABI allowlist must contain 99 sorted symbols")
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
  expected_index =
    "P2 implementation in progress; " \
    "Task 11 local gates green/final review pending"
  require_quality(compact.call(index).include?(expected_index),
                  "issue index overstates or omits the P2 review state")
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
