
cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework - Phase 8 Caching Engine
#
# File: runtime/cache/CMakeLists.txt
# ============================================================

find_package(Threads REQUIRED)

if(NOT TARGET lamp::bridge)
    message(FATAL_ERROR
        "Lamp Cache requires lamp::bridge."
    )
endif()

add_library(lamp_cache INTERFACE)
add_library(lamp::cache ALIAS lamp_cache)

target_compile_features(
    lamp_cache
    INTERFACE
        cxx_std_20
)

target_include_directories(
    lamp_cache
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include/lamp/runtime>
)

target_link_libraries(
    lamp_cache
    INTERFACE
        lamp::bridge
        Threads::Threads
)

target_sources(
    lamp_cache
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/cache_key.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/local_operation_cache.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/layered_operation_cache.hpp"
)

if(LAMP_BUILD_TESTS)

    add_executable(
        lamp_cache_test
        tests/cache_test.cpp
    )

    target_compile_features(
        lamp_cache_test
        PRIVATE
            cxx_std_20
    )

    set_target_properties(
        lamp_cache_test
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            FOLDER "Lamp/Tests/Cache"
    )

    target_link_libraries(
        lamp_cache_test
        PRIVATE
            lamp::cache
    )

    if(MSVC)
        target_compile_options(
            lamp_cache_test
            PRIVATE
                /W4
                /permissive-
        )
        set_property(
            TARGET lamp_cache_test
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            lamp_cache_test
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    if(WIN32)
        target_compile_definitions(
            lamp_cache_test
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )
    endif()

    add_test(
        NAME lamp_cache_test
        COMMAND lamp_cache_test
    )

    set_tests_properties(
        lamp_cache_test
        PROPERTIES
            TIMEOUT 30
            LABELS "lamp;cache;performance"
    )

    message(STATUS
        "[Lamp Cache] Registered: lamp_cache_test"
    )

endif()
