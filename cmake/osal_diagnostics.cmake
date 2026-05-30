# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

function(osal_target_enable_alignment_diagnostics target)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wcast-align
        -Waddress-of-packed-member
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            -Wcast-align=strict
        )
    endif()
endfunction()