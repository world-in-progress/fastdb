#include "payload/codegen/Renderer.hpp"

#include "payload/codegen/Identifier.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/spec/Model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fastdb::payload::codegen {
namespace {

using spec::Component;
using spec::Entry;
using spec::Field;
using spec::TypeKind;

struct ScalarGetter final {
    std::string_view type;
    std::string_view method;
};

ScalarGetter cpp_scalar_getter(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return {"bool", "get_bool"};
    case TypeKind::u8:
        return {"std::uint8_t", "get_u8"};
    case TypeKind::u16:
        return {"std::uint16_t", "get_u16"};
    case TypeKind::u32:
        return {"std::uint32_t", "get_u32"};
    case TypeKind::i32:
        return {"std::int32_t", "get_i32"};
    case TypeKind::u8n:
        return {"double", "get_u8n"};
    case TypeKind::u16n:
        return {"double", "get_u16n"};
    case TypeKind::f32:
        return {"float", "get_f32"};
    case TypeKind::f64:
        return {"double", "get_f64"};
    case TypeKind::str:
    case TypeKind::wstr:
    case TypeKind::bytes:
    case TypeKind::component:
    case TypeKind::ref:
    case TypeKind::list:
        return {};
    }
    return {};
}

void append_provenance(std::string& output,
                       std::string_view digest) {
    output += "// generated-by: fastdb.payload.codegen.v1\n";
    output += "// payload-sha256: ";
    output += digest;
    output += "\n// core-abi-version: 1\n";
    output += "// generator-version: fastdb.payload.codegen.v1\n";
    output += "// target: cpp\n";
}

void append_id_metadata(std::string& output,
                        const spec::ResolvedSpec& resolved) {
    output +=
        "struct IdMetadata final {\n"
        "    std::string_view symbol;\n"
        "    std::string_view original_id;\n"
        "    std::uint32_t stable_index;\n"
        "};\n\n";
    output += "inline constexpr std::array<IdMetadata, ";
    output += std::to_string(resolved.entries().size());
    output += "> entries{{\n";
    for (const Entry& entry : resolved.entries()) {
        const std::string symbol =
            project_identifier(Target::cpp, entry.id) + "_entry_index";
        output += "    IdMetadata{\"";
        output += symbol;
        output += "\", \"";
        output += entry.id;
        output += "\", ";
        output += std::to_string(entry.index);
        output += "U},\n";
    }
    output += "}};\n\ninline constexpr std::array<IdMetadata, ";
    output += std::to_string(resolved.components().size());
    output += "> components{{\n";
    for (const Component& component : resolved.components()) {
        const std::string symbol =
            project_identifier(Target::cpp, component.id) +
            "_component_index";
        output += "    IdMetadata{\"";
        output += symbol;
        output += "\", \"";
        output += component.id;
        output += "\", ";
        output += std::to_string(component.index);
        output += "U},\n";
    }
    output +=
        "}};\n\n"
        "struct FieldMetadata final {\n"
        "    std::string_view symbol;\n"
        "    std::string_view original_id;\n"
        "    std::uint32_t component_index;\n"
        "    std::uint32_t field_index;\n"
        "};\n\n";

    std::size_t field_count = 0U;
    for (const Component& component : resolved.components()) {
        field_count += component.fields.size();
    }
    output += "inline constexpr std::array<FieldMetadata, ";
    output += std::to_string(field_count);
    output += "> fields{{\n";
    for (const Component& component : resolved.components()) {
        for (const Field& field : component.fields) {
            const std::string symbol =
                project_identifier(Target::cpp, field.id) + "_field_index";
            output += "    FieldMetadata{\"";
            output += symbol;
            output += "\", \"";
            output += field.id;
            output += "\", ";
            output += std::to_string(component.index);
            output += "U, ";
            output += std::to_string(field.index);
            output += "U},\n";
        }
    }
    output += "}};\n\n";
}

void append_entry(std::string& output,
                  const Entry& entry,
                  spec::Profile profile) {
    const std::string symbol = project_identifier(Target::cpp, entry.id);
    const std::string type = "FdbCppEntry_" + symbol + "_Sequence";
    output += "inline constexpr std::uint32_t ";
    output += symbol;
    output += "_entry_index = ";
    output += std::to_string(entry.index);
    output += "U;\n\nclass ";
    output += type;
    output +=
        " final {\n"
        "public:\n"
        "    explicit ";
    output += type;
    output +=
        "(fastdb::payload::v1::View view) : view_(std::move(view)) {}\n\n"
        "    fastdb::payload::v1::View generic_view() const { return view_; }\n"
        "    std::uint64_t size() const { return view_.length(); }\n"
        "    fastdb::payload::v1::View at(std::uint64_t index) const {\n"
        "        return view_.at(index);\n"
        "    }\n";
    if (entry.type.kind == TypeKind::ref) {
        output +=
            "    fastdb::payload::v1::View at_ref_target(std::uint64_t index) const {\n"
            "        return at(index).ref_target();\n"
            "    }\n";
    }
    if (profile == spec::Profile::object_graph_v1 &&
        entry.type.kind == TypeKind::component) {
        output +=
            "    fastdb::payload::v1::GraphIdentity at_graph_identity(\n"
            "        std::uint64_t index) const {\n"
            "        return at(index).graph_identity();\n"
            "    }\n";
    }
    output += "    ";
    output += type;
    output +=
        " materialize() const {\n"
        "        return ";
    output += type;
    output +=
        "(view_.materialize());\n"
        "    }\n\n"
        "private:\n"
        "    fastdb::payload::v1::View view_;\n"
        "};\n\ninline ";
    output += type;
    output += " ";
    output += symbol;
    output +=
        "_from_payload(const fastdb::payload::v1::Payload& payload) {\n"
        "    return ";
    output += type;
    output += "(payload.entry_view(";
    output += symbol;
    output +=
        "_entry_index));\n"
        "}\n\ninline fastdb::payload::v1::Builder& ";
    output += symbol;
    output +=
        "_builder_entry_begin(fastdb::payload::v1::Builder& builder,\n"
        "                     std::uint64_t value_count) {\n"
        "    return builder.entry_begin(";
    output += symbol;
    output +=
        "_entry_index, value_count);\n"
        "}\n\n";
}

