#include "GoldenCorpus.hpp"
#include "TestSupport.hpp"

#include "payload/build/PayloadBuilder.hpp"
#include "payload/build/RecordEncoder.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/RecordLayout.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/Open.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace allocation_failure {

std::atomic<std::int64_t> fail_after{INT64_C(-1)};
std::atomic<std::size_t> largest_allocation{0U};
std::atomic<std::size_t> current_live_bytes{0U};
std::atomic<std::size_t> peak_live_bytes{0U};

struct AllocationHeader final {
    void* raw;
    std::size_t requested;
};

constexpr std::size_t kDefaultNewAlignment =
    static_cast<std::size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);
static_assert(kDefaultNewAlignment != 0U);
static_assert((kDefaultNewAlignment & (kDefaultNewAlignment - 1U)) == 0U);
static_assert((alignof(AllocationHeader) &
               (alignof(AllocationHeader) - 1U)) == 0U);

void update_peak(std::atomic<std::size_t>& peak, std::size_t candidate) {
    std::size_t observed = peak.load(std::memory_order_relaxed);
    while (observed < candidate &&
           !peak.compare_exchange_weak(
               observed, candidate, std::memory_order_relaxed)) {
    }
}

void* allocate(std::size_t size,
               std::size_t alignment = kDefaultNewAlignment) {
    update_peak(largest_allocation, size);
    const std::int64_t remaining =
        fail_after.load(std::memory_order_relaxed);
    if (remaining >= INT64_C(0)) {
        if (fail_after.fetch_sub(INT64_C(1), std::memory_order_relaxed) ==
            INT64_C(0)) {
            throw std::bad_alloc();
        }
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        throw std::bad_alloc();
    }
    alignment = std::max(
        {alignment, alignof(AllocationHeader), kDefaultNewAlignment});
    const std::size_t payload_size = size == 0U ? 1U : size;
    const std::size_t padding = alignment - 1U;
    constexpr std::size_t maximum =
        std::numeric_limits<std::size_t>::max();
    if (padding > maximum - sizeof(AllocationHeader)) {
        throw std::bad_alloc();
    }
    const std::size_t overhead = sizeof(AllocationHeader) + padding;
    if (payload_size > maximum - overhead) {
        throw std::bad_alloc();
    }
    const std::size_t allocation_size = payload_size + overhead;
    void* const raw = std::malloc(allocation_size);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    void* candidate = static_cast<void*>(
        static_cast<std::uint8_t*>(raw) + sizeof(AllocationHeader));
    std::size_t space = allocation_size - sizeof(AllocationHeader);
    void* const aligned =
        std::align(alignment, payload_size, candidate, space);
    if (aligned == nullptr) {
        std::free(raw);
        throw std::bad_alloc();
    }
    auto* const header =
        static_cast<AllocationHeader*>(aligned) - 1;
    header->raw = raw;
    header->requested = size;
    const std::size_t live =
        current_live_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    update_peak(peak_live_bytes, live);
    return aligned;
}

void deallocate(void* value) noexcept {
    if (value == nullptr) {
        return;
    }
    auto* const header =
        reinterpret_cast<AllocationHeader*>(value) - 1;
    current_live_bytes.fetch_sub(header->requested,
                                 std::memory_order_relaxed);
    std::free(header->raw);
}

}  // namespace allocation_failure

void* operator new(std::size_t size) {
    return allocation_failure::allocate(size);
}
void* operator new[](std::size_t size) {
    return allocation_failure::allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocation_failure::allocate(
        size, static_cast<std::size_t>(alignment));
}
void operator delete(void* value) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::size_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete(void* value, std::size_t,
                     std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value, std::size_t,
                       std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}

namespace fastdb::payload::layout {

struct RuntimeSchemaTestAccess final {
    static void remove_last_list(RuntimeSchema& schema) {
        if (!schema.list_nodes_.empty()) {
            schema.list_nodes_.pop_back();
        }
    }

    static void make_first_list_cyclic(RuntimeSchema& schema) {
        if (!schema.list_nodes_.empty()) {
            schema.list_nodes_.front().item_runtime_type_id =
                schema.list_nodes_.front().owner_runtime_type_id;
        }
    }
};

}  // namespace fastdb::payload::layout

namespace {

using fastdb::payload::build::ByteSink;
using fastdb::payload::build::LogicalPayload;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::layout::RecordLayout;
using fastdb::payload::layout::RuntimeSchema;
using fastdb::payload::spec::CompiledSpec;
using fastdb::test::payload::BinaryGoldenCase;

std::uint8_t hex_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    return static_cast<std::uint8_t>(value - 'a' + 10);
}

std::vector<std::uint8_t> decode_hex(std::string_view hexadecimal) {
    std::vector<std::uint8_t> bytes(hexadecimal.size() / 2U, UINT8_C(0));
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            (hex_nibble(hexadecimal[index * 2U]) << 4U) |
            hex_nibble(hexadecimal[index * 2U + 1U]));
    }
    return bytes;
}

std::string read_binary_fixture(std::string_view relative) {
    const std::string path =
        std::string(FASTDB_PAYLOAD_BINARY_FIXTURE_DIR) + "/" +
        std::string(relative);
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string component_id(std::uint32_t index) {
    std::string digits = std::to_string(index);
    return "c" + std::string(5U - digits.size(), '0') + digits;
}

std::string doubling_component_spec(bool reachable) {
    constexpr std::uint32_t component_count = UINT32_C(34);
    std::string source =
        "{\"schema\":\"fastdb.payload.v1\",\"profile\":\"record.v1\",";
    if (reachable) {
        source +=
            "\"entries\":[{\"id\":\"root\",\"cardinality\":\"one\","
            "\"type\":{\"kind\":\"component\",\"id\":\"c00000\"}}],";
    } else {
        source +=
            "\"entries\":[{\"id\":\"root\",\"cardinality\":\"one\","
            "\"type\":{\"kind\":\"u8\"}}],";
    }
    source += "\"components\":[";
    for (std::uint32_t index = UINT32_C(0); index < component_count; ++index) {
        if (index != UINT32_C(0)) {
            source += ',';
        }
        source += "{\"id\":\"" + component_id(index) +
                  "\",\"kind\":\"record\",\"fields\":[";
        if (index + UINT32_C(1) < component_count) {
            const std::string next = component_id(index + UINT32_C(1));
            source +=
                "{\"id\":\"left\",\"type\":{\"kind\":\"component\","
                "\"id\":\"" +
                next +
                "\"}},{\"id\":\"right\",\"type\":{\"kind\":"
                "\"component\",\"id\":\"" +
                next + "\"}}";
        }
        source += "]}";
    }
    source += "]}";
    return source;
}

std::string deep_list_spec(std::uint32_t depth) {
    std::string source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"deep","cardinality":"one","type":)";
    for (std::uint32_t index = UINT32_C(0); index < depth; ++index) {
        source += R"({"kind":"list","items":)";
    }
    source += R"({"kind":"u8"})";
    source.append(static_cast<std::size_t>(depth), '}');
    source += R"(}],"components":[]})";
    return source;
}

class VectorSink final : public ByteSink {
public:
    explicit VectorSink(std::uint64_t size)
        : bytes_(static_cast<std::size_t>(size), UINT8_C(0xa5)) {}

    Result<void> write(std::uint64_t offset,
                       const std::uint8_t* data,
                       std::uint64_t size) override {
        if ((data == nullptr && size != UINT64_C(0)) ||
            offset != next_offset_ || size > bytes_.size() - next_offset_) {
            return Result<void>::failure(
                fastdb::payload::error::Error::from_details(
                    FDB_PAYLOAD_E_OUT_OF_BOUNDS,
                    JsonPointer{}.append("sink"), "Invalid test sink write",
                    fastdb::payload::json::JsonValue::object({})));
        }
        std::copy_n(data, static_cast<std::size_t>(size),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        next_offset_ += size;
        ++write_count_;
        return Result<void>::success();
    }

    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    std::uint64_t next_offset() const noexcept { return next_offset_; }
    std::uint64_t write_count() const noexcept { return write_count_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t next_offset_{UINT64_C(0)};
    std::uint64_t write_count_{UINT64_C(0)};
};

Result<LogicalPayload> build_scenario(CompiledSpec spec,
                                      std::string_view scenario) {
    auto created = PayloadBuilder::create(std::move(spec));
    if (!created.has_value()) {
        return Result<LogicalPayload>::failure(std::move(created).error());
    }
    PayloadBuilder& builder = created.value();
    if (scenario == "empty") {
        return builder.freeze();
    }

#define FASTDB_BINARY_SCENARIO_STEP(expression)                              \
    do {                                                                     \
        auto step_result = (expression);                                     \
        if (!step_result.has_value()) {                                      \
            return Result<LogicalPayload>::failure(                          \
                std::move(step_result).error());                             \
        }                                                                    \
    } while (false)

    if (scenario == "component_list_empty") {
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(1), UINT64_C(0)));
        return builder.freeze();
    }

