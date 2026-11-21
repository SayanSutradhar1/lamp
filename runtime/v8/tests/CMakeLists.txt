# ============================================================
# Lamp Framework - V8 Runtime Test Suite
# ============================================================
#
# File:
#   runtime/v8/tests/CMakeLists.txt
#
# Purpose:
#   Build and register the V8 runtime test executables.
#
# Requirements:
#   - CMake 3.20+
#   - C++20
#   - LAMP_ENABLE_V8=ON
#   - LAMP_BUILD_TESTS=ON
#   - Configured V8 SDK
#
# Test executables:
#
#   lamp_v8_platform_test
#   lamp_v8_isolate_test
#   lamp_v8_context_test
#   lamp_v8_script_engine_test
#   lamp_v8_value_test
#   lamp_v8_native_binding_test
#   lamp_v8_module_loader_test
#   lamp_v8_runtime_test
#
# ============================================================

# ------------------------------------------------------------
# Configuration Guards
# ------------------------------------------------------------

if(NOT LAMP_ENABLE_V8)
    message(STATUS
        "[Lamp V8 Tests] Skipped: LAMP_ENABLE_V8=OFF"
    )
    return()
endif()

if(NOT LAMP_BUILD_TESTS)
    message(STATUS
        "[Lamp V8 Tests] Skipped: LAMP_BUILD_TESTS=OFF"
    )
    return()
endif()

# ------------------------------------------------------------
# Validate Required Targets
# ------------------------------------------------------------

if(NOT TARGET lamp::v8)
    message(FATAL_ERROR
        "[Lamp V8 Tests] Required target lamp::v8 "
        "does not exist. Configure runtime/v8 first."
    )
endif()

# ------------------------------------------------------------
# Test Configuration
# ------------------------------------------------------------

set(LAMP_V8_TEST_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

set(LAMP_V8_TEST_RUNTIME_ROOT
    "${PROJECT_SOURCE_DIR}/runtime"
)

# ------------------------------------------------------------
# Test Helper Function
# ------------------------------------------------------------
#
# Usage:
#
#   lamp_add_v8_test(
#       lamp_v8_platform_test
#       v8_platform_test.cpp
#   )
#
# Each invocation:
#
#   1. Creates an executable.
#   2. Enables C++20.
#   3. Links the V8 runtime.
#   4. Applies project compiler options.
#   5. Registers the executable with CTest.
#   6. Adds test labels and timeout.
#
# ------------------------------------------------------------

function(lamp_add_v8_test TARGET_NAME SOURCE_FILE)

    # --------------------------------------------------------
    # Validate Source File
    # --------------------------------------------------------

    set(TEST_SOURCE_PATH
        "${LAMP_V8_TEST_ROOT}/${SOURCE_FILE}"
    )

    if(NOT EXISTS "${TEST_SOURCE_PATH}")
        message(FATAL_ERROR
            "[Lamp V8 Tests] Missing source file: "
            "${TEST_SOURCE_PATH}"
        )
    endif()

    # --------------------------------------------------------
    # Create Test Executable
    # --------------------------------------------------------

    add_executable(
        ${TARGET_NAME}
        "${TEST_SOURCE_PATH}"
    )

    # --------------------------------------------------------
    # Language Standard
    # --------------------------------------------------------

    target_compile_features(
        ${TARGET_NAME}
        PRIVATE
            cxx_std_20
    )

    # --------------------------------------------------------
    # Include Directories
    # --------------------------------------------------------

    target_include_directories(
        ${TARGET_NAME}
        PRIVATE
            "${LAMP_V8_TEST_RUNTIME_ROOT}"
    )

    # --------------------------------------------------------
    # Link Dependencies
    # --------------------------------------------------------

    target_link_libraries(
        ${TARGET_NAME}
        PRIVATE
            lamp::v8
    )

    # --------------------------------------------------------
    # Project Compiler Options
    # --------------------------------------------------------

    if(TARGET lamp_options)
        target_link_libraries(
            ${TARGET_NAME}
            PRIVATE
                lamp_options
        )
    endif()

    if(TARGET lamp_warnings)
        target_link_libraries(
            ${TARGET_NAME}
            PRIVATE
                lamp_warnings
        )
    endif()

    # --------------------------------------------------------
    # Target Properties
    # --------------------------------------------------------

    set_target_properties(
        ${TARGET_NAME}
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
    )

    # --------------------------------------------------------
    # Register With CTest
    # --------------------------------------------------------

    add_test(
        NAME
            ${TARGET_NAME}
        COMMAND
            $<TARGET_FILE:${TARGET_NAME}>
    )

    # --------------------------------------------------------
    # CTest Properties
    # --------------------------------------------------------

    set_tests_properties(
        ${TARGET_NAME}
        PROPERTIES
            LABELS "lamp;v8;runtime"
            TIMEOUT 120
            WORKING_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}"
    )

    # --------------------------------------------------------
    # IDE Organization
    # --------------------------------------------------------

    set_target_properties(
        ${TARGET_NAME}
        PROPERTIES
            FOLDER "Lamp/Tests/V8"
    )

    message(STATUS
        "[Lamp V8 Tests] Registered: ${TARGET_NAME}"
    )

