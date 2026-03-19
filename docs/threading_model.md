# MicrOSAL — Threading Model

## Thread Lifecycle

```text
         osal::thread t;
              │
              ▼
         t.create(cfg)        ──► OS creates thread; entry function runs
              │                   asynchronously
              ▼
         [running]
              │
              ├─► t.join()         Block caller until thread exits
              ├─► t.join_for(ms)   Block caller up to N ms
              ├─► t.join_until(tp) Block caller until absolute time point
              └─► t.detach()       Release thread; no join possible
```

When `join()` returns `osal::ok()` the internal handle is cleared and the
`osal::thread` object may be safely destroyed or re-used.

---

## Priority Model

OSAL priority is an unsigned 8-bit value in `[0, 255]`:

| OSAL value | Semantic |
| --- | --- |
| 0 | Lowest / idle |
| 128 (`NORMAL`) | Default application level |
| 255 (`HIGHEST`) | Highest real-time |

Each backend maps this range linearly to its native priority scheme:

- **FreeRTOS** — mapped to `[0, configMAX_PRIORITIES-1]` (inverted scale).
- **ThreadX / PX5** — mapped to `[0, TX_MAX_PRIORITIES-1]` (0 = highest).
- **Zephyr** — mapped to Zephyr preemptive priorities `[zmax, 0]`.
- **POSIX / Linux** — mapped to `SCHED_FIFO` priorities if `priority != NORMAL`;
  otherwise inherits parent scheduling policy.
- **Bare-metal** — priority is ignored; cooperative scheduling only.
- **CMSIS-RTOS** — mapped to `osPriority` enum (7 levels: Idle…Realtime).
- **CMSIS-RTOS2** — mapped to `osPriority_t` enum (56 levels: Idle…Realtime7).

---

## Affinity

Thread pinning is gated behind `capabilities::has_thread_affinity`:

```cpp
if constexpr (osal::active_capabilities::has_thread_affinity) {
    t.set_affinity(0x3); // pin to CPU 0 and CPU 1
}
```

`osal::AFFINITY_ANY` (= `0`) means "no pin; let the scheduler decide."

---

## ISR Safety — Rules

The following functions are safe to call from interrupt context on
**all supporting backends**:

| Function | FreeRTOS | Zephyr | ThreadX | PX5 | POSIX | Linux | BM | C1 | C2 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `osal_semaphore_give_isr()` | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ | ✓ | ✓ |
| `osal_queue_send_isr()` | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ | ✓ | ✓ |
| `osal_queue_receive_isr()` | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ | ✓ | ✓ |
| `osal_event_flags_set_isr()` | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ | ✓ | ✓ |
| `osal_work_queue_submit_from_isr()` | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |

① VxWorks, CMSIS-RTOS: emulated event flags use
  `osal_semaphore_give_isr` to wake waiters from ISR context.

**Thread functions** (`create`, `join`, `yield`, `sleep`) MUST NOT be called
from ISR context.

---

## Memory Model — Guarantee Table

| Primitive | Allocation | Stack owner |
| --- | --- | --- |
| `osal::mutex` | Static pool on static-pool RTOS backends; heap-backed control object on POSIX-family backends (POSIX, Linux, NuttX, QNX, RTEMS, INTEGRITY) | N/A |
| `osal::semaphore` | Static pool on static-pool RTOS backends; heap-backed control object on POSIX-family backends | N/A |
| `osal::queue<T,N>` | Static `alignas(T) uint8_t storage_[N*sizeof(T)]` in object | Object itself |
| `osal::mailbox<T>` | Embedded `queue<T,1>` storage inside the object | Object itself |
| `osal::thread` | Static task/context storage on embedded RTOS backends; heap-backed pthread control object on POSIX-family backends. Embedded backends use caller-managed stack storage; POSIX-family backends may use caller-supplied or native pthread-managed stacks. | Caller / backend |
| `osal::timer` | Static pool on static-pool RTOS backends; heap-backed control object on POSIX-family backends | N/A |
| `osal::event_flags` | Static pool (all backends) | N/A |
| `osal::wait_set` | Heap-backed native wait-set object on POSIX, Linux, RTEMS, and INTEGRITY; native PX5 wait-set; unsupported elsewhere. Use `osal::object_wait_set` for portable OSAL-object waiting. | N/A |
| `osal::barrier` | Heap-backed native pthread barrier on POSIX-family backends with native support; static-pool shared emulated barrier on other backends | N/A |
| `osal::work_queue` | Native object or worker-thread wrapper. POSIX, Linux, RTEMS, and INTEGRITY use a heap-backed pthread worker; OSAL-thread based backends use caller-managed stack storage. | Caller / backend |
| `osal::condvar` | Static pool / emulated object on static-pool RTOS backends; heap-backed native pthread object on POSIX-family backends | N/A |
| `osal::memory_pool` | Static control pool (all backends); **backing buffer is caller-supplied** | Caller |
| `osal::rwlock` | Static pool / emulated object on static-pool RTOS backends; heap-backed native pthread object on POSIX-family backends | N/A |
| `osal::ring_buffer<T,N>` | Fully embedded in the object — **no OS dependency**, no heap, header-only | Object itself |

