add_library(fastdb_java SHARED
    JniSupport.cpp
    SpecJni.cpp
    BuilderJni.cpp
    PayloadJni.cpp
    ViewJni.cpp
    AccessJni.cpp
)
target_link_libraries(fastdb_java PRIVATE fastdb_payload_native)

set(jni_targets fastdb_java)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_library(fastdb_java_test SHARED ../src/test/native/NativeBackingTest.cpp)
    target_include_directories(fastdb_java_test PRIVATE "${PROJECT_DIR}/include")
    target_link_libraries(fastdb_java_test PRIVATE fastdb_java Threads::Threads)
    list(APPEND jni_targets fastdb_java_test)
endif()

foreach(target IN LISTS jni_targets)
    target_include_directories(${target} SYSTEM PRIVATE ${JNI_INCLUDE_DIRS})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    fastdb_apply_sanitizers(${target})
endforeach()
