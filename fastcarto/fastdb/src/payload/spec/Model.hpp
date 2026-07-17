#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fastdb::payload::spec {

enum class Profile : std::uint8_t { record_v1, object_graph_v1 };

enum class Cardinality : std::uint8_t { one, many };

enum class TypeKind : std::uint8_t {
    boolean,
    u8,
    u16,
    u32,
    i32,
    u8n,
    u16n,
    f32,
    f64,
    str,
    wstr,
    bytes,
    component,
    ref,
    list,
};

struct TypeNode final {
    TypeNode(TypeKind type_kind, bool is_nullable) noexcept
        : kind(type_kind), nullable(is_nullable) {}

    TypeNode(const TypeNode&) = delete;
    TypeNode& operator=(const TypeNode&) = delete;
    TypeNode(TypeNode&&) noexcept = default;
    TypeNode& operator=(TypeNode&& other) noexcept {
        if (this != &other) {
            clear_items();
            kind = other.kind;
            nullable = other.nullable;
            minimum = other.minimum;
            maximum = other.maximum;
            source_id = std::move(other.source_id);
            items = std::move(other.items);
        }
        return *this;
    }
    ~TypeNode() { clear_items(); }

    TypeKind kind;
    bool nullable;
    double minimum{0.0};
    double maximum{0.0};
    std::string source_id;
    std::unique_ptr<TypeNode> items;

private:
    void clear_items() noexcept {
        std::unique_ptr<TypeNode> current = std::move(items);
        while (current != nullptr) {
            std::unique_ptr<TypeNode> next = std::move(current->items);
            current.reset();
            current = std::move(next);
        }
    }
};

struct Entry final {
    Entry(std::string entry_id,
          Cardinality entry_cardinality,
          TypeNode entry_type)
        : id(std::move(entry_id)),
          cardinality(entry_cardinality),
          type(std::move(entry_type)) {}

    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&&) noexcept = default;
    Entry& operator=(Entry&&) noexcept = default;
    ~Entry() = default;

    std::string id;
    Cardinality cardinality;
    TypeNode type;
};

struct Field final {
    Field(std::string field_id, TypeNode field_type)
        : id(std::move(field_id)), type(std::move(field_type)) {}

    Field(const Field&) = delete;
    Field& operator=(const Field&) = delete;
    Field(Field&&) noexcept = default;
    Field& operator=(Field&&) noexcept = default;
    ~Field() = default;

    std::string id;
    TypeNode type;
};

struct Component final {
    Component(std::string component_id, std::vector<Field> component_fields)
        : id(std::move(component_id)),
          fields(std::move(component_fields)) {}

    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) noexcept = default;
    Component& operator=(Component&&) noexcept = default;
    ~Component() = default;

    std::string id;
    std::vector<Field> fields;
};

struct SourceSpec final {
    SourceSpec(Profile source_profile,
               std::vector<Entry> source_entries,
               std::vector<Component> source_components)
        : profile(source_profile),
          entries(std::move(source_entries)),
          components(std::move(source_components)) {}

    SourceSpec(const SourceSpec&) = delete;
    SourceSpec& operator=(const SourceSpec&) = delete;
    SourceSpec(SourceSpec&&) noexcept = default;
    SourceSpec& operator=(SourceSpec&&) noexcept = default;
    ~SourceSpec() = default;

    Profile profile;
    std::vector<Entry> entries;
    std::vector<Component> components;
};

}  // namespace fastdb::payload::spec
