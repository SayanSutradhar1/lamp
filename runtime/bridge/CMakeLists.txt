
cmake_minimum_required(VERSION 3.20)

# ============================================================
# Lamp Framework
# Native Execution Bridge
#
# File: runtime/bridge/CMakeLists.txt
#
# Purpose:
#   Expose Lamp's operation-oriented native execution bridge.
#
# Architecture:
#   - Transport-independent core
#   - Header-only bridge components
#   - No mandatory V8 dependency
#   - No mandatory HTTP dependency for consumers
#   - Optional HTTP and V8 adapters
#   - Async operation coordination via Phase 5 scheduler
# ============================================================

# ------------------------------------------------------------
# Bridge library
# ------------------------------------------------------------

add_library(lamp_bridge INTERFACE)

add_library(lamp::bridge ALIAS lamp_bridge)

# ------------------------------------------------------------
# C++20
# ------------------------------------------------------------

target_compile_features(
    lamp_bridge
    INTERFACE
        cxx_std_20
)

# ------------------------------------------------------------
# Public include directories
# ------------------------------------------------------------

target_include_directories(
    lamp_bridge
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:include/lamp/runtime>
)

# ------------------------------------------------------------
# Public headers
# ------------------------------------------------------------

target_sources(
    lamp_bridge
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/operation_descriptor.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/request_envelope.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/result_envelope.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/operation_registry.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/operation_cache_interface.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/execution_dispatcher.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/async_operation_coordinator.hpp"
)

# ------------------------------------------------------------
# HTTP transport adapter
# ------------------------------------------------------------

# The HTTP adapter is available only when the HTTP target
# has already been configured.
#
# Consumers using the adapter must also link lamp::http.

if(TARGET lamp::http)

    add_library(lamp_bridge_http INTERFACE)

    add_library(
        lamp::bridge_http
        ALIAS lamp_bridge_http
    )

    target_link_libraries(
        lamp_bridge_http
        INTERFACE
            lamp::bridge
            lamp::http
    )

    target_sources(
        lamp_bridge_http
        INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/http_transport_adapter.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/async_http_operation_pump.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/async_http_transport_adapter.hpp"
    )

endif()

# ------------------------------------------------------------
# V8 operation binding
# ------------------------------------------------------------

# V8 is optional.
#
# Native-only consumers must not inherit V8 headers,
# compile definitions or linker dependencies.

if(LAMP_ENABLE_V8 AND TARGET lamp::v8)

    add_library(lamp_bridge_v8 INTERFACE)

    add_library(
        lamp::bridge_v8
        ALIAS lamp_bridge_v8
    )

    target_link_libraries(
        lamp_bridge_v8
        INTERFACE
            lamp::bridge
            lamp::v8
    )

    target_sources(
        lamp_bridge_v8
        INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/v8_operation_binding.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/v8_sdk_host_bridge.hpp"
    )

endif()

# ------------------------------------------------------------
# Integration tests
# ------------------------------------------------------------

if(LAMP_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ------------------------------------------------------------
# IDE organization
# ------------------------------------------------------------

set_target_properties(
    lamp_bridge
    PROPERTIES
        FOLDER "Lamp/Runtime"
)

# ============================================================
# End of Lamp Native Execution Bridge Configuration
# ============================================================