    if (scenario == "numeric_edges" ||
        scenario == "numeric_edges_alt_nan") {
        const bool alternate_nan = scenario == "numeric_edges_alt_nan";
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(1), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_MAX));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(2), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_MAX));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(3), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_MAX));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(4), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_MIN));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_MAX));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(5), UINT64_C(6)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0x80000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0x7f800000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0xff800000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(alternate_nan ? UINT32_C(0x7fffffff)
                                                : UINT32_C(0x7fa12345)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(alternate_nan ? UINT32_C(0xff800001)
                                                : UINT32_C(0xffdabcde)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(6), UINT64_C(6)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_f64_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x8000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0xfff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(
                alternate_nan ? UINT64_C(0x7fffffffffffffff)
                              : UINT64_C(0x7ff0000000000042)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(
                alternate_nan ? UINT64_C(0xfff0000000000001)
                              : UINT64_C(0xfff8000000001234)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(7), UINT64_C(6)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x0000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x8000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x406fe00000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x3ff8000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x0000000000000001)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(8), UINT64_C(4)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0xc0e0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x40dfffc000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0xc0dfffe000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0xc0dfffa000000000)));
        return builder.freeze();
    }

    if (scenario == "nested_components") {
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x40efffe000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x7ff0000000000042)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x3fe0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x8000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_MAX));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(1), UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x0000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x3ff8000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0xfff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(0)));

        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x40efffe000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_f64_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x3ff8000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0xfff8000000001234)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(42)));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(2), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        return builder.freeze();
    }

    if (scenario == "text_bytes") {
        const std::array<std::uint16_t, 1> wide_a{{UINT16_C(0x0041)}};
        const std::array<std::uint16_t, 2> wide_bmp_nul{
            {UINT16_C(0x4e2d), UINT16_C(0x0000)}};
        const std::array<std::uint16_t, 2> wide_supplementary{
            {UINT16_C(0xd83d), UINT16_C(0xde03)}};
        const std::array<std::uint16_t, 5> wide_nested{
            {UINT16_C(0x0041), UINT16_C(0x4e2d), UINT16_C(0xd83d),
             UINT16_C(0xde03), UINT16_C(0x0000)}};
        const std::array<std::uint8_t, 3> opaque{
            {UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x80)}};
        const std::array<std::uint8_t, 4> nested_opaque{
            {UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x80), UINT8_C(0x41)}};
        const std::array<std::uint8_t, 2> suffix{
            {UINT8_C(0x01), UINT8_C(0x02)}};

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(7)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str(""));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("ASCII"));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_str(std::string_view{"\xe4\xb8\xad\0", 4U}));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("repeat"));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("repeat"));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("tail"));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(1), UINT64_C(6)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_wstr(nullptr, UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_wstr(wide_a.data(), wide_a.size()));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_wstr(wide_bmp_nul.data(), wide_bmp_nul.size()));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_wstr(
            wide_supplementary.data(), wide_supplementary.size()));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_wstr(wide_a.data(), wide_a.size()));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(2), UINT64_C(6)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bytes(nullptr, UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_bytes(opaque.data(), opaque.size()));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_bytes(opaque.data(), opaque.size()));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bytes(
            reinterpret_cast<const std::uint8_t*>("A\0"), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bytes(nullptr, UINT64_C(0)));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(3), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("P"));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("I"));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_wstr(wide_nested.data(), wide_nested.size()));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_bytes(nested_opaque.data(), nested_opaque.size()));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_bytes(suffix.data(), suffix.size()));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str(""));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("repeat"));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_wstr(wide_a.data(), wide_a.size()));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_bytes(nested_opaque.data(), nested_opaque.size()));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bytes(nullptr, UINT64_C(0)));
        return builder.freeze();
    }

    if (scenario == "nested_lists") {
        const std::array<std::uint16_t, 3> wide{{UINT16_C(0x0041),
                                                 UINT16_C(0xd83d),
                                                 UINT16_C(0xde03)}};
        const std::array<std::uint8_t, 3> opaque{{UINT8_C(0x00),
                                                  UINT8_C(0xff),
                                                  UINT8_C(0x41)}};
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str(""));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("alpha"));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));

        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_wstr(nullptr, UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_wstr(wide.data(), wide.size()));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_bytes(opaque.data(), opaque.size()));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(1), UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(7)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(9)));

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(2), UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str(""));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("tail"));
        return builder.freeze();
    }

    if (scenario == "component_list_composition") {
        const std::array<std::uint16_t, 1> wide{{UINT16_C(0x4e2d)}};
        const std::array<std::uint8_t, 3> opaque{{UINT8_C(0x00),
                                                  UINT8_C(0xff),
                                                  UINT8_C(0x80)}};
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(8)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(16)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(32)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_C(-32)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16n_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0x3f800000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x4000000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("component"));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_wstr(wide.data(), wide.size()));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_bytes(opaque.data(), opaque.size()));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_MAX));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_MAX));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_MAX));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_MIN));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_MAX));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8n_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u8n_bits(UINT64_C(0x3ff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0xbff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_u16n_bits(UINT64_C(0x3ff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0x80000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f32_bits(UINT32_C(0x7fa12345)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_f64_bits(UINT64_C(0xfff8000000001234)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str(""));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("list"));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_wstr(wide.data(), wide.size()));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(
            builder.push_bytes(opaque.data(), opaque.size()));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("two"));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(9)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("root"));

        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8n_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16n_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_f64_bits(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str(""));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_wstr(nullptr, UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_bytes(nullptr, UINT64_C(0)));
        for (std::uint32_t field = UINT32_C(0); field < UINT32_C(12);
             ++field) {
            FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        }
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());

        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(1), UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(7)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_str("entry"));
        return builder.freeze();
    }

    if (scenario == "simple_lists") {
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(4)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(3)));
        return builder.freeze();
    }

    if (scenario == "zero_count_list_aggregate") {
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(2)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        return builder.freeze();
    }

    if (scenario == "wide_lists") {
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(7)));
        return builder.freeze();
    }

    if (scenario == "wide_budget_list") {
        constexpr std::uint64_t width = UINT64_C(4096);
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(width));
        for (std::uint64_t index = UINT64_C(0); index < width; ++index) {
            FASTDB_BINARY_SCENARIO_STEP(
                builder.push_u8(static_cast<std::uint8_t>(index)));
        }
        return builder.freeze();
    }

    if (scenario == "wide_budget_many") {
        constexpr std::uint64_t width = UINT64_C(4096);
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), width));
        for (std::uint64_t index = UINT64_C(0); index < width; ++index) {
            FASTDB_BINARY_SCENARIO_STEP(
                builder.push_u8(static_cast<std::uint8_t>(index)));
        }
        return builder.freeze();
    }

    if (scenario == "wide_budget_component") {
        constexpr std::uint64_t width = UINT64_C(4096);
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_component());
        for (std::uint64_t index = UINT64_C(0); index < width; ++index) {
            FASTDB_BINARY_SCENARIO_STEP(
                builder.push_u8(static_cast<std::uint8_t>(index)));
        }
        return builder.freeze();
    }

    if (scenario == "nested_simple") {
        FASTDB_BINARY_SCENARIO_STEP(
            builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_BINARY_SCENARIO_STEP(builder.begin_list(UINT64_C(1)));
        FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(7)));
        return builder.freeze();
    }

    if (scenario != "fixed_scalars") {
        return Result<LogicalPayload>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INVALID_ARGUMENT,
                JsonPointer{}.append("scenario"), "Unknown binary scenario",
                fastdb::payload::json::JsonValue::object({})));
    }

    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(0), UINT64_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_bool(UINT8_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(1), UINT64_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_u8(UINT8_C(0xab)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(2), UINT64_C(3)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_C(0x1234)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_null());
    FASTDB_BINARY_SCENARIO_STEP(builder.push_u16(UINT16_MAX));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(3), UINT64_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_u32(UINT32_C(0x12345678)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(4), UINT64_C(1)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_i32(INT32_C(-2)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(5), UINT64_C(4)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0x80000000)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0x7f800000)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0xff800000)));
    FASTDB_BINARY_SCENARIO_STEP(builder.push_f32_bits(UINT32_C(0x7fa12345)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.begin_entry(UINT32_C(6), UINT64_C(4)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.push_f64_bits(UINT64_C(0x8000000000000000)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.push_f64_bits(UINT64_C(0x7ff0000000000000)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.push_f64_bits(UINT64_C(0xfff0000000000000)));
    FASTDB_BINARY_SCENARIO_STEP(
        builder.push_f64_bits(UINT64_C(0x7ff0000000000042)));
#undef FASTDB_BINARY_SCENARIO_STEP
    return builder.freeze();
}

struct Encoded final {
    CompiledSpec spec;
    RecordLayout layout;
    std::vector<std::uint8_t> bytes;
    std::uint64_t writes;
};

Result<Encoded> encode_case(const BinaryGoldenCase& item) {
    auto compiled = CompiledSpec::compile(item.success.source);
    if (!compiled.has_value()) {
        return Result<Encoded>::failure(std::move(compiled).error());
    }
    CompiledSpec spec = compiled.value();
    auto values = build_scenario(compiled.value(), item.success.scenario);
    if (!values.has_value()) {
        return Result<Encoded>::failure(std::move(values).error());
    }
    auto runtime = RuntimeSchema::compile(spec);
    if (!runtime.has_value()) {
        return Result<Encoded>::failure(std::move(runtime).error());
    }
    auto layout = RecordLayout::plan(runtime.value(), values.value());
    if (!layout.has_value()) {
        return Result<Encoded>::failure(std::move(layout).error());
    }
    VectorSink sink(layout.value().total_length());
    auto encoded = fastdb::payload::build::encode_record(
        layout.value(), values.value(), sink);
    if (!encoded.has_value()) {
        return Result<Encoded>::failure(std::move(encoded).error());
    }
    if (sink.next_offset() != layout.value().total_length()) {
        return Result<Encoded>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("sink"),
                "Encoder did not consume sink",
                fastdb::payload::json::JsonValue::object({})));
    }
    return Result<Encoded>::success(
        Encoded{std::move(spec), std::move(layout).value(), sink.bytes(),
                sink.write_count()});
}

Result<Encoded> encode_source_scenario(std::string_view source,
                                       std::string_view scenario) {
    auto compiled = CompiledSpec::compile(source);
    if (!compiled.has_value()) {
        return Result<Encoded>::failure(std::move(compiled).error());
    }
    CompiledSpec spec = compiled.value();
    auto values = build_scenario(compiled.value(), scenario);
    if (!values.has_value()) {
        return Result<Encoded>::failure(std::move(values).error());
    }
    auto runtime = RuntimeSchema::compile(spec);
    if (!runtime.has_value()) {
        return Result<Encoded>::failure(std::move(runtime).error());
    }
    auto planned = RecordLayout::plan(runtime.value(), values.value());
    if (!planned.has_value()) {
        return Result<Encoded>::failure(std::move(planned).error());
    }
    VectorSink sink(planned.value().total_length());
    auto encoded = fastdb::payload::build::encode_record(
        planned.value(), values.value(), sink);
    if (!encoded.has_value()) {
        return Result<Encoded>::failure(std::move(encoded).error());
    }
    return Result<Encoded>::success(
        Encoded{std::move(spec), std::move(planned).value(), sink.bytes(),
                sink.write_count()});
}

const BinaryGoldenCase* find_case(const std::vector<BinaryGoldenCase>& cases,
                                  std::string_view name) {
    const auto found = std::find_if(
        cases.begin(), cases.end(), [name](const BinaryGoldenCase& item) {
            return item.name == name;
        });
    return found == cases.end() ? nullptr : &*found;
}

int test_binary_goldens_determinism_hash_and_headers() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    require(corpus.size() == 8U);
    for (const BinaryGoldenCase& item : corpus) {
        auto first = encode_case(item);
        auto second = encode_case(item);
        require(first.has_value(),
                first.has_value()
                    ? item.name
                    : item.name + ":" +
                          std::to_string(first.error().code()) + ":" +
                          std::string(first.error().path()) + ":" +
                          std::string(first.error().details_json()));
        require(second.has_value());
        const auto golden = decode_hex(item.success.binary_hex);
        require(first.value().bytes == golden, item.name);
        require(second.value().bytes == golden, item.name);
        require(first.value().bytes == second.value().bytes);
        require(first.value().writes > UINT64_C(0));
        require(fastdb::payload::identity::sha256_lower_hex(
                    fastdb::payload::identity::sha256(
                        golden.data(), golden.size())) == item.success.sha256);
        require(std::equal(first.value().spec.digest().begin(),
                           first.value().spec.digest().end(),
                           golden.begin() +
                               static_cast<std::ptrdiff_t>(
                                   fastdb::payload::layout::header_spec_digest_offset)));
        auto opened = fastdb::payload::view::open_record(
            first.value().spec, golden.data(), golden.size());
        require(opened.has_value());
        require(opened.value().total_length() == golden.size());
        require(opened.value().validation_work() ==
                    first.value().layout.validation_work(),
                item.name + ":opened=" +
                    std::to_string(opened.value().validation_work()) +
                    ":planned=" +
                    std::to_string(first.value().layout.validation_work()));
        auto exact_limits = fastdb::payload::view::default_open_options();
        exact_limits.max_validation_work =
            first.value().layout.validation_work();
        require(fastdb::payload::view::open_record(
                    first.value().spec, golden.data(), golden.size(),
                    exact_limits)
                    .has_value());
        auto short_limits = exact_limits;
        short_limits.max_validation_work -= UINT64_C(1);
        auto limited = fastdb::payload::view::open_record(
            first.value().spec, golden.data(), golden.size(), short_limits);
        require(!limited.has_value());
        require(limited.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
        const std::string expected_short_path =
            item.name == "empty"               ? "/binary/header"
            : item.name == "fixed-scalars"     ? "/binary/regions/2"
            : item.name == "numeric-edges"     ? "/binary/header/total_length"
            : item.name == "nested-components" ? "/entries/c_empty_many/1"
            : item.name == "text-bytes" ? "/entries/nested/1/suffix"
            : item.name == "nested-lists" ? "/entries/matrices/2/2"
            : item.name == "component-list-empty"
                ? "/binary/header"
                : "/entries/leaf_lists/2/1/label";
        require(limited.error().path() == expected_short_path,
                item.name + ":" + std::string(limited.error().path()));
        require(limited.error().details_json() ==
                "{\"actual\":\"" +
                    std::to_string(first.value().layout.validation_work()) +
                    "\",\"limit\":\"" +
                    std::to_string(
                        first.value().layout.validation_work() - UINT64_C(1)) +
                    "\",\"resource\":\"validation_work\"}");
    }

    const BinaryGoldenCase* empty = find_case(corpus, "empty");
    const BinaryGoldenCase* fixed = find_case(corpus, "fixed-scalars");
    require(empty != nullptr && fixed != nullptr);
    auto empty_encoded = encode_case(*empty);
    auto fixed_encoded = encode_case(*fixed);
    require(empty_encoded.value().layout.region_count() == UINT32_C(0));
    require(empty_encoded.value().layout.entry_count() == UINT32_C(0));
    require(empty_encoded.value().layout.total_length() == UINT64_C(128));
    require(fixed_encoded.value().layout.region_count() == UINT32_C(8));
    require(fixed_encoded.value().layout.entry_count() == UINT32_C(7));
    require(fastdb::payload::layout::load_u64_le(
                fixed_encoded.value().bytes.data(),
                fixed_encoded.value().bytes.size(),
                fastdb::payload::layout::header_total_length_offset,
                JsonPointer{})
                .value() == UINT64_C(928));
    require(fastdb::payload::layout::load_u64_le(
                fixed_encoded.value().bytes.data(),
                fixed_encoded.value().bytes.size(),
                fastdb::payload::layout::header_entry_directory_offset,
                JsonPointer{})
                .value() == UINT64_C(576));
    return EXIT_SUCCESS;
}

int test_digest_equal_independent_specs_share_stable_wire_layout() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* fixed = find_case(corpus, "fixed-scalars");
    require(fixed != nullptr);
    auto runtime_spec = CompiledSpec::compile(fixed->success.source);
    auto values_spec = CompiledSpec::compile(fixed->success.source);
    require(runtime_spec.has_value());
    require(values_spec.has_value());
    require(runtime_spec.value().digest() == values_spec.value().digest());
    require(&runtime_spec.value().resolved().entries()[0].type !=
            &values_spec.value().resolved().entries()[0].type);
    auto runtime = RuntimeSchema::compile(runtime_spec.value());
    auto values = build_scenario(values_spec.value(), fixed->success.scenario);
    require(runtime.has_value());
    require(values.has_value());
    auto planned = RecordLayout::plan(runtime.value(), values.value());
    require(planned.has_value());
    VectorSink sink(planned.value().total_length());
    require(fastdb::payload::build::encode_record(
                planned.value(), values.value(), sink)
                .has_value());
    require(sink.bytes() == decode_hex(fixed->success.binary_hex));
    return EXIT_SUCCESS;
}

std::string exact_resource_details(std::uint64_t actual,
                                   std::uint64_t limit,
                                   std::string_view resource) {
    return "{\"actual\":\"" + std::to_string(actual) +
           "\",\"limit\":\"" + std::to_string(limit) +
           "\",\"resource\":\"" + std::string(resource) + "\"}";
}

int require_resource_limit(
    const Result<fastdb::payload::view::PayloadIndex>& result,
    std::string_view path,
    std::uint64_t actual,
    std::uint64_t limit,
    std::string_view resource) {
    require(!result.has_value());
    require(result.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(result.error().path() == path,
            std::string(result.error().path()) + ":" +
                std::string(result.error().details_json()));
    require(result.error().details_json() ==
            exact_resource_details(actual, limit, resource));
    return EXIT_SUCCESS;
}

int require_reason_error(
    const Result<fastdb::payload::view::PayloadIndex>& result,
    std::uint32_t code, std::string_view path, std::string_view reason);

int require_exact_error(
    const Result<fastdb::payload::view::PayloadIndex>& result,
    std::uint32_t code,
    std::string_view path,
    std::uint64_t actual,
    std::uint64_t expected,
    std::string_view reason) {
    require(!result.has_value());
    require(result.error().code() == code);
    require(result.error().path() == path,
            std::string(result.error().path()) + ":" +
                std::string(result.error().details_json()));
    require(result.error().details_json() ==
            "{\"actual\":\"" + std::to_string(actual) +
                "\",\"expected\":\"" + std::to_string(expected) +
                "\",\"reason\":\"" + std::string(reason) + "\"}");
    return EXIT_SUCCESS;
}

int test_open_preflights_static_spec_limits_and_known_work() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* fixed = find_case(corpus, "fixed-scalars");
    require(fixed != nullptr);
    auto compiled = CompiledSpec::compile(fixed->success.source);
    require(compiled.has_value());
    const auto bytes = decode_hex(fixed->success.binary_hex);

    auto limits = fastdb::payload::view::default_open_options();
    limits.max_entries = UINT64_C(6);
    require(require_resource_limit(fastdb::payload::view::open_record(
                                       compiled.value(), bytes.data(),
                                       bytes.size(), limits),
                                   "/binary/header/entry_count", UINT64_C(7),
                                   UINT64_C(6),
                                   "entries") == EXIT_SUCCESS);

    limits = fastdb::payload::view::default_open_options();
    limits.max_regions = UINT64_C(7);
    require(require_resource_limit(fastdb::payload::view::open_record(
                                       compiled.value(), bytes.data(),
                                       bytes.size(), limits),
                                   "/binary/header/region_count", UINT64_C(8),
                                   UINT64_C(7),
                                   "regions") == EXIT_SUCCESS);

    limits = fastdb::payload::view::default_open_options();
    limits.max_validation_work = UINT64_C(15);
    require(require_resource_limit(fastdb::payload::view::open_record(
                                       compiled.value(), bytes.data(),
                                       bytes.size(), limits),
                                   "/binary/header", UINT64_C(16),
                                   UINT64_C(15), "validation_work") ==
            EXIT_SUCCESS);

    std::string wide_source = doubling_component_spec(true);
    auto wide = CompiledSpec::compile(wide_source);
    require(wide.has_value());
    auto wide_bytes = bytes;
    std::copy(wide.value().digest().begin(), wide.value().digest().end(),
              wide_bytes.begin() + static_cast<std::ptrdiff_t>(
                                       fastdb::payload::layout::header_spec_digest_offset));
    limits = fastdb::payload::view::default_open_options();
    limits.max_components = UINT64_C(33);
    require(require_resource_limit(fastdb::payload::view::open_record(
                                       wide.value(), wide_bytes.data(),
                                       wide_bytes.size(), limits),
                                   "/binary/header", UINT64_C(34),
                                   UINT64_C(33),
                                   "components") == EXIT_SUCCESS);

    auto bad_magic = bytes;
    bad_magic[0] ^= UINT8_C(1);
    limits = fastdb::payload::view::default_open_options();
    limits.max_entries = UINT64_C(0);
    require(fastdb::payload::view::open_record(
                compiled.value(), bad_magic.data(), bad_magic.size(), limits)
                .error()
                .code() == FDB_PAYLOAD_E_INVALID_MAGIC);
    return EXIT_SUCCESS;
}

int require_partition_error(
    const Result<fastdb::payload::view::PayloadIndex>& result,
    std::string_view path,
    std::uint64_t expected,
    std::uint64_t actual,
    std::string_view reason) {
    require(!result.has_value());
    require(result.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);
    require(result.error().path() == path,
            std::string(result.error().path()) + ":" +
                std::string(result.error().details_json()));
    require(result.error().details_json() ==
            "{\"actual\":\"" + std::to_string(actual) +
                "\",\"expected\":\"" + std::to_string(expected) +
                "\",\"reason\":\"" + std::string(reason) + "\"}");
    return EXIT_SUCCESS;
}

