# SPDX-License-Identifier: Apache-2.0
# CMakeLists.txt — MicrOSAL
#
# Usage (standalone, non-Zephyr):
#
#   cmake -B build \
#         -DOSAL_BACKEND=LINUX \       # one of: FREERTOS ZEPHYR THREADX PX5 POSIX LINUX BAREMETAL VXWORKS NUTTX MICRIUM CHIBIOS EMBOS QNX CMSIS_RTOS CMSIS_RTOS2
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# For RTOS backends (FreeRTOS, ThreadX, PX5) point to SDK headers:
#   cmake -B build \
#         -DOSAL_BACKEND=FREERTOS \
#         -DOSAL_RTOS_INCLUDE_DIR=/path/to/FreeRTOS-Kernel/include
#
# Usage (Zephyr):
#   Standard west build — CMakeLists.txt detects DEFINED CONFIG_MICRO_OSAL.
#
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Zephyr module path — must appear before project() when building with west
# ---------------------------------------------------------------------------
if(DEFINED CONFIG_MICRO_OSAL)
    # We are inside the Zephyr build system.
    find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
    project(microsal LANGUAGES CXX)

    zephyr_library()
    zephyr_library_include_directories(include)
    zephyr_library_compile_definitions(OSAL_BACKEND_ZEPHYR)
    if(DEFINED CONFIG_MICRO_OSAL_TICK_WIDTH_64 AND CONFIG_MICRO_OSAL_TICK_WIDTH_64)
        zephyr_library_compile_definitions(OSAL_CFG_TICK_TYPE_U64)
    else()
        zephyr_library_compile_definitions(OSAL_CFG_TICK_TYPE_U32)
    endif()
    if(DEFINED CONFIG_TIMEOUT_64BIT AND CONFIG_TIMEOUT_64BIT)
        zephyr_library_compile_definitions(OSAL_CFG_NATIVE_TICK_BITS=64)
    else()
        zephyr_library_compile_definitions(OSAL_CFG_NATIVE_TICK_BITS=32)
    endif()
    if(DEFINED CONFIG_MICRO_OSAL_STRICT_TICK_INFERENCE AND CONFIG_MICRO_OSAL_STRICT_TICK_INFERENCE)
        zephyr_library_compile_definitions(OSAL_STRICT_TICK_INFERENCE=1)
    else()
        zephyr_library_compile_definitions(OSAL_STRICT_TICK_INFERENCE=0)
    endif()

    zephyr_library_sources(
        src/zephyr/zephyr_backend.cpp
        src/common/osal_c.cpp
    )
    return()
endif()

