#include "GoldenCorpus.hpp"
#include "TestSupport.hpp"

#include "payload/build/GraphEncoder.hpp"
#include "payload/build/PayloadBuilder.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/layout/BinaryFormat.hpp"
#include "payload/layout/GraphLayout.hpp"
#include "payload/layout/RuntimeSchema.hpp"
#include "payload/spec/CompiledSpec.hpp"
#include "payload/view/GraphOpen.hpp"

#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace allocation_failure {

thread_local std::int64_t fail_after = INT64_C(-1);

struct AllocationHeader final {
    void* raw;
};

constexpr std::size_t default_new_alignment =
    static_cast<std::size_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);

void* allocate(std::size_t size,
               std::size_t alignment = default_new_alignment) {
    if (fail_after >= INT64_C(0)) {
        if (fail_after == INT64_C(0)) {
            fail_after = INT64_C(-1);
            throw std::bad_alloc();
        }
        --fail_after;
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        throw std::bad_alloc();
    }
    alignment = std::max(
        {alignment, alignof(AllocationHeader), default_new_alignment});
    const std::size_t payload = size == 0U ? 1U : size;
    const std::size_t padding = alignment - 1U;
    constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (padding > maximum - sizeof(AllocationHeader)) {
        throw std::bad_alloc();
    }
    const std::size_t overhead = sizeof(AllocationHeader) + padding;
    if (payload > maximum - overhead) {
        throw std::bad_alloc();
    }
    const std::size_t allocation_size = payload + overhead;
    void* const raw = std::malloc(allocation_size);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    void* candidate = static_cast<void*>(
        static_cast<std::uint8_t*>(raw) + sizeof(AllocationHeader));
    std::size_t space = allocation_size - sizeof(AllocationHeader);
    void* const aligned = std::align(alignment, payload, candidate, space);
    if (aligned == nullptr) {
        std::free(raw);
        throw std::bad_alloc();
    }
    (static_cast<AllocationHeader*>(aligned) - 1)->raw = raw;
    return aligned;
}

void deallocate(void* value) noexcept {
    if (value != nullptr) {
        std::free((static_cast<AllocationHeader*>(value) - 1)->raw);
    }
}

struct Reset final {
    ~Reset() { fail_after = INT64_C(-1); }
};

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
void operator delete(void* value,
                     std::size_t,
                     std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}
void operator delete[](void* value,
                       std::size_t,
                       std::align_val_t) noexcept {
    allocation_failure::deallocate(value);
}

namespace {

using fastdb::payload::build::ByteSink;
using fastdb::payload::build::LogicalPayload;
using fastdb::payload::build::ObjectHandle;
using fastdb::payload::build::PayloadBuilder;
using fastdb::payload::error::Result;
using fastdb::payload::json::JsonPointer;
using fastdb::payload::layout::GraphLayout;
using fastdb::payload::layout::RegionKind;
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
                    JsonPointer{}.append("sink"),
                    "Invalid graph test sink write",
                    fastdb::payload::json::JsonValue::object({})));
        }
        std::copy_n(data, static_cast<std::size_t>(size),
                    bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        next_offset_ += size;
        return Result<void>::success();
    }

    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    std::uint64_t next_offset() const noexcept { return next_offset_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t next_offset_{UINT64_C(0)};
};

std::uint32_t component_index(const CompiledSpec& spec,
                              std::string_view id) {
    const auto index = spec.component_index(id);
    if (!index.has_value()) {
        std::abort();
    }
    return *index;
}

