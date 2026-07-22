#include "payload/codegen/Renderer.hpp"

#include "payload/codegen/Generator.hpp"
#include "payload/codegen/Identifier.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/spec/Model.hpp"

#include <cstddef>
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

ScalarGetter rust_scalar_getter(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return {"bool", "get_bool"};
    case TypeKind::u8:
        return {"u8", "get_u8"};
    case TypeKind::u16:
        return {"u16", "get_u16"};
    case TypeKind::u32:
        return {"u32", "get_u32"};
    case TypeKind::i32:
        return {"i32", "get_i32"};
    case TypeKind::u8n:
        return {"f64", "get_u8n"};
    case TypeKind::u16n:
        return {"f64", "get_u16n"};
    case TypeKind::f32:
        return {"f32", "get_f32"};
    case TypeKind::f64:
        return {"f64", "get_f64"};
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
    output += "// generated-by: ";
    output += generator_version;
    output += "\n";
    output += "// payload-sha256: ";
    output += digest;
    output += "\n// core-abi-version: ";
    output += std::to_string(generator_core_abi_version);
    output += "\n// generator-version: ";
    output += generator_version;
    output += "\n";
    output += "// target: rust\n";
}

void append_metadata(std::string& output,
                     const spec::ResolvedSpec& resolved) {
    output +=
        "#[derive(Clone, Copy, Debug, Eq, PartialEq)]\n"
        "pub struct IdMetadata {\n"
        "    pub symbol: &'static str,\n"
        "    pub original_id: &'static str,\n"
        "    pub stable_index: u32,\n"
        "}\n\n"
        "pub const ENTRIES: &[IdMetadata] = &[\n";
    for (const Entry& entry : resolved.entries()) {
        const std::string symbol =
            project_identifier(Target::rust, entry.id) + "_entry_index";
        output += "    IdMetadata { symbol: \"";
        output += symbol;
        output += "\", original_id: \"";
        output += entry.id;
        output += "\", stable_index: ";
        output += std::to_string(entry.index);
        output += " },\n";
    }
    output += "];\n\npub const COMPONENTS: &[IdMetadata] = &[\n";
    for (const Component& component : resolved.components()) {
        const std::string symbol =
            project_identifier(Target::rust, component.id) +
            "_component_index";
        output += "    IdMetadata { symbol: \"";
        output += symbol;
        output += "\", original_id: \"";
        output += component.id;
        output += "\", stable_index: ";
        output += std::to_string(component.index);
        output += " },\n";
    }
    output +=
        "];\n\n"
        "#[derive(Clone, Copy, Debug, Eq, PartialEq)]\n"
        "pub struct FieldMetadata {\n"
        "    pub symbol: &'static str,\n"
        "    pub original_id: &'static str,\n"
        "    pub component_index: u32,\n"
        "    pub field_index: u32,\n"
        "}\n\n"
        "pub const FIELDS: &[FieldMetadata] = &[\n";
    for (const Component& component : resolved.components()) {
        for (const Field& field : component.fields) {
            const std::string symbol =
                project_identifier(Target::rust, field.id) + "_field_index";
            output += "    FieldMetadata { symbol: \"";
            output += symbol;
            output += "\", original_id: \"";
            output += field.id;
            output += "\", component_index: ";
            output += std::to_string(component.index);
            output += ", field_index: ";
            output += std::to_string(field.index);
            output += " },\n";
        }
    }
    output += "];\n\n";
}

void append_entry(std::string& output,
                  const Entry& entry,
                  spec::Profile profile) {
    const std::string symbol =
        project_identifier(Target::rust, entry.id);
    const std::string type = "FdbRustEntry_" + symbol + "_Sequence";
    output += "pub const ";
    output += symbol;
    output += "_entry_index: u32 = ";
    output += std::to_string(entry.index);
    output += ";\n\n#[derive(Clone)]\npub struct ";
    output += type;
    output +=
        " {\n"
        "    view: fastdb::View,\n"
        "}\n\nimpl ";
    output += type;
    output +=
        " {\n"
        "    fn new(view: fastdb::View) -> Self { Self { view } }\n"
        "    pub fn generic_view(&self) -> fastdb::View { self.view.clone() }\n"
        "    pub fn len(&self) -> Result<u64, fastdb::PayloadError> {\n"
        "        self.view.length()\n"
        "    }\n"
        "    pub fn at(&self, index: u64) -> Result<fastdb::View, fastdb::PayloadError> {\n"
        "        self.view.at(index)\n"
        "    }\n";
    if (entry.type.kind == TypeKind::ref) {
        output +=
            "    pub fn at_ref_target(&self, index: u64) -> Result<fastdb::View, fastdb::PayloadError> {\n"
            "        self.at(index)?.ref_target()\n"
            "    }\n";
    }
    if (profile == spec::Profile::object_graph_v1 &&
        entry.type.kind == TypeKind::component) {
        output +=
            "    pub fn at_graph_identity(&self, index: u64) -> Result<fastdb::GraphIdentity, fastdb::PayloadError> {\n"
            "        self.at(index)?.graph_identity()\n"
            "    }\n";
    }
    output +=
        "    pub fn materialize(&self) -> Result<Self, fastdb::PayloadError> {\n"
        "        Ok(Self::new(self.view.materialize()?))\n"
        "    }\n"
        "}\n\npub fn ";
    output += symbol;
    output += "_from_payload(payload: &fastdb::Payload) -> Result<";
    output += type;
    output +=
        ", fastdb::PayloadError> {\n"
        "    Ok(";
    output += type;
    output += "::new(payload.entry_view(";
    output += symbol;
    output +=
        "_entry_index)?))\n"
        "}\n\npub fn ";
    output += symbol;
    output +=
        "_builder_entry_begin<'a>(\n"
        "    builder: &'a mut fastdb::Builder,\n"
        "    value_count: u64,\n"
        ") -> Result<&'a mut fastdb::Builder, fastdb::PayloadError> {\n"
        "    builder.entry_begin(";
    output += symbol;
    output += "_entry_index, value_count)\n}\n\n";
}

