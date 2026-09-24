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
constexpr std::size_t kSharedChildFanOut = 50000U;

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

JsonValue nest_objects(JsonValue leaf) {
    JsonValue nested = std::move(leaf);
    for (std::size_t depth = 0; depth < kDepth; ++depth) {
        JsonValue::Object wrapper;
        wrapper.reserve(1U);
        wrapper.emplace_back("k", std::move(nested));
        nested = JsonValue::object(std::move(wrapper));
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

int require_deep_object_serialization(const JsonValue& value,
                                      std::string_view leaf) {
    constexpr std::string_view prefix = R"({"k":)";
    const auto result = jcs_serialize(value);
    const auto* serialized = std::get_if<std::string>(&result);
    require(serialized != nullptr);
    require(serialized->size() == kDepth * (prefix.size() + 1U) +
                                      leaf.size());
    for (std::size_t depth = 0; depth < kDepth; ++depth) {
        require(serialized->compare(depth * prefix.size(), prefix.size(),
                                    prefix) == 0);
        require((*serialized)[serialized->size() - depth - 1U] == '}');
    }
    require(serialized->compare(kDepth * prefix.size(), leaf.size(), leaf) ==
            0);
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

int serialize_deep_object_tree() {
    const JsonValue value = nest_objects(JsonValue{"leaf"});
    require(require_deep_object_serialization(value, "\"leaf\"") ==
            EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int destroy_deep_object_tree() {
    {
        const JsonValue value = nest_objects(JsonValue{true});
        require(std::holds_alternative<JsonValue::Object>(value.storage()));
    }
    return EXIT_SUCCESS;
}

int unwind_deep_object_leaf_failure() {
    const JsonValue value =
        nest_objects(JsonValue{std::numeric_limits<double>::quiet_NaN()});
    const auto result = jcs_serialize(value);
    const auto* failure = std::get_if<JcsFailure>(&result);
    require(failure != nullptr);
    require(*failure == JcsFailure::non_finite_number);
    return EXIT_SUCCESS;
}

JsonValue shared_child_parent(const JsonValue& child) {
    JsonValue::Object parent;
    parent.reserve(1U);
    parent.emplace_back("child", child);
    return JsonValue::object(std::move(parent));
}

int require_shared_child_parent(const JsonValue& value) {
    constexpr std::string_view expected =
        R"({"child":{"payload":["shared",42,true]}})";
    const auto result = jcs_serialize(value);
    const auto* serialized = std::get_if<std::string>(&result);
    require(serialized != nullptr);
    require(*serialized == expected);
    return EXIT_SUCCESS;
}

int copy_move_release_shared_child_dag() {
    JsonValue::Array child_values;
    child_values.reserve(3U);
    child_values.emplace_back("shared");
    child_values.emplace_back(42.0);
    child_values.emplace_back(true);

    JsonValue::Object child_members;
    child_members.reserve(1U);
    child_members.emplace_back(
        "payload", JsonValue::array(std::move(child_values)));
    JsonValue shared_child = JsonValue::object(std::move(child_members));

    std::vector<JsonValue> parents;
    parents.reserve(kSharedChildFanOut);
    for (std::size_t index = 0; index < kSharedChildFanOut; ++index) {
        parents.push_back(shared_child_parent(shared_child));
    }

    std::vector<JsonValue> copied_parents;
    copied_parents.reserve(kSharedChildFanOut / 5U);
    for (std::size_t index = 0; index < kSharedChildFanOut; index += 5U) {
        copied_parents.push_back(parents[index]);
    }

    std::vector<JsonValue> moved_parents;
    moved_parents.reserve(kSharedChildFanOut / 2U);
    for (std::size_t index = 0; index < kSharedChildFanOut; index += 2U) {
        moved_parents.push_back(std::move(parents[index]));
    }

    shared_child = JsonValue{};
    for (std::size_t index = 1U; index < kSharedChildFanOut; index += 4U) {
        parents[index] = JsonValue{};
    }
    for (std::size_t remaining = kSharedChildFanOut; remaining > 0U;
         --remaining) {
        const std::size_t index = remaining - 1U;
        if (index % 2U != 0U) {
            parents[index] = JsonValue{};
        }
    }
    parents.clear();

    require(!moved_parents.empty());
    require(!copied_parents.empty());
    require(require_shared_child_parent(moved_parents.front()) ==
            EXIT_SUCCESS);
    require(require_shared_child_parent(copied_parents.back()) ==
            EXIT_SUCCESS);

    JsonValue survivor = copied_parents.back();
    for (JsonValue& parent : moved_parents) {
        parent = JsonValue{};
    }
    for (std::size_t remaining = copied_parents.size(); remaining > 0U;
         --remaining) {
        copied_parents[remaining - 1U] = JsonValue{};
    }
    moved_parents.clear();
    copied_parents.clear();

    JsonValue survivor_copy = survivor;
    JsonValue survivor_moved = std::move(survivor_copy);
    survivor = JsonValue{};
    require(require_shared_child_parent(survivor_moved) == EXIT_SUCCESS);
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
    if (mode == "object_serialize") {
        return serialize_deep_object_tree();
    }
    if (mode == "object_destroy") {
        return destroy_deep_object_tree();
    }
    if (mode == "object_failure") {
        return unwind_deep_object_leaf_failure();
    }
    if (mode == "shared_child_dag") {
        return copy_move_release_shared_child_dag();
    }
    return EXIT_FAILURE;
}
