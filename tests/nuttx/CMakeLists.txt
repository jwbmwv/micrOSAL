# SPDX-License-Identifier: Apache-2.0
# tests/nuttx/CMakeLists.txt
#
# NuttX CMake application descriptor for the MicrOSAL test suite.
#
# This file is consumed by the NuttX CMake build system when the directory is
# placed (or symlinked) under nuttx-apps/testing/microsal_test.
#
# Integration:
#   ln -s <micrOSAL_root>/tests/nuttx nuttx-apps/testing/microsal_test
#
# Then from the NuttX source directory:
#   cmake -B build -DBOARD_CONFIG=sim/nsh
#   ninja -C build -j$(nproc)
#
# MICROSAL_ROOT may be passed explicitly:
#   cmake -B build ... -DMICROSAL_ROOT=<path>

# Guard: only build when the Kconfig symbol is enabled.
if(CONFIG_TESTING_MICROSAL_TEST)

    # Resolve the micrOSAL root.  CMAKE_CURRENT_LIST_DIR is the *logical* path
    # (through the nuttx-apps symlink). Use `realpath` to resolve symlinks
    # before traversing ../../.. so we land in the real project root.
    # (Avoids CMake policy CMP0152 issues with file(REAL_PATH) on CMake 3.28.)
    if(NOT DEFINED MICROSAL_ROOT)
        execute_process(
            COMMAND realpath "${CMAKE_CURRENT_LIST_DIR}"
            OUTPUT_VARIABLE _real_list_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        get_filename_component(MICROSAL_ROOT "${_real_list_dir}/../.." ABSOLUTE)
    endif()
    set(MICROSAL_INCLUDE_DIR "${MICROSAL_ROOT}/include")
    set(MICROSAL_COMMON_DIR  "${MICROSAL_ROOT}/src/common")
    # nuttx_add_application uses file(GLOB) to validate SRCS, which only finds
    # files in glob-reachable paths.  The backend lives in the micrOSAL source
    # tree; we include it via a local shim file in this app dir.
    set(MICROSAL_NUTTX_SRC   src/nuttx_backend_impl.cpp)

    # NuttX sim builds use -nostdinc++ which removes GCC C++ stdlib headers.
    # Restore a minimal set via -isystem so they rank lower than NuttX headers.
    set(_GCC_CXX_ISYSTEM_FLAGS "")
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-multiarch
        OUTPUT_VARIABLE _multiarch
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    file(GLOB _cxx_ver_dirs LIST_DIRECTORIES true /usr/include/c++/*)
    foreach(_d ${_cxx_ver_dirs})
        if(IS_DIRECTORY "${_d}" AND EXISTS "${_d}/atomic")
            list(APPEND _GCC_CXX_ISYSTEM_FLAGS "-isystem${_d}")
            if(_multiarch)
                get_filename_component(_ver "${_d}" NAME)
                set(_arch_dir "/usr/include/${_multiarch}/c++/${_ver}")
                if(IS_DIRECTORY "${_arch_dir}")
                    list(APPEND _GCC_CXX_ISYSTEM_FLAGS "-isystem${_arch_dir}")
                endif()
            endif()
            break()
        endif()
    endforeach()

    # nuttx_add_application handles target naming (apps_microsal_test) and
    # automatically applies -Dmain=microsal_test_main to src/main.cpp.
    nuttx_add_application(
        NAME        microsal_test
        STACKSIZE   ${CONFIG_TESTING_MICROSAL_TEST_STACKSIZE}
        PRIORITY    ${CONFIG_TESTING_MICROSAL_TEST_PRIORITY}
        SRCS
            src/main.cpp
            ${MICROSAL_NUTTX_SRC}
        INCLUDE_DIRECTORIES
            ${MICROSAL_INCLUDE_DIR}
            ${MICROSAL_COMMON_DIR}
        DEFINITIONS
            OSAL_BACKEND_NUTTX
            OSAL_THREAD_LOCAL_USE_NATIVE=1
        COMPILE_FLAGS
            -std=c++20
            -fno-exceptions
            -fno-rtti
            -Wall
            -Wextra
            -Wno-unused-parameter
            ${_GCC_CXX_ISYSTEM_FLAGS}
    )

endif()
