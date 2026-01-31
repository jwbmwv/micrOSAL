# C Interface Layer

MicrOSAL's primary API is C++17, but a pure-C interface is provided so that
C modules (drivers, legacy code, mixed-language projects) can use the same
OSAL primitives without a C++ compiler.

---

## Files

| File | Purpose |
| --- | --- |
| `include/osal/osal_c.h` | Pure C11 header — types, constants, functions |
| `src/common/osal_c.cpp` | C++17 bridge — thin wrappers → backend calls |

`osal_c.h` is self-contained; it does **not** `#include` any C++ headers.
Both C and C++ translation units can include it (guarded by `extern "C"`).

---

## Design

### Handle layout compatibility

Every C handle (`osal_mutex_handle`, `osal_semaphore_handle`, …) is:

```c
typedef struct { void* native; } osal_mutex_handle;
```

The C++ handles (`osal::active_traits::mutex_handle_t`, …) have the
identical layout.  The bridge implementation `reinterpret_cast`s between
the two — no copying, no allocation.

`osal_notification_handle` and `osal_delayable_work_handle` are the
intentional exceptions. `osal_notification_handle` is a composite pure-C helper
struct built from existing C `mutex`/`condvar` handles plus caller-supplied
slot state. `osal_delayable_work_handle` is a composite helper built from a
`timer`, `mutex`, `work_queue`, and inline dispatch state, because there is no
native backend handle to cast to for these portable higher-level helpers.

### Error code mapping

The C return type is `osal_result_t` (`int32_t`).  Values match the C++
`osal::error_code` enum one-to-one:

| C constant | Value | C++ equivalent |
| --- | --- | --- |
| `OSAL_OK` | 0 | `osal::error_code::ok` |
| `OSAL_TIMEOUT` | 1 | `osal::error_code::timeout` |
| `OSAL_WOULD_BLOCK` | 2 | `osal::error_code::would_block` |
| `OSAL_INVALID_ARGUMENT` | 3 | `osal::error_code::invalid_argument` |
| `OSAL_NOT_SUPPORTED` | 4 | `osal::error_code::not_supported` |
| `OSAL_OUT_OF_RESOURCES` | 5 | `osal::error_code::out_of_resources` |
| `OSAL_PERMISSION_DENIED` | 6 | `osal::error_code::permission_denied` |
| `OSAL_ALREADY_EXISTS` | 7 | `osal::error_code::already_exists` |
| `OSAL_NOT_INITIALIZED` | 8 | `osal::error_code::not_initialized` |
| `OSAL_OVERFLOW` | 9 | `osal::error_code::overflow` |
| `OSAL_UNDERFLOW` | 10 | `osal::error_code::underflow` |
| `OSAL_DEADLOCK_DETECTED` | 11 | `osal::error_code::deadlock_detected` |
| `OSAL_NOT_OWNER` | 12 | `osal::error_code::not_owner` |
| `OSAL_ISR_INVALID` | 13 | `osal::error_code::isr_invalid` |
| `OSAL_UNKNOWN` | 255 | `osal::error_code::unknown` |

### Naming convention

C functions use the `osal_c_` prefix (e.g. `osal_c_mutex_create`) to
distinguish them from the low-level backend functions (`osal_mutex_create`),
which have C linkage but take C++ parameter types.

---

## Covered Primitives

