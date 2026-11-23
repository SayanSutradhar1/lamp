
# ============================================================
# Lamp Framework - Native Runtime
# ============================================================

# ------------------------------------------------------------
# Runtime Configuration
# ------------------------------------------------------------

set(LAMP_RUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# ------------------------------------------------------------
# Core Runtime Library
# ------------------------------------------------------------

# Start with an INTERFACE target because runtime source files
# have not been created yet.
#
# Once the core implementation exists, we will introduce a
# compiled library target for the runtime.

add_library(lamp_runtime INTERFACE)

add_library(lamp::runtime ALIAS lamp_runtime)

target_compile_features(
    lamp_runtime
    INTERFACE
        cxx_std_20
)

target_include_directories(
    lamp_runtime
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/runtime>
)

target_link_libraries(
    lamp_runtime
    INTERFACE
        lamp_options
        lamp_warnings
)

# ------------------------------------------------------------
# Runtime Feature Definitions
# ------------------------------------------------------------

target_compile_definitions(
    lamp_runtime
    INTERFACE
        LAMP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        LAMP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        LAMP_VERSION_PATCH=${PROJECT_VERSION_PATCH}

        $<$<BOOL:${LAMP_ENABLE_V8}>:LAMP_ENABLE_V8=1>
        $<$<BOOL:${LAMP_ENABLE_L2_CACHE}>:LAMP_ENABLE_L2_CACHE=1>
        $<$<BOOL:${LAMP_ENABLE_NATIVE_TASKS}>:LAMP_ENABLE_NATIVE_TASKS=1>
)

# ------------------------------------------------------------
# Modular Runtime Components
# ------------------------------------------------------------

# Each component will eventually provide its own CMakeLists.txt.
#
# Components are registered automatically when their build
# configuration file exists.

set(LAMP_RUNTIME_COMPONENTS
    core
    http
    v8
    bridge
    cache
    execution
    serialization
    streaming
    security
    transport
    router
    telemetry
)

foreach(component IN LISTS LAMP_RUNTIME_COMPONENTS)

    set(
        component_directory
        "${LAMP_RUNTIME_ROOT}/${component}"
    )

    if(EXISTS "${component_directory}/CMakeLists.txt")

        message(
            STATUS
            "[Lamp] Registering runtime component: ${component}"
        )

        add_subdirectory(
            "${component_directory}"
        )

    else()

        message(
            STATUS
            "[Lamp] Component pending: ${component}"
        )

    endif()

endforeach()

# ------------------------------------------------------------
# Optional V8 Runtime Integration
# ------------------------------------------------------------

if(LAMP_ENABLE_V8)

    if(NOT TARGET lamp_v8)

        message(
            FATAL_ERROR
            "LAMP_ENABLE_V8 is ON, but lamp_v8 "
            "was not registered successfully."
        )

    endif()

    target_link_libraries(
        lamp_runtime
        INTERFACE
            lamp::v8
    )

endif()

# ------------------------------------------------------------
# Platform Configuration
# ------------------------------------------------------------

if(WIN32)

    target_compile_definitions(
        lamp_runtime
        INTERFACE
            LAMP_PLATFORM_WINDOWS=1
    )

elseif(APPLE)

    target_compile_definitions(
        lamp_runtime
        INTERFACE
            LAMP_PLATFORM_MACOS=1
    )

elseif(UNIX)

    target_compile_definitions(
        lamp_runtime
        INTERFACE
            LAMP_PLATFORM_LINUX=1
    )

endif()

# ------------------------------------------------------------
# Runtime Build Summary
# ------------------------------------------------------------

message(STATUS "")
message(STATUS "========================================")
message(STATUS " Lamp Native Runtime")
message(STATUS "========================================")
message(STATUS " Version       : ${PROJECT_VERSION}")
message(STATUS " C++ Standard  : C++${CMAKE_CXX_STANDARD}")
message(STATUS " V8 Engine     : ${LAMP_ENABLE_V8}")
message(STATUS " L2 Cache      : ${LAMP_ENABLE_L2_CACHE}")
message(STATUS " Native Tasks  : ${LAMP_ENABLE_NATIVE_TASKS}")
message(STATUS "========================================")
message(STATUS "")
