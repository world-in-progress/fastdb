project(fastdb)
set(PROJECT_NAME fastdb)
set(PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(USE_SWIG_PYTHON "use swig python wrapper" OFF)
option(USE_SWIG_NODE   "use swig node wrapper" OFF)
option(USE_SWIG_GO     "use swig go wrapper"   OFF)

add_definitions(-DFASTDB_EXPORT)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${LIB_DIR}/gaiageo/headers)
include_directories(${LIB_DIR}/gaiageo/headers/spatialite)
include_directories(${LIB_DIR}/clipper/Clipper2Lib/include)
include_directories(${LIB_DIR}/clipper/Clipper2Lib/include/clipper2)

add_source_by_dir(${PROJECT_DIR} SOURCES)

file(GLOB_RECURSE EXCLUED_FILES ${PROJECT_DIR}/swig/*.*)
list(REMOVE_ITEM SOURCES ${EXCLUED_FILES})

if(is_emscripten)
    set(BUILD_TYPE STATIC)
else()
    set(BUILD_TYPE SHARED)
endif(is_emscripten)

add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE gaiageo Clipper2)

get_target_property(swig_out_dir ${PROJECT_NAME} SWIG_OUTPUT_DIR)

set(swig_dir   ${PROJECT_DIR}/swig)
set(script_dir ${ROOT_DIR}/scripts)

if (NOT is_emscripten)
    if(USE_SWIG_PYTHON)
        set(script_dir ${ROOT_DIR}/../python/fastdb4py/core)
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
        message(STATUS numpy_dir=${Python3_NumPy_INCLUDE_DIRS})
        message(STATUS python_libs=${Python3_LIBRARY_DIRS})
        add_definitions(-DUSE_SWIG_PYTHON -DSWIG_PYTHON_INTERPRETER_NO_DEBUG)

        add_library(fastdb4py SHARED ${SWIG_GENERATED_DIR}/fastdb4py_wrap.cxx)
        target_link_libraries(fastdb4py PRIVATE ${PROJECT_NAME})
        target_link_directories(fastdb4py PRIVATE ${Python3_LIBRARY_DIRS})

        target_link_options(fastdb4py
            PUBLIC
                $<$<BOOL:${APPLE}>:-undefineddynamic_lookup>)
        if(WIN32)
            set(pyd pyd)
        else()
            set(pyd so)
        endif(WIN32)
        
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
        include_directories(${ROOT_DIR}/node_modules/node-api-headers/include/)
        add_library(fastdb4node SHARED ${PROJECT_DIR}/swig/fastdb4node.cxx)
        target_link_libraries(fastdb4node PRIVATE ${PROJECT_NAME})

        target_link_options(fastdb4node
            PUBLIC
                $<$<BOOL:${APPLE}>:-undefineddynamic_lookup>)
        
        if(WIN32)
            target_link_libraries(fastdb4node PRIVATE ${ROOT_DIR}/node_modules/node-api-headers/lib/node_api.lib)
            target_link_libraries(fastdb4node PRIVATE ${ROOT_DIR}/node_modules/node-api-headers/lib/js_native_api.lib)
        endif()


        add_custom_command(TARGET fastdb4node POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fastdb4node>  ${script_dir}/_fastdb.node
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}>  ${script_dir}/$<TARGET_FILE_NAME:${PROJECT_NAME}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${swig_dir}/fastdb.js  ${script_dir}/fastdb.js
            
            COMMENT "updating fastdb4node into swig directory"
        )
    endif(USE_SWIG_NODE)

    if(USE_SWIG_GO)
        set(SWIG_GO_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../go/fastdb4go/core)
        file(MAKE_DIRECTORY ${SWIG_GO_OUT_DIR})
        
        add_custom_command(
            OUTPUT ${SWIG_GO_OUT_DIR}/fastdb4go_wrap.cxx 
                   ${SWIG_GO_OUT_DIR}/fastdb4go.go
            COMMAND swig -c++ -go -cgo -intgosize 64
                    -I${PROJECT_DIR}/include
                    -I${PROJECT_DIR}/swig
                    -outdir ${SWIG_GO_OUT_DIR}
                    -o ${SWIG_GO_OUT_DIR}/fastdb4go_wrap.cxx
                    ${PROJECT_DIR}/swig/fastdb4go.i
            DEPENDS ${PROJECT_DIR}/swig/fastdb4go.i
                    ${PROJECT_DIR}/include/fastdb.h
            COMMENT "Generating SWIG Go bindings"
        )
        
        add_custom_target(fastdb_go_bindings ALL DEPENDS ${SWIG_GO_OUT_DIR}/fastdb4go.go)
        message(STATUS "Go bindings will be generated in ${SWIG_GO_OUT_DIR}")

        add_custom_command(TARGET fastdb_go_bindings POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}> ${SWIG_GO_OUT_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_DIR}/include/fastdb.h ${SWIG_GO_OUT_DIR}/include/fastdb.h
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_DIR}/include/fastdb-geometry-utils.h ${SWIG_GO_OUT_DIR}/include/fastdb-geometry-utils.h
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_DIR}/include/fastdb-config.h ${SWIG_GO_OUT_DIR}/include/fastdb-config.h
            COMMENT "Copying fastdb library and headers to Go directory"
        )
    endif(USE_SWIG_GO)
else()
    message(STATUS "linking swig wrapper...") 
     add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
     COMMAND emcc 
            "-O2" 
            "-s" "WASM=0" 
            #"-s" "MODULARIZE=1" 
            #"-s" EXPORT_NAME="'fastdb'"
            #"-s" 'EXTRA_EXPORTED_RUNTIME_METHODS=["cwrap","_free","_malloc"]'
            "-s" "ALLOW_MEMORY_GROWTH=1" 
            "-s" "INITIAL_MEMORY=16777216" 
            "-s" "TOTAL_MEMORY=16777216" 
            "-s" "TOTAL_STACK=5242880" 
            "-s" "ERROR_ON_UNDEFINED_SYMBOLS=0" 
            "--post-js" "${swig_dir}/fastdb4em_i.js" 
            "-I" ${CMAKE_CURRENT_SOURCE_DIR}/include
            "$<TARGET_FILE:${PROJECT_NAME}>"    "${swig_dir}/fastdb4em.cxx"
            "-o" "${script_dir}/fastdb4em.js"
     COMMENT "emcc linking fastdb.js"
     )
endif()