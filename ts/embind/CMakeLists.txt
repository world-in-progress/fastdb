cmake_minimum_required(VERSION 3.16)

project(fastdb4ts)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    message(FATAL_ERROR
        "fastdb4ts must be built with the Emscripten toolchain.\n"
        "Use: emcmake cmake -S ts/embind -B build-wasm"
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BUILD_TOOLS OFF CACHE BOOL "Build CLI tools" FORCE)
set(USE_SWIG_PYTHON OFF CACHE BOOL "use swig python wrapper" FORCE)
set(USE_SWIG_NODE OFF CACHE BOOL "use swig node wrapper" FORCE)
set(USE_SWIG_GO OFF CACHE BOOL "use swig go wrapper" FORCE)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto
    ${CMAKE_CURRENT_BINARY_DIR}/fastcarto
)

set(WASM_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../fastdb4ts/src/wasm)

add_executable(fastdb4ts fastdb4ts.cpp)
target_include_directories(fastdb4ts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include)
target_link_libraries(fastdb4ts PRIVATE fastdb gaiageo Clipper2)

set_target_properties(fastdb4ts PROPERTIES
    OUTPUT_NAME "fastdb4ts"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${WASM_OUTPUT_DIR}"
)

target_link_options(fastdb4ts PRIVATE
    "--bind"
    "SHELL:-s WASM=1"
    "--no-entry"
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=FastdbWasm"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s ENVIRONMENT=web,worker,node"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','HEAP32','HEAPF64']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_malloc','_free']"
    "--post-js=${CMAKE_CURRENT_SOURCE_DIR}/post.js"
)
