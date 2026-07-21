#include <fastdb_payload.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC_PATH must name the matching spec"
#endif

#ifndef FASTDB_PAYLOAD_FUZZ_MISMATCH_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_MISMATCH_SPEC_PATH must name the mismatch spec"
#endif
#ifndef FASTDB_PAYLOAD_FUZZ_GRAPH_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_GRAPH_SPEC_PATH must name the graph value spec"
#endif
#ifndef FASTDB_PAYLOAD_FUZZ_GRAPH_CYCLE_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_GRAPH_CYCLE_SPEC_PATH must name the graph cycle spec"
#endif
#ifndef FASTDB_PAYLOAD_FUZZ_GRAPH_NULL_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_GRAPH_NULL_SPEC_PATH must name the graph null spec"
#endif
#ifndef FASTDB_PAYLOAD_FUZZ_GRAPH_DISCONNECTED_SPEC_PATH
#error "FASTDB_PAYLOAD_FUZZ_GRAPH_DISCONNECTED_SPEC_PATH must name the disconnected graph spec"
#endif

namespace {

constexpr fdb_payload_v1_status_t kSuccess = UINT32_C(0);
constexpr std::uint64_t kTraversalLimit = UINT64_C(262144);

[[noreturn]] void invariant_failure(
    fdb_payload_v1_error_t* error = nullptr) {
    fdb_payload_v1_error_release(error);
    std::abort();
}

void require_success(fdb_payload_v1_status_t status,
                     fdb_payload_v1_error_t* error) {
    if (status != kSuccess || error != nullptr) {
        invariant_failure(error);
    }
}

#define FASTDB_FUZZ_REQUIRE_SUCCESS(expression)                             \
    do {                                                                    \
        const fdb_payload_v1_status_t call_status = (expression);           \
        require_success(call_status, error);                                \
        error = nullptr;                                                    \
    } while (false)

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        invariant_failure();
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

fdb_payload_v1_spec_t* compile_spec(const char* path) {
    const std::string source = read_file(path);
    fdb_payload_v1_spec_t* spec = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const auto status = fdb_payload_v1_spec_compile_json(
        reinterpret_cast<const std::uint8_t*>(source.data()),
        static_cast<std::uint64_t>(source.size()), nullptr, &spec, &error);
    require_success(status, error);
    if (spec == nullptr) {
        invariant_failure();
    }
    return spec;
}

struct Specs final {
    Specs()
        : matching(compile_spec(FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC_PATH)),
          mismatch(compile_spec(FASTDB_PAYLOAD_FUZZ_MISMATCH_SPEC_PATH)),
          graph(compile_spec(FASTDB_PAYLOAD_FUZZ_GRAPH_SPEC_PATH)),
          graph_cycle(
              compile_spec(FASTDB_PAYLOAD_FUZZ_GRAPH_CYCLE_SPEC_PATH)),
          graph_null(compile_spec(FASTDB_PAYLOAD_FUZZ_GRAPH_NULL_SPEC_PATH)),
          graph_disconnected(compile_spec(
              FASTDB_PAYLOAD_FUZZ_GRAPH_DISCONNECTED_SPEC_PATH)) {
        const std::array<fdb_payload_v1_spec_t*, 6> all_specs{{
            matching, mismatch, graph, graph_cycle, graph_null,
            graph_disconnected}};
        std::array<std::array<std::uint8_t, FDB_PAYLOAD_V1_SHA256_SIZE>, 6>
            digests{};
        fdb_payload_v1_error_t* error = nullptr;
        for (std::size_t index = 0U; index < all_specs.size(); ++index) {
            FASTDB_FUZZ_REQUIRE_SUCCESS(fdb_payload_v1_spec_sha256(
                all_specs[index], digests[index].data(), &error));
            fdb_payload_v1_profile_t profile = UINT32_C(0);
            FASTDB_FUZZ_REQUIRE_SUCCESS(fdb_payload_v1_spec_profile(
                all_specs[index], &profile, &error));
            const auto expected_profile =
                index < 2U ? FDB_PAYLOAD_PROFILE_RECORD_V1
                           : FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1;
            if (profile != expected_profile) {
                invariant_failure();
            }
            for (std::size_t prior = 0U; prior < index; ++prior) {
                if (digests[index] == digests[prior]) {
                    invariant_failure();
                }
            }
        }
    }

