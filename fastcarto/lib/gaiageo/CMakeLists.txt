# cmake_minimum_required (VERSION 3.27)
set(CUR_PROJ_NAME 	gaiageo)
set(CUR_PROJ_DIR	${LIB_DIR}/gaiageo)


if(MSVC)
    add_definitions(-DYY_NO_UNISTD_H)
endif(MSVC)

add_source_by_dir(${CUR_PROJ_DIR}   all_files)
#设置包含目录
include_directories(${CUR_PROJ_DIR}/headers)
include_directories(${CUR_PROJ_DIR}/headers/spatialite)
include_directories(${CUR_PROJ_DIR}/src)
#设置生成库
add_library(${CUR_PROJ_NAME} STATIC ${all_files})
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

#设置链接目录
link_directories(${BIN_DIR})
#设置vs工程属性
SET_PROPERTY(TARGET ${CUR_PROJ_NAME} PROPERTY FOLDER "lib")