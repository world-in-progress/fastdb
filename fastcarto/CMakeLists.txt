include(cmake/FastdbCMakeMinimumRequired.cmake)
cmake_minimum_required(VERSION ${FASTDB_CMAKE_VERSION_MIN}...${FASTDB_CMAKE_VERSION_MAX})

project(FastCarto)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY   ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY   ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY   ${CMAKE_SOURCE_DIR}/bin)