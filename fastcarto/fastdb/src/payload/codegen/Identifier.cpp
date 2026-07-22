#include "payload/codegen/Identifier.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace fastdb::payload::codegen {
namespace {

std::string_view identifier_prefix(Target target) noexcept {
    switch (target) {
    case Target::cpp:
        return "fdb_cpp_id_";
    case Target::rust:
        return "fdb_rust_id_";
    case Target::python:
        return "fdb_python_id_";
    case Target::typescript:
        return "fdb_ts_id_";
    }
    return {};
}

std::string_view type_prefix(Target target) noexcept {
    switch (target) {
    case Target::cpp:
        return "FdbCppType_";
    case Target::rust:
        return "FdbRustType_";
    case Target::python:
        return "FdbPythonType_";
    case Target::typescript:
        return "FdbTsType_";
    }
    return {};
}

}  // namespace

std::string_view target_name(Target target) noexcept {
    switch (target) {
    case Target::cpp:
        return "cpp";
    case Target::rust:
        return "rust";
    case Target::python:
        return "python";
    case Target::typescript:
        return "typescript";
    }
    return {};
}

std::string project_identifier(Target target, std::string_view source_id) {
    constexpr char kHex[] = "0123456789abcdef";
    const std::string_view prefix = identifier_prefix(target);
    if (prefix.empty()) {
        return {};
    }

    std::string result;
    result.reserve(prefix.size() + source_id.size() * 2U);
    result.append(prefix);
    for (const char character : source_id) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(kHex[byte >> 4U]);
        result.push_back(kHex[byte & 0x0fU]);
    }
    return result;
}

std::string project_type_identifier(Target target,
                                    std::string_view source_id) {
    const std::string_view prefix = type_prefix(target);
    const std::string identifier = project_identifier(target, source_id);
    if (prefix.empty() || identifier.empty()) {
        return {};
    }

    std::string result;
    result.reserve(prefix.size() + identifier.size() + 5U);
    result.append(prefix);
    result.append(identifier);
    result.append("_View");
    return result;
}

}  // namespace fastdb::payload::codegen