endfunction()

# ============================================================
# V8 Platform Tests
# ============================================================

lamp_add_v8_test(
    lamp_v8_platform_test
    v8_platform_test.cpp
)

# ============================================================
# V8 Isolate Tests
# ============================================================

lamp_add_v8_test(
    lamp_v8_isolate_test
    v8_isolate_test.cpp
)

# ============================================================
# V8 Context Tests
# ============================================================

lamp_add_v8_test(
    lamp_v8_context_test
    v8_context_test.cpp
)

# ============================================================
# V8 Script Engine Tests
# ============================================================

lamp_add_v8_test(
    lamp_v8_script_engine_test
    v8_script_engine_test.cpp
)

# ============================================================
# V8 Value Conversion Tests
# ============================================================

lamp_add_v8_test(
    lamp_v8_value_test
    v8_value_test.cpp
)

# ============================================================
# V8 Native Binding Tests
# ============================================================

lamp_add_v8_test(
    lamp_v8_native_binding_test
    v8_native_binding_test.cpp
)

# ============================================================
# V8 Module Loader Tests
# ============================================================

lamp_add_v8_test(
    lamp_v8_module_loader_test
    v8_module_loader_test.cpp
)

# ============================================================
# V8 Runtime Integration Tests
# ============================================================

lamp_add_v8_test(
    lamp_v8_runtime_test
    v8_runtime_test.cpp
)

# ============================================================
# Test Suite Summary
# ============================================================

set(LAMP_V8_TEST_TARGETS
    lamp_v8_platform_test
    lamp_v8_isolate_test
    lamp_v8_context_test
    lamp_v8_script_engine_test
    lamp_v8_value_test
    lamp_v8_native_binding_test
    lamp_v8_module_loader_test
    lamp_v8_runtime_test
)

list(LENGTH
    LAMP_V8_TEST_TARGETS
    LAMP_V8_TEST_COUNT
)

# ------------------------------------------------------------
# Aggregate Build Target
# ------------------------------------------------------------
#
# Builds all V8 test executables:
#
#   cmake --build build --target lamp_v8_tests
#
# ------------------------------------------------------------

add_custom_target(
    lamp_v8_tests
    DEPENDS
        ${LAMP_V8_TEST_TARGETS}
)

set_target_properties(
    lamp_v8_tests
    PROPERTIES
        FOLDER "Lamp/Tests/V8"
)

# ------------------------------------------------------------
# Configuration Summary
# ------------------------------------------------------------

message(STATUS "")
message(STATUS "========================================")
message(STATUS " Lamp V8 Test Suite")
message(STATUS "========================================")
message(STATUS " Enabled      : YES")
message(STATUS " Language     : C++20")
message(STATUS " Test targets : ${LAMP_V8_TEST_COUNT}")
message(STATUS " V8 target    : lamp::v8")
message(STATUS " CTest label  : v8")
message(STATUS "========================================")
message(STATUS "")