# MicrOSAL Release Contract

This document defines the support boundary for MicrOSAL `0.1.0`.

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

The `osal::result` representation and existing error-code numbers are unchanged.
New error codes are appended for robust ownership and cancellation (15--17).
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

## Correctness Hardening and Migration

- Queues retain value-copy semantics: a successful send owns a copy of the
  payload, not the caller's pointer. Ports must not round native word sizes past
  caller storage or mutate a queue when reporting unsupported peek.
- The hardened Linux/POSIX/QNX semaphore implementations enforce the configured
  maximum, including binary limits. The shared hosted RTEMS/INTEGRITY path uses
  the same implementation. A give at the maximum fails; it does not add another
  permit. FreeRTOS uses native bounded semaphores.
- Finite timeout arguments at the backend ABI are OSAL ticks. Native absolute
  timeout APIs must receive a deadline using the clock required by that API.
- Changing a timer period preserves one-shot versus periodic mode. Callback,
  thread, and work-item storage must not be recycled while native users remain.
- Production bare-metal thread creation returns `not_supported`; the former
  shared-stack jump implementation was unsafe. `thread::is_supported` and
  `thread::require_support()` expose this boundary. The hosted test-only
  separate-stack scheduler is unchanged. Production clocks, tick-driven timers,
  and single-context/ISR-safe primitives remain available.
- Delayed-work cancellation and rescheduling propagate timer-stop failures and
  retain the arm on failure. An expiry from a previous arm cannot consume a
  replacement before its deadline. Dispatch errors are published before idle,
  so a completed `flush()` observes the failure.
- Pub/sub subscription fails when backing resources did not initialize.
  Unsubscribe returns `false` while a blocking receive holds that slot;
  callers must let receivers finish before releasing the subscription.
- `has_native_memory_pool` describes the implemented allocator, not merely an
  API present in the kernel SDK. ThreadX, PX5, VxWorks, Micrium, ChibiOS, and
  embOS currently use shared emulation. Zephyr's system-clock query returns
  uptime and does not advertise a wall clock.

**API/ABI impact:** existing primitive backend function signatures and opaque
handles are unchanged. The C `osal_delayable_work_handle` and C++
`delayable_work` layouts grow to hold generation/deadline state; pub/sub topic
layouts also grow to track active receivers. Checked lifecycle state and
diagnostic counters change C++ wrapper layouts. Rebuild the library and all
consumers together; do not mix old headers with the new library. Version
`0.1.0` marks this rebuild boundary. The new mutex policy/consistency entrypoints
extend the primitive API; no mailbox or higher-level-helper backend hooks were
added. The C mutex policy descriptor is separate from the unchanged legacy C
mutex configuration.

The installed package discovers its pthread dependency and provides
`MicrOSAL::OSAL`, matching the build-tree alias. The previous installed
`MicrOSAL::microsal` spelling is retained for compatibility.
Package version compatibility is same-minor, not same-major: a `0.0` request
must not accept the changed `0.1` ABI.

## Portable Extensions

### Ownership and routing

The original queue-based pub/sub APIs retain value-copy semantics, including
the legacy raw-pointer `publish_zero_copy` compatibility overload. Genuine
zero-copy is a separate bounded owned-loan transport: a producer exclusively
owns writable pool storage until publication; subscribers receive immutable
loans referring to that same payload. The pool and transport must outlive every
outstanding loan. A slow subscriber can exhaust the pool; exhaustion and
delivery drops are observable, not reasons to overwrite borrowed data.

Typed topic registries provide bounded, allocation-free routing. Registered
topics are borrowed, not owned. Unregister before destroying a topic, and check
the unregister result when a dispatch is in flight. Native routing/zero-copy
traits describe actual backend-native implementations, not the new portable
compositions or mock behavior. See [premium pub/sub](bus/premium_pubsub.md).

### Cancellation and multi-object wakeups

