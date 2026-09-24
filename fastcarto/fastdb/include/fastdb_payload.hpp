#pragma once

#include <fastdb_payload.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace fastdb::payload::v1 {

/* Non-owning byte span. size is a byte count; keep its documented owner alive. */
struct ByteView final {
    const std::uint8_t* data;
    std::size_t size;
};

/*
 * Non-owning wide-text span. size counts uint16_t code units, not bytes; keep
 * its documented owner alive.
 */
struct WideView final {
    const std::uint16_t* data;
    std::size_t size;
};

enum class Profile : std::uint32_t {
    record_v1 = FDB_PAYLOAD_PROFILE_RECORD_V1,
    object_graph_v1 = FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1,
};

enum class BuildPolicy : std::uint32_t {
    allow_staging = FDB_PAYLOAD_BUILD_ALLOW_STAGING,
    require_direct = FDB_PAYLOAD_BUILD_REQUIRE_DIRECT,
};

enum class ViewKind : std::uint32_t {
    sequence = FDB_PAYLOAD_VIEW_SEQUENCE,
    boolean = FDB_PAYLOAD_VIEW_BOOL,
    u8 = FDB_PAYLOAD_VIEW_U8,
    u16 = FDB_PAYLOAD_VIEW_U16,
    u32 = FDB_PAYLOAD_VIEW_U32,
    i32 = FDB_PAYLOAD_VIEW_I32,
    u8n = FDB_PAYLOAD_VIEW_U8N,
    u16n = FDB_PAYLOAD_VIEW_U16N,
    f32 = FDB_PAYLOAD_VIEW_F32,
    f64 = FDB_PAYLOAD_VIEW_F64,
    str = FDB_PAYLOAD_VIEW_STR,
    wstr = FDB_PAYLOAD_VIEW_WSTR,
    bytes = FDB_PAYLOAD_VIEW_BYTES,
    component = FDB_PAYLOAD_VIEW_COMPONENT,
    list = FDB_PAYLOAD_VIEW_LIST,
    ref = FDB_PAYLOAD_VIEW_REF,
};

enum class CodegenTarget : std::uint64_t {
    cpp = FDB_PAYLOAD_CODEGEN_TARGET_CPP,
    rust = FDB_PAYLOAD_CODEGEN_TARGET_RUST,
    python = FDB_PAYLOAD_CODEGEN_TARGET_PYTHON,
    typescript = FDB_PAYLOAD_CODEGEN_TARGET_TYPESCRIPT,
};

enum class ArtifactKind : std::uint32_t {
    source = FDB_PAYLOAD_ARTIFACT_SOURCE,
};

class Access;
class BuildPlan;
class Builder;
class Payload;
class View;

class ObjectHandle final {
public:
    constexpr ObjectHandle() noexcept = default;

private:
    friend class Builder;

    explicit constexpr ObjectHandle(
        fdb_payload_v1_object_handle_t value) noexcept
        : value_(value) {}

    fdb_payload_v1_object_handle_t value_{
        FDB_PAYLOAD_V1_INVALID_OBJECT_HANDLE};
};

struct GraphIdentity final {
    std::uint32_t component_index;
    std::uint64_t object_id;
};

namespace detail {
struct BlobFactory;
struct ErrorFactory;
struct SizeFactory;
std::size_t checked_size(std::uint64_t value, const char* argument);
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

    ByteView bytes() const {
        return {fdb_payload_v1_blob_data(handle_),
                detail::checked_size(fdb_payload_v1_blob_size(handle_),
                                     "blob_size")};
    }

