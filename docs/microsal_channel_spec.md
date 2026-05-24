# micrOSAL Bus Layer — Design Brief and Current Status

This file started as the implementation brief for the bus/signal layer. It is
kept as a status note, not as the normative API reference.

## Current Implementation Snapshot

Implemented today:

- `osal::osal_bus<T, Capacity, BackendTag>`
- `osal::osal_signal<T, MaxSubscribers, PerSubCapacity, BackendTag>`
- `osal::osal_signal_premium<T, ...>` with portable observer storage,
  copy-based `publish_zero_copy()`, and a stub `route_to()`
- delegated `bus_backend_*` tags for all non-Zephyr OSAL backends
- `bus_backend_mock` for hosted premium tests
- hosted test coverage in `tests/bus/`

Not implemented yet:

- a working Zephyr Zbus runtime backend
- native zero-copy publish behavior
- topic registry / cross-topic routing
- backend-specific optimized bus runtimes beyond delegated generic behavior

## Design Goals That Still Apply

- C++20 or better
- bounded memory and bounded latency
- no dynamic allocation in the core bus/signal objects
- traits-based, compile-time backend selection
- compile-time capability detection via traits and concepts
- no hidden global state

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

- observers are stored in a fixed-size local array
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
- `include/microsal/bus/*.hpp`
- `docs/diagrams/bus_*.puml`

## Remaining Work

1. Replace the Zephyr skeleton with a real Zbus integration.
2. Decide whether `native_*` traits should remain optimistic for the Zephyr tag
   before that runtime implementation exists.
3. Add native routing if a topic registry or RTOS-native mechanism becomes part
   of the supported contract.
- All file skeletons
- Generic micrOSAL signal backend
- Mock premium backend
- Tests, examples, docs, and PlantUML diagrams
- CI/CD enhancements

Focus on clarity, determinism, and minimal runtime overhead.
