# MicrOSAL — Backend Integration Guide

## Adding a New Backend

Follow these steps to integrate a new RTOS or platform.

---

### Step 1 — Add a backend tag type

In `include/osal/backends.hpp`, add a new empty struct:

```cpp
struct backend_mynewrtos {};
```

Then add it to the macro selector block:

```cpp
#elif defined(OSAL_BACKEND_MYNEWRTOS)
using active_backend = backend_mynewrtos;
```

---

### Step 2 — Define capability flags

In `include/osal/capabilities.hpp`, add a specialisation:

```cpp
template<>
struct capabilities<backend_mynewrtos> {
    static constexpr bool has_recursive_mutex       = true;
    static constexpr bool has_timed_mutex           = true;
    static constexpr bool has_priority_inheritance  = true;   // set false if no PI on mutexes
    static constexpr bool has_isr_semaphore         = true;
    static constexpr bool has_timed_semaphore       = true;
    static constexpr bool has_thread_affinity       = false;
    static constexpr bool has_timed_join            = false;
    static constexpr bool has_thread_suspend_resume = false;
    static constexpr bool has_thread_local_data     = true;   // header-only emulation
    static constexpr bool has_dynamic_thread_priority = true; // runtime priority change API
    static constexpr bool has_task_notification     = false;  // direct-to-task lightweight signal
    static constexpr bool has_stack_overflow_detection = false;
    static constexpr bool has_cpu_load_stats        = false;
    static constexpr bool has_isr_queue             = true;
    static constexpr bool has_timed_queue           = true;
    static constexpr bool has_timer                 = true;
    static constexpr bool has_periodic_timer        = true;
    static constexpr bool has_native_event_flags    = false;
    static constexpr bool has_isr_event_flags       = false;
    static constexpr bool has_timed_event_flags     = true;
    static constexpr bool has_wait_set              = false;
    static constexpr bool has_native_work_queue     = false;
    static constexpr bool has_native_condvar        = false;
    static constexpr bool has_native_memory_pool    = false;
    static constexpr bool has_native_stream_buffer  = false;
    static constexpr bool has_isr_stream_buffer     = false;
    static constexpr bool has_native_message_buffer = false;  // length-prefixed message buffers
    static constexpr bool has_isr_message_buffer    = false;
    static constexpr bool has_native_rwlock         = false;
    static constexpr bool has_spinlock              = false;
    static constexpr bool has_barrier               = false;
    static constexpr bool has_monotonic_clock       = true;
    static constexpr bool has_system_clock          = false;
    static constexpr bool has_high_resolution       = false;
};
```

Set only the flags that your RTOS natively supports.  Flags that are `false`
will produce `error_code::not_supported` at runtime if called, and the
`if constexpr` guards in application code will skip the code path at
compile-time.

`has_thread_local_data` should be `true` in MicrOSAL backends because the
portable header-only emulation (`osal::thread_local_data`) is part of the
baseline C++17 implementation.

On POSIX-compatible backends (POSIX, Linux, NuttX, QNX), define
`OSAL_THREAD_LOCAL_USE_NATIVE=1` to use pthread TLS instead of the C++
`thread_local` emulation. This is opt-in because key lifecycle semantics may
differ slightly from the default emulation contract.

For FreeRTOS, the same switch uses task-local storage slots when
`configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0`. Zephyr keeps emulation mode
because `k_thread_custom_data` is a single-slot API incompatible with
micrOSAL's keyed multi-slot contract.

You can check backend-native TLS availability via:

`osal::has_native_thread_local_data<backend_tag>`

---

### Step 3 — Define handle types

In `include/osal/backend_traits.hpp`, add a specialisation for your backend:

```cpp
template<>
struct backend_traits<backend_mynewrtos> {
    static constexpr std::size_t default_stack_bytes     = 2048U;
    static constexpr std::size_t thread_storage_bytes    = 256U;
    static constexpr std::size_t mutex_storage_bytes     = 64U;
    static constexpr std::size_t semaphore_storage_bytes = 64U;
    static constexpr std::size_t queue_storage_bytes     = 128U;
    static constexpr std::size_t timer_storage_bytes     = 64U;
    static constexpr std::size_t event_flags_storage_bytes = 64U;
    static constexpr std::size_t wait_set_storage_bytes  = 0U;

    struct thread_handle_t       { void* native; };
    struct mutex_handle_t        { void* native; };
    struct semaphore_handle_t    { void* native; };
    struct queue_handle_t        { void* native; };
    struct timer_handle_t        { void* native; };
    struct event_flags_handle_t  { void* native; };
    struct wait_set_handle_t     { void* native; };
    struct work_queue_handle_t   { void* native; };
    struct condvar_handle_t      { void* native; };
    struct memory_pool_handle_t  { void* native; };
    struct stream_buffer_handle_t  { void* native; };
    struct message_buffer_handle_t { void* native; };
    struct rwlock_handle_t       { void* native; };
};
```

