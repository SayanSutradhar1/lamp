# ============================================================
# Lamp Framework
# Phase 5 - Asynchronous Execution Engine
# File: runtime/execution/CMakeLists.txt
# ============================================================

cmake_minimum_required(VERSION 3.21)

if(NOT TARGET lamp_execution)
    add_library(lamp_execution INTERFACE)
endif()

if(NOT TARGET lamp::execution)
    add_library(lamp::execution ALIAS lamp_execution)
endif()

find_package(Threads REQUIRED)

if(NOT TARGET lamp::bridge)
    message(FATAL_ERROR
        "Lamp Execution requires lamp::bridge. Register the bridge component before execution.")
endif()

target_link_libraries(lamp_execution INTERFACE lamp::bridge Threads::Threads)
target_include_directories(lamp_execution INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(lamp_execution INTERFACE cxx_std_20)

if(LAMP_BUILD_TESTS)
    # Each test suite has its own executable to isolate failures.
    # Preserve the existing public CTest names and labels.

    add_executable(lamp_native_worker_pool_test
        tests/native_worker_pool_test.cpp)
    target_link_libraries(lamp_native_worker_pool_test PRIVATE lamp::execution)
    target_compile_features(lamp_native_worker_pool_test PRIVATE cxx_std_20)
    add_test(NAME lamp_native_worker_pool COMMAND lamp_native_worker_pool_test)
    set_tests_properties(lamp_native_worker_pool PROPERTIES
        LABELS "lamp;execution;native;integration" TIMEOUT 30)

    add_executable(lamp_cancellation_propagation_test
        tests/cancellation_propagation_test.cpp)
    target_link_libraries(lamp_cancellation_propagation_test PRIVATE lamp::execution)
    target_compile_features(lamp_cancellation_propagation_test PRIVATE cxx_std_20)
    add_test(NAME lamp_cancellation_propagation COMMAND lamp_cancellation_propagation_test)
    set_tests_properties(lamp_cancellation_propagation PROPERTIES
        LABELS "lamp;execution;cancellation;regression" TIMEOUT 30)

    if(NOT TARGET lamp::v8)
        message(FATAL_ERROR
            "Lamp V8 worker pool tests require lamp::v8. Configure the V8 runtime before enabling the execution test suite.")
    endif()

    add_executable(lamp_v8_isolate_worker_pool_test
        tests/v8_isolate_worker_pool_test.cpp)
    target_link_libraries(lamp_v8_isolate_worker_pool_test PRIVATE lamp::execution lamp::v8)
    target_compile_features(lamp_v8_isolate_worker_pool_test PRIVATE cxx_std_20)
    add_test(NAME lamp_v8_isolate_worker_pool COMMAND lamp_v8_isolate_worker_pool_test)
    set_tests_properties(lamp_v8_isolate_worker_pool PROPERTIES
        LABELS "lamp;execution;v8;isolate;integration" TIMEOUT 60)

    add_executable(lamp_v8_isolate_worker_pool_lifecycle_test
        tests/v8_isolate_worker_pool_lifecycle_test.cpp)
    target_link_libraries(lamp_v8_isolate_worker_pool_lifecycle_test PRIVATE lamp::execution lamp::v8)
    target_compile_features(lamp_v8_isolate_worker_pool_lifecycle_test PRIVATE cxx_std_20)
    add_test(NAME lamp_v8_isolate_worker_pool_lifecycle COMMAND lamp_v8_isolate_worker_pool_lifecycle_test)
    set_tests_properties(lamp_v8_isolate_worker_pool_lifecycle PROPERTIES
        LABELS "lamp;execution;v8;isolate;lifecycle")

    add_executable(lamp_v8_promise_execution_contract_test
        tests/v8_promise_execution_contract_test.cpp)
    target_link_libraries(lamp_v8_promise_execution_contract_test PRIVATE lamp::execution lamp::v8)
    target_compile_features(lamp_v8_promise_execution_contract_test PRIVATE cxx_std_20)
    add_test(NAME lamp_v8_promise_execution_contract COMMAND lamp_v8_promise_execution_contract_test)
    set_tests_properties(lamp_v8_promise_execution_contract PROPERTIES
        LABELS "lamp;execution;v8;promise")

    add_executable(lamp_v8_promise_continuation_scheduler_test
        tests/v8_promise_continuation_scheduler_test.cpp)
    target_link_libraries(lamp_v8_promise_continuation_scheduler_test PRIVATE lamp::execution lamp::v8)
    target_compile_features(lamp_v8_promise_continuation_scheduler_test PRIVATE cxx_std_20)
    add_test(NAME lamp_v8_promise_continuation_scheduler COMMAND lamp_v8_promise_continuation_scheduler_test)
    set_tests_properties(lamp_v8_promise_continuation_scheduler PROPERTIES
        LABELS "lamp;execution;v8;promise;scheduler")

    add_executable(lamp_v8_async_worker_integration_test
        tests/v8_async_worker_integration_test.cpp)
    target_link_libraries(lamp_v8_async_worker_integration_test PRIVATE lamp::execution lamp::v8)
    target_compile_features(lamp_v8_async_worker_integration_test PRIVATE cxx_std_20)
    add_test(NAME lamp_v8_async_worker_integration COMMAND lamp_v8_async_worker_integration_test)
    set_tests_properties(lamp_v8_async_worker_integration PROPERTIES
        LABELS "lamp;execution;v8;promise;integration")

    add_executable(lamp_async_execution_dispatcher_test
        tests/async_execution_dispatcher_test.cpp)
    target_link_libraries(lamp_async_execution_dispatcher_test PRIVATE lamp::execution)
    target_compile_features(lamp_async_execution_dispatcher_test PRIVATE cxx_std_20)
    add_test(NAME lamp_async_execution_dispatcher COMMAND lamp_async_execution_dispatcher_test)
    set_tests_properties(lamp_async_execution_dispatcher PROPERTIES
        LABELS "lamp;execution;dispatcher")

    # End-to-end Promise integration: executor -> worker pool ->
    # continuation scheduler -> AsyncTaskCompletion.
    add_executable(lamp_v8_isolate_worker_pool_promise_integration_test
        tests/v8_isolate_worker_pool_promise_integration_test.cpp)
    target_link_libraries(lamp_v8_isolate_worker_pool_promise_integration_test
        PRIVATE lamp::execution lamp::v8)
    target_compile_features(lamp_v8_isolate_worker_pool_promise_integration_test
        PRIVATE cxx_std_20)
    add_test(NAME lamp_v8_isolate_worker_pool_promise_integration
        COMMAND lamp_v8_isolate_worker_pool_promise_integration_test)
    set_tests_properties(lamp_v8_isolate_worker_pool_promise_integration PROPERTIES
        LABELS "lamp;execution;v8;promise;integration;end-to-end"
        TIMEOUT 90)

    if(TARGET lamp::bridge_http)
        add_executable(lamp_v8_async_http_pump_integration_test
            tests/v8_async_http_pump_integration_test.cpp)
        target_link_libraries(lamp_v8_async_http_pump_integration_test
            PRIVATE lamp::execution lamp::v8 lamp::bridge_http)
        if(WIN32)
            target_link_libraries(lamp_v8_async_http_pump_integration_test
                PRIVATE ws2_32)
        endif()
        target_compile_features(lamp_v8_async_http_pump_integration_test
            PRIVATE cxx_std_20)
        add_test(NAME lamp_v8_async_http_pump_integration
            COMMAND lamp_v8_async_http_pump_integration_test)
        set_tests_properties(lamp_v8_async_http_pump_integration PROPERTIES
            LABELS "lamp;execution;bridge;http;v8;pump;integration;end-to-end"
            TIMEOUT 90)
    endif()

    message(STATUS "[Lamp Execution] Native worker pool tests enabled")
    message(STATUS "[Lamp Execution] Cancellation tests enabled")
    message(STATUS "[Lamp Execution] V8 isolate worker pool tests enabled")
    message(STATUS "[Lamp Execution] V8 Promise integration tests enabled")
endif()

message(STATUS "[Lamp Execution] Async execution engine configured")
message(STATUS "[Lamp Execution] Target: lamp::execution")