# ---------------------------------------------------------------------------
# Standalone cmake project
# ---------------------------------------------------------------------------
project(microsal
    VERSION 0.0.1
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(OSAL_BACKEND "LINUX" CACHE STRING
    "OSAL backend to compile: FREERTOS ZEPHYR THREADX PX5 POSIX LINUX BAREMETAL VXWORKS NUTTX MICRIUM CHIBIOS EMBOS QNX CMSIS_RTOS CMSIS_RTOS2")

set_property(CACHE OSAL_BACKEND PROPERTY STRINGS
    FREERTOS ZEPHYR THREADX PX5 POSIX LINUX BAREMETAL VXWORKS NUTTX MICRIUM CHIBIOS EMBOS QNX CMSIS_RTOS CMSIS_RTOS2)

option(OSAL_BUILD_TESTS     "Build OSAL unit tests"    OFF)
option(OSAL_BUILD_EXAMPLES  "Build OSAL examples"      OFF)
set(OSAL_TICK_WIDTH "32" CACHE STRING "OSAL tick width: 16, 32, or 64")
set_property(CACHE OSAL_TICK_WIDTH PROPERTY STRINGS 16 32 64)
set(OSAL_NATIVE_TICK_BITS "AUTO" CACHE STRING
    "Native backend tick width for compatibility checks: AUTO, 16, 32, or 64")
set_property(CACHE OSAL_NATIVE_TICK_BITS PROPERTY STRINGS AUTO 16 32 64)
set(OSAL_FREERTOS_TICK_BITS "AUTO" CACHE STRING
    "FreeRTOS native tick width hint when OSAL_NATIVE_TICK_BITS=AUTO: AUTO, 16, 32, or 64")
set_property(CACHE OSAL_FREERTOS_TICK_BITS PROPERTY STRINGS AUTO 16 32 64)
option(OSAL_ZEPHYR_TIMEOUT_64BIT
    "Zephyr native timeout width hint for standalone builds (ON=64-bit, OFF=32-bit)"
    OFF)
option(OSAL_STRICT_TICK_INFERENCE
    "Treat ambiguous AUTO native tick inference as a CMake error instead of warning"
    OFF)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# Backend source mapping
# ---------------------------------------------------------------------------
string(TOUPPER "${OSAL_BACKEND}" _OSAL_BACKEND_UPPER)

if(_OSAL_BACKEND_UPPER STREQUAL "FREERTOS")
    set(OSAL_BACKEND_SRC src/freertos/freertos_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_FREERTOS)
elseif(_OSAL_BACKEND_UPPER STREQUAL "ZEPHYR")
    set(OSAL_BACKEND_SRC src/zephyr/zephyr_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_ZEPHYR)
elseif(_OSAL_BACKEND_UPPER STREQUAL "THREADX")
    set(OSAL_BACKEND_SRC src/threadx/threadx_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_THREADX)
elseif(_OSAL_BACKEND_UPPER STREQUAL "PX5")
    set(OSAL_BACKEND_SRC src/px5/px5_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_PX5)
elseif(_OSAL_BACKEND_UPPER STREQUAL "POSIX")
    set(OSAL_BACKEND_SRC src/posix/posix_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_POSIX)
elseif(_OSAL_BACKEND_UPPER STREQUAL "LINUX")
    set(OSAL_BACKEND_SRC src/linux/linux_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_LINUX)
elseif(_OSAL_BACKEND_UPPER STREQUAL "BAREMETAL")
    set(OSAL_BACKEND_SRC src/bare_metal/bare_metal_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_BAREMETAL)
elseif(_OSAL_BACKEND_UPPER STREQUAL "VXWORKS")
    set(OSAL_BACKEND_SRC src/vxworks/vxworks_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_VXWORKS)
elseif(_OSAL_BACKEND_UPPER STREQUAL "NUTTX")
    set(OSAL_BACKEND_SRC src/nuttx/nuttx_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_NUTTX)
elseif(_OSAL_BACKEND_UPPER STREQUAL "MICRIUM")
    set(OSAL_BACKEND_SRC src/micrium/micrium_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_MICRIUM)
elseif(_OSAL_BACKEND_UPPER STREQUAL "CHIBIOS")
    set(OSAL_BACKEND_SRC src/chibios/chibios_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_CHIBIOS)
elseif(_OSAL_BACKEND_UPPER STREQUAL "EMBOS")
    set(OSAL_BACKEND_SRC src/embos/embos_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_EMBOS)
elseif(_OSAL_BACKEND_UPPER STREQUAL "QNX")
    set(OSAL_BACKEND_SRC src/qnx/qnx_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_QNX)
elseif(_OSAL_BACKEND_UPPER STREQUAL "CMSIS_RTOS")
    set(OSAL_BACKEND_SRC src/cmsis_rtos/cmsis_rtos_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_CMSIS_RTOS)
elseif(_OSAL_BACKEND_UPPER STREQUAL "CMSIS_RTOS2")
    set(OSAL_BACKEND_SRC src/cmsis_rtos2/cmsis_rtos2_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_CMSIS_RTOS2)
else()
    message(FATAL_ERROR
        "Unknown OSAL_BACKEND '${OSAL_BACKEND}'. "
        "Choose one of: FREERTOS ZEPHYR THREADX PX5 POSIX LINUX BAREMETAL VXWORKS NUTTX MICRIUM CHIBIOS EMBOS QNX CMSIS_RTOS CMSIS_RTOS2")
endif()

message(STATUS "OSAL backend: ${_OSAL_BACKEND_UPPER} (${OSAL_BACKEND_MACRO})")

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(microsal STATIC ${OSAL_BACKEND_SRC} src/common/osal_c.cpp)

target_include_directories(microsal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(microsal
    PUBLIC
        ${OSAL_BACKEND_MACRO}
)

if(OSAL_TICK_WIDTH STREQUAL "64")
    target_compile_definitions(microsal PUBLIC OSAL_CFG_TICK_TYPE_U64)
elseif(OSAL_TICK_WIDTH STREQUAL "16")
    target_compile_definitions(microsal PUBLIC OSAL_CFG_TICK_TYPE_U16)
elseif(OSAL_TICK_WIDTH STREQUAL "32")
    target_compile_definitions(microsal PUBLIC OSAL_CFG_TICK_TYPE_U32)
else()
    message(FATAL_ERROR "Invalid OSAL_TICK_WIDTH='${OSAL_TICK_WIDTH}'. Use 16, 32, or 64.")
endif()

if(OSAL_NATIVE_TICK_BITS STREQUAL "AUTO")
    if(_OSAL_BACKEND_UPPER MATCHES "LINUX|POSIX|NUTTX|QNX")
        # POSIX-family uses absolute-time APIs with 64-bit time domain.
        set(_OSAL_NATIVE_TICK_BITS "64")
    elseif(_OSAL_BACKEND_UPPER STREQUAL "FREERTOS")
        # FreeRTOS tick width is configuration-dependent.
        if(OSAL_FREERTOS_TICK_BITS STREQUAL "AUTO")
            set(_OSAL_NATIVE_TICK_BITS "32")
            if(OSAL_STRICT_TICK_INFERENCE)
                message(FATAL_ERROR
                    "OSAL_NATIVE_TICK_BITS=AUTO and backend=FREERTOS is ambiguous. "
                    "Set -DOSAL_FREERTOS_TICK_BITS=16/32/64 to match your FreeRTOS configuration "
                    "(for example configUSE_16_BIT_TICKS or configTICK_TYPE_WIDTH_IN_BITS), "
                    "or disable OSAL_STRICT_TICK_INFERENCE.")
            else()
                message(WARNING
                    "OSAL_NATIVE_TICK_BITS=AUTO and backend=FREERTOS: defaulting native tick width to 32. "
                    "Set -DOSAL_FREERTOS_TICK_BITS=16/32/64 to match your FreeRTOS configuration "
                    "(for example configUSE_16_BIT_TICKS or configTICK_TYPE_WIDTH_IN_BITS).")
            endif()
        else()
            set(_OSAL_NATIVE_TICK_BITS "${OSAL_FREERTOS_TICK_BITS}")
        endif()
    elseif(_OSAL_BACKEND_UPPER STREQUAL "ZEPHYR")
        # In standalone mode we don't have CONFIG_TIMEOUT_64BIT from west/Kconfig.
        if(OSAL_ZEPHYR_TIMEOUT_64BIT)
            set(_OSAL_NATIVE_TICK_BITS "64")
        else()
            set(_OSAL_NATIVE_TICK_BITS "32")
        endif()
    else()
        # Most remaining RTOS wrappers expose 32-bit tick APIs.
        set(_OSAL_NATIVE_TICK_BITS "32")
    endif()
else()
    set(_OSAL_NATIVE_TICK_BITS "${OSAL_NATIVE_TICK_BITS}")
endif()

if(NOT _OSAL_NATIVE_TICK_BITS MATCHES "^(16|32|64)$")
    message(FATAL_ERROR "Invalid OSAL_NATIVE_TICK_BITS='${OSAL_NATIVE_TICK_BITS}'. Use AUTO, 16, 32, or 64.")
endif()

if(NOT OSAL_FREERTOS_TICK_BITS MATCHES "^(AUTO|16|32|64)$")
    message(FATAL_ERROR "Invalid OSAL_FREERTOS_TICK_BITS='${OSAL_FREERTOS_TICK_BITS}'. Use AUTO, 16, 32, or 64.")
endif()

target_compile_definitions(microsal PUBLIC OSAL_CFG_NATIVE_TICK_BITS=${_OSAL_NATIVE_TICK_BITS})
message(STATUS "OSAL native tick bits: ${_OSAL_NATIVE_TICK_BITS}")

target_compile_features(microsal PUBLIC cxx_std_17)

# Enable more warnings in development.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(microsal PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wno-unused-parameter
        -fno-exceptions -fno-rtti
    )
endif()

# POSIX / Linux / NuttX / QNX require pthread.
if(_OSAL_BACKEND_UPPER MATCHES "POSIX|LINUX|NUTTX|QNX")
    find_package(Threads REQUIRED)
    target_link_libraries(microsal PUBLIC Threads::Threads)
    if(_OSAL_BACKEND_UPPER MATCHES "LINUX|QNX")
        target_link_libraries(microsal PUBLIC rt) # for timer_create
    endif()
endif()

# ---------------------------------------------------------------------------
# RTOS SDK include path (FreeRTOS, ThreadX, PX5)
# ---------------------------------------------------------------------------
# Backends like Zephyr, POSIX, Linux, and bare-metal pick up their headers
# from the system or Zephyr build, so this is only needed for RTOSes whose
# SDK lives outside the standard search path.
#
# Usage:
#   cmake -B build -DOSAL_BACKEND=FREERTOS \
#         -DOSAL_RTOS_INCLUDE_DIR=/path/to/FreeRTOS-Kernel/include
#
# Multiple directories can also be supplied via a semicolon-separated list:
#   -DOSAL_RTOS_INCLUDE_DIR="/path/to/FreeRTOS/include;/path/to/FreeRTOS/portable/GCC/ARM_CM4F"
if(_OSAL_BACKEND_UPPER MATCHES "FREERTOS|THREADX|PX5|VXWORKS|MICRIUM|CHIBIOS|EMBOS|CMSIS_RTOS|CMSIS_RTOS2")
    set(OSAL_RTOS_INCLUDE_DIR "" CACHE PATH
        "Path(s) to RTOS SDK headers (FreeRTOS, ThreadX, PX5, VxWorks, Micrium, ChibiOS, embOS). Semicolon-separated.")

    if(OSAL_RTOS_INCLUDE_DIR)
        target_include_directories(microsal PRIVATE ${OSAL_RTOS_INCLUDE_DIR})
        message(STATUS "OSAL RTOS include: ${OSAL_RTOS_INCLUDE_DIR}")
    else()
        message(WARNING
            "OSAL_RTOS_INCLUDE_DIR is not set. "
            "The ${_OSAL_BACKEND_UPPER} backend requires RTOS SDK headers on the include path. "
            "Set -DOSAL_RTOS_INCLUDE_DIR=/path/to/sdk/include or ensure the headers are "
            "reachable via your toolchain's default search paths.")
    endif()
endif()

# FreeRTOS: the caller may also need to link the FreeRTOS kernel library.
# Expected usage: target_link_libraries(myapp microsal freertos_kernel)

# ---------------------------------------------------------------------------
# Alias for find_package consumers
# ---------------------------------------------------------------------------
add_library(MicrOSAL::OSAL ALIAS microsal)

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS microsal
    EXPORT  microsal-config
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT microsal-config
    NAMESPACE  MicrOSAL::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/microsal
)

# ---------------------------------------------------------------------------
# Optional sub-projects
# ---------------------------------------------------------------------------
if(OSAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OSAL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
