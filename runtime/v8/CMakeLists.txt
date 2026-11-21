# ============================================================
# Lamp Framework - Embedded V8 Runtime
# ============================================================
#
# Target:
#
#   lamp_v8
#   lamp::v8
#
# Requirements:
#
#   - C++20
#   - V8 JavaScript engine
#   - V8 libplatform
#   - Compatible V8 headers and libraries
#
# Supported dependency configurations:
#
#   1. Existing CMake targets:
#        V8::V8
#        V8::Platform
#
#   2. Manually supplied V8 SDK:
#        LAMP_V8_ROOT
#
# The manually supplied SDK must provide compatible
# headers and libraries built using the same toolchain,
# architecture, and runtime configuration as Lamp.
#
# ============================================================


# ------------------------------------------------------------
# Feature Guard
# ------------------------------------------------------------

if(NOT LAMP_ENABLE_V8)
    return()
endif()


# ------------------------------------------------------------
# Runtime Configuration
# ------------------------------------------------------------

set(
    LAMP_V8_ROOT
    ""
    CACHE PATH
    "Root directory of the V8 SDK"
)

set(
    LAMP_V8_INCLUDE_DIR
    ""
    CACHE PATH
    "Directory containing v8.h"
)

set(
    LAMP_V8_LIBRARY
    ""
    CACHE FILEPATH
    "Path to the main V8 library"
)

set(
    LAMP_V8_PLATFORM_LIBRARY
    ""
    CACHE FILEPATH
    "Path to the V8 platform library"
)

option(
    LAMP_V8_USE_SYSTEM_PACKAGE
    "Prefer an existing V8 CMake package"
    ON
)


# ------------------------------------------------------------
# Create Lamp V8 Target
# ------------------------------------------------------------
#
# The current V8 runtime components are header-only.
#
# An INTERFACE target is appropriate until compiled
# runtime implementation files are introduced.
#
# ------------------------------------------------------------

add_library(
    lamp_v8
    INTERFACE
)

add_library(
    lamp::v8
    ALIAS lamp_v8
)

target_compile_features(
    lamp_v8
    INTERFACE
    cxx_std_20
)

target_include_directories(
    lamp_v8
    INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)


# ------------------------------------------------------------
# Link Lamp Build Configuration
# ------------------------------------------------------------

if(TARGET lamp_options)
    target_link_libraries(
        lamp_v8
        INTERFACE
        lamp_options
    )
endif()

if(TARGET lamp_warnings)
    target_link_libraries(
        lamp_v8
        INTERFACE
        lamp_warnings
    )
endif()


# ------------------------------------------------------------
# Dependency Discovery
# ------------------------------------------------------------

set(
    LAMP_V8_DEPENDENCY_FOUND
    FALSE
)

if(LAMP_V8_USE_SYSTEM_PACKAGE)

    find_package(
        V8 CONFIG QUIET
    )

    if(
        TARGET V8::V8
        AND TARGET V8::Platform
    )

        target_link_libraries(
            lamp_v8
            INTERFACE
            V8::V8
            V8::Platform
        )

        set(
            LAMP_V8_DEPENDENCY_FOUND
            TRUE
        )

        message(
            STATUS
            "Lamp V8: using imported CMake targets"
        )

    endif()

endif()


# ------------------------------------------------------------
# Manual SDK Discovery
# ------------------------------------------------------------

