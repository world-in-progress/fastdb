#include <emscripten/bind.h>

#include "fastdb.h"

using namespace emscripten;
using namespace wx;

EMSCRIPTEN_BINDINGS(fastdb4ts) {
    class_<MemoryStream>("WxMemoryStream")
        .constructor<>()
        .function("reset", &MemoryStream::reset);
}