Caller-owned stop sources and borrowed tokens provide cooperative cancellation,
not forced task deletion. Sources must outlive all tokens, registered callbacks
and concurrent operations. Registrations are bounded; registration errors must
be checked. Cancellation callbacks execute synchronously in requesting task
context and may not block on the requester or destroy an in-flight registration.
Cancellation is currently a C++ API. Check source `initialization_status()` and
use `request_stop(bool& first_request)` to distinguish repeated requests from
synchronization failures. Callback registrations expose checked `close()`;
retain the callback, source and argument when close fails. Metadata uses a
native mutex, requesting priority inheritance where supported, and completion
uses condition-variable broadcast rather than task-context spin acquisition.
Source status retains synchronization failures; persistent backend faults can
prevent successful destruction or waking an indefinite wait.

`wait_signal` and the new `object_wait_set` overload use a native semaphore
wakeup. This is **explicit opt-in**: every producer must mutate observed state
and then notify the supplied signal. Unmodified C/backend producers do not
automatically subscribe. One concurrent consumer is allowed per signal.
Readiness predicates must be nonblocking and noexcept. Existing polling waits
remain available. Bare-metal semaphore waits spin, so `wait_signal::is_supported`
is false on that backend, including its hosted test scheduler; use polling there.
Thread-only cancellation and independently gated ISR
notification must not be confused; see [design](design.md).

### Checked lifecycle and mutex policies

Mutex, semaphore, queue, timer, work queue and thread wrappers retain
`initialization_status()`. Existing convenience constructors and void/bool
operations remain; checked operations expose native errors. Explicit `close()`
is idempotent and preserves a valid handle on failure. Work shutdown and thread
join/detach are cooperative lifecycle boundaries; destruction does not force
thread termination or guarantee successful shutdown. Close/check results before
destroying wrappers when backend lifecycle restrictions can apply.

`mutex::checked_lock_guard` owns a lock only after successful acquisition or
`owner_died`. A robust mutex returning `owner_died` is **already acquired**:
repair state and call `consistent()` before unlocking; otherwise subsequent
locks return `not_recoverable`. Legacy boolean and unchecked guard APIs must not
be used for robust recovery.

Explicit mutex protocol/robustness requests are never silently substituted:

| Backend | Explicit policy selection |
| --- | --- |
| Linux / POSIX | None, inheritance, ceiling through native pthread attributes; robust recovery on Linux hosts |
| FreeRTOS / Zephyr | Inheritance, using their inheriting native mutexes |
| Other ports | Native default; explicit unsupported policies fail with `not_supported` |

`supports_protocol()` and `supports_robustness` describe implemented API paths,
not a guarantee that a host accepts a policy combination. Ceiling priorities
are native scheduler priorities; creation/acquisition may fail due to invalid
priorities, host limitations or privileges. Existing priority-inheritance
capabilities describe kernel availability, not that every default host mutex
was configured for inheritance.

Changing a timer period preserves inactive state and one-shot/periodic mode.
An active timer begins a new interval using the new period; changing an inactive
timer requires a later `start()` or `reset()` to arm it. This intentionally
corrects ports that previously started a stopped timer during `set_period()`.
Unsupported period changes fail without modifying configuration. A callback
already in flight may complete; changing a period is not a callback fence.
`timer::can_reclaim_after_start` exposes the compile-time reclamation boundary:
false means a timer must not be started if later reclamation is required.
True still requires backend-specific context/quiescence conditions and checking
the `close()` result.
Zephyr UP timer reclamation runs in task context with interrupts masked; ISR
and SMP reclamation return `not_supported` and retain ownership because an
already-dispatched callback cannot safely be fenced by `k_timer_stop` alone.

### Operational diagnostics

`diagnostic_snapshot` / `diagnostic_counters` provide allocation-free 32-bit
counters and independently sampled gauges. Global resource snapshots cover
owning C/C++ frontend handles and the instrumented shared emulated memory/work
control pools. Instance queue/work statistics record task-context operations,
failures, drops and observed backlog; loan transports expose their own pool
and delivery statistics. Capacity zero means unknown/unbounded, not an empty
kernel pool. These are instrumentation scopes, not an inventory of every
native SDK allocation.