    std::string_view as_string_view() const {
        const ByteView value = bytes();
        if (value.data == nullptr) {
            return {};
        }
        return {reinterpret_cast<const char*>(value.data), value.size};
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
    friend struct detail::SizeFactory;

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

struct BuilderOptions final {
    BuilderOptions() noexcept { fdb_payload_v1_builder_options_init(&value); }

    fdb_payload_v1_builder_options_t value;
};

struct OpenOptions final {
    OpenOptions() noexcept { fdb_payload_v1_open_options_init(&value); }

    fdb_payload_v1_open_options_t value;
};

struct PlanInfo final {
    PlanInfo() noexcept { fdb_payload_v1_plan_info_init(&value); }

    fdb_payload_v1_plan_info_t value;
};

struct ExecutionReport final {
    ExecutionReport() noexcept {
        fdb_payload_v1_execution_report_init(&value);
    }

    fdb_payload_v1_execution_report_t value;
};

struct CodegenOptions final {
    CodegenOptions() noexcept { fdb_payload_v1_codegen_options_init(&value); }

    fdb_payload_v1_codegen_options_t value;
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

struct SizeFactory final {
    [[noreturn]] static void raise(std::uint64_t value,
                                   const char* argument) {
        const std::string details =
            std::string{"{\"actual\":\""} + std::to_string(value) +
            "\",\"argument\":\"" + argument +
            "\",\"reason\":\"native_size_overflow\"}";
        throw PayloadError(
            FDB_PAYLOAD_E_INVALID_ARGUMENT, "INVALID_ARGUMENT", "",
            "FastDB payload ABI length exceeds native size_t", details);
    }
};

inline std::size_t checked_size(std::uint64_t value,
                                const char* argument) {
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (value > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max())) {
            SizeFactory::raise(value, argument);
        }
    }
    return static_cast<std::size_t>(value);
}

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
            checked_size(size, "error_field_size")};
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

class Artifact final {
public:
    const Blob& relative_path_blob() const noexcept { return relative_path_; }
    std::string_view relative_path() const {
        return relative_path_.as_string_view();
    }
    ArtifactKind kind() const noexcept { return kind_; }
    const Blob& bytes_blob() const noexcept { return bytes_; }
    ByteView bytes() const { return bytes_.bytes(); }
    const std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE>&
    sha256() const noexcept {
        return sha256_;
    }

private:
    friend class ArtifactSet;

    Artifact(Blob relative_path, ArtifactKind kind, Blob bytes,
             std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> sha256)
        : relative_path_(std::move(relative_path)), kind_(kind),
          bytes_(std::move(bytes)), sha256_(sha256) {}

    Blob relative_path_;
    ArtifactKind kind_;
    Blob bytes_;
    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> sha256_;
};

class ArtifactSet final {
public:
    ArtifactSet(const ArtifactSet& other) noexcept : handle_(other.handle_) {
        fdb_payload_v1_codegen_result_retain(handle_);
    }
    ArtifactSet(ArtifactSet&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    ArtifactSet& operator=(const ArtifactSet& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_codegen_result_retain(other.handle_);
            fdb_payload_v1_codegen_result_release(handle_);
            handle_ = other.handle_;
        }
        return *this;
    }
    ArtifactSet& operator=(ArtifactSet&& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_codegen_result_release(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    ~ArtifactSet() { fdb_payload_v1_codegen_result_release(handle_); }

    std::uint64_t size() const {
        std::uint64_t result = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_codegen_result_artifact_count(
                          handle_, &result, &error),
                      &error);
        return result;
    }

    Artifact at(std::uint64_t index) const {
        fdb_payload_v1_blob_t* path = nullptr;
        fdb_payload_v1_blob_t* bytes = nullptr;
        fdb_payload_v1_artifact_kind_t kind = UINT32_C(0);
        std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_codegen_result_artifact_relative_path(
                          handle_, index, &path, &error),
                      &error);
        Blob owned_path = detail::BlobFactory::take(path);
        detail::check(fdb_payload_v1_codegen_result_artifact_kind(
                          handle_, index, &kind, &error),
                      &error);
        detail::check(fdb_payload_v1_codegen_result_artifact_bytes(
                          handle_, index, &bytes, &error),
                      &error);
        Blob owned_bytes = detail::BlobFactory::take(bytes);
        detail::check(fdb_payload_v1_codegen_result_artifact_sha256(
                          handle_, index, digest.data(), &error),
                      &error);
        return Artifact(std::move(owned_path), static_cast<ArtifactKind>(kind),
                        std::move(owned_bytes), digest);
    }

private:
    friend class CompiledSpec;

    explicit ArtifactSet(fdb_payload_v1_codegen_result_t* handle) noexcept
        : handle_(handle) {}