Notes:

- In FreeRTOS static mode, the first `sizeof(StaticTask_t)` bytes of `thread_config::stack` are used for the task control block; the remaining bytes form the usable task stack.
- In the bare-metal backend, task metadata is stored but no independent stack switch occurs; each task runs on the scheduler's call stack.

## Backend Allocation Profiles

This matrix describes the current MicrOSAL backend implementations, not just the
native RTOS APIs in isolation. Some kernels expose their own compile-time
declaration macros, but the portable MicrOSAL API intentionally normalizes that
into a smaller set of storage models.

| Backend | Current MicrOSAL storage profile | What the application typically supplies | Notes |
| --- | --- | --- | --- |
| FreeRTOS | Static pool + caller-supplied storage by default | Thread / work-queue stack storage; queue, stream-buffer, message-buffer, and memory-pool backing storage | Defining `OSAL_FREERTOS_DYNAMIC_ALLOC` enables FreeRTOS dynamic-allocation paths for task/control objects. |
| Zephyr | Static pool + caller-supplied storage | Thread / work-queue stack storage; queue and memory-pool backing storage | Current backend keeps `k_mutex`, `k_sem`, `k_msgq`, `k_timer`, and related objects in fixed backend pools. |
| ThreadX | Static pool + caller-supplied storage | Thread / work-queue stack storage; queue and memory-pool backing storage | Current backend uses fixed slot arrays for native `TX_*` objects. |
| PX5 | Static pool + caller-supplied storage | Thread / work-queue stack storage; queue and memory-pool backing storage | Same model as ThreadX, with PX5 native wait-set support. |
| Bare-metal | Static pool + caller-supplied storage | Task / work-queue stack storage; queue and memory-pool backing storage | Cooperative scheduler; tasks are tracked in static pools and run on the scheduler call stack. |
| Micrium µC/OS-III | Static pool + caller-supplied storage | Thread stack storage; queue and memory-pool backing storage | Current backend uses fixed `OS_TCB`, `OS_MUTEX`, `OS_SEM`, `OS_Q`, and `OS_TMR` pools. |
| ChibiOS | Static pool + caller-supplied storage | Thread stack storage; mailbox / queue and memory-pool backing storage | Current backend uses fixed pools for threads, mutexes, semaphores, mailboxes, timers, events, and condvars. |
| embOS | Static pool + caller-supplied storage | Thread stack storage; queue and memory-pool backing storage | Current backend uses fixed pools for OS tasks and synchronization objects. |
| VxWorks | Native OS-managed objects | Thread configuration; any payload buffers required by higher-level fixed-storage wrappers | MicrOSAL calls `taskSpawn`, `semMCreate`, `semCCreate`, `msgQCreate`, and related native APIs directly; this is not a caller-supplied static-object model through the portable API. |
| CMSIS-RTOS v1 | Wrapper slot + native OS-managed objects | Depends on the underlying CMSIS port; memory-pool backing storage stays caller-supplied | MicrOSAL keeps fixed tracking slots, but queues/mutexes/timers are created through `os*Create` APIs, so true static declaration is not portable at the MicrOSAL layer. |
| CMSIS-RTOS2 | Wrapper slot + native OS-managed objects | Depends on the underlying CMSIS port; memory-pool backing storage stays caller-supplied | MicrOSAL keeps fixed tracking slots, but queues/mutexes/timers/event flags are created through `os*New` APIs. |
| POSIX | Heap-backed wrapper objects for many control primitives | Optional thread / work-queue stack storage; payload storage for fixed-storage wrappers such as `queue<T,N>` and `memory_pool` | The backend heap-allocates pthread / semaphore / timer / wait-set wrapper objects with `new`, even though several higher-level OSAL containers still embed their own payload storage. |
| Linux | Heap-backed wrapper objects for many control primitives | Optional thread / work-queue stack storage; payload storage for fixed-storage wrappers such as `queue<T,N>` and `memory_pool` | Same model as POSIX, with Linux-specific control objects such as `timerfd` / `epoll` wrappers allocated on the heap. |
| NuttX | Heap-backed wrapper objects for many control primitives | Optional thread stack storage; payload storage for fixed-storage wrappers such as `queue<T,N>` and `memory_pool` | NuttX currently uses the POSIX-style wrapper model in MicrOSAL, even though the RTOS itself is embedded. |
| QNX | Heap-backed wrapper objects for many control primitives | Optional thread stack storage; payload storage for fixed-storage wrappers such as `queue<T,N>` and `memory_pool` | Same general model as the POSIX-family backends. |
| RTEMS | Heap-backed wrapper objects for many control primitives | Optional thread stack storage; payload storage for fixed-storage wrappers such as `queue<T,N>` and `memory_pool` | RTEMS currently builds through the shared POSIX-family backend implementation. |
| INTEGRITY | Heap-backed wrapper objects for many control primitives | Optional thread stack storage; payload storage for fixed-storage wrappers such as `queue<T,N>` and `memory_pool` | INTEGRITY currently builds through the shared POSIX-family backend implementation. |

