project(dump-fastdb)
set(PROJECT_NAME dump-fastdb)
set(PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -march=native -DNDEBUG")


include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${ROOT_DIR}/fastdb/include)
include_directories(${GDAL_INCLUDE_DIRS})

add_source_by_dir(${PROJECT_DIR} SOURCES)


add_executable(${PROJECT_NAME}   ${SOURCES} )

target_link_libraries(${PROJECT_NAME} fastdb)

#message(STATUS ${GDAL_INCLUDE_DIRS})
#message(STATUS ${GDAL_LIBRARIES})





