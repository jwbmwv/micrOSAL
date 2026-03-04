# MicrOSAL — Architecture & Design

## Overview

The Micro Operating System Abstraction Layer (OSAL) provides a thin,
zero-cost C++17 interface to common RTOS primitives across fifteen backends:

| Backend | Target | File |
| --- | --- | --- |
| `FREERTOS` | FreeRTOS ≥ 10.0 (all chip families) | `src/freertos/freertos_backend.cpp` |
| `ZEPHYR` | Zephyr RTOS ≥ 3.0 (tested with 4.3) | `src/zephyr/zephyr_backend.cpp` |
| `THREADX` | Azure RTOS ThreadX | `src/threadx/threadx_backend.cpp` |
| `PX5` | PX5 RTOS (ThreadX superset) | `src/px5/px5_backend.cpp` |
| `POSIX` | Any POSIX:2008 system | `src/posix/posix_backend.cpp` |
| `LINUX` | Linux ≥ 4.11 (extends POSIX) | `src/linux/linux_backend.cpp` |
| `BAREMETAL` | No OS — cooperative, single-core | `src/bare_metal/bare_metal_backend.cpp` |
| `VXWORKS` | VxWorks 7 (Wind River) | `src/vxworks/vxworks_backend.cpp` |
| `NUTTX` | Apache NuttX ≥ 12 | `src/nuttx/nuttx_backend.cpp` |
| `MICRIUM` | Micrium µC/OS-III | `src/micrium/micrium_backend.cpp` |
| `CHIBIOS` | ChibiOS/RT ≥ 21 | `src/chibios/chibios_backend.cpp` |
| `EMBOS` | SEGGER embOS | `src/embos/embos_backend.cpp` |
| `QNX` | QNX Neutrino RTOS | `src/qnx/qnx_backend.cpp` |
| `CMSIS_RTOS` | ARM CMSIS-RTOS v1 | `src/cmsis_rtos/cmsis_rtos_backend.cpp` |
| `CMSIS_RTOS2` | ARM CMSIS-RTOS2 (v2) | `src/cmsis_rtos2/cmsis_rtos2_backend.cpp` |

---

## Design Principles

1. **No virtual functions** — all dispatch is resolved at compile-time via
   template traits and `if constexpr`.

2. **No RTTI** — compile with `-fno-rtti`.

3. **No dynamic allocation in the primitives** — the embedded backends
   (FreeRTOS static mode, Zephyr, ThreadX, PX5, bare-metal) use exclusively
   static pools.  POSIX / Linux allocate with `new` because the OS itself
   manages process memory.  An optional `OSAL_FREERTOS_DYNAMIC_ALLOC` macro
   restores dynamic allocation for FreeRTOS if needed.

4. **`noexcept` everywhere** — no exceptions are thrown or propagated.

5. **Error reporting via `osal::result`** — a small struct wrapping
   `osal::error_code`.  Callers are expected to check the result.

---

## Config / Data Split (FLASH Placement)

On embedded targets, read-only data placed in the `.rodata` section lives in
FLASH and consumes zero RAM.  MicrOSAL exploits this by separating each
primitive's immutable **creation parameters** from its mutable **runtime
state**:

| Component | Lifetime | Section | Location |
| --- | --- | --- | --- |
| `*_config` | Entire program | `.rodata` | FLASH |
| OS handle + `valid_` | Runtime | `.data`/`.bss` | RAM |

### Config structs

Every primitive that takes creation-time parameters has a corresponding
`*_config` aggregate struct:

| Config struct | Primitive | Key fields |
| --- | --- | --- |
| `mutex_config` | `mutex` | `type` |
| `semaphore_config` | `semaphore` | `type`, `initial_count`, `max_count` |
| `timer_config` | `timer` | `callback`, `arg`, `period`, `mode`, `name` |
| `thread_config` | `thread` | `entry`, `arg`, `priority`, `affinity`, `stack`, `stack_bytes`, `name` |
| `work_queue_config` | `work_queue` | `stack`, `stack_bytes`, `depth`, `name` |
| `memory_pool_config` | `memory_pool` | `buffer`, `buf_bytes`, `block_size`, `block_count`, `name` |

All config structs use **aggregate initialisation** with default member
initialisers — they are `constexpr`-constructible (or at least
`const`-qualified) and the linker places them in `.rodata`:

