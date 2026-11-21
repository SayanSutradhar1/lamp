
cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework
# Native Execution Bridge Tests
#
# File: runtime/bridge/tests/CMakeLists.txt
#
# Tests:
#   - Native bridge integration
#   - Async operation coordinator
#
# Requirements:
#   - C++20
#   - lamp::bridge
#   - lamp::http (integration test only)
#   - Threads::Threads
#   - CTest enabled at project level
#
# The bridge components are header-only.
# V8 is not required by these tests.
# ============================================================

if(NOT LAMP_BUILD_TESTS)
    return()
endif()

# ------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------

find_package(Threads REQUIRED)

if(NOT TARGET lamp::bridge)
    message(FATAL_ERROR
        "Lamp bridge tests require lamp::bridge."
    )
endif()

if(NOT TARGET lamp::http)
    message(FATAL_ERROR
        "Lamp bridge integration tests require lamp::http. "
        "Configure runtime/http before runtime/bridge."
    )
endif()

# ------------------------------------------------------------
# Shared test target configuration
# ------------------------------------------------------------

function(lamp_configure_bridge_test target)

    target_compile_features(
        ${target}
        PRIVATE
            cxx_std_20
    )

    set_target_properties(
        ${target}
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            FOLDER "Lamp/Tests/Bridge"
    )

    target_include_directories(
        ${target}
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    )

    target_link_libraries(
        ${target}
        PRIVATE
            lamp::bridge
            Threads::Threads
    )

    if(MSVC)

        target_compile_options(
            ${target}
            PRIVATE
                /W4
                /permissive-
        )

        set_property(
            TARGET ${target}
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )

    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")

        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

    endif()

    if(WIN32)

        target_compile_definitions(
            ${target}
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )

    endif()

endfunction()

# ------------------------------------------------------------
# Existing native bridge integration test
# ------------------------------------------------------------

add_executable(
    lamp_bridge_integration_test
    bridge_integration_test.cpp
)

lamp_configure_bridge_test(
    lamp_bridge_integration_test
)

target_link_libraries(
    lamp_bridge_integration_test
    PRIVATE
        lamp::http
)

add_test(
    NAME lamp_bridge_integration
    COMMAND lamp_bridge_integration_test
)

set_tests_properties(
    lamp_bridge_integration
    PROPERTIES
        TIMEOUT 30
        LABELS "lamp;bridge;integration"
)

# ------------------------------------------------------------
# Phase 6B async operation coordinator test
# ------------------------------------------------------------

add_executable(
    lamp_async_operation_coordinator_test
    async_operation_coordinator_test.cpp
)

lamp_configure_bridge_test(
    lamp_async_operation_coordinator_test
)

add_test(
    NAME lamp_async_operation_coordinator
    COMMAND lamp_async_operation_coordinator_test
)

set_tests_properties(
    lamp_async_operation_coordinator
    PROPERTIES
        TIMEOUT 30
        LABELS "lamp;bridge;async;coordinator"
)

# ------------------------------------------------------------
# Phase 6B async HTTP operation pump & adapter test
# ------------------------------------------------------------

if(TARGET lamp::bridge_http)

    add_executable(
        lamp_async_http_operation_pump_test
        async_http_operation_pump_test.cpp
    )

    lamp_configure_bridge_test(
        lamp_async_http_operation_pump_test
    )

    target_link_libraries(
        lamp_async_http_operation_pump_test
        PRIVATE
            lamp::bridge_http
    )

    if(WIN32)
        target_link_libraries(
            lamp_async_http_operation_pump_test
            PRIVATE
                ws2_32
        )
    endif()

    add_test(
        NAME lamp_async_http_operation_pump
        COMMAND lamp_async_http_operation_pump_test
    )

    set_tests_properties(
        lamp_async_http_operation_pump
        PROPERTIES
            TIMEOUT 30
            LABELS "lamp;bridge;http;async;pump;integration"
    )

    message(STATUS
        "[Lamp Bridge Tests] Registered: "
        "lamp_async_http_operation_pump"
    )

endif()

# ------------------------------------------------------------
# Phase 11 V8 <-> SDK Host Bridge test
# ------------------------------------------------------------

if(LAMP_ENABLE_V8 AND TARGET lamp::bridge_v8 AND TARGET lamp::http)

    add_executable(
        lamp_v8_sdk_host_bridge_test
        v8_sdk_host_bridge_test.cpp
    )

    lamp_configure_bridge_test(
        lamp_v8_sdk_host_bridge_test
    )

    target_link_libraries(
        lamp_v8_sdk_host_bridge_test
        PRIVATE
            lamp::bridge_v8
            lamp::bridge_http
    )

    add_test(
        NAME lamp_v8_sdk_host_bridge
        COMMAND lamp_v8_sdk_host_bridge_test
    )

    set_tests_properties(
        lamp_v8_sdk_host_bridge
        PROPERTIES
            TIMEOUT 30
            LABELS "lamp;bridge;v8;sdk;integration"
    )

    message(STATUS
        "[Lamp Bridge Tests] Registered: "
        "lamp_v8_sdk_host_bridge"
    )

endif()

# ------------------------------------------------------------
# IDE organization
# ------------------------------------------------------------

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        bridge_integration_test.cpp
        async_operation_coordinator_test.cpp
        async_http_operation_pump_test.cpp
)

# ------------------------------------------------------------
# Configuration summary
# ------------------------------------------------------------

message(STATUS
    "[Lamp Bridge Tests] Registered: "
    "lamp_bridge_integration"
)

message(STATUS
    "[Lamp Bridge Tests] Registered: "
    "lamp_async_operation_coordinator"
)

# ============================================================
# End of Lamp Bridge Test Configuration
# ============================================================
