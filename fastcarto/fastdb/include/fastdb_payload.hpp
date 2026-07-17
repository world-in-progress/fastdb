#pragma once

#include <fastdb_payload.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fastdb::payload::v1 {

struct ByteView final {
    const std::uint8_t* data;
    std::uint64_t size;
};

enum class Profile : std::uint32_t {
    record_v1 = FDB_PAYLOAD_PROFILE_RECORD_V1,
    object_graph_v1 = FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1,
};

namespace detail {
struct BlobFactory;
struct ErrorFactory;
}  // namespace detail

class Blob final {
public:
    Blob(const Blob& other) noexcept : handle_(other.handle_) {
        fdb_payload_v1_blob_retain(handle_);
    }

    Blob(Blob&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Blob& operator=(const Blob& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_blob_retain(other.handle_);
            fdb_payload_v1_blob_release(handle_);
            handle_ = other.handle_;
        }
        return *this;
    }

    Blob& operator=(Blob&& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_blob_release(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Blob() { fdb_payload_v1_blob_release(handle_); }

    ByteView bytes() const noexcept {
        return {fdb_payload_v1_blob_data(handle_),
                fdb_payload_v1_blob_size(handle_)};
    }

    std::string_view as_string_view() const noexcept {
        const ByteView value = bytes();
        if (value.data == nullptr) {
            return {};
        }
        return {reinterpret_cast<const char*>(value.data),
                static_cast<std::size_t>(value.size)};
    }

private:
    friend struct detail::BlobFactory;

    explicit Blob(fdb_payload_v1_blob_t* handle) noexcept : handle_(handle) {}

    fdb_payload_v1_blob_t* handle_{nullptr};
};

class PayloadError final : public std::runtime_error {
public:
    PayloadError(const PayloadError&) = default;
    PayloadError(PayloadError&&) noexcept = default;
    PayloadError& operator=(const PayloadError&) = default;
    PayloadError& operator=(PayloadError&&) noexcept = default;
    ~PayloadError() override = default;

    std::uint32_t code() const noexcept { return code_; }
    std::string_view symbol() const noexcept { return symbol_; }
    std::string_view path() const noexcept { return path_; }
    std::string_view details_json() const noexcept { return details_json_; }

private:
    friend struct detail::ErrorFactory;

    PayloadError(std::uint32_t code,
                 std::string symbol,
                 std::string path,
                 std::string message,
                 std::string details_json)
        : std::runtime_error(message),
          code_(code),
          symbol_(std::move(symbol)),
          path_(std::move(path)),
          details_json_(std::move(details_json)) {}

    std::uint32_t code_;
    std::string symbol_;
    std::string path_;
    std::string details_json_;
};

struct CompileOptions final {
    CompileOptions() noexcept { fdb_payload_v1_compile_options_init(&value); }

    fdb_payload_v1_compile_options_t value;
};

struct Capabilities final {
    Capabilities() noexcept { fdb_payload_v1_capabilities_init(&value); }

    fdb_payload_v1_capabilities_t value;
};

namespace detail {

struct BlobFactory final {
    static Blob take(fdb_payload_v1_blob_t* handle) noexcept {
        return Blob(handle);
    }
};

struct ErrorOwner final {
    explicit ErrorOwner(fdb_payload_v1_error_t* value) noexcept
        : handle(value) {}
    ErrorOwner(const ErrorOwner&) = delete;
    ErrorOwner& operator=(const ErrorOwner&) = delete;
    ~ErrorOwner() { fdb_payload_v1_error_release(handle); }

    fdb_payload_v1_error_t* handle;
};

inline std::string error_bytes(fdb_payload_v1_error_t* error,
                               void (*getter)(const fdb_payload_v1_error_t*,
                                              const std::uint8_t**,
                                              std::uint64_t*)) {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    getter(error, &data, &size);
    if (data == nullptr) {
        return {};
    }
    return {reinterpret_cast<const char*>(data),
            static_cast<std::size_t>(size)};
}

struct ErrorFactory final {
    [[noreturn]] static void raise(fdb_payload_v1_status_t status,
                                   fdb_payload_v1_error_t* error) {
        ErrorOwner owner(error);
        if (error == nullptr) {
            throw PayloadError(
                status, "INTERNAL", "",
                "FastDB payload call failed without an error handle", "{}");
        }
        const std::uint32_t code = fdb_payload_v1_error_code(error);
        std::string symbol = error_bytes(error, fdb_payload_v1_error_symbol);
        std::string path = error_bytes(error, fdb_payload_v1_error_path);
        std::string message = error_bytes(error, fdb_payload_v1_error_message);
        std::string details =
            error_bytes(error, fdb_payload_v1_error_details_json);
        throw PayloadError(code, std::move(symbol), std::move(path),
                           std::move(message), std::move(details));
    }
};

inline void check(fdb_payload_v1_status_t status,
                  fdb_payload_v1_error_t* const* error) {
    fdb_payload_v1_error_t* const owned_error =
        error == nullptr ? nullptr : *error;
    if (status != UINT32_C(0)) {
        ErrorFactory::raise(status, owned_error);
    }
    fdb_payload_v1_error_release(owned_error);
}

}  // namespace detail

class CompiledSpec final {
public:
    static CompiledSpec compile(
        std::string_view source,
        const CompileOptions& options = CompileOptions{}) {
        fdb_payload_v1_spec_t* spec = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        const fdb_payload_v1_status_t status =
            fdb_payload_v1_spec_compile_json(
                reinterpret_cast<const std::uint8_t*>(source.data()),
                static_cast<std::uint64_t>(source.size()), &options.value,
                &spec, &error);
        detail::check(status, &error);
        return CompiledSpec(spec);
    }

    CompiledSpec(const CompiledSpec& other) noexcept : handle_(other.handle_) {
        fdb_payload_v1_spec_retain(handle_);
    }

    CompiledSpec(CompiledSpec&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    CompiledSpec& operator=(const CompiledSpec& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_spec_retain(other.handle_);
            fdb_payload_v1_spec_release(handle_);
            handle_ = other.handle_;
        }
        return *this;
    }

    CompiledSpec& operator=(CompiledSpec&& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_spec_release(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~CompiledSpec() { fdb_payload_v1_spec_release(handle_); }

    Blob canonical_json() const {
        fdb_payload_v1_blob_t* blob = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_spec_canonical_json(handle_, &blob, &error),
            &error);
        return detail::BlobFactory::take(blob);
    }

    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> sha256() const {
        std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_spec_sha256(handle_, digest.data(), &error),
            &error);
        return digest;
    }

    Blob manifest_json() const {
        fdb_payload_v1_blob_t* blob = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_spec_manifest_json(handle_, &blob, &error),
            &error);
        return detail::BlobFactory::take(blob);
    }

    Profile profile() const {
        fdb_payload_v1_profile_t profile_value = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_spec_profile(handle_, &profile_value, &error),
            &error);
        return static_cast<Profile>(profile_value);
    }

