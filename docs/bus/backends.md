# Bus + Signal Backend Reference

## Runtime Status Summary

| Backend tag | Capability traits | Current runtime status |
| --- | --- | --- |
| `bus_backend_generic` | All `native_*` and `zero_copy` flags are `false` | Fully implemented; canonical bus/signal behavior |
| Delegated `bus_backend_*` tags | Same flags as `bus_backend_generic` | Fully implemented by delegating to the generic backend |
| `bus_backend_mock` | All premium flags are `true` | Implemented for hosted tests; uses the generic bus/signal path plus the premium wrapper's local observer table |
| `bus_backend_zephyr` | All premium flags are `true` | Compile-time Zbus placeholder; methods are still TODO stubs and are not a usable runtime implementation yet |

## `bus_backend_generic`

**File:** `include/microsal/bus/detail/osal_signal_backend_generic.hpp`

`bus_backend_generic` is the authoritative implementation for the bus layer.
It uses the MicrOSAL queue C ABI for `osal_bus` and `osal_signal` data flow,
plus `osal::mutex` for subscriber-slot management.

This backend provides:

- fixed-capacity `osal::osal_bus<T, Capacity>`
- LCD `osal::osal_signal<T, MaxSubscribers, PerSubCapacity>`
- the baseline behavior inherited by all delegated backends

## Delegated Backends

**File:** `include/microsal/bus/detail/osal_signal_backend_delegated.hpp`

All non-Zephyr, non-mock OSAL backends currently reuse the generic runtime
implementation through `OSAL_BUS_DELEGATE_BACKEND_()`. Each tag is distinct at
compile time, but today they all behave like `bus_backend_generic`.

| Backend tag | Selected by |
| --- | --- |
| `bus_backend_freertos` | `OSAL_BACKEND_FREERTOS` |
| `bus_backend_posix` | `OSAL_BACKEND_POSIX` |
| `bus_backend_linux` | `OSAL_BACKEND_LINUX` |
| `bus_backend_bare_metal` | `OSAL_BACKEND_BAREMETAL` |
| `bus_backend_threadx` | `OSAL_BACKEND_THREADX` |
| `bus_backend_px5` | `OSAL_BACKEND_PX5` |
| `bus_backend_vxworks` | `OSAL_BACKEND_VXWORKS` |
| `bus_backend_nuttx` | `OSAL_BACKEND_NUTTX` |
| `bus_backend_micrium` | `OSAL_BACKEND_MICRIUM` |
| `bus_backend_chibios` | `OSAL_BACKEND_CHIBIOS` |
| `bus_backend_embos` | `OSAL_BACKEND_EMBOS` |
| `bus_backend_qnx` | `OSAL_BACKEND_QNX` |
| `bus_backend_rtems` | `OSAL_BACKEND_RTEMS` |
| `bus_backend_integrity` | `OSAL_BACKEND_INTEGRITY` |
| `bus_backend_cmsis_rtos` | `OSAL_BACKEND_CMSIS_RTOS` |
| `bus_backend_cmsis_rtos2` | `OSAL_BACKEND_CMSIS_RTOS2` |

## `bus_backend_mock`

**File:** `include/microsal/bus/detail/osal_signal_backend_mock.hpp`

`bus_backend_mock` exists to exercise `osal::osal_signal_premium` in hosted
tests. It reuses the generic bus/signal implementation, while the premium
wrapper exposes observer support and copy-based `publish_zero_copy()` behavior.

## `bus_backend_zephyr`

**File:** `include/microsal/bus/detail/osal_signal_backend_zephyr.hpp`

The Zephyr specialisations are currently a Zbus integration skeleton. The
capability traits still advertise native pub/sub, observers, routing, and
zero-copy as the intended end state, but the runtime methods remain TODO stubs.

Until that wiring lands, prefer `osal::bus_backend_generic` explicitly for a
working bus/signal implementation on Zephyr.

## Backend Selection Macro

`MICROSAL_DEFAULT_BACKEND_TAG` is auto-defined in `include/microsal/microsal_config.hpp`
from the active `OSAL_BACKEND_*` macro:

| OSAL backend macro | Default bus tag |
| --- | --- |
| `OSAL_BACKEND_ZEPHYR` | `bus_backend_zephyr` |
| `OSAL_BACKEND_FREERTOS` | `bus_backend_freertos` |
| `OSAL_BACKEND_THREADX` | `bus_backend_threadx` |
| `OSAL_BACKEND_PX5` | `bus_backend_px5` |
| `OSAL_BACKEND_POSIX` | `bus_backend_posix` |
| `OSAL_BACKEND_LINUX` | `bus_backend_linux` |
| `OSAL_BACKEND_BAREMETAL` | `bus_backend_bare_metal` |
| `OSAL_BACKEND_VXWORKS` | `bus_backend_vxworks` |
| `OSAL_BACKEND_NUTTX` | `bus_backend_nuttx` |
| `OSAL_BACKEND_MICRIUM` | `bus_backend_micrium` |
| `OSAL_BACKEND_CHIBIOS` | `bus_backend_chibios` |
| `OSAL_BACKEND_EMBOS` | `bus_backend_embos` |
| `OSAL_BACKEND_QNX` | `bus_backend_qnx` |
| `OSAL_BACKEND_RTEMS` | `bus_backend_rtems` |
| `OSAL_BACKEND_INTEGRITY` | `bus_backend_integrity` |
| `OSAL_BACKEND_CMSIS_RTOS` | `bus_backend_cmsis_rtos` |
| `OSAL_BACKEND_CMSIS_RTOS2` | `bus_backend_cmsis_rtos2` |
| none of the above | `bus_backend_generic` |

Override before including any bus header:

```cpp
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_generic
```

## Adding a New Bus Backend

1. Add the tag struct in `include/microsal/microsal_config.hpp`.
2. Add or update the `osal_signal_capabilities<>` specialisation in
   `include/microsal/bus/detail/osal_signal_traits.hpp`.
3. Either:
   - add a delegated specialisation via `OSAL_BUS_DELEGATE_BACKEND_()` when the
     generic runtime path is sufficient, or
   - add a new native backend header under `include/microsal/bus/detail/` and
     include it from `include/microsal/bus/osal_bus.hpp`.
4. Extend the `bus_backend_tag` concept in `osal_signal_traits.hpp`.
5. Update the `MICROSAL_DEFAULT_BACKEND_TAG` selector if the new backend should
   become the default for an `OSAL_BACKEND_*` macro.
