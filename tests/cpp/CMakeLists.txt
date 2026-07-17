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
    EXTRA_LIBRARIES fastdb_payload_core
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
endif()