int test_region_matrix_zero_boundaries_and_partition_rules() {
    using fastdb::payload::layout::RegionCountUnit;
    using fastdb::payload::layout::RegionAlignmentRule;
    using fastdb::payload::layout::RegionKind;
    using fastdb::payload::layout::RegionOwnerRule;
    using fastdb::payload::layout::RegionStrideRule;
    using fastdb::payload::layout::RegionTypeRule;
    constexpr std::array<RegionKind, 7> kinds{{
        RegionKind::entry_values, RegionKind::entry_validity,
        RegionKind::list_items, RegionKind::list_validity,
        RegionKind::utf8_pool, RegionKind::utf16_pool,
        RegionKind::bytes_pool}};
    constexpr std::array<std::uint32_t, 7> numbers{{UINT32_C(1), UINT32_C(2),
                                                    UINT32_C(3), UINT32_C(4),
                                                    UINT32_C(5), UINT32_C(6),
                                                    UINT32_C(7)}};
    for (std::size_t index = 0U; index < kinds.size(); ++index) {
        const auto rule = fastdb::payload::layout::region_rule(kinds[index]);
        require(rule.kind_value == numbers[index]);
    }
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .count_unit == RegionCountUnit::values);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .owner_rule == RegionOwnerRule::entry_index);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .type_rule ==
            RegionTypeRule::entry_root_runtime_type_id);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .stride_rule == RegionStrideRule::slot_stride);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_values)
                .alignment_rule == RegionAlignmentRule::slot_alignment);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_validity)
                .is_validity);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_validity)
                .stride_rule == RegionStrideRule::zero);
    require(fastdb::payload::layout::region_rule(RegionKind::entry_validity)
                .alignment_rule == RegionAlignmentRule::one);
    require(fastdb::payload::layout::region_rule(RegionKind::list_items)
                .count_unit == RegionCountUnit::items);
    require(fastdb::payload::layout::region_rule(RegionKind::list_items)
                .owner_rule == RegionOwnerRule::list_runtime_type_id);
    require(fastdb::payload::layout::region_rule(RegionKind::list_items)
                .type_rule == RegionTypeRule::list_item_runtime_type_id);
    require(fastdb::payload::layout::region_rule(RegionKind::list_items)
                .stride_rule == RegionStrideRule::slot_stride);
    require(fastdb::payload::layout::region_rule(RegionKind::list_validity)
                .is_validity);
    require(fastdb::payload::layout::region_rule(RegionKind::list_validity)
                .alignment_rule == RegionAlignmentRule::one);
    require(fastdb::payload::layout::region_rule(RegionKind::utf8_pool)
                .count_unit == RegionCountUnit::bytes);
    require(fastdb::payload::layout::region_rule(RegionKind::utf8_pool)
                .owner_rule == RegionOwnerRule::sentinel);
    require(fastdb::payload::layout::region_rule(RegionKind::utf8_pool)
                .type_rule == RegionTypeRule::sentinel);
    require(fastdb::payload::layout::region_rule(RegionKind::utf16_pool)
                .count_unit == RegionCountUnit::utf16_code_units);
    require(fastdb::payload::layout::region_rule(RegionKind::utf16_pool)
                .alignment_rule == RegionAlignmentRule::two);
    require(fastdb::payload::layout::region_rule(RegionKind::bytes_pool)
                .count_unit == RegionCountUnit::bytes);
    require(fastdb::payload::layout::region_rule(RegionKind::bytes_pool)
                .alignment_rule == RegionAlignmentRule::one);

    const JsonPointer path = JsonPointer{}.append("partition");
    require(fastdb::payload::layout::checked_partition_advance(
                UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(4), path)
                .value() == UINT64_C(0));
    require(fastdb::payload::layout::checked_partition_advance(
                UINT64_C(0), UINT64_C(1), UINT64_C(1), UINT64_C(4), path)
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);
    require(fastdb::payload::layout::checked_partition_advance(
                UINT64_C(0), UINT64_C(0), UINT64_C(3), UINT64_C(4), path)
                .value() == UINT64_C(3));
    require(fastdb::payload::layout::require_partition_consumed(
                UINT64_C(3), UINT64_C(4), path)
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);
    require(fastdb::payload::layout::require_partition_consumed(
                UINT64_C(4), UINT64_C(4), path)
                .has_value());
    std::uint64_t pool_cursor = UINT64_C(0);
    pool_cursor = fastdb::payload::layout::checked_partition_advance(
                      pool_cursor, UINT64_C(0), UINT64_C(0), UINT64_C(3), path)
                      .value();
    pool_cursor = fastdb::payload::layout::checked_partition_advance(
                      pool_cursor, UINT64_C(0), UINT64_C(3), UINT64_C(3), path)
                      .value();
    require(fastdb::payload::layout::require_partition_consumed(
                pool_cursor, UINT64_C(3), path)
                .has_value());
    std::uint64_t list_cursor = UINT64_C(0);
    list_cursor = fastdb::payload::layout::checked_partition_advance(
                      list_cursor, UINT64_C(0), UINT64_C(2), UINT64_C(4), path)
                      .value();
    list_cursor = fastdb::payload::layout::checked_partition_advance(
                      list_cursor, UINT64_C(2), UINT64_C(2), UINT64_C(4), path)
                      .value();
    require(fastdb::payload::layout::require_partition_consumed(
                list_cursor, UINT64_C(4), path)
                .has_value());

    auto compiled = CompiledSpec::compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"empty","cardinality":"many","type":{"kind":"u8"}}],"components":[]})");
    require(compiled.has_value());
    auto values = build_scenario(compiled.value(), "empty");
    require(!values.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    require(created.value().begin_entry(UINT32_C(0), UINT64_C(0)).has_value());
    auto frozen = created.value().freeze();
    require(frozen.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto layout = RecordLayout::plan(runtime.value(), frozen.value());
    require(layout.has_value());
    require(layout.value().regions().size() == 1U);
    require(layout.value().regions()[0].byte_length == UINT64_C(0));
    require(layout.value().regions()[0].data_offset == UINT64_C(224));
    require(layout.value().total_length() == UINT64_C(224));
    return EXIT_SUCCESS;
}

int test_task4_zero_length_pools_and_canonical_value_offsets() {
    auto compiled = CompiledSpec::compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"texts","cardinality":"many","type":{"kind":"str"}},{"id":"wide","cardinality":"many","type":{"kind":"wstr"}},{"id":"blobs","cardinality":"many","type":{"kind":"bytes"}}],"components":[]})");
    require(compiled.has_value());
    auto builder = PayloadBuilder::create(compiled.value());
    require(builder.has_value());
    require(builder.value().begin_entry(UINT32_C(0), UINT64_C(0)).has_value());
    require(builder.value().begin_entry(UINT32_C(1), UINT64_C(0)).has_value());
    require(builder.value().begin_entry(UINT32_C(2), UINT64_C(0)).has_value());
    auto values = builder.value().freeze();
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(values.has_value() && runtime.has_value());
    auto planned = RecordLayout::plan(runtime.value(), values.value());
    require(planned.has_value());
    require(planned.value().region_count() == UINT32_C(6));
    require(planned.value().total_length() == UINT64_C(584));
    const auto& regions = planned.value().regions();
    require(regions[0].kind ==
            fastdb::payload::layout::RegionKind::entry_values);
    require(regions[1].kind ==
            fastdb::payload::layout::RegionKind::entry_values);
    require(regions[2].kind ==
            fastdb::payload::layout::RegionKind::entry_values);
    require(regions[3].kind == fastdb::payload::layout::RegionKind::utf8_pool);
    require(regions[4].kind == fastdb::payload::layout::RegionKind::utf16_pool);
    require(regions[5].kind == fastdb::payload::layout::RegionKind::bytes_pool);
    for (const auto& region : regions) {
        require(region.data_offset == UINT64_C(584));
        require(region.byte_length == UINT64_C(0));
    }
    require(regions[3].element_count == UINT64_C(0));
    require(regions[4].element_count == UINT64_C(0));
    require(regions[5].element_count == UINT64_C(0));
    VectorSink sink(planned.value().total_length());
    require(fastdb::payload::build::encode_record(planned.value(),
                                                  values.value(), sink)
                .has_value());
    auto opened = fastdb::payload::view::open_record(
        compiled.value(), sink.bytes().data(), sink.bytes().size());
    require(opened.has_value());
    require(opened.value().variable_slots().empty());
    require(opened.value()
                .pool_metadata(fastdb::payload::layout::RegionKind::utf8_pool)
                .value()
                .byte_length == UINT64_C(0));
    require(opened.value()
                .pool_metadata(fastdb::payload::layout::RegionKind::utf16_pool)
                .value()
                .element_count == UINT64_C(0));
    require(opened.value()
                .pool_metadata(fastdb::payload::layout::RegionKind::bytes_pool)
                .value()
                .byte_length == UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_task4_pool_metadata_eager_lazy_and_traversal_order() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* text = find_case(corpus, "text-bytes");
    require(text != nullptr);
    auto encoded = encode_case(*text);
    require(encoded.has_value());
    const auto& regions = encoded.value().layout.regions();
    require(encoded.value().layout.region_count() == UINT32_C(10));
    require(encoded.value().layout.total_length() == UINT64_C(1416));
    require(regions[7].kind == fastdb::payload::layout::RegionKind::utf8_pool);
    require(regions[7].data_offset == UINT64_C(1336));
    require(regions[7].byte_length == UINT64_C(33));
    require(regions[8].kind == fastdb::payload::layout::RegionKind::utf16_pool);
    require(regions[8].data_offset == UINT64_C(1370));
    require(regions[8].byte_length == UINT64_C(24));
    require(regions[8].element_count == UINT64_C(12));
    require(regions[9].kind == fastdb::payload::layout::RegionKind::bytes_pool);
    require(regions[9].data_offset == UINT64_C(1394));
    require(regions[9].byte_length == UINT64_C(18));
    require(encoded.value().layout.variable_values().size() == 29U);

    auto eager = fastdb::payload::view::open_record(
        encoded.value().spec, encoded.value().bytes.data(),
        encoded.value().bytes.size());
    require(eager.has_value());
    require(eager.value().text_validated_eagerly());
    require(eager.value().validation_work() ==
            encoded.value().layout.validation_work());
    require(eager.value().variable_slots().size() == 29U);
    const auto& slots = eager.value().variable_slots();
    require(!slots[0].present && slots[0].pool_relative_offset == UINT64_C(0) &&
            slots[0].byte_length == UINT64_C(0));
    require(slots[1].present && slots[1].pool_relative_offset == UINT64_C(0) &&
            slots[1].byte_length == UINT64_C(0));
    require(slots[2].pool_relative_offset == UINT64_C(0) &&
            slots[2].byte_length == UINT64_C(5));
    require(slots[4].pool_relative_offset == UINT64_C(9) &&
            slots[5].pool_relative_offset == UINT64_C(15));
    require(slots[7].kind == fastdb::payload::spec::TypeKind::wstr &&
            !slots[7].present);
    require(slots[9].pool_relative_offset == UINT64_C(0) &&
            slots[9].byte_length == UINT64_C(2));
    require(slots[19].kind == fastdb::payload::spec::TypeKind::str &&
            slots[19].pool_relative_offset == UINT64_C(25));
    require(slots[21].kind == fastdb::payload::spec::TypeKind::wstr &&
            slots[21].pool_relative_offset == UINT64_C(12));
    require(slots[22].kind == fastdb::payload::spec::TypeKind::bytes &&
            slots[22].pool_relative_offset == UINT64_C(8));
    require(slots[25].kind == fastdb::payload::spec::TypeKind::str &&
            slots[25].pool_relative_offset == UINT64_C(27) &&
            slots[25].byte_length == UINT64_C(6));
    require(slots[27].kind == fastdb::payload::spec::TypeKind::bytes &&
            slots[27].pool_relative_offset == UINT64_C(14));

    auto invalid_utf8 = encoded.value().bytes;
    invalid_utf8[static_cast<std::size_t>(regions[7].data_offset)] =
        UINT8_C(0xff);
    auto eager_invalid = fastdb::payload::view::open_record(
        encoded.value().spec, invalid_utf8.data(), invalid_utf8.size());
    require(!eager_invalid.has_value());
    require(eager_invalid.error().code() ==
            FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);
    require(eager_invalid.error().path() == "/entries/texts/2");
    require(eager_invalid.error().details_json() ==
            "{\"encoding\":\"utf-8\",\"reason\":\"invalid_sequence\"}");

    auto lazy_limits = fastdb::payload::view::default_open_options();
    lazy_limits.validate_text_eager = false;
    auto lazy = fastdb::payload::view::open_record(
        encoded.value().spec, invalid_utf8.data(), invalid_utf8.size(),
        lazy_limits);
    require(lazy.has_value());
    require(!lazy.value().text_validated_eagerly());
    require(lazy.value().variable_slots().size() == 29U);
    require(lazy.value().validation_work() + UINT64_C(45) ==
            eager.value().validation_work());
    require(lazy.value().retained_max_string_bytes() ==
            lazy_limits.max_string_bytes);
    require(lazy.value().retained_max_validation_work() ==
            lazy_limits.max_validation_work);

    auto string_limits = fastdb::payload::view::default_open_options();
    string_limits.max_string_bytes = UINT64_C(56);
    require(require_resource_limit(
                fastdb::payload::view::open_record(
                    encoded.value().spec, encoded.value().bytes.data(),
                    encoded.value().bytes.size(), string_limits),
                "/binary/regions/8/byte_length", UINT64_C(57), UINT64_C(56),
                "string_bytes") == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

std::uint64_t region_field(std::uint32_t region_index,
                           std::uint64_t field_offset) {
    return fastdb::payload::layout::header_size +
           static_cast<std::uint64_t>(region_index) *
               fastdb::payload::layout::region_descriptor_size +
           field_offset;
}

int test_task4_malformed_pools_and_descriptors_have_exact_errors() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* text = find_case(corpus, "text-bytes");
    require(text != nullptr);
    auto encoded = encode_case(*text);
    require(encoded.has_value());
    const auto& layout = encoded.value().layout;
    const auto& regions = layout.regions();
    const auto open = [&](const std::vector<std::uint8_t>& bytes) {
        return fastdb::payload::view::open_record(encoded.value().spec,
                                                  bytes.data(), bytes.size());
    };

    auto wrong_kind = encoded.value().bytes;
    require(fastdb::payload::layout::store_u32_le(
                wrong_kind.data(), wrong_kind.size(),
                region_field(UINT32_C(7),
                             fastdb::payload::layout::region_kind_offset),
                FDB_PAYLOAD_REGION_BYTES_POOL, JsonPointer{})
                .has_value());
    require(require_exact_error(
                open(wrong_kind), FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/7/kind",
                static_cast<std::uint32_t>(
                    fastdb::payload::layout::RegionKind::bytes_pool),
                static_cast<std::uint32_t>(regions[7].kind), "region_kind") ==
            EXIT_SUCCESS);

    auto wrong_owner = encoded.value().bytes;
    require(
        fastdb::payload::layout::store_u32_le(
            wrong_owner.data(), wrong_owner.size(),
            region_field(UINT32_C(7),
                         fastdb::payload::layout::region_owner_index_offset),
            UINT32_C(0), JsonPointer{})
            .has_value());
    require(require_exact_error(
                open(wrong_owner), FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/7/owner_index", UINT64_C(0), UINT32_MAX,
                "region_owner_index") == EXIT_SUCCESS);

    auto wrong_alignment = encoded.value().bytes;
    require(fastdb::payload::layout::store_u32_le(
                wrong_alignment.data(), wrong_alignment.size(),
                region_field(UINT32_C(7),
                             fastdb::payload::layout::region_alignment_offset),
                UINT32_C(2), JsonPointer{})
                .has_value());
    require(require_exact_error(
                open(wrong_alignment), FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/7/alignment", UINT64_C(2),
                regions[7].alignment, "region_alignment") == EXIT_SUCCESS);

    auto overlap = encoded.value().bytes;
    require(
        fastdb::payload::layout::store_u64_le(
            overlap.data(), overlap.size(),
            region_field(UINT32_C(8),
                         fastdb::payload::layout::region_data_offset_offset),
            regions[7].data_offset, JsonPointer{})
            .has_value());
    require(require_exact_error(
                open(overlap), FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/8/data_offset", regions[7].data_offset,
                regions[8].data_offset, "region_data_offset") ==
            EXIT_SUCCESS);

    auto out_of_bounds = encoded.value().bytes;
    require(
        fastdb::payload::layout::store_u64_le(
            out_of_bounds.data(), out_of_bounds.size(),
            region_field(UINT32_C(7),
                         fastdb::payload::layout::region_byte_length_offset),
            encoded.value().bytes.size(), JsonPointer{})
            .has_value());
    require(fastdb::payload::layout::store_u64_le(
                out_of_bounds.data(), out_of_bounds.size(),
                region_field(
                    UINT32_C(7),
                    fastdb::payload::layout::region_element_count_offset),
                encoded.value().bytes.size(), JsonPointer{})
                .has_value());
    require(open(out_of_bounds).error().code() == FDB_PAYLOAD_E_OUT_OF_BOUNDS);
    require(open(out_of_bounds).error().path() ==
            "/binary/regions/7/byte_length");

    auto overflow = encoded.value().bytes;
    require(
        fastdb::payload::layout::store_u64_le(
            overflow.data(), overflow.size(),
            region_field(UINT32_C(7),
                         fastdb::payload::layout::region_byte_length_offset),
            UINT64_MAX, JsonPointer{})
            .has_value());
    require(fastdb::payload::layout::store_u64_le(
                overflow.data(), overflow.size(),
                region_field(
                    UINT32_C(7),
                    fastdb::payload::layout::region_element_count_offset),
                UINT64_MAX, JsonPointer{})
                .has_value());
    require(open(overflow).error().code() == FDB_PAYLOAD_E_LENGTH_OVERFLOW);
    require(open(overflow).error().path() ==
            "/binary/regions/7/byte_length");

    const std::uint64_t text_values = regions[1].data_offset;
    auto nonzero_null = encoded.value().bytes;
    require(fastdb::payload::layout::store_u64_le(
                nonzero_null.data(), nonzero_null.size(), text_values + 8U,
                UINT64_C(1), JsonPointer{})
                .has_value());
    require(require_reason_error(
                open(nonzero_null), FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/entries/texts/0", "nonzero_null_storage") == EXIT_SUCCESS);

    auto gap = encoded.value().bytes;
    require(fastdb::payload::layout::store_u64_le(
                gap.data(), gap.size(), text_values + UINT64_C(3 * 16),
                UINT64_C(6), JsonPointer{})
                .has_value());
    require(open(gap).error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);
    require(open(gap).error().path() == "/entries/texts/3");
    require(
        open(gap).error().details_json() ==
        "{\"actual\":\"6\",\"expected\":\"5\",\"reason\":\"partition_cursor_"
        "mismatch\"}");

    auto alias = encoded.value().bytes;
    require(fastdb::payload::layout::store_u64_le(
                alias.data(), alias.size(), text_values + UINT64_C(5 * 16),
                UINT64_C(9), JsonPointer{})
                .has_value());
    require(open(alias).error().path() == "/entries/texts/5");
    require(
        open(alias).error().details_json() ==
        "{\"actual\":\"9\",\"expected\":\"15\",\"reason\":\"partition_cursor_"
        "mismatch\"}");

    auto out_of_order = encoded.value().bytes;
    require(fastdb::payload::layout::store_u64_le(
                out_of_order.data(), out_of_order.size(),
                text_values + UINT64_C(4 * 16), UINT64_C(15), JsonPointer{})
                .has_value());
    require(open(out_of_order).error().path() == "/entries/texts/4");

    const std::uint64_t wide_values = regions[3].data_offset;
    auto odd_offset = encoded.value().bytes;
    require(fastdb::payload::layout::store_u64_le(
                odd_offset.data(), odd_offset.size(),
                wide_values + UINT64_C(2 * 16), UINT64_C(1), JsonPointer{})
                .has_value());
    require(require_reason_error(
                open(odd_offset), FDB_PAYLOAD_E_MISALIGNED, "/entries/wide/2",
                "utf16_descriptor_misaligned") == EXIT_SUCCESS);

    auto odd_length = encoded.value().bytes;
    require(fastdb::payload::layout::store_u64_le(
                odd_length.data(), odd_length.size(),
                wide_values + UINT64_C(2 * 16 + 8), UINT64_C(3), JsonPointer{})
                .has_value());
    require(require_reason_error(
                open(odd_length), FDB_PAYLOAD_E_MISALIGNED, "/entries/wide/2",
                "utf16_descriptor_misaligned") == EXIT_SUCCESS);

    auto invalid_utf16 = encoded.value().bytes;
    invalid_utf16[static_cast<std::size_t>(regions[8].data_offset)] =
        UINT8_C(0x00);
    invalid_utf16[static_cast<std::size_t>(regions[8].data_offset + 1U)] =
        UINT8_C(0xd8);
    require(open(invalid_utf16).error().code() ==
            FDB_PAYLOAD_E_INVALID_TEXT_ENCODING);
    require(open(invalid_utf16).error().path() == "/entries/wide/2");
    require(open(invalid_utf16).error().details_json() ==
            "{\"encoding\":\"utf-16le\",\"reason\":\"unpaired_surrogate\"}");

    auto tail = encoded.value().bytes;
    require(
        fastdb::payload::layout::store_u64_le(
            tail.data(), tail.size(),
            region_field(UINT32_C(9),
                         fastdb::payload::layout::region_byte_length_offset),
            UINT64_C(19), JsonPointer{})
            .has_value());
    require(
        fastdb::payload::layout::store_u64_le(
            tail.data(), tail.size(),
            region_field(UINT32_C(9),
                         fastdb::payload::layout::region_element_count_offset),
            UINT64_C(19), JsonPointer{})
            .has_value());
    const auto tail_result = open(tail);
    require(!tail_result.has_value());
    require(tail_result.error().path() ==
                "/binary/regions/9/byte_length",
            std::string(tail_result.error().path()) + ":" +
                std::string(tail_result.error().details_json()));
    require(tail_result.error().details_json() ==
            "{\"actual\":\"18\",\"expected\":\"19\",\"reason\":\"partition_not_"
            "consumed\"}");
    return EXIT_SUCCESS;
}

int test_task3_metadata_observation_and_nan_canonicalization() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* numeric = find_case(corpus, "numeric-edges");
    const BinaryGoldenCase* nested = find_case(corpus, "nested-components");
    require(numeric != nullptr && nested != nullptr);

    auto numeric_encoded = encode_case(*numeric);
    require(numeric_encoded.has_value());
    BinaryGoldenCase alternate = *numeric;
    alternate.success.scenario = "numeric_edges_alt_nan";
    auto alternate_encoded = encode_case(alternate);
    require(alternate_encoded.has_value());
    require(alternate_encoded.value().bytes == numeric_encoded.value().bytes);

    auto numeric_opened = fastdb::payload::view::open_record(
        numeric_encoded.value().spec, numeric_encoded.value().bytes.data(),
        numeric_encoded.value().bytes.size());
    require(numeric_opened.has_value());
    const auto normalized_entry =
        numeric_opened.value().entry_slot(UINT32_C(7));
    require(normalized_entry.has_value());
    require(normalized_entry.value().runtime_type_id == UINT32_C(7));
    require(normalized_entry.value().value_count == UINT64_C(6));
    require(normalized_entry.value().stride == UINT32_C(1));
    require(!normalized_entry.value().has_validity);
    require(numeric_opened.value()
                .scalar(numeric_encoded.value().bytes.data(),
                        numeric_encoded.value().bytes.size(), UINT32_C(7),
                        UINT64_C(3))
                .value()
                .bits == UINT64_C(0x0000000000000000));
    require(numeric_opened.value()
                .scalar(numeric_encoded.value().bytes.data(),
                        numeric_encoded.value().bytes.size(), UINT32_C(7),
                        UINT64_C(4))
                .value()
                .bits == UINT64_C(0x4000000000000000));
    require(numeric_opened.value()
                .scalar(numeric_encoded.value().bytes.data(),
                        numeric_encoded.value().bytes.size(), UINT32_C(8),
                        UINT64_C(2))
                .value()
                .bits == UINT64_C(0xc0e0000000000000));
    require(numeric_opened.value()
                .scalar(numeric_encoded.value().bytes.data(),
                        numeric_encoded.value().bytes.size(), UINT32_C(8),
                        UINT64_C(3))
                .value()
                .bits == UINT64_C(0xc0dfff8000000000));

    auto nested_encoded = encode_case(*nested);
    require(nested_encoded.has_value());
    auto nested_opened = fastdb::payload::view::open_record(
        nested_encoded.value().spec, nested_encoded.value().bytes.data(),
        nested_encoded.value().bytes.size());
    require(nested_opened.has_value());
    const auto direct_entry = nested_opened.value().entry_slot(UINT32_C(0));
    require(direct_entry.has_value());
    require(direct_entry.value().data_offset == UINT64_C(472));
    require(direct_entry.value().value_count == UINT64_C(1));
    require(direct_entry.value().stride == UINT32_C(72));
    require(!direct_entry.value().has_validity);
    const std::array<std::uint32_t, 3> code_path{{UINT32_C(1), UINT32_C(1),
                                                  UINT32_C(1)}};
    const auto code_slot = nested_opened.value().field_slot(
        UINT32_C(0), code_path.data(), code_path.size());
    require(code_slot.has_value());
    require(code_slot.value().relative_offset == UINT32_C(34));
    require(code_slot.value().stride == UINT32_C(2));
    require(code_slot.value().kind ==
            fastdb::payload::spec::TypeKind::u16n);
    require(nested_opened.value()
                .field_scalar(nested_encoded.value().bytes.data(),
                              nested_encoded.value().bytes.size(),
                              UINT32_C(0), UINT64_C(0), code_path.data(),
                              code_path.size())
                .value()
                .bits == UINT64_C(0x40efffe000000000));
    const std::array<std::uint32_t, 2> signed_zero_path{{UINT32_C(2),
                                                        UINT32_C(2)}};
    require(nested_opened.value()
                .field_scalar(nested_encoded.value().bytes.data(),
                              nested_encoded.value().bytes.size(),
                              UINT32_C(0), UINT64_C(0),
                              signed_zero_path.data(),
                              signed_zero_path.size())
                .value()
                .bits == UINT64_C(0x8000000000000000));
    const auto null_descendant = nested_opened.value().field_scalar(
        nested_encoded.value().bytes.data(), nested_encoded.value().bytes.size(),
        UINT32_C(1), UINT64_C(0), code_path.data(), code_path.size());
    require(null_descendant.has_value());
    require(!null_descendant.value().present);
    return EXIT_SUCCESS;
}

int test_open_component_nesting_depth_limits() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* numeric = find_case(corpus, "numeric-edges");
    const BinaryGoldenCase* nested = find_case(corpus, "nested-components");
    require(numeric != nullptr && nested != nullptr);

    auto numeric_encoded = encode_case(*numeric);
    require(numeric_encoded.has_value());
    auto scalar_limits = fastdb::payload::view::default_open_options();
    scalar_limits.max_nesting_depth = UINT64_C(0);
    require(fastdb::payload::view::open_record(
                numeric_encoded.value().spec,
                numeric_encoded.value().bytes.data(),
                numeric_encoded.value().bytes.size(), scalar_limits)
                .has_value());

    auto nested_encoded = encode_case(*nested);
    require(nested_encoded.has_value());
    auto unrestricted = fastdb::payload::view::open_record(
        nested_encoded.value().spec, nested_encoded.value().bytes.data(),
        nested_encoded.value().bytes.size());
    require(unrestricted.has_value());

    auto exact_limits = fastdb::payload::view::default_open_options();
    exact_limits.max_nesting_depth = UINT64_C(3);
    auto exact_first = fastdb::payload::view::open_record(
        nested_encoded.value().spec, nested_encoded.value().bytes.data(),
        nested_encoded.value().bytes.size(), exact_limits);
    auto exact_second = fastdb::payload::view::open_record(
        nested_encoded.value().spec, nested_encoded.value().bytes.data(),
        nested_encoded.value().bytes.size(), exact_limits);
    require(exact_first.has_value() && exact_second.has_value());
    require(exact_first.value().validation_work() ==
            unrestricted.value().validation_work());
    require(exact_second.value().validation_work() ==
            unrestricted.value().validation_work());
    require(exact_first.value().validation_work() ==
            nested_encoded.value().layout.validation_work());

    auto short_limits = exact_limits;
    short_limits.max_nesting_depth = UINT64_C(2);
    short_limits.max_validation_work = unrestricted.value().validation_work();
    const auto short_first = fastdb::payload::view::open_record(
        nested_encoded.value().spec, nested_encoded.value().bytes.data(),
        nested_encoded.value().bytes.size(), short_limits);
    const auto short_second = fastdb::payload::view::open_record(
        nested_encoded.value().spec, nested_encoded.value().bytes.data(),
        nested_encoded.value().bytes.size(), short_limits);
    require(require_resource_limit(short_first,
                                   "/entries/a_direct/b_middle/b_leaf_reuse",
                                   UINT64_C(3), UINT64_C(2),
                                   "nesting_depth") == EXIT_SUCCESS);
    require(require_resource_limit(short_second,
                                   "/entries/a_direct/b_middle/b_leaf_reuse",
                                   UINT64_C(3), UINT64_C(2),
                                   "nesting_depth") == EXIT_SUCCESS);

    auto work_short_limits = exact_limits;
    work_short_limits.max_validation_work =
        unrestricted.value().validation_work() - UINT64_C(1);
    const auto work_short = fastdb::payload::view::open_record(
        nested_encoded.value().spec, nested_encoded.value().bytes.data(),
        nested_encoded.value().bytes.size(), work_short_limits);
    require(require_resource_limit(
                work_short, "/entries/c_empty_many/1",
                unrestricted.value().validation_work(),
                unrestricted.value().validation_work() - UINT64_C(1),
                "validation_work") == EXIT_SUCCESS);

    auto nullable_spec = CompiledSpec::compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"root","cardinality":"one","type":{"kind":"component","id":"Outer"}}],"components":[{"id":"Leaf","kind":"record","fields":[{"id":"value","type":{"kind":"u8"}}]},{"id":"Outer","kind":"record","fields":[{"id":"absent","type":{"kind":"component","id":"Leaf","nullable":true}},{"id":"scalar","type":{"kind":"u8"}}]}]})");
    require(nullable_spec.has_value());
    auto nullable_builder = PayloadBuilder::create(nullable_spec.value());
    require(nullable_builder.has_value());
    require(nullable_builder.value()
                .begin_entry(UINT32_C(0), UINT64_C(1))
                .has_value());
    require(nullable_builder.value().begin_component().has_value());
    require(nullable_builder.value().push_null().has_value());
    require(nullable_builder.value().push_u8(UINT8_C(7)).has_value());
    auto nullable_values = nullable_builder.value().freeze();
    auto nullable_runtime = RuntimeSchema::compile(nullable_spec.value());
    require(nullable_values.has_value() && nullable_runtime.has_value());
    auto nullable_layout =
        RecordLayout::plan(nullable_runtime.value(), nullable_values.value());
    require(nullable_layout.has_value());
    VectorSink nullable_sink(nullable_layout.value().total_length());
    require(fastdb::payload::build::encode_record(
                nullable_layout.value(), nullable_values.value(), nullable_sink)
                .has_value());

    auto nullable_limits = fastdb::payload::view::default_open_options();
    nullable_limits.max_nesting_depth = UINT64_C(1);
    const auto nullable_opened = fastdb::payload::view::open_record(
        nullable_spec.value(), nullable_sink.bytes().data(),
        nullable_sink.bytes().size(), nullable_limits);
    require(nullable_opened.has_value());
    require(nullable_opened.value().validation_work() ==
            nullable_layout.value().validation_work());

    nullable_limits.max_nesting_depth = UINT64_C(0);
    require(require_resource_limit(
                fastdb::payload::view::open_record(
                    nullable_spec.value(), nullable_sink.bytes().data(),
                    nullable_sink.bytes().size(), nullable_limits),
                "/entries/root", UINT64_C(1), UINT64_C(0),
                "nesting_depth") == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int test_large_many_component_stride_and_count_overflow() {
    auto compiled = CompiledSpec::compile(
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"rows","cardinality":"many","type":{"kind":"component","id":"Wide"}}],"components":[{"id":"Wide","kind":"record","fields":[{"id":"a","type":{"kind":"f64"}},{"id":"b","type":{"kind":"u32"}}]}]})");
    require(compiled.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    constexpr std::uint64_t row_count = UINT64_C(4096);
    require(created.value().begin_entry(UINT32_C(0), row_count).has_value());
    for (std::uint64_t index = UINT64_C(0); index < row_count; ++index) {
        require(created.value().begin_component().has_value());
        require(created.value()
                    .push_f64_bits(UINT64_C(0x3ff0000000000000) + index)
                    .has_value());
        require(created.value().push_u32(static_cast<std::uint32_t>(index))
                    .has_value());
    }
    auto values = created.value().freeze();
    require(values.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto planned = RecordLayout::plan(runtime.value(), values.value());
    require(planned.has_value());
    require(planned.value().regions()[0].stride == UINT32_C(16));
    require(planned.value().regions()[0].byte_length == UINT64_C(65536));
    VectorSink sink(planned.value().total_length());
    require(fastdb::payload::build::encode_record(
                planned.value(), values.value(), sink)
                .has_value());
    require(fastdb::payload::view::open_record(
                compiled.value(), sink.bytes().data(), sink.bytes().size())
                .has_value());

    auto overflow = sink.bytes();
    require(fastdb::payload::layout::store_u64_le(
                overflow.data(), overflow.size(),
                fastdb::payload::layout::header_root_value_count_offset,
                UINT64_MAX, JsonPointer{})
                .has_value());
    const std::uint64_t entry_base =
        fastdb::payload::layout::header_size +
        fastdb::payload::layout::region_descriptor_size;
    require(fastdb::payload::layout::store_u64_le(
                overflow.data(), overflow.size(),
                entry_base + fastdb::payload::layout::entry_value_count_offset,
                UINT64_MAX, JsonPointer{})
                .has_value());
    const auto rejected = fastdb::payload::view::open_record(
        compiled.value(), overflow.data(), overflow.size());
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_LENGTH_OVERFLOW);
    require(rejected.error().path() == "/binary/entries/0/value_count",
            std::string(rejected.error().path()) + ":" +
                std::string(rejected.error().details_json()));
    require(rejected.error().details_json() ==
            "{\"reason\":\"wire_multiply_overflow\"}");
    return EXIT_SUCCESS;
}

