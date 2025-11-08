cmake_minimum_required (VERSION 3.30)
set(CUR_PROJ_NAME 	gpc)
set(CUR_PROJ_DIR	${LIB_DIR}/gpc)

#添加源代码
add_source_by_dir(${CUR_PROJ_DIR}   all_files)
#设置包含目录
include_directories(CUR_PROJ_DIR)
#设置生成库
add_library(${CUR_PROJ_NAME} STATIC ${all_files})

#target_link_libraries(${CUR_PROJ_NAME} uid qlock mpConfigure)
#设置链接目录
link_directories(${BIN_DIR})
#设置vs工程属性
SET_PROPERTY(TARGET ${CUR_PROJ_NAME} PROPERTY FOLDER "lib")