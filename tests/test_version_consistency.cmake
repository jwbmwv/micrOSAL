# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED OSAL_TEST_BINARY_DIR)
    message(FATAL_ERROR "Pass OSAL_TEST_BINARY_DIR for generated version-check fixtures.")
endif()

function(check_fixture name header cmake_version doxygen expected_status expected_message)
    set(root "${OSAL_TEST_BINARY_DIR}/version-fixtures/${name}")
    file(MAKE_DIRECTORY "${root}/include/osal")
    file(WRITE "${root}/include/osal/version.hpp" "${header}")
    file(WRITE "${root}/CMakeLists.txt" "project(microsal\n    VERSION ${cmake_version}\n    LANGUAGES CXX)\n")
    file(WRITE "${root}/Doxyfile" "${doxygen}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DOSAL_VERSION_SOURCE_DIR=${root}"
            -P "${CMAKE_CURRENT_LIST_DIR}/../cmake/check_version.cmake"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if((expected_status STREQUAL "pass" AND NOT status STREQUAL "0") OR
       (expected_status STREQUAL "fail" AND status STREQUAL "0") OR
       NOT "${output}${errors}" MATCHES "${expected_message}")
        message(FATAL_ERROR "${name}: unexpected version-check result (${status}):\n${output}${errors}")
    endif()
endfunction()

set(header "#define MICRO_OSAL_VERSION_MAJOR 2\n#define MICRO_OSAL_VERSION_MINOR 3\n#define MICRO_OSAL_VERSION_PATCH 4\n")
check_fixture(matching "${header}" 2.3.4 "PROJECT_NUMBER = 2.3.4\n" pass "versions agree: 2.3.4")
check_fixture(quoted_doxygen "${header}" 2.3.4 "PROJECT_NUMBER = \"2.3.4\"\n" pass "versions agree: 2.3.4")
check_fixture(cmake_mismatch "${header}" 2.3.5 "PROJECT_NUMBER = 2.3.4\n" fail "version mismatch")
check_fixture(header_mismatch "${header}" 2.3.5 "PROJECT_NUMBER = 2.3.5\n" fail "version mismatch")
check_fixture(doxygen_mismatch "${header}" 2.3.4 "PROJECT_NUMBER = 2.3.5\n" fail "version mismatch")
check_fixture(malformed_header "#define MICRO_OSAL_VERSION_MAJOR broken\n" 2.3.4
    "PROJECT_NUMBER = 2.3.4\n" fail "malformed MICRO_OSAL_VERSION_MAJOR")
check_fixture(duplicate_doxygen "${header}" 2.3.4
    "PROJECT_NUMBER = 2.3.4\nPROJECT_NUMBER = 2.3.4\n" fail "malformed PROJECT_NUMBER")
check_fixture(unclosed_doxygen_quote "${header}" 2.3.4
    "PROJECT_NUMBER = \"2.3.4\n" fail "malformed PROJECT_NUMBER")
check_fixture(missing_cmake_version "${header}" broken
    "PROJECT_NUMBER = 2.3.4\n" fail "malformed microsal project VERSION")
