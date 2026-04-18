# Channel Backend Reference

## Available Backends

### `bus_backend_generic` (LCD fallback)

**File:** `include/microsal/bus/detail/osal_signal_backend_generic.hpp`

Uses `osal::queue` (via the C ABI) and `osal::mutex` from the active micrOSAL backend.
Works on every supported RTOS and bare-metal target.  This is the underlying
implementation shared by all delegated backends.

| Capability     | Supported |
|----------------|-----------|
| native_pubsub  | ✗         |
| observers      | ✗         |
| zero_copy      | ✗         |
| routing        | ✗         |

### `bus_backend_zephyr` (premium)

**File:** `include/microsal/bus/detail/osal_signal_backend_zephyr.hpp`

Maps `osal_signal<T>` to a Zephyr Zbus channel.  Currently a **skeleton** —
all methods contain TODO stubs pending full Zbus wiring.

| Capability     | Supported |
|----------------|-----------|
| native_pubsub  | ✓         |
| observers      | ✓         |
| zero_copy      | ✓         |
| routing        | ✓         |

**Integration steps (TODO):**
1. Add `ZBUS_CHAN_DEFINE` at file scope for each topic type.
2. Implement `publish()` via `zbus_chan_pub()`.
3. Implement `subscribe()` via `zbus_obs_attach_to_chan()`.
4. Implement `receive()` via `zbus_sub_wait()`.
5. Connect observer callbacks to Zbus listener threads.

### `bus_backend_mock` (test-only)

**File:** `include/microsal/bus/detail/osal_signal_backend_mock.hpp`

Identical to `bus_backend_generic` in behaviour but reports all premium
capability flags as `true`.  Use in unit tests to exercise `osal_signal_premium`
premium code paths without requiring Zephyr.

### Delegated Backends (16 backends)

**File:** `include/microsal/bus/detail/osal_signal_backend_delegated.hpp`

All non-Zephyr/non-mock backends share the same micrOSAL C ABI for queue and
mutex operations.  Each backend has its own distinct tag type but delegates
entirely to `bus_backend_generic` for the channel implementation.  This
design allows future per-backend optimisations without changing user code.

| Backend Tag                  | OSAL Macro              | Notes                        |
|------------------------------|-------------------------|------------------------------|
| `bus_backend_freertos`   | `OSAL_BACKEND_FREERTOS` | FreeRTOS native queues       |
| `bus_backend_posix`      | `OSAL_BACKEND_POSIX`    | POSIX pthreads               |
| `bus_backend_linux`      | `OSAL_BACKEND_LINUX`    | Linux (POSIX + extensions)   |
| `bus_backend_bare_metal` | `OSAL_BACKEND_BAREMETAL` | Spin-based, ISR-safe         |
| `bus_backend_threadx`    | `OSAL_BACKEND_THREADX`  | ThreadX / Azure RTOS         |
| `bus_backend_px5`        | `OSAL_BACKEND_PX5`      | PX5 (ThreadX-compatible)     |
| `bus_backend_vxworks`    | `OSAL_BACKEND_VXWORKS`  | Wind River VxWorks           |
| `bus_backend_nuttx`      | `OSAL_BACKEND_NUTTX`    | Apache NuttX (POSIX)         |
| `bus_backend_micrium`    | `OSAL_BACKEND_MICRIUM`  | Micrium µC/OS-III            |
| `bus_backend_chibios`    | `OSAL_BACKEND_CHIBIOS`  | ChibiOS/RT                   |
| `bus_backend_embos`      | `OSAL_BACKEND_EMBOS`    | SEGGER embOS                 |
| `bus_backend_qnx`        | `OSAL_BACKEND_QNX`      | QNX Neutrino                 |
| `bus_backend_rtems`      | `OSAL_BACKEND_RTEMS`    | RTEMS (POSIX)                |
| `bus_backend_integrity`  | `OSAL_BACKEND_INTEGRITY` | Green Hills INTEGRITY        |
| `bus_backend_cmsis_rtos` | `OSAL_BACKEND_CMSIS_RTOS` | ARM CMSIS-RTOS v1          |
| `bus_backend_cmsis_rtos2`| `OSAL_BACKEND_CMSIS_RTOS2` | ARM CMSIS-RTOS2 v2        |

All delegated backends have the following capability profile:

| Capability     | Supported |
|----------------|-----------|
| native_pubsub  | ✗         |
| observers      | ✗ (emulated via `osal_signal_premium`) |
| zero_copy      | ✗ (falls back to copy) |
| routing        | ✗ (stub)  |

## Backend Selection Macro

`MICROSAL_DEFAULT_BACKEND_TAG` is auto-defined in `microsal_config.hpp` based
on the active `OSAL_BACKEND_*` macro:

| OSAL Backend Macro         | Default Channel Tag            |
|----------------------------|--------------------------------|
| `OSAL_BACKEND_ZEPHYR`     | `bus_backend_zephyr`       |
| `OSAL_BACKEND_FREERTOS`   | `bus_backend_freertos`     |
| `OSAL_BACKEND_THREADX`    | `bus_backend_threadx`      |
| `OSAL_BACKEND_PX5`        | `bus_backend_px5`          |
| `OSAL_BACKEND_POSIX`      | `bus_backend_posix`        |
| `OSAL_BACKEND_LINUX`      | `bus_backend_linux`        |
| `OSAL_BACKEND_BAREMETAL`  | `bus_backend_bare_metal`   |
| `OSAL_BACKEND_VXWORKS`    | `bus_backend_vxworks`      |
| `OSAL_BACKEND_NUTTX`      | `bus_backend_nuttx`        |
| `OSAL_BACKEND_MICRIUM`    | `bus_backend_micrium`      |
| `OSAL_BACKEND_CHIBIOS`    | `bus_backend_chibios`      |
| `OSAL_BACKEND_EMBOS`      | `bus_backend_embos`        |
| `OSAL_BACKEND_QNX`        | `bus_backend_qnx`          |
| `OSAL_BACKEND_RTEMS`      | `bus_backend_rtems`        |
| `OSAL_BACKEND_INTEGRITY`  | `bus_backend_integrity`    |
| `OSAL_BACKEND_CMSIS_RTOS` | `bus_backend_cmsis_rtos`   |
| `OSAL_BACKEND_CMSIS_RTOS2`| `bus_backend_cmsis_rtos2`  |
| (none of the above)       | `bus_backend_generic`      |

Override before including any channel header:

```cpp
#define MICROSAL_DEFAULT_BACKEND_TAG ::osal::bus_backend_generic
```

## Adding a New Backend

1. Add a tag struct in `microsal_config.hpp`.
2. Add a `osal_signal_capabilities<>` specialisation in `osal_signal_traits.hpp`.
3. Either:
   - **Delegated:** Add a `OSAL_BUS_DELEGATE_BACKEND_()` invocation in
     `osal_signal_backend_delegated.hpp` (if it uses the common C ABI).
   - **Native:** Create `detail/osal_signal_backend_<name>.hpp` with
     `osal_bus` and `osal_signal` specialisations, and include it from
     `osal_bus.hpp`.
4. Update `bus_backend_tag` concept in `osal_signal_traits.hpp`.
5. Update `MICROSAL_DEFAULT_BACKEND_TAG` selection if needed.
