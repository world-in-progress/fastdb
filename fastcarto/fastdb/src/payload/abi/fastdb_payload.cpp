#include <fastdb_payload.h>

#include "payload/abi/Handles.hpp"
#include "payload/error/Error.hpp"
#include "payload/json/JsonPointer.hpp"
#include "payload/json/JsonValue.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace fastdb::payload::abi {
namespace {

detail::ByteView byte_view(std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()),
            static_cast<std::uint64_t>(value.size())};
}

struct OwnedErrorHandle final : fdb_payload_v1_error_t {
    explicit OwnedErrorHandle(error::Error value);

    error::Error value;
};

void destroy_owned_error(fdb_payload_v1_error_t* handle) noexcept {
    delete static_cast<OwnedErrorHandle*>(handle);
}

OwnedErrorHandle::OwnedErrorHandle(error::Error initial_value)
    : fdb_payload_v1_error_t(UINT64_C(1), UINT32_C(0), {}, {}, {}, {},
                             destroy_owned_error),
      value(std::move(initial_value)) {
    code = value.code();
    symbol = byte_view(value.symbol());
    path = byte_view(value.path());
    message = byte_view(value.message());
    details_json = byte_view(value.details_json());
}

template <typename Result, typename Operation>
Result guard_value(Result fallback, Operation&& operation) noexcept {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
        return fallback;
    } catch (const error::Error&) {
        return fallback;
    } catch (const std::exception&) {
        return fallback;
    } catch (...) {
        return fallback;
    }
}

template <typename Operation>
void guard_void(Operation&& operation) noexcept {
    try {
        std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
    } catch (const error::Error&) {
    } catch (const std::exception&) {
    } catch (...) {
    }
}

void write_error_bytes(const fdb_payload_v1_error_t* error,
                       detail::ByteView fdb_payload_v1_error_t::*member,
                       const std::uint8_t** out_data,
                       std::uint64_t* out_size) noexcept {
    if (out_data != nullptr) {
        *out_data = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = UINT64_C(0);
    }
    if (error == nullptr) {
        return;
    }
    const detail::ByteView value = error->*member;
    if (out_data != nullptr) {
        *out_data = value.data;
    }
    if (out_size != nullptr) {
        *out_size = value.size;
    }
}

}  // namespace

fdb_payload_v1_blob_t* make_blob(std::vector<std::uint8_t> bytes) {
    return new fdb_payload_v1_blob_t(std::move(bytes));
}

fdb_payload_v1_error_t* allocation_error() noexcept {
    static constexpr char symbol[] = "ALLOCATION_FAILED";
    static constexpr char path[] = "";
    static constexpr char message[] = "FastDB payload allocation failed";
    static constexpr char details[] = "{}";
    static fdb_payload_v1_error_t emergency{
        std::numeric_limits<std::uint64_t>::max(),
        FDB_PAYLOAD_E_ALLOCATION_FAILED,
        byte_view(symbol),
        byte_view(path),
        byte_view(message),
        byte_view(details),
        nullptr,
    };
    return &emergency;
}

fdb_payload_v1_error_t* make_error(error::Error value) noexcept {
    try {
        return new OwnedErrorHandle(std::move(value));
    } catch (const std::bad_alloc&) {
        return allocation_error();
    } catch (const error::Error&) {
        return allocation_error();
    } catch (const std::exception&) {
        return allocation_error();
    } catch (...) {
        return allocation_error();
    }
}

fdb_payload_v1_status_t publish_error(
    const error::Error& value,
    fdb_payload_v1_error_t** out_error) noexcept {
    if (out_error == nullptr) {
        return FDB_PAYLOAD_E_INVALID_ARGUMENT;
    }
    try {
        *out_error = make_error(value);
    } catch (const std::bad_alloc&) {
        *out_error = allocation_error();
    } catch (const error::Error&) {
        *out_error = allocation_error();
    } catch (const std::exception&) {
        *out_error = allocation_error();
    } catch (...) {
        *out_error = allocation_error();
    }
    return (*out_error)->code;
}

fdb_payload_v1_status_t publish_internal_error(
    const char* message,
    fdb_payload_v1_error_t** out_error) noexcept {
    if (out_error == nullptr) {
        return FDB_PAYLOAD_E_INVALID_ARGUMENT;
    }
    try {
        const error::Error internal = error::Error::from_details(
            FDB_PAYLOAD_E_INTERNAL, json::JsonPointer{}, message,
            json::JsonValue::object({}));
        return publish_error(internal, out_error);
    } catch (const std::bad_alloc&) {
        *out_error = allocation_error();
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    } catch (const error::Error& known) {
        return publish_error(known, out_error);
    } catch (const std::exception&) {
        *out_error = allocation_error();
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    } catch (...) {
        *out_error = allocation_error();
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }
}

}  // namespace fastdb::payload::abi

