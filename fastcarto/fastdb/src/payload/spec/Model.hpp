#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fastdb::payload::error {

template <typename T>
class Result;

}  // namespace fastdb::payload::error

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
    static constexpr std::uint32_t unresolved_component_index =
        std::numeric_limits<std::uint32_t>::max();

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
            resolved_component_index = other.resolved_component_index;
            variable_width = other.variable_width;
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
    std::uint32_t resolved_component_index{unresolved_component_index};
    bool variable_width{false};
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
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t source_index{std::numeric_limits<std::uint32_t>::max()};
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
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t source_index{std::numeric_limits<std::uint32_t>::max()};
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
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t source_index{std::numeric_limits<std::uint32_t>::max()};
    bool variable_width{false};
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

struct SemanticFacts final {
    bool has_nullable{false};
    bool has_lists{false};
    bool has_references{false};
    bool has_variable_width{false};
    bool has_normalized_integers{false};
    std::uint64_t semantic_flags{UINT64_C(0)};
};

class ResolvedSpec;

error::Result<ResolvedSpec> resolve_source(SourceSpec source);

class ResolvedSpec final {
public:
    ResolvedSpec(const ResolvedSpec&) = delete;
    ResolvedSpec& operator=(const ResolvedSpec&) = delete;
    ResolvedSpec(ResolvedSpec&&) noexcept = default;
    ResolvedSpec& operator=(ResolvedSpec&&) noexcept = default;
    ~ResolvedSpec() = default;

    Profile profile() const noexcept { return source_.profile; }
    const std::vector<Entry>& entries() const noexcept {
        return source_.entries;
    }
    const std::vector<Component>& components() const noexcept {
        return source_.components;
    }
    const SemanticFacts& facts() const noexcept { return facts_; }
    const SourceSpec& source() const noexcept { return source_; }

private:
    friend error::Result<ResolvedSpec> resolve_source(SourceSpec source);

    ResolvedSpec(SourceSpec source, SemanticFacts facts)
        : source_(std::move(source)), facts_(facts) {}

    SourceSpec source_;
    SemanticFacts facts_;
};

}  // namespace fastdb::payload::spec