```cpp
// Lives in FLASH — zero RAM cost for the config itself.
constexpr osal::mutex_config mtx_cfg{osal::mutex_type::recursive};
osal::mutex m{mtx_cfg};  // only handle_ + valid_ in RAM (≤ 12 bytes typ.)
```

### Backward compatibility

Original positional constructors remain alongside the new config-taking
overloads.  Existing code compiles unmodified:

```cpp
osal::mutex m;                                   // existing — still works
osal::mutex m2{osal::mutex_type::recursive};     // existing — still works
osal::mutex m3{mtx_cfg};                         // new — config-based
```

### C interface

The C API mirrors the pattern with `osal_*_config` structs and
`osal_c_*_create_with_cfg()` functions (C cannot overload):

```c
const osal_mutex_config cfg = {.recursive = 0};
osal_mutex_handle mtx;
osal_c_mutex_create_with_cfg(&mtx, &cfg);
```

### Dead field removal

Where a creation parameter was previously stored as a class member but
never read after construction, it was removed to save RAM:

| Primitive | Removed field | Saving | Reason |
| --- | --- | --- | --- |
| `mutex` | `type_` | 1 byte + pad | Only used to select native vs recursive at init |
| `semaphore` | `type_` | 1 byte + pad | Same — only used to clamp `max_count` at init |

`memory_pool::block_size_` is retained because it has a public runtime
getter (`block_size()`).

### Embedded linker verification

To confirm FLASH placement, inspect the linker map or use `objdump`:

```sh
arm-none-eabi-objdump -t firmware.elf | grep mtx_cfg
# Should show .rodata section
```

---

## Compilation Model

### Single backend macro

Exactly one `OSAL_BACKEND_*` macro is defined per translation unit:

```cmake
target_compile_definitions(myapp PRIVATE OSAL_BACKEND_LINUX)
```

`include/osal/backends.hpp` reads this macro and produces:

```cpp
using active_backend      = backend_linux;
using active_capabilities = capabilities<active_backend>;
using active_traits       = backend_traits<active_backend>;
```

All primitive classes (`osal::mutex`, `osal::semaphore`, etc.) store an
`active_traits::*_handle_t` which carries a platform-native pointer.

### C-linkage bridge

Every `.hpp` primitive header declares a set of `extern "C"` functions that
form the actual ABI:

```cpp
extern "C" {
    osal::result  osal_mutex_create (osal::active_traits::mutex_handle_t*, bool);
    osal::result  osal_mutex_lock   (osal::active_traits::mutex_handle_t*, osal::tick_t);
    // ...
}
```

> **Pure-C interface:** These `extern "C"` functions use C++ types in their
> signatures, so they are not callable from C.  A separate pure-C header
> (`include/osal/osal_c.h`) and bridge (`src/common/osal_c.cpp`) provide
> C11-compatible wrappers.  See [docs/c_api.md](c_api.md) for details.

The corresponding backend `.cpp` provides the definitions.  The C++ wrapper
in the header inlines straight into the call:

```cpp
inline osal::result mutex::lock() noexcept {
    return osal_mutex_lock(&handle_, clock_utils::ms_to_ticks(/* infinite */osal::WAIT_FOREVER));
}
```

This ensures the per-platform code is isolated in one `.cpp` per backend,
while the C++ API is header-only.

---

## Key Types

### `osal::tick_t`

A `uint32_t` count of OS ticks.  Special values:

| Value | Meaning |
| --- | --- |
| `osal::WAIT_FOREVER` | Block indefinitely |
| `osal::NO_WAIT` | Non-blocking / immediate timeout |

### `osal::result`

```cpp
struct result {
    error_code code;
    constexpr bool ok() const noexcept { return code == error_code::ok; }
    constexpr operator bool() const noexcept { return ok(); }
};
constexpr result ok() noexcept { return { error_code::ok }; }
```

### `clock_utils`

Defined in `include/osal/clock.hpp`.  The key function:

```cpp
static constexpr tick_t ms_to_ticks(std::uint32_t ms) noexcept;
```

It calls `osal_clock_tick_period_us()` which is provided by each backend and
returns the number of microseconds per tick.  For the POSIX/Linux backends
ticks are milliseconds (period = 1000 µs).  For FreeRTOS the period derives
from `configTICK_RATE_HZ`.

---

## Capability Flags

`capabilities<Backend>` contains `static constexpr bool` flags that gate
advanced features at compile-time:

