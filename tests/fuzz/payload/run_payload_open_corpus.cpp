#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);

int main(int argc, char** argv) {
    if (argc < 2) {
        return EXIT_FAILURE;
    }
    for (int index = 1; index < argc; ++index) {
        std::ifstream input(argv[index], std::ios::binary);
        if (!input) {
            return EXIT_FAILURE;
        }
        const std::string bytes{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
        if (LLVMFuzzerTestOneInput(data, bytes.size()) != 0) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