For bare-metal backends, replace `void* native` with a concrete struct:

```cpp
struct mutex_handle_t {
    volatile std::uint32_t lock_word;
    bool                   valid;
};
```

---

### Step 4 — Implement the C-linkage functions

Create `src/mynewrtos/mynewrtos_backend.cpp`:

```cpp
#define OSAL_BACKEND_MYNEWRTOS
#include <osal/osal.hpp>
#include <mynewrtos.h>

extern "C" {

// ---- Clock ---------------------------------------------------------------
std::int64_t      osal_clock_monotonic_ms() noexcept;
std::int64_t      osal_clock_system_ms()    noexcept;
osal::tick_t      osal_clock_ticks()        noexcept;
std::uint32_t     osal_clock_tick_period_us() noexcept;

// ---- Thread --------------------------------------------------------------
osal::result osal_thread_create(
    osal::active_traits::thread_handle_t*,
    void(*)(void*), void*, osal::priority_t,
    osal::affinity_t, void*, osal::stack_size_t, const char*) noexcept;
osal::result osal_thread_join(osal::active_traits::thread_handle_t*, osal::tick_t) noexcept;
osal::result osal_thread_detach(osal::active_traits::thread_handle_t*) noexcept;
osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t*, osal::priority_t) noexcept;
osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t*, osal::affinity_t) noexcept;
void         osal_thread_yield() noexcept;
void         osal_thread_sleep_ms(std::uint32_t) noexcept;

// ---- Mutex ---------------------------------------------------------------
osal::result osal_mutex_create(osal::active_traits::mutex_handle_t*, bool recursive) noexcept;
osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t*) noexcept;
osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t*, osal::tick_t) noexcept;
osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t*) noexcept;
osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t*) noexcept;

// ---- Semaphore -----------------------------------------------------------
osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t*, unsigned init, unsigned max) noexcept;
osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t*) noexcept;
osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t*) noexcept;
osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t*) noexcept;
osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t*, osal::tick_t) noexcept;
osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t*) noexcept;

// ---- Queue ---------------------------------------------------------------
osal::result osal_queue_create(osal::active_traits::queue_handle_t*, void*, std::size_t item_sz, std::size_t cap) noexcept;
osal::result osal_queue_destroy(osal::active_traits::queue_handle_t*) noexcept;
osal::result osal_queue_send(osal::active_traits::queue_handle_t*, const void*, osal::tick_t) noexcept;
osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t*, const void*) noexcept;
osal::result osal_queue_receive(osal::active_traits::queue_handle_t*, void*, osal::tick_t) noexcept;
osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t*, void*) noexcept;
osal::result osal_queue_peek(osal::active_traits::queue_handle_t*, void*, osal::tick_t) noexcept;
std::size_t  osal_queue_count(const osal::active_traits::queue_handle_t*) noexcept;
std::size_t  osal_queue_free(const osal::active_traits::queue_handle_t*) noexcept;

// ---- Timer ---------------------------------------------------------------
osal::result osal_timer_create(osal::active_traits::timer_handle_t*, const char*,
    osal_timer_callback_t, void*, osal::tick_t period, bool auto_reload) noexcept;
osal::result osal_timer_destroy(osal::active_traits::timer_handle_t*) noexcept;
osal::result osal_timer_start(osal::active_traits::timer_handle_t*) noexcept;
osal::result osal_timer_stop(osal::active_traits::timer_handle_t*) noexcept;
osal::result osal_timer_reset(osal::active_traits::timer_handle_t*) noexcept;
osal::result osal_timer_set_period(osal::active_traits::timer_handle_t*, osal::tick_t) noexcept;
bool         osal_timer_is_active(const osal::active_traits::timer_handle_t*) noexcept;

// ---- Event flags ---------------------------------------------------------
osal::result      osal_event_flags_create(osal::active_traits::event_flags_handle_t*) noexcept;
osal::result      osal_event_flags_destroy(osal::active_traits::event_flags_handle_t*) noexcept;
osal::result      osal_event_flags_set(osal::active_traits::event_flags_handle_t*, osal::event_bits_t) noexcept;
osal::result      osal_event_flags_clear(osal::active_traits::event_flags_handle_t*, osal::event_bits_t) noexcept;
osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t*) noexcept;
osal::result      osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t*,
    osal::event_bits_t, osal::event_bits_t*, bool clear_on_exit, osal::tick_t) noexcept;
osal::result      osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t*,
    osal::event_bits_t, osal::event_bits_t*, bool clear_on_exit, osal::tick_t) noexcept;
osal::result      osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t*, osal::event_bits_t) noexcept;

// ---- Wait-set (stub if not supported) ------------------------------------
osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept;
osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept;
osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int fd, std::uint32_t events) noexcept;
osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int fd) noexcept;
osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t*,
    int* fds_ready, std::size_t max_ready, std::size_t* n_ready, osal::tick_t) noexcept;

// ---- Work queue ----------------------------------------------------------
osal::result osal_work_queue_create(osal::active_traits::work_queue_handle_t*,
    void* stack, std::size_t stack_bytes, std::size_t depth, const char* name) noexcept;
osal::result osal_work_queue_destroy(osal::active_traits::work_queue_handle_t*) noexcept;
osal::result osal_work_queue_submit(osal::active_traits::work_queue_handle_t*,
    osal_work_func_t func, void* arg) noexcept;
osal::result osal_work_queue_submit_from_isr(osal::active_traits::work_queue_handle_t*,
    osal_work_func_t func, void* arg) noexcept;
osal::result osal_work_queue_flush(osal::active_traits::work_queue_handle_t*, osal::tick_t timeout) noexcept;
osal::result osal_work_queue_cancel_all(osal::active_traits::work_queue_handle_t*) noexcept;
std::size_t  osal_work_queue_pending(const osal::active_traits::work_queue_handle_t*) noexcept;

// ---- Condition variable --------------------------------------------------
osal::result osal_condvar_create(osal::active_traits::condvar_handle_t*) noexcept;
osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t*) noexcept;
osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t*,
    osal::active_traits::mutex_handle_t*) noexcept;
osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t*) noexcept;
osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t*) noexcept;

// ---- Memory pool ---------------------------------------------------------
osal::result osal_memory_pool_create(osal::active_traits::memory_pool_handle_t*,
    void* buffer, std::size_t buf_bytes, std::size_t block_size,
    std::size_t block_count, const char* name) noexcept;
osal::result osal_memory_pool_destroy(osal::active_traits::memory_pool_handle_t*) noexcept;
void*        osal_memory_pool_allocate(osal::active_traits::memory_pool_handle_t*) noexcept;
void*        osal_memory_pool_allocate_timed(osal::active_traits::memory_pool_handle_t*,
    osal::tick_t timeout) noexcept;
osal::result osal_memory_pool_deallocate(osal::active_traits::memory_pool_handle_t*,
    void* block) noexcept;
std::size_t  osal_memory_pool_available(const osal::active_traits::memory_pool_handle_t*) noexcept;

// ---- Read-write lock -----------------------------------------------------
osal::result osal_rwlock_create(osal::active_traits::rwlock_handle_t*) noexcept;
osal::result osal_rwlock_destroy(osal::active_traits::rwlock_handle_t*) noexcept;
osal::result osal_rwlock_read_lock(osal::active_traits::rwlock_handle_t*,
    osal::tick_t timeout) noexcept;
osal::result osal_rwlock_read_unlock(osal::active_traits::rwlock_handle_t*) noexcept;
osal::result osal_rwlock_write_lock(osal::active_traits::rwlock_handle_t*,
    osal::tick_t timeout) noexcept;
osal::result osal_rwlock_write_unlock(osal::active_traits::rwlock_handle_t*) noexcept;

// ---- Stream buffer -------------------------------------------------------
osal::result osal_stream_buffer_create(osal::active_traits::stream_buffer_handle_t*,
    void* buffer, std::size_t capacity, std::size_t trigger_level) noexcept;
osal::result osal_stream_buffer_destroy(osal::active_traits::stream_buffer_handle_t*) noexcept;
osal::result osal_stream_buffer_send(osal::active_traits::stream_buffer_handle_t*,
    const void* data, std::size_t len, osal::tick_t timeout_ticks) noexcept;
osal::result osal_stream_buffer_send_isr(osal::active_traits::stream_buffer_handle_t*,
    const void* data, std::size_t len) noexcept;
std::size_t  osal_stream_buffer_receive(osal::active_traits::stream_buffer_handle_t*,
    void* buf, std::size_t max_len, osal::tick_t timeout_ticks) noexcept;
std::size_t  osal_stream_buffer_receive_isr(osal::active_traits::stream_buffer_handle_t*,
    void* buf, std::size_t max_len) noexcept;
std::size_t  osal_stream_buffer_available(const osal::active_traits::stream_buffer_handle_t*) noexcept;
std::size_t  osal_stream_buffer_free_space(const osal::active_traits::stream_buffer_handle_t*) noexcept;
osal::result osal_stream_buffer_reset(osal::active_traits::stream_buffer_handle_t*) noexcept;

// ---- Message buffer ------------------------------------------------------
osal::result osal_message_buffer_create(osal::active_traits::message_buffer_handle_t*,
    void* buffer, std::size_t capacity) noexcept;
osal::result osal_message_buffer_destroy(osal::active_traits::message_buffer_handle_t*) noexcept;
osal::result osal_message_buffer_send(osal::active_traits::message_buffer_handle_t*,
    const void* msg, std::size_t len, osal::tick_t timeout_ticks) noexcept;
osal::result osal_message_buffer_send_isr(osal::active_traits::message_buffer_handle_t*,
    const void* msg, std::size_t len) noexcept;
std::size_t  osal_message_buffer_receive(osal::active_traits::message_buffer_handle_t*,
    void* buf, std::size_t max_len, osal::tick_t timeout_ticks) noexcept;
std::size_t  osal_message_buffer_receive_isr(osal::active_traits::message_buffer_handle_t*,
    void* buf, std::size_t max_len) noexcept;
std::size_t  osal_message_buffer_available(const osal::active_traits::message_buffer_handle_t*) noexcept;
std::size_t  osal_message_buffer_free_space(const osal::active_traits::message_buffer_handle_t*) noexcept;
osal::result osal_message_buffer_reset(osal::active_traits::message_buffer_handle_t*) noexcept;

// ---- Spinlock (gated by has_spinlock) ------------------------------------
osal::result osal_spinlock_create(osal::active_traits::spinlock_handle_t*) noexcept;
osal::result osal_spinlock_destroy(osal::active_traits::spinlock_handle_t*) noexcept;
osal::result osal_spinlock_lock(osal::active_traits::spinlock_handle_t*) noexcept;
bool         osal_spinlock_try_lock(osal::active_traits::spinlock_handle_t*) noexcept;
void         osal_spinlock_unlock(osal::active_traits::spinlock_handle_t*) noexcept;

// ---- Barrier (gated by has_barrier) --------------------------------------
osal::result osal_barrier_create(osal::active_traits::barrier_handle_t*,
    std::uint32_t count) noexcept;
osal::result osal_barrier_destroy(osal::active_traits::barrier_handle_t*) noexcept;
osal::result osal_barrier_wait(osal::active_traits::barrier_handle_t*) noexcept;

// ---- Task notification (gated by has_task_notification) ------------------
osal::result osal_task_notify(osal::active_traits::thread_handle_t* handle,
    std::uint32_t value) noexcept;
osal::result osal_task_notify_isr(osal::active_traits::thread_handle_t* handle,
    std::uint32_t value) noexcept;
osal::result osal_task_notify_wait(std::uint32_t clear_on_entry,
    std::uint32_t clear_on_exit, std::uint32_t* value_out,
    osal::tick_t timeout) noexcept;

} // extern "C"
```

