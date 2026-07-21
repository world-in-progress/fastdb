#!/usr/bin/env ruby
# frozen_string_literal: true

require "json"
require "minitest/autorun"
require_relative "check_p3_runtime_quality"

class P3RuntimeQualityTest < Minitest::Test
  def assert_quality_error
    assert_raises(P3RuntimeQuality::QualityError) { yield }
  end

  def proof_document
    JSON.parse(File.read(File.join(__dir__, "p3_malformed_class_map.json")))
  end

  def test_rejects_missing_duplicate_or_misordered_class
    baseline = proof_document
    expected_d1_status = baseline.fetch("d1").fetch("status")
    P3RuntimeQuality.check_class_inventory(baseline, expected_d1_status)

    document = proof_document
    document.fetch("classes").delete_at(0)
    assert_quality_error do
      P3RuntimeQuality.check_class_inventory(document, expected_d1_status)
    end

    document = proof_document
    document.fetch("classes")[1]["class"] =
      document.fetch("classes")[0].fetch("class")
    assert_quality_error do
      P3RuntimeQuality.check_class_inventory(document, expected_d1_status)
    end

    document = proof_document
    document.fetch("classes")[0], document.fetch("classes")[1] =
      document.fetch("classes")[1], document.fetch("classes")[0]
    assert_quality_error do
      P3RuntimeQuality.check_class_inventory(document, expected_d1_status)
    end
  end

  def test_rejects_missing_or_unexecuted_proof_symbol
    document = proof_document
    document.fetch("classes")[0].fetch("proofs")[0]["test"] = "missing"
    reader = ->(_path) { "int present() {}\nint main() { present(); }\n" }
    assert_quality_error do
      P3RuntimeQuality.check_proof_entries(document, reader)
    end
  end

  def test_rejects_non_exact_abi
    exact = (1..105).map { |index| format("symbol_%03d", index) }
    P3RuntimeQuality.check_abi_symbols(exact, exact)
    assert_quality_error { P3RuntimeQuality.check_abi_symbols(exact.drop(1), exact) }
    assert_quality_error { P3RuntimeQuality.check_abi_symbols(exact.reverse, exact) }
    assert_quality_error do
      P3RuntimeQuality.check_abi_symbols(exact[0...-1] + [exact[0]], exact)
    end
  end

  def test_rejects_non_exact_corpus_inventory
    expected = P3RuntimeQuality::CORPUS_CASES
    manifest = expected.map do |name, klass, status, path|
      {"name" => name, "class" => klass,
       "expected" => {"status" => status, "path" => path}}
    end
    P3RuntimeQuality.check_corpus_inventory(manifest, expected.map(&:first))
    assert_quality_error do
      P3RuntimeQuality.check_corpus_inventory(manifest.drop(1), expected.map(&:first))
    end
    assert_quality_error do
      P3RuntimeQuality.check_corpus_inventory(manifest, expected.map(&:first).drop(1))
    end
  end

  def test_rejects_stale_graph_manifest_truth
    accepted = {
      "manifest" => "available graph_layout_exact_after_freeze",
      "schema" => "available graph_layout_exact_after_freeze",
      "golden" => "available graph_layout_exact_after_freeze"
    }
    P3RuntimeQuality.check_manifest_truth(accepted)
    accepted.each_key do |key|
      stale = accepted.merge(key => "not_evaluated runtime_slice_not_implemented")
      assert_quality_error { P3RuntimeQuality.check_manifest_truth(stale) }
    end
  end

  def test_rejects_missing_d1_proof_fields
    d1 = proof_document.fetch("d1")
    expected_status = d1.fetch("status")
    P3RuntimeQuality.check_d1_contract(d1, expected_status)
    P3RuntimeQuality::D1_KEYS.each do |key|
      invalid = d1.dup
      invalid.delete(key)
      assert_quality_error do
        P3RuntimeQuality.check_d1_contract(invalid, expected_status)
      end
    end
  end

  def test_d1_status_requires_the_explicit_expected_transition
    current_d1 = proof_document.fetch("d1")
    open_d1 = current_d1.merge("status" => P3RuntimeQuality::D1_OPEN_STATUS)
    closed_d1 = current_d1.merge(
      "status" => P3RuntimeQuality::D1_CLOSED_STATUS
    )

    P3RuntimeQuality.check_d1_contract(
      open_d1,
      P3RuntimeQuality::D1_OPEN_STATUS
    )
    P3RuntimeQuality.check_d1_contract(
      closed_d1,
      P3RuntimeQuality::D1_CLOSED_STATUS
    )
    assert_quality_error do
      P3RuntimeQuality.check_d1_contract(
        open_d1,
        P3RuntimeQuality::D1_CLOSED_STATUS
      )
    end
    assert_quality_error do
      P3RuntimeQuality.check_d1_contract(
        closed_d1,
        P3RuntimeQuality::D1_OPEN_STATUS
      )
    end
  end

  def test_rejects_missing_issue_truth
    truth = P3RuntimeQuality::ISSUE_MARKERS.join("\n")
    P3RuntimeQuality.check_issue_truth(truth)
    P3RuntimeQuality::ISSUE_MARKERS.each do |marker|
      assert_quality_error do
        P3RuntimeQuality.check_issue_truth(truth.sub(marker, "missing"))
      end
    end
  end

  def test_rejects_forbidden_public_ownership_or_type_terms
    P3RuntimeQuality.check_forbidden_public_terms({"header" => "str wstr"})
    P3RuntimeQuality::FORBIDDEN_PUBLIC_PATTERNS.each do |pattern|
      sample = pattern.source.include?("text") ? '"kind":"text"' : "C-Two route"
      assert_quality_error do
        P3RuntimeQuality.check_forbidden_public_terms({"header" => sample})
      end
    end
  end

  def test_rejects_incomplete_workflow_contract
    accepted = P3RuntimeQuality::WORKFLOW_MARKERS.join("\n")
    P3RuntimeQuality.check_workflow_contract(accepted)
    P3RuntimeQuality::WORKFLOW_MARKERS.each do |marker|
      assert_quality_error do
        P3RuntimeQuality.check_workflow_contract(accepted.sub(marker, "missing"))
      end
    end
  end

  def test_rejects_incomplete_package_contract
    accepted = P3RuntimeQuality::PACKAGE_MARKERS.join("\n")
    P3RuntimeQuality.check_package_contract(accepted)
    P3RuntimeQuality::PACKAGE_MARKERS.each do |marker|
      assert_quality_error do
        P3RuntimeQuality.check_package_contract(accepted.sub(marker, "missing"))
      end
    end
  end
end
