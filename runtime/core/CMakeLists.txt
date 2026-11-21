
# ============================================================
# Lamp Framework
# Core Runtime Module
#
# C++20
#
# Library:
#   lamp_core
#   lamp::core
#
# Test targets:
#   lamp_core_tests
#   lamp_lifecycle_tests
#   lamp_event_dispatcher_tests
#   lamp_worker_pool_tests
#   lamp_task_scheduler_tests
#   lamp_runtime_context_tests
#   lamp_runtime_manager_tests
#   lamp_service_registry_tests
# ============================================================

# ------------------------------------------------------------
# Core Runtime Library
# ------------------------------------------------------------

add_library(lamp_core STATIC
    application.cpp
)

add_library(lamp::core ALIAS lamp_core)

target_compile_features(lamp_core
    PUBLIC
        cxx_std_20
)

# ------------------------------------------------------------
# Public Include Directories
# ------------------------------------------------------------

# Allows consumers to include:
#
#   #include "core/application.hpp"
#   #include "core/lifecycle.hpp"
#   #include "core/event_dispatcher.hpp"
#   #include "core/worker_pool.hpp"
#   #include "core/task_scheduler.hpp"
#   #include "core/runtime_context.hpp"
#   #include "core/runtime_manager.hpp"
#   #include "core/service_registry.hpp"

target_include_directories(lamp_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/runtime>
        $<INSTALL_INTERFACE:include>
)

# ------------------------------------------------------------
# Framework Build Options
# ------------------------------------------------------------

if(TARGET lamp_options)
    target_link_libraries(lamp_core
        PUBLIC
            lamp_options
    )
endif()

if(TARGET lamp_warnings)
    target_link_libraries(lamp_core
        PRIVATE
            lamp_warnings
    )
endif()

# ------------------------------------------------------------
# Threading Support
# ------------------------------------------------------------

find_package(Threads REQUIRED)

target_link_libraries(lamp_core
    PUBLIC
        Threads::Threads
)

# ------------------------------------------------------------
# Core Test Configuration
# ------------------------------------------------------------

if(LAMP_BUILD_TESTS)

    # The root CMakeLists.txt must enable CTest before
    # adding the runtime subdirectory.

    # --------------------------------------------------------
    # Reusable Test Registration
    # --------------------------------------------------------

    function(lamp_add_core_test
        target_name
        source_file
        test_name
    )

        add_executable(${target_name}
            ${source_file}
        )

        target_compile_features(${target_name}
            PRIVATE
                cxx_std_20
        )

        target_link_libraries(${target_name}
            PRIVATE
                lamp::core
        )

        if(TARGET lamp_warnings)
            target_link_libraries(${target_name}
                PRIVATE
                    lamp_warnings
            )
        endif()

        add_test(
            NAME ${test_name}
            COMMAND $<TARGET_FILE:${target_name}>
        )

        set_tests_properties(${test_name}
            PROPERTIES
                TIMEOUT 60
        )

    endfunction()

    # --------------------------------------------------------
    # Application Tests
    # --------------------------------------------------------

    lamp_add_core_test(
        lamp_core_tests
        application_test.cpp
        lamp.core.application
    )

    # --------------------------------------------------------
    # Lifecycle Tests
    # --------------------------------------------------------

    lamp_add_core_test(
        lamp_lifecycle_tests
        lifecycle_test.cpp
        lamp.core.lifecycle
    )

    # --------------------------------------------------------
    # Event Dispatcher Tests
    # --------------------------------------------------------

    lamp_add_core_test(
        lamp_event_dispatcher_tests
        event_dispatcher_test.cpp
        lamp.core.event_dispatcher
    )

    # --------------------------------------------------------
    # Worker Pool Tests
    # --------------------------------------------------------

    lamp_add_core_test(
        lamp_worker_pool_tests
        worker_pool_test.cpp
        lamp.core.worker_pool
    )

    # --------------------------------------------------------
    # Task Scheduler Tests
    # --------------------------------------------------------

    lamp_add_core_test(
        lamp_task_scheduler_tests
        task_scheduler_test.cpp
        lamp.core.task_scheduler
    )

    # --------------------------------------------------------
    # Runtime Context Tests
    # --------------------------------------------------------

    lamp_add_core_test(
        lamp_runtime_context_tests
        runtime_context_test.cpp
        lamp.core.runtime_context
    )

    # --------------------------------------------------------
    # Runtime Manager Tests
    # --------------------------------------------------------

    lamp_add_core_test(
        lamp_runtime_manager_tests
        runtime_manager_test.cpp
        lamp.core.runtime_manager
    )

    # --------------------------------------------------------
    # Service Registry Tests
    # --------------------------------------------------------

    lamp_add_core_test(
        lamp_service_registry_tests
        service_registry_test.cpp
        lamp.core.service_registry
    )

endif()
