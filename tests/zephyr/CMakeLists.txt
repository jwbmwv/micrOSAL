# SPDX-License-Identifier: Apache-2.0
# tests/zephyr/CMakeLists.txt — Zephyr ztest application for MicrOSAL
#
# Build with `west build` or `twister`:
#
#   west build -b native_sim tests/zephyr -- -DEXTRA_ZEPHYR_MODULES=/path/to/micrOSAL
#   west twister -T tests/zephyr
#
cmake_minimum_required(VERSION 3.20.0)

list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_LIST_DIR}/../..)

# Merge the C++20 dialect choice after prj.conf so Zephyr's Kconfig choice is
# preserved in the generated .config.
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/cpp20.conf)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microsal_ztest LANGUAGES CXX)

target_sources(app PRIVATE
    src/main.cpp
)

# MicrOSAL public headers
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# The module library gets OSAL_BACKEND_ZEPHYR via zephyr_library_compile_definitions(),
# but the app target needs it too so the public headers resolve active_traits.
if(CONFIG_MICRO_OSAL_BACKEND_ZEPHYR)
    target_compile_definitions(app PRIVATE OSAL_BACKEND_ZEPHYR)
endif()

# Zephyr selects the C++ dialect from Kconfig via cpp20.conf; only keep the
# embedded-facing ABI constraints here.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(app PRIVATE -fno-exceptions -fno-rtti)
endif()
