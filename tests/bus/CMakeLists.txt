# SPDX-License-Identifier: Apache-2.0
# tests/bus/CMakeLists.txt — bus pub/sub test targets

function(bus_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE microsal doctest::doctest)
    target_compile_definitions(${name} PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE -fno-exceptions)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

bus_add_test(test_osal_signal_lcd      test_osal_signal_lcd.cpp)
bus_add_test(test_osal_signal_premium  test_osal_signal_premium.cpp)
bus_add_test(test_osal_signal_backends test_osal_signal_backends.cpp)
