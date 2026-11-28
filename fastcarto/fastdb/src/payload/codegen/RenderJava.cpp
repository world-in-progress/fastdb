#include "payload/codegen/Renderer.hpp"

#include "payload/codegen/Generator.hpp"
#include "payload/codegen/Identifier.hpp"
#include "payload/identity/Sha256.hpp"
#include "payload/spec/Model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

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

ScalarGetter java_scalar_getter(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::boolean:
        return {"boolean", "getBool"};
    case TypeKind::u8:
        return {"int", "getU8"};
    case TypeKind::u16:
        return {"int", "getU16"};
    case TypeKind::u32:
        return {"long", "getU32"};
    case TypeKind::i32:
        return {"int", "getI32"};
    case TypeKind::u8n:
        return {"double", "getU8n"};
    case TypeKind::u16n:
        return {"double", "getU16n"};
    case TypeKind::f32:
        return {"float", "getF32"};
    case TypeKind::f64:
        return {"double", "getF64"};
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

void append_header(CheckedOutput& output, std::string_view digest,
                   const spec::ResolvedSpec& resolved,
                   const spec::RuntimeTopology& topology) {
    output += "// generated-by: ";
    output += generator_version;
    output += "\n// payload-sha256: ";
    output += digest;
    output += "\n// core-abi-version: ";
    output += std::to_string(generator_core_abi_version);
    output += "\n// generator-version: ";
    output += generator_version;
    output += "\n// target: java\n\npackage io.fastdb.payload.generated;\n\n";

    const bool has_entries = !resolved.entries().empty();
    if (has_entries || topology.has_objects) {
        output += "import io.fastdb.payload.Builder;\n";
    }
    output += "import io.fastdb.payload.CompiledSpec;\n";
    if (topology.has_objects) {
        output += "import io.fastdb.payload.GraphIdentity;\n"
                  "import io.fastdb.payload.ObjectHandle;\n";
    }
    if (has_entries) {
        output += "import io.fastdb.payload.Payload;\n";
    }
    if (has_entries || !resolved.components().empty()) {
        output += "import io.fastdb.payload.View;\n";
    }
    output += "import java.nio.charset.StandardCharsets;\n"
              "import java.util.List;\n\n";
}

void append_digest_bytes(CheckedOutput& output,
                         const std::array<std::uint8_t, 32>& digest) {
    constexpr char hex[] = "0123456789abcdef";
    output += "    public static byte[] payloadSha256() {\n"
              "        return new byte[] {\n";
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        if (index % 8U == 0U) {
            output += "            ";
        }
        const std::uint8_t byte = digest[index];
        output += "(byte) 0x";
        output += hex[byte >> 4U];
        output += hex[byte & 0x0fU];
        output += index % 8U == 7U ? ",\n" : ", ";
    }
    output += "        };\n    }\n\n";
}

void append_string_literal(CheckedOutput& output, std::string_view text) {
    output += '"';
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            output += '\\';
            output += character;
        } else if (byte < 0x20U || byte == 0x7fU) {
            constexpr char octal[] = "01234567";
            output += '\\';
            output += octal[byte >> 6U];
            output += octal[(byte >> 3U) & 7U];
            output += octal[byte & 7U];
        } else {
            output += character;
        }
    }
    output += '"';
}

void append_canonical_source(CheckedOutput& output, std::string_view source) {
    constexpr std::size_t chunk_bytes = 16384U;
    output += "    public static String canonicalSourceText() {\n";
    if (source.size() <= chunk_bytes) {
        output += "        return ";
        append_string_literal(output, source);
        output += ";\n";
    } else {
        // Runtime concatenation keeps javac from folding the chunks into a
        // single constant. Split only between UTF-8 code points.
        output += "        return new StringBuilder()\n";
        for (std::size_t begin = 0U; begin < source.size();) {
            std::size_t end =
                begin + std::min(chunk_bytes, source.size() - begin);
            while (end < source.size() &&
                   (static_cast<unsigned char>(source[end]) & 0xc0U) == 0x80U) {
                --end;
            }
            output += "            .append(";
            append_string_literal(output, source.substr(begin, end - begin));
            output += ")\n";
            begin = end;
        }
        output += "            .toString();\n";
    }
    output += "    }\n\n"
              "    public static byte[] canonicalSource() {\n"
              "        return canonicalSourceText().getBytes(StandardCharsets.UTF_8);\n"
              "    }\n\n"
              "    public static CompiledSpec compileSpec() {\n"
              "        return CompiledSpec.compile(canonicalSource());\n"
              "    }\n\n";
}

