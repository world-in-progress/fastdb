#include "FastVectorDbLayerBuild_p.h"
#include "TestSupport.hpp"
#include "fastdb.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace {

std::vector<unsigned char> build_scalar_database() {
    wx::FastVectorDbBuild build;
    build.begin("{}");
    build.createLayerBegin("values");
    build.addField("value", wx::ftU8);
    build.addField("floating", wx::ftF64);
    build.addFeatureBegin();
    build.setField(0, 7);
    build.setField(1, 7.25);
    build.addFeatureEnd();
    build.createLayerEnd();

    std::vector<unsigned char> bytes(build.byteLength());
    if (build.postToBuffer(bytes.data(), bytes.size()) != bytes.size()) {
        return {};
    }
    return bytes;
}

}  // namespace

int main() {
    std::vector<unsigned char> expected = build_scalar_database();
    require(!expected.empty());
    for (int iteration = 0; iteration < 32; ++iteration) {
        require(build_scalar_database() == expected);
    }

    constexpr std::size_t database_header_size = 20;
    constexpr std::size_t descriptor_offset =
        database_header_size + sizeof(wx::layer_header_t);
    require(expected.size() >=
            descriptor_offset + sizeof(wx::field_desc_ex_t));

    wx::field_desc_ex_t descriptor{};
    std::memcpy(&descriptor, expected.data() + descriptor_offset,
                sizeof(descriptor));
    require(descriptor.type == wx::ftU8);
    require(descriptor.element_type == 0);
    const std::size_t padding_begin =
        offsetof(wx::field_desc_ex_t, element_type) +
        sizeof(descriptor.element_type);
    const std::size_t padding_end =
        offsetof(wx::field_desc_ex_t, vmin);
    require(padding_begin <= padding_end);
    for (std::size_t offset = padding_begin; offset < padding_end; ++offset) {
        require(expected[descriptor_offset + offset] == 0);
    }

    std::unique_ptr<wx::FastVectorDb> database(
        wx::FastVectorDb::load(expected.data(), expected.size(), nullptr,
                               nullptr));
    require(database != nullptr);
    require(database->getLayerCount() == 1);
    wx::FastVectorDbLayer* layer = database->getLayer(0);
    require(layer != nullptr);
    require(layer->getFeatureCount() == 1);
    layer->rewind();
    require(layer->next());
    require(layer->getFieldAsInt(0) == 7);
    require(layer->getFieldAsFloat(1) == 7.25);
    return EXIT_SUCCESS;
}