    ~Specs() {
        fdb_payload_v1_spec_release(graph_disconnected);
        fdb_payload_v1_spec_release(graph_null);
        fdb_payload_v1_spec_release(graph_cycle);
        fdb_payload_v1_spec_release(graph);
        fdb_payload_v1_spec_release(mismatch);
        fdb_payload_v1_spec_release(matching);
    }

    Specs(const Specs&) = delete;
    Specs& operator=(const Specs&) = delete;

    fdb_payload_v1_spec_t* matching;
    fdb_payload_v1_spec_t* mismatch;
    fdb_payload_v1_spec_t* graph;
    fdb_payload_v1_spec_t* graph_cycle;
    fdb_payload_v1_spec_t* graph_null;
    fdb_payload_v1_spec_t* graph_disconnected;
};

Specs& specs() {
    static Specs instance;
    return instance;
}

std::string error_text(
    const fdb_payload_v1_error_t* error,
    void (*query)(const fdb_payload_v1_error_t*, const std::uint8_t**,
                  std::uint64_t*)) {
    const std::uint8_t* data = nullptr;
    std::uint64_t size = UINT64_C(0);
    query(error, &data, &size);
    if ((data == nullptr && size != UINT64_C(0)) ||
        size > static_cast<std::uint64_t>(
                   std::numeric_limits<std::size_t>::max())) {
        invariant_failure();
    }
    if (size == UINT64_C(0)) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data),
                       static_cast<std::size_t>(size));
}

struct ErrorFacts final {
    fdb_payload_v1_status_t status{kSuccess};
    std::uint32_t code{UINT32_C(0)};
    std::string symbol;
    std::string path;
    std::string message;
    std::string details;

    friend bool operator==(const ErrorFacts& left,
                           const ErrorFacts& right) {
        return left.status == right.status && left.code == right.code &&
               left.symbol == right.symbol && left.path == right.path &&
               left.message == right.message &&
               left.details == right.details;
    }
};

struct OpenResult final {
    fdb_payload_v1_payload_t* payload{nullptr};
    ErrorFacts error;

    bool succeeded() const noexcept { return payload != nullptr; }
};

ErrorFacts take_error_facts(fdb_payload_v1_status_t status,
                            fdb_payload_v1_error_t* error) {
    if (status == kSuccess || error == nullptr ||
        fdb_payload_v1_error_code(error) != status) {
        invariant_failure(error);
    }
    ErrorFacts facts;
    facts.status = status;
    facts.code = fdb_payload_v1_error_code(error);
    facts.symbol = error_text(error, fdb_payload_v1_error_symbol);
    facts.path = error_text(error, fdb_payload_v1_error_path);
    facts.message = error_text(error, fdb_payload_v1_error_message);
    facts.details = error_text(error, fdb_payload_v1_error_details_json);
    fdb_payload_v1_error_release(error);
    return facts;
}