int require_reason_error(
    const Result<fastdb::payload::view::PayloadIndex>& result,
    std::uint32_t code,
    std::string_view path,
    std::string_view reason) {
    require(!result.has_value());
    require(result.error().code() == code);
    require(result.error().path() == path);
    require(result.error().details_json() ==
            "{\"reason\":\"" + std::string(reason) + "\"}");
    return EXIT_SUCCESS;
}

int test_task11_header_directory_and_descriptor_failures() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* empty = find_case(corpus, "empty");
    require(empty != nullptr);
    auto encoded = encode_case(*empty);
    require(encoded.has_value());
    const auto& spec = encoded.value().spec;
    const auto& canonical = encoded.value().bytes;
    const auto open = [&](const std::vector<std::uint8_t>& bytes) {
        return fastdb::payload::view::open_record(spec, bytes.data(),
                                                  bytes.size());
    };
    const auto mutate_u16 = [&](std::uint64_t offset, std::uint16_t value) {
        auto bytes = canonical;
        if (!fastdb::payload::layout::store_u16_le(
                 bytes.data(), bytes.size(), offset, value, JsonPointer{})
                 .has_value()) {
            std::abort();
        }
        return bytes;
    };
    const auto mutate_u32 = [&](std::uint64_t offset, std::uint32_t value) {
        auto bytes = canonical;
        if (!fastdb::payload::layout::store_u32_le(
                 bytes.data(), bytes.size(), offset, value, JsonPointer{})
                 .has_value()) {
            std::abort();
        }
        return bytes;
    };
    const auto mutate_u64 = [&](std::uint64_t offset, std::uint64_t value) {
        auto bytes = canonical;
        if (!fastdb::payload::layout::store_u64_le(
                 bytes.data(), bytes.size(), offset, value, JsonPointer{})
                 .has_value()) {
            std::abort();
        }
        return bytes;
    };

    auto bad_magic = canonical;
    bad_magic[0] ^= UINT8_C(1);
    const auto magic_result = open(bad_magic);
    require(!magic_result.has_value());
    require(magic_result.error().code() == FDB_PAYLOAD_E_INVALID_MAGIC);
    require(magic_result.error().path() == "/binary/header/magic");

    require(require_exact_error(
                open(mutate_u16(
                    fastdb::payload::layout::header_major_offset,
                    UINT16_C(2))),
                FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION,
                "/binary/header/major", UINT64_C(2), UINT64_C(1),
                "unsupported_binary_major") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u16(
                    fastdb::payload::layout::header_minor_offset,
                    UINT16_C(1))),
                FDB_PAYLOAD_E_UNSUPPORTED_BINARY_VERSION,
                "/binary/header/minor", UINT64_C(1), UINT64_C(0),
                "unsupported_binary_minor") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    fastdb::payload::layout::header_size_offset,
                    UINT32_C(127))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/binary/header/size",
                UINT64_C(127), UINT64_C(128), "header_size") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    fastdb::payload::layout::header_profile_offset,
                    FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1)),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/profile",
                FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1,
                FDB_PAYLOAD_PROFILE_RECORD_V1, "profile") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    fastdb::payload::layout::header_flags_offset,
                    UINT32_C(1))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/binary/header/flags",
                UINT64_C(1), UINT64_C(0), "header_flags") == EXIT_SUCCESS);
    require(require_reason_error(
                open(mutate_u64(
                    fastdb::payload::layout::header_total_length_offset,
                    UINT64_C(129))),
                FDB_PAYLOAD_E_OUT_OF_BOUNDS,
                "/binary/header/total_length",
                "declared_total_out_of_bounds") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u64(
                    fastdb::payload::layout::header_total_length_offset,
                    UINT64_C(127))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/total_length", UINT64_C(127),
                UINT64_C(128), "trailing_binary_bytes") == EXIT_SUCCESS);

    auto wrong_digest = canonical;
    wrong_digest[static_cast<std::size_t>(
        fastdb::payload::layout::header_spec_digest_offset)] ^= UINT8_C(1);
    const auto digest_result = open(wrong_digest);
    require(!digest_result.has_value());
    require(digest_result.error().code() == FDB_PAYLOAD_E_DIGEST_MISMATCH);
    require(digest_result.error().path() == "/binary/header/spec_sha256");

    auto reserved = canonical;
    reserved[static_cast<std::size_t>(
        fastdb::payload::layout::header_reserved_offset)] = UINT8_C(1);
    require(require_exact_error(open(reserved),
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "/binary/header/reserved", UINT64_C(1),
                                UINT64_C(0), "nonzero_header_reserved") ==
            EXIT_SUCCESS);

    require(require_exact_error(
                open(mutate_u64(
                    fastdb::payload::layout::header_region_directory_offset,
                    UINT64_C(129))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/region_directory_offset", UINT64_C(129),
                UINT64_C(128), "region_directory_offset") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    fastdb::payload::layout::header_region_count_offset,
                    UINT32_C(1))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/region_count", UINT64_C(1), UINT64_C(0),
                "region_count") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    fastdb::payload::layout::header_region_descriptor_size_offset,
                    UINT32_C(55))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/region_descriptor_size", UINT64_C(55),
                UINT64_C(56), "region_descriptor_size") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u64(
                    fastdb::payload::layout::header_entry_directory_offset,
                    UINT64_C(129))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/entry_directory_offset", UINT64_C(129),
                UINT64_C(128), "entry_directory_not_contiguous") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    fastdb::payload::layout::header_entry_count_offset,
                    UINT32_C(1))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/entry_count", UINT64_C(1), UINT64_C(0),
                "entry_count") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    fastdb::payload::layout::header_entry_descriptor_size_offset,
                    UINT32_C(39))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/entry_descriptor_size", UINT64_C(39),
                UINT64_C(40), "entry_descriptor_size") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u64(
                    fastdb::payload::layout::header_root_value_count_offset,
                    UINT64_C(1))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/header/root_value_count", UINT64_C(1),
                UINT64_C(0), "root_value_count_mismatch") == EXIT_SUCCESS);

    auto truncated = canonical;
    truncated.resize(static_cast<std::size_t>(
        fastdb::payload::layout::header_size - UINT32_C(1)));
    require(require_reason_error(open(truncated), FDB_PAYLOAD_E_OUT_OF_BOUNDS,
                                "/binary/header", "header_out_of_bounds") ==
            EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int test_component_malformed_bytes_have_exact_diagnostics() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* nested = find_case(corpus, "nested-components");
    require(nested != nullptr);
    auto encoded = encode_case(*nested);
    require(encoded.has_value());
    const auto& layout = encoded.value().layout;
    const auto& entry0 = layout.entries()[0];
    const auto& entry1 = layout.entries()[1];
    const auto& value0 = layout.regions()[entry0.values_region_index];
    const auto& validity1 = layout.regions()[entry1.validity_region_index];

    auto bad_tail_bit = encoded.value().bytes;
    bad_tail_bit[static_cast<std::size_t>(validity1.data_offset)] |=
        UINT8_C(0x80);
    require(require_exact_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, bad_tail_bit.data(),
                    bad_tail_bit.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/1/data", UINT64_C(128), UINT64_C(0),
                "nonzero_validity_tail") == EXIT_SUCCESS);

    auto bad_field_padding = encoded.value().bytes;
    bad_field_padding[static_cast<std::size_t>(value0.data_offset +
                                                UINT64_C(2))] = UINT8_C(1);
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, bad_field_padding.data(),
                    bad_field_padding.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/entries/a_direct",
                "nonzero_component_padding") == EXIT_SUCCESS);

    auto bad_tail_padding = encoded.value().bytes;
    bad_tail_padding[static_cast<std::size_t>(value0.data_offset +
                                               UINT64_C(68))] = UINT8_C(1);
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, bad_tail_padding.data(),
                    bad_tail_padding.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY, "/entries/a_direct",
                "nonzero_component_padding") == EXIT_SUCCESS);

    auto bad_null = encoded.value().bytes;
    const auto& value1 = layout.regions()[entry1.values_region_index];
    bad_null[static_cast<std::size_t>(value1.data_offset)] = UINT8_C(1);
    require(require_reason_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, bad_null.data(), bad_null.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/entries/b_nullable_many/0", "nonzero_null_storage") ==
            EXIT_SUCCESS);

    const std::uint64_t region0 = fastdb::payload::layout::header_size;
    auto wrong_stride = encoded.value().bytes;
    require(fastdb::payload::layout::store_u32_le(
                wrong_stride.data(), wrong_stride.size(),
                region0 + fastdb::payload::layout::region_stride_offset,
                value0.stride + UINT32_C(1), JsonPointer{})
                .has_value());
    require(require_exact_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, wrong_stride.data(),
                    wrong_stride.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/0/stride", value0.stride + UINT32_C(1),
                value0.stride, "region_stride") == EXIT_SUCCESS);

    auto wrong_alignment = encoded.value().bytes;
    require(fastdb::payload::layout::store_u32_le(
                wrong_alignment.data(), wrong_alignment.size(),
                region0 + fastdb::payload::layout::region_alignment_offset,
                UINT32_C(4), JsonPointer{})
                .has_value());
    require(require_exact_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, wrong_alignment.data(),
                    wrong_alignment.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/0/alignment", UINT64_C(4),
                value0.alignment, "region_alignment") == EXIT_SUCCESS);

    auto wrong_type = encoded.value().bytes;
    require(fastdb::payload::layout::store_u32_le(
                wrong_type.data(), wrong_type.size(),
                region0 +
                    fastdb::payload::layout::region_runtime_type_id_offset,
                UINT32_C(999), JsonPointer{})
                .has_value());
    require(require_exact_error(
                fastdb::payload::view::open_record(
                    encoded.value().spec, wrong_type.data(), wrong_type.size()),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/0/runtime_type_id", UINT64_C(999),
                entry0.runtime_type_id, "region_runtime_type_id") ==
            EXIT_SUCCESS);

    auto truncated = encoded.value().bytes;
    truncated.resize(static_cast<std::size_t>(value0.data_offset +
                                               value0.byte_length -
                                               UINT64_C(1)));
    require(fastdb::payload::layout::store_u64_le(
                truncated.data(), truncated.size(),
                fastdb::payload::layout::header_total_length_offset,
                truncated.size(), JsonPointer{})
                .has_value());
    const auto truncated_result = fastdb::payload::view::open_record(
        encoded.value().spec, truncated.data(), truncated.size());
    require(!truncated_result.has_value());
    require(truncated_result.error().code() == FDB_PAYLOAD_E_OUT_OF_BOUNDS);
    require(truncated_result.error().path() ==
            "/binary/regions/0/byte_length");
    require(truncated_result.error().details_json() ==
            "{\"available\":\"543\",\"end\":\"544\",\"reason\":"
            "\"wire_range_out_of_bounds\"}");
    return EXIT_SUCCESS;
}