Result<LogicalPayload> build_graph_scenario(
    CompiledSpec spec,
    std::string_view scenario,
    bool reverse_fill = false,
    bool reverse_declarations = false) {
    auto created = PayloadBuilder::create(spec);
    if (!created.has_value()) {
        return Result<LogicalPayload>::failure(std::move(created).error());
    }
    PayloadBuilder& builder = created.value();

#define FASTDB_GRAPH_STEP(expression)                                       \
    do {                                                                     \
        auto step_result = (expression);                                     \
        if (!step_result.has_value()) {                                      \
            return Result<LogicalPayload>::failure(                          \
                std::move(step_result).error());                             \
        }                                                                    \
    } while (false)

    if (scenario == "graph_empty") {
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(0)));
        return builder.freeze();
    }

    if (scenario == "graph_all_values") {
        const std::uint32_t node = component_index(spec, "Node");
        const std::uint32_t asset = component_index(spec, "Asset");
        auto node_object = builder.declare_object(node);
        auto asset_object = builder.declare_object(asset);
        if (!node_object.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(node_object).error());
        }
        if (!asset_object.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(asset_object).error());
        }
        const std::array<std::uint16_t, 3> wide{{
            UINT16_C(0x0041), UINT16_C(0xd83d), UINT16_C(0xde00)}};
        const std::array<std::uint8_t, 3> opaque{{
            UINT8_C(0x00), UINT8_C(0xff), UINT8_C(0x7e)}};
        FASTDB_GRAPH_STEP(builder.begin_object_fill(node_object.value()));
        FASTDB_GRAPH_STEP(builder.push_bool(UINT8_C(1)));
        FASTDB_GRAPH_STEP(builder.push_u8(UINT8_C(0x12)));
        FASTDB_GRAPH_STEP(builder.push_u16(UINT16_C(0x3456)));
        FASTDB_GRAPH_STEP(builder.push_u32(UINT32_C(0x789abcde)));
        FASTDB_GRAPH_STEP(builder.push_i32(INT32_C(-1234567)));
        FASTDB_GRAPH_STEP(
            builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
        FASTDB_GRAPH_STEP(builder.push_u16n_bits(UINT64_C(0)));
        FASTDB_GRAPH_STEP(builder.push_f32_bits(UINT32_C(0x7fa12345)));
        FASTDB_GRAPH_STEP(
            builder.push_f64_bits(UINT64_C(0xfff8000000001234)));
        FASTDB_GRAPH_STEP(builder.push_str("same"));
        FASTDB_GRAPH_STEP(builder.push_wstr(wide.data(), wide.size()));
        FASTDB_GRAPH_STEP(builder.push_bytes(opaque.data(), opaque.size()));
        FASTDB_GRAPH_STEP(builder.begin_component());
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.push_u16(UINT16_C(0xbeef)));
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_GRAPH_STEP(
            builder.push_f32_bits(UINT32_C(0x80000000)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(
            builder.push_f32_bits(UINT32_C(0xff800001)));
        FASTDB_GRAPH_STEP(builder.push_ref(node_object.value()));
        FASTDB_GRAPH_STEP(builder.push_ref(asset_object.value()));

        FASTDB_GRAPH_STEP(builder.begin_object_fill(asset_object.value()));
        FASTDB_GRAPH_STEP(builder.push_str("same"));
        FASTDB_GRAPH_STEP(builder.push_ref(node_object.value()));

        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_GRAPH_STEP(builder.push_object(node_object.value()));
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(1)));
        FASTDB_GRAPH_STEP(builder.push_object(asset_object.value()));
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(2), UINT64_C(2)));
        FASTDB_GRAPH_STEP(builder.push_ref(node_object.value()));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(3), UINT64_C(3)));
        FASTDB_GRAPH_STEP(builder.push_u8n_bits(UINT64_C(0)));
        FASTDB_GRAPH_STEP(
            builder.push_u8n_bits(UINT64_C(0x3fe0000000000000)));
        FASTDB_GRAPH_STEP(
            builder.push_u8n_bits(UINT64_C(0x3ff0000000000000)));
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(4), UINT64_C(3)));
        FASTDB_GRAPH_STEP(
            builder.push_u16n_bits(UINT64_C(0xbff0000000000000)));
        FASTDB_GRAPH_STEP(builder.push_u16n_bits(UINT64_C(0)));
        FASTDB_GRAPH_STEP(
            builder.push_u16n_bits(UINT64_C(0x3ff0000000000000)));
        return builder.freeze();
    }

    if (scenario == "graph_nested_lists") {
        const std::uint32_t node = component_index(spec, "Node");
        auto object = builder.declare_object(node);
        if (!object.has_value()) {
            return Result<LogicalPayload>::failure(std::move(object).error());
        }
        FASTDB_GRAPH_STEP(builder.begin_object_fill(object.value()));
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(3)));
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_GRAPH_STEP(builder.push_u16(UINT16_C(1)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_GRAPH_STEP(builder.begin_component());
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_GRAPH_STEP(builder.push_u8(UINT8_C(7)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_GRAPH_STEP(builder.push_object(object.value()));
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(3)));
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(2)));
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(1)));
        FASTDB_GRAPH_STEP(builder.push_u16(UINT16_C(9)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(0)));
        return builder.freeze();
    }

    if (scenario == "graph_disconnected_roots") {
        const std::uint32_t node = component_index(spec, "Node");
        auto first = builder.declare_object(node);
        auto second = builder.declare_object(node);
        if (!first.has_value()) {
            return Result<LogicalPayload>::failure(std::move(first).error());
        }
        if (!second.has_value()) {
            return Result<LogicalPayload>::failure(std::move(second).error());
        }
        FASTDB_GRAPH_STEP(builder.begin_object_fill(first.value()));
        FASTDB_GRAPH_STEP(builder.push_u32(UINT32_C(11)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_object_fill(second.value()));
        FASTDB_GRAPH_STEP(builder.push_u32(UINT32_C(22)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(2)));
        FASTDB_GRAPH_STEP(builder.push_object(first.value()));
        FASTDB_GRAPH_STEP(builder.push_object(second.value()));
        return builder.freeze();
    }

    if (scenario == "graph_null_empty") {
        const std::uint32_t node = component_index(spec, "Node");
        auto object = builder.declare_object(node);
        if (!object.has_value()) {
            return Result<LogicalPayload>::failure(std::move(object).error());
        }
        FASTDB_GRAPH_STEP(builder.begin_object_fill(object.value()));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.push_wstr(nullptr, UINT64_C(0)));
        FASTDB_GRAPH_STEP(builder.push_bytes(nullptr, UINT64_C(0)));
        FASTDB_GRAPH_STEP(builder.begin_list(UINT64_C(0)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(2)));
        FASTDB_GRAPH_STEP(builder.push_object(object.value()));
        FASTDB_GRAPH_STEP(builder.push_null());
        return builder.freeze();
    }

    const std::uint32_t node = component_index(spec, "Node");
    if (scenario == "graph_single_root") {
        auto object = builder.declare_object(node);
        if (!object.has_value()) {
            return Result<LogicalPayload>::failure(std::move(object).error());
        }
        FASTDB_GRAPH_STEP(builder.begin_object_fill(object.value()));
        FASTDB_GRAPH_STEP(builder.push_u16(UINT16_C(0x1234)));
        FASTDB_GRAPH_STEP(builder.push_null());
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
        FASTDB_GRAPH_STEP(builder.push_object(object.value()));
        FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(1)));
        FASTDB_GRAPH_STEP(builder.push_null());
        return builder.freeze();
    }

    if (scenario != "graph_shared_cycle") {
        return Result<LogicalPayload>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INVALID_ARGUMENT,
                JsonPointer{}.append("scenario"),
                "Unknown graph binary scenario",
                fastdb::payload::json::JsonValue::object({})));
    }

    ObjectHandle first = UINT64_C(0);
    ObjectHandle second = UINT64_C(0);
    if (reverse_declarations) {
        auto second_result = builder.declare_object(node);
        auto first_result = builder.declare_object(node);
        if (!second_result.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(second_result).error());
        }
        if (!first_result.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(first_result).error());
        }
        second = second_result.value();
        first = first_result.value();
    } else {
        auto first_result = builder.declare_object(node);
        auto second_result = builder.declare_object(node);
        if (!first_result.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(first_result).error());
        }
        if (!second_result.has_value()) {
            return Result<LogicalPayload>::failure(
                std::move(second_result).error());
        }
        first = first_result.value();
        second = second_result.value();
    }

    const auto fill = [&](ObjectHandle object,
                          std::uint32_t value,
                          ObjectHandle self,
                          ObjectHandle shared,
                          std::uint8_t flag,
                          std::uint16_t code) -> Result<void> {
        auto started = builder.begin_object_fill(object);
        if (!started.has_value()) {
            return started;
        }
        auto pushed = builder.push_u32(value);
        if (!pushed.has_value()) {
            return pushed;
        }
        pushed = builder.push_ref(self);
        if (!pushed.has_value()) {
            return pushed;
        }
        pushed = builder.push_ref(shared);
        if (!pushed.has_value()) {
            return pushed;
        }
        pushed = builder.begin_component();
        if (!pushed.has_value()) {
            return pushed;
        }
        pushed = builder.push_bool(flag);
        if (!pushed.has_value()) {
            return pushed;
        }
        return builder.push_u16(code);
    };

    Result<void> filled = Result<void>::success();
    if (reverse_fill) {
        filled = fill(second, UINT32_C(200), second, second, UINT8_C(0),
                      UINT16_C(0xabcd));
        if (filled.has_value()) {
            filled = fill(first, UINT32_C(100), first, second, UINT8_C(1),
                          UINT16_C(0x1234));
        }
    } else {
        filled = fill(first, UINT32_C(100), first, second, UINT8_C(1),
                      UINT16_C(0x1234));
        if (filled.has_value()) {
            filled = fill(second, UINT32_C(200), second, second, UINT8_C(0),
                          UINT16_C(0xabcd));
        }
    }
    if (!filled.has_value()) {
        return Result<LogicalPayload>::failure(std::move(filled).error());
    }
    FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(0), UINT64_C(1)));
    FASTDB_GRAPH_STEP(builder.push_object(first));
    FASTDB_GRAPH_STEP(builder.begin_entry(UINT32_C(1), UINT64_C(1)));
    FASTDB_GRAPH_STEP(builder.push_object(second));
#undef FASTDB_GRAPH_STEP
    return builder.freeze();
}

struct EncodedGraph final {
    CompiledSpec spec;
    GraphLayout layout;
    std::vector<std::uint8_t> bytes;
};

Result<EncodedGraph> encode_graph_case(const BinaryGoldenCase& item,
                                       bool reverse_fill = false,
                                       bool reverse_declarations = false) {
    auto compiled = CompiledSpec::compile(item.success.source);
    if (!compiled.has_value()) {
        return Result<EncodedGraph>::failure(std::move(compiled).error());
    }
    CompiledSpec spec = compiled.value();
    auto values = build_graph_scenario(compiled.value(),
                                       item.success.scenario,
                                       reverse_fill,
                                       reverse_declarations);
    if (!values.has_value()) {
        return Result<EncodedGraph>::failure(std::move(values).error());
    }
    auto runtime = RuntimeSchema::compile(spec);
    if (!runtime.has_value()) {
        return Result<EncodedGraph>::failure(std::move(runtime).error());
    }
    auto planned = GraphLayout::plan(runtime.value(), values.value());
    if (!planned.has_value()) {
        return Result<EncodedGraph>::failure(std::move(planned).error());
    }
    VectorSink sink(planned.value().total_length());
    auto encoded = fastdb::payload::build::encode_graph(
        planned.value(), values.value(), sink);
    if (!encoded.has_value()) {
        return Result<EncodedGraph>::failure(std::move(encoded).error());
    }
    if (sink.next_offset() != planned.value().total_length()) {
        return Result<EncodedGraph>::failure(
            fastdb::payload::error::Error::from_details(
                FDB_PAYLOAD_E_INTERNAL, JsonPointer{}.append("sink"),
                "Graph encoder did not consume sink",
                fastdb::payload::json::JsonValue::object({})));
    }
    return Result<EncodedGraph>::success(EncodedGraph{
        std::move(spec), std::move(planned).value(), sink.bytes()});
}

