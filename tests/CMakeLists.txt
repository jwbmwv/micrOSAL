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
osal_add_test(test_thread       test_thread.cpp)
osal_add_test(test_timer        test_timer.cpp)
osal_add_test(test_event_flags  test_event_flags.cpp)
osal_add_test(test_wait_set     test_wait_set.cpp)
osal_add_test(test_result       test_result.cpp)
osal_add_test(test_condvar      test_condvar.cpp)
osal_add_test(test_work_queue   test_work_queue.cpp)
osal_add_test(test_integration  test_integration.cpp)
osal_add_test(test_stream_buffer         test_stream_buffer.cpp)
osal_add_test(test_message_buffer        test_message_buffer.cpp)
osal_add_test(test_rwlock                test_rwlock.cpp)
osal_add_test(test_memory_pool           test_memory_pool.cpp)
osal_add_test(test_ring_buffer           test_ring_buffer.cpp)
osal_add_test(test_thread_local_data     test_thread_local_data.cpp)
osal_add_test(test_spinlock              test_spinlock.cpp)
osal_add_test(test_barrier               test_barrier.cpp)

# ---------------------------------------------------------------------------
# C API test — links a pure-C compilation check with a doctest C++ harness
# ---------------------------------------------------------------------------
add_executable(test_c_api test_c_api.cpp c_api_c_check.c)
target_link_libraries(test_c_api PRIVATE microsal doctest::doctest)
target_compile_definitions(test_c_api PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
)
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