int test_task4_layout_encode_open_allocation_sweeps() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* text = find_case(corpus, "text-bytes");
    require(text != nullptr);
    auto compiled = CompiledSpec::compile(text->success.source);
    auto values = build_scenario(compiled.value(), text->success.scenario);
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(compiled.has_value() && values.has_value() && runtime.has_value());

    std::uint64_t layout_failures = UINT64_C(0);
    Result<RecordLayout> planned = Result<RecordLayout>::failure(
        fastdb::payload::error::Error::from_details(
            FDB_PAYLOAD_E_INTERNAL, JsonPointer{}, "uninitialized",
            fastdb::payload::json::JsonValue::object({})));
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(4096);
         ++allocation) {
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        try {
            planned = RecordLayout::plan(runtime.value(), values.value());
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "layout escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (planned.has_value()) {
            break;
        }
        require(planned.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++layout_failures;
    }
    require(planned.has_value());
    require(layout_failures > UINT64_C(0));

    std::uint64_t encode_failures = UINT64_C(0);
    std::vector<std::uint8_t> encoded;
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(4096);
         ++allocation) {
        VectorSink sink(planned.value().total_length());
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        Result<void> result = Result<void>::success();
        try {
            result = fastdb::payload::build::encode_record(
                planned.value(), values.value(), sink);
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "encode escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (result.has_value()) {
            encoded = sink.bytes();
            break;
        }
        require(result.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++encode_failures;
    }
    require(!encoded.empty());
    require(encode_failures > UINT64_C(0));

    std::uint64_t open_failures = UINT64_C(0);
    Result<fastdb::payload::view::PayloadIndex> opened =
        Result<fastdb::payload::view::PayloadIndex>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}, "uninitialized",
                fastdb::payload::json::JsonValue::object({})));
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(4096);
         ++allocation) {
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        try {
            opened = fastdb::payload::view::open_record(
                compiled.value(), encoded.data(), encoded.size());
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "open escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (opened.has_value()) {
            break;
        }
        require(opened.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++open_failures;
    }
    require(opened.has_value());
    require(opened.value().variable_slots().size() == 29U);
    require(opened.value()
                .pool_metadata(fastdb::payload::layout::RegionKind::utf16_pool)
                .value()
                .element_count == UINT64_C(12));
    require(open_failures > UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_open_observation_and_malformed_canonical_values() {
    const auto corpus = fastdb::test::payload::load_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* fixed = find_case(corpus, "fixed-scalars");
    require(fixed != nullptr);
    auto encoded = encode_case(*fixed);
    require(encoded.has_value());

    std::vector<std::uint8_t> unaligned(encoded.value().bytes.size() + 1U,
                                        UINT8_C(0));
    std::copy(encoded.value().bytes.begin(), encoded.value().bytes.end(),
              unaligned.begin() + 1);
    auto opened = fastdb::payload::view::open_record(
        encoded.value().spec, unaligned.data() + 1,
        encoded.value().bytes.size());
    require(opened.has_value());
    require(opened.value().entry_count() == UINT32_C(7));
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(0), UINT64_C(0))
                .value()
                .bits ==
            UINT64_C(1));
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(2), UINT64_C(1))
                .value()
                .present ==
            false);
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(5), UINT64_C(0))
                .value()
                .bits ==
            UINT64_C(0x80000000));
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(5), UINT64_C(3))
                .value()
                .bits ==
            UINT64_C(0x7fc00000));
    require(opened.value()
                .scalar(unaligned.data() + 1, encoded.value().bytes.size(),
                        UINT32_C(6), UINT64_C(3))
                .value()
                .bits ==
            UINT64_C(0x7ff8000000000000));

    auto bad_bool = encoded.value().bytes;
    bad_bool[856] = UINT8_C(2);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_bool.data(), bad_bool.size())
                .error()
                .code() == FDB_PAYLOAD_E_INVALID_BINARY_VALUE);

    auto bad_null = encoded.value().bytes;
    bad_null[862] = UINT8_C(1);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_null.data(), bad_null.size())
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto bad_padding = encoded.value().bytes;
    bad_padding[859] = UINT8_C(1);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_padding.data(), bad_padding.size())
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto bad_nan = encoded.value().bytes;
    bad_nan[888] = UINT8_C(1);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_nan.data(), bad_nan.size())
                .error()
                .code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto bad_digest = encoded.value().bytes;
    bad_digest[32] ^= UINT8_C(1);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, bad_digest.data(), bad_digest.size())
                .error()
                .code() == FDB_PAYLOAD_E_DIGEST_MISMATCH);

    auto limits = fastdb::payload::view::default_open_options();
    limits.max_total_bytes = UINT64_C(128);
    require(fastdb::payload::view::open_record(
                encoded.value().spec, encoded.value().bytes.data(),
                encoded.value().bytes.size(), limits)
                .error()
                .code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    require(fastdb::payload::error::symbol_for_code(
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY) ==
            "NON_CANONICAL_BINARY");
    require(fastdb::payload::error::symbol_for_code(
                FDB_PAYLOAD_E_INVALID_BINARY_VALUE) ==
            "INVALID_BINARY_VALUE");
    return EXIT_SUCCESS;
}

