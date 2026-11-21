
cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Native HTTP Engine
# ============================================================
#
# Native HTTP/1.x infrastructure:
#
#   http_method.hpp
#   http_headers.hpp
#   http_request.hpp
#   http_response.hpp
#   http_router.hpp
#   http_parser.hpp
#   http_connection.hpp
#   http_async_connection.hpp
#   http_server.hpp
#   http_server.cpp
#   http_middleware.hpp
#   http_route_group.hpp
#   http_completion_queue.hpp
#
# Public headers:
#
#   #include "http/http_server.hpp"
#   #include "http/http_async_connection.hpp"
#
# This component is independent of V8 and the
# public TypeScript application interface.
#
# ============================================================

# ------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------

find_package(Threads REQUIRED)

# ------------------------------------------------------------
# HTTP Library
# ------------------------------------------------------------

add_library(lamp_http STATIC
    http_server.cpp
    http_async_server.cpp
    http_method.hpp
    http_headers.hpp
    http_request.hpp
    http_response.hpp
    http_router.hpp
    http_parser.hpp
    http_connection.hpp
    http_async_connection.hpp
    http_server.hpp
    http_async_server.hpp
    http_middleware.hpp
    http_route_group.hpp
    http_completion_queue.hpp
)

add_library(lamp::http ALIAS lamp_http)

# ------------------------------------------------------------
# Language Standard
# ------------------------------------------------------------

target_compile_features(lamp_http
    PUBLIC
        cxx_std_20
)

