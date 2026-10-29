#include "TestSupport.hpp"
#include "fastdb.h"
#include "FastVectorDbLayerBuild_p.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

void* map_buffer(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                        PAGE_READWRITE);
#else
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return data == MAP_FAILED ? nullptr : data;
#endif
}

bool protect_buffer(void* data, size_t size, bool readable) {
#ifdef _WIN32
    DWORD previous = 0;
    return VirtualProtect(data, size, readable ? PAGE_READWRITE : PAGE_NOACCESS,
                          &previous) != 0;
#else
    return mprotect(data, size, readable ? PROT_READ | PROT_WRITE : PROT_NONE) == 0;
#endif
}

bool unmap_buffer(void* data, size_t size) {
#ifdef _WIN32
    (void)size;
    return VirtualFree(data, 0, MEM_RELEASE) != 0;
#else
    return munmap(data, size) == 0;
#endif
}

std::vector<wx::u8> build_database() {
    wx::FastVectorDbBuild build;
    build.begin("{}");
    build.createLayerBegin("values");
    build.addField("tag", wx::ftU8);
    build.addField("value", wx::ftF64);
    for (int row = 0; row < 2; ++row) {
        build.addFeatureBegin();
        wx::point2_t point{double(row), double(row)};
        build.setGeometry(&point, sizeof(point), wx::ginPoint2);
        build.setField(0, row + 1);
        build.setField(1, 2.5 + row);
        build.addFeatureEnd();
    }
    build.createLayerEnd();
    std::vector<wx::u8> bytes(build.byteLength());
    if (build.postToBuffer(bytes.data(), bytes.size()) != bytes.size())
        return {};
    return bytes;
}

struct CallbackState {
    void* expected_data = nullptr;
    size_t expected_size = 0;
    int calls = 0;
    bool matched = false;
    bool restored = false;
    bool unmapped = false;
};

void release_buffer(void* data, size_t size, void* cookie) {
    auto* state = static_cast<CallbackState*>(cookie);
    state->matched = data == state->expected_data &&
                     size == state->expected_size;
    ++state->calls;
    state->restored = protect_buffer(data, size, true);
    state->unmapped = unmap_buffer(data, size);
}

int check_database(wx::FastVectorDb* db) {
    require(db != nullptr);
    require(db->getLayerCount() == 1);
    auto* layer = db->getLayer(0);
    require(layer != nullptr);
    require(layer == db->getLayer(0));
    require(layer->getFeatureCount() == 2);
    require(layer->getFieldOffset(0) == 0);
    require(layer->getFieldOffset(1) == sizeof(wx::u8));
    require(layer->getFeatureByteSize() >= sizeof(wx::u8) + sizeof(double));

    auto* base = static_cast<const wx::u8*>(layer->getTableDataAddress());
    require(base != nullptr);
    const size_t stride = layer->getFeatureByteSize();
    const size_t value_offset = layer->getFieldOffset(1);
    for (int row = 0; row < 2; ++row) {
        require(base[row * stride] == row + 1);
        double value = 0;
        std::memcpy(&value, base + row * stride + value_offset,
                    sizeof(value));
        require(value == 2.5 + row);
        auto* feature = layer->tryGetFeatureAt(row);
        require(feature != nullptr);
        require(feature == layer->tryGetFeatureAt(row));
        require(feature->getFieldAsFloat(1) == value);
    }
    require(layer->tryGetFeatureAt(2) == nullptr);
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2);
    // A complete layer header does not imply its descriptors or string counts
    // exist. Reject these envelopes before the layer constructor reads them.
    std::vector<wx::u8> truncated(20 + sizeof(wx::layer_header_t), 0);
    std::memcpy(truncated.data(), "FASTVectorDB0.1", sizeof("FASTVectorDB0.1"));
    const wx::u32 layer_count = 1;
    std::memcpy(truncated.data() + 16, &layer_count, sizeof(layer_count));
    wx::layer_header_t header{};
    header.total_size = sizeof(header);
    for (wx::u16 fields : {wx::u16(1), wx::u16(0)}) {
        header.field_count = fields;
        std::memcpy(truncated.data() + 20, &header, sizeof(header));
        require(wx::FastVectorDb::load(truncated.data(), truncated.size(), nullptr, nullptr) == nullptr);
    }
    void* invalid = map_buffer(truncated.size());
    require(invalid != nullptr);
    std::memcpy(invalid, truncated.data(), truncated.size());
    CallbackState rejected{invalid, truncated.size()};
    require(wx::FastVectorDb::load(invalid, truncated.size(), release_buffer, &rejected) == nullptr);
    require(rejected.calls == 1 && rejected.restored && rejected.unmapped);

    auto bytes = build_database();
    require(!bytes.empty());

    // Revoking reads after caching children catches destructor access to backing.
    void* borrowed = map_buffer(bytes.size());
    require(borrowed != nullptr);
    std::memcpy(borrowed, bytes.data(), bytes.size());
    {
        std::unique_ptr<wx::FastVectorDb> db(
            wx::FastVectorDb::load(borrowed, bytes.size(), nullptr, nullptr));
        require(check_database(db.get()) == EXIT_SUCCESS);
        require(protect_buffer(borrowed, bytes.size(), false));
    }
    require(protect_buffer(borrowed, bytes.size(), true));
    require(static_cast<const char*>(borrowed)[0] == 'F');
    require(unmap_buffer(borrowed, bytes.size()));

    // The owned callback restores and releases a protected mapping exactly once.
    void* owned = map_buffer(bytes.size());
    require(owned != nullptr);
    std::memcpy(owned, bytes.data(), bytes.size());
    CallbackState state{owned, bytes.size()};
    {
        std::unique_ptr<wx::FastVectorDb> db(wx::FastVectorDb::load(
            owned, bytes.size(), release_buffer, &state));
        require(check_database(db.get()) == EXIT_SUCCESS);
        require(state.calls == 0);
        require(protect_buffer(owned, bytes.size(), false));
    }
    require(state.calls == 1);
    require(state.matched);
    require(state.restored);
    require(state.unmapped);

    // CTest supplies a path in the build directory, outside the launch cwd.
    const char* filename = argv[1];
    {
        std::ofstream file(filename, std::ios::binary);
        require(file.good());
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        require(file.good());
    }
    {
        std::unique_ptr<wx::FastVectorDb> db(wx::FastVectorDb::load(filename));
        require(check_database(db.get()) == EXIT_SUCCESS);
    }
    require(std::remove(filename) == 0);
    return EXIT_SUCCESS;
}