void append_component(std::string& output,
                      const Component& component,
                      bool is_identity) {
    const std::string symbol =
        project_identifier(Target::rust, component.id);
    const std::string type =
        project_type_identifier(Target::rust, component.id);
    output += "pub const ";
    output += symbol;
    output += "_component_index: u32 = ";
    output += std::to_string(component.index);
    output += ";\n\n#[derive(Clone)]\npub struct ";
    output += type;
    output +=
        " {\n"
        "    view: fastdb::View,\n"
        "}\n\nimpl ";
    output += type;
    output +=
        " {\n"
        "    pub fn try_from_view(view: fastdb::View) -> Result<Option<Self>, fastdb::PayloadError> {\n"
        "        if view.component_index()? != ";
    output += symbol;
    output +=
        "_component_index {\n"
        "            return Ok(None);\n"
        "        }\n"
        "        Ok(Some(Self { view }))\n"
        "    }\n"
        "    pub fn generic_view(&self) -> fastdb::View { self.view.clone() }\n";
    if (is_identity) {
        output +=
            "    pub fn graph_identity(&self) -> Result<fastdb::GraphIdentity, fastdb::PayloadError> {\n"
            "        self.view.graph_identity()\n"
            "    }\n";
    }
    output +=
        "    pub fn materialize(&self) -> Result<Self, fastdb::PayloadError> {\n"
        "        Ok(Self { view: self.view.materialize()? })\n"
        "    }\n";
    for (const Field& field : component.fields) {
        const std::string field_symbol =
            project_identifier(Target::rust, field.id);
        output += "    pub const ";
        output += field_symbol;
        output += "_field_index: u32 = ";
        output += std::to_string(field.index);
        output += ";\n";
        output += "    pub fn ";
        output += field_symbol;
        output += "(&self) -> Result<fastdb::View, fastdb::PayloadError> {\n";
        output += "        self.view.field(Self::";
        output += field_symbol;
        output += "_field_index)\n    }\n";
        const ScalarGetter scalar = rust_scalar_getter(field.type.kind);
        if (!scalar.type.empty()) {
            output += "    pub fn ";
            output += field_symbol;
            output += "_value(&self) -> Result<";
            output += scalar.type;
            output += ", fastdb::PayloadError> {\n        self.";
            output += field_symbol;
            output += "()?.";
            output += scalar.method;
            output += "()\n    }\n";
        }
        if (field.type.kind == TypeKind::ref) {
            output += "    pub fn ";
            output += field_symbol;
            output += "_ref_target(&self) -> Result<fastdb::View, fastdb::PayloadError> {\n";
            output += "        self.";
            output += field_symbol;
            output += "()?.ref_target()\n    }\n";
        }
    }
    output += "}\n\n";
    if (is_identity) {
        output += "pub fn ";
        output += symbol;
        output +=
            "_builder_declare(\n"
            "    builder: &mut fastdb::Builder,\n"
            ") -> Result<fastdb::ObjectHandle, fastdb::PayloadError> {\n"
            "    builder.declare_object(";
        output += symbol;
        output += "_component_index)\n}\n\n";
    }
}

}  // namespace

std::string render_rust(const spec::CompiledSpec& compiled,
                        const spec::RuntimeTopology& topology) {
    const std::string digest =
        identity::sha256_lower_hex(compiled.digest());
    const spec::ResolvedSpec& resolved = compiled.resolved();
    std::string output;
    output.reserve(compiled.canonical_bytes().size() + 4096U);
    append_provenance(output, digest);
    output +=
        "#![allow(non_camel_case_types, non_upper_case_globals)]\n\n"
        "pub const CANONICAL_SOURCE: &[u8] = br#\"";
    output += compiled.canonical_bytes();
    output += "\"#;\npub const PAYLOAD_SHA256: &str = \"";
    output += digest;
    output +=
        "\";\n\npub fn compile_spec() -> Result<fastdb::CompiledSpec, fastdb::PayloadError> {\n"
        "    fastdb::CompiledSpec::compile(CANONICAL_SOURCE)\n"
        "}\n\n";
    append_metadata(output, resolved);
    for (const Entry& entry : resolved.entries()) {
        append_entry(output, entry, resolved.profile());
    }
    for (std::size_t index = 0U; index < resolved.components().size();
         ++index) {
        append_component(output, resolved.components()[index],
                         topology.identity_components[index] != UINT8_C(0));
    }
    if (output.size() >= 2U &&
        output.compare(output.size() - 2U, 2U, "\n\n") == 0) {
        output.pop_back();
    }
    return output;
}

}  // namespace fastdb::payload::codegen