---

### Step 4.1 — Reuse `backend_timeout_adapter.hpp` for timed-native APIs

Backends that convert OSAL timeouts to native absolute-time forms should use:

```cpp
#include "../common/backend_timeout_adapter.hpp"
```

Then convert timeouts via the shared helper instead of per-backend ad-hoc logic:

```cpp
const struct timespec abs =
    osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_MONOTONIC, timeout_ticks);

const int poll_timeout_ms =
    osal::detail::backend_timeout_adapter::to_poll_timeout_ms(timeout_ticks);
```

This preserves `NO_WAIT` / `WAIT_FOREVER` semantics and avoids overflow-prone
casts (for example when an API expects `int` milliseconds).

Current users: Linux, POSIX, QNX, and NuttX backends.

---

### Step 5 — Add to CMakeLists.txt

In `osal/CMakeLists.txt`, extend the backend selector block:

```cmake
elseif(_OSAL_BACKEND_UPPER STREQUAL "MYNEWRTOS")
    set(OSAL_BACKEND_SRC src/mynewrtos/mynewrtos_backend.cpp)
    set(OSAL_BACKEND_MACRO OSAL_BACKEND_MYNEWRTOS)
```

---

### Step 6 — Add to Kconfig (Zephyr only)

If building with west, add a Kconfig entry in `Kconfig`:

