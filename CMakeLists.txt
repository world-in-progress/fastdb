cmake_minimum_required(VERSION 3.30)
#set(CMAKE_TOOLCHAIN_FILE /Users/laowine-x/code/vcpkg/scripts/buildsystems/vcpkg.cmake)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(FastCarto)

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


add_subdirectory(lib)
add_subdirectory(fastdb)
add_subdirectory(make-fastdb)
add_subdirectory(dump-fastdb)