fdb_payload_v1_open_options_t options_for_input(const std::uint8_t* data,
                                                std::size_t size) {
    fdb_payload_v1_open_options_t options{};
    fdb_payload_v1_open_options_init(&options);
    const std::uint64_t input_size = static_cast<std::uint64_t>(size);
    options.max_total_bytes = std::max(input_size, UINT64_C(128));
    options.max_regions = UINT64_C(4096);
    options.max_entries = UINT64_C(1024);
    options.max_components = UINT64_C(1024);
    options.max_nesting_depth = UINT64_C(512);
    options.max_list_elements = kTraversalLimit;
    options.max_graph_objects = UINT64_C(65536);
    options.max_string_bytes = std::max(input_size, UINT64_C(1));
    options.max_validation_work = UINT64_C(262144);

    // No byte is consumed or stripped. A byte from the exact borrowed span
    // selects only bounded V1 option variants. Every reviewed seed begins with
    // the canonical magic prefix and therefore selects the complete path.
    const std::uint8_t selector =
        size > 1U ? static_cast<std::uint8_t>(data[1] & UINT8_C(3))
                  : UINT8_C(0);
    if (selector == UINT8_C(1)) {
        options.flags = UINT32_C(0);
    } else if (selector == UINT8_C(2)) {
        options.max_regions = UINT64_C(8);
    } else if (selector == UINT8_C(3)) {
        options.max_validation_work = UINT64_C(256);
    }
    return options;
}

OpenResult open_once(const fdb_payload_v1_spec_t* spec,
                     const std::uint8_t* data,
                     std::size_t size,
                     const fdb_payload_v1_open_options_t& options) {
    fdb_payload_v1_payload_t* payload = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    const auto status = fdb_payload_v1_payload_open_copy(
        spec, data, static_cast<std::uint64_t>(size), &options, &payload,
        &error);
    if (status == kSuccess) {
        if (payload == nullptr || error != nullptr) {
            invariant_failure(error);
        }
        return OpenResult{payload, {}};
    }
    if (payload != nullptr) {
        fdb_payload_v1_payload_release(payload);
        invariant_failure(error);
    }
    return OpenResult{nullptr, take_error_facts(status, error)};
}

void check_scalar(fdb_payload_v1_view_t* view, std::uint32_t kind) {
    fdb_payload_v1_error_t* error = nullptr;
    switch (kind) {
    case FDB_PAYLOAD_VIEW_BOOL: {
        std::uint8_t value = UINT8_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_bool(view, &value, &error));
        break;
    }
    case FDB_PAYLOAD_VIEW_U8: {
        std::uint8_t value = UINT8_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_u8(view, &value, &error));
        break;
    }
    case FDB_PAYLOAD_VIEW_U16: {
        std::uint16_t value = UINT16_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_u16(view, &value, &error));
        break;
    }
    case FDB_PAYLOAD_VIEW_U32: {
        std::uint32_t value = UINT32_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_u32(view, &value, &error));
        break;
    }
    case FDB_PAYLOAD_VIEW_I32: {
        std::int32_t value = INT32_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_i32(view, &value, &error));
        break;
    }
    case FDB_PAYLOAD_VIEW_U8N: {
        std::uint64_t value = UINT64_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_u8n_f64_bits(view, &value, &error));
        break;
    }
    case FDB_PAYLOAD_VIEW_U16N: {
        std::uint64_t value = UINT64_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_u16n_f64_bits(view, &value, &error));
        break;
    }
    case FDB_PAYLOAD_VIEW_F32: {
        std::uint32_t value = UINT32_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_f32_bits(view, &value, &error));
        break;
    }
    case FDB_PAYLOAD_VIEW_F64: {
        std::uint64_t value = UINT64_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_get_f64_bits(view, &value, &error));
        break;
    }
    default:
        invariant_failure();
    }
}

