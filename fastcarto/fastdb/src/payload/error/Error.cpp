#include "payload/error/Error.hpp"

#include <fastdb_payload.h>

#include <string>
#include <utility>
#include <variant>

namespace fastdb::payload::error {

std::string_view symbol_for_code(std::uint32_t code) noexcept {
    switch (code) {
    case FDB_PAYLOAD_E_INVALID_JSON:
        return "INVALID_JSON";
    case FDB_PAYLOAD_E_DUPLICATE_KEY:
        return "DUPLICATE_KEY";
    case FDB_PAYLOAD_E_UNKNOWN_FIELD:
        return "UNKNOWN_FIELD";
    case FDB_PAYLOAD_E_UNSUPPORTED_SCHEMA:
        return "UNSUPPORTED_SCHEMA";
    case FDB_PAYLOAD_E_INVALID_TYPE:
        return "INVALID_TYPE";
    case FDB_PAYLOAD_E_DUPLICATE_ID:
        return "DUPLICATE_ID";
    case FDB_PAYLOAD_E_UNRESOLVED_COMPONENT:
        return "UNRESOLVED_COMPONENT";
    case FDB_PAYLOAD_E_PROFILE_VIOLATION:
        return "PROFILE_VIOLATION";
    case FDB_PAYLOAD_E_INVALID_NUMBER:
        return "INVALID_NUMBER";
    case FDB_PAYLOAD_E_SPEC_RESOURCE_LIMIT:
        return "SPEC_RESOURCE_LIMIT";
    case FDB_PAYLOAD_E_MISSING_ENTRY:
        return "MISSING_ENTRY";
    case FDB_PAYLOAD_E_MISSING_FIELD:
        return "MISSING_FIELD";
    case FDB_PAYLOAD_E_UNEXPECTED_NULL:
        return "UNEXPECTED_NULL";
    case FDB_PAYLOAD_E_TYPE_MISMATCH:
        return "TYPE_MISMATCH";
    case FDB_PAYLOAD_E_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case FDB_PAYLOAD_E_BUILDER_STATE:
        return "BUILDER_STATE";
    case FDB_PAYLOAD_E_DIRECT_UNAVAILABLE:
        return "DIRECT_UNAVAILABLE";
    case FDB_PAYLOAD_E_PLAN_STATE:
        return "PLAN_STATE";
    case FDB_PAYLOAD_E_RUNTIME_UNAVAILABLE:
        return "RUNTIME_UNAVAILABLE";
    case FDB_PAYLOAD_E_INVALID_TEXT_ENCODING:
        return "INVALID_TEXT_ENCODING";
    case FDB_PAYLOAD_E_BUILDER_LENGTH_OVERFLOW:
        return "BUILDER_LENGTH_OVERFLOW";
    case FDB_PAYLOAD_E_BUILDER_OUT_OF_BOUNDS:
        return "BUILDER_OUT_OF_BOUNDS";
    case FDB_PAYLOAD_E_BUILDER_RESOURCE_LIMIT:
        return "BUILDER_RESOURCE_LIMIT";
    case FDB_PAYLOAD_E_INVALID_MAGIC:
        return "INVALID_MAGIC";
    case FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION:
        return "UNSUPPORTED_BINARY_VERSION";
    case FDB_PAYLOAD_E_LENGTH_OVERFLOW:
        return "LENGTH_OVERFLOW";
    case FDB_PAYLOAD_E_OUT_OF_BOUNDS:
        return "OUT_OF_BOUNDS";
    case FDB_PAYLOAD_E_MISALIGNED:
        return "MISALIGNED";
    case FDB_PAYLOAD_E_DIGEST_MISMATCH:
        return "DIGEST_MISMATCH";
    case FDB_PAYLOAD_E_INVALID_REFERENCE:
        return "INVALID_REFERENCE";
    case FDB_PAYLOAD_E_RESOURCE_LIMIT:
        return "RESOURCE_LIMIT";
    case FDB_PAYLOAD_E_NON_CANONICAL_BINARY:
        return "NON_CANONICAL_BINARY";
    case FDB_PAYLOAD_E_INVALID_BINARY_VALUE:
        return "INVALID_BINARY_VALUE";
    case FDB_PAYLOAD_E_VIEW_INVALIDATED:
        return "VIEW_INVALIDATED";
    case FDB_PAYLOAD_E_STALE_GENERATION:
        return "STALE_GENERATION";
    case FDB_PAYLOAD_E_READ_ONLY:
        return "READ_ONLY";
    case FDB_PAYLOAD_E_BACKING_CONTRACT:
        return "BACKING_CONTRACT";
    case FDB_PAYLOAD_E_ALLOCATION_FAILED:
        return "ALLOCATION_FAILED";
    case FDB_PAYLOAD_E_COMMIT_FAILED:
        return "COMMIT_FAILED";
    case FDB_PAYLOAD_E_ROLLBACK_FAILED:
        return "ROLLBACK_FAILED";
    case FDB_PAYLOAD_E_UNSUPPORTED_TARGET:
        return "UNSUPPORTED_TARGET";
    case FDB_PAYLOAD_E_INVALID_ARTIFACT_PATH:
        return "INVALID_ARTIFACT_PATH";
    case FDB_PAYLOAD_E_GENERATOR_FAILED:
        return "GENERATOR_FAILED";
    case FDB_PAYLOAD_E_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case FDB_PAYLOAD_E_UNSUPPORTED_ABI:
        return "UNSUPPORTED_ABI";
    case FDB_PAYLOAD_E_NOT_FOUND:
        return "NOT_FOUND";
    case FDB_PAYLOAD_E_INDEX_OUT_OF_RANGE:
        return "INDEX_OUT_OF_RANGE";
    case FDB_PAYLOAD_E_INTERNAL:
        return "INTERNAL";
    default:
        return {};
    }
}

std::uint32_t code_for_jcs_failure(json::JcsFailure failure) noexcept {
    switch (failure) {
    case json::JcsFailure::non_finite_number:
        return FDB_PAYLOAD_E_INVALID_NUMBER;
    case json::JcsFailure::invalid_utf8:
        return FDB_PAYLOAD_E_INVALID_JSON;
    case json::JcsFailure::duplicate_member:
        return FDB_PAYLOAD_E_DUPLICATE_KEY;
    }
    return FDB_PAYLOAD_E_INTERNAL;
}

Error Error::from_details(std::uint32_t code,
                          json::JsonPointer path,
                          std::string message,
                          json::JsonValue details) {
    const std::string_view symbol = symbol_for_code(code);
    if (symbol.empty()) {
        return internal_diagnostic_failure();
    }

    const auto serialized_message =
        json::jcs_serialize(json::JsonValue{message});
    if (!std::holds_alternative<std::string>(serialized_message)) {
        return internal_diagnostic_failure();
    }

    auto serialized = json::jcs_serialize(details);
    auto* const details_json = std::get_if<std::string>(&serialized);
    if (details_json == nullptr) {
        return internal_diagnostic_failure();
    }

    return Error(code, std::string(symbol), path.value(), std::move(message),
                 std::move(*details_json));
}

Error Error::from_jcs_failure(json::JcsFailure failure,
                              json::JsonPointer path,
                              std::string message,
                              json::JsonValue details) {
    return from_details(code_for_jcs_failure(failure), std::move(path),
                        std::move(message), std::move(details));
}

std::uint32_t Error::code() const noexcept { return code_; }

std::string_view Error::symbol() const noexcept { return symbol_; }

std::string_view Error::path() const noexcept { return path_; }

std::string_view Error::message() const noexcept { return message_; }

std::string_view Error::details_json() const noexcept {
    return details_json_;
}

const char* Error::what() const noexcept { return message_.c_str(); }

Error::Error(std::uint32_t code,
             std::string symbol,
             std::string path,
             std::string message,
             std::string details_json)
    : code_(code),
      symbol_(std::move(symbol)),
      path_(std::move(path)),
      message_(std::move(message)),
      details_json_(std::move(details_json)) {}

Error Error::internal_diagnostic_failure() {
    return Error(FDB_PAYLOAD_E_INTERNAL, "INTERNAL", {},
                 "Core error diagnostic validation failed", "{}");
}

}  // namespace fastdb::payload::error
