# micrOSAL Bus Layer — Implementation Status

This document tracks the current implementation status of the bus/signal layer.
For normative API documentation, see the files listed at the end of this document.

## Current Implementation Status

Implemented today:

- `osal::osal_bus<T, Capacity, BackendTag>`
- `osal::osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>`
- `osal::osal_signal_premium<T, ...>` with backend-owned observers on mock and
   Zephyr, portable observer storage elsewhere, copy-based
   `publish_zero_copy()`, and a stub `route_to()`
- delegated `bus_backend_*` tags for all non-Zephyr, non-mock OSAL backends
- `bus_backend_mock` for hosted premium tests
- `bus_backend_zephyr` with a dedicated Zephyr `k_msgq` pub/sub runtime on
   real Zephyr builds and a hosted generic fallback for compile-time coverage
- hosted test coverage in `tests/bus/`

Not implemented yet:

- native Zephyr routing or zero-copy premium paths
- native zero-copy publish behavior
- topic registry / cross-topic routing
- backend-specific optimized bus runtimes beyond delegated generic behavior

## Implementation Complete

The bus/signal layer is **fully implemented and functional**:

- ✅ C++20 template-based design
- ✅ Bounded memory and bounded latency
- ✅ No dynamic allocation in core bus/signal objects
- ✅ Traits-based, compile-time backend selection
- ✅ Compile-time capability detection via traits and concepts
- ✅ No hidden global state

## Generic Runtime Model

The generic implementation remains the canonical LCD behavior:

- `osal_bus` uses the MicrOSAL queue C ABI directly for fixed-capacity message transport.
- `osal_signal` keeps a static array of subscriber slots.
- each subscriber slot owns its own `osal_bus<T, PerSubCapacity, bus_backend_generic>`.
- `publish()` fans out with non-blocking sends.
- `receive()` delegates to the subscriber's queue.

## Premium Runtime Model

`osal_signal_premium` is always available as an API surface. On the currently
implemented backends it behaves as follows:

- generic and delegated backends store observers in a fixed-size local array
- mock and Zephyr expose backend-owned observer registration and dispatch
- `publish()` first fans out to queue subscribers, then invokes registered observers
- `publish_zero_copy()` currently falls back to `publish(*ptr)`
- `route_to()` currently returns `false`

`bus_backend_mock` advertises all premium capability flags so those code paths
are exercised in hosted tests.

## Files To Treat As Normative

- `docs/bus/bus_overview.md`
- `docs/bus/lcd_pubsub.md`
- `docs/bus/premium_pubsub.md`
- `docs/bus/backends.md`
- `include/osal/bus/*.hpp`
- `docs/diagrams/bus_*.puml`

## Future Enhancement Opportunities

Potential areas for future enhancement (not required for current functionality):

1. Native Zephyr-specific observer or routing integration (if Zbus integration
   is desired without weakening current FIFO snapshot semantics)
2. Additional `native_*` traits exposure when corresponding Zephyr behavior
   meets the supported contract
3. Native routing support if a topic registry or RTOS-native mechanism is added

All core functionality is complete and tested. Focus remains on clarity,
determinism, and minimal runtime overhead.