```kconfig
config MICRO_OSAL_BACKEND_MYNEWRTOS
    bool "MyNewRTOS"
    depends on MICRO_OSAL
    help
      Use the MyNewRTOS backend for MicrOSAL.
```

---

## Required Function Contract

### Clock functions

| Function                        | Contract                                                             |
|---------------------------------|----------------------------------------------------------------------|
| `osal_clock_monotonic_ms()`     | Returns ms since power-on; monotonically increasing; never negative |
| `osal_clock_system_ms()`        | May return wall-clock time; may wrap on 32-bit systems              |
| `osal_clock_ticks()`            | Returns current OS tick counter                                      |
| `osal_clock_tick_period_us()`   | Returns microseconds per tick; used by `clock_utils::ms_to_ticks()` |

### Timeout semantics

All blocking functions receive a `tick_t timeout_ticks` parameter:

- `osal::WAIT_FOREVER` — block indefinitely.
- `osal::NO_WAIT` — return immediately if not available.
- Any other value — block for at most *N* ticks, then return
  `error_code::timeout`.

### Compile-time tick compatibility guard

MicrOSAL can enforce that `osal::tick_t` / `osal_tick_t` is not wider than the
backend's native timeout width.

- Guard macro: `OSAL_CFG_NATIVE_TICK_BITS` (`16`, `32`, or `64`)
- Enforced via `static_assert` in both C++ (`types.hpp`) and C (`osal_c.h`)
- OSAL tick type selection: `OSAL_CFG_TICK_TYPE_U16|U32|U64`