void append_component(std::string& output,
                      const Component& component,
                      bool is_identity) {
    const std::string symbol =
        project_identifier(Target::cpp, component.id);
    const std::string type =
        project_type_identifier(Target::cpp, component.id);
    output += "inline constexpr std::uint32_t ";
    output += symbol;
    output += "_component_index = ";
    output += std::to_string(component.index);
    output += "U;\n\nclass ";
    output += type;
    output +=
        " final {\n"
        "public:\n"
        "    static std::optional<";
    output += type;
    output +=
        "> try_from_view(fastdb::payload::v1::View view);\n\n"
        "    fastdb::payload::v1::View generic_view() const { return view_; }\n";
    if (is_identity) {
        output +=
            "    fastdb::payload::v1::GraphIdentity graph_identity() const {\n"
            "        return view_.graph_identity();\n"
            "    }\n";
    }
    output += "    ";
    output += type;
    output +=
        " materialize() const {\n"
        "        return ";
    output += type;
    output +=
        "(view_.materialize());\n"
        "    }\n\n";

    for (const Field& field : component.fields) {
        const std::string field_symbol =
            project_identifier(Target::cpp, field.id);
        output += "    static constexpr std::uint32_t ";
        output += field_symbol;
        output += "_field_index = ";
        output += std::to_string(field.index);
        output += "U;\n";
        output += "    fastdb::payload::v1::View ";
        output += field_symbol;
        output += "() const { return view_.field(";
        output += field_symbol;
        output += "_field_index); }\n";

        const ScalarGetter scalar = cpp_scalar_getter(field.type.kind);
        if (!scalar.type.empty()) {
            output += "    ";
            output += scalar.type;
            output += " ";
            output += field_symbol;
            output += "_value() const { return ";
            output += field_symbol;
            output += "().";
            output += scalar.method;
            output += "(); }\n";
        }
        if (field.type.kind == TypeKind::ref) {
            output += "    fastdb::payload::v1::View ";
            output += field_symbol;
            output += "_ref_target() const { return ";
            output += field_symbol;
            output += "().ref_target(); }\n";
        }
    }
    output +=
        "\nprivate:\n"
        "    explicit ";
    output += type;
    output +=
        "(fastdb::payload::v1::View view) : view_(std::move(view)) {}\n\n"
        "    fastdb::payload::v1::View view_;\n"
        "};\n\n";
    output += "inline std::optional<";
    output += type;
    output += "> ";
    output += type;
    output +=
        "::try_from_view(fastdb::payload::v1::View view) {\n"
        "    if (view.component_index() != ";
    output += symbol;
    output +=
        "_component_index) {\n"
        "        return std::nullopt;\n"
        "    }\n"
        "    return ";
    output += type;
    output += "(std::move(view));\n}\n\n";
    if (is_identity) {
        output += "inline fastdb::payload::v1::ObjectHandle ";
        output += symbol;
        output +=
            "_builder_declare(fastdb::payload::v1::Builder& builder) {\n"
            "    return builder.declare_object(";
        output += symbol;
        output += "_component_index);\n}\n\n";
    }
}

}  // namespace

std::string render_cpp(const spec::CompiledSpec& compiled,
                       const spec::RuntimeTopology& topology) {
    const std::string digest =
        identity::sha256_lower_hex(compiled.digest());
    const spec::ResolvedSpec& resolved = compiled.resolved();

    std::string output;
    output.reserve(compiled.canonical_bytes().size() + 4096U);
    append_provenance(output, digest);
    output +=
        "#pragma once\n\n"
        "#include <fastdb_payload.hpp>\n\n"
        "#include <array>\n"
        "#include <cstdint>\n";
    if (!resolved.components().empty()) {
        output += "#include <optional>\n";
    }
    output +=
        "#include <string_view>\n"
        "#include <utility>\n\n"
        "namespace fastdb_payload_";
    output += digest;
    output +=
        " {\n\n"
        "inline constexpr std::string_view canonical_source =\n"
        "    R\"FDB_PAYLOAD(";
    output += compiled.canonical_bytes();
    output +=
        ")FDB_PAYLOAD\";\n"
        "inline constexpr std::string_view payload_sha256 = \"";
    output += digest;
    output +=
        "\";\n\n"
        "inline fastdb::payload::v1::CompiledSpec compile_spec() {\n"
        "    return fastdb::payload::v1::CompiledSpec::compile(canonical_source);\n"
        "}\n\n";

    append_id_metadata(output, resolved);
    for (const Entry& entry : resolved.entries()) {
        append_entry(output, entry, resolved.profile());
    }
    for (std::size_t index = 0U; index < resolved.components().size();
         ++index) {
        append_component(output, resolved.components()[index],
                         topology.identity_components[index] != UINT8_C(0));
    }
    output += "}  // namespace fastdb_payload_";
    output += digest;
    output += "\n";
    return output;
}

}  // namespace fastdb::payload::codegen
