# SPDX-License-Identifier: Apache-2.0
# examples/bus/CMakeLists.txt

function(bus_add_example name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE microsal)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE
            -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
    endif()
endfunction()

bus_add_example(example_signal_lcd     example_signal_lcd.cpp)
bus_add_example(example_signal_zephyr  example_signal_zephyr.cpp)