const BinaryGoldenCase* find_case(const std::vector<BinaryGoldenCase>& cases,
                                  std::string_view name) {
    const auto found = std::find_if(
        cases.begin(), cases.end(), [name](const BinaryGoldenCase& item) {
            return item.name == name;
        });
    return found == cases.end() ? nullptr : &*found;
}

std::uint64_t load_u64(const std::vector<std::uint8_t>& bytes,
                       std::uint64_t offset) {
    auto value = fastdb::payload::layout::load_u64_le(
        bytes.data(), bytes.size(), offset, JsonPointer{});
    require(value.has_value());
    return value.value();
}

std::uint32_t load_u32(const std::vector<std::uint8_t>& bytes,
                       std::uint64_t offset) {
    auto value = fastdb::payload::layout::load_u32_le(
        bytes.data(), bytes.size(), offset, JsonPointer{});
    require(value.has_value());
    return value.value();
}

void store_u16(std::vector<std::uint8_t>& bytes,
               std::uint64_t offset,
               std::uint16_t value) {
    bytes[static_cast<std::size_t>(offset)] =
        static_cast<std::uint8_t>(value & UINT16_C(0xff));
    bytes[static_cast<std::size_t>(offset + UINT64_C(1))] =
        static_cast<std::uint8_t>(value >> UINT16_C(8));
}

void store_u32(std::vector<std::uint8_t>& bytes,
               std::uint64_t offset,
               std::uint32_t value) {
    for (std::uint32_t byte = UINT32_C(0); byte < UINT32_C(4); ++byte) {
        bytes[static_cast<std::size_t>(offset + byte)] =
            static_cast<std::uint8_t>(value >> (byte * UINT32_C(8)));
    }
}

void store_u64(std::vector<std::uint8_t>& bytes,
               std::uint64_t offset,
               std::uint64_t value) {
    for (std::uint32_t byte = UINT32_C(0); byte < UINT32_C(8); ++byte) {
        bytes[static_cast<std::size_t>(offset + byte)] =
            static_cast<std::uint8_t>(value >> (byte * UINT32_C(8)));
    }
}

bool stable_graph_rejection(const CompiledSpec& spec,
                            const std::vector<std::uint8_t>& bytes,
                            std::uint32_t status,
                            std::string_view reason = {}) {
    auto first = fastdb::payload::view::open_graph(
        spec, bytes.data(), bytes.size());
    auto second = fastdb::payload::view::open_graph(
        spec, bytes.data(), bytes.size());
    if (first.has_value() || second.has_value() ||
        first.error().code() != status ||
        second.error().code() != first.error().code() ||
        second.error().symbol() != first.error().symbol() ||
        second.error().path() != first.error().path() ||
        second.error().message() != first.error().message() ||
        second.error().details_json() != first.error().details_json()) {
        return false;
    }
    return reason.empty() ||
           first.error().details_json().find(reason) != std::string_view::npos;
}

const fastdb::payload::layout::RegionDescriptor& require_region(
    const GraphLayout& layout,
    std::size_t index,
    RegionKind kind,
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t count,
    std::uint32_t stride,
    std::uint32_t alignment) {
    if (index >= layout.regions().size()) {
        std::abort();
    }
    const auto& region = layout.regions()[index];
    if (region.kind != kind || region.data_offset != offset ||
        region.byte_length != length || region.element_count != count ||
        region.stride != stride || region.alignment != alignment) {
        std::abort();
    }
    return region;
}

int test_region_rule_and_public_boundary() {
    static_assert(FDB_PAYLOAD_REGION_OBJECT_VALUES == UINT32_C(8));
    static_assert(static_cast<std::uint32_t>(RegionKind::object_values) ==
                  UINT32_C(8));
    const auto rule =
        fastdb::payload::layout::region_rule(RegionKind::object_values);
    require(rule.kind_value == UINT32_C(8));
    require(rule.count_unit ==
            fastdb::payload::layout::RegionCountUnit::objects);
    require(rule.owner_rule ==
            fastdb::payload::layout::RegionOwnerRule::component_index);
    require(rule.type_rule ==
            fastdb::payload::layout::RegionTypeRule::sentinel);
    require(rule.stride_rule ==
            fastdb::payload::layout::RegionStrideRule::component_stride);
    require(rule.alignment_rule ==
            fastdb::payload::layout::RegionAlignmentRule::component_alignment);
    require(!rule.is_validity && !rule.is_pool);
    return EXIT_SUCCESS;
}