extern "C" uint32_t fdb_payload_v1_abi_version(void) {
    return fastdb::payload::abi::guard_value(
        UINT32_C(0), []() { return FDB_PAYLOAD_V1_ABI_VERSION; });
}

extern "C" void fdb_payload_v1_compile_options_init(
    fdb_payload_v1_compile_options_t* options) {
    fastdb::payload::abi::guard_void([options]() {
        if (options == nullptr) {
            return;
        }
        const fdb_payload_v1_compile_options_t initialized{
            FDB_PAYLOAD_V1_COMPILE_OPTIONS_V1_SIZE,
            UINT32_C(0),
            UINT64_C(16777216),
            UINT64_C(1000000),
            UINT32_C(128),
            UINT32_C(65536),
            UINT32_C(65536),
            UINT32_C(65536),
            UINT64_C(1000000),
            {UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)},
        };
        *options = initialized;
    });
}

extern "C" void fdb_payload_v1_capabilities_init(
    fdb_payload_v1_capabilities_t* capabilities) {
    fastdb::payload::abi::guard_void([capabilities]() {
        if (capabilities == nullptr) {
            return;
        }
        const fdb_payload_v1_capabilities_t initialized{
            FDB_PAYLOAD_V1_CAPABILITIES_V1_SIZE,
            UINT32_C(0),
            UINT64_C(0),
            UINT64_C(0),
            UINT64_C(0),
            FDB_PAYLOAD_DIRECT_BUILD_NOT_EVALUATED,
            UINT32_C(0),
            {UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)},
        };
        *capabilities = initialized;
    });
}

extern "C" void fdb_payload_v1_blob_retain(fdb_payload_v1_blob_t* blob) {
    fastdb::payload::abi::guard_void([blob]() {
        fastdb::payload::abi::detail::retain_reference(blob);
    });
}

extern "C" void fdb_payload_v1_blob_release(fdb_payload_v1_blob_t* blob) {
    fastdb::payload::abi::guard_void([blob]() {
        fastdb::payload::abi::detail::release_reference(
            blob, [](fdb_payload_v1_blob_t* value) noexcept { delete value; });
    });
}

extern "C" const uint8_t* fdb_payload_v1_blob_data(
    const fdb_payload_v1_blob_t* blob) {
    return fastdb::payload::abi::guard_value(
        static_cast<const std::uint8_t*>(nullptr), [blob]() {
            if (blob == nullptr || blob->bytes.empty()) {
                return static_cast<const std::uint8_t*>(nullptr);
            }
            return blob->bytes.data();
        });
}

extern "C" uint64_t fdb_payload_v1_blob_size(
    const fdb_payload_v1_blob_t* blob) {
    return fastdb::payload::abi::guard_value(UINT64_C(0), [blob]() {
        return blob == nullptr ? UINT64_C(0)
                               : static_cast<std::uint64_t>(blob->bytes.size());
    });
}

extern "C" void fdb_payload_v1_error_retain(fdb_payload_v1_error_t* error) {
    fastdb::payload::abi::guard_void([error]() {
        fastdb::payload::abi::detail::retain_reference(error);
    });
}

extern "C" void fdb_payload_v1_error_release(fdb_payload_v1_error_t* error) {
    fastdb::payload::abi::guard_void([error]() {
        fastdb::payload::abi::detail::release_reference(
            error, [](fdb_payload_v1_error_t* value) noexcept {
                if (value->destroy != nullptr) {
                    value->destroy(value);
                }
            });
    });
}

extern "C" uint32_t fdb_payload_v1_error_code(
    const fdb_payload_v1_error_t* error) {
    return fastdb::payload::abi::guard_value(
        UINT32_C(0), [error]() {
            return error == nullptr ? UINT32_C(0) : error->code;
        });
}

extern "C" void fdb_payload_v1_error_symbol(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size) {
    fastdb::payload::abi::guard_void([=]() {
        fastdb::payload::abi::write_error_bytes(
            error, &fdb_payload_v1_error_t::symbol, out_data, out_size);
    });
}

extern "C" void fdb_payload_v1_error_path(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size) {
    fastdb::payload::abi::guard_void([=]() {
        fastdb::payload::abi::write_error_bytes(
            error, &fdb_payload_v1_error_t::path, out_data, out_size);
    });
}

extern "C" void fdb_payload_v1_error_message(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size) {
    fastdb::payload::abi::guard_void([=]() {
        fastdb::payload::abi::write_error_bytes(
            error, &fdb_payload_v1_error_t::message, out_data, out_size);
    });
}

extern "C" void fdb_payload_v1_error_details_json(
    const fdb_payload_v1_error_t* error,
    const uint8_t** out_data,
    uint64_t* out_size) {
    fastdb::payload::abi::guard_void([=]() {
        fastdb::payload::abi::write_error_bytes(
            error, &fdb_payload_v1_error_t::details_json, out_data, out_size);
    });
}
