project(make-fastdb)
set(PROJECT_NAME make-fastdb)
set(PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /arch:AVX2 /DNDEBUG")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2 /arch:AVX2 /DNDEBUG")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -DNDEBUG")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -march=native -DNDEBUG")
endif()


find_package(GDAL REQUIRED)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${ROOT_DIR}/fastdb/include)
include_directories(${GDAL_INCLUDE_DIRS})

add_source_by_dir(${PROJECT_DIR} SOURCES)


add_executable(${PROJECT_NAME}   ${SOURCES} )

target_link_libraries(${PROJECT_NAME} fastdb ${GDAL_LIBRARIES})

#message(STATUS ${GDAL_INCLUDE_DIRS})
#message(STATUS ${GDAL_LIBRARIES})