int test_complete_graph_values_slice() {
    constexpr std::string_view source = R"JSON({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one",
     "type":{"kind":"component","id":"Node"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"label","type":{"kind":"str"}},
      {"id":"values","type":{"kind":"list","items":{
        "kind":"u16","nullable":true}}},
      {"id":"next","type":{"kind":"ref","target":"Node",
        "nullable":true}}
    ]}
  ]
})JSON";
    auto compiled = CompiledSpec::compile(source);
    require(compiled.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    PayloadBuilder builder = std::move(created).value();
    auto declared = builder.declare_object(UINT32_C(0));
    require(declared.has_value());
    require(builder.begin_object_fill(declared.value()).has_value());
    require(builder.push_str("node").has_value());
    require(builder.begin_list(UINT64_C(2)).has_value());
    require(builder.push_u16(UINT16_C(7)).has_value());
    require(builder.push_null().has_value());
    require(builder.push_null().has_value());
    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(builder.push_object(declared.value()).has_value());
    auto values = builder.freeze();
    require(values.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto layout = GraphLayout::plan(runtime.value(), values.value());
    require(layout.has_value());
    VectorSink sink(layout.value().total_length());
    auto encoded = fastdb::payload::build::encode_graph(
        layout.value(), values.value(), sink);
    require(encoded.has_value());
    auto opened = fastdb::payload::view::open_graph(
        compiled.value(), sink.bytes().data(), sink.bytes().size());
    require(opened.has_value());
    auto sequence = opened.value().entry_sequence(
        sink.bytes().data(), sink.bytes().size(), UINT32_C(0));
    require(sequence.has_value());
    auto root = opened.value().entry_value(
        sink.bytes().data(), sink.bytes().size(), sequence.value(),
        UINT64_C(0));
    require(root.has_value());
    const auto* object =
        std::get_if<fastdb::payload::view::IdentityObjectCursor>(
            &root.value());
    require(object != nullptr && object->component_index == UINT32_C(0) &&
            object->object_id == UINT64_C(0) && object->present);
    auto identity = opened.value().graph_identity(root.value());
    require(identity.has_value());
    require(identity.value().component_index == UINT32_C(0));
    require(identity.value().object_id == UINT64_C(0));
    auto label = opened.value().component_field(
        sink.bytes().data(), sink.bytes().size(), root.value(), UINT32_C(0));
    auto list = opened.value().component_field(
        sink.bytes().data(), sink.bytes().size(), root.value(), UINT32_C(1));
    auto reference = opened.value().component_field(
        sink.bytes().data(), sink.bytes().size(), root.value(), UINT32_C(2));
    require(label.has_value() && list.has_value() && reference.has_value());
    require(fastdb::payload::view::value_cursor_kind(label.value()) ==
            fastdb::payload::spec::TypeKind::str);
    require(fastdb::payload::view::value_cursor_kind(list.value()) ==
            fastdb::payload::spec::TypeKind::list);
    const auto* ref =
        std::get_if<fastdb::payload::view::RefCursor>(&reference.value());
    require(ref != nullptr && !ref->present &&
            ref->target_component_index == UINT32_C(0));
    auto list_count = opened.value().list_length(
        sink.bytes().data(), sink.bytes().size(), list.value());
    require(list_count.has_value() && list_count.value() == UINT64_C(2));
    auto first = opened.value().list_item(
        sink.bytes().data(), sink.bytes().size(), list.value(), UINT64_C(0));
    auto second = opened.value().list_item(
        sink.bytes().data(), sink.bytes().size(), list.value(), UINT64_C(1));
    require(first.has_value() && second.has_value());
    require(fastdb::payload::view::value_cursor_present(first.value()));
    require(!fastdb::payload::view::value_cursor_present(second.value()));
    return EXIT_SUCCESS;
}

int test_ordered_graph_goldens_and_strict_open() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    require(corpus.size() == 7U);
    const std::vector<std::string_view> names{
        "graph-all-values",
        "graph-nested-lists",
        "graph-disconnected-roots",
        "graph-null-empty",
        "graph-empty",
        "graph-single-root",
        "graph-shared-cycle",
    };
    for (const std::string_view name : names) {
        const BinaryGoldenCase* item = find_case(corpus, name);
        require(item != nullptr);
        require(!item->success.layout_relative_path.empty());
        require(!item->success.layout_receipt.empty());
        auto encoded = encode_graph_case(*item);
        auto repeated_encoded = encode_graph_case(*item);
        const std::string encode_diagnostic =
            encoded.has_value()
                ? std::string{name}
                : std::string{name} + ": " +
                      std::string{encoded.error().details_json()};
        require(encoded.has_value() && repeated_encoded.has_value(),
                encode_diagnostic);
        require(encoded.value().bytes == repeated_encoded.value().bytes);
        const auto golden = decode_hex(item->success.binary_hex);
        require(encoded.value().bytes == golden);
        require(fastdb::payload::identity::sha256_lower_hex(
                    fastdb::payload::identity::sha256(
                        golden.data(), golden.size())) == item->success.sha256);
        auto opened = fastdb::payload::view::open_graph(
            encoded.value().spec, golden.data(), golden.size());
        auto repeated = fastdb::payload::view::open_graph(
            encoded.value().spec, golden.data(), golden.size());
        require(opened.has_value() && repeated.has_value());
        require(opened.value().total_length() == golden.size());
        require(opened.value().total_length() ==
                repeated.value().total_length());
        require(opened.value().root_value_count() ==
                encoded.value().layout.root_value_count());
        require(opened.value().graph_object_count() ==
                encoded.value().layout.graph_object_count());
        require(opened.value().validation_work() ==
                encoded.value().layout.validation_work());
        require(opened.value().region_count() ==
                encoded.value().layout.region_count());
        require(opened.value().entry_count() ==
                encoded.value().layout.entries().size());
    }
    return EXIT_SUCCESS;
}

