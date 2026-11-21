cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework - Phase 12 Performance Benchmarks
#
# File: benchmarks/CMakeLists.txt
# ============================================================

find_package(Threads REQUIRED)

add_executable(
    lamp_runtime_benchmark
    lamp_runtime_benchmark.cpp
)

target_compile_features(
    lamp_runtime_benchmark
    PRIVATE
        cxx_std_20
)

set_target_properties(
    lamp_runtime_benchmark
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        FOLDER "Lamp/Benchmarks"
)

target_link_libraries(
    lamp_runtime_benchmark
    PRIVATE
        lamp::runtime
        lamp::http
        lamp::bridge
        lamp::cache
        lamp::security
        lamp::streaming
        lamp::serialization
        lamp::telemetry
        Threads::Threads
)

if(MSVC)
    target_compile_options(
        lamp_runtime_benchmark
        PRIVATE
            /W4
            /permissive-
    )
    set_property(
        TARGET lamp_runtime_benchmark
        PROPERTY
            MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        lamp_runtime_benchmark
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

if(WIN32)
    target_compile_definitions(
        lamp_runtime_benchmark
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
endif()
