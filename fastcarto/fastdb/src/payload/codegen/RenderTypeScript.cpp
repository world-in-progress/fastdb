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

ScalarGetter typescript_scalar_getter(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return {"boolean", "getBool"};
    case TypeKind::u8:
        return {"number", "getU8"};
    case TypeKind::u16:
        return {"number", "getU16"};
    case TypeKind::u32:
        return {"number", "getU32"};
    case TypeKind::i32:
        return {"number", "getI32"};
    case TypeKind::u8n:
        return {"number", "getU8n"};
    case TypeKind::u16n:
        return {"number", "getU16n"};
    case TypeKind::f32:
        return {"number", "getF32"};
    case TypeKind::f64:
        return {"number", "getF64"};
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
    output += "// target: typescript\n";
}

void append_metadata(std::string& output,
                     const spec::ResolvedSpec& resolved) {
    output +=
        "export interface IdMetadata {\n"
        "  readonly symbol: string;\n"
        "  readonly originalId: string;\n"
        "  readonly stableIndex: number;\n"
        "}\n\n"
        "export const ENTRIES: readonly IdMetadata[] = Object.freeze([\n";
    for (const Entry& entry : resolved.entries()) {
        const std::string symbol =
            project_identifier(Target::typescript, entry.id) +
            "_entry_index";
        output += "  { symbol: '";
        output += symbol;
        output += "', originalId: '";
        output += entry.id;
        output += "', stableIndex: ";
        output += std::to_string(entry.index);
        output += " },\n";
    }
    output +=
        "]);\n\n"
        "export const COMPONENTS: readonly IdMetadata[] = Object.freeze([\n";
    for (const Component& component : resolved.components()) {
        const std::string symbol =
            project_identifier(Target::typescript, component.id) +
            "_component_index";
        output += "  { symbol: '";
        output += symbol;
        output += "', originalId: '";
        output += component.id;
        output += "', stableIndex: ";
        output += std::to_string(component.index);
        output += " },\n";
    }
    output +=
        "]);\n\n"
        "export interface FieldMetadata {\n"
        "  readonly symbol: string;\n"
        "  readonly originalId: string;\n"
        "  readonly componentIndex: number;\n"
        "  readonly fieldIndex: number;\n"
        "}\n\n"
        "export const FIELDS: readonly FieldMetadata[] = Object.freeze([\n";
    for (const Component& component : resolved.components()) {
        for (const Field& field : component.fields) {
            const std::string symbol =
                project_identifier(Target::typescript, field.id) +
                "_field_index";
            output += "  { symbol: '";
            output += symbol;
            output += "', originalId: '";
            output += field.id;
            output += "', componentIndex: ";
            output += std::to_string(component.index);
            output += ", fieldIndex: ";
            output += std::to_string(field.index);
            output += " },\n";
        }
    }
    output += "]);\n\n";
}

void append_entry(std::string& output,
                  const Entry& entry,
                  spec::Profile profile) {
    const std::string symbol =
        project_identifier(Target::typescript, entry.id);
    const std::string type = "FdbTsEntry_" + symbol + "_Sequence";
    output += "export const ";
    output += symbol;
    output += "_entry_index = ";
    output += std::to_string(entry.index);
    output += " as const;\n\nexport class ";
    output += type;
    output +=
        " {\n"
        "  constructor(private readonly view: View) {}\n\n"
        "  genericView(): View { return this.view.clone(); }\n"
        "  length(): bigint { return this.view.length(); }\n"
        "  at(index: bigint): View { return this.view.at(index); }\n";
    if (entry.type.kind == TypeKind::ref) {
        output +=
            "  atRefTarget(index: bigint): View { return this.at(index).refTarget(); }\n";
    }
    if (profile == spec::Profile::object_graph_v1 &&
        entry.type.kind == TypeKind::component) {
        output +=
            "  atGraphIdentity(index: bigint): GraphIdentity { return this.at(index).graphIdentity(); }\n";
    }
    output += "  materialize(): ";
    output += type;
    output += " { return new ";
    output += type;
    output +=
        "(this.view.materialize()); }\n"
        "  dispose(): void { this.view.dispose(); }\n"
        "}\n\nexport function ";
    output += symbol;
    output += "_from_payload(payload: Payload): ";
    output += type;
    output += " {\n  return new ";
    output += type;
    output += "(payload.entryView(";
    output += symbol;
    output +=
        "_entry_index));\n"
        "}\n\nexport function ";
    output += symbol;
    output +=
        "_builder_entry_begin(builder: Builder, valueCount: bigint): Builder {\n"
        "  return builder.entryBegin(";
    output += symbol;
    output += "_entry_index, valueCount);\n}\n\n";
}

