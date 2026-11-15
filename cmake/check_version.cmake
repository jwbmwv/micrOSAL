# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED OSAL_VERSION_SOURCE_DIR)
    get_filename_component(OSAL_VERSION_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

foreach(component MAJOR MINOR PATCH)
    file(STRINGS "${OSAL_VERSION_SOURCE_DIR}/include/osal/version.hpp" definitions
        REGEX "^#define[ \t]+MICRO_OSAL_VERSION_${component}[ \t]+")
    list(LENGTH definitions count)
    if(NOT count EQUAL 1 OR NOT definitions MATCHES
        "^#define[ \t]+MICRO_OSAL_VERSION_${component}[ \t]+([0-9]+)[ \t]*$")
        message(FATAL_ERROR "Missing, duplicate, or malformed MICRO_OSAL_VERSION_${component} in version.hpp")
    endif()
    set(version_${component} "${CMAKE_MATCH_1}")
endforeach()
set(header_version "${version_MAJOR}.${version_MINOR}.${version_PATCH}")

file(READ "${OSAL_VERSION_SOURCE_DIR}/CMakeLists.txt" cmake_source)
if(NOT cmake_source MATCHES
    "(^|[\r\n])[ \t]*project\\([ \t\r\n]*microsal[ \t\r\n]+VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)[ \t\r\n\\)]")
    message(FATAL_ERROR "Missing or malformed microsal project VERSION in CMakeLists.txt")
endif()
set(cmake_version "${CMAKE_MATCH_2}")

file(STRINGS "${OSAL_VERSION_SOURCE_DIR}/Doxyfile" definitions REGEX "^PROJECT_NUMBER[ \t]*=")
list(LENGTH definitions count)
if(NOT count EQUAL 1 OR NOT definitions MATCHES
    "^PROJECT_NUMBER[ \t]*=[ \t]*(\"([0-9]+\\.[0-9]+\\.[0-9]+)\"|([0-9]+\\.[0-9]+\\.[0-9]+))[ \t]*$")
    message(FATAL_ERROR "Missing, duplicate, or malformed PROJECT_NUMBER in Doxyfile")
endif()
set(doxygen_version "${CMAKE_MATCH_2}${CMAKE_MATCH_3}")

if(NOT header_version STREQUAL cmake_version OR NOT header_version STREQUAL doxygen_version)
    message(FATAL_ERROR
        "MicrOSAL version mismatch: version.hpp=${header_version}, "
        "CMakeLists.txt=${cmake_version}, Doxyfile=${doxygen_version}")
endif()
message(STATUS "MicrOSAL versions agree: ${header_version}")
