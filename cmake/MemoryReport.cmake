# Memory reporting utilities for micrOSAL build artifacts
# Extracts RAM/FLASH usage from ELF binaries and generates reports

# Add a memory report target for an executable
# Usage: add_memory_report(TARGET target_name)
function(add_memory_report)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "TARGET" "")

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "add_memory_report: TARGET argument required")
    endif()

    if(NOT TARGET ${ARG_TARGET})
        message(FATAL_ERROR "add_memory_report: ${ARG_TARGET} is not a target")
    endif()

    # Find required tools
    find_program(SIZE_TOOL
        NAMES ${CMAKE_SIZE} arm-none-eabi-size size
        DOC "Binary size analysis tool"
    )

    find_program(NM_TOOL
        NAMES ${CMAKE_NM} arm-none-eabi-nm nm
        DOC "Symbol listing tool"
    )

    if(NOT SIZE_TOOL)
        message(STATUS "Memory report: 'size' tool not found, skipping for ${ARG_TARGET}")
        return()
    endif()

    # Create custom target for memory report
    set(REPORT_TARGET "${ARG_TARGET}_memory_report")
    set(REPORT_FILE "${CMAKE_BINARY_DIR}/${ARG_TARGET}_memory.txt")

    add_custom_target(${REPORT_TARGET}
        COMMAND ${CMAKE_COMMAND} -E echo "==================================================================="
        COMMAND ${CMAKE_COMMAND} -E echo "Memory Report for: ${ARG_TARGET}"
        COMMAND ${CMAKE_COMMAND} -E echo "==================================================================="
        COMMAND ${CMAKE_COMMAND} -E echo ""
        COMMAND ${SIZE_TOOL} -A $<TARGET_FILE:${ARG_TARGET}>
        COMMAND ${CMAKE_COMMAND} -E echo ""
        COMMAND ${CMAKE_COMMAND} -E echo "-------------------------------------------------------------------"
        COMMAND ${CMAKE_COMMAND} -E echo "Berkeley format:"
        COMMAND ${SIZE_TOOL} -B $<TARGET_FILE:${ARG_TARGET}>
        COMMAND ${CMAKE_COMMAND} -E echo ""
        DEPENDS ${ARG_TARGET}
        COMMENT "Generating memory report for ${ARG_TARGET}"
        VERBATIM
    )

    # Optional: Add symbol analysis if nm is available
    if(NM_TOOL)
        add_custom_target(${ARG_TARGET}_symbols
            COMMAND ${CMAKE_COMMAND} -E echo "==================================================================="
            COMMAND ${CMAKE_COMMAND} -E echo "Top 20 Largest Symbols in: ${ARG_TARGET}"
            COMMAND ${CMAKE_COMMAND} -E echo "==================================================================="
            COMMAND ${CMAKE_COMMAND} -E echo ""
            COMMAND ${NM_TOOL} --print-size --size-sort --radix=d $<TARGET_FILE:${ARG_TARGET}> | tail -20
            COMMAND ${CMAKE_COMMAND} -E echo ""
            DEPENDS ${ARG_TARGET}
            COMMENT "Analyzing symbols for ${ARG_TARGET}"
            VERBATIM
        )
    endif()

    # Add to a global memory_reports target
    if(NOT TARGET memory_reports)
        add_custom_target(memory_reports
            COMMENT "Generate memory reports for all targets"
        )
    endif()
    add_dependencies(memory_reports ${REPORT_TARGET})

endfunction()

# Generate a summary memory report comparing multiple targets
function(add_memory_summary)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "OUTPUT" "TARGETS")

    if(NOT ARG_TARGETS)
        message(FATAL_ERROR "add_memory_summary: TARGETS argument required")
    endif()

    if(NOT ARG_OUTPUT)
        set(ARG_OUTPUT "${CMAKE_BINARY_DIR}/memory_summary.txt")
    endif()

    find_program(SIZE_TOOL NAMES ${CMAKE_SIZE} arm-none-eabi-size size)
    if(NOT SIZE_TOOL)
        return()
    endif()

    # Build command to process all targets
    set(SUMMARY_CMD
        ${CMAKE_COMMAND} -E echo "==================================================================="
        COMMAND ${CMAKE_COMMAND} -E echo "Memory Summary - All Targets"
        COMMAND ${CMAKE_COMMAND} -E echo "==================================================================="
        COMMAND ${CMAKE_COMMAND} -E echo ""
    )

    foreach(target ${ARG_TARGETS})
        if(TARGET ${target})
            list(APPEND SUMMARY_CMD
                COMMAND ${CMAKE_COMMAND} -E echo "--- ${target} ---"
                COMMAND ${SIZE_TOOL} -B $<TARGET_FILE:${target}>
                COMMAND ${CMAKE_COMMAND} -E echo ""
            )
        endif()
    endforeach()

    add_custom_target(memory_summary
        ${SUMMARY_CMD}
        DEPENDS ${ARG_TARGETS}
        COMMENT "Generating memory summary"
        VERBATIM
    )

endfunction()

# Parse SIZE output and extract text/data/bss sections
function(extract_memory_info TARGET_FILE OUTPUT_VAR)
    find_program(SIZE_TOOL NAMES ${CMAKE_SIZE} arm-none-eabi-size size)
    if(NOT SIZE_TOOL)
        return()
    endif()

    execute_process(
        COMMAND ${SIZE_TOOL} -A ${TARGET_FILE}
        OUTPUT_VARIABLE SIZE_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Parse output for .text, .data, .bss
    string(REGEX MATCH "\\.text[^0-9]+([0-9]+)" _ ${SIZE_OUTPUT})
    set(TEXT_SIZE ${CMAKE_MATCH_1})

    string(REGEX MATCH "\\.data[^0-9]+([0-9]+)" _ ${SIZE_OUTPUT})
    set(DATA_SIZE ${CMAKE_MATCH_1})

    string(REGEX MATCH "\\.bss[^0-9]+([0-9]+)" _ ${SIZE_OUTPUT})
    set(BSS_SIZE ${CMAKE_MATCH_1})

    set(${OUTPUT_VAR} "text=${TEXT_SIZE};data=${DATA_SIZE};bss=${BSS_SIZE}" PARENT_SCOPE)
endfunction()

# Configuration summary printer
function(print_osal_memory_config)
    message(STATUS "")
    message(STATUS "=================================================================")
    message(STATUS "micrOSAL Memory Configuration")
    message(STATUS "=================================================================")
    message(STATUS "Active Backend: ${OSAL_BACKEND}")
    message(STATUS "")

    # Print pool configuration if defined
    get_directory_property(COMPILE_DEFS COMPILE_DEFINITIONS)
    foreach(def ${COMPILE_DEFS})
        if(def MATCHES "OSAL_.*_MAX_.*=([0-9]+)")
            message(STATUS "  ${def}")
        elseif(def MATCHES "OSAL_.*_POOL_SIZE=([0-9]+)")
            message(STATUS "  ${def}")
        endif()
    endforeach()

    message(STATUS "")
    message(STATUS "Use 'make memory_reports' to generate runtime memory analysis")
    message(STATUS "Use 'python3 scripts/memory_footprint.py' for pool sizing")
    message(STATUS "=================================================================")
    message(STATUS "")
endfunction()
