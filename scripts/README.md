# micrOSAL Scripts

This directory contains tools for analyzing and reporting micrOSAL memory usage and configuration.

## Memory Footprint Calculator

`memory_footprint.py` analyzes pool configurations and calculates RAM/FLASH usage per backend.

### Usage

#### Analyze a single backend:
```bash
./scripts/memory_footprint.py --backend FREERTOS
```

#### Analyze all backends:
```bash
./scripts/memory_footprint.py --all-backends
```

#### Calculate queue memory usage:
```bash
# queue<uint32_t, 32>
./scripts/memory_footprint.py --queue 4 32
```

#### Calculate ring buffer memory usage:
```bash
# ring_buffer<uint16_t, 64>
./scripts/memory_footprint.py --ring 2 64
```

#### Use custom pool configuration:
```bash
./scripts/memory_footprint.py --backend FREERTOS --config my_config.h
```

### Example Output

```
======================================================================
FREERTOS Backend Memory Footprint
======================================================================

Pool Type                                Count Per Item      Total
----------------------------------------------------------------------
MAX_MUTEXES                                 16        80B       1280B
MAX_SEMAPHORES                              16        80B       1280B
MAX_TIMERS                                   8        88B        704B
MAX_EVENT_GROUPS                             8        64B        512B
MAX_STREAM_BUFFERS                           8        64B        512B
MAX_MESSAGE_BUFFERS                          8        64B        512B
MAX_THREADS                                  8        96B        768B
----------------------------------------------------------------------
Backend Pools Total                                           6568B

Per-Object Overhead (handle + valid)                            12B
(paid per instantiated primitive)

TOTAL RAM (static pools)                                      6568B
                                                              6.41 KiB
```

### Configuration Override

Create a header file with custom pool sizes:

```c
// my_config.h
#define OSAL_FREERTOS_MAX_MUTEXES 32
#define OSAL_FREERTOS_MAX_SEMAPHORES 32
#define OSAL_FREERTOS_MAX_TIMERS 16
```

Then analyze:
```bash
./scripts/memory_footprint.py --backend FREERTOS --config my_config.h
```

## Build-Time Memory Reporting

CMake integration for extracting memory usage from build artifacts.

### Usage in CMakeLists.txt

```cmake
# Include the memory reporting module
include(cmake/MemoryReport.cmake)

# Add memory report for an executable
add_executable(my_app main.cpp)
add_memory_report(TARGET my_app)

# Generate reports
# make my_app_memory_report    # Single target report
# make memory_reports          # All target reports
```

### Example Output

```
===================================================================
Memory Report for: my_app
===================================================================

my_app  :
section           size      addr
.text            24576   0x8000
.rodata           4096   0xe000
.data             1024   0x20000
.bss              2048   0x20400
Total            31744

-------------------------------------------------------------------
Berkeley format:
   text    data     bss     dec     hex filename
  28672    1024    2048   31744    7c00 my_app
```

### Symbol Analysis

For detailed symbol-level analysis:

```bash
make my_app_symbols
```

Output shows the 20 largest symbols by size.

### Configuration Summary

The build system automatically prints pool configuration at configure time:

```cmake
print_osal_memory_config()
```

## Integration with Build System

The main CMakeLists.txt can include automatic memory reporting:

```cmake
if(CMAKE_BUILD_TYPE MATCHES "Release|MinSizeRel")
    # Add memory reports for release builds
    foreach(target ${OSAL_TARGETS})
        add_memory_report(TARGET ${target})
    endforeach()
endif()
```

## Requirements

- **Python 3.6+** for memory_footprint.py
- **CMake 3.15+** for memory reporting integration
- **binutils** (`size`, `nm`) for build-time analysis
