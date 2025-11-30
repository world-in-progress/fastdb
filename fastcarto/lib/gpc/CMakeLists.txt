# cmake_minimum_required (VERSION 3.27)
set(CUR_PROJ_NAME 	gpc)
set(CUR_PROJ_DIR	${LIB_DIR}/gpc)

#����Դ����
add_source_by_dir(${CUR_PROJ_DIR}   all_files)
#���ð���Ŀ¼
include_directories(CUR_PROJ_DIR)
#�������ɿ�
add_library(${CUR_PROJ_NAME} STATIC ${all_files})

#target_link_libraries(${CUR_PROJ_NAME} uid qlock mpConfigure)
#��������Ŀ¼
link_directories(${BIN_DIR})
#����vs��������
SET_PROPERTY(TARGET ${CUR_PROJ_NAME} PROPERTY FOLDER "lib")