set_target_properties(lamp_http PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# ------------------------------------------------------------
# Public Include Directories
# ------------------------------------------------------------

target_include_directories(lamp_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include/lamp/runtime>
)

# ------------------------------------------------------------
# Threading
# ------------------------------------------------------------

target_link_libraries(lamp_http
    PUBLIC
        Threads::Threads
)

# ------------------------------------------------------------
# Platform Networking
# ------------------------------------------------------------

if(WIN32)
    target_link_libraries(lamp_http
        PRIVATE
            ws2_32
    )
endif()

# ------------------------------------------------------------
# Compiler Warnings
# ------------------------------------------------------------

if(MSVC)
    target_compile_options(lamp_http
        PRIVATE
            /W4
            /permissive-
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lamp_http
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

# ------------------------------------------------------------
# Platform Definitions
# ------------------------------------------------------------

if(WIN32)
    target_compile_definitions(lamp_http
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
endif()

# ------------------------------------------------------------
# IDE Organization
# ------------------------------------------------------------

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        http_server.cpp
        http_async_server.cpp
        http_method.hpp
        http_headers.hpp
        http_request.hpp
        http_response.hpp
        http_router.hpp
        http_parser.hpp
        http_connection.hpp
        http_async_connection.hpp
        http_server.hpp
        http_async_server.hpp
        http_middleware.hpp
        http_route_group.hpp
        http_completion_queue.hpp
)

# ------------------------------------------------------------
# Target Metadata
# ------------------------------------------------------------

set_target_properties(lamp_http PROPERTIES
    OUTPUT_NAME "lamp_http"
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Lamp/Runtime"
)

# ============================================================
# HTTP Test Suite
# ============================================================

if(LAMP_BUILD_TESTS)

    # Existing HTTP parser, router, connection and server tests.
    add_subdirectory(tests)

    # --------------------------------------------------------
    # Phase 6 - HTTP Middleware Tests
    # --------------------------------------------------------

    if(NOT TARGET lamp_http_middleware_test)

        add_executable(lamp_http_middleware_test
            tests/http_middleware_test.cpp
        )

        target_compile_features(
            lamp_http_middleware_test
            PRIVATE cxx_std_20
        )

        target_include_directories(
            lamp_http_middleware_test
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/.."
        )

        add_test(
            NAME lamp_http_middleware_test
            COMMAND lamp_http_middleware_test
        )

        set_tests_properties(
            lamp_http_middleware_test
            PROPERTIES
                LABELS "lamp;http;middleware;regression"
                TIMEOUT 30
        )

    endif()

    # --------------------------------------------------------
    # Phase 6 - HTTP Route Group Tests
    # --------------------------------------------------------

    if(NOT TARGET lamp_http_route_group_test)

        add_executable(lamp_http_route_group_test
            tests/http_route_group_test.cpp
        )

        target_compile_features(
            lamp_http_route_group_test
            PRIVATE cxx_std_20
        )

        target_include_directories(
            lamp_http_route_group_test
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/.."
        )

        add_test(
            NAME lamp_http_route_group_test
            COMMAND lamp_http_route_group_test
        )

        set_tests_properties(
            lamp_http_route_group_test
            PROPERTIES
                LABELS "lamp;http;routing;route-group;regression"
                TIMEOUT 30
        )

    endif()

    # --------------------------------------------------------
    # Phase 6 - HTTP Completion Queue Tests
    # --------------------------------------------------------

    if(NOT TARGET lamp_http_completion_queue_test)

        add_executable(lamp_http_completion_queue_test
            tests/http_completion_queue_test.cpp
        )

        target_compile_features(
            lamp_http_completion_queue_test
            PRIVATE cxx_std_20
        )

        target_link_libraries(
            lamp_http_completion_queue_test
            PRIVATE
                lamp::http
        )

        add_test(
            NAME lamp_http_completion_queue_test
            COMMAND lamp_http_completion_queue_test
        )

        set_tests_properties(
            lamp_http_completion_queue_test
            PROPERTIES
                LABELS "lamp;http;execution;completion;concurrency;regression"
                TIMEOUT 30
        )

    endif()

    # --------------------------------------------------------
    # Phase 6B - Async HTTP Connection Tests
    # --------------------------------------------------------

    if(NOT TARGET lamp_http_async_connection_test)

        add_executable(lamp_http_async_connection_test
            tests/http_async_connection_test.cpp
        )

        target_compile_features(
            lamp_http_async_connection_test
            PRIVATE
                cxx_std_20
        )

        target_link_libraries(
            lamp_http_async_connection_test
            PRIVATE
                lamp::http
        )

        if(MSVC)
            target_compile_options(
                lamp_http_async_connection_test
                PRIVATE
                    /W4
                    /permissive-
            )

            set_property(
                TARGET lamp_http_async_connection_test
                PROPERTY
                    MSVC_RUNTIME_LIBRARY
                    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
            )

        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(
                lamp_http_async_connection_test
                PRIVATE
                    -Wall
                    -Wextra
                    -Wpedantic
            )
        endif()

        add_test(
            NAME lamp_http_async_connection_test
            COMMAND lamp_http_async_connection_test
        )

        set_tests_properties(
            lamp_http_async_connection_test
            PROPERTIES
                LABELS "lamp;http;async;connection;regression"
                TIMEOUT 30
        )

        set_target_properties(
            lamp_http_async_connection_test
            PROPERTIES
                FOLDER "Lamp/Tests/HTTP"
        )

        message(STATUS
            "[Lamp HTTP Tests] Registered: "
            "lamp_http_async_connection_test"
        )

    endif()

    # --------------------------------------------------------
    # Phase 6C - Async HTTP Server Tests
    # --------------------------------------------------------

    if(NOT TARGET lamp_http_async_server_test)

        add_executable(lamp_http_async_server_test
            tests/http_async_server_test.cpp
        )

        target_compile_features(
            lamp_http_async_server_test
            PRIVATE
                cxx_std_20
        )

        target_link_libraries(
            lamp_http_async_server_test
            PRIVATE
                lamp::http
        )

        if(WIN32)
            target_link_libraries(
                lamp_http_async_server_test
                PRIVATE
                    ws2_32
            )

            target_compile_definitions(
                lamp_http_async_server_test
                PRIVATE
                    WIN32_LEAN_AND_MEAN
                    NOMINMAX
            )

            target_compile_options(
                lamp_http_async_server_test
                PRIVATE
                    /W4
                    /permissive-
            )

            set_property(
                TARGET lamp_http_async_server_test
                PROPERTY
                    MSVC_RUNTIME_LIBRARY
                    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
            )

        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(
                lamp_http_async_server_test
                PRIVATE
                    -Wall
                    -Wextra
                    -Wpedantic
            )
        endif()

        add_test(
            NAME lamp_http_async_server_test
            COMMAND lamp_http_async_server_test
        )

        set_tests_properties(
            lamp_http_async_server_test
            PROPERTIES
                LABELS "lamp;http;async;server;integration"
                TIMEOUT 60
        )

        set_target_properties(
            lamp_http_async_server_test
            PROPERTIES
                FOLDER "Lamp/Tests/HTTP"
        )

        message(STATUS
            "[Lamp HTTP Tests] Registered: "
            "lamp_http_async_server_test"
        )

    endif()

endif()

# ============================================================
# End of Lamp HTTP Engine Configuration
# ============================================================
