cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework - Native Binary Transport Module (`LMP1`/`LCB1`)
#
# File: runtime/transport/CMakeLists.txt
# ============================================================

find_package(Threads REQUIRED)

add_library(lamp_transport INTERFACE)
add_library(lamp::transport ALIAS lamp_transport)

target_compile_features(
    lamp_transport
    INTERFACE
        cxx_std_20
)

target_include_directories(
    lamp_transport
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include/lamp/runtime>
)

target_link_libraries(
    lamp_transport
    INTERFACE
        lamp::core
        lamp::serialization
        lamp::streaming
        lamp::security
        Threads::Threads
)

target_sources(
    lamp_transport
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/lamp_binary_protocol.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/async_binary_converter.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/wire_cache.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/lamp_binary_server.hpp"
)

if(LAMP_BUILD_TESTS)

    add_executable(
        lamp_transport_test
        tests/transport_test.cpp
    )

    target_compile_features(
        lamp_transport_test
        PRIVATE
            cxx_std_20
    )

    set_target_properties(
        lamp_transport_test
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            FOLDER "Lamp/Tests/Transport"
    )

    target_link_libraries(
        lamp_transport_test
        PRIVATE
            lamp::transport
            lamp::http
    )

    if(MSVC)
        target_compile_options(
            lamp_transport_test
            PRIVATE
                /W4
                /permissive-
        )
        set_property(
            TARGET lamp_transport_test
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            lamp_transport_test
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    if(WIN32)
        target_link_libraries(
            lamp_transport_test
            PRIVATE
                ws2_32
        )
        target_compile_definitions(
            lamp_transport_test
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )
    endif()

    add_test(
        NAME lamp_transport_test
        COMMAND lamp_transport_test
    )

    set_tests_properties(
        lamp_transport_test
        PROPERTIES
            TIMEOUT 60
            LABELS "lamp;transport;binary;cache;benchmark"
    )

    message(STATUS "[Lamp Transport] Registered: lamp_transport_test")

endif()
