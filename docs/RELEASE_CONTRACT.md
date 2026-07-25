# MicrOSAL Release Contract

This document defines the support boundary for MicrOSAL `0.0.2`.

The release version is declared in all of the following locations, which must
remain synchronized:

- `include/osal/version.hpp` for the public compile-time API
- `CMakeLists.txt` for CMake package metadata
- `Doxyfile` for generated API documentation

## Language and Configuration

- The public API requires C++20.
- A build selects exactly one `OSAL_BACKEND` implementation. Backend dispatch,
  capability checks, and fixed-storage constraints are resolved at compile
  time; unsupported backends are not linked into the selected build.
- MicrOSAL has no virtual dispatch, does not require RTTI, and reports runtime
  failures through `osal::result` rather than exceptions.
- Allocation behavior is backend-dependent. Static-pool backends are designed
  for allocation-free use, while POSIX-family backends may use small
  heap-backed control objects. Applications with a process-wide allocation ban
  must enforce that policy in their compiler and runtime configuration.

## Capability, Thread, and ISR Use

- Native-only and optional facilities are exposed through capability traits and
  `is_supported` helpers. Applications must branch on those capabilities or
  use the matching `require_support()` helper when a build-time requirement is
  intended.
- Public primitives are not generally safe for unsynchronized concurrent
  mutation. Applications must establish object ownership or use MicrOSAL
  synchronization primitives around shared state.
- ISR use is limited to the explicitly documented ISR-safe APIs and to
  backends that advertise the corresponding capability. Keep ISR work bounded
  and defer blocking or policy work to task context.
- Production bare-metal integrations must drive
  `osal_baremetal_tick()` or `osal_baremetal_tick_with_timers()` from the
  target timer interrupt path. The hosted self-tick helper exists only for the
  CTest suite.

## Validation Boundary

| Configuration | Evidence | Limit |
| --- | --- | --- |
| Linux, POSIX, RTEMS, INTEGRITY, and hosted bare-metal | CMake/doctest suites run by CTest in CI | Hosted execution does not prove target timing or ABI |
| FreeRTOS v11 | POSIX simulation doctest suite in CI | It is not a real interrupt or board validation |
| NuttX main | `sim/nsh` built-in test application in CI | It does not exercise a deployed board configuration |
| Zephyr v3.7 and v4.4 | `native_sim` and nRF52840 Renode ztest runs in CI | Simulation does not replace physical-device validation |
| ThreadX, PX5, VxWorks, Micrium, ChibiOS, embOS, CMSIS-RTOS v1/v2, and QNX | Documented integration contracts | Required SDKs, BSPs, simulators, or toolchains are not provisioned in GitHub-hosted CI |

The detailed test inventory, known gaps, and backend-specific commands are in
[`TestCoverage.md`](TestCoverage.md). Backend integration requirements are in
[`backend_integration.md`](backend_integration.md).

## Release Verification

For a hosted Linux release check, run:

```bash
CLANG_FORMAT=clang-format-18 ./scripts/format.sh --check
cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
rm -rf docs/doxygen/html docs/doxygen/latex docs/doxygen/xml
doxygen Doxyfile
```

Run the backend-specific suites needed by the target release, then complete
board-level functional, timing, memory, and fault-injection validation with
the selected compiler, RTOS configuration, and hardware. CI and simulator
results are regression evidence, not a claim of deployment readiness.
