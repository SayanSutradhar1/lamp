cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework - Phase 10 Security & Reliability Engine
#
# File: runtime/security/CMakeLists.txt
# ============================================================

find_package(Threads REQUIRED)

if(NOT TARGET lamp::bridge)
    message(FATAL_ERROR
        "Lamp Security requires lamp::bridge."
    )
endif()

if(NOT TARGET lamp::http)
    message(FATAL_ERROR
        "Lamp Security requires lamp::http."
    )
endif()

add_library(lamp_security INTERFACE)
add_library(lamp::security ALIAS lamp_security)

target_compile_features(
    lamp_security
    INTERFACE
        cxx_std_20
)

target_include_directories(
    lamp_security
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include/lamp/runtime>
)

target_link_libraries(
    lamp_security
    INTERFACE
        lamp::bridge
        lamp::http
        Threads::Threads
)

target_sources(
    lamp_security
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/crypto_utils.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/security_context.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/input_sanitizer.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/auth_policy.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/rate_limiter.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/security_gateway.hpp"
)

if(LAMP_BUILD_TESTS)

    add_executable(
        lamp_security_test
        tests/security_test.cpp
    )

    target_compile_features(
        lamp_security_test
        PRIVATE
            cxx_std_20
    )

    set_target_properties(
        lamp_security_test
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            FOLDER "Lamp/Tests/Security"
    )

    target_link_libraries(
        lamp_security_test
        PRIVATE
            lamp::security
    )

    if(MSVC)
        target_compile_options(
            lamp_security_test
            PRIVATE
                /W4
                /permissive-
        )
        set_property(
            TARGET lamp_security_test
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            lamp_security_test
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    if(WIN32)
        target_compile_definitions(
            lamp_security_test
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )
    endif()

    add_test(
        NAME lamp_security_test
        COMMAND lamp_security_test
    )

    set_tests_properties(
        lamp_security_test
        PROPERTIES
            TIMEOUT 30
            LABELS "lamp;security;auth;ratelimit;resilience"
    )

    message(STATUS
        "[Lamp Security] Registered: lamp_security_test"
    )

endif()