struct AlgebraCoverageRow final {
    std::string_view kind;
    std::string_view root_case;
    std::string_view component_case;
    std::string_view list_case;
};

constexpr std::array<AlgebraCoverageRow, 14> task5_algebra_coverage{{
    {"bool", "fixed-scalars", "component-list-composition", "component-list-composition"},
    {"u8", "fixed-scalars", "component-list-composition", "component-list-composition"},
    {"u16", "fixed-scalars", "component-list-composition", "component-list-composition"},
    {"u32", "fixed-scalars", "component-list-composition", "component-list-composition"},
    {"i32", "fixed-scalars", "component-list-composition", "component-list-composition"},
    {"u8n", "numeric-edges", "component-list-composition", "component-list-composition"},
    {"u16n", "numeric-edges", "component-list-composition", "component-list-composition"},
    {"f32", "fixed-scalars", "component-list-composition", "component-list-composition"},
    {"f64", "fixed-scalars", "component-list-composition", "component-list-composition"},
    {"str", "text-bytes", "component-list-composition", "component-list-composition"},
    {"wstr", "text-bytes", "component-list-composition", "component-list-composition"},
    {"bytes", "text-bytes", "component-list-composition", "component-list-composition"},
    {"component", "nested-components", "component-list-composition", "component-list-composition"},
    {"list", "nested-lists", "component-list-composition", "nested-lists"},
}};

int test_task5_recursive_lists_and_named_algebra_matrix() {
    for (const AlgebraCoverageRow& row : task5_algebra_coverage) {
        require(!row.kind.empty());
        require(!row.root_case.empty());
        require(!row.component_case.empty());
        require(!row.list_case.empty());
    }

    const std::string nested_source =
        read_binary_fixture("spec/nested-lists.source.json");
    const std::string composition_source =
        read_binary_fixture("spec/component-list-composition.source.json");
    require(!nested_source.empty());
    require(!composition_source.empty());
    auto nested_spec = CompiledSpec::compile(nested_source);
    auto composition_spec = CompiledSpec::compile(composition_source);
    require(nested_spec.has_value());
    require(composition_spec.has_value());
    auto nested_values =
        build_scenario(nested_spec.value(), "nested_lists");
    auto composition_values = build_scenario(
        composition_spec.value(), "component_list_composition");
    require(nested_values.has_value(),
            nested_values.has_value()
                ? std::string_view{}
                : nested_values.error().details_json());
    require(composition_values.has_value(),
            composition_values.has_value()
                ? std::string_view{}
                : composition_values.error().details_json());
    auto nested_runtime = RuntimeSchema::compile(nested_spec.value());
    auto composition_runtime = RuntimeSchema::compile(composition_spec.value());
    require(nested_runtime.has_value());
    require(composition_runtime.has_value());
    require(nested_runtime.value().list_nodes().size() == 7U);
    require(composition_runtime.value().list_nodes().size() == 16U);
    for (std::size_t index = 1U;
         index < nested_runtime.value().list_nodes().size(); ++index) {
        require(nested_runtime.value().list_nodes()[index - 1U]
                    .owner_runtime_type_id <
                nested_runtime.value().list_nodes()[index]
                    .owner_runtime_type_id);
    }

    auto nested_layout =
        RecordLayout::plan(nested_runtime.value(), nested_values.value());
    const std::string nested_layout_error =
        nested_layout.has_value()
            ? std::string{}
            : std::to_string(nested_layout.error().code()) + ":" +
                  std::string(nested_layout.error().path()) + ":" +
                  std::string(nested_layout.error().details_json());
    require(nested_layout.has_value(),
            nested_layout_error);
    auto composition_layout = RecordLayout::plan(
        composition_runtime.value(), composition_values.value());
    require(composition_layout.has_value(),
            composition_layout.has_value()
                ? std::string_view{}
                : composition_layout.error().details_json());

    constexpr std::array<std::uint64_t, 7> expected_nested_counts{{
        UINT64_C(3), UINT64_C(3), UINT64_C(3), UINT64_C(3),
        UINT64_C(3), UINT64_C(3), UINT64_C(2)}};
    std::array<std::uint64_t, 7> actual_nested_counts{};
    std::size_t list_index = 0U;
    for (const auto& region : nested_layout.value().regions()) {
        if (region.kind !=
            fastdb::payload::layout::RegionKind::list_items) {
            continue;
        }
        require(list_index < actual_nested_counts.size());
        actual_nested_counts[list_index++] = region.element_count;
    }
    require(list_index == actual_nested_counts.size());
    require(actual_nested_counts == expected_nested_counts);
    require(nested_layout.value().region_count() == UINT32_C(21));
    require(nested_layout.value().descriptor_facts().size() == 27U);
    require(nested_layout.value().list_aggregates().size() == 7U);
    require(nested_layout.value().list_aggregates().front()
                .owner_runtime_type_id == UINT32_C(1));
    require(nested_layout.value().descriptor_facts().front()
                .runtime_type_id == UINT32_C(6));
    require(nested_layout.value().descriptor_facts().front()
                .runtime_type_id !=
            nested_layout.value().list_aggregates().front()
                .owner_runtime_type_id);
    for (const auto& fact : nested_layout.value().descriptor_facts()) {
        require(nested_layout.value().descriptor_fact(fact.node_index) ==
                &fact);
    }
    const auto& scalar_nodes = nested_layout.value().entry_values()[1];
    require(scalar_nodes.size() == 3U);
    const auto* null_scalar_list =
        nested_layout.value().descriptor_fact(scalar_nodes[0]);
    const auto* empty_scalar_list =
        nested_layout.value().descriptor_fact(scalar_nodes[1]);
    const auto* present_scalar_list =
        nested_layout.value().descriptor_fact(scalar_nodes[2]);
    require(null_scalar_list != nullptr && empty_scalar_list != nullptr &&
            present_scalar_list != nullptr);
    require(null_scalar_list->first == UINT64_C(0) &&
            null_scalar_list->count == UINT64_C(0));
    require(empty_scalar_list->first == UINT64_C(0) &&
            empty_scalar_list->count == UINT64_C(0));
    require(present_scalar_list->first == UINT64_C(0) &&
            present_scalar_list->count == UINT64_C(3));
    for (const auto& list : nested_runtime.value().list_nodes()) {
        const auto* aggregate = nested_layout.value().list_aggregate(
            list.owner_runtime_type_id);
        require(aggregate != nullptr);
        require(aggregate->item_runtime_type_id ==
                list.item_runtime_type_id);
        require(aggregate->items_region_index != UINT32_MAX);
        require((aggregate->validity_region_index != UINT32_MAX) ==
                list.item_nullable);
    }

    VectorSink nested_sink(nested_layout.value().total_length());
    require(fastdb::payload::build::encode_record(
                nested_layout.value(), nested_values.value(), nested_sink)
                .has_value());
    require(nested_sink.next_offset() ==
            nested_layout.value().total_length());
    VectorSink composition_sink(composition_layout.value().total_length());
    require(fastdb::payload::build::encode_record(
                composition_layout.value(), composition_values.value(),
                composition_sink)
                .has_value());
    require(composition_sink.next_offset() ==
            composition_layout.value().total_length());

    auto missing_runtime = RuntimeSchema::compile(nested_spec.value());
    require(missing_runtime.has_value());
    fastdb::payload::layout::RuntimeSchemaTestAccess::remove_last_list(
        missing_runtime.value());
    const auto missing_layout =
        RecordLayout::plan(missing_runtime.value(), nested_values.value());
    require(!missing_layout.has_value());
    require(missing_layout.error().code() == FDB_PAYLOAD_E_INTERNAL);
    require(missing_layout.error().path() == "/runtime/lists");
    require(missing_layout.error().details_json() ==
            R"({"reason":"missing_list_runtime_metadata"})");

    auto cyclic_runtime = RuntimeSchema::compile(nested_spec.value());
    require(cyclic_runtime.has_value());
    fastdb::payload::layout::RuntimeSchemaTestAccess::make_first_list_cyclic(
        cyclic_runtime.value());
    const auto cyclic_layout =
        RecordLayout::plan(cyclic_runtime.value(), nested_values.value());
    require(!cyclic_layout.has_value());
    require(cyclic_layout.error().code() == FDB_PAYLOAD_E_INTERNAL);
    require(cyclic_layout.error().path() == "/runtime/lists");
    require(cyclic_layout.error().details_json() ==
            R"({"reason":"cyclic_list_runtime_metadata"})");
    return EXIT_SUCCESS;
}

std::string simple_list_source() {
    return R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"lists","cardinality":"many","type":{"kind":"list","nullable":true,"items":{"kind":"u8","nullable":true}}}],"components":[]})";
}

std::string zero_count_list_aggregate_source() {
    return simple_list_source();
}

std::string wide_list_source() {
    return R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"wide","cardinality":"one","type":{"kind":"list","items":{"kind":"u16"}}}],"components":[]})";
}

std::string wide_budget_list_source() {
    return R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"wide","cardinality":"one","type":{"kind":"list","items":{"kind":"u8"}}}],"components":[]})";
}

std::string wide_budget_many_source() {
    return R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"wide","cardinality":"many","type":{"kind":"u8"}}],"components":[]})";
}