| Primitive | C functions |
| --- | --- |
| **Clock** | `osal_c_clock_monotonic_ms`, `_system_ms`, `_ticks`, `_tick_period_us` |
| **Mutex** | `osal_c_mutex_create`, `_create_with_cfg`, `_destroy`, `_lock`, `_try_lock`, `_unlock` |
| **Semaphore** | `osal_c_semaphore_create`, `_create_with_cfg`, `_destroy`, `_give`, `_give_isr`, `_take`, `_try_take` |
| **Queue** | `osal_c_queue_create`, `_create_with_cfg`, `_destroy`, `_send`, `_send_isr`, `_receive`, `_receive_isr`, `_peek`, `_count`, `_free` |
| **Mailbox** | No dedicated C handle; use `osal_c_queue_*` with `capacity = 1` |
| **Thread** | `osal_c_thread_create`, `_create_with_cfg`, `_join`, `_detach`, `_set_priority`, `_set_affinity`, `_suspend`, `_resume`, `_yield`, `_sleep_ms` |
| **Timer** | `osal_c_timer_create`, `_create_with_cfg`, `_destroy`, `_start`, `_stop`, `_reset`, `_set_period`, `_is_active` |
| **Event Flags** | `osal_c_event_flags_create`, `_destroy`, `_set`, `_clear`, `_get`, `_wait_any`, `_wait_all`, `_set_isr` |
| **Condvar** | `osal_c_condvar_create`, `_destroy`, `_wait`, `_notify_one`, `_notify_all` |
| **Notification** | `osal_c_notification_create`, `_create_with_cfg`, `_destroy`, `_notify`, `_wait`, `_clear`, `_reset`, `_pending`, `_peek` |
| **Work Queue** | `osal_c_work_queue_create`, `_create_with_cfg`, `_destroy`, `_submit`, `_submit_from_isr`, `_flush`, `_cancel_all`, `_pending` |
| **Delayable Work** | `osal_c_delayable_work_create`, `_create_with_cfg`, `_destroy`, `_schedule`, `_reschedule`, `_cancel`, `_flush`, `_scheduled`, `_pending`, `_running` |
| **Memory Pool** | `osal_c_memory_pool_create`, `_create_with_cfg`, `_destroy`, `_allocate`, `_allocate_timed`, `_deallocate`, `_available` |
| **Read-Write Lock** | `osal_c_rwlock_create`, `_destroy`, `_read_lock`, `_read_unlock`, `_write_lock`, `_write_unlock` |

**Not covered:**

- `wait_set` — its C++ API uses templates and variadic packs that don't
    translate well to C.
- `object_wait_set` — the current portable API is C++-only and object-typed.

---

## Notification and Delayable Work in C

The C bridge exposes the same portable higher-level helpers that were added to
the C++ API:

- `osal_c_notification_*` implements indexed 32-bit notification slots with
    `OVERWRITE`, `NO_OVERWRITE`, `SET_BITS`, and `INCREMENT` actions.
- `osal_c_delayable_work_*` implements one-shot delayed dispatch onto an
    existing `osal_c_work_queue`.

Both remain allocation-free:

- `osal_c_notification_create` takes caller-supplied `values[]` and
    `pending[]` arrays.
- `osal_c_delayable_work_create` stores its timer/mutex/dispatch state directly
    inside the handle and targets a caller-managed work queue.
- On backends whose timer callbacks may run in ISR context, C delayable work
    requires ISR-safe `osal_c_work_queue_submit_from_isr` support.

### C notification example

```c
uint32_t values[2] = {0};
uint8_t pending[2] = {0};
osal_notification_handle note;

osal_c_notification_create(&note, values, pending, 2);
osal_c_notification_notify(&note, 0x1234U, OSAL_NOTIFICATION_OVERWRITE, 1);

uint32_t out = 0;
osal_c_notification_wait(&note, 1, &out, 0U, 0xFFFFFFFFU, OSAL_NO_WAIT);
/* out == 0x1234U */

osal_c_notification_destroy(&note);
```

### C delayable-work example

```c
static uint8_t wq_stack[4096];

static void delayed_cb(void* arg)
{
        (void)arg;
}

osal_work_queue_handle wq;
osal_delayable_work_handle work;

osal_c_work_queue_create(&wq, wq_stack, sizeof(wq_stack), 8, "c_wq");
osal_c_delayable_work_create(&work, &wq, delayed_cb, NULL, "c_dw");
osal_c_delayable_work_schedule(&work, 10);
osal_c_delayable_work_flush(&work, OSAL_WAIT_FOREVER);
osal_c_delayable_work_destroy(&work);
osal_c_work_queue_destroy(&wq);
```