    Capabilities capabilities() const {
        Capabilities result;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_spec_capabilities(handle_, &result.value, &error),
            &error);
        return result;
    }

    std::uint32_t entry_count() const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_spec_entry_count(handle_, &result, &error),
            &error);
        return result;
    }

    Blob entry_id(std::uint32_t index) const {
        fdb_payload_v1_blob_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_spec_entry_id(handle_, index, &result, &error),
            &error);
        return detail::BlobFactory::take(result);
    }

    std::uint32_t entry_index(std::string_view id) const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_spec_entry_index(
                          handle_,
                          reinterpret_cast<const std::uint8_t*>(id.data()),
                          static_cast<std::uint64_t>(id.size()), &result,
                          &error),
                      &error);
        return result;
    }

    std::uint32_t component_count() const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_spec_component_count(handle_, &result, &error),
            &error);
        return result;
    }

    Blob component_id(std::uint32_t index) const {
        fdb_payload_v1_blob_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_spec_component_id(
                          handle_, index, &result, &error),
                      &error);
        return detail::BlobFactory::take(result);
    }

    std::uint32_t component_index(std::string_view id) const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_spec_component_index(
                          handle_,
                          reinterpret_cast<const std::uint8_t*>(id.data()),
                          static_cast<std::uint64_t>(id.size()), &result,
                          &error),
                      &error);
        return result;
    }

    std::uint32_t component_field_count(
        std::uint32_t component_index_value) const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_spec_component_field_count(
                          handle_, component_index_value, &result, &error),
                      &error);
        return result;
    }

    Blob component_field_id(std::uint32_t component_index_value,
                            std::uint32_t field_index) const {
        fdb_payload_v1_blob_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_spec_component_field_id(
                          handle_, component_index_value, field_index, &result,
                          &error),
                      &error);
        return detail::BlobFactory::take(result);
    }

    std::uint32_t component_field_index(
        std::uint32_t component_index_value,
        std::string_view id) const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_spec_component_field_index(
                          handle_, component_index_value,
                          reinterpret_cast<const std::uint8_t*>(id.data()),
                          static_cast<std::uint64_t>(id.size()), &result,
                          &error),
                      &error);
        return result;
    }

private:
    explicit CompiledSpec(fdb_payload_v1_spec_t* handle) noexcept
        : handle_(handle) {}

    fdb_payload_v1_spec_t* handle_{nullptr};
};

inline Blob source_schema() {
    fdb_payload_v1_blob_t* blob = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    detail::check(fdb_payload_v1_source_schema_json(&blob, &error), &error);
    return detail::BlobFactory::take(blob);
}

inline std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE>
source_schema_sha256() {
    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
    fdb_payload_v1_error_t* error = nullptr;
    detail::check(
        fdb_payload_v1_source_schema_sha256(digest.data(), &error), &error);
    return digest;
}

}  // namespace fastdb::payload::v1
