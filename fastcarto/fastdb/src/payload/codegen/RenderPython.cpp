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

ScalarGetter python_scalar_getter(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return {"bool", "get_bool"};
    case TypeKind::u8:
    case TypeKind::u16:
    case TypeKind::u32:
    case TypeKind::i32:
        return {"int", kind == TypeKind::u8
                           ? "get_u8"
                           : kind == TypeKind::u16
                                 ? "get_u16"
                                 : kind == TypeKind::u32 ? "get_u32"
                                                        : "get_i32"};
    case TypeKind::u8n:
        return {"float", "get_u8n"};
    case TypeKind::u16n:
        return {"float", "get_u16n"};
    case TypeKind::f32:
        return {"float", "get_f32"};
    case TypeKind::f64:
        return {"float", "get_f64"};
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
    output += "# generated-by: ";
    output += generator_version;
    output += "\n";
    output += "# payload-sha256: ";
    output += digest;
    output += "\n# core-abi-version: ";
    output += std::to_string(generator_core_abi_version);
    output += "\n# generator-version: ";
    output += generator_version;
    output += "\n";
    output += "# target: python\n";
}

void append_metadata(std::string& output,
                     const spec::ResolvedSpec& resolved) {
    output +=
        "@dataclass(frozen=True)\n"
        "class IdMetadata:\n"
        "    symbol: str\n"
        "    original_id: str\n"
        "    stable_index: int\n\n"
        "ENTRIES: tuple[IdMetadata, ...] = (\n";
    for (const Entry& entry : resolved.entries()) {
        const std::string symbol =
            project_identifier(Target::python, entry.id) + "_entry_index";
        output += "    IdMetadata('";
        output += symbol;
        output += "', '";
        output += entry.id;
        output += "', ";
        output += std::to_string(entry.index);
        output += "),\n";
    }
    output += ")\n\nCOMPONENTS: tuple[IdMetadata, ...] = (\n";
    for (const Component& component : resolved.components()) {
        const std::string symbol =
            project_identifier(Target::python, component.id) +
            "_component_index";
        output += "    IdMetadata('";
        output += symbol;
        output += "', '";
        output += component.id;
        output += "', ";
        output += std::to_string(component.index);
        output += "),\n";
    }
    output +=
        ")\n\n"
        "@dataclass(frozen=True)\n"
        "class FieldMetadata:\n"
        "    symbol: str\n"
        "    original_id: str\n"
        "    component_index: int\n"
        "    field_index: int\n\n"
        "FIELDS: tuple[FieldMetadata, ...] = (\n";
    for (const Component& component : resolved.components()) {
        for (const Field& field : component.fields) {
            const std::string symbol =
                project_identifier(Target::python, field.id) +
                "_field_index";
            output += "    FieldMetadata('";
            output += symbol;
            output += "', '";
            output += field.id;
            output += "', ";
            output += std::to_string(component.index);
            output += ", ";
            output += std::to_string(field.index);
            output += "),\n";
        }
    }
    output += ")\n\n";
}

void append_entry(std::string& output,
                  const Entry& entry,
                  spec::Profile profile) {
    const std::string symbol =
        project_identifier(Target::python, entry.id);
    const std::string type = "FdbPythonEntry_" + symbol + "_Sequence";
    output += symbol;
    output += "_entry_index = ";
    output += std::to_string(entry.index);
    output += "\n\nclass ";
    output += type;
    output +=
        ":\n"
        "    def __init__(self, view: View) -> None:\n"
        "        self._view = view\n\n"
        "    def generic_view(self) -> View:\n"
        "        return self._view.clone()\n\n"
        "    def __len__(self) -> int:\n"
        "        return self._view.length()\n\n"
        "    def at(self, index: int) -> View:\n"
        "        return self._view.at(index)\n\n";
    if (entry.type.kind == TypeKind::ref) {
        output +=
            "    def at_ref_target(self, index: int) -> View:\n"
            "        return self.at(index).ref_target()\n\n";
    }
    if (profile == spec::Profile::object_graph_v1 &&
        entry.type.kind == TypeKind::component) {
        output +=
            "    def at_graph_identity(self, index: int) -> GraphIdentity:\n"
            "        return self.at(index).graph_identity()\n\n";
    }
    output += "    def materialize(self) -> '";
    output += type;
    output +=
        "':\n"
        "        return type(self)(self._view.materialize())\n\n"
        "    def close(self) -> None:\n"
        "        self._view.close()\n\n"
        "def ";
    output += symbol;
    output += "_from_payload(payload: Payload) -> ";
    output += type;
    output += ":\n    return ";
    output += type;
    output += "(payload.entry_view(";
    output += symbol;
    output +=
        "_entry_index))\n\n"
        "def ";
    output += symbol;
    output +=
        "_builder_entry_begin(builder: Builder, value_count: int) -> Builder:\n"
        "    return builder.entry_begin(";
    output += symbol;
    output += "_entry_index, value_count)\n\n";
}

