
cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework - Phase 9 Streaming & Real-Time Engine
#
# File: runtime/streaming/CMakeLists.txt
# ============================================================

find_package(Threads REQUIRED)

if(NOT TARGET lamp::bridge)
    message(FATAL_ERROR
        "Lamp Streaming requires lamp::bridge."
    )
endif()

if(NOT TARGET lamp::http)
    message(FATAL_ERROR
        "Lamp Streaming requires lamp::http."
    )
endif()

add_library(lamp_streaming INTERFACE)
add_library(lamp::streaming ALIAS lamp_streaming)

target_compile_features(
    lamp_streaming
    INTERFACE
        cxx_std_20
)

target_include_directories(
    lamp_streaming
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include/lamp/runtime>
)

target_link_libraries(
    lamp_streaming
    INTERFACE
        lamp::bridge
        lamp::http
        Threads::Threads
)

target_sources(
    lamp_streaming
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/stream_channel.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/chunked_http_codec.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/stream_hub.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/streaming_http_bridge.hpp"
)

if(LAMP_BUILD_TESTS)

    add_executable(
        lamp_streaming_test
        tests/streaming_test.cpp
    )

    target_compile_features(
        lamp_streaming_test
        PRIVATE
            cxx_std_20
    )

    set_target_properties(
        lamp_streaming_test
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            FOLDER "Lamp/Tests/Streaming"
    )

    target_link_libraries(
        lamp_streaming_test
        PRIVATE
            lamp::streaming
    )

    if(MSVC)
        target_compile_options(
            lamp_streaming_test
            PRIVATE
                /W4
                /permissive-
        )
        set_property(
            TARGET lamp_streaming_test
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            lamp_streaming_test
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    if(WIN32)
        target_link_libraries(
            lamp_streaming_test
            PRIVATE
                ws2_32
        )
        target_compile_definitions(
            lamp_streaming_test
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )
    endif()

    add_test(
        NAME lamp_streaming_test
        COMMAND lamp_streaming_test
    )

    set_tests_properties(
        lamp_streaming_test
        PROPERTIES
            TIMEOUT 30
            LABELS "lamp;streaming;realtime;backpressure"
    )

    message(STATUS
        "[Lamp Streaming] Registered: lamp_streaming_test"
    )

endif()
