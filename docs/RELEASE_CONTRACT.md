# MicrOSAL Release Contract

This document defines the support boundary for MicrOSAL `0.0.2`.

The release version is declared in all of the following locations, which must
remain synchronized:

- `include/osal/version.hpp` for the public compile-time API
- `CMakeLists.txt` for CMake package metadata
- `Doxyfile` for generated API documentation

## Language and Configuration

- The public API requires C++20.
- Standalone CMake defaults to C++20 and preserves an explicit
  `CMAKE_CXX_STANDARD=23` or `26`. The selected mode must be supported by CMake,
  the compiler, and the target SDK. Standards older than C++20 are rejected.
- A build selects exactly one `OSAL_BACKEND` implementation. Backend dispatch,
  capability checks, and fixed-storage constraints are resolved at compile
  time; unsupported backends are not linked into the selected build.
- MicrOSAL has no virtual dispatch, does not require RTTI, and reports runtime
  failures through `osal::result` rather than exceptions.
- Allocation behavior is backend-dependent. Static-pool backends are designed
  for allocation-free use, while POSIX-family backends may use small
  heap-backed control objects. Applications with a process-wide allocation ban
  must enforce that policy in their compiler and runtime configuration.

## Optional C++ Features

Availability follows feature-test macros from the selected standard library,
not just the compiler version or language flag. No additional backend ABI hooks
are introduced. Zephyr applications must also select a language mode supported
by their Zephyr configuration and toolchain.

| Facility | Availability | Contract |
| --- | --- | --- |
| `result::and_then`, `transform`, `or_else` | C++20 baseline | Status-only callbacks must return `osal::result`; `transform` is a synonym for `and_then`, not a value transformation |
| `result::to_expected`, queue receive adapters, pool allocation adapter, `object_wait_set::wait_expected` | `__cpp_lib_expected >= 202202L` | Return `std::expected<value_type, error_code>`; `to_expected` has `void` value type |
| Standard expected `and_then`, `or_else`, `transform` | `__cpp_lib_expected >= 202211L` | Standard monadic operations on the returned expected object |
| `detail::to_underlying` | `__cpp_lib_to_underlying >= 202102L` | Uses the standard utility when available, otherwise an underlying-type cast |
| Native bounded-container storage | `__cpp_lib_inplace_vector >= 202406L` | Uses `std::inplace_vector` behind a checked wrapper; otherwise uses inline `std::array` storage |
| `OSAL_ASSUME` | Supported `assume` attribute or compiler builtin | An internal invariant hint, which may also be active in C++20 |
| `OSAL_UNREACHABLE` | `__cpp_lib_unreachable >= 202202L` or compiler builtin | Only valid for genuinely unreachable control flow; builtin fallbacks may also be active in C++20 |

`result::and_then` calls its nullary callback only on success and preserves the
original error otherwise. `or_else` passes an `error_code` to its handler only on
failure. These status helpers are conditionally `noexcept` according to the
callback; they do not catch callback exceptions. Use `to_expected()` when a
callback returns a value or another expected object:

```cpp
#include <osal/queue.hpp>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
std::expected<int, osal::error_code> receive_after(osal::result status,
                                                 osal::queue<int, 8>& queue) noexcept
{
    return status.to_expected().and_then([&queue]() noexcept {
        return queue.try_receive_expected();
    });
}
#endif
```

Check an expected object with `has_value()` or its boolean conversion before
dereferencing it; call `error()` only on failure. `value()` on an error throws
`std::bad_expected_access` in exception-enabled applications and is not a
recoverable error path in an exception-disabled build. Disabling exceptions
does not change standard-library failure semantics.

The adapter-specific contracts are:

- `queue::receive_expected()` waits indefinitely; `try_receive_expected()` does
  not block. Both preserve backend errors, including `not_initialized`, rather
  than replacing all failures with `would_block`. Their message type must have
  non-throwing default and move construction in addition to `queue_element`.
  The existing output-parameter receive APIs keep their original requirements.
- `memory_pool::allocate_expected()` returns `not_initialized` for an invalid
  pool and `out_of_resources` when a valid pool returns no block. The backend's
  pointer-returning allocation ABI cannot distinguish more specific failures.
- `object_wait_set::wait_expected<MaxReady>()` requires `MaxReady > 0`. It uses
  capacity-sized result storage directly and has the timeout, truncation, and
  probe side effects of `wait()`. In particular, all registered probes are
  examined even when fewer identifiers fit; clearing probes can consume state
  for identifiers omitted by truncation.
- The bounded wrapper supports trivial, non-throwing snapshot values. `full()`,
  boolean `push_back()`, and boolean `resize()` have the same contract in both
  implementations: exceeding capacity returns `false` without changing state.
  Native insertion uses `try_push_back()`, including library revisions that
  return either a pointer or an optional reference. Both implementations
  reserve storage for the full capacity; neither allocates from the heap. The
  fallback initializes all slots, while native observer snapshots can avoid
  constructing unused slots. The value-wait adapter activates its full output
  capacity before probing. No stack-size, code-size, or latency improvement is
  guaranteed without measurements on the selected target.
- Optimizer hints do not validate input. Null caller handles and invalid public
  enum values must retain their documented error paths; assumptions belong only
  after validation or on proven internal invariants.

The `osal::result` representation, error codes, and C/backend ABI are unchanged.
The optional methods are header-level additions; status callbacks returning
other types are deliberately rejected. Do not depend on the bounded wrapper's
binary layout or mix language modes/library feature configurations across
translation units that use these C++ definitions. Rebuild MicrOSAL and its C++
consumers together when changing modes. A passing C++20 build does not validate
feature-gated C++23 or native C++26 code.

Doxygen's preprocessing profile includes optional expected declarations in the
API reference; this does not enable them in a C++20 build.

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
