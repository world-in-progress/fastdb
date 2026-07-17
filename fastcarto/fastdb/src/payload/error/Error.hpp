#pragma once

#include "payload/json/Jcs.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace fastdb::payload::error {

class Error final : public std::exception {
public:
    static Error from_details(std::uint32_t code,
                              json::JsonPointer path,
                              std::string message,
                              json::JsonValue details);
    static Error from_jcs_failure(json::JcsFailure failure,
                                  json::JsonPointer path,
                                  std::string message,
                                  json::JsonValue details);

    Error(const Error&) = default;
    Error(Error&&) noexcept = default;
    Error& operator=(const Error&) = default;
    Error& operator=(Error&&) noexcept = default;
    ~Error() override = default;

    std::uint32_t code() const noexcept;
    std::string_view symbol() const noexcept;
    std::string_view path() const noexcept;
    std::string_view message() const noexcept;
    std::string_view details_json() const noexcept;
    const char* what() const noexcept override;

private:
    Error(std::uint32_t code,
          std::string symbol,
          std::string path,
          std::string message,
          std::string details_json);

    static Error internal_details_failure();

    std::uint32_t code_;
    std::string symbol_;
    std::string path_;
    std::string message_;
    std::string details_json_;
};

std::string_view symbol_for_code(std::uint32_t code) noexcept;
std::uint32_t code_for_jcs_failure(json::JcsFailure failure) noexcept;

}  // namespace fastdb::payload::error