    fdb_payload_v1_codegen_result_t* handle_{nullptr};
};

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

    ArtifactSet generate(
        CodegenTarget target,
        const CodegenOptions& options = CodegenOptions{}) const {
        fdb_payload_v1_codegen_result_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_spec_codegen(
                          handle_,
                          static_cast<fdb_payload_v1_codegen_target_t>(target),
                          &options.value, &result, &error),
                      &error);
        return ArtifactSet(result);
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
    friend class Builder;
    friend class Payload;

    explicit CompiledSpec(fdb_payload_v1_spec_t* handle) noexcept
        : handle_(handle) {}

    fdb_payload_v1_spec_t* handle_{nullptr};
};

namespace detail {

template <typename Float, typename Bits>
Bits float_bits(Float value) noexcept {
    static_assert(sizeof(Float) == sizeof(Bits));
    static_assert(std::is_trivially_copyable_v<Float>);
    Bits bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

template <typename Float, typename Bits>
Float float_from_bits(Bits bits) noexcept {
    static_assert(sizeof(Float) == sizeof(Bits));
    static_assert(std::is_trivially_copyable_v<Float>);
    Float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline std::uint64_t abi_size(std::size_t value) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (value > static_cast<std::size_t>(
                        std::numeric_limits<std::uint64_t>::max())) {
            SizeFactory::raise(UINT64_MAX, "native_size");
        }
    }
    return static_cast<std::uint64_t>(value);
}

}  // namespace detail

/*
 * Unique owner of one live C access handle. payload_bytes(), str(), wstr(),
 * and bytes() return non-owning borrows whose lifetime ends when that
 * underlying handle is released. Moving Access transfers the handle and keeps
 * existing borrows valid only while the destination still owns it; destroying
 * that owner or overwriting it by move assignment releases the handle and
 * invalidates every outstanding borrow. Do not query a borrow concurrently
 * with release or overwrite of its owning Access.
 */
class Access final {
public:
    Access() noexcept = default;
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;

    Access(Access&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Access& operator=(Access&& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_access_release(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Access() { fdb_payload_v1_access_release(handle_); }

    /* ByteView::size is the complete payload byte count. */
    ByteView payload_bytes() const {
        const std::uint8_t* data = nullptr;
        std::uint64_t size = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_access_payload_bytes(
                          handle_, &data, &size, &error),
                      &error);
        return {data, detail::checked_size(size, "payload_access_size")};
    }

    /* string_view::size() is the UTF-8 byte count. */
    std::string_view str() const {
        const std::uint8_t* data = nullptr;
        std::uint64_t size = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_access_str(handle_, &data, &size, &error),
            &error);
        const std::size_t native =
            detail::checked_size(size, "str_access_size");
        return data == nullptr
                   ? std::string_view{}
                   : std::string_view{
                         reinterpret_cast<const char*>(data), native};
    }

    /* WideView::size counts uint16_t code units, not bytes. */
    WideView wstr() const {
        const std::uint16_t* data = nullptr;
        std::uint64_t size = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_access_wstr(handle_, &data, &size, &error),
            &error);
        return {data, detail::checked_size(size, "wstr_access_size")};
    }

    /* ByteView::size is the opaque-value byte count. */
    ByteView bytes() const {
        const std::uint8_t* data = nullptr;
        std::uint64_t size = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_access_bytes(handle_, &data, &size, &error),
            &error);
        return {data, detail::checked_size(size, "bytes_access_size")};
    }

private:
    friend class Payload;
    friend class View;

    explicit Access(fdb_payload_v1_access_t* handle) noexcept
        : handle_(handle) {}

    fdb_payload_v1_access_t* handle_{nullptr};
};

class View final {
public:
    View(const View& other) noexcept : handle_(other.handle_) {
        fdb_payload_v1_view_retain(handle_);
    }

    View(View&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    View& operator=(const View& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_view_retain(other.handle_);
            fdb_payload_v1_view_release(handle_);
            handle_ = other.handle_;
        }
        return *this;
    }

    View& operator=(View&& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_view_release(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~View() { fdb_payload_v1_view_release(handle_); }

    ViewKind kind() const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_kind(handle_, &result, &error),
                      &error);
        return static_cast<ViewKind>(result);
    }

