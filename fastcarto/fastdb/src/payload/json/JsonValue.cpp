#include "payload/json/JsonValue.hpp"

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <variant>

namespace fastdb::payload::json {
namespace {

JsonValue::Storage checked_string_storage(const char* value) {
    if (value == nullptr) {
        throw std::invalid_argument("JSON string pointer must not be null");
    }
    return JsonValue::Storage(std::in_place_type<std::string>, value);
}

const JsonValue::Storage& moved_from_storage() noexcept {
    static const JsonValue::Storage storage{nullptr};
    return storage;
}

}  // namespace

struct JsonValue::Node final {
    explicit Node(Storage value) : storage(std::move(value)) {}

    std::atomic<std::size_t> references{1U};
    Node* release_next{nullptr};
    Storage storage;
};

JsonValue::JsonValue() noexcept = default;

JsonValue::JsonValue(std::nullptr_t) noexcept {}

JsonValue::JsonValue(bool value) : JsonValue(Storage{value}) {}

JsonValue::JsonValue(double value) : JsonValue(Storage{value}) {}

JsonValue::JsonValue(std::string value)
    : JsonValue(Storage(std::in_place_type<std::string>, std::move(value))) {}

JsonValue::JsonValue(const char* value)
    : JsonValue(checked_string_storage(value)) {}

JsonValue::JsonValue(const JsonValue& other) noexcept
    : node_(retain_node(other.node_)) {}

JsonValue::JsonValue(JsonValue&& other) noexcept
    : node_(std::exchange(other.node_, nullptr)) {}

JsonValue::~JsonValue() { release_node(node_); }

JsonValue& JsonValue::operator=(const JsonValue& other) noexcept {
    Node* const replacement = retain_node(other.node_);
    Node* const previous = std::exchange(node_, replacement);
    release_node(previous);
    return *this;
}

JsonValue& JsonValue::operator=(JsonValue&& other) noexcept {
    if (this != &other) {
        Node* const previous =
            std::exchange(node_, std::exchange(other.node_, nullptr));
        release_node(previous);
    }
    return *this;
}

JsonValue JsonValue::array(Array values) {
    return JsonValue(
        Storage(std::in_place_type<Array>, std::move(values)));
}

JsonValue JsonValue::object(Object members) {
    return JsonValue(
        Storage(std::in_place_type<Object>, std::move(members)));
}

const JsonValue::Storage& JsonValue::storage() const noexcept {
    return node_ == nullptr ? moved_from_storage() : node_->storage;
}

JsonValue::JsonValue(Storage storage) : node_(new Node(std::move(storage))) {}

JsonValue::Node* JsonValue::retain_node(Node* node) noexcept {
    if (node != nullptr) {
        node->references.fetch_add(1U, std::memory_order_relaxed);
    }
    return node;
}

void JsonValue::release_node(Node* node) noexcept {
    Node* pending = nullptr;
    const auto enqueue_if_last = [&pending](Node* candidate) noexcept {
        if (candidate == nullptr) {
            return;
        }
        if (candidate->references.fetch_sub(1U, std::memory_order_release) ==
            1U) {
            std::atomic_thread_fence(std::memory_order_acquire);
            candidate->release_next = pending;
            pending = candidate;
        }
    };

    enqueue_if_last(node);
    while (pending != nullptr) {
        Node* const current = pending;
        pending = current->release_next;

        if (auto* array = std::get_if<Array>(&current->storage)) {
            for (JsonValue& child : *array) {
                enqueue_if_last(std::exchange(child.node_, nullptr));
            }
        } else if (auto* object = std::get_if<Object>(&current->storage)) {
            for (Member& member : *object) {
                enqueue_if_last(
                    std::exchange(member.second.node_, nullptr));
            }
        }

        delete current;
    }
}

}  // namespace fastdb::payload::json