template<class Item>
void append_id_metadata(CheckedOutput& output, std::string_view list_name,
                        const std::vector<Item>& items,
                        std::string_view index_suffix) {
    output += "    public static final List<IdMetadata> ";
    output += list_name;
    output += " = List.of(";
    std::string_view separator = "\n";
    for (const auto& item : items) {
        output += separator;
        output += "        new IdMetadata(\"";
        output += project_identifier(Target::java, item.id);
        output += index_suffix;
        output += "\", \"";
        output += item.id;
        output += "\", ";
        output += std::to_string(item.index);
        output += ')';
        separator = ",\n";
    }
    output += items.empty() ? ");\n" : "\n    );\n";
}

void append_metadata(CheckedOutput& output, const spec::ResolvedSpec& resolved) {
    output += "    public record IdMetadata(String symbol, String originalId, "
              "int stableIndex) {}\n"
              "    public record FieldMetadata(String symbol, String originalId, "
              "int componentIndex, int fieldIndex) {}\n\n";
    append_id_metadata(output, "ENTRIES", resolved.entries(), "_ENTRY_INDEX");
    append_id_metadata(output, "COMPONENTS", resolved.components(),
                       "_COMPONENT_INDEX");
    output += "    public static final List<FieldMetadata> FIELDS = List.of(";
    std::string_view separator = "\n";
    for (const Component& component : resolved.components()) {
        for (const Field& field : component.fields) {
            output += separator;
            output += "        new FieldMetadata(\"";
            output += project_identifier(Target::java, field.id);
            output += "_FIELD_INDEX\", \"";
            output += field.id;
            output += "\", ";
            output += std::to_string(component.index);
            output += ", ";
            output += std::to_string(field.index);
            output += ')';
            separator = ",\n";
        }
    }
    output += separator == "\n" ? ");\n" : "\n    );\n";
}

void append_class_open(CheckedOutput& output, std::string_view type) {
    output += "\n    public static final class ";
    output += type;
    output += " implements AutoCloseable {\n"
              "        private final View view;\n\n"
              "        private ";
    output += type;
    output += "(View view) { this.view = view; }\n\n"
              "        public View genericView() { return view.retain(); }\n"
              "        public ";
    output += type;
    output += " materialize() { return new ";
    output += type;
    output += "(view.materialize()); }\n"
              "        @Override\n"
              "        public void close() { view.close(); }\n";
}

void append_index(CheckedOutput& output, std::string_view indent,
                  std::string_view name, std::uint32_t index) {
    output += indent;
    output += "public static final int ";
    output += name;
    output += " = ";
    output += std::to_string(index);
    output += ";\n";
}

void append_entry(CheckedOutput& output, const Entry& entry,
                  spec::Profile profile) {
    const std::string symbol = project_identifier(Target::java, entry.id);
    const std::string type = "FdbJavaEntry_" + symbol + "_Sequence";
    const std::string index = symbol + "_ENTRY_INDEX";
    output += '\n';
    append_index(output, "    ", index, entry.index);
    append_class_open(output, type);
    output += "\n        public long length() { return view.length(); }\n"
              "        public View at(long index) { return view.at(index); }\n";
    if (entry.type.kind == TypeKind::ref) {
        output +=
            "        public View atRefTarget(long index) {\n"
            "            try (View child = at(index)) {\n"
            "                return child.refTarget();\n"
            "            }\n"
            "        }\n";
    }
    if (profile == spec::Profile::object_graph_v1 &&
        entry.type.kind == TypeKind::component) {
        output +=
            "        public GraphIdentity atGraphIdentity(long index) {\n"
            "            try (View child = at(index)) {\n"
            "                return child.graphIdentity();\n"
            "            }\n"
            "        }\n";
    }
    output += "    }\n\n    public static ";
    output += type;
    output += ' ';
    output += symbol;
    output += "_fromPayload(Payload payload) {\n"
              "        payload.requireSpecSha256(payloadSha256());\n"
              "        return new ";
    output += type;
    output += "(payload.entryView(";
    output += index;
    output += "));\n    }\n\n    public static Builder ";
    output += symbol;
    output += "_builderEntryBegin(Builder builder, long valueCount) {\n"
              "        builder.requireSpecSha256(payloadSha256());\n"
              "        return builder.entryBegin(";
    output += index;
    output += ", valueCount);\n    }\n";
}