std::string wide_budget_component_source() {
    constexpr std::uint32_t width = UINT32_C(4096);
    std::string source =
        R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"wide","cardinality":"one","type":{"kind":"component","id":"Wide"}}],"components":[{"id":"Wide","kind":"record","fields":[)";
    for (std::uint32_t index = UINT32_C(0); index < width; ++index) {
        if (index != UINT32_C(0)) {
            source += ',';
        }
        source += "{\"id\":\"f" + std::to_string(index) +
                  "\",\"type\":{\"kind\":\"u8\"}}";
    }
    source += "]}]}";
    return source;
}

std::string nested_simple_source() {
    return R"({"schema":"fastdb.payload.v1","profile":"record.v1","entries":[{"id":"nested","cardinality":"one","type":{"kind":"list","items":{"kind":"list","nullable":true,"items":{"kind":"u8"}}}}],"components":[]})";
}

int test_task5_zero_count_list_aggregate_regions() {
    auto encoded = encode_source_scenario(
        zero_count_list_aggregate_source(), "zero_count_list_aggregate");
    require(encoded.has_value(),
            encoded.has_value() ? std::string_view{}
                                : encoded.error().details_json());

    auto runtime = RuntimeSchema::compile(encoded.value().spec);
    require(runtime.has_value());
    require(runtime.value().list_nodes().size() == 1U);
    const auto& list_node = runtime.value().list_nodes().front();
    require(list_node.owner_runtime_type_id == UINT32_C(0));
    require(list_node.item_runtime_type_id == UINT32_C(1));
    require(list_node.item_nullable);

    const auto& layout = encoded.value().layout;
    const auto& regions = layout.regions();
    require(regions.size() == 4U);
    require(regions[0].kind ==
            fastdb::payload::layout::RegionKind::entry_validity);
    require(regions[0].owner_index == UINT32_C(0));
    require(regions[0].runtime_type_id == UINT32_C(0));
    require(regions[0].data_offset == UINT64_C(392));
    require(regions[0].byte_length == UINT64_C(1));
    require(regions[0].element_count == UINT64_C(2));
    require(regions[0].stride == UINT32_C(0));
    require(regions[0].alignment == UINT32_C(1));
    require(regions[1].kind ==
            fastdb::payload::layout::RegionKind::entry_values);
    require(regions[1].owner_index == UINT32_C(0));
    require(regions[1].runtime_type_id == UINT32_C(0));
    require(regions[1].data_offset == UINT64_C(400));
    require(regions[1].byte_length == UINT64_C(32));
    require(regions[1].element_count == UINT64_C(2));
    require(regions[1].stride == UINT32_C(16));
    require(regions[1].alignment == UINT32_C(8));

    const auto& list_validity = regions[2];
    const auto& list_items = regions[3];
    require(list_validity.kind ==
            fastdb::payload::layout::RegionKind::list_validity);
    require(list_validity.owner_index == UINT32_C(0));
    require(list_validity.runtime_type_id == UINT32_C(1));
    require(list_validity.data_offset == UINT64_C(432));
    require(list_validity.byte_length == UINT64_C(0));
    require(list_validity.element_count == UINT64_C(0));
    require(list_validity.stride == UINT32_C(0));
    require(list_validity.alignment == UINT32_C(1));
    require(list_items.kind ==
            fastdb::payload::layout::RegionKind::list_items);
    require(list_items.owner_index == UINT32_C(0));
    require(list_items.runtime_type_id == UINT32_C(1));
    require(list_items.data_offset == UINT64_C(432));
    require(list_items.byte_length == UINT64_C(0));
    require(list_items.element_count == UINT64_C(0));
    require(list_items.stride == UINT32_C(1));
    require(list_items.alignment == UINT32_C(1));
    require(list_validity.data_offset == list_items.data_offset);
    require(list_items.data_offset == layout.total_length());

    require(encoded.value().bytes[static_cast<std::size_t>(
                regions[0].data_offset)] == UINT8_C(0x02));
    const auto descriptor_word = [&](std::uint64_t instance,
                                     std::uint64_t word) {
        return fastdb::payload::layout::load_u64_le(
            encoded.value().bytes.data(), encoded.value().bytes.size(),
            regions[1].data_offset + instance * UINT64_C(16) +
                word * UINT64_C(8),
            JsonPointer{});
    };
    for (std::uint64_t instance = UINT64_C(0); instance < UINT64_C(2);
         ++instance) {
        require(descriptor_word(instance, UINT64_C(0)).value() ==
                UINT64_C(0));
        require(descriptor_word(instance, UINT64_C(1)).value() ==
                UINT64_C(0));
    }

    const auto opened = fastdb::payload::view::open_record(
        encoded.value().spec, encoded.value().bytes.data(),
        encoded.value().bytes.size());
    require(opened.has_value());
    require(opened.value().validation_work() == layout.validation_work());
    return EXIT_SUCCESS;
}

int test_task5_list_region_descriptor_and_partition_failures() {
    auto encoded = encode_source_scenario(simple_list_source(), "simple_lists");
    require(encoded.has_value(),
            encoded.has_value() ? std::string_view{}
                                : encoded.error().details_json());
    const auto& regions = encoded.value().layout.regions();
    require(regions.size() == 4U);
    require(regions[0].kind ==
            fastdb::payload::layout::RegionKind::entry_validity);
    require(regions[1].kind ==
            fastdb::payload::layout::RegionKind::entry_values);
    require(regions[2].kind ==
            fastdb::payload::layout::RegionKind::list_validity);
    require(regions[3].kind ==
            fastdb::payload::layout::RegionKind::list_items);
    require(regions[3].element_count == UINT64_C(4));
    require(encoded.value().bytes[static_cast<std::size_t>(
                regions[0].data_offset)] == UINT8_C(0x0e));
    const auto descriptor_word = [&](std::uint64_t instance,
                                     std::uint64_t word) {
        return fastdb::payload::layout::load_u64_le(
            encoded.value().bytes.data(), encoded.value().bytes.size(),
            regions[1].data_offset + instance * UINT64_C(16) +
                word * UINT64_C(8),
            JsonPointer{});
    };
    require(descriptor_word(UINT64_C(0), UINT64_C(0)).value() ==
            UINT64_C(0));
    require(descriptor_word(UINT64_C(0), UINT64_C(1)).value() ==
            UINT64_C(0));
    require(descriptor_word(UINT64_C(1), UINT64_C(0)).value() ==
            UINT64_C(0));
    require(descriptor_word(UINT64_C(1), UINT64_C(1)).value() ==
            UINT64_C(0));
    require(descriptor_word(UINT64_C(2), UINT64_C(0)).value() ==
            UINT64_C(0));
    require(descriptor_word(UINT64_C(2), UINT64_C(1)).value() ==
            UINT64_C(3));
    require(descriptor_word(UINT64_C(3), UINT64_C(0)).value() ==
            UINT64_C(3));
    require(descriptor_word(UINT64_C(3), UINT64_C(1)).value() ==
            UINT64_C(1));
    require(encoded.value().bytes[static_cast<std::size_t>(
                regions[2].data_offset)] == UINT8_C(0x0d));
    require(encoded.value().bytes[static_cast<std::size_t>(
                regions[3].data_offset)] == UINT8_C(1));
    require(encoded.value().bytes[static_cast<std::size_t>(
                regions[3].data_offset + UINT64_C(1))] == UINT8_C(0));
    require(encoded.value().bytes[static_cast<std::size_t>(
                regions[3].data_offset + UINT64_C(2))] == UINT8_C(2));
    require(encoded.value().bytes[static_cast<std::size_t>(
                regions[3].data_offset + UINT64_C(3))] == UINT8_C(3));
    const auto open = [&](const std::vector<std::uint8_t>& bytes) {
        return fastdb::payload::view::open_record(
            encoded.value().spec, bytes.data(), bytes.size());
    };
    const auto opened = open(encoded.value().bytes);
    const std::string open_error =
        opened.has_value()
            ? std::string{}
            : std::to_string(opened.error().code()) + ":" +
                  std::string(opened.error().path()) + ":" +
                  std::string(opened.error().details_json());
    require(opened.has_value(), open_error);

    for (const std::uint32_t declared_regions :
         std::array<std::uint32_t, 2>{UINT32_C(3), UINT32_C(5)}) {
        auto wrong_inventory = encoded.value().bytes;
        require(fastdb::payload::layout::store_u32_le(
                    wrong_inventory.data(), wrong_inventory.size(),
                    fastdb::payload::layout::header_region_count_offset,
                    declared_regions, JsonPointer{})
                    .has_value());
        require(require_exact_error(
                    open(wrong_inventory),
                    FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                    "/binary/header/region_count", declared_regions,
                    regions.size(), "region_count") == EXIT_SUCCESS);
    }

    const auto mutate_u32 = [&](std::uint32_t region,
                                std::uint64_t field,
                                std::uint32_t value) {
        auto bytes = encoded.value().bytes;
        if (!fastdb::payload::layout::store_u32_le(
                 bytes.data(), bytes.size(), region_field(region, field),
                 value, JsonPointer{})
                 .has_value()) {
            std::abort();
        }
        return bytes;
    };
    const auto mutate_u64 = [&](std::uint32_t region,
                                std::uint64_t field,
                                std::uint64_t value) {
        auto bytes = encoded.value().bytes;
        if (!fastdb::payload::layout::store_u64_le(
                 bytes.data(), bytes.size(), region_field(region, field),
                 value, JsonPointer{})
                 .has_value()) {
            std::abort();
        }
        return bytes;
    };
    require(require_exact_error(
                open(mutate_u64(
                    UINT32_C(3),
                    fastdb::payload::layout::region_element_count_offset,
                    UINT64_C(5))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/3/byte_length", regions[3].byte_length,
                UINT64_C(5), "region_byte_length") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(3),
                    fastdb::payload::layout::region_kind_offset,
                    static_cast<std::uint32_t>(
                        fastdb::payload::layout::RegionKind::list_validity))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/3/kind",
                static_cast<std::uint32_t>(
                    fastdb::payload::layout::RegionKind::list_validity),
                static_cast<std::uint32_t>(regions[3].kind), "region_kind") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(UINT32_C(3),
                                fastdb::payload::layout::region_stride_offset,
                                UINT32_C(2))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/3/stride", UINT64_C(2), regions[3].stride,
                "region_stride") == EXIT_SUCCESS);

    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(2),
                    fastdb::payload::layout::region_kind_offset,
                    static_cast<std::uint32_t>(
                        fastdb::payload::layout::RegionKind::list_items))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/2/kind",
                static_cast<std::uint32_t>(
                    fastdb::payload::layout::RegionKind::list_items),
                static_cast<std::uint32_t>(regions[2].kind), "region_kind") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u64(
                    UINT32_C(2),
                    fastdb::payload::layout::region_byte_length_offset,
                    UINT64_C(2))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/2/byte_length", UINT64_C(2),
                regions[2].byte_length, "region_byte_length") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(2),
                    fastdb::payload::layout::region_stride_offset,
                    UINT32_C(1))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/2/stride", UINT64_C(1), regions[2].stride,
                "region_stride") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(2),
                    fastdb::payload::layout::region_owner_index_offset,
                    UINT32_C(7))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/2/owner_index", UINT64_C(7),
                regions[2].owner_index, "region_owner_index") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(2),
                    fastdb::payload::layout::region_runtime_type_id_offset,
                    UINT32_MAX)),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/2/runtime_type_id", UINT32_MAX,
                regions[2].runtime_type_id, "region_runtime_type_id") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(2),
                    fastdb::payload::layout::region_alignment_offset,
                    UINT32_C(2))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/2/alignment", UINT64_C(2),
                regions[2].alignment, "region_alignment") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u64(
                    UINT32_C(2),
                    fastdb::payload::layout::region_element_count_offset,
                    UINT64_C(5))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/3/element_count",
                regions[3].element_count, UINT64_C(5),
                "list_validity_element_count") == EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(3),
                    fastdb::payload::layout::region_owner_index_offset,
                    UINT32_C(7))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/3/owner_index", UINT64_C(7),
                regions[3].owner_index, "region_owner_index") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(3),
                    fastdb::payload::layout::region_runtime_type_id_offset,
                    UINT32_MAX)),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/3/runtime_type_id", UINT32_MAX,
                regions[3].runtime_type_id, "region_runtime_type_id") ==
            EXIT_SUCCESS);
    require(require_exact_error(
                open(mutate_u32(
                    UINT32_C(3),
                    fastdb::payload::layout::region_alignment_offset,
                    UINT32_C(2))),
                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                "/binary/regions/3/alignment", UINT64_C(2),
                regions[3].alignment, "region_alignment") == EXIT_SUCCESS);

    auto bad_tail = encoded.value().bytes;
    bad_tail[static_cast<std::size_t>(regions[2].data_offset)] |= UINT8_C(0x80);
    require(require_exact_error(open(bad_tail),
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "/binary/regions/2/data", UINT64_C(128),
                                UINT64_C(0), "nonzero_validity_tail") ==
            EXIT_SUCCESS);

    const std::uint64_t descriptor_base = regions[1].data_offset;
    const auto mutate_descriptor = [&](std::uint64_t instance,
                                       std::uint64_t first,
                                       std::uint64_t count) {
        auto bytes = encoded.value().bytes;
        const std::uint64_t offset =
            descriptor_base + instance * UINT64_C(16);
        if (!fastdb::payload::layout::store_u64_le(
                 bytes.data(), bytes.size(), offset, first, JsonPointer{})
                 .has_value() ||
            !fastdb::payload::layout::store_u64_le(
                 bytes.data(), bytes.size(), offset + UINT64_C(8), count,
                 JsonPointer{})
                 .has_value()) {
            std::abort();
        }
        return bytes;
    };
    require(require_reason_error(open(mutate_descriptor(UINT64_C(0),
                                                        UINT64_C(1),
                                                        UINT64_C(0))),
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "/entries/lists/0",
                                "nonzero_null_storage") == EXIT_SUCCESS);
    require(require_partition_error(
                open(mutate_descriptor(UINT64_C(2), UINT64_C(1),
                                       UINT64_C(3))),
                "/entries/lists/2", UINT64_C(0), UINT64_C(1),
                "partition_cursor_mismatch") == EXIT_SUCCESS);
    require(require_partition_error(
                open(mutate_descriptor(UINT64_C(3), UINT64_C(2),
                                       UINT64_C(1))),
                "/entries/lists/3", UINT64_C(3), UINT64_C(2),
                "partition_cursor_mismatch") == EXIT_SUCCESS);
    require(require_partition_error(
                open(mutate_descriptor(UINT64_C(3), UINT64_C(0),
                                       UINT64_C(1))),
                "/entries/lists/3", UINT64_C(3), UINT64_C(0),
                "partition_cursor_mismatch") == EXIT_SUCCESS);
    require(require_partition_error(
                open(mutate_descriptor(UINT64_C(2), UINT64_C(3),
                                       UINT64_C(1))),
                "/entries/lists/2", UINT64_C(0), UINT64_C(3),
                "partition_cursor_mismatch") == EXIT_SUCCESS);
    require(require_reason_error(
                open(mutate_descriptor(UINT64_C(2), UINT64_MAX,
                                       UINT64_C(2))),
                FDB_PAYLOAD_E_LENGTH_OVERFLOW, "/entries/lists/2",
                "wire_add_overflow") == EXIT_SUCCESS);

    auto nonzero_null_item = encoded.value().bytes;
    nonzero_null_item[static_cast<std::size_t>(
        regions[3].data_offset + UINT64_C(1))] = UINT8_C(9);
    require(require_reason_error(open(nonzero_null_item),
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "/entries/lists/2/1",
                                "nonzero_null_storage") == EXIT_SUCCESS);

    auto unconsumed = encoded.value().bytes;
    require(fastdb::payload::layout::store_u64_le(
                unconsumed.data(), unconsumed.size(),
                region_field(
                    UINT32_C(2),
                    fastdb::payload::layout::region_element_count_offset),
                UINT64_C(5), JsonPointer{})
                .has_value());
    require(fastdb::payload::layout::store_u64_le(
                unconsumed.data(), unconsumed.size(),
                region_field(UINT32_C(3),
                             fastdb::payload::layout::region_byte_length_offset),
                UINT64_C(5), JsonPointer{})
                .has_value());
    require(fastdb::payload::layout::store_u64_le(
                unconsumed.data(), unconsumed.size(),
                region_field(
                    UINT32_C(3),
                    fastdb::payload::layout::region_element_count_offset),
                UINT64_C(5), JsonPointer{})
                .has_value());
    require(require_partition_error(open(unconsumed),
                                    "/binary/regions/3/element_count",
                                    UINT64_C(5), UINT64_C(4),
                                    "partition_not_consumed") ==
            EXIT_SUCCESS);

    auto wide = encode_source_scenario(wide_list_source(), "wide_lists");
    require(wide.has_value());
    const std::uint64_t wide_descriptor =
        wide.value().layout.regions()[0].data_offset;
    const auto mutate_wide_descriptor = [&](std::uint64_t first,
                                            std::uint64_t count) {
        auto bytes = wide.value().bytes;
        if (!fastdb::payload::layout::store_u64_le(
                 bytes.data(), bytes.size(), wide_descriptor, first,
                 JsonPointer{})
                 .has_value() ||
            !fastdb::payload::layout::store_u64_le(
                 bytes.data(), bytes.size(),
                 wide_descriptor + UINT64_C(8), count, JsonPointer{})
                 .has_value()) {
            std::abort();
        }
        return bytes;
    };
    const auto open_wide = [&](const std::vector<std::uint8_t>& bytes) {
        return fastdb::payload::view::open_record(
            wide.value().spec, bytes.data(), bytes.size());
    };
    require(require_reason_error(
                open_wide(mutate_wide_descriptor(
                    UINT64_MAX / UINT64_C(2) + UINT64_C(1), UINT64_C(0))),
                FDB_PAYLOAD_E_LENGTH_OVERFLOW, "/entries/wide",
                "wire_multiply_overflow") == EXIT_SUCCESS);
    require(require_reason_error(
                open_wide(mutate_wide_descriptor(UINT64_C(0), UINT64_MAX)),
                FDB_PAYLOAD_E_LENGTH_OVERFLOW, "/entries/wide",
                "wire_multiply_overflow") == EXIT_SUCCESS);

    auto nested =
        encode_source_scenario(nested_simple_source(), "nested_simple");
    require(nested.has_value());
    require(nested.value().layout.regions().size() == 4U);
    auto invalid_nested = nested.value().bytes;
    const std::uint64_t inner_descriptor =
        nested.value().layout.regions()[2].data_offset + UINT64_C(32);
    require(fastdb::payload::layout::store_u64_le(
                invalid_nested.data(), invalid_nested.size(),
                inner_descriptor, UINT64_C(2), JsonPointer{})
                .has_value());
    const auto invalid_nested_result =
        fastdb::payload::view::open_record(
            nested.value().spec, invalid_nested.data(),
            invalid_nested.size());
    require(!invalid_nested_result.has_value());
    require(invalid_nested_result.error().code() == FDB_PAYLOAD_E_OUT_OF_BOUNDS);
    require(invalid_nested_result.error().path() == "/entries/nested/2");
    require(invalid_nested_result.error().details_json() ==
            R"({"available":"1","end":"3","reason":"wire_range_out_of_bounds"})");
    return EXIT_SUCCESS;
}

