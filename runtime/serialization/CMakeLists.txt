
cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework - Phase 7 Serialization Engine
#
# File: runtime/serialization/CMakeLists.txt
# ============================================================

find_package(Threads REQUIRED)

if(NOT TARGET lamp::bridge)
    message(FATAL_ERROR
        "Lamp Serialization requires lamp::bridge."
    )
endif()

add_library(lamp_serialization INTERFACE)
add_library(lamp::serialization ALIAS lamp_serialization)

target_compile_features(
    lamp_serialization
    INTERFACE
        cxx_std_20
)

target_include_directories(
    lamp_serialization
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include/lamp/runtime>
)

target_link_libraries(
    lamp_serialization
    INTERFACE
        lamp::bridge
        Threads::Threads
)

target_sources(
    lamp_serialization
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/serialization_value.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/schema_registry.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/protobuf_wire.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/protobuf_codec.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/json_codec.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/codec_negotiator.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/operation_codec_adapter.hpp"
)

if(LAMP_BUILD_TESTS)

    add_executable(
        lamp_serialization_test
        tests/serialization_test.cpp
    )

    target_compile_features(
        lamp_serialization_test
        PRIVATE
            cxx_std_20
    )

    set_target_properties(
        lamp_serialization_test
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            FOLDER "Lamp/Tests/Serialization"
    )

    target_link_libraries(
        lamp_serialization_test
        PRIVATE
            lamp::serialization
    )

    if(MSVC)
        target_compile_options(
            lamp_serialization_test
            PRIVATE
                /W4
                /permissive-
        )
        set_property(
            TARGET lamp_serialization_test
            PROPERTY
                MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            lamp_serialization_test
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

    if(WIN32)
        target_compile_definitions(
            lamp_serialization_test
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )
    endif()

    add_test(
        NAME lamp_serialization_test
        COMMAND lamp_serialization_test
    )

    set_tests_properties(
        lamp_serialization_test
        PROPERTIES
            TIMEOUT 30
            LABELS "lamp;serialization;protobuf;json;schema"
    )

    message(STATUS
        "[Lamp Serialization] Registered: lamp_serialization_test"
    )

endif()