void append_component(std::string& output,
                      const Component& component,
                      bool is_identity) {
    const std::string symbol =
        project_identifier(Target::python, component.id);
    const std::string type =
        project_type_identifier(Target::python, component.id);
    output += symbol;
    output += "_component_index = ";
    output += std::to_string(component.index);
    output += "\n\nclass ";
    output += type;
    output +=
        ":\n"
        "    def __init__(self, view: View, token: object) -> None:\n"
        "        if token is not _fastdb_component_view_token:\n"
        "            raise TypeError('FastDB generated component views require checked construction')\n"
        "        self._view = view\n\n"
        "    @classmethod\n"
        "    def try_from_view(cls, view: View) -> '";
    output += type;
    output +=
        " | None':\n"
        "        if view.component_index() != ";
    output += symbol;
    output +=
        "_component_index:\n"
        "            return None\n"
        "        return cls(view, _fastdb_component_view_token)\n\n"
        "    def generic_view(self) -> View:\n"
        "        return self._view.clone()\n\n";
    if (is_identity) {
        output +=
            "    def graph_identity(self) -> GraphIdentity:\n"
            "        return self._view.graph_identity()\n\n";
    }
    output += "    def materialize(self) -> '";
    output += type;
    output +=
        "':\n"
        "        return type(self)(self._view.materialize(), _fastdb_component_view_token)\n\n"
        "    def close(self) -> None:\n"
        "        self._view.close()\n\n";
    for (const Field& field : component.fields) {
        const std::string field_symbol =
            project_identifier(Target::python, field.id);
        output += "    ";
        output += field_symbol;
        output += "_field_index = ";
        output += std::to_string(field.index);
        output += "\n\n    def ";
        output += field_symbol;
        output += "(self) -> View:\n        return self._view.field(self.";
        output += field_symbol;
        output += "_field_index)\n\n";
        const ScalarGetter scalar = python_scalar_getter(field.type.kind);
        if (!scalar.type.empty()) {
            output += "    def ";
            output += field_symbol;
            output += "_value(self) -> ";
            output += scalar.type;
            output += ":\n        return self.";
            output += field_symbol;
            output += "().";
            output += scalar.method;
            output += "()\n\n";
        }
        if (field.type.kind == TypeKind::ref) {
            output += "    def ";
            output += field_symbol;
            output += "_ref_target(self) -> View:\n        return self.";
            output += field_symbol;
            output += "().ref_target()\n\n";
        }
    }
    if (is_identity) {
        output += "def ";
        output += symbol;
        output +=
            "_builder_declare(builder: Builder) -> ObjectHandle:\n"
            "    return builder.declare_object(";
        output += symbol;
        output += "_component_index)\n\n";
    }
}

}  // namespace

std::string render_python(const spec::CompiledSpec& compiled,
                          const spec::RuntimeTopology& topology) {
    const std::string digest =
        identity::sha256_lower_hex(compiled.digest());
    const spec::ResolvedSpec& resolved = compiled.resolved();
    std::string output;
    output.reserve(compiled.canonical_bytes().size() + 4096U);
    append_provenance(output, digest);
    output +=
        "from __future__ import annotations\n\n"
        "from dataclasses import dataclass\n\n"
        "from fastdb4py.payload import (\n"
        "    Builder,\n"
        "    CompiledSpec,\n"
        "    GraphIdentity,\n"
        "    ObjectHandle,\n"
        "    Payload,\n"
        "    PayloadError,\n"
        "    View,\n"
        ")\n\n";
    if (!resolved.components().empty()) {
        output += "_fastdb_component_view_token = object()\n\n";
    }
    output +=
        "CANONICAL_SOURCE = b'";
    output += compiled.canonical_bytes();
    output += "'\nPAYLOAD_SHA256 = '";
    output += digest;
    output +=
        "'\n\ndef compile_spec() -> CompiledSpec:\n"
        "    return CompiledSpec.compile(CANONICAL_SOURCE)\n\n";
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
