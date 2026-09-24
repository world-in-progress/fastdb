#include "TestSupport.hpp"

#include <double-conversion/double-conversion.h>
#include <picosha2.h>
#include <yyjson.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace {

std::size_t duplicate_member_count(std::string_view source) {
    yyjson_doc* document = yyjson_read(source.data(), source.size(), 0);
    if (document == nullptr) {
        return 0;
    }

    yyjson_val* root = yyjson_doc_get_root(document);
    const std::size_t count = yyjson_is_obj(root) ? yyjson_obj_size(root) : 0;
    yyjson_doc_free(document);
    return count;
}

std::string ecmascript_number(double value) {
    char buffer[64]{};
    double_conversion::StringBuilder builder(buffer, sizeof(buffer));
    const bool converted =
        double_conversion::DoubleToStringConverter::EcmaScriptConverter()
            .ToShortest(value, &builder);
    return converted ? builder.Finalize() : std::string{};
}

std::string sha256_hex(std::string_view value) {
    return picosha2::hash256_hex_string(std::string(value));
}

}  // namespace

int main() {
    require(duplicate_member_count(R"({"x":1,"x":2})") == 2);
    require(ecmascript_number(1e30) == "1e+30");
    require(sha256_hex("The quick brown fox jumps over the lazy dog") ==
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    return EXIT_SUCCESS;
}