if(NOT LAMP_V8_DEPENDENCY_FOUND)

    # --------------------------------------------------------
    # Find Headers
    # --------------------------------------------------------

    if(NOT LAMP_V8_INCLUDE_DIR)

        find_path(
            LAMP_V8_INCLUDE_DIR
            NAMES
                v8.h
            HINTS
                "${LAMP_V8_ROOT}"
            PATH_SUFFIXES
                include
                include/v8
                ""
        )

    endif()

    # --------------------------------------------------------
    # Find Main V8 Library
    # --------------------------------------------------------

    if(NOT LAMP_V8_LIBRARY)

        find_library(
            LAMP_V8_LIBRARY
            NAMES
                v8_monolith
                v8
            HINTS
                "${LAMP_V8_ROOT}"
            PATH_SUFFIXES
                lib
                lib64
                out.gn/x64.release/obj
                out.gn/x64.release
                ""
        )

    endif()

    # --------------------------------------------------------
    # Find V8 Platform Library
    # --------------------------------------------------------
    #
    # Monolithic V8 distributions may already include
    # the platform implementation.
    #
    # Separate-library distributions require an
    # additional platform library.
    #
    # --------------------------------------------------------

    if(NOT LAMP_V8_PLATFORM_LIBRARY)

        find_library(
            LAMP_V8_PLATFORM_LIBRARY
            NAMES
                v8_libplatform
                v8_libplatform_static
            HINTS
                "${LAMP_V8_ROOT}"
            PATH_SUFFIXES
                lib
                lib64
                out.gn/x64.release/obj
                out.gn/x64.release
                ""
        )

    endif()

    # --------------------------------------------------------
    # Validate Required SDK Components
    # --------------------------------------------------------

    if(
        NOT LAMP_V8_INCLUDE_DIR
        OR NOT LAMP_V8_LIBRARY
    )

        message(
            FATAL_ERROR
            "Lamp V8: V8 SDK not found.\n"
            "Set LAMP_V8_ROOT or provide:\n"
            "  LAMP_V8_INCLUDE_DIR\n"
            "  LAMP_V8_LIBRARY\n"
            "  LAMP_V8_PLATFORM_LIBRARY (if separate)\n"
            "Alternatively provide V8::V8 and "
            "V8::Platform CMake targets."
        )

    endif()

    if(
        NOT EXISTS
        "${LAMP_V8_INCLUDE_DIR}/v8.h"
    )

        message(
            FATAL_ERROR
            "Lamp V8: v8.h does not exist in "
            "LAMP_V8_INCLUDE_DIR"
        )

    endif()

    if(
        NOT EXISTS
        "${LAMP_V8_INCLUDE_DIR}/libplatform/libplatform.h"
    )

        message(
            FATAL_ERROR
            "Lamp V8: libplatform/libplatform.h "
            "was not found in the V8 include directory"
        )

    endif()

    if(
        NOT EXISTS
        "${LAMP_V8_LIBRARY}"
    )

        message(
            FATAL_ERROR
            "Lamp V8: main V8 library does not exist"
        )

    endif()

    # --------------------------------------------------------
    # Register Imported V8 Library
    # --------------------------------------------------------

    add_library(
        lamp_v8_external
        UNKNOWN IMPORTED
    )

    set_target_properties(
        lamp_v8_external
        PROPERTIES
        IMPORTED_LOCATION
            "${LAMP_V8_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${LAMP_V8_INCLUDE_DIR}"
    )

    target_link_libraries(
        lamp_v8
        INTERFACE
        lamp_v8_external
    )

    # --------------------------------------------------------
    # Register Optional Platform Library
    # --------------------------------------------------------

    if(LAMP_V8_PLATFORM_LIBRARY)

        if(
            NOT EXISTS
            "${LAMP_V8_PLATFORM_LIBRARY}"
        )

            message(
                FATAL_ERROR
                "Lamp V8: platform library does not exist"
            )

        endif()

        add_library(
            lamp_v8_platform_external
            UNKNOWN IMPORTED
        )

        set_target_properties(
            lamp_v8_platform_external
            PROPERTIES
            IMPORTED_LOCATION
                "${LAMP_V8_PLATFORM_LIBRARY}"
        )

        target_link_libraries(
            lamp_v8
            INTERFACE
            lamp_v8_platform_external
        )

    endif()

    target_include_directories(
        lamp_v8
        INTERFACE
        "${LAMP_V8_INCLUDE_DIR}"
    )

    set(
        LAMP_V8_DEPENDENCY_FOUND
        TRUE
    )

    message(
        STATUS
        "Lamp V8: using manually configured SDK"
    )

    message(
        STATUS
        "Lamp V8 include: ${LAMP_V8_INCLUDE_DIR}"
    )

    message(
        STATUS
        "Lamp V8 library: ${LAMP_V8_LIBRARY}"
    )

    if(LAMP_V8_PLATFORM_LIBRARY)

        message(
            STATUS
            "Lamp V8 platform: ${LAMP_V8_PLATFORM_LIBRARY}"
        )

    endif()

endif()


# ------------------------------------------------------------
# Platform-Specific Dependencies
# ------------------------------------------------------------

if(WIN32)

    # Windows system libraries commonly required by
    # native V8 builds.

    target_link_libraries(
        lamp_v8
        INTERFACE
        winmm
        dbghelp
        shlwapi
    )

elseif(UNIX)

    find_package(
        Threads REQUIRED
    )

    target_link_libraries(
        lamp_v8
        INTERFACE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

endif()


# ------------------------------------------------------------
# Compile Definitions
# ------------------------------------------------------------

# Must match the GN configuration used to build v8_monolith.lib.
# These definitions affect the V8 public API and object layout.

target_compile_definitions(
    lamp_v8
    INTERFACE
        LAMP_ENABLE_V8=1
        V8_COMPRESS_POINTERS
        V8_COMPRESS_POINTERS_IN_SHARED_CAGE
        V8_31BIT_SMIS_ON_64BIT_ARCH
        CPPGC_POINTER_COMPRESSION
)


# ------------------------------------------------------------
# SDK Compatibility Notes
# ------------------------------------------------------------
#
# V8 builds can differ in:
#
#   - Pointer compression
#   - Sandbox configuration
#   - Shared versus static linking
#   - C++ standard library ABI
#   - Compiler runtime
#   - Debug versus release configuration
#
# Consumers must use headers and libraries generated
# from the same compatible V8 build configuration.
#
# The SDK must supply any required compile definitions
# and transitive libraries.
#
# This CMake file cannot infer all V8 GN build flags.
#
# ------------------------------------------------------------


# ------------------------------------------------------------
# Tests
# ------------------------------------------------------------
#
# Lamp's root project uses LAMP_BUILD_TESTS.
#
# Tests will be registered after the V8 test targets
# are introduced.
#
# ------------------------------------------------------------

if(LAMP_BUILD_TESTS)

    if(EXISTS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt"
    )

        add_subdirectory(
            tests
        )

    endif()

endif()


# ------------------------------------------------------------
# Configuration Summary
# ------------------------------------------------------------

message(
    STATUS
    "Lamp V8 runtime: ENABLED"
)

message(
    STATUS
    "Lamp V8 dependency found: "
    "${LAMP_V8_DEPENDENCY_FOUND}"
)

message(
    STATUS
    "Lamp V8 target: lamp::v8"
)