    bool is_null() const {
        std::uint8_t result = UINT8_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_is_null(handle_, &result, &error),
                      &error);
        return result != UINT8_C(0);
    }

    std::uint64_t length() const {
        std::uint64_t result = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_length(handle_, &result, &error),
                      &error);
        return result;
    }

    View at(std::uint64_t index) const {
        fdb_payload_v1_view_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_at(handle_, index, &result, &error), &error);
        return View(result);
    }

    std::uint32_t component_index() const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_component_index(
                          handle_, &result, &error),
                      &error);
        return result;
    }

    std::uint32_t field_count() const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_field_count(handle_, &result, &error),
            &error);
        return result;
    }

    View field(std::uint32_t index) const {
        fdb_payload_v1_view_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_field(handle_, index, &result, &error),
            &error);
        return View(result);
    }

    View ref_target() const {
        fdb_payload_v1_view_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_ref_target(handle_, &result, &error), &error);
        return View(result);
    }

    GraphIdentity graph_identity() const {
        GraphIdentity result{UINT32_C(0), UINT64_C(0)};
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_graph_identity(
                          handle_, &result.component_index, &result.object_id,
                          &error),
                      &error);
        return result;
    }

    bool get_bool() const {
        std::uint8_t result = UINT8_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_get_bool(handle_, &result, &error), &error);
        return result != UINT8_C(0);
    }

    std::uint8_t get_u8() const {
        std::uint8_t result = UINT8_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_get_u8(handle_, &result, &error), &error);
        return result;
    }

    std::uint16_t get_u16() const {
        std::uint16_t result = UINT16_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_get_u16(handle_, &result, &error), &error);
        return result;
    }

    std::uint32_t get_u32() const {
        std::uint32_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_get_u32(handle_, &result, &error), &error);
        return result;
    }

    std::int32_t get_i32() const {
        std::int32_t result = INT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_get_i32(handle_, &result, &error), &error);
        return result;
    }

    double get_u8n() const {
        std::uint64_t bits = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_get_u8n_f64_bits(
                          handle_, &bits, &error),
                      &error);
        return detail::float_from_bits<double>(bits);
    }

    double get_u16n() const {
        std::uint64_t bits = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_get_u16n_f64_bits(
                          handle_, &bits, &error),
                      &error);
        return detail::float_from_bits<double>(bits);
    }

    float get_f32() const {
        std::uint32_t bits = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_get_f32_bits(
                          handle_, &bits, &error),
                      &error);
        return detail::float_from_bits<float>(bits);
    }

    double get_f64() const {
        std::uint64_t bits = UINT64_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_get_f64_bits(
                          handle_, &bits, &error),
                      &error);
        return detail::float_from_bits<double>(bits);
    }

    Access acquire() const {
        fdb_payload_v1_access_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_view_acquire(handle_, &result, &error), &error);
        return Access(result);
    }

    View materialize() const {
        fdb_payload_v1_view_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_materialize(
                          handle_, &result, &error),
                      &error);
        return View(result);
    }

    void require_spec_sha256(
        const std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE>& expected)
        const {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_view_require_spec_sha256(
                          handle_, expected.data(), &error),
                      &error);
    }

private:
    friend class Payload;

    explicit View(fdb_payload_v1_view_t* handle) noexcept : handle_(handle) {}

    fdb_payload_v1_view_t* handle_{nullptr};
};

