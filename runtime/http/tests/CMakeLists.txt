
# ============================================================
# Lamp Framework - Native HTTP Test Suite
# ============================================================

cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------
# Preconditions
# ------------------------------------------------------------

if(NOT TARGET lamp_http)
    message(FATAL_ERROR
        "[Lamp HTTP Tests] Required target 'lamp_http' "
        "does not exist. Configure runtime/http first."
    )
endif()

if(NOT LAMP_BUILD_TESTS)
    message(STATUS
        "[Lamp HTTP Tests] Lamp testing is disabled."
    )
    return()
endif()

# Ensure CTest is available without reconfiguring the
# project's global testing policy.
if(NOT COMMAND add_test)
    message(FATAL_ERROR
        "[Lamp HTTP Tests] CTest is not enabled. "
        "Call enable_testing() in the root CMakeLists.txt."
    )
endif()

# ------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------

find_package(Threads REQUIRED)

# ------------------------------------------------------------
# Test Configuration
# ------------------------------------------------------------

set(LAMP_HTTP_TEST_TIMEOUT 30
    CACHE STRING
    "Timeout in seconds for each Lamp HTTP test executable"
)

set(LAMP_HTTP_TEST_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

# ------------------------------------------------------------
# Test Registration Function
# ------------------------------------------------------------

function(lamp_add_http_test target source)

    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        message(FATAL_ERROR
            "[Lamp HTTP Tests] Missing test source: ${source}"
        )
    endif()

    # --------------------------------------------------------
    # Executable
    # --------------------------------------------------------

    add_executable(
        ${target}
        "${source}"
    )

    # --------------------------------------------------------
    # Language Standard
    # --------------------------------------------------------

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
            RUNTIME_OUTPUT_DIRECTORY
                "${LAMP_HTTP_TEST_OUTPUT_DIRECTORY}"
            FOLDER "Lamp/Tests/HTTP"
    )

    # --------------------------------------------------------
    # Dependencies
    # --------------------------------------------------------

    target_link_libraries(
        ${target}
        PRIVATE
            lamp::http
            Threads::Threads
    )

    # The HTTP public headers are located under runtime/http.
    # Tests include them as "http/http_parser.hpp", etc.
    target_include_directories(
        ${target}
        PRIVATE
            "${PROJECT_SOURCE_DIR}/runtime"
    )

    # --------------------------------------------------------
    # Compiler Warnings
    # --------------------------------------------------------

    if(MSVC)

        target_compile_options(
            ${target}
            PRIVATE
                /W4
                /permissive-
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

    # --------------------------------------------------------
    # Windows Configuration
    # --------------------------------------------------------

    if(WIN32)

        target_compile_definitions(
            ${target}
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )

        # The HTTP server integration test creates native
        # Winsock TCP client sockets.
        target_link_libraries(
            ${target}
            PRIVATE
                ws2_32
        )

    endif()

    # --------------------------------------------------------
    # CTest Registration
    # --------------------------------------------------------

    add_test(
        NAME
            ${target}
        COMMAND
            $<TARGET_FILE:${target}>
    )

    set_tests_properties(
        ${target}
        PROPERTIES
            TIMEOUT "${LAMP_HTTP_TEST_TIMEOUT}"
            LABELS "lamp;http;native"
            WORKING_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}"
    )

    message(STATUS
        "[Lamp HTTP Tests] Registered: ${target}"
    )

endfunction()

# ------------------------------------------------------------
# HTTP Parser Tests
# ------------------------------------------------------------

lamp_add_http_test(
    lamp_http_parser_test
    http_parser_test.cpp
)

# ------------------------------------------------------------
# HTTP Router Tests
# ------------------------------------------------------------

lamp_add_http_test(
    lamp_http_router_test
    http_router_test.cpp
)

# ------------------------------------------------------------
# HTTP Connection Tests
# ------------------------------------------------------------

lamp_add_http_test(
    lamp_http_connection_test
    http_connection_test.cpp
)

# ------------------------------------------------------------
# HTTP Server Integration Tests
# ------------------------------------------------------------

lamp_add_http_test(
    lamp_http_server_test
    http_server_test.cpp
)

# Server tests involve real TCP sockets, concurrency,
# connection timeouts, and graceful shutdown.
set_tests_properties(
    lamp_http_server_test
    PROPERTIES
        TIMEOUT 90
        LABELS "lamp;http;native;integration"
)

# ------------------------------------------------------------
# Build Convenience Target
# ------------------------------------------------------------

add_custom_target(
    lamp_http_tests
    DEPENDS
        lamp_http_parser_test
        lamp_http_router_test
        lamp_http_connection_test
        lamp_http_server_test
)

set_target_properties(
    lamp_http_tests
    PROPERTIES
        FOLDER "Lamp/Tests/HTTP"
)

# ------------------------------------------------------------
# HTTP Tests
# ------------------------------------------------------------

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()


# ------------------------------------------------------------
# Configuration Summary
# ------------------------------------------------------------

message(STATUS "")
message(STATUS "========================================")
message(STATUS " Lamp HTTP Test Suite")
message(STATUS "========================================")
message(STATUS " Parser Tests     : Enabled")
message(STATUS " Router Tests     : Enabled")
message(STATUS " Connection Tests : Enabled")
message(STATUS " Server Tests     : Enabled")
message(STATUS " Test Timeout     : ${LAMP_HTTP_TEST_TIMEOUT}s")
message(STATUS "========================================")
message(STATUS "")
