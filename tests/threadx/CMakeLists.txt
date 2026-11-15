# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(microsal_threadx_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The ThreadX SDK tests require the upstream Linux/GNU port.")
endif()

include(FetchContent)
find_package(Threads REQUIRED)

set(THREADX_ARCH linux CACHE STRING "" FORCE)
set(THREADX_TOOLCHAIN gnu CACHE STRING "" FORCE)
FetchContent_Declare(threadx
    GIT_REPOSITORY https://github.com/eclipse-threadx/threadx.git
    # v6.4.2_rel, pinned so adapter validation is reproducible.
    GIT_TAG 06dabb0ad0403f59889ce85eeb3d48dbd9d3759b
)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(threadx doctest)

add_library(microsal STATIC
    ../../src/threadx/threadx_backend.cpp
    ../../src/common/osal_c.cpp
)
target_include_directories(microsal PUBLIC ../../include)
target_compile_definitions(microsal PUBLIC
    OSAL_BACKEND_THREADX OSAL_CFG_TICK_TYPE_U32 OSAL_CFG_NATIVE_TICK_BITS=32)
target_link_libraries(microsal PUBLIC threadx Threads::Threads)
target_compile_options(microsal PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
include(../../cmake/osal_diagnostics.cmake)
osal_target_enable_alignment_diagnostics(microsal)

add_executable(test_threadx src/main.cpp)
target_link_libraries(test_threadx PRIVATE microsal doctest::doctest)
target_compile_definitions(test_threadx PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(test_threadx PRIVATE -Wall -Wextra -fno-exceptions)
osal_target_enable_alignment_diagnostics(test_threadx)

enable_testing()
add_test(NAME test_threadx COMMAND test_threadx)
set_tests_properties(test_threadx PROPERTIES TIMEOUT 30)
