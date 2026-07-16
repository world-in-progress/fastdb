include(cmake/FastdbCMakeMinimumRequired.cmake)
cmake_minimum_required(VERSION ${FASTDB_CMAKE_VERSION_MIN}...${FASTDB_CMAKE_VERSION_MAX})

project(FastCarto)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

option(
    FASTDB_ENABLE_SANITIZERS
    "Enable address and undefined behavior sanitizers for FastDB native targets"
    OFF
)

if(FASTDB_ENABLE_SANITIZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        message(FATAL_ERROR
            "FASTDB_ENABLE_SANITIZERS requires Clang, AppleClang, or GCC "
            "for both C and C++; got C=${CMAKE_C_COMPILER_ID}, "
            "CXX=${CMAKE_CXX_COMPILER_ID}"
        )
    endif()
endif()

function(fastdb_apply_sanitizers target_name)
    if(FASTDB_ENABLE_SANITIZERS)
        if(NOT TARGET ${target_name})
            message(FATAL_ERROR
                "Cannot apply FastDB sanitizers to missing target ${target_name}"
            )
        endif()
        target_compile_options(
            ${target_name}
            PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
        )
        target_link_options(
            ${target_name}
            PRIVATE
                -fsanitize=address,undefined
        )
    endif()
endfunction()


if(WIN32)
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/source-charset:utf-8>")
    add_compile_options("$<$<C_COMPILER_ID:MSVC>:/source-charset:utf-8>")
endif()

function( add_source_by_dir cur_dir return_list)
    file(GLOB_RECURSE new_files
        ${cur_dir}/*.c
        ${cur_dir}/*.cpp
        ${cur_dir}/*.cxx
        ${cur_dir}/*.h
        ${cur_dir}/*.hpp
        ${cur_dir}/*.hxx
    )
    set(${return_list} ${new_files} PARENT_SCOPE)

endfunction(add_source_by_dir)

set(LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(is_emscripten ON)
endif()

add_subdirectory(lib)
add_subdirectory(fastdb)
option(BUILD_TOOLS "Build CLI tools" ON)

if (NOT is_emscripten AND BUILD_TOOLS)
    add_subdirectory(make-fastdb)
    add_subdirectory(dump-fastdb)
endif()

if(BUILD_TESTING)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/cpp
        ${CMAKE_CURRENT_BINARY_DIR}/tests/cpp
    )
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY   ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY   ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY   ${CMAKE_SOURCE_DIR}/bin)
