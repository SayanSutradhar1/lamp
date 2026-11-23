
cmake_minimum_required(VERSION 3.24)

# ============================================================
# Lamp Framework - Root Build Configuration
# ============================================================

project(
    lamp
    VERSION 0.1.0
    DESCRIPTION
        "High-performance C++ server runtime with embedded V8"
    LANGUAGES CXX
)

# ============================================================
# C++ Language Configuration
# ============================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for IDE tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# Build Options
# ============================================================

option(
    LAMP_BUILD_TESTS
    "Build Lamp unit and integration tests"
    OFF
)

option(
    LAMP_BUILD_BENCHMARKS
    "Build Lamp performance benchmarks"
    OFF
)

option(
    LAMP_ENABLE_V8
    "Enable embedded V8 JavaScript engine"
    OFF
)

option(
    LAMP_ENABLE_L2_CACHE
    "Enable optional distributed L2 caching"
    OFF
)

option(
    LAMP_ENABLE_NATIVE_TASKS
    "Enable native CPU task execution"
    ON
)

# ============================================================
# Shared Compiler Options
# ============================================================

add_library(
    lamp_options
    INTERFACE
)

add_library(
    lamp::options
    ALIAS
    lamp_options
)

target_compile_features(
    lamp_options
    INTERFACE
        cxx_std_20
)

# ============================================================
# Compiler Warnings
# ============================================================

add_library(
    lamp_warnings
    INTERFACE
)

add_library(
    lamp::warnings
    ALIAS
    lamp_warnings
)

if(MSVC)

    target_compile_options(
        lamp_warnings
        INTERFACE
            /W4
            /permissive-
    )

else()

    target_compile_options(
        lamp_warnings
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
    )

endif()

# ============================================================
# Testing Configuration
# ============================================================

# Testing must be enabled in the top-level directory.
#
# This generates the root CTestTestfile.cmake needed for:
#
#   ctest --test-dir build
#
# The test executables themselves are defined in their
# respective component CMakeLists.txt files.

if(LAMP_BUILD_TESTS)

    enable_testing()

    message(
        STATUS
        "[Lamp] CTest enabled"
    )

endif()

# ============================================================
# Platform Information
# ============================================================

message(STATUS "")
message(STATUS "========================================")
message(STATUS " Lamp Framework v${PROJECT_VERSION}")
message(STATUS "========================================")
message(STATUS " Platform     : ${CMAKE_SYSTEM_NAME}")
message(STATUS " Architecture : ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS " Compiler     : ${CMAKE_CXX_COMPILER_ID}")
message(STATUS " C++ Standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS " Build Type   : ${CMAKE_BUILD_TYPE}")
message(STATUS " V8 Enabled   : ${LAMP_ENABLE_V8}")
message(STATUS " Native Tasks : ${LAMP_ENABLE_NATIVE_TASKS}")
message(STATUS " L2 Cache     : ${LAMP_ENABLE_L2_CACHE}")
message(STATUS " Tests        : ${LAMP_BUILD_TESTS}")
message(STATUS "========================================")
message(STATUS "")

# ============================================================
# Optional Feature Status
# ============================================================

if(LAMP_ENABLE_V8)

    message(
        STATUS
        "[Lamp] V8 integration requested"
    )

    # V8 dependency discovery and linking will be added
    # when the V8 execution module is implemented.

endif()

if(LAMP_ENABLE_L2_CACHE)

    message(
        STATUS
        "[Lamp] Distributed L2 cache requested"
    )

endif()

if(LAMP_BUILD_BENCHMARKS)

    message(
        STATUS
        "[Lamp] Benchmark support enabled"
    )

    # Benchmark targets will be registered later.

endif()

# ============================================================
# Runtime Registration
# ============================================================

# IMPORTANT:
#
# This statement connects the root project to:
#
#   runtime/CMakeLists.txt
#
# That file discovers:
#
#   runtime/core/CMakeLists.txt
#
# The core component defines:
#
#   lamp_core
#   lamp_core_tests
#
# Without this statement, CMake cannot build the
# runtime or discover its tests.

add_subdirectory(runtime)

if(LAMP_ENABLE_V8)
    add_subdirectory(host)
endif()

if(LAMP_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ============================================================
# Build Summary
# ============================================================

message(STATUS "")
message(STATUS "========================================")
message(STATUS " Lamp Build Configuration Complete")
message(STATUS "========================================")
message(STATUS " Version      : ${PROJECT_VERSION}")
message(STATUS " Runtime      : Enabled")
message(STATUS " Tests        : ${LAMP_BUILD_TESTS}")
message(STATUS " Benchmarks   : ${LAMP_BUILD_BENCHMARKS}")
message(STATUS "========================================")
message(STATUS "")