void check_variable_access(fdb_payload_v1_view_t* view,
                           std::uint32_t kind) {
    fdb_payload_v1_access_t* access = nullptr;
    fdb_payload_v1_error_t* error = nullptr;
    FASTDB_FUZZ_REQUIRE_SUCCESS(
        fdb_payload_v1_view_acquire(view, &access, &error));
    if (access == nullptr) {
        invariant_failure();
    }
    std::uint64_t size = UINT64_C(0);
    if (kind == FDB_PAYLOAD_VIEW_WSTR) {
        const std::uint16_t* data = nullptr;
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_access_wstr(access, &data, &size, &error));
        if ((data == nullptr && size != UINT64_C(0)) ||
            (data != nullptr &&
             reinterpret_cast<std::uintptr_t>(data) %
                     alignof(std::uint16_t) !=
                 std::uintptr_t{0})) {
            fdb_payload_v1_access_release(access);
            invariant_failure();
        }
    } else {
        const std::uint8_t* data = nullptr;
        const auto status =
            kind == FDB_PAYLOAD_VIEW_STR
                ? fdb_payload_v1_access_str(access, &data, &size, &error)
                : fdb_payload_v1_access_bytes(access, &data, &size, &error);
        require_success(status, error);
        error = nullptr;
        if (data == nullptr && size != UINT64_C(0)) {
            fdb_payload_v1_access_release(access);
            invariant_failure();
        }
    }
    fdb_payload_v1_access_release(access);
}