```cpp
if constexpr (osal::active_capabilities::has_timed_mutex) {
    result r = m.try_lock_for(osal::milliseconds{50});
}
```

Key flags and which backends support them:

| Flag | FR | ZY | TX | PX | PO | LI | BM | VX | NX | MI | CH | EM | QX | C1 | C2 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `has_recursive_mutex` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ |
| `has_timed_mutex` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ |
| `has_priority_inheritance` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `has_isr_semaphore` | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ |
| `has_thread_affinity` | ✗ | ✓ | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ |
| `has_timed_join` | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ |
| `has_thread_suspend_resume` | cfg* | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `has_thread_local_data` | emu† | emu† | emu† | emu† | emu† | emu† | emu† | emu† | emu† | emu† | emu† | emu† | emu† | emu† | emu† |
| `has_dynamic_thread_priority` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `has_task_notification` | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ | ✗ |
| `has_stack_overflow_detection` | cfg‡ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | ✗ | ✗ |
| `has_cpu_load_stats` | cfg§ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ | ✗ |
| `has_wait_set` | ✗ | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `has_native_event_flags` | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ |
| `has_native_work_queue` | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `has_native_condvar` | ✗ | ✓ | ✗ | ✗ | ✓ | ✓ | ✗ | ✓ | ✓ | ✗ | ✓ | ✗ | ✓ | ✗ | ✗ |
| `has_native_memory_pool` | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ |
| `has_native_stream_buffer` | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `has_isr_stream_buffer` | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `has_native_message_buffer` | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `has_isr_message_buffer` | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `has_native_rwlock` | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ | ✓ | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ |
| `has_spinlock` | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| `has_barrier` | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ | ✓ | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ |

FR=FreeRTOS, ZY=Zephyr, TX=ThreadX, PX=PX5, PO=POSIX, LI=Linux, BM=Bare-metal, VX=VxWorks,
NX=NuttX, MI=Micrium, CH=ChibiOS, EM=embOS, QX=QNX, C1=CMSIS-RTOS, C2=CMSIS-RTOS2

