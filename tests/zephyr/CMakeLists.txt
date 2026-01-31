# SPDX-License-Identifier: Apache-2.0
# tests/zephyr/CMakeLists.txt — Zephyr ztest application for MicrOSAL
#
# Build with `west build` or `twister`:
#
#   west build -b native_sim tests/zephyr -- -DEXTRA_ZEPHYR_MODULES=/path/to/micrOSAL
#   west twister -T tests/zephyr
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microsal_ztest LANGUAGES CXX)

target_sources(app PRIVATE
    src/main.cpp
)

# MicrOSAL public headers
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# Compile as C++17 with embedded flags
target_compile_features(app PRIVATE cxx_std_17)

# The module library gets OSAL_BACKEND_ZEPHYR via zephyr_library_compile_definitions(),
# but the app target needs it too so the public headers resolve active_traits.
if(CONFIG_MICRO_OSAL_BACKEND_ZEPHYR)
    target_compile_definitions(app PRIVATE OSAL_BACKEND_ZEPHYR)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(app PRIVATE -fno-exceptions -fno-rtti)
endif()