If violated, compilation fails with a clear error so mismatched timeout-width
configurations are caught early.

Zephyr integration sets this automatically from `CONFIG_TIMEOUT_64BIT`.
In Zephyr Kconfig, MicrOSAL exposes only 32-bit and 64-bit tick-width choices.
Zephyr can also enable strict mode via `CONFIG_MICRO_OSAL_STRICT_TICK_INFERENCE`.

For standalone CMake builds:

- `-DOSAL_NATIVE_TICK_BITS=AUTO` (recommended default)
  - Auto-maps to `64` for `LINUX|POSIX|NUTTX|QNX`
  - Auto-maps to `32` for other backends unless overridden
- `-DOSAL_FREERTOS_TICK_BITS=16|32|64` to override FreeRTOS AUTO mapping
- `-DOSAL_ZEPHYR_TIMEOUT_64BIT=ON|OFF` to hint Zephyr timeout width in
    standalone (non-west) builds
- `-DOSAL_TICK_WIDTH=16|32|64` to choose the OSAL API tick token width
    (the `16` option is intended for standalone/non-Zephyr builds)
- `-DOSAL_STRICT_TICK_INFERENCE=ON` to turn ambiguous AUTO inference into a
    hard CMake configure error (recommended for CI/release builds)

If AUTO cannot infer a configurable backend width precisely, MicrOSAL defaults
conservatively and emits a CMake warning with the override to use.

### ISR variants

Functions suffixed `_isr` (e.g., `osal_semaphore_give_isr`) MUST be safe
to call from interrupt context.  If the underlying RTOS does not support
ISR-safe access, return `error_code::not_supported`.

### Null-safety

All public C functions must tolerate a `nullptr` handle pointer and return
`error_code::not_initialized` or an appropriate error rather than crashing.

---

## Bare-Metal Backend Reference

The `BAREMETAL` backend (`src/bare_metal/bare_metal_backend.cpp`) targets
systems with no OS at all — Cortex-M, RISC-V, or simulation environments
where no scheduler exists.  It provides the complete OSAL API surface, but
important constraints apply.

---

### Cooperative scheduling model

Bare-metal "threads" are **not preemptive**.  The scheduler is a simple
round-robin triggered only when a task explicitly calls `osal::thread::yield()`
or a blocking OSAL function (`sleep_for`, `mutex::lock`, `semaphore::take`,
etc.).  Key implications:

