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

add_fastdb_payload_test(
    dependency_smoke
    SOURCES payload/test_dependency_smoke.cpp
    EXTRA_LIBRARIES
        fastdb_yyjson
        fastdb_double_conversion
        fastdb_picosha2
)
