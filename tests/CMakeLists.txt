# SPDX-License-Identifier: Apache-2.0
# tests/CMakeLists.txt — MicrOSAL test suite
#
# Test framework: doctest (header-only, no-exceptions compatible)
# Fetched automatically via FetchContent.

include(FetchContent)

FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

# ---------------------------------------------------------------------------
# Helper: register one test source as a CTest target
# ---------------------------------------------------------------------------
function(osal_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE microsal doctest::doctest)
    target_compile_definitions(${name} PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    )
    osal_target_enable_alignment_diagnostics(${name})
    # Re-enable RTTI for test harness (doctest uses typeid internally)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE -fno-exceptions)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------
osal_add_test(test_clock        test_clock.cpp)
osal_add_test(test_mutex        test_mutex.cpp)
osal_add_test(test_semaphore    test_semaphore.cpp)
osal_add_test(test_queue        test_queue.cpp)
osal_add_test(test_mailbox      test_mailbox.cpp)
osal_add_test(test_thread       test_thread.cpp)
osal_add_test(test_timer        test_timer.cpp)
osal_add_test(test_event_flags  test_event_flags.cpp)
osal_add_test(test_wait_set     test_wait_set.cpp)
osal_add_test(test_result       test_result.cpp)
osal_add_test(test_condvar      test_condvar.cpp)
osal_add_test(test_work_queue   test_work_queue.cpp)
osal_add_test(test_delayable_work test_delayable_work.cpp)
osal_add_test(test_notification   test_notification.cpp)
osal_add_test(test_object_wait_set test_object_wait_set.cpp)
osal_add_test(test_integration  test_integration.cpp)
osal_add_test(test_stream_buffer         test_stream_buffer.cpp)
osal_add_test(test_message_buffer        test_message_buffer.cpp)
osal_add_test(test_rwlock                test_rwlock.cpp)
osal_add_test(test_memory_pool           test_memory_pool.cpp)
osal_add_test(test_ring_buffer           test_ring_buffer.cpp)
osal_add_test(test_thread_local_data     test_thread_local_data.cpp)
osal_add_test(test_spinlock              test_spinlock.cpp)
osal_add_test(test_barrier               test_barrier.cpp)
osal_add_test(test_embedded_adapters     test_embedded_adapters.cpp)
osal_add_test(test_cancellation_wait    test_cancellation_wait.cpp)
osal_add_test(test_checked_lifecycle    test_checked_lifecycle.cpp)
osal_add_test(test_diagnostics          test_diagnostics.cpp)
osal_add_test(test_native_semantics     test_native_semantics.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(test_checked_lifecycle PRIVATE OSAL_TEST_CHECKED_LIFECYCLE_FAULT_INJECTION)
    target_link_options(test_checked_lifecycle PRIVATE
        "LINKER:--wrap=osal_semaphore_destroy"
        "LINKER:--wrap=osal_timer_destroy"
        "LINKER:--wrap=osal_queue_destroy"
        "LINKER:--wrap=osal_work_queue_destroy"
        "LINKER:--wrap=osal_queue_create")
    target_compile_definitions(test_cancellation_wait PRIVATE
        OSAL_TEST_WAIT_SIGNAL_FAULTS OSAL_TEST_WAIT_SIGNAL_DESTROY_FAULTS)
    target_link_options(test_cancellation_wait PRIVATE
        "LINKER:--wrap=osal_semaphore_create"
        "LINKER:--wrap=osal_semaphore_take"
        "LINKER:--wrap=osal_semaphore_give"
        "LINKER:--wrap=osal_semaphore_destroy")
    target_compile_definitions(test_queue PRIVATE OSAL_TEST_QUEUE_FAULT_INJECTION)
    target_link_options(test_queue PRIVATE
        "LINKER:--wrap=osal_queue_create"
        "LINKER:--wrap=osal_queue_receive"
    )
    if(OSAL_BACKEND STREQUAL "LINUX")
        target_compile_definitions(test_timer PRIVATE OSAL_TEST_TIMER_FAULT_INJECTION)
        target_link_options(test_timer PRIVATE "LINKER:--wrap=poll")
        osal_add_test(test_delayable_work_faults test_delayable_work_faults.cpp)
        target_link_options(test_delayable_work_faults PRIVATE
            "LINKER:--wrap=osal_clock_monotonic_ms"
            "LINKER:--wrap=osal_clock_ticks"
            "LINKER:--wrap=osal_clock_tick_period_us"
            "LINKER:--wrap=osal_queue_create"
            "LINKER:--wrap=osal_queue_receive"
            "LINKER:--wrap=osal_timer_create"
            "LINKER:--wrap=osal_timer_start"
            "LINKER:--wrap=osal_timer_stop"
            "LINKER:--wrap=osal_timer_set_period"
            "LINKER:--wrap=osal_work_queue_submit")
    endif()
endif()

# ---------------------------------------------------------------------------
# C API test — links a pure-C compilation check with a doctest C++ harness
# ---------------------------------------------------------------------------
add_executable(test_c_api test_c_api.cpp c_api_c_check.c)
target_link_libraries(test_c_api PRIVATE microsal doctest::doctest)
target_compile_definitions(test_c_api PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
)
osal_target_enable_alignment_diagnostics(test_c_api)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # C++ flags
    set_source_files_properties(test_c_api.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
    # C flags — compile as C11 to prove header C-compatibility
    set_source_files_properties(c_api_c_check.c PROPERTIES
        LANGUAGE C
        COMPILE_OPTIONS "-std=c11;-Wall;-Wextra;-Wpedantic"
    )
endif()
add_test(NAME test_c_api COMMAND test_c_api)

# ---------------------------------------------------------------------------
# Channel / pub-sub tests
# ---------------------------------------------------------------------------
add_subdirectory(bus)

if(OSAL_BACKEND STREQUAL "LINUX" OR OSAL_BACKEND STREQUAL "POSIX")
    osal_add_test(test_mutex_policy test_mutex_policy.cpp)
    osal_add_test(test_backend_conformance test_backend_conformance.cpp)
    osal_add_test(test_emulation_concurrency test_emulation_concurrency.cpp)

    file(READ "${PROJECT_SOURCE_DIR}/README.md" readme)
    string(REGEX MATCH "```cpp\n([^`]+)```" example "${readme}")
    if(NOT example)
        message(FATAL_ERROR "README C++ quick-start example was not found")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/readme_example.cpp" "${CMAKE_MATCH_1}")
    add_executable(test_readme_example "${CMAKE_CURRENT_BINARY_DIR}/readme_example.cpp")
    target_link_libraries(test_readme_example PRIVATE microsal)
    add_test(NAME test_readme_example COMMAND test_readme_example)

    configure_file(test_package.cmake.in test_package.cmake @ONLY)
    add_test(NAME test_installed_package
        COMMAND ${CMAKE_COMMAND} "-DOSAL_TEST_CONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_BINARY_DIR}/test_package.cmake")
endif()
