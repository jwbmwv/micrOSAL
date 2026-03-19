# SPDX-License-Identifier: Apache-2.0
# tests/freertos/CMakeLists.txt
#
# Self-contained FreeRTOS test build.
# Uses the FreeRTOS-Kernel POSIX simulation port so the tests run as an
# ordinary Linux/macOS process — no cross-compiler or QEMU required.
#
# Build & run:
#   cmake -B build-freertos tests/freertos -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build-freertos -j$(nproc)
#   ctest --test-dir build-freertos --output-on-failure

cmake_minimum_required(VERSION 3.20)
project(microsal_freertos_tests CXX C)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_C_STANDARD            11)

include(FetchContent)

# ---------------------------------------------------------------------------
# FreeRTOS-Kernel (POSIX simulation port)
# ---------------------------------------------------------------------------
# The V11 FreeRTOS-Kernel cmake expects a `freertos_config` INTERFACE library
# that exposes the directory containing FreeRTOSConfig.h.
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}   # our FreeRTOSConfig.h lives here
)

set(FREERTOS_PORT GCC_POSIX CACHE INTERNAL "FreeRTOS port selection")
set(FREERTOS_HEAP "3"       CACHE INTERNAL "FreeRTOS heap selection (3 = wrap malloc)")

FetchContent_Declare(
    FreeRTOS_Kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.1.0
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(FreeRTOS_Kernel)

# ---------------------------------------------------------------------------
# MicrOSAL — FreeRTOS backend
# ---------------------------------------------------------------------------
# Build the library directly here rather than calling add_subdirectory on the
# root CMakeLists.txt so we can inject the FreeRTOS definitions cleanly.
add_library(microsal STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/freertos/freertos_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/osal_c.cpp
)

target_include_directories(microsal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_definitions(microsal PUBLIC
    OSAL_BACKEND_FREERTOS
    OSAL_CFG_TICK_TYPE_U32
    OSAL_CFG_NATIVE_TICK_BITS=32
    # Dynamic alloc: simpler test code; no static TCB carving from stack buffer.
    OSAL_FREERTOS_DYNAMIC_ALLOC
)

target_link_libraries(microsal PUBLIC freertos_kernel)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(microsal PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-exceptions -fno-rtti
    )
endif()

# ---------------------------------------------------------------------------
# doctest (same version as the Linux/POSIX suite)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(doctest)

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
add_executable(test_freertos src/main.cpp)

target_include_directories(test_freertos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}           # FreeRTOSConfig.h
    ${doctest_SOURCE_DIR}                 # gives <doctest/doctest.h>
)

target_compile_definitions(test_freertos PRIVATE
    OSAL_BACKEND_FREERTOS
    OSAL_CFG_TICK_TYPE_U32
    OSAL_CFG_NATIVE_TICK_BITS=32
    OSAL_FREERTOS_DYNAMIC_ALLOC
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
)

target_compile_features(test_freertos PRIVATE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_freertos PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-exceptions -fno-rtti
    )
endif()

target_link_libraries(test_freertos PRIVATE microsal freertos_kernel pthread)

# ---------------------------------------------------------------------------
# CTest registration
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME test_freertos COMMAND test_freertos)
set_tests_properties(test_freertos PROPERTIES TIMEOUT 120)
