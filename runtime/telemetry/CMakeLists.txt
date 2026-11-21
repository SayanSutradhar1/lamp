cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework - Phase 12 Telemetry & Observability
#
# File: runtime/telemetry/CMakeLists.txt
# ============================================================

find_package(Threads REQUIRED)

if(NOT TARGET lamp::bridge)
    message(FATAL_ERROR
        "Lamp Telemetry requires lamp::bridge."
    )
endif()

if(NOT TARGET lamp::http)
    message(FATAL_ERROR
        "Lamp Telemetry requires lamp::http."
    )
endif()

add_library(lamp_telemetry INTERFACE)
add_library(lamp::telemetry ALIAS lamp_telemetry)

target_compile_features(
    lamp_telemetry
    INTERFACE
        cxx_std_20
)

target_include_directories(
    lamp_telemetry
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include/lamp/runtime>
)

target_link_libraries(
    lamp_telemetry
    INTERFACE
        lamp::bridge
        lamp::http
        Threads::Threads
)

target_sources(
    lamp_telemetry
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/metrics_registry.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tracing.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/health_check.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/telemetry_dispatcher.hpp"
)

if(LAMP_BUILD_TESTS)

    add_executable(
        lamp_telemetry_test
        tests/telemetry_test.cpp
    )

    target_compile_features(
        lamp_telemetry_test
        PRIVATE
            cxx_std_20
    )

    set_target_properties(
        lamp_telemetry_test
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            FOLDER "Lamp/Tests/Telemetry"
    )

    target_link_libraries(
        lamp_telemetry_test
        PRIVATE
            lamp::telemetry
            lamp::cache
    )

    if(MSVC)
        target_compile_options(
            lamp_telemetry_test
            PRIVATE
                /W4
                /permissive-
        )
        set_property(
            TARGET lamp_telemetry_test
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            lamp_telemetry_test
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    if(WIN32)
        target_compile_definitions(
            lamp_telemetry_test
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )
    endif()

    add_test(
        NAME lamp_telemetry_test
        COMMAND lamp_telemetry_test
    )

    set_tests_properties(
        lamp_telemetry_test
        PROPERTIES
            TIMEOUT 30
            LABELS "lamp;telemetry;observability;health"
    )

    message(STATUS
        "[Lamp Telemetry] Registered: lamp_telemetry_test"
    )

endif()
