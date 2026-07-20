function(add_fastdb_payload_test name)
    set(options)
    set(one_value_args)
    set(multi_value_args SOURCES EXTRA_LIBRARIES)
    cmake_parse_arguments(
        PAYLOAD_TEST
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    if(PAYLOAD_TEST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "add_fastdb_payload_test(${name}) received unknown arguments: "
            "${PAYLOAD_TEST_UNPARSED_ARGUMENTS}"
        )
    endif()

    if(NOT PAYLOAD_TEST_SOURCES)
        message(FATAL_ERROR
            "add_fastdb_payload_test(${name}) requires at least one source"
        )
    endif()

    set(missing_libraries)
    foreach(extra_library IN LISTS PAYLOAD_TEST_EXTRA_LIBRARIES)
        if(NOT TARGET ${extra_library})
            list(APPEND missing_libraries ${extra_library})
        endif()
    endforeach()
    if(missing_libraries)
        message(FATAL_ERROR
            "add_fastdb_payload_test(${name}) cannot resolve dependency targets: "
            "${missing_libraries}"
        )
    endif()

    set(target_name fastdb_payload_test_${name})
    add_executable(${target_name} ${PAYLOAD_TEST_SOURCES})
    target_include_directories(
        ${target_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/payload
    )
    target_link_libraries(
        ${target_name}
        PRIVATE
            ${PAYLOAD_TEST_EXTRA_LIBRARIES}
    )
    fastdb_apply_sanitizers(${target_name})

    add_test(NAME payload.${name} COMMAND ${target_name})
endfunction()

set(FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC
    ${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/binary/spec/component-list-composition.source.json
)
set(FASTDB_PAYLOAD_FUZZ_MISMATCH_SPEC
    ${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/binary/spec/numeric-edges.source.json
)
set(FASTDB_PAYLOAD_FUZZ_SPEC_DEFINITIONS
    "FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC_PATH=\"${FASTDB_PAYLOAD_FUZZ_MATCHING_SPEC}\""
    "FASTDB_PAYLOAD_FUZZ_MISMATCH_SPEC_PATH=\"${FASTDB_PAYLOAD_FUZZ_MISMATCH_SPEC}\""
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    file(
        GLOB FASTDB_PAYLOAD_REVIEWED_BINARY_SEEDS
        CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/../fuzz/payload/binary-corpus/*.bin
    )
    list(SORT FASTDB_PAYLOAD_REVIEWED_BINARY_SEEDS)
    list(LENGTH FASTDB_PAYLOAD_REVIEWED_BINARY_SEEDS
         FASTDB_PAYLOAD_REVIEWED_BINARY_SEED_COUNT)
    if(NOT FASTDB_PAYLOAD_REVIEWED_BINARY_SEED_COUNT EQUAL 10)
        message(FATAL_ERROR
            "The reviewed portable payload binary corpus must contain exactly "
            "10 seeds; found ${FASTDB_PAYLOAD_REVIEWED_BINARY_SEED_COUNT}"
        )
    endif()

    add_executable(
        fastdb_payload_test_binary_open_corpus
        ../fuzz/payload/fuzz_payload_open.cpp
        ../fuzz/payload/run_payload_open_corpus.cpp
    )
    target_include_directories(
        fastdb_payload_test_binary_open_corpus
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
    )
    target_link_libraries(
        fastdb_payload_test_binary_open_corpus
        PRIVATE fastdb_payload_core
    )
    target_compile_definitions(
        fastdb_payload_test_binary_open_corpus
        PRIVATE ${FASTDB_PAYLOAD_FUZZ_SPEC_DEFINITIONS}
    )
    target_compile_features(
        fastdb_payload_test_binary_open_corpus PRIVATE cxx_std_17
    )
    fastdb_apply_sanitizers(fastdb_payload_test_binary_open_corpus)
    add_test(
        NAME payload.binary_open_corpus
        COMMAND
            fastdb_payload_test_binary_open_corpus
            ${FASTDB_PAYLOAD_REVIEWED_BINARY_SEEDS}
    )
    set_tests_properties(payload.binary_open_corpus PROPERTIES TIMEOUT 120)
endif()

if(FASTDB_BUILD_FUZZERS)
    set(FASTDB_FUZZ_COMPILE_OPTIONS
        -fsanitize=fuzzer-no-link
    )
    foreach(payload_fuzz_library IN ITEMS
            fastdb_payload_core
            fastdb_yyjson
            fastdb_double_conversion)
        if(NOT TARGET ${payload_fuzz_library})
            message(FATAL_ERROR
                "FASTDB_BUILD_FUZZERS cannot instrument missing target "
                "${payload_fuzz_library}"
            )
        endif()
        target_compile_options(
            ${payload_fuzz_library}
            PRIVATE ${FASTDB_FUZZ_COMPILE_OPTIONS}
        )
    endforeach()

    add_executable(
        fuzz_payload_spec_compile
        ../fuzz/payload/fuzz_spec_compile.cpp
    )
    target_include_directories(
        fuzz_payload_spec_compile
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
    )
    target_link_libraries(
        fuzz_payload_spec_compile
        PRIVATE fastdb_payload_core
    )
    target_compile_features(fuzz_payload_spec_compile PRIVATE cxx_std_17)
    target_compile_options(
        fuzz_payload_spec_compile
        PRIVATE ${FASTDB_FUZZ_COMPILE_OPTIONS}
    )
    fastdb_apply_sanitizers(fuzz_payload_spec_compile)
    target_link_options(
        fuzz_payload_spec_compile
        PRIVATE -fsanitize=fuzzer
    )
    set_target_properties(
        fuzz_payload_spec_compile
        PROPERTIES
            CXX_EXTENSIONS OFF
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            LINKER_LANGUAGE CXX
    )

    add_executable(
        fuzz_payload_open
        ../fuzz/payload/fuzz_payload_open.cpp
    )
    target_include_directories(
        fuzz_payload_open
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
    )
    target_link_libraries(
        fuzz_payload_open
        PRIVATE fastdb_payload_core
    )
    target_compile_definitions(
        fuzz_payload_open
        PRIVATE ${FASTDB_PAYLOAD_FUZZ_SPEC_DEFINITIONS}
    )
    target_compile_features(fuzz_payload_open PRIVATE cxx_std_17)
    target_compile_options(
        fuzz_payload_open
        PRIVATE ${FASTDB_FUZZ_COMPILE_OPTIONS}
    )
    fastdb_apply_sanitizers(fuzz_payload_open)
    target_link_options(
        fuzz_payload_open
        PRIVATE -fsanitize=fuzzer
    )
    set_target_properties(
        fuzz_payload_open
        PROPERTIES
            CXX_EXTENSIONS OFF
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            LINKER_LANGUAGE CXX
    )
endif()

add_fastdb_payload_test(
    dependency_smoke
    SOURCES payload/test_dependency_smoke.cpp
    EXTRA_LIBRARIES
        fastdb_yyjson
        fastdb_double_conversion
        fastdb_picosha2
)

add_fastdb_payload_test(
    jcs
    SOURCES payload/test_jcs.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_jcs
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_compile_definitions(
    fastdb_payload_test_jcs
    PRIVATE
        "FASTDB_PAYLOAD_JCS_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/jcs\""
)

add_fastdb_payload_test(
    json_document
    SOURCES payload/test_json_document.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_json_document
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_compile_definitions(
    fastdb_payload_test_json_document
    PRIVATE
        "FASTDB_PAYLOAD_SCHEMA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../../schemas\""
)

add_fastdb_payload_test(
    spec_parse
    SOURCES payload/test_spec_parse.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_spec_parse
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_compile_definitions(
    fastdb_payload_test_spec_parse
    PRIVATE
        "FASTDB_PAYLOAD_SPEC_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/spec\""
)

add_fastdb_payload_test(
    spec_resolve
    SOURCES payload/test_spec_resolve.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_spec_resolve
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_compile_definitions(
    fastdb_payload_test_spec_resolve
    PRIVATE
        "FASTDB_PAYLOAD_SPEC_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/spec\""
)
set_tests_properties(payload.spec_resolve PROPERTIES TIMEOUT 60)

find_package(Threads REQUIRED)

add_fastdb_payload_test(
    compiled_spec
    SOURCES
        payload/GoldenCorpus.cpp
        payload/test_compiled_spec.cpp
    EXTRA_LIBRARIES
        fastdb_payload_core
        Threads::Threads
)
target_include_directories(
    fastdb_payload_test_compiled_spec
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_compile_definitions(
    fastdb_payload_test_compiled_spec
    PRIVATE
        "FASTDB_PAYLOAD_SPEC_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/spec\""
        "FASTDB_PAYLOAD_SCHEMA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../../schemas\""
)
set_tests_properties(payload.compiled_spec PROPERTIES TIMEOUT 60)

add_fastdb_payload_test(
    c_header_smoke
    SOURCES payload/test_c_header_smoke.c
    EXTRA_LIBRARIES fastdb
)
target_include_directories(
    fastdb_payload_test_c_header_smoke
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
)
set_target_properties(
    fastdb_payload_test_c_header_smoke
    PROPERTIES
        C_EXTENSIONS OFF
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        LINKER_LANGUAGE CXX
)

add_fastdb_payload_test(
    spec_abi
    SOURCES payload/test_spec_abi.cpp
    EXTRA_LIBRARIES
        fastdb_payload_core
        Threads::Threads
)
target_include_directories(
    fastdb_payload_test_spec_abi
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
set_tests_properties(payload.spec_abi PROPERTIES TIMEOUT 120)

add_fastdb_payload_test(
    runtime_abi
    SOURCES payload/test_runtime_abi.cpp
    EXTRA_LIBRARIES
        fastdb_payload_core
        Threads::Threads
)
target_include_directories(
    fastdb_payload_test_runtime_abi
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
set_tests_properties(payload.runtime_abi PROPERTIES TIMEOUT 120)

add_fastdb_payload_test(
    cpp_facade
    SOURCES payload/test_cpp_facade.cpp
    EXTRA_LIBRARIES
        fastdb_payload_core
        Threads::Threads
)
target_include_directories(
    fastdb_payload_test_cpp_facade
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
)

add_fastdb_payload_test(
    runtime_cpp_facade
    SOURCES payload/test_runtime_cpp_facade.cpp
    EXTRA_LIBRARIES
        fastdb
)
target_include_directories(
    fastdb_payload_test_runtime_cpp_facade
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
)
set_tests_properties(payload.runtime_cpp_facade PROPERTIES TIMEOUT 120)

add_fastdb_payload_test(
    error
    SOURCES payload/test_error.cpp
    EXTRA_LIBRARIES
        fastdb_payload_core
        Threads::Threads
)
target_include_directories(
    fastdb_payload_test_error
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)

add_fastdb_payload_test(
    payload_builder
    SOURCES payload/test_payload_builder.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_payload_builder
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
set_tests_properties(payload.payload_builder PROPERTIES TIMEOUT 240)

add_fastdb_payload_test(
    record_layout
    SOURCES payload/test_record_layout.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_record_layout
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
set_tests_properties(payload.record_layout PROPERTIES TIMEOUT 120)

add_fastdb_payload_test(
    graph_runtime_schema
    SOURCES payload/test_graph_runtime_schema.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_graph_runtime_schema
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
set_tests_properties(payload.graph_runtime_schema PROPERTIES TIMEOUT 120)

add_fastdb_payload_test(
    graph_builder
    SOURCES payload/test_graph_builder.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_graph_builder
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
set_tests_properties(payload.graph_builder PROPERTIES TIMEOUT 180)

add_fastdb_payload_test(
    graph_binary
    SOURCES
        payload/GoldenCorpus.cpp
        payload/test_graph_binary.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_graph_binary
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_compile_definitions(
    fastdb_payload_test_graph_binary
    PRIVATE
        "FASTDB_PAYLOAD_BINARY_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/binary\""
)
set_tests_properties(payload.graph_binary PROPERTIES TIMEOUT 180)

add_fastdb_payload_test(
    record_binary
    SOURCES
        payload/GoldenCorpus.cpp
        payload/test_record_binary.cpp
    EXTRA_LIBRARIES fastdb_payload_core
)
target_include_directories(
    fastdb_payload_test_record_binary
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_compile_definitions(
    fastdb_payload_test_record_binary
    PRIVATE
        "FASTDB_PAYLOAD_BINARY_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/binary\""
)
set_tests_properties(payload.record_binary PROPERTIES TIMEOUT 240)

add_fastdb_payload_test(
    payload_backing
    SOURCES payload/test_payload_backing.cpp
    EXTRA_LIBRARIES
        fastdb_payload_core
        Threads::Threads
)
target_include_directories(
    fastdb_payload_test_payload_backing
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
set_tests_properties(payload.payload_backing PROPERTIES TIMEOUT 120)

add_fastdb_payload_test(
    payload_open
    SOURCES
        payload/GoldenCorpus.cpp
        payload/test_payload_open.cpp
    EXTRA_LIBRARIES
        fastdb_payload_core
        Threads::Threads
)
target_include_directories(
    fastdb_payload_test_payload_open
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_compile_definitions(
    fastdb_payload_test_payload_open
    PRIVATE
        "FASTDB_PAYLOAD_BINARY_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/binary\""
        "FASTDB_PAYLOAD_CORE_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src/payload\""
)
set_tests_properties(payload.payload_open PROPERTIES TIMEOUT 120)

add_fastdb_payload_test(
    checked_view
    SOURCES payload/test_checked_view.cpp
    EXTRA_LIBRARIES
        fastdb_payload_core
        Threads::Threads
)
target_include_directories(
    fastdb_payload_test_checked_view
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
set_tests_properties(payload.checked_view PROPERTIES TIMEOUT 180)

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    add_executable(
        fastdb_payload_wasm_runtime_harness
        ../wasm/payload_runtime_harness.cpp
    )
    target_include_directories(
        fastdb_payload_wasm_runtime_harness
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
    )
    target_compile_definitions(
        fastdb_payload_wasm_runtime_harness
        PRIVATE
            "FASTDB_PAYLOAD_BINARY_FIXTURE_DIR=\"/payload-binary\""
    )
    target_link_libraries(
        fastdb_payload_wasm_runtime_harness
        PRIVATE fastdb_payload_core
    )
    target_link_options(
        fastdb_payload_wasm_runtime_harness
        PRIVATE
            "-sENVIRONMENT=node"
            "-sEXIT_RUNTIME=1"
            "--embed-file"
            "${CMAKE_CURRENT_SOURCE_DIR}/../golden/payload/v1/binary@/payload-binary"
    )
    set_target_properties(
        fastdb_payload_wasm_runtime_harness
        PROPERTIES
            OUTPUT_NAME payload_runtime_harness
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm-tests"
            SUFFIX ".js"
    )

    add_executable(
        fastdb_payload_wasm_runtime_abi_single_thread
        payload/test_runtime_abi.cpp
    )
    target_include_directories(
        fastdb_payload_wasm_runtime_abi_single_thread
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/payload
            ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
    )
    target_link_libraries(
        fastdb_payload_wasm_runtime_abi_single_thread
        PRIVATE fastdb
    )
    target_link_options(
        fastdb_payload_wasm_runtime_abi_single_thread
        PRIVATE
            "-sENVIRONMENT=node"
            "-sEXIT_RUNTIME=1"
    )
    set_target_properties(
        fastdb_payload_wasm_runtime_abi_single_thread
        PROPERTIES
            OUTPUT_NAME payload_runtime_abi_single_thread
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm-tests"
            SUFFIX ".js"
    )
endif()

add_executable(
    fastdb_payload_test_json_depth
    payload/test_json_depth.cpp
)
target_include_directories(
    fastdb_payload_test_json_depth
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/payload
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fastcarto/fastdb/src
)
target_link_libraries(
    fastdb_payload_test_json_depth
    PRIVATE
        fastdb_payload_core
        Threads::Threads
)
fastdb_apply_sanitizers(fastdb_payload_test_json_depth)

set(FASTDB_JSON_DEPTH_TESTS
    serialize
    destroy
    failure
    copy_move
    object_serialize
    object_destroy
    object_failure
    shared_child_dag
)
foreach(depth_test IN LISTS FASTDB_JSON_DEPTH_TESTS)
    add_test(
        NAME payload.json_depth.${depth_test}
        COMMAND fastdb_payload_test_json_depth ${depth_test}
    )
    set_tests_properties(
        payload.json_depth.${depth_test}
        PROPERTIES TIMEOUT 30
    )
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
   CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME payload.abi_symbols
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/check_payload_abi_symbols.py
            --build-dir
            ${CMAKE_BINARY_DIR}
    )
    set_tests_properties(payload.abi_symbols PROPERTIES TIMEOUT 30)

    add_test(
        NAME payload.binary_fuzz_corpus
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/check_payload_binary_corpus.py
            --check
    )
    set_tests_properties(payload.binary_fuzz_corpus PROPERTIES TIMEOUT 30)

    find_program(FASTDB_RUBY_EXECUTABLE ruby REQUIRED)
    add_test(
        NAME payload.p2_task11_quality
        COMMAND
            ${FASTDB_RUBY_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_p2_task11_quality.rb
            --check-repository
    )
    set_tests_properties(payload.p2_task11_quality PROPERTIES TIMEOUT 30)
endif()