void traverse_owned_view(fdb_payload_v1_view_t* root,
                         fdb_payload_v1_profile_t profile) {
    if (root == nullptr) {
        invariant_failure();
    }
    std::vector<fdb_payload_v1_view_t*> pending;
    pending.push_back(root);
    std::set<std::pair<std::uint32_t, std::uint64_t>> visited_objects;
    std::uint64_t visited_values = UINT64_C(0);
    while (!pending.empty()) {
        fdb_payload_v1_view_t* const view = pending.back();
        pending.pop_back();
        ++visited_values;
        if (visited_values > kTraversalLimit) {
            fdb_payload_v1_view_release(view);
            for (auto* remaining : pending) {
                fdb_payload_v1_view_release(remaining);
            }
            invariant_failure();
        }

        fdb_payload_v1_error_t* error = nullptr;
        std::uint32_t kind = UINT32_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_kind(view, &kind, &error));
        std::uint8_t is_null = UINT8_C(0);
        FASTDB_FUZZ_REQUIRE_SUCCESS(
            fdb_payload_v1_view_is_null(view, &is_null, &error));
        if (is_null != UINT8_C(0)) {
            fdb_payload_v1_view_release(view);
            continue;
        }

        if (kind == FDB_PAYLOAD_VIEW_REF) {
            if (profile != FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1) {
                fdb_payload_v1_view_release(view);
                invariant_failure();
            }
            std::uint32_t component_index = UINT32_C(0);
            std::uint64_t object_id = UINT64_C(0);
            FASTDB_FUZZ_REQUIRE_SUCCESS(
                fdb_payload_v1_view_graph_identity(
                    view, &component_index, &object_id, &error));
            fdb_payload_v1_view_t* target = nullptr;
            FASTDB_FUZZ_REQUIRE_SUCCESS(fdb_payload_v1_view_ref_target(
                view, &target, &error));
            if (target == nullptr) {
                invariant_failure();
            }
            std::uint32_t target_component = UINT32_C(0);
            std::uint64_t target_object = UINT64_C(0);
            FASTDB_FUZZ_REQUIRE_SUCCESS(
                fdb_payload_v1_view_graph_identity(
                    target, &target_component, &target_object, &error));
            if (target_component != component_index ||
                target_object != object_id) {
                fdb_payload_v1_view_release(target);
                fdb_payload_v1_view_release(view);
                invariant_failure();
            }
            if (visited_objects.find({component_index, object_id}) !=
                visited_objects.end()) {
                fdb_payload_v1_view_release(target);
            } else {
                pending.push_back(target);
            }
        } else if (kind == FDB_PAYLOAD_VIEW_SEQUENCE ||
            kind == FDB_PAYLOAD_VIEW_LIST) {
            std::uint64_t length = UINT64_C(0);
            FASTDB_FUZZ_REQUIRE_SUCCESS(
                fdb_payload_v1_view_length(view, &length, &error));
            if (length > kTraversalLimit - visited_values ||
                length > kTraversalLimit -
                             static_cast<std::uint64_t>(pending.size())) {
                fdb_payload_v1_view_release(view);
                for (auto* remaining : pending) {
                    fdb_payload_v1_view_release(remaining);
                }
                invariant_failure();
            }
            for (std::uint64_t index = UINT64_C(0); index < length; ++index) {
                fdb_payload_v1_view_t* child = nullptr;
                FASTDB_FUZZ_REQUIRE_SUCCESS(
                    fdb_payload_v1_view_at(view, index, &child, &error));
                if (child == nullptr) {
                    invariant_failure();
                }
                pending.push_back(child);
            }
        } else if (kind == FDB_PAYLOAD_VIEW_COMPONENT) {
            std::uint32_t component_index = UINT32_C(0);
            FASTDB_FUZZ_REQUIRE_SUCCESS(fdb_payload_v1_view_component_index(
                view, &component_index, &error));
            if (profile == FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1) {
                std::uint32_t identity_component = UINT32_C(0);
                std::uint64_t object_id = UINT64_C(0);
                const auto identity_status =
                    fdb_payload_v1_view_graph_identity(
                        view, &identity_component, &object_id, &error);
                if (identity_status == kSuccess) {
                    if (error != nullptr ||
                        identity_component != component_index) {
                        invariant_failure(error);
                    }
                    const bool inserted =
                        visited_objects
                            .insert({identity_component, object_id})
                            .second;
                    if (!inserted) {
                        fdb_payload_v1_view_release(view);
                        continue;
                    }
                } else if (identity_status == FDB_PAYLOAD_E_TYPE_MISMATCH &&
                           error != nullptr &&
                           fdb_payload_v1_error_code(error) ==
                               identity_status) {
                    fdb_payload_v1_error_release(error);
                    error = nullptr;
                } else {
                    invariant_failure(error);
                }
            }
            std::uint32_t field_count = UINT32_C(0);
            FASTDB_FUZZ_REQUIRE_SUCCESS(fdb_payload_v1_view_field_count(
                view, &field_count, &error));
            if (static_cast<std::uint64_t>(field_count) >
                kTraversalLimit -
                    static_cast<std::uint64_t>(pending.size())) {
                fdb_payload_v1_view_release(view);
                for (auto* remaining : pending) {
                    fdb_payload_v1_view_release(remaining);
                }
                invariant_failure();
            }
            for (std::uint32_t field = UINT32_C(0); field < field_count;
                 ++field) {
                fdb_payload_v1_view_t* child = nullptr;
                FASTDB_FUZZ_REQUIRE_SUCCESS(
                    fdb_payload_v1_view_field(view, field, &child, &error));
                if (child == nullptr) {
                    invariant_failure();
                }
                pending.push_back(child);
            }
        } else if (kind == FDB_PAYLOAD_VIEW_STR ||
                   kind == FDB_PAYLOAD_VIEW_WSTR ||
                   kind == FDB_PAYLOAD_VIEW_BYTES) {
            check_variable_access(view, kind);
        } else if (kind >= FDB_PAYLOAD_VIEW_BOOL &&
                   kind <= FDB_PAYLOAD_VIEW_F64) {
            check_scalar(view, kind);
        } else {
            fdb_payload_v1_view_release(view);
            for (auto* remaining : pending) {
                fdb_payload_v1_view_release(remaining);
            }
            invariant_failure();
        }
        fdb_payload_v1_view_release(view);
    }
}

