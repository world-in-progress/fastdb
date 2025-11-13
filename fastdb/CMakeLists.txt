project(fastdb)
set(PROJECT_NAME fastdb)
set(PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(USE_SWIG_PYTHON "use swig python wrapper" OFF)
option(USE_SWIG_NODE   "use swig node wrapper" OFF)

add_definitions(-DFASTDB_EXPORT)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${LIB_DIR}/gaiageo/headers/spatialite)
include_directories(${LIB_DIR}/gaiageo/headers)
include_directories(${LIB_DIR}/clipper/Clipper2Lib/include)
include_directories(${LIB_DIR}/clipper/Clipper2Lib/include/clipper2)

add_source_by_dir(${PROJECT_DIR} SOURCES)

file(GLOB_RECURSE EXCLUED_FILES ${PROJECT_DIR}/swig/*.*)
list(REMOVE_ITEM SOURCES ${EXCLUED_FILES})

add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE gaiageo Clipper2)

get_target_property(swig_out_dir ${PROJECT_NAME} SWIG_OUTPUT_DIR)

set(swig_dir   ${PROJECT_DIR}/swig)
set(script_dir ${ROOT_DIR}/scripts)

if(USE_SWIG_PYTHON)
    set(script_dir ${ROOT_DIR}/python/fastdb/core)
    set(SWIG_GENERATED_DIR ${CMAKE_BINARY_DIR}/swig_generated)

    # Copy SWIG generated files to the build directory
    add_custom_command(
        OUTPUT ${SWIG_GENERATED_DIR}/fastdb4py_wrap.cxx 
            ${SWIG_GENERATED_DIR}/fastdb4py.py
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SWIG_GENERATED_DIR}
        COMMAND swig -c++ -python 
                -I${PROJECT_DIR}/include
                -I${PROJECT_DIR}/swig
                -outdir ${SWIG_GENERATED_DIR}
                -o ${SWIG_GENERATED_DIR}/fastdb4py_wrap.cxx
                ${PROJECT_DIR}/swig/fastdb4py.i
        DEPENDS ${PROJECT_DIR}/swig/fastdb4py.i
                ${PROJECT_DIR}/include/fastdb.h
                ${PROJECT_DIR}/include/fastdb-geometry-utils.h
        COMMENT "Generating SWIG Python bindings"
    )

    set(Python3_FIND_VIRTUALENV FIRST)
    find_package(Python3 ${PYTHON_VERSION_STRING}  COMPONENTS Interpreter  Development NumPy)
    include_directories(${Python3_INCLUDE_DIRS} )
    include_directories(${Python3_NumPy_INCLUDE_DIRS} )
    message(STATUS "building fastdb with swig python wrapper support!")
    message(STATUS python_dir=${Python3_INCLUDE_DIRS})
    message(STATUS numpy_dir=${Python3_NumPy_INCLUDE_DIRS})
    add_definitions(-DUSE_SWIG_PYTHON)
    # endif(USE_SWIG_PYTHON)
    # if(USE_SWIG_PYTHON)
    add_library(fastdb4py SHARED ${SWIG_GENERATED_DIR}/fastdb4py_wrap.cxx)
    target_link_libraries(fastdb4py PRIVATE ${PROJECT_NAME})

    target_link_options(fastdb4py
        PUBLIC
            $<$<BOOL:${APPLE}>:-undefineddynamic_lookup>)
    
    if(WINDOWS)
        set(pyd pyd)
    else()
        set(pyd so)
    endif(WINDOWS)
     
    add_custom_command(TARGET fastdb4py POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fastdb4py>  ${script_dir}/_fastdb4py.${pyd}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}>  ${script_dir}/$<TARGET_FILE_NAME:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SWIG_GENERATED_DIR}/fastdb4py.py  ${script_dir}/__init__.py
        COMMENT "updating fastdb4py into scripts directory"
    )

else()
    message(STATUS "if you want swig python support please check USE_SWIG_PYTHON!")
endif(USE_SWIG_PYTHON)

if(USE_SWIG_NODE)
    include_directories(${ROOT_DIR}/node_modules/node-addon-api)
    include_directories(${ROOT_DIR}/node_modules/node-api-headers/include)
    add_library(fastdb4node SHARED ${PROJECT_DIR}/swig/fastdb4node.cxx)
    target_link_libraries(fastdb4node PRIVATE ${PROJECT_NAME})

    target_link_options(fastdb4node
        PUBLIC
            $<$<BOOL:${APPLE}>:-undefineddynamic_lookup>)
     

    add_custom_command(TARGET fastdb4node POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fastdb4node>  ${script_dir}/_fastdb.node
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}>  ${script_dir}/$<TARGET_FILE_NAME:${PROJECT_NAME}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${swig_dir}/fastdb.js  ${script_dir}/fastdb.js
        
        COMMENT "updating fastdb4node into swig directory"
    )
endif(USE_SWIG_NODE)