void append_component(std::string& output,
                      const Component& component,
                      bool is_identity) {
    const std::string symbol =
        project_identifier(Target::typescript, component.id);
    const std::string type =
        project_type_identifier(Target::typescript, component.id);
    output += "export const ";
    output += symbol;
    output += "_component_index = ";
    output += std::to_string(component.index);
    output += " as const;\n\nexport class ";
    output += type;
    output +=
        " {\n"
        "  private constructor(private readonly view: View, token: symbol) {\n"
        "    if (token !== fastdbComponentViewToken) {\n"
        "      throw new TypeError('FastDB generated component views require checked construction');\n"
        "    }\n"
        "  }\n\n"
        "  static tryFromView(view: View): ";
    output += type;
    output +=
        " | undefined {\n"
        "    if (view.componentIndex() !== ";
    output += symbol;
    output +=
        "_component_index) { return undefined; }\n"
        "    return new ";
    output += type;
    output +=
        "(view, fastdbComponentViewToken);\n"
        "  }\n\n"
        "  genericView(): View { return this.view.clone(); }\n";
    if (is_identity) {
        output +=
            "  graphIdentity(): GraphIdentity { return this.view.graphIdentity(); }\n";
    }
    output += "  materialize(): ";
    output += type;
    output += " { return new ";
    output += type;
    output +=
        "(this.view.materialize(), fastdbComponentViewToken); }\n"
        "  dispose(): void { this.view.dispose(); }\n\n";
    for (const Field& field : component.fields) {
        const std::string field_symbol =
            project_identifier(Target::typescript, field.id);
        output += "  static readonly ";
        output += field_symbol;
        output += "_field_index = ";
        output += std::to_string(field.index);
        output += " as const;\n";
        output += "  ";
        output += field_symbol;
        output += "(): View { return this.view.field(";
        output += type;
        output += ".";
        output += field_symbol;
        output += "_field_index); }\n";
        const ScalarGetter scalar =
            typescript_scalar_getter(field.type.kind);
        if (!scalar.type.empty()) {
            output += "  ";
            output += field_symbol;
            output += "_value(): ";
            output += scalar.type;
            output += " { return this.";
            output += field_symbol;
            output += "().";
            output += scalar.method;
            output += "(); }\n";
        }
        if (field.type.kind == TypeKind::ref) {
            output += "  ";
            output += field_symbol;
            output += "_refTarget(): View { return this.";
            output += field_symbol;
            output += "().refTarget(); }\n";
        }
    }
    output += "}\n\n";
    if (is_identity) {
        output += "export function ";
        output += symbol;
        output +=
            "_builder_declare(builder: Builder): ObjectHandle {\n"
            "  return builder.declareObject(";
        output += symbol;
        output += "_component_index);\n}\n\n";
    }
}

}  // namespace

std::string render_typescript(const spec::CompiledSpec& compiled,
                              const spec::RuntimeTopology& topology) {
    const std::string digest =
        identity::sha256_lower_hex(compiled.digest());
    const spec::ResolvedSpec& resolved = compiled.resolved();
    std::string output;
    output.reserve(compiled.canonical_bytes().size() + 4096U);
    append_provenance(output, digest);
    const bool has_entries = !resolved.entries().empty();
    const bool has_components = !resolved.components().empty();
    const bool has_identity = topology.has_objects;
    output += "import {\n";
    if (has_entries || has_identity) {
        output += "  Builder,\n";
    }
    output += "  CompiledSpec,\n";
    if (has_identity) {
        output +=
            "  GraphIdentity,\n"
            "  ObjectHandle,\n";
    }
    if (has_entries) {
        output += "  Payload,\n";
    }
    if (has_entries || has_components) {
        output += "  View,\n";
    }
    output += "} from 'fastdb4ts/payload';\n\n";
    if (!resolved.components().empty()) {
        output +=
            "const fastdbComponentViewToken = Symbol('fastdb.payload.codegen.component-view');\n\n";
    }
    output +=
        "export const CANONICAL_SOURCE_TEXT = ";
    output.push_back(static_cast<char>(0x60));
    output += compiled.canonical_bytes();
    output.push_back(static_cast<char>(0x60));
    output +=
        ";\n"
        "export const CANONICAL_SOURCE = new TextEncoder().encode(CANONICAL_SOURCE_TEXT);\n"
        "export const PAYLOAD_SHA256 = '";
    output += digest;
    output +=
        "' as const;\n\n"
        "export function compileSpec(): CompiledSpec {\n"
        "  return CompiledSpec.compile(CANONICAL_SOURCE);\n"
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