class Payload final {
public:
    static Payload open_copy(
        const CompiledSpec& spec,
        ByteView bytes,
        const OpenOptions& options = OpenOptions{}) {
        fdb_payload_v1_payload_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_payload_open_copy(
                          spec.handle_, bytes.data,
                          detail::abi_size(bytes.size), &options.value,
                          &result, &error),
                      &error);
        return Payload(result);
    }

    static Payload open_external(
        const CompiledSpec& spec,
        ByteView bytes,
        const fdb_payload_v1_backing_v1_t& backing,
        void* owner_token,
        const OpenOptions& options = OpenOptions{}) {
        fdb_payload_v1_payload_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_payload_open_external(
                          spec.handle_, bytes.data,
                          detail::abi_size(bytes.size), &backing,
                          owner_token, &options.value, &result, &error),
                      &error);
        return Payload(result);
    }

    Payload(const Payload& other) noexcept : handle_(other.handle_) {
        fdb_payload_v1_payload_retain(handle_);
    }

    Payload(Payload&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Payload& operator=(const Payload& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_payload_retain(other.handle_);
            fdb_payload_v1_payload_release(handle_);
            handle_ = other.handle_;
        }
        return *this;
    }

    Payload& operator=(Payload&& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_payload_release(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Payload() { fdb_payload_v1_payload_release(handle_); }

    std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> sha256() const {
        std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE> digest{};
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_payload_sha256(handle_, digest.data(), &error),
            &error);
        return digest;
    }

    Profile profile() const {
        fdb_payload_v1_profile_t result = UINT32_C(0);
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_payload_profile(handle_, &result, &error),
            &error);
        return static_cast<Profile>(result);
    }

    ExecutionReport execution_report() const {
        ExecutionReport result;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_payload_execution_report(
                          handle_, &result.value, &error),
                      &error);
        return result;
    }

    Blob binary_blob() const {
        fdb_payload_v1_blob_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_payload_binary_blob(handle_, &result, &error),
            &error);
        return detail::BlobFactory::take(result);
    }

    Access acquire() const {
        fdb_payload_v1_access_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_payload_acquire(handle_, &result, &error),
            &error);
        return Access(result);
    }

    View entry_view(std::uint32_t index) const {
        fdb_payload_v1_view_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_payload_entry_view(
                          handle_, index, &result, &error),
                      &error);
        return View(result);
    }

    void invalidate() const {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_payload_invalidate(handle_, &error),
                      &error);
    }

    void require_spec_sha256(
        const std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE>& expected)
        const {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_payload_require_spec_sha256(
                          handle_, expected.data(), &error),
                      &error);
    }

private:
    friend class BuildPlan;

    explicit Payload(fdb_payload_v1_payload_t* handle) noexcept
        : handle_(handle) {}

    fdb_payload_v1_payload_t* handle_{nullptr};
};

struct BuildResult final {
    Payload payload;
    ExecutionReport report;
};

class BuildPlan final {
public:
    BuildPlan(const BuildPlan& other) noexcept : handle_(other.handle_) {
        fdb_payload_v1_plan_retain(handle_);
    }

    BuildPlan(BuildPlan&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    BuildPlan& operator=(const BuildPlan& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_plan_retain(other.handle_);
            fdb_payload_v1_plan_release(handle_);
            handle_ = other.handle_;
        }
        return *this;
    }

    BuildPlan& operator=(BuildPlan&& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_plan_release(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~BuildPlan() { fdb_payload_v1_plan_release(handle_); }

    PlanInfo info() const {
        PlanInfo result;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_plan_info(handle_, &result.value, &error),
            &error);
        return result;
    }

    BuildResult execute(
        BuildPolicy policy,
        const fdb_payload_v1_backing_v1_t* backing = nullptr) const {
        fdb_payload_v1_payload_t* payload = nullptr;
        ExecutionReport report;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_plan_execute(
                          handle_, static_cast<std::uint32_t>(policy), backing,
                          &payload, &report.value, &error),
                      &error);
        return BuildResult{Payload(payload), report};
    }

private:
    friend class Builder;

    explicit BuildPlan(fdb_payload_v1_plan_t* handle) noexcept
        : handle_(handle) {}

    fdb_payload_v1_plan_t* handle_{nullptr};
};

struct FixedRun final {
    FixedRun() noexcept { fdb_payload_v1_fixed_run_init(&value); }

    fdb_payload_v1_fixed_run_v1_t value;
};

