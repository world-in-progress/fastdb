
add_subdirectory(gpc)
add_subdirectory(gaiageo)
add_subdirectory(clipper)

add_library(fastdb_yyjson STATIC yyjson/src/yyjson.c)
target_include_directories(
    fastdb_yyjson
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/yyjson/src
)
target_compile_definitions(
    fastdb_yyjson
    PRIVATE
        YYJSON_DISABLE_NON_STANDARD=1
        YYJSON_DISABLE_UTILS=1
)
set_target_properties(
    fastdb_yyjson
    PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
)
fastdb_apply_sanitizers(fastdb_yyjson)

set(DOUBLE_CONVERSION_SOURCES
    double-conversion/double-conversion/bignum.cc
    double-conversion/double-conversion/bignum-dtoa.cc
    double-conversion/double-conversion/cached-powers.cc
    double-conversion/double-conversion/double-to-string.cc
    double-conversion/double-conversion/fast-dtoa.cc
    double-conversion/double-conversion/fixed-dtoa.cc
    double-conversion/double-conversion/string-to-double.cc
    double-conversion/double-conversion/strtod.cc
)
add_library(fastdb_double_conversion STATIC ${DOUBLE_CONVERSION_SOURCES})
target_include_directories(
    fastdb_double_conversion
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/double-conversion
)
set_target_properties(
    fastdb_double_conversion
    PROPERTIES
        POSITION_INDEPENDENT_CODE ON
)
fastdb_apply_sanitizers(fastdb_double_conversion)

add_library(fastdb_picosha2 INTERFACE)
target_include_directories(
    fastdb_picosha2
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/picosha2
)
