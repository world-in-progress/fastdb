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

set(BUILD_TESTING OFF CACHE BOOL "Build native tests" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "Build CLI tools" FORCE)
set(USE_SWIG_PYTHON OFF CACHE BOOL "use swig python wrapper" FORCE)
set(USE_SWIG_NODE OFF CACHE BOOL "use swig node wrapper" FORCE)
set(USE_SWIG_GO OFF CACHE BOOL "use swig go wrapper" FORCE)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto
    ${CMAKE_CURRENT_BINARY_DIR}/fastcarto
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(
    FASTDB_PAYLOAD_WASM_EXPORTS
    ${CMAKE_CURRENT_BINARY_DIR}/fastdb_payload_wasm_exports.txt
)
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_payload_wasm_exports.py
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/abi/fastdb_payload_v1_symbols.txt
)
execute_process(
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_payload_wasm_exports.py
        --allowlist
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/abi/fastdb_payload_v1_symbols.txt
        --output
        ${FASTDB_PAYLOAD_WASM_EXPORTS}
    RESULT_VARIABLE FASTDB_PAYLOAD_WASM_EXPORT_RESULT
    OUTPUT_VARIABLE FASTDB_PAYLOAD_WASM_EXPORT_STDOUT
    ERROR_VARIABLE FASTDB_PAYLOAD_WASM_EXPORT_STDERR
)
if(NOT FASTDB_PAYLOAD_WASM_EXPORT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Failed to generate exact FastDB payload Wasm exports:\n"
        "${FASTDB_PAYLOAD_WASM_EXPORT_STDERR}"
    )
endif()
file(READ ${FASTDB_PAYLOAD_WASM_EXPORTS} FASTDB_PAYLOAD_WASM_EXPORT_LIST)
string(STRIP "${FASTDB_PAYLOAD_WASM_EXPORT_LIST}" FASTDB_PAYLOAD_WASM_EXPORT_LIST)

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
    "SHELL:-s WASM_BIGINT=1"
    "SHELL:-s ENVIRONMENT=web,worker,node"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','HEAP32','HEAPF64']"
    "SHELL:-s EXPORTED_FUNCTIONS=${FASTDB_PAYLOAD_WASM_EXPORT_LIST}"
    "--post-js=${CMAKE_CURRENT_SOURCE_DIR}/post.js"
)
