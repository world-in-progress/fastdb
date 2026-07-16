#include "TestSupport.hpp"

#include "payload/json/Jcs.hpp"
#include "payload/json/JsonValue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using fastdb::payload::json::JcsFailure;
using fastdb::payload::json::JsonValue;
using fastdb::payload::json::jcs_serialize;

constexpr std::size_t kDepth = 50000U;

JsonValue nest_arrays(JsonValue leaf) {
    JsonValue nested = std::move(leaf);
    for (std::size_t depth = 0; depth < kDepth; ++depth) {
        JsonValue::Array wrapper;
        wrapper.reserve(1U);
        wrapper.push_back(std::move(nested));
        nested = JsonValue::array(std::move(wrapper));
    }
    return nested;
}

int require_deep_serialization(const JsonValue& value,
                               std::string_view leaf) {
    const auto result = jcs_serialize(value);
    const auto* serialized = std::get_if<std::string>(&result);
    require(serialized != nullptr);
    require(serialized->size() == kDepth * 2U + leaf.size());
    require(serialized->compare(kDepth, leaf.size(), leaf) == 0);
    for (std::size_t index = 0; index < kDepth; ++index) {
        require((*serialized)[index] == '[');
        require((*serialized)[serialized->size() - index - 1U] == ']');
    }
    return EXIT_SUCCESS;
}

int serialize_deep_tree() {
    const JsonValue value = nest_arrays(JsonValue{"leaf"});
    require(require_deep_serialization(value, "\"leaf\"") == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int destroy_deep_tree() {
    {
        const JsonValue value = nest_arrays(JsonValue{true});
        require(std::holds_alternative<JsonValue::Array>(value.storage()));
    }
    return EXIT_SUCCESS;
}

int unwind_deep_leaf_failure() {
    const JsonValue value =
        nest_arrays(JsonValue{std::numeric_limits<double>::quiet_NaN()});
    const auto result = jcs_serialize(value);
    const auto* failure = std::get_if<JcsFailure>(&result);
    require(failure != nullptr);
    require(*failure == JcsFailure::non_finite_number);
    return EXIT_SUCCESS;
}

int copy_and_move_deep_tree() {
    const JsonValue original = nest_arrays(JsonValue{"copy"});
    JsonValue copy = original;
    JsonValue moved = std::move(copy);
    JsonValue assigned;
    assigned = original;
    assigned = assigned;
    JsonValue move_assigned;
    move_assigned = std::move(assigned);
    move_assigned = std::move(move_assigned);

    require(require_deep_serialization(original, "\"copy\"") ==
            EXIT_SUCCESS);
    require(require_deep_serialization(moved, "\"copy\"") == EXIT_SUCCESS);
    require(require_deep_serialization(move_assigned, "\"copy\"") ==
            EXIT_SUCCESS);

    constexpr std::size_t worker_count = 4U;
    JsonValue concurrent = nest_arrays(JsonValue{"thread"});
    std::atomic<bool> start{false};
    std::atomic<bool> succeeded{true};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        JsonValue worker_value = concurrent;
        workers.emplace_back(
            [value = std::move(worker_value), &start, &succeeded]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                if (require_deep_serialization(value, "\"thread\"") !=
                    EXIT_SUCCESS) {
                    succeeded.store(false, std::memory_order_relaxed);
                }
            });
    }
    concurrent = JsonValue{};
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(succeeded.load(std::memory_order_relaxed));
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argument_count, char** arguments) {
    require(argument_count == 2);
    const std::string_view mode = arguments[1];
    if (mode == "serialize") {
        return serialize_deep_tree();
    }
    if (mode == "destroy") {
        return destroy_deep_tree();
    }
    if (mode == "failure") {
        return unwind_deep_leaf_failure();
    }
    if (mode == "copy_move") {
        return copy_and_move_deep_tree();
    }
    return EXIT_FAILURE;
}