Regardless of backend, the following APIs are intentionally fixed-storage at
the C++ surface:

- `osal::queue<T,N>` and `osal::mailbox<T>`
- `osal::ring_buffer<T,N>`
- `osal::stream_buffer<N>` and `osal::message_buffer<N>`
- `osal::memory_pool` backing storage
- `osal::notification<Slots>`

On POSIX-family backends that does not mean the entire implementation is heap
free; it means the payload storage is caller-owned while the backend may still
allocate a small control object behind the handle.

### Queue storage layout

`queue<T,N>` embeds its ring buffer directly:

```cpp
alignas(T) uint8_t storage_[N * sizeof(T)];
```

This means the entire queue lives in the object (typically on the stack or in
BSS).  Items are copied in/out via `std::memcpy` — therefore `T` must satisfy:

- `std::is_trivially_copyable<T>`
- `std::is_standard_layout<T>`
- `N > 0`

These constraints are enforced via `static_assert` in `queue.hpp`.

---

## Cooperative Scheduling (Bare-metal)

The bare-metal backend implements a cooperative round-robin scheduler.
There is no pre-emption.

### Tick source

The application's SysTick (or equivalent) handler MUST call ONE of:

```cpp
extern "C" void osal_baremetal_tick();               // clock only
extern "C" void osal_baremetal_tick_with_timers();   // clock + timers
```

A typical ARM Cortex-M integration:

```cpp
extern "C" void SysTick_Handler() {
    osal_baremetal_tick_with_timers();
}
```

Hosted doctest builds for the bare-metal backend enable a test-only synthetic
tick/context helper so the suite can run under `ctest` without an external ISR.
Production bare-metal builds do not enable that helper; application code must
still drive one of the tick hooks above.

### Yielding

Tasks must call `osal_thread_yield()` (or sleep) periodically to allow other
tasks to run:

```cpp
void my_task(void*) {
    while (true) {
        do_some_work();
        osal_thread_yield();  // cooperate
    }
}
```

`osal_thread_sleep_ms(N)` busy-waits by calling `osal_thread_yield()` in a
loop until `N` ticks have elapsed.

### Limitations

- No pre-emption; starvation is possible if a task never yields.
- Stack switching is **not** performed — each task runs on the call stack of
  the initial scheduler invocation.  For independent stacks, use a proper
  port with context-switch assembly.
- Priority is ignored; tasks are scheduled in creation order.
- Affinity is not supported.

---

## Thread Stack Sizing

As a baseline, the following minimum stack sizes are recommended:

| Use case | Min stack (bytes) |
| --- | --- |
| Simple compute loop, no I/O | 512 |
| Uses `printf` / `std::ostream` | 2048 |
| Uses C++ exceptions (not OSAL!) | 4096 |
| Deeply recursive algorithms | 8192+ |

Guard pattern: the bare-metal backend writes `0xDEADBEEF` to the stack base
during initialisation for manual overflow detection.

---

## Common Pitfalls

1. **Calling `join()` from the thread itself** — deadlock.  Check the handle
   before calling join.

2. **Destroying an `osal::mutex` while locked** — undefined behaviour on all
   backends.  Always `unlock()` before `destroy()`.

3. **ISR gives semaphore faster than consumer takes** — the semaphore count
   will climb until the next `take()`.  Size counting semaphores by the
   maximum burst depth.

4. **Queue full in ISR** — `send_isr` returns `would_block`; the ISR cannot
   block.  Size the queue appropriately or discard with a counter.

5. **Using `queue<T,N>` with non-trivially-copyable T** — compile error via
   `static_assert`.  Wrap the non-trivial type in a pointer or use an index.