\* `cfg` = depends on backend configuration (`INCLUDE_vTaskSuspend == 1` for FreeRTOS).
† `emu` = provided by the header-only `osal::thread_local_data` emulation (guaranteed under MicrOSAL's C++17 baseline).
‡ `cfg` = depends on `configCHECK_FOR_STACK_OVERFLOW > 0` (FreeRTOS).
§ `cfg` = depends on `configGENERATE_RUN_TIME_STATS == 1` (FreeRTOS).

### Thread-local data

`osal::thread_local_data` provides key-based, per-thread pointer storage that is
independent of backend-native TLS APIs. It is implemented as a portable
header-only emulation using C++ `thread_local` and a small key registry.

On POSIX-compatible backends (POSIX, Linux, NuttX, QNX), native pthread TLS
can be enabled with:

`-DOSAL_THREAD_LOCAL_USE_NATIVE=1`

For FreeRTOS builds, the same switch enables task-local storage slots when
`configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0`.

Native mode is intentionally opt-in because key lifecycle details may differ
from the default emulation contract.

To query backend-native availability independently of emulation support, use:

`osal::has_native_thread_local_data<osal::active_backend>`

Key properties:

- No backend glue required (works uniformly through the C++ layer).
- Per-thread isolation (each thread sees its own value for the same key).
- Key reuse safety via generation counters (stale values are invalidated).
- Bounded key count via `OSAL_THREAD_LOCAL_MAX_KEYS` (default 16).

API surface:

- `thread_local_data::valid()`
- `thread_local_data::set(void*)`
- `thread_local_data::get()`

---

## Work Queue

`osal::work_queue` provides a deferred-execution model: application code submits
lightweight work items (function pointer + void* argument) and a dedicated worker
thread executes them in FIFO order.

### Work Queue API Surface

| Method | Description |
| --- | --- |
| `submit(fn, arg)` | Enqueue a work item for execution |
| `submit_from_isr(fn, arg)` | ISR-safe submit (native backends only) |
| `flush(timeout)` | Block until all queued items have been executed |
| `cancel_all()` | Discard all pending (not yet executing) items |
| `pending()` | Number of items currently in the queue |
| `valid()` | True if the work queue was created successfully |

### Work Queue Native vs Emulated

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native | Zephyr, NuttX, VxWorks | `k_work_queue`, `work_queue()`, `jobQueueLib` |
| Emulated (pthread) | POSIX, Linux | `posix_pthread_work_queue.inl` — dedicated pthread + ring buffer + `pthread_cond_t` |
| Emulated (OSAL) | All other 10 backends | `emulated_work_queue.inl` — worker thread + ring buffer + `osal::condvar` |

Compile-time detection:

```cpp
if constexpr (osal::active_capabilities::has_native_work_queue) {
    wq.submit_from_isr(handler, ctx);  // safe in ISR context
}
```

---

## Condition Variable

`osal::condvar` provides the classic monitor-style condition variable.
A thread that holds an `osal::mutex` may atomically release it and sleep
until another thread signals the condition.

### Condition Variable API Surface

| Method | Description |
| --- | --- |
| `wait(mutex&)` | Atomically unlock mutex, sleep, re-lock on wake |
| `wait_for(mutex&, ms)` | Timed wait; returns `true` if notified, `false` on timeout |
| `notify_one()` | Wake one waiting thread |
| `notify_all()` | Wake all waiting threads |
| `valid()` | True if the condvar was created successfully |

### Condition Variable Native vs Emulated

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native | POSIX, Linux, NuttX, QNX, Zephyr, ChibiOS, VxWorks | `pthread_cond_t`, `k_condvar`, `condition_variable_t`, `condVarLib` |
| Emulated | FreeRTOS, ThreadX, PX5, Micrium, embOS, Bare-metal, CMSIS-RTOS, CMSIS-RTOS2 | Birrell's pattern (guard mutex + binary semaphore array) |

Compile-time detection:

```cpp
if constexpr (osal::active_capabilities::has_native_condvar) {
    // native implementation in use
}
```

---

## Memory Pool

`osal::memory_pool` provides a deterministic fixed-size block allocator with
O(1) alloc/free and zero fragmentation.  The caller supplies the backing
storage buffer.

### Memory Pool API Surface

| Method | Description |
| --- | --- |
| `allocate()` | Allocate one block (non-blocking, returns `nullptr` if exhausted) |
| `allocate_for(ms)` | Allocate one block with timeout |
| `deallocate(block)` | Return a block to the pool |
| `available()` | Number of blocks currently free |
| `block_size()` | Size of each block in bytes |
| `valid()` | True if the pool was created successfully |

### Memory Pool Native vs Emulated

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native | ThreadX, PX5, VxWorks, Micrium, ChibiOS, embOS, CMSIS-RTOS, CMSIS-RTOS2 | `tx_block_pool`, `memPartLib`, `OSMemCreate`, `chPoolInit`, `OS_MEMPOOL`, `osPool`, `osMemoryPool` |
| Emulated | FreeRTOS, Zephyr, POSIX, Linux, NuttX, QNX, Bare-metal | Bitmap + OSAL mutex + counting semaphore |

Compile-time detection:

```cpp
if constexpr (osal::active_capabilities::has_native_memory_pool) {
    // native block pool in use
}
```

---

## Ring Buffer

`osal::ring_buffer<T, N>` is a **header-only**, lock-free, single-producer /
single-consumer (SPSC) ring buffer.  It has **zero OS dependency** — no
mutex, no semaphore — and works identically on all 15 backends.

### Design

- Internal storage of `N + 1` elements (one sentinel slot).
- `std::atomic<std::size_t>` head and tail with `acquire`/`release` ordering.
- `T` must be `std::is_trivially_copyable`; `N > 0` (enforced by `static_assert`).

### Ring Buffer API Surface

| Method | Description |
| --- | --- |
| `try_push(v)` | Enqueue one item; returns `false` if full |
| `try_pop(v)` | Dequeue one item; returns `false` if empty |
| `peek(v)` | Read front without removing; returns `false` if empty |
| `size()` | Number of items currently in the buffer |
| `free()` | Number of free slots |
| `empty()` | True if buffer has no items |
| `full()` | True if buffer is at capacity |
| `capacity()` | Maximum number of items (`N`) |
| `reset()` | Clear the buffer (single-thread only) |

Usage:

```cpp
osal::ring_buffer<int, 64> rb;
rb.try_push(42);
int val;
rb.try_pop(val);  // val == 42
```

---

## Read-Write Lock

`osal::rwlock` provides multiple-reader / single-writer semantics.  Multiple
readers may hold the lock concurrently; a writer requires exclusive access.

### Read-Write Lock API Surface

| Method | Description |
| --- | --- |
| `read_lock()` | Acquire shared (read) access — blocking |
| `read_lock_for(ms)` | Timed shared lock |
| `read_unlock()` | Release read lock |
| `write_lock()` | Acquire exclusive (write) access — blocking |
| `write_lock_for(ms)` | Timed exclusive lock |
| `write_unlock()` | Release write lock |
| `valid()` | True if the rwlock was created successfully |

RAII guards:

```cpp
osal::rwlock rw;
{ osal::rwlock::read_guard  rg{rw}; /* shared read */ }
{ osal::rwlock::write_guard wg{rw}; /* exclusive write */ }
```

### Read-Write Lock Native vs Emulated

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native | POSIX, Linux, NuttX, QNX | `pthread_rwlock_t` |
| Emulated | All other 11 backends | OSAL mutex + condvar + reader counter |

Compile-time detection:

```cpp
if constexpr (osal::active_capabilities::has_native_rwlock) {
    // native pthread_rwlock_t in use
}
```

---

## Event Flags

`osal::event_flags` provides a shared bitmask synchronisation primitive.
Threads set, clear, or wait on individual bits within a 32-bit word.

### Event Flags API Surface

| Method | Description |
| --- | --- |
| `set(bits)` | Set one or more bits (task context) |
| `clear(bits)` | Clear one or more bits |
| `get()` | Non-blocking read of current bits |
| `wait_any(bits, …)` | Block until **any** of the requested bits are set |
| `wait_all(bits, …)` | Block until **all** of the requested bits are set |
| `set_isr(bits)` | ISR-safe set (see ISR safety table) |

### Event Flags Native vs Emulated

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native | FreeRTOS, Zephyr, ThreadX, PX5, Micrium, ChibiOS, embOS, CMSIS-RTOS2 (8) | `xEventGroup`, `k_event`, `tx_event_flags`, `osEventFlags`, etc. |
| Emulated | POSIX, Linux, Bare-metal, VxWorks, NuttX, QNX, CMSIS-RTOS (7) | OSAL mutex + per-waiter binary semaphore array (Birrell) |

Compile-time detection:

```cpp
if constexpr (osal::active_capabilities::has_native_event_flags) {
    // native event group in use
}
```

---

## Stream Buffer

`osal::stream_buffer<N, TriggerLevel>` provides a byte-oriented, blocking
single-producer / single-consumer (SPSC) ring buffer modelled on FreeRTOS
`xStreamBuffer` semantics.

Unlike `osal::queue<T,N>` (item-oriented, typed), a stream buffer treats the
ring as a raw byte sequence — multiple bytes may be written or read per call,
and a **trigger level** controls how many bytes must accumulate before a
blocking `receive()` unblocks.

### Stream Buffer API Surface

| Method | Description |
| --- | --- |
| `send(data, len)` | Block until `len` bytes of space are available, then write |
| `try_send(data, len)` | Non-blocking write; returns `true` if all bytes were written |
| `send_for(data, len, ms)` | Timed write; returns `true` on success |
| `send_isr(data, len)` | ISR-safe write (non-blocking) |
| `receive(buf, max_len)` | Block until ≥ TriggerLevel bytes are available, then read |
| `try_receive(buf, max_len)` | Non-blocking read; returns bytes read (0 if below trigger) |
| `receive_for(buf, max_len, ms)` | Timed read; returns bytes read within timeout |
| `receive_isr(buf, max_len)` | ISR-safe read (non-blocking) |
| `available()` | Bytes currently in the ring |
| `free_space()` | Bytes that can be written without blocking |
| `reset()` | Discard all buffered data (single-thread only) |
| `valid()` | True if initialisation succeeded |

### Template Parameters

| Parameter | Meaning | Constraint |
| --- | --- | --- |
| `N` | Byte capacity (usable bytes) | N > 0 |
| `TriggerLevel` | Minimum bytes before `receive()` unblocks | 1 ≤ TriggerLevel ≤ N |

### Stream Buffer Native vs Emulated

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native | FreeRTOS | `xStreamBufferCreate`, `xStreamBufferSendFromISR`, `xStreamBufferReceiveFromISR` |
| Emulated | All other 14 backends | SPSC ring + binary semaphores (`emulated_stream_buffer.inl`) |

The emulated implementation uses two binary semaphores — `data_sem` (signalled
when bytes become available) and `space_sem` (signalled when space is freed) —
and updates the ring's `head_` pointer atomically in a single release-store
after the complete write, preventing partial-frame visibility by the consumer.

Compile-time detection:

```cpp
if constexpr (osal::active_capabilities::has_native_stream_buffer) {
    // native xStreamBuffer in use (FreeRTOS only)
}
if constexpr (osal::active_capabilities::has_isr_stream_buffer) {
    sb.send_isr(data, len);  // genuinely ISR-safe
}
```

Usage:

```cpp
osal::stream_buffer<128> sb;   // 128-byte ring, trigger = 1
sb.send("hello", 5);           // blocks if ring is full
char rx[64];
std::size_t n = sb.receive(rx, sizeof(rx));  // blocks until ≥1 byte
```

---

## Message Buffer

`osal::message_buffer<N>` provides a message-oriented blocking SPSC ring
modelled on FreeRTOS `xMessageBuffer` semantics.  Each send enqueues exactly
one indivisible message; each receive dequeues exactly one message.

Internally, each message is framed as:

```text
[ osal_mb_length_t (2 bytes) ][ payload bytes ]
```

This framing is transparent to the user.  The maximum payload per message is
`N - sizeof(osal_mb_length_t)` bytes.

### Message Buffer API Surface

| Method | Description |
| --- | --- |
| `send(msg, len)` | Block until the frame fits, then enqueue |
| `try_send(msg, len)` | Non-blocking enqueue; returns `true` on success |
| `send_for(msg, len, ms)` | Timed enqueue |
| `send_isr(msg, len)` | ISR-safe enqueue (non-blocking) |
| `receive(buf, max_len)` | Block until a complete message is available, then read |
| `try_receive(buf, max_len)` | Non-blocking read (0 if no complete message) |
| `receive_for(buf, max_len, ms)` | Timed read |
| `receive_isr(buf, max_len)` | ISR-safe read (non-blocking) |
| `next_message_size()` | Payload size of the next queued message (0 if empty) |
| `free_space()` | Maximum payload that can be enqueued without blocking |
| `reset()` | Discard all queued messages (single-thread only) |
| `valid()` | True if initialisation succeeded |

### Length-Prefix Type

The per-message length header type is `osal_mb_length_t` (`uint16_t` by
default).  Override before including `message_buffer.hpp` if any single
message can exceed 65 535 bytes:

```cpp
#define OSAL_MESSAGE_BUFFER_LENGTH_TYPE std::uint32_t
#include <osal/message_buffer.hpp>
```

### Message Buffer Native vs Emulated

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native | FreeRTOS | `xMessageBufferCreate`, `xMessageBufferSendFromISR`, etc. |
| Emulated | All other 14 backends | Length-prefixed SPSC ring (`emulated_message_buffer.inl`) |

The emulated path uses a distinct `emu_mb_` symbol prefix (separate from the
`emu_sb_` prefix in `emulated_stream_buffer.inl`) to avoid anonymous-namespace
collisions when both `.inl` files are included in the same translation unit.

Truncation semantics: if the receive buffer (`max_len`) is smaller than the
stored message payload, the excess bytes are silently discarded — matching
FreeRTOS behaviour.

Compile-time detection:

```cpp
if constexpr (osal::active_capabilities::has_native_stream_buffer) {
    // native xMessageBuffer in use (FreeRTOS only)
}
```

Usage:

```cpp
struct Sensor { float value; uint32_t ts; };
osal::message_buffer<128> mb;    // 128-byte ring, 126-byte max payload

Sensor s{42.0f, 1234};
mb.send(&s, sizeof(s));          // blocks until space available

Sensor rx{};
mb.receive(&rx, sizeof(rx));     // blocks until a message arrives
```

---

## `ms_to_ticks` Design (Implementation Note)

An earlier design placed `static tick_t ms_to_ticks()` as a private member
of each primitive class and required a per-class per-backend `.cpp` definition
to avoid ODR issues with the template `queue<T,N>`.

The current design resolves this cleanly:

1. `clock.hpp` defines `clock_utils::ms_to_ticks()` as an `inline` function.
2. `backends.hpp` `#include`s `clock.hpp` so it is visible in all primitive headers.
3. All primitive headers call `clock_utils::ms_to_ticks(ms)` directly.
4. No per-primitive `.cpp` needed for tick conversion.

The only runtime dependency is `osal_clock_tick_period_us()`, which is
provided by each backend's `.cpp`.

---

## Static Pool Sizing

For backed that use static object pools (FreeRTOS, Zephyr, ThreadX, PX5,
bare-metal), the pool sizes are defined as preprocessor macros at the top of
each backend `.cpp` file.  For production, adjust these or replace the pool
implementation with a deterministic allocator.

| Macro | Default |
| --- | --- |
| `OSAL_FR_MAX_THREADS` | 8 |
| `OSAL_FR_MAX_MUTEXES` | 16 |
| `OSAL_ZEPHYR_MAX_SEMS` | 16 |
| `OSAL_TX_MAX_TIMERS` | 8 |
| … (see each backend) | |

---

## Spinlock

`osal::spinlock` provides a low-level, busy-wait mutual exclusion primitive
for contexts where sleeping is not permitted (e.g. interrupt service routines
or real-time critical sections).  It is intentionally minimal — no recursion,
no ownership transfer.

Where native RTOS spinlock support is not available the operations return
`error_code::not_supported` (capability-guarded at compile time via
`has_spinlock`).

### Spinlock API Surface

| Method | Description |
| --- | --- |
| `lock()` | Acquire the spinlock; busy-waits until free.  Returns `result::ok()` or `not_supported`. |
| `try_lock()` | Non-blocking attempt.  Returns `true` if acquired, `false` otherwise. |
| `unlock()` | Release the spinlock.  No-op on stub backends. |
| `valid()` | Returns `true` if the underlying native handle was initialised successfully. |

RAII helper: `osal::spinlock::lock_guard g{sl};` acquires on construction and
releases on destruction.

### Spinlock Native vs Stub

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native — `k_spinlock` | Zephyr | `src/zephyr/zephyr_spinlock.inl` |
| Stub (`not_supported`) | All other 14 backends | `src/common/emulated_spinlock.inl` |

---

## Barrier

`osal::barrier` implements a _rendezvous_ point: a fixed number of threads
(`count`) all call `wait()`, and none may proceed until every thread has
arrived.  One thread — the _serial thread_ — receives `error_code::barrier_serial`
from `wait()` to indicate it is responsible for any once-per-cycle
post-barrier work; all others receive `result::ok()`.

Where native barrier support is unavailable `wait()` returns
`error_code::not_supported` (gated by `has_barrier`).

### Barrier API Surface

| Method | Description |
| --- | --- |
| `barrier(count)` | Construct a barrier for `count` threads.  Check `valid()` after construction. |
| `wait()` | Block until `count` threads have called `wait()`.  Returns `barrier_serial` for the serial thread, `ok` for all others, or `not_supported` on stub backends. |
| `valid()` | Returns `true` if the native handle was initialised successfully. |

### Barrier Native vs Stub

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native — `pthread_barrier_t` | POSIX, Linux, NuttX, QNX | `src/common/posix/posix_barrier.inl` |
| Stub (`not_supported`) | All other 11 backends | `src/common/emulated_barrier.inl` |

`error_code::barrier_serial` (value 14) mirrors the POSIX constant
`PTHREAD_BARRIER_SERIAL_THREAD` and identifies the serial thread.

---

## Thread Task Notification

`osal::thread::notify()`, `notify_isr()`, and the static
`wait_for_notification()` implement a lightweight, lock-free event-delivery
mechanism that bypasses the scheduler queue.  A 32-bit value can be posted
to a thread's notification word and the recipient thread blocks until it is
set.

This is most useful on RTOS kernels that provide native task-notification
primitives (FreeRTOS `xTaskNotify`, embOS task events).  On all other
backends the calls return `error_code::not_supported` (gated by
`has_task_notification`).

### Task Notification API Surface

| Method | Description |
| --- | --- |
| `notify(value = 0)` | Post `value` to the thread's notification word from a normal context. |
| `notify_isr(value = 0)` | Post `value` from an ISR context (uses the RTOS ISR-safe variant). |
| `static wait_for_notification(timeout, value_out*)` | Block until a notification arrives or `timeout` expires.  Optionally receive the posted value via `value_out`. |

### Task Notification Native vs Stub

| Strategy | Backends | Implementation |
| --- | --- | --- |
| Native — `xTaskNotify` / `xTaskNotifyWait` | FreeRTOS | Inline in `src/freertos/freertos_backend.cpp` |
| Native — `OS_TASKEVENT_Set` / `GetMasked` | embOS | Inline in `src/embos/embos_backend.cpp` |
| Stub (`not_supported`) | All other 13 backends | `src/common/emulated_task_notify.inl` |