int test_selected_graph_invalid_golden_corpus() {
    const auto corpus =
        fastdb::test::payload::load_graph_binary_open_golden_corpus(
            FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    std::size_t checked = 0U;
    for (const auto& item : corpus) {
        if (item.name.rfind("invalid-graph-", 0U) != 0U) {
            continue;
        }
        const auto* invalid =
            std::get_if<fastdb::test::payload::BinaryGoldenInvalid>(
                &item.expected);
        require(invalid != nullptr, item.name);
        auto compiled = CompiledSpec::compile(invalid->source);
        require(compiled.has_value(), item.name);
        const auto bytes = decode_hex(invalid->binary_hex);
        auto first = fastdb::payload::view::open_graph(
            compiled.value(), bytes.data(), bytes.size());
        auto second = fastdb::payload::view::open_graph(
            compiled.value(), bytes.data(), bytes.size());
        require(!first.has_value() && !second.has_value(), item.name);
        const auto& expected = invalid->expected;
        require(first.error().code() == expected.status, item.name);
        require(first.error().symbol() == expected.symbol, item.name);
        require(first.error().path() == expected.path, item.name);
        require(first.error().message() == expected.message, item.name);
        require(first.error().details_json() == expected.details_json,
                item.name);
        require(second.error().code() == first.error().code(), item.name);
        require(second.error().symbol() == first.error().symbol(), item.name);
        require(second.error().path() == first.error().path(), item.name);
        require(second.error().message() == first.error().message(), item.name);
        require(second.error().details_json() == first.error().details_json(),
                item.name);
        ++checked;
    }
    require(checked == 7U);
    return EXIT_SUCCESS;
}

int test_strict_open_rejects_malformed_graph_bytes_and_limits() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* single = find_case(corpus, "graph-single-root");
    const BinaryGoldenCase* cycle = find_case(corpus, "graph-shared-cycle");
    const BinaryGoldenCase* empty = find_case(corpus, "graph-empty");
    require(single != nullptr && cycle != nullptr && empty != nullptr);
    auto single_encoded = encode_graph_case(*single);
    auto cycle_encoded = encode_graph_case(*cycle);
    auto empty_encoded = encode_graph_case(*empty);
    require(single_encoded.has_value() && cycle_encoded.has_value() &&
            empty_encoded.has_value());

    const auto open_single = [&](const std::vector<std::uint8_t>& bytes) {
        return fastdb::payload::view::open_graph(
            single_encoded.value().spec, bytes.data(), bytes.size());
    };

    auto wrong_profile = single_encoded.value().bytes;
    wrong_profile[static_cast<std::size_t>(
        fastdb::payload::layout::header_profile_offset)] = UINT8_C(1);
    auto rejected = open_single(wrong_profile);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto wrong_object_region = single_encoded.value().bytes;
    constexpr std::uint64_t object_region_descriptor =
        fastdb::payload::layout::header_size +
        UINT64_C(4) * fastdb::payload::layout::region_descriptor_size;
    wrong_object_region[static_cast<std::size_t>(
        object_region_descriptor +
        fastdb::payload::layout::region_runtime_type_id_offset)] = UINT8_C(0);
    rejected = open_single(wrong_object_region);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto invalid_root = single_encoded.value().bytes;
    invalid_root[496U] = UINT8_C(1);
    rejected = open_single(invalid_root);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_INVALID_REFERENCE);
    require(rejected.error().details_json() ==
            R"({"object_id":"1","reason":"root_object_id_out_of_range"})");

    auto nonzero_entry_padding = single_encoded.value().bytes;
    nonzero_entry_padding[489U] = UINT8_C(1);
    rejected = open_single(nonzero_entry_padding);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto nonzero_validity_tail = single_encoded.value().bytes;
    nonzero_validity_tail[520U] = UINT8_C(0x80);
    rejected = open_single(nonzero_validity_tail);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto nonzero_object_padding = single_encoded.value().bytes;
    nonzero_object_padding[521U] = UINT8_C(1);
    rejected = open_single(nonzero_object_padding);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto object_depth = fastdb::payload::view::default_open_options();
    object_depth.max_nesting_depth = UINT64_C(0);
    rejected = fastdb::payload::view::open_graph(
        single_encoded.value().spec, single_encoded.value().bytes.data(),
        single_encoded.value().bytes.size(), object_depth);
    require(!rejected.has_value());
    require(rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    object_depth.max_nesting_depth = UINT64_C(1);
    require(fastdb::payload::view::open_graph(
                single_encoded.value().spec,
                single_encoded.value().bytes.data(),
                single_encoded.value().bytes.size(), object_depth)
                .has_value());

    auto missing_empty_pool = empty_encoded.value().bytes;
    missing_empty_pool[static_cast<std::size_t>(
        fastdb::payload::layout::header_region_count_offset)] = UINT8_C(1);
    auto empty_rejected = fastdb::payload::view::open_graph(
        empty_encoded.value().spec, missing_empty_pool.data(),
        missing_empty_pool.size());
    require(!empty_rejected.has_value());
    require(empty_rejected.error().code() ==
            FDB_PAYLOAD_E_NON_CANONICAL_BINARY);

    auto limited = fastdb::payload::view::default_open_options();
    limited.max_graph_objects = UINT64_C(1);
    auto cycle_rejected = fastdb::payload::view::open_graph(
        cycle_encoded.value().spec, cycle_encoded.value().bytes.data(),
        cycle_encoded.value().bytes.size(), limited);
    require(!cycle_rejected.has_value());
    require(cycle_rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);

    auto invalid_reference = cycle_encoded.value().bytes;
    invalid_reference[408U] = UINT8_C(2);
    cycle_rejected = fastdb::payload::view::open_graph(
        cycle_encoded.value().spec, invalid_reference.data(),
        invalid_reference.size());
    require(!cycle_rejected.has_value());
    require(cycle_rejected.error().code() == FDB_PAYLOAD_E_INVALID_REFERENCE);
    require(cycle_rejected.error().details_json() ==
            R"({"object_id":"2","reason":"reference_object_id_out_of_range"})");

    auto shallow = fastdb::payload::view::default_open_options();
    shallow.max_nesting_depth = UINT64_C(1);
    cycle_rejected = fastdb::payload::view::open_graph(
        cycle_encoded.value().spec, cycle_encoded.value().bytes.data(),
        cycle_encoded.value().bytes.size(), shallow);
    require(!cycle_rejected.has_value());
    require(cycle_rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    shallow.max_nesting_depth = UINT64_C(2);
    require(fastdb::payload::view::open_graph(
                cycle_encoded.value().spec,
                cycle_encoded.value().bytes.data(),
                cycle_encoded.value().bytes.size(), shallow)
                .has_value());

    auto exact_work = fastdb::payload::view::default_open_options();
    exact_work.max_validation_work =
        cycle_encoded.value().layout.validation_work();
    require(fastdb::payload::view::open_graph(
                cycle_encoded.value().spec,
                cycle_encoded.value().bytes.data(),
                cycle_encoded.value().bytes.size(), exact_work)
                .has_value());
    --exact_work.max_validation_work;
    cycle_rejected = fastdb::payload::view::open_graph(
        cycle_encoded.value().spec, cycle_encoded.value().bytes.data(),
        cycle_encoded.value().bytes.size(), exact_work);
    require(!cycle_rejected.has_value());
    require(cycle_rejected.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT);
    return EXIT_SUCCESS;
}

int test_complete_graph_malformed_field_matrix() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* disconnected =
        find_case(corpus, "graph-disconnected-roots");
    const BinaryGoldenCase* all_values =
        find_case(corpus, "graph-all-values");
    const BinaryGoldenCase* null_empty =
        find_case(corpus, "graph-null-empty");
    require(disconnected != nullptr && all_values != nullptr &&
            null_empty != nullptr);
    auto disconnected_encoded = encode_graph_case(*disconnected);
    auto all_values_encoded = encode_graph_case(*all_values);
    auto null_empty_encoded = encode_graph_case(*null_empty);
    require(disconnected_encoded.has_value() &&
            all_values_encoded.has_value() && null_empty_encoded.has_value());

    constexpr std::uint64_t object_descriptor =
        fastdb::payload::layout::header_size +
        fastdb::payload::layout::region_descriptor_size;
    const auto reject_disconnected = [&](const auto& bytes,
                                         std::uint32_t status,
                                         std::string_view reason) {
        return stable_graph_rejection(disconnected_encoded.value().spec,
                                      bytes, status, reason);
    };

    auto missing_region = disconnected_encoded.value().bytes;
    store_u32(missing_region,
              fastdb::payload::layout::header_region_count_offset,
              UINT32_C(1));
    require(reject_disconnected(missing_region,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_count"));

    auto extra_region = disconnected_encoded.value().bytes;
    store_u32(extra_region,
              fastdb::payload::layout::header_region_count_offset,
              UINT32_C(3));
    require(reject_disconnected(extra_region,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_count"));

    auto unknown_kind = disconnected_encoded.value().bytes;
    store_u32(unknown_kind,
              object_descriptor +
                  fastdb::payload::layout::region_kind_offset,
              UINT32_C(99));
    require(reject_disconnected(unknown_kind,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_kind"));

    auto wrong_type = disconnected_encoded.value().bytes;
    store_u32(wrong_type,
              object_descriptor +
                  fastdb::payload::layout::region_runtime_type_id_offset,
              UINT32_C(0));
    require(reject_disconnected(wrong_type,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_runtime_type_id"));

    auto wrong_count = disconnected_encoded.value().bytes;
    store_u64(wrong_count,
              object_descriptor +
                  fastdb::payload::layout::region_element_count_offset,
              UINT64_C(3));
    require(reject_disconnected(wrong_count,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_byte_length"));

    auto wrong_alignment = disconnected_encoded.value().bytes;
    store_u32(wrong_alignment,
              object_descriptor +
                  fastdb::payload::layout::region_alignment_offset,
              UINT32_C(4));
    require(reject_disconnected(wrong_alignment,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_alignment"));

    auto wrong_length = disconnected_encoded.value().bytes;
    store_u64(wrong_length,
              object_descriptor +
                  fastdb::payload::layout::region_byte_length_offset,
              UINT64_C(31));
    require(reject_disconnected(wrong_length,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_byte_length"));

    auto wrong_offset = disconnected_encoded.value().bytes;
    store_u64(wrong_offset,
              object_descriptor +
                  fastdb::payload::layout::region_data_offset_offset,
              UINT64_C(304));
    require(reject_disconnected(wrong_offset,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_data_offset"));

    auto wrong_flags = disconnected_encoded.value().bytes;
    store_u32(wrong_flags,
              object_descriptor +
                  fastdb::payload::layout::region_flags_offset,
              UINT32_C(1));
    require(reject_disconnected(wrong_flags,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_flags"));

    auto wrong_reserved = disconnected_encoded.value().bytes;
    store_u64(wrong_reserved,
              object_descriptor +
                  fastdb::payload::layout::region_reserved_offset,
              UINT64_C(1));
    require(reject_disconnected(wrong_reserved,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "region_reserved"));

    auto overflowing_count = disconnected_encoded.value().bytes;
    store_u64(overflowing_count,
              object_descriptor +
                  fastdb::payload::layout::region_element_count_offset,
              UINT64_MAX);
    require(reject_disconnected(overflowing_count,
                                FDB_PAYLOAD_E_LENGTH_OVERFLOW,
                                "wire_multiply_overflow"));

    auto object_padding = disconnected_encoded.value().bytes;
    object_padding[297U] = UINT8_C(1);
    require(reject_disconnected(object_padding,
                                FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                "nonzero_component_padding"));

    auto validity_tail = null_empty_encoded.value().bytes;
    validity_tail[616U] = UINT8_C(0x81);
    require(stable_graph_rejection(null_empty_encoded.value().spec,
                                   validity_tail,
                                   FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                                   "nonzero_validity_tail"));

    const GraphLayout& all_layout = all_values_encoded.value().layout;
    const std::uint32_t node =
        component_index(all_values_encoded.value().spec, "Node");
    const auto aggregate = std::find_if(
        all_layout.object_aggregates().begin(),
        all_layout.object_aggregates().end(),
        [node](const auto& value) { return value.component_index == node; });
    require(aggregate != all_layout.object_aggregates().end());
    const auto* node_layout = all_layout.runtime_schema().component(node);
    require(node_layout != nullptr && node_layout->fields.size() == 16U);
    const std::uint64_t node_base =
        all_layout.regions()[aggregate->region_index].data_offset;
    const auto reject_all = [&](const auto& bytes,
                                std::uint32_t status,
                                std::string_view reason) {
        return stable_graph_rejection(all_values_encoded.value().spec, bytes,
                                      status, reason);
    };

    auto invalid_boolean = all_values_encoded.value().bytes;
    invalid_boolean[static_cast<std::size_t>(
        node_base + node_layout->fields[0].offset)] = UINT8_C(2);
    require(reject_all(invalid_boolean, FDB_PAYLOAD_E_INVALID_BINARY_VALUE,
                       "invalid_boolean_byte"));

    auto invalid_f32_nan = all_values_encoded.value().bytes;
    store_u32(invalid_f32_nan,
              node_base + node_layout->fields[7].offset,
              UINT32_C(0x7fc00001));
    require(reject_all(invalid_f32_nan, FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                       "noncanonical_f32_nan"));

    auto invalid_f64_nan = all_values_encoded.value().bytes;
    store_u64(invalid_f64_nan,
              node_base + node_layout->fields[8].offset,
              UINT64_C(0x7ff8000000000001));
    require(reject_all(invalid_f64_nan, FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                       "noncanonical_f64_nan"));

    auto invalid_list_partition = all_values_encoded.value().bytes;
    store_u64(invalid_list_partition,
              node_base + node_layout->fields[13].offset + UINT64_C(8),
              UINT64_C(4));
    require(reject_all(invalid_list_partition, FDB_PAYLOAD_E_OUT_OF_BOUNDS,
                       "wire_range_out_of_bounds"));

    const auto utf8 = std::find_if(
        all_layout.regions().begin(), all_layout.regions().end(),
        [](const auto& region) {
            return region.kind == RegionKind::utf8_pool;
        });
    const auto utf16 = std::find_if(
        all_layout.regions().begin(), all_layout.regions().end(),
        [](const auto& region) {
            return region.kind == RegionKind::utf16_pool;
        });
    require(utf8 != all_layout.regions().end() &&
            utf16 != all_layout.regions().end());

    auto invalid_utf8 = all_values_encoded.value().bytes;
    invalid_utf8[static_cast<std::size_t>(utf8->data_offset)] = UINT8_C(0xff);
    require(reject_all(invalid_utf8, FDB_PAYLOAD_E_INVALID_TEXT_ENCODING,
                       "invalid_sequence"));

    auto invalid_utf16 = all_values_encoded.value().bytes;
    store_u16(invalid_utf16,
              utf16->data_offset + UINT64_C(4), UINT16_C(0x0041));
    require(reject_all(invalid_utf16, FDB_PAYLOAD_E_INVALID_TEXT_ENCODING,
                       "unpaired_surrogate"));

    const auto list_validity = std::find_if(
        all_layout.regions().begin(), all_layout.regions().end(),
        [](const auto& region) {
            return region.kind == RegionKind::list_validity;
        });
    const auto list_items = std::find_if(
        all_layout.regions().begin(), all_layout.regions().end(),
        [](const auto& region) {
            return region.kind == RegionKind::list_items;
        });
    require(list_validity != all_layout.regions().end() &&
            list_items != all_layout.regions().end());

    auto invalid_list_tail = all_values_encoded.value().bytes;
    invalid_list_tail[static_cast<std::size_t>(list_validity->data_offset)] =
        UINT8_C(0x85);
    require(reject_all(invalid_list_tail,
                       FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                       "nonzero_validity_tail"));

    auto invalid_list_nan = all_values_encoded.value().bytes;
    store_u32(invalid_list_nan,
              list_items->data_offset + UINT64_C(8),
              UINT32_C(0x7fc00001));
    require(reject_all(invalid_list_nan,
                       FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                       "noncanonical_f32_nan"));

    auto final_padding = all_values_encoded.value().bytes;
    final_padding.back() = UINT8_C(1);
    require(reject_all(final_padding, FDB_PAYLOAD_E_NON_CANONICAL_BINARY,
                       "nonzero_final_padding"));
    return EXIT_SUCCESS;
}

int test_graph_resource_boundaries_and_empty_target_pool() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* all_values =
        find_case(corpus, "graph-all-values");
    require(all_values != nullptr);
    auto encoded = encode_graph_case(*all_values);
    require(encoded.has_value());

    auto exact = fastdb::payload::view::default_open_options();
    exact.max_graph_objects = UINT64_C(2);
    exact.max_list_elements = UINT64_C(3);
    exact.max_string_bytes = UINT64_C(14);
    require(fastdb::payload::view::open_graph(
                encoded.value().spec, encoded.value().bytes.data(),
                encoded.value().bytes.size(), exact)
                .has_value());

    auto object_short = exact;
    object_short.max_graph_objects = UINT64_C(1);
    auto object_result = fastdb::payload::view::open_graph(
        encoded.value().spec, encoded.value().bytes.data(),
        encoded.value().bytes.size(), object_short);
    require(!object_result.has_value() &&
            object_result.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT &&
            object_result.error().details_json().find("graph_objects") !=
                std::string_view::npos);

    auto list_short = exact;
    list_short.max_list_elements = UINT64_C(2);
    auto list_result = fastdb::payload::view::open_graph(
        encoded.value().spec, encoded.value().bytes.data(),
        encoded.value().bytes.size(), list_short);
    require(!list_result.has_value() &&
            list_result.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT &&
            list_result.error().details_json().find("list_elements") !=
                std::string_view::npos);

    auto string_short = exact;
    string_short.max_string_bytes = UINT64_C(13);
    auto string_result = fastdb::payload::view::open_graph(
        encoded.value().spec, encoded.value().bytes.data(),
        encoded.value().bytes.size(), string_short);
    require(!string_result.has_value() &&
            string_result.error().code() == FDB_PAYLOAD_E_RESOURCE_LIMIT &&
            string_result.error().details_json().find("string_bytes") !=
                std::string_view::npos);

    constexpr std::string_view source = R"JSON({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one",
     "type":{"kind":"component","id":"Node"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"target","type":{"kind":"ref","target":"Empty",
       "nullable":true}}
    ]},
    {"id":"Empty","kind":"record","fields":[]}
  ]
})JSON";
    auto compiled = CompiledSpec::compile(source);
    require(compiled.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    PayloadBuilder builder = std::move(created).value();
    const std::uint32_t node = component_index(compiled.value(), "Node");
    const std::uint32_t empty = component_index(compiled.value(), "Empty");
    auto object = builder.declare_object(node);
    require(object.has_value());
    require(builder.begin_object_fill(object.value()).has_value());
    require(builder.push_null().has_value());
    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(builder.push_object(object.value()).has_value());
    auto values = builder.freeze();
    require(values.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto layout = GraphLayout::plan(runtime.value(), values.value());
    require(layout.has_value());
    VectorSink sink(layout.value().total_length());
    require(fastdb::payload::build::encode_graph(
                layout.value(), values.value(), sink)
                .has_value());
    require(fastdb::payload::view::open_graph(
                compiled.value(), sink.bytes().data(), sink.bytes().size())
                .has_value());

    const auto empty_aggregate = std::find_if(
        layout.value().object_aggregates().begin(),
        layout.value().object_aggregates().end(),
        [empty](const auto& value) {
            return value.component_index == empty;
        });
    const auto node_aggregate = std::find_if(
        layout.value().object_aggregates().begin(),
        layout.value().object_aggregates().end(),
        [node](const auto& value) {
            return value.component_index == node;
        });
    require(empty_aggregate != layout.value().object_aggregates().end() &&
            empty_aggregate->object_nodes.empty() &&
            node_aggregate != layout.value().object_aggregates().end());
    const auto* node_layout = runtime.value().component(node);
    require(node_layout != nullptr && node_layout->fields.size() == 1U &&
            node_layout->fields[0].validity_bit == UINT32_C(0));
    const std::uint64_t node_base =
        layout.value().regions()[node_aggregate->region_index].data_offset;
    auto present_zero_into_empty = sink.bytes();
    present_zero_into_empty[static_cast<std::size_t>(node_base)] = UINT8_C(1);
    require(stable_graph_rejection(compiled.value(), present_zero_into_empty,
                                   FDB_PAYLOAD_E_INVALID_REFERENCE,
                                   "reference_object_id_out_of_range"));

    constexpr std::string_view mismatched_source = R"JSON({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one",
     "type":{"kind":"component","id":"Node"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"target","type":{"kind":"ref","target":"Node",
       "nullable":true}}
    ]},
    {"id":"Empty","kind":"record","fields":[]}
  ]
})JSON";
    auto mismatched = CompiledSpec::compile(mismatched_source);
    require(mismatched.has_value());
    require(stable_graph_rejection(mismatched.value(), sink.bytes(),
                                   FDB_PAYLOAD_E_DIGEST_MISMATCH,
                                   "spec_digest_mismatch"));
    return EXIT_SUCCESS;
}

int test_graph_layout_and_open_allocation_sweeps() {
    allocation_failure::Reset reset;
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* item = find_case(corpus, "graph-all-values");
    require(item != nullptr);
    auto compiled = CompiledSpec::compile(item->success.source);
    require(compiled.has_value());
    auto values = build_graph_scenario(
        compiled.value(), item->success.scenario);
    require(values.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());

    // The number of allocations depends on the standard library (notably its
    // string representation). Measure this build's successful path so the
    // sweep tests every allocation instead of imposing a platform-specific
    // allocation ceiling.
    constexpr std::int64_t allocation_budget =
        std::numeric_limits<std::int64_t>::max();
    allocation_failure::fail_after = allocation_budget;
    auto measured_layout = GraphLayout::plan(runtime.value(), values.value());
    const std::int64_t layout_allocations =
        allocation_budget - allocation_failure::fail_after;
    allocation_failure::fail_after = INT64_C(-1);
    require(measured_layout.has_value());
    require(layout_allocations > INT64_C(0));

    std::int64_t layout_success = INT64_C(-1);
    for (std::int64_t allocation = INT64_C(0);
         allocation <= layout_allocations; ++allocation) {
        allocation_failure::fail_after = allocation;
        auto planned = GraphLayout::plan(runtime.value(), values.value());
        allocation_failure::fail_after = INT64_C(-1);
        if (planned.has_value()) {
            layout_success = allocation;
            break;
        }
        require(planned.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        auto retry = GraphLayout::plan(runtime.value(), values.value());
        require(retry.has_value());
    }
    require(layout_success == layout_allocations);

    auto encoded = encode_graph_case(*item);
    require(encoded.has_value());
    allocation_failure::fail_after = allocation_budget;
    auto measured_open = fastdb::payload::view::open_graph(
        encoded.value().spec, encoded.value().bytes.data(),
        encoded.value().bytes.size());
    const std::int64_t open_allocations =
        allocation_budget - allocation_failure::fail_after;
    allocation_failure::fail_after = INT64_C(-1);
    require(measured_open.has_value());
    require(open_allocations > INT64_C(0));
    std::cout << "graph allocation sweep: layout=" << layout_allocations
              << " open=" << open_allocations << '\n';

    std::int64_t open_success = INT64_C(-1);
    for (std::int64_t allocation = INT64_C(0);
         allocation <= open_allocations; ++allocation) {
        allocation_failure::fail_after = allocation;
        auto opened = fastdb::payload::view::open_graph(
            encoded.value().spec, encoded.value().bytes.data(),
            encoded.value().bytes.size());
        allocation_failure::fail_after = INT64_C(-1);
        if (opened.has_value()) {
            open_success = allocation;
            break;
        }
        require(opened.error().code() == FDB_PAYLOAD_E_ALLOCATION_FAILED);
        auto retry = fastdb::payload::view::open_graph(
            encoded.value().spec, encoded.value().bytes.data(),
            encoded.value().bytes.size());
        require(retry.has_value());
    }
    require(open_success == open_allocations);
    return EXIT_SUCCESS;
}

int test_fifty_thousand_object_binary_cycle_is_iterative() {
    constexpr std::size_t object_count = 50000U;
    constexpr std::string_view source = R"JSON({
  "schema":"fastdb.payload.v1",
  "profile":"object_graph.v1",
  "entries":[
    {"id":"root","cardinality":"one",
     "type":{"kind":"component","id":"Node"}}
  ],
  "components":[
    {"id":"Node","kind":"record","fields":[
      {"id":"next","type":{"kind":"ref","target":"Node"}}
    ]}
  ]
})JSON";
    auto compiled = CompiledSpec::compile(source);
    require(compiled.has_value());
    auto created = PayloadBuilder::create(compiled.value());
    require(created.has_value());
    PayloadBuilder& builder = created.value();
    const std::uint32_t node = component_index(compiled.value(), "Node");
    std::vector<ObjectHandle> objects;
    objects.reserve(object_count);
    for (std::size_t index = 0U; index < object_count; ++index) {
        auto declared = builder.declare_object(node);
        require(declared.has_value());
        objects.push_back(declared.value());
    }
    for (std::size_t index = 0U; index < object_count; ++index) {
        require(builder.begin_object_fill(objects[index]).has_value());
        require(builder
                    .push_ref(objects[(index + 1U) % object_count])
                    .has_value());
    }
    require(builder.begin_entry(UINT32_C(0), UINT64_C(1)).has_value());
    require(builder.push_object(objects.front()).has_value());
    auto values = builder.freeze();
    require(values.has_value());
    auto runtime = RuntimeSchema::compile(compiled.value());
    require(runtime.has_value());
    auto layout = GraphLayout::plan(runtime.value(), values.value());
    require(layout.has_value());
    require(layout.value().graph_object_count() == object_count);
    VectorSink sink(layout.value().total_length());
    require(fastdb::payload::build::encode_graph(
                layout.value(), values.value(), sink)
                .has_value());
    auto limits = fastdb::payload::view::default_open_options();
    limits.max_graph_objects = object_count;
    limits.max_nesting_depth = UINT64_C(1);
    auto opened = fastdb::payload::view::open_graph(
        compiled.value(), sink.bytes().data(), sink.bytes().size(), limits);
    require(opened.has_value());
    require(opened.value().graph_object_count() == object_count);
    require(opened.value().validation_work() ==
            layout.value().validation_work());
    return EXIT_SUCCESS;
}

int test_empty_zero_count_identity_pool() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* item = find_case(corpus, "graph-empty");
    require(item != nullptr);
    auto encoded = encode_graph_case(*item);
    require(encoded.has_value());
    const GraphLayout& layout = encoded.value().layout;
    require(layout.total_length() == UINT64_C(280));
    require(layout.root_value_count() == UINT64_C(0));
    require(layout.graph_object_count() == UINT64_C(0));
    require(layout.region_count() == UINT32_C(2));
    require(layout.entries().size() == 1U);
    require(layout.object_aggregates().size() == 1U);
    require_region(layout, 0U, RegionKind::entry_values, UINT64_C(280),
                   UINT64_C(0), UINT64_C(0), UINT32_C(8), UINT32_C(8));
    const auto& objects = require_region(
        layout, 1U, RegionKind::object_values, UINT64_C(280), UINT64_C(0),
        UINT64_C(0), UINT32_C(1), UINT32_C(1));
    require(objects.owner_index == UINT32_C(0));
    require(objects.runtime_type_id == UINT32_MAX);
    require(layout.object_aggregates()[0].component_index == UINT32_C(0));
    require(layout.object_aggregates()[0].region_index == UINT32_C(1));
    require(layout.object_aggregates()[0].object_nodes.empty());
    require(load_u32(encoded.value().bytes,
                     fastdb::payload::layout::header_profile_offset) ==
            FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1);
    require(load_u64(encoded.value().bytes,
                     fastdb::payload::layout::header_root_value_count_offset) ==
            UINT64_C(0));
    return EXIT_SUCCESS;
}

int test_object_zero_null_and_component_record_layout() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* item = find_case(corpus, "graph-single-root");
    require(item != nullptr);
    auto encoded = encode_graph_case(*item);
    require(encoded.has_value());
    const GraphLayout& layout = encoded.value().layout;
    require(layout.total_length() == UINT64_C(536));
    require(layout.root_value_count() == UINT64_C(2));
    require(layout.graph_object_count() == UINT64_C(1));
    require(layout.region_count() == UINT32_C(5));
    require_region(layout, 0U, RegionKind::entry_validity, UINT64_C(488),
                   UINT64_C(1), UINT64_C(1), UINT32_C(0), UINT32_C(1));
    require_region(layout, 1U, RegionKind::entry_values, UINT64_C(496),
                   UINT64_C(8), UINT64_C(1), UINT32_C(8), UINT32_C(8));
    require_region(layout, 2U, RegionKind::entry_validity, UINT64_C(504),
                   UINT64_C(1), UINT64_C(1), UINT32_C(0), UINT32_C(1));
    require_region(layout, 3U, RegionKind::entry_values, UINT64_C(512),
                   UINT64_C(8), UINT64_C(1), UINT32_C(8), UINT32_C(8));
    const auto& objects = require_region(
        layout, 4U, RegionKind::object_values, UINT64_C(520), UINT64_C(16),
        UINT64_C(1), UINT32_C(16), UINT32_C(8));
    require(objects.owner_index == UINT32_C(0));
    require(objects.runtime_type_id == UINT32_MAX);
    const auto& bytes = encoded.value().bytes;
    require(bytes[488] == UINT8_C(1));
    require(bytes[504] == UINT8_C(0));
    require(load_u64(bytes, UINT64_C(496)) == UINT64_C(0));
    require(load_u64(bytes, UINT64_C(512)) == UINT64_C(0));
    require(bytes[520] == UINT8_C(0));
    require(bytes[521] == UINT8_C(0));
    require(bytes[522] == UINT8_C(0x34));
    require(bytes[523] == UINT8_C(0x12));
    for (std::size_t index = 524U; index < 536U; ++index) {
        require(bytes[index] == UINT8_C(0));
    }
    return EXIT_SUCCESS;
}

int test_shared_cycle_fill_order_and_declaration_ids() {
    const auto corpus = fastdb::test::payload::load_graph_binary_golden_corpus(
        FASTDB_PAYLOAD_BINARY_FIXTURE_DIR);
    const BinaryGoldenCase* item = find_case(corpus, "graph-shared-cycle");
    require(item != nullptr);
    auto encoded = encode_graph_case(*item);
    auto reverse_fill = encode_graph_case(*item, true, false);
    auto reverse_declarations = encode_graph_case(*item, false, true);
    require(encoded.has_value() && reverse_fill.has_value() &&
            reverse_declarations.has_value());
    require(encoded.value().bytes == reverse_fill.value().bytes);
    require(encoded.value().bytes != reverse_declarations.value().bytes);
    const GraphLayout& layout = encoded.value().layout;
    require(layout.total_length() == UINT64_C(456));
    require(layout.root_value_count() == UINT64_C(2));
    require(layout.graph_object_count() == UINT64_C(2));
    require(layout.region_count() == UINT32_C(3));
    require_region(layout, 0U, RegionKind::entry_values, UINT64_C(376),
                   UINT64_C(8), UINT64_C(1), UINT32_C(8), UINT32_C(8));
    require_region(layout, 1U, RegionKind::entry_values, UINT64_C(384),
                   UINT64_C(8), UINT64_C(1), UINT32_C(8), UINT32_C(8));
    const auto& objects = require_region(
        layout, 2U, RegionKind::object_values, UINT64_C(392), UINT64_C(64),
        UINT64_C(2), UINT32_C(32), UINT32_C(8));
    require(objects.owner_index == UINT32_C(0));
    require(objects.runtime_type_id == UINT32_MAX);
    const auto& bytes = encoded.value().bytes;
    require(load_u64(bytes, UINT64_C(376)) == UINT64_C(0));
    require(load_u64(bytes, UINT64_C(384)) == UINT64_C(1));
    require(load_u32(bytes, UINT64_C(392)) == UINT32_C(100));
    require(load_u64(bytes, UINT64_C(400)) == UINT64_C(0));
    require(load_u64(bytes, UINT64_C(408)) == UINT64_C(1));
    require(bytes[416] == UINT8_C(1));
    require(bytes[418] == UINT8_C(0x34));
    require(bytes[419] == UINT8_C(0x12));
    require(load_u32(bytes, UINT64_C(424)) == UINT32_C(200));
    require(load_u64(bytes, UINT64_C(432)) == UINT64_C(1));
    require(load_u64(bytes, UINT64_C(440)) == UINT64_C(1));
    require(bytes[448] == UINT8_C(0));
    require(bytes[450] == UINT8_C(0xcd));
    require(bytes[451] == UINT8_C(0xab));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    if (test_region_rule_and_public_boundary() != EXIT_SUCCESS ||
        test_complete_graph_values_slice() != EXIT_SUCCESS ||
        test_ordered_graph_goldens_and_strict_open() != EXIT_SUCCESS ||
        test_selected_graph_invalid_golden_corpus() != EXIT_SUCCESS ||
        test_strict_open_rejects_malformed_graph_bytes_and_limits() !=
            EXIT_SUCCESS ||
        test_complete_graph_malformed_field_matrix() != EXIT_SUCCESS ||
        test_graph_resource_boundaries_and_empty_target_pool() !=
            EXIT_SUCCESS ||
        test_graph_layout_and_open_allocation_sweeps() != EXIT_SUCCESS ||
        test_fifty_thousand_object_binary_cycle_is_iterative() !=
            EXIT_SUCCESS ||
        test_empty_zero_count_identity_pool() != EXIT_SUCCESS ||
        test_object_zero_null_and_component_record_layout() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_shared_cycle_fill_order_and_declaration_ids();
}