- A task that never yields, sleeps, or blocks will starve all other tasks.
- Priority values passed to `thread_config::priority` are **ignored** — the
  scheduler picks the next valid task in slot order.
- `osal::AFFINITY_ANY` (= 0) is the only meaningful affinity value; any other
  value is accepted but silently ignored.

---

### Thread/stack model — important limitation

The user-supplied `stack` buffer in `thread_config` is **stored but not used
for context switching**.  The bare-metal scheduler calls the entry function
directly on the current operating stack using `setjmp`/`longjmp` for yield
points; it does not perform true stack-frame manipulation.

Consequence: each task consumes stack space on the main/interrupt stack during
its execution slice, not on its private buffer.  The `stack_bytes` field is
preserved for forward compatibility with RTOS backends but serves no purpose
in the current bare-metal implementation.

---

### SysTick integration — required user action

The bare-metal backend provides **two** tick-advance symbols.  The user's
SysTick handler (or equivalent) must call **exactly one** of them:

| Symbol | When to use |
| --- | --- |
| `osal_baremetal_tick()` | Clock and semaphore wakeups only; timers are **not** advanced |
| `osal_baremetal_tick_with_timers()` | Clock, semaphore wakeups, **and** software timer callbacks |

If `osal_baremetal_tick()` is called instead of `osal_baremetal_tick_with_timers()`,
`osal::timer` objects will never fire.  Both symbols are `extern "C"` with
`noexcept`.

---

### Blocking semantics are spin/yield loops

All timed-blocking functions (`mutex::try_lock_for`, `semaphore::take_for`,
`thread::sleep_for`, `memory_pool::allocate_for`, etc.) are implemented as
**busy-yield polling loops** — they call `osal::thread::yield()` in a loop
until the condition is met or the deadline passes.  There is no OS-assisted
sleep.  On real hardware this means the CPU spins at full frequency during a
wait, which increases power consumption.

---

### Maximum task count

The static task pool is `OSAL_BM_MAX_TASKS` (default **8**).  Attempting to
create more tasks returns `error_code::out_of_resources`.  There is no
compile-time guard; the failure is silent until runtime.  Override at the top
of `bare_metal_backend.cpp` if more tasks are needed:

```cpp
#define OSAL_BM_MAX_TASKS 16
```

Similarly, `OSAL_BM_MAX_TIMERS` (default **8**) caps the software timer pool.

---

### Critical section macros

The backend defines default critical sections:

| Target | Macro | Effect |
| --- | --- | --- |
| ARM Cortex-M (`__arm__` or `__ARM_ARCH`) | `cpsid i` / `cpsie i` | Disable/enable all interrupts |
| All other hosts | No-op | No interrupt masking |

The no-op fallback is safe for single-core hosted simulation but **incorrect**
for real multi-core bare-metal targets.  Override before the `#include` that
triggers the backend:

```cpp
#define OSAL_BM_ENTER_CRITICAL()  my_irq_disable()
#define OSAL_BM_EXIT_CRITICAL()   my_irq_enable()
```

---

### Not-supported features

The following always return `error_code::not_supported` on bare-metal:

| Feature | Reason |
| --- | --- |
| `osal::wait_set` | Requires OS-level event demultiplexing (e.g. epoll) |
| `osal_semaphore_give_isr` | ISR-safe semaphore requires IRQ-level context tracking |
| `osal_queue_send_isr` / `receive_isr` | Same |
| `osal_event_flags_set_isr` | Same |
| `osal_work_queue_submit_from_isr` | Same |
| `osal::thread::join_for` (timed join) | No OS join primitive; bare-metal join spins without a deadline |

---

### Emulated primitives on bare-metal

All "advanced" primitives are emulated on top of the bare-metal mutex and
semaphore (which are themselves spin-based):

| Primitive | Implementation |
| --- | --- |
| `condvar` | `emulated_condvar.inl` (Birrell: guard mutex + semaphore array) |
| `work_queue` | `emulated_work_queue.inl` (cooperative thread + ring buffer) |
| `memory_pool` | `emulated_memory_pool.inl` (bitmap + mutex + counting semaphore) |
| `rwlock` | `emulated_rwlock.inl` (mutex + condvar + reader counter) |
| `event_flags` | `emulated_event_flags.inl` (mutex + per-waiter semaphore array) |
| `stream_buffer` | `emulated_stream_buffer.inl` (SPSC ring + binary semaphores) |
| `message_buffer` | `emulated_message_buffer.inl` (length-prefixed SPSC ring) |