void append_field(CheckedOutput& output, const Field& field) {
    const std::string symbol = project_identifier(Target::java, field.id);
    const std::string index = symbol + "_FIELD_INDEX";
    output += '\n';
    append_index(output, "        ", index, field.index);
    output += "        public View ";
    output += symbol;
    output += "() { return view.field(";
    output += index;
    output += "); }\n";
    const ScalarGetter getter = java_scalar_getter(field.type.kind);
    const bool is_ref = field.type.kind == TypeKind::ref;
    if (getter.type.empty() && !is_ref) {
        return;
    }
    output += "        public ";
    output += is_ref ? "View" : getter.type;
    output += ' ';
    output += symbol;
    output += is_ref ? "_refTarget" : "_value";
    output += "() {\n            try (View child = ";
    output += symbol;
    output += "()) {\n"
              "                return child.";
    output += is_ref ? "refTarget" : getter.method;
    output += "();\n            }\n        }\n";
}

void append_component(CheckedOutput& output, const Component& component,
                      bool is_identity) {
    const std::string symbol = project_identifier(Target::java, component.id);
    const std::string type = project_type_identifier(Target::java, component.id);
    const std::string index = symbol + "_COMPONENT_INDEX";
    output += '\n';
    append_index(output, "    ", index, component.index);
    append_class_open(output, type);
    output += "\n        public static ";
    output += type;
    output += " tryFromView(View view) {\n"
              "            view.requireSpecSha256(payloadSha256());\n"
              "            if (view.componentIndex() != ";
    output += index;
    output += ") { return null; }\n"
              "            return new ";
    output += type;
    output += "(view.retain());\n        }\n";
    if (is_identity) {
        output += "        public GraphIdentity graphIdentity() { "
                  "return view.graphIdentity(); }\n";
    }
    for (const Field& field : component.fields) {
        append_field(output, field);
    }
    output += "    }\n";
    if (is_identity) {
        output += "\n    public static ObjectHandle ";
        output += symbol;
        output += "_builderDeclare(Builder builder) {\n"
                  "        builder.requireSpecSha256(payloadSha256());\n"
                  "        return builder.declareObject(";
        output += index;
        output += ");\n    }\n";
    }
}

}  // namespace

std::string render_java(const spec::CompiledSpec& compiled,
                        const spec::RuntimeTopology& topology,
                        std::uint64_t max_total_bytes) {
    const std::string digest = identity::sha256_lower_hex(compiled.digest());
    const std::string type = "FastdbPayload_" + digest;
    const spec::ResolvedSpec& resolved = compiled.resolved();
    CheckedOutput output(max_total_bytes);
    output.reserve(compiled.canonical_bytes().size() + 4096U);
    append_header(output, digest, resolved, topology);
    output += "public final class ";
    output += type;
    output += " {\n    private ";
    output += type;
    output += "() {}\n\n"
              "    public static final String PAYLOAD_SHA256 = \"";
    output += digest;
    output += "\";\n\n";
    append_digest_bytes(output, compiled.digest());
    append_canonical_source(output, compiled.canonical_bytes());
    append_metadata(output, resolved);
    for (const Entry& entry : resolved.entries()) {
        append_entry(output, entry, resolved.profile());
    }
    for (std::size_t index = 0U; index < resolved.components().size(); ++index) {
        append_component(output, resolved.components()[index],
                         topology.identity_components[index] != UINT8_C(0));
    }
    output += "}\n";
    return std::move(output).finish();
}

}  // namespace fastdb::payload::codegen