class Builder final {
public:
    static Builder create(
        const CompiledSpec& spec,
        const BuilderOptions& options = BuilderOptions{}) {
        fdb_payload_v1_builder_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_create(
                          spec.handle_, &options.value, &result, &error),
                      &error);
        return Builder(result);
    }

    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;

    Builder(Builder&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Builder& operator=(Builder&& other) noexcept {
        if (this != &other) {
            fdb_payload_v1_builder_release(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Builder() { fdb_payload_v1_builder_release(handle_); }

    void require_spec_sha256(
        const std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE>& expected)
        const {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_require_spec_sha256(
                          handle_, expected.data(), &error),
                      &error);
    }

    Builder& entry_begin(std::uint32_t index, std::uint64_t count) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_entry_begin(
                          handle_, index, count, &error),
                      &error);
        return *this;
    }

    ObjectHandle declare_object(std::uint32_t component_index) {
        fdb_payload_v1_object_handle_t result =
            FDB_PAYLOAD_V1_INVALID_OBJECT_HANDLE;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_object_declare(
                          handle_, component_index, &result, &error),
                      &error);
        return ObjectHandle(result);
    }

    Builder& begin_object_fill(ObjectHandle object) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_object_fill_begin(
                          handle_, object.value_, &error),
                      &error);
        return *this;
    }

    Builder& value_null() {
        return call(fdb_payload_v1_builder_value_null);
    }

    Builder& value_bool(bool value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_bool(
                          handle_, value ? UINT8_C(1) : UINT8_C(0), &error),
                      &error);
        return *this;
    }

    Builder& value_u8(std::uint8_t value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_builder_value_u8(handle_, value, &error), &error);
        return *this;
    }

    Builder& value_u16(std::uint16_t value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_builder_value_u16(handle_, value, &error),
            &error);
        return *this;
    }

    Builder& value_u32(std::uint32_t value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_builder_value_u32(handle_, value, &error),
            &error);
        return *this;
    }

    Builder& value_i32(std::int32_t value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_builder_value_i32(handle_, value, &error),
            &error);
        return *this;
    }

    Builder& value_u8n(double value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_u8n_f64_bits(
                          handle_, detail::float_bits<double, std::uint64_t>(
                                       value),
                          &error),
                      &error);
        return *this;
    }

    Builder& value_u16n(double value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_u16n_f64_bits(
                          handle_, detail::float_bits<double, std::uint64_t>(
                                       value),
                          &error),
                      &error);
        return *this;
    }

    Builder& value_f32(float value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_f32_bits(
                          handle_, detail::float_bits<float, std::uint32_t>(
                                       value),
                          &error),
                      &error);
        return *this;
    }

    Builder& value_f64(double value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_f64_bits(
                          handle_, detail::float_bits<double, std::uint64_t>(
                                       value),
                          &error),
                      &error);
        return *this;
    }

    Builder& value_str(std::string_view value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_str(
                          handle_, reinterpret_cast<const std::uint8_t*>(
                                       value.data()),
                          detail::abi_size(value.size()), &error),
                      &error);
        return *this;
    }

    Builder& value_wstr(WideView value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_wstr(
                          handle_, value.data,
                          detail::abi_size(value.size), &error),
                      &error);
        return *this;
    }

    Builder& value_bytes(ByteView value) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_bytes(
                          handle_, value.data,
                          detail::abi_size(value.size), &error),
                      &error);
        return *this;
    }

    Builder& value_fixed_run(const FixedRun& run) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_fixed_run(
                          handle_, &run.value, &error),
                      &error);
        return *this;
    }

    Builder& value_component_begin() {
        return call(fdb_payload_v1_builder_value_component_begin);
    }

    Builder& value_list_begin(std::uint64_t item_count) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_list_begin(
                          handle_, item_count, &error),
                      &error);
        return *this;
    }

    Builder& value_object(ObjectHandle object) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_object(
                          handle_, object.value_, &error),
                      &error);
        return *this;
    }

    Builder& value_ref(ObjectHandle object) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(fdb_payload_v1_builder_value_ref(
                          handle_, object.value_, &error),
                      &error);
        return *this;
    }

    BuildPlan freeze() {
        fdb_payload_v1_plan_t* result = nullptr;
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(
            fdb_payload_v1_builder_freeze(handle_, &result, &error), &error);
        return BuildPlan(result);
    }

private:
    using UnaryBuilderCall = fdb_payload_v1_status_t (*)(
        fdb_payload_v1_builder_t*, fdb_payload_v1_error_t**);

    explicit Builder(fdb_payload_v1_builder_t* handle) noexcept
        : handle_(handle) {}

    Builder& call(UnaryBuilderCall operation) {
        fdb_payload_v1_error_t* error = nullptr;
        detail::check(operation(handle_, &error), &error);
        return *this;
    }

    fdb_payload_v1_builder_t* handle_{nullptr};
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
