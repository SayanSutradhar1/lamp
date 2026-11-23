cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework - Native App Host (`lamp_server`)
#
# File: host/CMakeLists.txt
# ============================================================

find_package(Threads REQUIRED)

if(NOT LAMP_ENABLE_V8 OR NOT TARGET lamp::bridge_v8)
    message(STATUS "[Lamp Host] Skipped (LAMP_ENABLE_V8 is OFF)")
    return()
endif()

add_library(lamp_host INTERFACE)
add_library(lamp::host ALIAS lamp_host)

target_compile_features(
    lamp_host
    INTERFACE
        cxx_std_20
)

target_include_directories(
    lamp_host
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/runtime>
)

target_link_libraries(
    lamp_host
    INTERFACE
        lamp::runtime
        lamp::core
        lamp::transport
        lamp::bridge
        lamp::bridge_v8
        lamp::cache
        lamp::execution
        lamp::security
        lamp::serialization
        lamp::telemetry
        Threads::Threads
)

add_executable(
    lamp_server
    lamp_server_main.cpp
)

target_compile_features(
    lamp_server
    PRIVATE
        cxx_std_20
)

set_target_properties(
    lamp_server
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        FOLDER "Lamp/Host"
)

target_link_libraries(
    lamp_server
    PRIVATE
        lamp::host
)

if(MSVC)
    target_compile_options(lamp_server PRIVATE /W4 /permissive-)
    set_property(TARGET lamp_server PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lamp_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(lamp_server PRIVATE ws2_32)
    target_compile_definitions(lamp_server PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(LAMP_BUILD_TESTS)

    add_executable(
        lamp_app_server_test
        tests/lamp_app_server_test.cpp
    )

    target_compile_features(lamp_app_server_test PRIVATE cxx_std_20)

    set_target_properties(
        lamp_app_server_test
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            FOLDER "Lamp/Tests/Host"
    )

    target_link_libraries(
        lamp_app_server_test
        PRIVATE
            lamp::host
    )

    if(MSVC)
        target_compile_options(lamp_app_server_test PRIVATE /W4 /permissive-)
        set_property(TARGET lamp_app_server_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(lamp_app_server_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(WIN32)
        target_link_libraries(lamp_app_server_test PRIVATE ws2_32)
        target_compile_definitions(lamp_app_server_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    add_test(
        NAME lamp_app_server_test
        COMMAND lamp_app_server_test
    )

    set_tests_properties(
        lamp_app_server_test
        PROPERTIES
            TIMEOUT 30
            LABELS "lamp;host;v8;binary;end-to-end"
    )

    message(STATUS "[Lamp Host] Registered: lamp_server & lamp_app_server_test")

endif()
