#!/usr/bin/env ruby
# frozen_string_literal: true

require "json"
require "pathname"

module P3RuntimeQuality
  ROOT = Pathname.new(__dir__).join("../..").expand_path
  PROOF_MAP = ROOT.join("tests/ci/p3_malformed_class_map.json")
  ABI_ALLOWLIST = ROOT.join("tests/abi/fastdb_payload_v1_symbols.txt")
  CORPUS_MANIFEST = ROOT.join("tests/fuzz/payload/binary-corpus.json")
  CORPUS_DIRECTORY = ROOT.join("tests/fuzz/payload/binary-corpus")
  ISSUE = ROOT.join("docs/issues/0002-portable-payload-foundation-implementation-status.md")
  WORKFLOW = ROOT.join(".github/workflows/tests.yml")
  PACKAGE_CHECKER = ROOT.join("tools/check_python_package_inventory.py")

  class QualityError < StandardError; end

  CLASSES = %w[
    profile-and-region-inventory
    object-descriptor-shape
    padding-validity-and-null-slots
    root-reference-and-object-id-bounds
    reachability
    scalar-and-value-canonicality
    list-partitions
    utf8-and-utf16
    resource-limits-work-and-nesting
    builder-state-and-handles
    allocation-failure
    backing-direct-no-full-image
    backing-callback-failures
    view-generation-and-access-drain
    materialization-closure
    abi-prefix-output-and-exception
    wasm-graph-and-exception-containment
  ].freeze

  CORPUS_CASES = [
    ["valid-empty.bin", "valid-empty", 0, ""],
    ["valid-fixed.bin", "valid-fixed", 0, ""],
    ["valid-text.bin", "valid-text", 0, ""],
    ["valid-list.bin", "valid-list", 0, ""],
    ["malformed-magic.bin", "malformed-magic", 3001,
     "/binary/header/magic"],
    ["malformed-length.bin", "malformed-length", 3009,
     "/binary/header/total_length"],
    ["malformed-offset.bin", "malformed-offset", 3005,
     "/binary/regions/0/data_offset"],
    ["malformed-validity.bin", "malformed-validity", 3009,
     "/binary/regions/1/data"],
    ["malformed-text.bin", "malformed-text", 2010,
     "/entries/records/0/scalars/j_str"],
    ["malformed-list.bin", "malformed-list", 3004,
     "/entries/records/0/bools"],
    ["valid-graph-cycle.bin", "valid-graph-cycle", 0, ""],
    ["valid-graph-variable.bin", "valid-graph-variable", 0, ""],
    ["valid-graph-null.bin", "valid-graph-null", 0, ""],
    ["malformed-graph-object-region.bin", "malformed-graph-object-region",
     3009, "/binary/regions/1/stride"],
    ["malformed-graph-reference.bin", "malformed-graph-reference", 3007,
     "/objects/0/0/c_shared"],
    ["malformed-graph-unreachable.bin", "malformed-graph-unreachable", 3009,
     "/objects/Node/1"]
  ].freeze

  P3_ABI_ADDITIONS = %w[
    fdb_payload_v1_builder_object_declare
    fdb_payload_v1_builder_object_fill_begin
    fdb_payload_v1_builder_value_object
    fdb_payload_v1_builder_value_ref
    fdb_payload_v1_view_graph_identity
    fdb_payload_v1_view_ref_target
  ].freeze

  D1_KEYS = %w[status direct_test source report_fields facts].freeze
  D1_OPEN_STATUS = "open-until-task-10"
  D1_CLOSED_STATUS = "closed-task-10"
  D1_CLOSURE_MARKER = "D1 closure state: `closed-task-10`"
  D1_FACTS = %w[
    direct-reserve-only
    no-heap-reserve
    monotonic-full-coverage
    allocation-threshold
    report-truth
    staged-byte-identity
    source-audit
    cycles-sharing-variable-values-and-failures
  ].freeze

  P3_DESIGN_SECTIONS = (4..21).map(&:to_s).freeze
  STAGE_B_REQUIREMENT_IDS = %w[
    graph-authoring
    all-v1-values
    graph-topology
    immutable-build-plan
    backing-execution
    hardened-open
    malformed-id-rejection
    checked-views
    closure-materialization
    lifetime-invalidation
    binary-contract
    manifest-truth
    public-abi
    quality-proof
  ].freeze

  ISSUE_MARKERS = [
    "#### P3 Task 9 local hardening evidence",
    "16/16 reviewed binary-open seeds",
    "D1 remains open until Task 10",
    "Hosted results remain pending",
    "full graph wasm runtime",
    "primary-agent review"
  ].freeze

  FORBIDDEN_PUBLIC_PATTERNS = [
    /\b(?:C-Two|Toodle|CRM|route|relay|transport|lease)\b/i,
    /"kind"\s*:\s*"text"/
  ].freeze

  WORKFLOW_MARKERS = [
    "check_p3_runtime_quality.rb --check-repository",
    "test_check_p3_runtime_quality.rb",
    "fuzz_payload_open",
    "payload_runtime_harness.js",
    "check_payload_abi_symbols.py --wasm-build-dir",
    "tools/check_payload_binary_corpus.py --check"
  ].freeze

  PACKAGE_MARKERS = %w[
    fastcarto/fastdb/src/payload/build/GraphAuthoring.cpp
    fastcarto/fastdb/src/payload/build/GraphAuthoring.hpp
    fastcarto/fastdb/src/payload/build/GraphEncoder.cpp
    fastcarto/fastdb/src/payload/build/GraphEncoder.hpp
    fastcarto/fastdb/src/payload/layout/GraphLayout.cpp
    fastcarto/fastdb/src/payload/layout/GraphLayout.hpp
    fastcarto/fastdb/src/payload/view/GraphOpen.cpp
    fastcarto/fastdb/src/payload/view/GraphOpen.hpp
    fastcarto/fastdb/src/payload/view/GraphView.cpp
    fastcarto/fastdb/src/payload/view/GraphMaterialize.cpp
    fastcarto/fastdb/src/payload/spec/Manifest.cpp
    fastcarto/fastdb/src/payload/spec/EmbeddedSchemas.inc
  ].freeze

  module_function

  def require_quality(condition, message)
    raise QualityError, message unless condition
  end

  def check_class_inventory(document, expected_d1_status = D1_OPEN_STATUS)
    require_quality(document.fetch("schema") ==
                    "fastdb.payload.p3-proof-map.v1",
                    "unexpected P3 proof-map schema")
    rows = document.fetch("classes")
    require_quality(rows.is_a?(Array), "P3 proof-map classes must be an array")
    actual = rows.map { |row| row.fetch("class") }
    require_quality(actual == CLASSES,
                    "P3 proof classes must be exact, unique, and ordered")
    rows.each do |row|
      proofs = row.fetch("proofs")
      require_quality(proofs.is_a?(Array) && !proofs.empty?,
                      "#{row.fetch('class')} has no exact proof")
    end
    check_d1_contract(document.fetch("d1"), expected_d1_status)
  rescue KeyError => error
    raise QualityError, "P3 proof map omits #{error.key.inspect}"
  end

  def check_proof_entries(document, reader = nil)
    reader ||= ->(relative) { ROOT.join(relative).read }
    document.fetch("classes").each do |row|
      row.fetch("proofs").each do |proof|
        relative = proof.fetch("file")
        name = proof.fetch("test")
        require_quality(relative.is_a?(String) && !relative.empty? &&
                        name.is_a?(String) && !name.empty?,
                        "P3 proof entries require file and test strings")
        source = reader.call(relative)
        if name.end_with?(".bin")
          require_quality(source.include?(name),
                          "#{relative} does not name reviewed seed #{name}")
        else
          definition = /\b(?:int|bool|void)\s+#{Regexp.escape(name)}\s*\(/
          invocation = /\b#{Regexp.escape(name)}\s*\(/
          require_quality(source.match?(definition),
                          "#{relative} does not define #{name}")
          require_quality(source.scan(invocation).length >= 2,
                          "#{relative} does not execute #{name}")
        end
      end
    end
  rescue KeyError, Errno::ENOENT => error
    raise QualityError, "invalid P3 proof entry: #{error.message}"
  end

  def check_traceability(document, reader = nil)
    reader ||= ->(relative) { ROOT.join(relative).read }
    traceability = document.fetch("traceability")
    require_quality(traceability.keys ==
                    %w[p3_design_sections active_goal_stage_b],
                    "P3 traceability groups must be exact and ordered")

    sections = traceability.fetch("p3_design_sections")
    require_quality(sections.is_a?(Array) &&
                    sections.map { |row| row.fetch("section") } ==
                      P3_DESIGN_SECTIONS,
                    "P3 design sections 4-21 must be exact and ordered")
    sections.each do |row|
      require_quality(row.keys ==
                      %w[section requirement implementation proofs],
                      "P3 design trace rows must have exact fields")
      require_quality(non_empty_string?(row.fetch("requirement")),
                      "P3 design trace requirement must be non-empty")
      implementations = row.fetch("implementation")
      require_quality(implementations.is_a?(Array) &&
                      !implementations.empty?,
                      "P3 design trace must name implementation authority")
      implementations.each do |implementation|
        require_quality(implementation.keys == %w[file symbol] &&
                        non_empty_string?(implementation.fetch("file")) &&
                        non_empty_string?(implementation.fetch("symbol")),
                        "P3 design implementation trace is incomplete")
        source = reader.call(implementation.fetch("file"))
        require_quality(source.include?(implementation.fetch("symbol")),
                        "P3 design implementation symbol is absent: " \
                        "#{implementation.fetch('symbol')}")
      end
      proofs = row.fetch("proofs")
      require_quality(proofs.is_a?(Array) && !proofs.empty?,
                      "P3 design trace must name executable proof")
      proofs.each do |proof|
        require_quality(proof.keys == %w[file evidence] &&
                        non_empty_string?(proof.fetch("file")) &&
                        non_empty_string?(proof.fetch("evidence")),
                        "P3 design proof trace is incomplete")
        reader.call(proof.fetch("file"))
      end
    end

    goals = traceability.fetch("active_goal_stage_b")
    require_quality(goals.is_a?(Array) &&
                    goals.map { |row| row.fetch("id") } ==
                      STAGE_B_REQUIREMENT_IDS,
                    "active Goal Stage B requirements must be exact and ordered")
    goals.each do |row|
      require_quality(row.keys == %w[id requirement implementation proof],
                      "Goal Stage B trace rows must have exact fields")
      %w[requirement implementation proof].each do |key|
        require_quality(non_empty_string?(row.fetch(key)),
                        "Goal Stage B #{key} must be non-empty")
      end
    end
  rescue KeyError, Errno::ENOENT => error
    raise QualityError, "invalid P3 closure traceability: #{error.message}"
  end

  def check_abi_symbols(actual, expected)
    require_quality(expected.length == 105 && expected == expected.uniq.sort,
                    "reviewed ABI expectation must be 105 sorted unique symbols")
    require_quality(actual == expected,
                    "portable payload ABI symbols are not the exact reviewed 105")
  end

  def check_corpus_inventory(manifest, filenames)
    actual = manifest.map do |item|
      expectation = item.fetch("expected")
      [item.fetch("name"), item.fetch("class"),
       expectation.fetch("status"), expectation.fetch("path")]
    end
    require_quality(actual == CORPUS_CASES,
                    "binary-open corpus manifest is not the exact reviewed 16")
    expected_names = CORPUS_CASES.map(&:first)
    require_quality(filenames.length == filenames.uniq.length &&
                    filenames.sort == expected_names.sort,
                    "binary-open corpus files are not the exact reviewed 16")
  rescue KeyError => error
    raise QualityError, "binary-open corpus omits #{error.key.inspect}"
  end

  def check_manifest_truth(contents)
    contents.each do |label, source|
      require_quality(source.include?("available") &&
                      source.include?("graph_layout_exact_after_freeze") &&
                      !source.include?("runtime_slice_not_implemented"),
                      "#{label} retains stale graph manifest truth")
    end
  end

  def check_d1_contract(d1, expected_status = D1_OPEN_STATUS)
    require_quality(d1.keys == D1_KEYS,
                    "D1 proof contract keys must be exact and ordered")
    require_quality([D1_OPEN_STATUS, D1_CLOSED_STATUS].include?(expected_status),
                    "unexpected expected D1 status #{expected_status.inspect}")
    require_quality(d1.fetch("status") == expected_status,
                    "D1 status does not match the Issue 0002 closure state")
    require_quality(d1.fetch("direct_test") ==
                    "test_direct_graph_has_no_full_image_allocation",
                    "D1 direct test is not the reviewed proof")
    require_quality(d1.fetch("source") ==
                    "fastcarto/fastdb/src/payload/build/GraphEncoder.cpp",
                    "D1 source authority drifted")
    require_quality(d1.fetch("report_fields") ==
                    %w[mode fallback_reason staging_bytes],
                    "D1 report fields drifted")
    require_quality(d1.fetch("facts") == D1_FACTS,
                    "D1 fact inventory drifted")
  rescue KeyError => error
    raise QualityError, "D1 proof contract omits #{error.key.inspect}"
  end

  def check_issue_truth(issue)
    missing = ISSUE_MARKERS.reject { |marker| issue.include?(marker) }
    require_quality(missing.empty?,
                    "Issue 0002 lacks P3 Task 9 truth: #{missing.join(', ')}")
  end

  def check_forbidden_public_terms(sources)
    sources.each do |label, source|
      FORBIDDEN_PUBLIC_PATTERNS.each do |pattern|
        require_quality(!source.match?(pattern),
                        "#{label} contains forbidden public term #{pattern.inspect}")
      end
    end
  end

  def check_workflow_contract(workflow)
    missing = WORKFLOW_MARKERS.reject { |marker| workflow.include?(marker) }
    require_quality(missing.empty?,
                    "workflow lacks P3 gates: #{missing.join(', ')}")
  end

  def check_package_contract(checker)
    missing = PACKAGE_MARKERS.reject { |marker| checker.include?(marker) }
    require_quality(missing.empty?,
                    "package inventory lacks P3 Core sources: #{missing.join(', ')}")
  end

  def check_d1_repository(document)
    d1 = document.fetch("d1")
    direct_source = ROOT.join("tests/cpp/payload/test_graph_backing.cpp").read
    direct_test = d1.fetch("direct_test")
    require_quality(direct_source.match?(
                      /\bint\s+#{Regexp.escape(direct_test)}\s*\(/) &&
                    direct_source.scan(/\b#{Regexp.escape(direct_test)}\s*\(/)
                                 .length >= 2,
                    "D1 direct no-full-image proof is absent or unexecuted")
    encoder = ROOT.join(d1.fetch("source")).read
    %w[AscendingWriter ByteSink zero_until encode_graph].each do |term|
      require_quality(encoder.include?(term),
                      "D1 source audit cannot find #{term}")
    end
    plan = ROOT.join("fastcarto/fastdb/src/payload/build/BuildPlan.cpp").read
    %w[RangeCallbackSink reservation.commit ExecutionReport encode_profile].each do |term|
      require_quality(plan.include?(term),
                      "D1 execution audit cannot find #{term}")
    end
    report = ROOT.join(
      "fastcarto/fastdb/src/payload/build/ExecutionReport.hpp"
    ).read
    d1.fetch("report_fields").each do |field|
      require_quality(report.include?(field),
                      "D1 report authority cannot find #{field}")
    end
  end

  def non_empty_string?(value)
    value.is_a?(String) && !value.empty?
  end

  def check_repository
    document = JSON.parse(PROOF_MAP.read)
    issue = ISSUE.read
    expected_d1_status = if issue.include?(D1_CLOSURE_MARKER)
                           D1_CLOSED_STATUS
                         else
                           D1_OPEN_STATUS
                         end
    check_class_inventory(document, expected_d1_status)
    check_proof_entries(document)
    check_traceability(document) if expected_d1_status == D1_CLOSED_STATUS

    symbols = ABI_ALLOWLIST.read.lines(chomp: true)
    check_abi_symbols(symbols, symbols)
    require_quality((P3_ABI_ADDITIONS - symbols).empty? &&
                    (symbols - P3_ABI_ADDITIONS).length == 99,
                    "ABI-105 must preserve 99 P2 exports and add exact P3 six")

    corpus = JSON.parse(CORPUS_MANIFEST.read).fetch("cases")
    corpus_files = CORPUS_DIRECTORY.children.select(&:file?).map do |path|
      path.basename.to_s
    end
    check_corpus_inventory(corpus, corpus_files)

    golden_hex = ROOT.join(
      "tests/golden/payload/v1/spec/valid/object-graph.manifest.hex"
    ).read.gsub(/\s+/, "")
    check_manifest_truth(
      "manifest" => ROOT.join(
        "fastcarto/fastdb/src/payload/spec/Manifest.cpp"
      ).read,
      "schema" => ROOT.join(
        "schemas/fastdb.payload.manifest.v1.schema.json"
      ).read,
      "golden" => [golden_hex].pack("H*")
    )

    check_d1_repository(document)
    check_issue_truth(issue)
    check_forbidden_public_terms(
      "C header" => ROOT.join(
        "fastcarto/fastdb/include/fastdb_payload.h"
      ).read,
      "C++ header" => ROOT.join(
        "fastcarto/fastdb/include/fastdb_payload.hpp"
      ).read,
      "source schema" => ROOT.join("schemas/fastdb.payload.v1.schema.json").read,
      "manifest schema" => ROOT.join(
        "schemas/fastdb.payload.manifest.v1.schema.json"
      ).read
    )
    check_workflow_contract(WORKFLOW.read)
    check_package_contract(PACKAGE_CHECKER.read)
  rescue JSON::ParserError, Errno::ENOENT => error
    raise QualityError, "cannot inspect P3 repository truth: #{error.message}"
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    mode = ARGV.fetch(0, "--check-repository")
    raise P3RuntimeQuality::QualityError, "unknown mode #{mode.inspect}" unless
      mode == "--check-repository"

    P3RuntimeQuality.check_repository
  rescue KeyError, P3RuntimeQuality::QualityError => error
    warn "P3 runtime quality check failed: #{error.message}"
    exit 1
  end
  puts "P3 runtime quality check passed"
end