void exercise_success(fdb_payload_v1_payload_t* payload,
                      const fdb_payload_v1_spec_t* spec,
                      const std::uint8_t* input,
                      std::size_t input_size) {
    fdb_payload_v1_error_t* error = nullptr;
    fdb_payload_v1_profile_t profile = UINT32_C(0);
    FASTDB_FUZZ_REQUIRE_SUCCESS(
        fdb_payload_v1_payload_profile(payload, &profile, &error));
    if (profile != FDB_PAYLOAD_PROFILE_RECORD_V1 &&
        profile != FDB_PAYLOAD_PROFILE_OBJECT_GRAPH_V1) {
        invariant_failure();
    }

    fdb_payload_v1_access_t* access = nullptr;
    FASTDB_FUZZ_REQUIRE_SUCCESS(
        fdb_payload_v1_payload_acquire(payload, &access, &error));
    const std::uint8_t* bytes = nullptr;
    std::uint64_t byte_count = UINT64_C(0);
    FASTDB_FUZZ_REQUIRE_SUCCESS(fdb_payload_v1_access_payload_bytes(
        access, &bytes, &byte_count, &error));
    if (byte_count != static_cast<std::uint64_t>(input_size) ||
        (input_size != 0U &&
         (bytes == nullptr || std::memcmp(bytes, input, input_size) != 0))) {
        fdb_payload_v1_access_release(access);
        invariant_failure();
    }
    fdb_payload_v1_access_release(access);

    std::uint32_t entry_count = UINT32_C(0);
    FASTDB_FUZZ_REQUIRE_SUCCESS(
        fdb_payload_v1_spec_entry_count(spec, &entry_count, &error));
    fdb_payload_v1_view_t* detached = nullptr;
    for (std::uint32_t entry = UINT32_C(0); entry < entry_count; ++entry) {
        fdb_payload_v1_view_t* sequence = nullptr;
        FASTDB_FUZZ_REQUIRE_SUCCESS(fdb_payload_v1_payload_entry_view(
            payload, entry, &sequence, &error));
        if (sequence == nullptr) {
            invariant_failure();
        }
        if (detached == nullptr) {
            FASTDB_FUZZ_REQUIRE_SUCCESS(fdb_payload_v1_view_materialize(
                sequence, &detached, &error));
            if (detached == nullptr) {
                invariant_failure();
            }
        }
        traverse_owned_view(sequence, profile);
    }

    FASTDB_FUZZ_REQUIRE_SUCCESS(
        fdb_payload_v1_payload_invalidate(payload, &error));
    if (detached != nullptr) {
        traverse_owned_view(detached, profile);
    }
    fdb_payload_v1_payload_release(payload);
}

void invalidate_and_release(fdb_payload_v1_payload_t* payload) {
    fdb_payload_v1_error_t* error = nullptr;
    FASTDB_FUZZ_REQUIRE_SUCCESS(
        fdb_payload_v1_payload_invalidate(payload, &error));
    fdb_payload_v1_payload_release(payload);
}

void exercise_spec(const fdb_payload_v1_spec_t* spec,
                   const std::uint8_t* data,
                   std::size_t size,
                   const fdb_payload_v1_open_options_t& options) {
    OpenResult first = open_once(spec, data, size, options);
    OpenResult second = open_once(spec, data, size, options);
    if (first.succeeded() != second.succeeded()) {
        if (first.payload != nullptr) {
            fdb_payload_v1_payload_release(first.payload);
        }
        if (second.payload != nullptr) {
            fdb_payload_v1_payload_release(second.payload);
        }
        invariant_failure();
    }
    if (!first.succeeded()) {
        if (!(first.error == second.error)) {
            invariant_failure();
        }
        return;
    }
    exercise_success(first.payload, spec, data, size);
    invalidate_and_release(second.payload);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (size > static_cast<std::size_t>(
                       std::numeric_limits<std::uint64_t>::max())) {
            return 0;
        }
    }

    const auto options = options_for_input(data, size);
    exercise_spec(specs().matching, data, size, options);
    exercise_spec(specs().mismatch, data, size, options);
    exercise_spec(specs().graph, data, size, options);
    exercise_spec(specs().graph_cycle, data, size, options);
    exercise_spec(specs().graph_null, data, size, options);
    exercise_spec(specs().graph_disconnected, data, size, options);
    return 0;
}