int test_task5_list_limits_and_20000_level_iteration() {
    auto simple = encode_source_scenario(simple_list_source(), "simple_lists");
    require(simple.has_value());
    auto limits = fastdb::payload::view::default_open_options();
    limits.max_list_elements = UINT64_C(4);
    require(fastdb::payload::view::open_record(
                simple.value().spec, simple.value().bytes.data(),
                simple.value().bytes.size(), limits)
                .has_value());
    limits.max_list_elements = UINT64_C(3);
    const auto limited_first = fastdb::payload::view::open_record(
        simple.value().spec, simple.value().bytes.data(),
        simple.value().bytes.size(), limits);
    const auto limited_second = fastdb::payload::view::open_record(
        simple.value().spec, simple.value().bytes.data(),
        simple.value().bytes.size(), limits);
    require(require_resource_limit(limited_first,
                                   "/binary/regions/3/element_count",
                                   UINT64_C(4),
                                   UINT64_C(3), "list_elements") ==
            EXIT_SUCCESS);
    require(limited_second.error().path() == limited_first.error().path());
    require(limited_second.error().details_json() ==
            limited_first.error().details_json());

    const auto simple_unrestricted = fastdb::payload::view::open_record(
        simple.value().spec, simple.value().bytes.data(),
        simple.value().bytes.size());
    require(simple_unrestricted.has_value());
    auto work_limits = fastdb::payload::view::default_open_options();
    work_limits.max_validation_work =
        simple_unrestricted.value().validation_work();
    require(fastdb::payload::view::open_record(
                simple.value().spec, simple.value().bytes.data(),
                simple.value().bytes.size(), work_limits)
                .has_value());
    --work_limits.max_validation_work;
    const auto work_short_first = fastdb::payload::view::open_record(
        simple.value().spec, simple.value().bytes.data(),
        simple.value().bytes.size(), work_limits);
    const auto work_short_second = fastdb::payload::view::open_record(
        simple.value().spec, simple.value().bytes.data(),
        simple.value().bytes.size(), work_limits);
    require(require_resource_limit(
                work_short_first, "/entries/lists/3/0",
                simple_unrestricted.value().validation_work(),
                simple_unrestricted.value().validation_work() - UINT64_C(1),
                "validation_work") == EXIT_SUCCESS);
    require(work_short_second.error().path() ==
            work_short_first.error().path());
    require(work_short_second.error().details_json() ==
            work_short_first.error().details_json());

    const std::string composition_source =
        read_binary_fixture("spec/component-list-composition.source.json");
    auto composition = encode_source_scenario(
        composition_source, "component_list_composition");
    require(composition.has_value());
    const std::string nested_order_source =
        read_binary_fixture("spec/nested-lists.source.json");
    auto nested_order =
        encode_source_scenario(nested_order_source, "nested_lists");
    require(nested_order.has_value());
    auto logical_order_limits = fastdb::payload::view::default_open_options();
    logical_order_limits.max_validation_work = UINT64_C(103);
    const auto logical_order_first = fastdb::payload::view::open_record(
        nested_order.value().spec, nested_order.value().bytes.data(),
        nested_order.value().bytes.size(), logical_order_limits);
    const auto logical_order_second = fastdb::payload::view::open_record(
        nested_order.value().spec, nested_order.value().bytes.data(),
        nested_order.value().bytes.size(), logical_order_limits);
    require(require_resource_limit(
                logical_order_first,
                "/entries/envelopes/0/text_groups/0", UINT64_C(104),
                UINT64_C(103), "validation_work") == EXIT_SUCCESS);
    require(logical_order_second.error().path() ==
            logical_order_first.error().path());
    require(logical_order_second.error().details_json() ==
            logical_order_first.error().details_json());

    constexpr std::uint64_t wide_width = UINT64_C(4096);
    auto wide_budget_spec =
        CompiledSpec::compile(wide_budget_list_source());
    require(wide_budget_spec.has_value());
    auto wide_budget_values = build_scenario(
        wide_budget_spec.value(), "wide_budget_list");
    auto wide_budget_runtime =
        RuntimeSchema::compile(wide_budget_spec.value());
    require(wide_budget_values.has_value() &&
            wide_budget_runtime.has_value());
    allocation_failure::largest_allocation.store(
        0U, std::memory_order_relaxed);
    auto wide_budget_layout = RecordLayout::plan(
        wide_budget_runtime.value(), wide_budget_values.value());
    require(wide_budget_layout.has_value());
    const std::size_t largest_plan_allocation =
        allocation_failure::largest_allocation.load(
            std::memory_order_relaxed);
    require(largest_plan_allocation < 65536U,
            "wide plan allocation=" +
                std::to_string(largest_plan_allocation));
    VectorSink wide_budget_sink(
        wide_budget_layout.value().total_length());
    require(fastdb::payload::build::encode_record(
                wide_budget_layout.value(), wide_budget_values.value(),
                wide_budget_sink)
                .has_value());
    auto wide_work_limits = fastdb::payload::view::default_open_options();
    wide_work_limits.max_validation_work =
        wide_budget_layout.value().validation_work() - wide_width;
    allocation_failure::largest_allocation.store(
        0U, std::memory_order_relaxed);
    const auto wide_work = fastdb::payload::view::open_record(
        wide_budget_spec.value(), wide_budget_sink.bytes().data(),
        wide_budget_sink.bytes().size(), wide_work_limits);
    require(require_resource_limit(
                wide_work, "/entries/wide/0",
                wide_work_limits.max_validation_work + UINT64_C(1),
                wide_work_limits.max_validation_work,
                "validation_work") == EXIT_SUCCESS);
    const std::size_t largest_open_allocation =
        allocation_failure::largest_allocation.load(
            std::memory_order_relaxed);
    require(largest_open_allocation < 65536U,
            "wide open allocation=" +
                std::to_string(largest_open_allocation));

    auto wide_many_spec =
        CompiledSpec::compile(wide_budget_many_source());
    require(wide_many_spec.has_value());
    auto wide_many_values = build_scenario(
        wide_many_spec.value(), "wide_budget_many");
    auto wide_many_runtime =
        RuntimeSchema::compile(wide_many_spec.value());
    require(wide_many_values.has_value() &&
            wide_many_runtime.has_value());
    allocation_failure::largest_allocation.store(
        0U, std::memory_order_relaxed);
    auto wide_many_layout = RecordLayout::plan(
        wide_many_runtime.value(), wide_many_values.value());
    require(wide_many_layout.has_value());
    const std::size_t largest_many_plan_allocation =
        allocation_failure::largest_allocation.load(
            std::memory_order_relaxed);
    require(largest_many_plan_allocation < 65536U,
            "wide many plan allocation=" +
                std::to_string(largest_many_plan_allocation));

    constexpr std::uint64_t component_width = UINT64_C(4096);
    auto wide_component_spec =
        CompiledSpec::compile(wide_budget_component_source());
    require(wide_component_spec.has_value());
    auto wide_component_values = build_scenario(
        wide_component_spec.value(), "wide_budget_component");
    auto wide_component_runtime =
        RuntimeSchema::compile(wide_component_spec.value());
    require(wide_component_values.has_value() &&
            wide_component_runtime.has_value());
    allocation_failure::largest_allocation.store(
        0U, std::memory_order_relaxed);
    auto wide_component_layout = RecordLayout::plan(
        wide_component_runtime.value(), wide_component_values.value());
    require(wide_component_layout.has_value());
    const std::size_t largest_component_plan_allocation =
        allocation_failure::largest_allocation.load(
            std::memory_order_relaxed);
    require(largest_component_plan_allocation < 200000U,
            "wide component plan allocation=" +
                std::to_string(largest_component_plan_allocation));
    VectorSink wide_component_sink(
        wide_component_layout.value().total_length());
    require(fastdb::payload::build::encode_record(
                wide_component_layout.value(),
                wide_component_values.value(), wide_component_sink)
                .has_value());
    auto component_work_limits =
        fastdb::payload::view::default_open_options();
    component_work_limits.max_validation_work =
        wide_component_layout.value().validation_work() - component_width;
    allocation_failure::largest_allocation.store(
        0U, std::memory_order_relaxed);
    const auto component_work = fastdb::payload::view::open_record(
        wide_component_spec.value(), wide_component_sink.bytes().data(),
        wide_component_sink.bytes().size(), component_work_limits);
    require(require_resource_limit(
                component_work, "/entries/wide/f0",
                component_work_limits.max_validation_work + UINT64_C(1),
                component_work_limits.max_validation_work,
                "validation_work") == EXIT_SUCCESS);
    const std::size_t largest_component_open_allocation =
        allocation_failure::largest_allocation.load(
            std::memory_order_relaxed);
    require(largest_component_open_allocation < 300000U,
            "wide component open allocation=" +
                std::to_string(largest_component_open_allocation) +
                ", limit=" +
                std::to_string(component_work_limits.max_validation_work));
    auto mixed_limits = fastdb::payload::view::default_open_options();
    mixed_limits.max_nesting_depth = UINT64_C(3);
    require(fastdb::payload::view::open_record(
                composition.value().spec, composition.value().bytes.data(),
                composition.value().bytes.size(), mixed_limits)
                .has_value());
    mixed_limits.max_nesting_depth = UINT64_C(2);
    const auto mixed_short_first = fastdb::payload::view::open_record(
        composition.value().spec, composition.value().bytes.data(),
        composition.value().bytes.size(), mixed_limits);
    const auto mixed_short_second = fastdb::payload::view::open_record(
        composition.value().spec, composition.value().bytes.data(),
        composition.value().bytes.size(), mixed_limits);
    require(require_resource_limit(mixed_short_first,
                                   "/entries/records/0/leaves/1",
                                   UINT64_C(3), UINT64_C(2),
                                   "nesting_depth") == EXIT_SUCCESS);
    require(mixed_short_second.error().path() ==
            mixed_short_first.error().path());
    require(mixed_short_second.error().details_json() ==
            mixed_short_first.error().details_json());

    constexpr std::uint32_t depth = UINT32_C(20000);
    const std::string source = deep_list_spec(depth);
    fastdb::payload::spec::CompileLimits compile_limits{};
    compile_limits.json.max_source_bytes = UINT64_C(64) * UINT64_C(1024) *
                                           UINT64_C(1024);
    compile_limits.json.max_json_values = UINT64_C(100000);
    compile_limits.json.max_nesting_depth = depth + UINT32_C(16);
    auto compiled = CompiledSpec::compile(source, compile_limits);
    require(compiled.has_value());
    auto builder_limits = fastdb::payload::build::default_builder_limits();
    builder_limits.max_nesting_depth = depth;
    builder_limits.max_total_builder_bytes =
        UINT64_C(8) * UINT64_C(1024) * UINT64_C(1024);
    auto builder = PayloadBuilder::create(compiled.value(), builder_limits);
    require(builder.has_value());
    require(builder.value().begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    for (std::uint32_t index = UINT32_C(0); index < depth; ++index) {
        require(builder.value().begin_list(UINT64_C(1)).has_value());
    }
    require(builder.value().push_u8(UINT8_C(42)).has_value());
    auto values = builder.value().freeze();
    require(values.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto planned = RecordLayout::plan(runtime.value(), values.value());
    require(planned.has_value(),
            planned.has_value() ? std::string_view{}
                                : planned.error().details_json());
    VectorSink sink(planned.value().total_length());
    require(fastdb::payload::build::encode_record(
                planned.value(), values.value(), sink)
                .has_value());
    auto open_limits = fastdb::payload::view::default_open_options();
    open_limits.max_regions = UINT64_C(50000);
    open_limits.max_nesting_depth = depth;
    open_limits.max_list_elements = depth;
    open_limits.max_validation_work = UINT64_C(1000000);
    const std::size_t open_live_baseline =
        allocation_failure::current_live_bytes.load(
            std::memory_order_relaxed);
    allocation_failure::peak_live_bytes.store(
        open_live_baseline, std::memory_order_relaxed);
    const auto deep_open = fastdb::payload::view::open_record(
        compiled.value(), sink.bytes().data(), sink.bytes().size(),
        open_limits);
    const std::size_t open_peak_live =
        allocation_failure::peak_live_bytes.load(
            std::memory_order_relaxed);
    const std::size_t open_peak_delta =
        open_peak_live - open_live_baseline;
    require(deep_open.has_value());
    require(open_peak_delta < 64U * 1024U * 1024U,
            "20000-level open peak live requested bytes=" +
                std::to_string(open_peak_delta));
    open_limits.max_nesting_depth = depth - UINT32_C(1);
    std::string depth_path = "/entries/deep";
    for (std::uint32_t index = UINT32_C(1); index < depth; ++index) {
        depth_path += "/0";
    }
    require(require_resource_limit(
                fastdb::payload::view::open_record(
                    compiled.value(), sink.bytes().data(), sink.bytes().size(),
                    open_limits),
                depth_path, depth, depth - UINT32_C(1), "nesting_depth") ==
            EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int test_task5_list_allocation_failure_sweeps() {
    const std::string source =
        read_binary_fixture("spec/component-list-composition.source.json");
    auto compiled = CompiledSpec::compile(source);
    require(compiled.has_value());
    auto values = build_scenario(compiled.value(),
                                 "component_list_composition");
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(values.has_value() && runtime.has_value());

    Result<RecordLayout> planned = Result<RecordLayout>::failure(
        fastdb::payload::error::Error::from_details(
            FDB_PAYLOAD_E_INTERNAL, JsonPointer{}, "uninitialized",
            fastdb::payload::json::JsonValue::object({})));
    std::uint64_t layout_failures = UINT64_C(0);
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(8192);
         ++allocation) {
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        try {
            planned = RecordLayout::plan(runtime.value(), values.value());
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "list layout escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (planned.has_value()) {
            break;
        }
        require(planned.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++layout_failures;
    }
    require(planned.has_value());
    require(layout_failures > UINT64_C(0));

    std::vector<std::uint8_t> encoded;
    std::uint64_t encode_failures = UINT64_C(0);
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(8192);
         ++allocation) {
        VectorSink sink(planned.value().total_length());
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        Result<void> result = Result<void>::success();
        try {
            result = fastdb::payload::build::encode_record(
                planned.value(), values.value(), sink);
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "list encode escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (result.has_value()) {
            encoded = sink.bytes();
            break;
        }
        require(result.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++encode_failures;
    }
    require(!encoded.empty());
    require(encode_failures > UINT64_C(0));

    Result<fastdb::payload::view::PayloadIndex> opened =
        Result<fastdb::payload::view::PayloadIndex>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}, "uninitialized",
                fastdb::payload::json::JsonValue::object({})));
    std::uint64_t open_failures = UINT64_C(0);
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(8192);
         ++allocation) {
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        try {
            opened = fastdb::payload::view::open_record(
                compiled.value(), encoded.data(), encoded.size());
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "list open escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        if (opened.has_value()) {
            break;
        }
        require(opened.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        ++open_failures;
    }
    require(opened.has_value());
    require(opened.value().validation_work() ==
            planned.value().validation_work());
    require(open_failures > UINT64_C(0));

    const auto validity = std::find_if(
        planned.value().regions().begin(), planned.value().regions().end(),
        [](const auto& region) {
            return region.kind ==
                       fastdb::payload::layout::RegionKind::list_validity &&
                   region.element_count % UINT64_C(8) != UINT64_C(0) &&
                   region.element_count != UINT64_C(0);
        });
    require(validity != planned.value().regions().end());
    auto malformed = encoded;
    malformed[static_cast<std::size_t>(validity->data_offset +
                                       validity->byte_length - UINT64_C(1))] |=
        UINT8_C(0x80);
    bool reached_validation_error = false;
    std::uint64_t malformed_failures = UINT64_C(0);
    for (std::int64_t allocation = INT64_C(0); allocation < INT64_C(8192);
         ++allocation) {
        Result<fastdb::payload::view::PayloadIndex> result =
            Result<fastdb::payload::view::PayloadIndex>::failure(
                fastdb::payload::error::Error::from_details(
                    FDB_PAYLOAD_E_INTERNAL, JsonPointer{}, "uninitialized",
                    fastdb::payload::json::JsonValue::object({})));
        allocation_failure::fail_after.store(allocation,
                                              std::memory_order_relaxed);
        try {
            result = fastdb::payload::view::open_record(
                compiled.value(), malformed.data(), malformed.size());
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after.store(INT64_C(-1),
                                                  std::memory_order_relaxed);
            require(false, "list validation escaped bad_alloc at " +
                               std::to_string(allocation));
        }
        allocation_failure::fail_after.store(INT64_C(-1),
                                              std::memory_order_relaxed);
        require(!result.has_value());
        if (result.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED) {
            ++malformed_failures;
            continue;
        }
        require(result.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);
        require(result.error().details_json() ==
                R"({"actual":"128","expected":"0","reason":"nonzero_validity_tail"})");
        reached_validation_error = true;
        break;
    }
    require(reached_validation_error);
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_task5_recursive_lists_and_named_algebra_matrix() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task5_zero_count_list_aggregate_regions() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task5_list_region_descriptor_and_partition_failures() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task5_list_limits_and_20000_level_iteration() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task5_list_allocation_failure_sweeps() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_binary_goldens_determinism_hash_and_headers() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_digest_equal_independent_specs_share_stable_wire_layout() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_open_preflights_static_spec_limits_and_known_work() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task11_header_directory_and_descriptor_failures() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_region_matrix_zero_boundaries_and_partition_rules() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task4_zero_length_pools_and_canonical_value_offsets() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task4_pool_metadata_eager_lazy_and_traversal_order() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task4_malformed_pools_and_descriptors_have_exact_errors() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task3_metadata_observation_and_nan_canonicalization() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_open_component_nesting_depth_limits() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_large_many_component_stride_and_count_overflow() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_component_malformed_bytes_have_exact_diagnostics() !=
        EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_task4_layout_encode_open_allocation_sweeps() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_open_observation_and_malformed_canonical_values();
}