Snapshots are approximate under concurrency; counters wrap modulo 2^32. They
are not transactionally consistent, resettable performance traces or hardware
latency guarantees. Counter updates with retry loops are thread-context only:
ISR operation paths intentionally do not record these statistics. Shared pool
high-water marks measure occupied control slots. Queue/work backlog is an
observation, not a guarantee that a worker has remained idle since the snapshot.
`osal_c_diagnostics_read` exposes the same global resource snapshots to C.

### Native-port limitations

- Zephyr, ThreadX, PX5, Micrium, and embOS reject thread detach with
  `not_supported`, keeping the live handle available for join.
- Zephyr rejects work-queue flush/destruction from ISR context or the queue's
  own worker with `not_supported`; perform lifecycle operations from an
  independent task.
- ThreadX, PX5, Micrium, and embOS queue peek returns `not_supported` without
  removing a message. ThreadX/PX5 queues reserve 256 native words per slot and
  accept at most 16 native words per item; configurations exceeding either
  bound fail explicitly.
- Micrium/embOS queue waits and ChibiOS event waits poll once per native tick.
  Their byte-copy critical sections must be included in target interrupt
  latency budgeting; these are not hardware latency guarantees.
- CMSIS-RTOS v1 queue count/free snapshots include in-flight send reservations.
- ChibiOS priority changes support only the current thread; changing another
  thread's priority returns `not_supported`.
- ThreadX, PX5, Micrium, ChibiOS, embOS, VxWorks, NuttX, CMSIS-RTOS v1 and v2
  timer destruction returns `not_supported` after **any start attempt**, even
  after stopping or callback completion. The handle and pool slot are retained
  permanently because callback quiescence has not been verified against those
  SDKs. Keep the wrapper and callback context alive; use application-lifetime
  timers or avoid starting them when reclamation is required. This is a safety
  restriction, not a reusable native lifecycle implementation.
- CMSIS-RTOS v1 join returns `not_supported` without terminating the thread or
  surrendering its handle. CMSIS-RTOS v2 lifecycle failures retain ownership.
- Micrium period changes are supported only before the first timer start
  attempt. ChibiOS, embOS, VxWorks and NuttX support stopped period changes,
  reject active changes, and reject restart after the first start attempt.
  CMSIS-RTOS v1 rejects active period changes but stores stopped changes for a
  subsequent start. ThreadX, PX5, CMSIS-RTOS v2 and QNX support active changes
  that begin a fresh interval. Unsupported changes preserve configuration.
- FreeRTOS timer destruction and period changes require a running scheduler
  and an independent task, not the timer daemon. Configure
  `INCLUDE_xTimerPendFunctionCall=1` and `INCLUDE_xTaskGetSchedulerState=1`;
  missing settings produce an explicit compile-time error.
- QNX timers use an owned watcher whose exit is joined before reclaiming timer
  storage. Periodic callbacks are serialized and fixed-delay: the next period
  begins after the previous callback completes. Code relying on overlapping
  callbacks or fixed-rate catch-up must be adapted.

Native SDK-dependent paths require SDK compilation and target verification.
Passing the portable queue contract tests on Linux is not evidence that each
native adapter was built or that SMP/ISR/callback quiescence is certified.
Configured semaphore-maximum enforcement remains a native-port conformance
requirement. These native adapters now enforce the configured maximum through
native ceiling operations or bounded bookkeeping; the SDK-dependent changes
are not certified by hosted tests. PX5 bookkeeping retries are bounded and may
return `would_block` under contention without giving a semaphore token.
Semaphore control/metadata defaults are 16 for `OSAL_TX_MAX_SEMS`,
`OSAL_PX5_MAX_SEMS`, `OSAL_UC_MAX_SEMS`, `OSAL_CH_MAX_SEMS`,
`OSAL_EMBOS_MAX_SEMS`, `OSAL_CMSIS1_MAX_SEMS`, and `OSAL_CMSIS2_MAX_SEMS`;
`OSAL_VX_MAX_SEMS` and `OSAL_NX_MAX_SEMS` default to 64. QNX uses host-managed
allocations rather than a fixed metadata limit. Exhaustion is reported instead
of discarding the ceiling contract.

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