---

## Thread-Local Data (C API)

The C interface now includes TLS key/value functions:

```c
typedef struct {
        uint8_t key;
        uint8_t valid;
} osal_tls_key_handle;

osal_result_t osal_c_tls_key_create(osal_tls_key_handle* handle);
osal_result_t osal_c_tls_key_destroy(osal_tls_key_handle* handle);
osal_result_t osal_c_tls_set(osal_tls_key_handle* handle, void* value);
void*         osal_c_tls_get(const osal_tls_key_handle* handle);
```

Behavior mirrors the C++ emulation:

- Key acquisition/release with bounded key count.
- Per-thread isolation for stored pointers.
- Safe invalidation on key reuse (generation tracking).
- Calling `osal_c_tls_get` on an invalid key returns `NULL`.
- Calling `osal_c_tls_set` / `osal_c_tls_key_destroy` on an invalid key returns `OSAL_NOT_INITIALIZED`.

### C usage example

```c
osal_tls_key_handle tls;
if (osal_c_tls_key_create(&tls) == OSAL_OK)
{
    int worker_id = 7;
    osal_c_tls_set(&tls, &worker_id);

    int* my_id = (int*)osal_c_tls_get(&tls);
    /* my_id is thread-local; each thread sees its own value */

    osal_c_tls_key_destroy(&tls);
}
```

---

## Config Structs (FLASH-Friendly Creation)

Each primitive has a companion `osal_*_config` struct that bundles all
creation-time parameters.  Declare them as `const` so the linker places
them in `.rodata` (FLASH):

```c
/* Lives in FLASH — only the handle is in RAM. */
static const osal_semaphore_config sem_cfg = {
    .initial_count = 0,
    .max_count     = 5
};

osal_semaphore_handle sem;
osal_c_semaphore_create_with_cfg(&sem, &sem_cfg);
```

| Config struct | `_create_with_cfg` function |
| --- | --- |
| `osal_mutex_config` | `osal_c_mutex_create_with_cfg` |
| `osal_semaphore_config` | `osal_c_semaphore_create_with_cfg` |
| `osal_queue_config` | `osal_c_queue_create_with_cfg` |
| `osal_thread_config` | `osal_c_thread_create_with_cfg` |
| `osal_timer_config` | `osal_c_timer_create_with_cfg` |
| `osal_work_queue_config` | `osal_c_work_queue_create_with_cfg` |
| `osal_memory_pool_config` | `osal_c_memory_pool_create_with_cfg` |

The original positional `_create` functions remain unchanged for backward
compatibility.

---

## Usage Example (C)

```c
#include <osal/osal_c.h>

void producer(void* arg)
{
    osal_queue_handle* q = (osal_queue_handle*)arg;
    int val = 42;
    osal_c_queue_send(q, &val, OSAL_WAIT_FOREVER);
}

int main(void)
{
    osal_queue_handle q;
    osal_c_queue_create(&q, NULL, sizeof(int), 8);

    osal_thread_handle thr;
    osal_c_thread_create(&thr, producer, &q,
                         OSAL_PRIORITY_NORMAL, 0,
                         NULL, 0, "prod");

    int out;
    osal_c_queue_receive(&q, &out, OSAL_WAIT_FOREVER);
    /* out == 42 */

    osal_c_thread_join(&thr, OSAL_WAIT_FOREVER);
    osal_c_queue_destroy(&q);
    return 0;
}
```

## Build Integration

The C bridge (`src/common/osal_c.cpp`) is compiled automatically as part of
the `microsal` static library.  C source files only need to include the
header and link against `microsal`:

```cmake
add_executable(my_c_app main.c)
target_link_libraries(my_c_app PRIVATE microsal)
```

For Zephyr builds the bridge source is added to the `zephyr_library_sources`
list automatically.
