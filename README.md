# MicrOSAL

**Micro Operating System Abstraction Layer** — a thin, zero-overhead C++17
interface to common RTOS primitives, portable across fifteen embedded and
hosted operating systems.

- No virtual functions — all backend dispatch is resolved at compile time.
- No RTTI — compiles cleanly with `-fno-rtti`.
- No dynamic allocation in primitives — static pools on embedded targets.
- `noexcept` everywhere — errors returned via `osal::result`, never thrown.
- Single backend selected at compile time; zero dead code for the others.

```cpp
#include <osal/osal.hpp>      // or include individual headers

osal::mutex          mtx;
osal::semaphore      sem{osal::semaphore_type::binary, 0U};
osal::queue<int, 8>  q;
osal::timer          t{timer_cfg};
osal::thread         worker{thread_cfg};

{
    osal::mutex::lock_guard lg{mtx};
    // critical section
}

auto rc = sem.take_for(std::chrono::milliseconds{100});
if (rc != osal::error_code::ok) { /* timed out */ }
```

---

## Backends

| Backend constant | RTOS / OS |
| --- | --- |
| `OSAL_BACKEND_FREERTOS` | FreeRTOS ≥ 10.0 |
| `OSAL_BACKEND_ZEPHYR` | Zephyr RTOS ≥ 3.0 |
| `OSAL_BACKEND_THREADX` | Azure RTOS ThreadX |
| `OSAL_BACKEND_PX5` | PX5 RTOS (ThreadX superset) |
| `OSAL_BACKEND_POSIX` | Any POSIX:2008 system |
| `OSAL_BACKEND_LINUX` | Linux ≥ 4.11 (epoll, timerfd, futex) |
| `OSAL_BACKEND_BAREMETAL` | No OS — cooperative, single-core |
| `OSAL_BACKEND_VXWORKS` | VxWorks 7 |
| `OSAL_BACKEND_NUTTX` | Apache NuttX ≥ 12 |
| `OSAL_BACKEND_MICRIUM` | Micrium µC/OS-III |
| `OSAL_BACKEND_CHIBIOS` | ChibiOS/RT ≥ 21 |
| `OSAL_BACKEND_EMBOS` | SEGGER embOS |
| `OSAL_BACKEND_QNX` | QNX Neutrino RTOS |
| `OSAL_BACKEND_CMSIS_RTOS` | ARM CMSIS-RTOS v1 |
| `OSAL_BACKEND_CMSIS_RTOS2` | ARM CMSIS-RTOS2 |

---

## Primitives

| Header | Primitive | Description |
| --- | --- | --- |
| `mutex.hpp` | `osal::mutex` | Recursive or non-recursive mutex with timed lock |
| `semaphore.hpp` | `osal::semaphore` | Binary or counting semaphore; ISR-safe give |
| `queue.hpp` | `osal::queue<T, N>` | Fixed-capacity FIFO message queue; ISR-safe send/receive |
| `timer.hpp` | `osal::timer` | One-shot or periodic software timer |
| `thread.hpp` | `osal::thread` | RTOS task/thread with priority, affinity, stack config |
| `condvar.hpp` | `osal::condvar` | Condition variable (native or emulated) |
| `event_flags.hpp` | `osal::event_flags` | 32-bit event flag group (native or emulated) |
| `rwlock.hpp` | `osal::rwlock` | Reader/writer lock with RAII guards |
| `stream_buffer.hpp` | `osal::stream_buffer` | Byte-stream ring buffer; ISR-safe on FreeRTOS/Zephyr |
| `message_buffer.hpp` | `osal::message_buffer` | Framed message buffer built on stream_buffer |
| `memory_pool.hpp` | `osal::memory_pool` | Fixed-block memory pool (native or emulated) |
| `ring_buffer.hpp` | `osal::ring_buffer<T, N>` | Header-only SPSC ring buffer, backend-independent |
| `work_queue.hpp` | `osal::work_queue` | Task-context deferred work queue |
| `thread_local_data.hpp` | `osal::thread_local_data` | Per-thread key/value storage |
| `wait_set.hpp` | `osal::wait_set` | Multi-object wait (Linux epoll; `not_supported` elsewhere) |
| `clock.hpp` | Free functions | `osal_clock_monotonic_ms`, `_ticks`, `_tick_period_us` |
| `osal_c.h` | C API | Pure-C wrappers for all primitives |

---

## Building

### Standalone CMake (Linux, POSIX, FreeRTOS, …)

```bash
cmake -B build \
      -DOSAL_BACKEND=LINUX \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Supported `-DOSAL_BACKEND` values: `FREERTOS ZEPHYR THREADX PX5 POSIX LINUX
BAREMETAL VXWORKS NUTTX MICRIUM CHIBIOS EMBOS QNX CMSIS_RTOS CMSIS_RTOS2`

For RTOS backends that require external headers:

```bash
cmake -B build \
      -DOSAL_BACKEND=FREERTOS \
      -DOSAL_RTOS_INCLUDE_DIR=/path/to/FreeRTOS-Kernel/include
cmake --build build -j$(nproc)
```

`find_package(microsal)` is supported after install; the library exports the
`microsal` CMake target.

### Zephyr (west)

Place or symlink the repository into the Zephyr workspace modules directory:

```bash
ln -sfn /path/to/micrOSAL zephyrproject/modules/lib/microsal
```

Then build any application with the module enabled:

```bash
west build -b native_sim path/to/app \
    -- -DEXTRA_ZEPHYR_MODULES=/path/to/micrOSAL
```

Kconfig options are exposed under `CONFIG_MICRO_OSAL*`.

### Apache NuttX

Symlink (or clone) the test app into the NuttX apps tree and enable it via
`CONFIG_TESTING_MICROSAL_TEST=y`.  See [`docs/TestCoverage.md`](docs/TestCoverage.md)
for the full build recipe.

---

## Running the tests

### Linux / POSIX / Bare-metal (doctest, CTest)

```bash
cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### FreeRTOS v11 (POSIX simulation, no emulator)

```bash
cmake -B build-freertos tests/freertos -DCMAKE_BUILD_TYPE=Debug
cmake --build build-freertos -j$(nproc)
ctest --test-dir build-freertos --output-on-failure
```

FreeRTOS-Kernel v11 and doctest are fetched automatically via CMake
`FetchContent`.

### Zephyr (west twister)

```bash
source /path/to/zephyr-env.sh
west twister -T tests/zephyr -p native_sim \
    --extra-args="EXTRA_ZEPHYR_MODULES=/path/to/micrOSAL"
```

### NuttX sim/nsh

```bash
# With the symlink already in place (see Building → Apache NuttX):
printf 'microsal_test\nexit\n' | timeout 120 ./nuttx/build/nuttx
```

---

## Test coverage

| Backend | Test suite | Runner | Status |
| --- | --- | --- | --- |
| Linux | CTest / doctest | Native process | ✅ 19/19 |
| POSIX | CTest / doctest | Native process | ✅ 19/19 |
| Bare-metal | CTest / doctest | Native process | ✅ 19/19 |
| FreeRTOS v11 | CTest / doctest | POSIX sim | ✅ 32/32 |
| NuttX (latest main) | printf framework | sim/nsh on x86-64 | ✅ 26/26 |
| Zephyr (`native_sim`) | west twister / ztest | Native process | ✅ 150/150 |
| Zephyr (`nrf52840dk`) | west twister / ztest | Renode v1.15.3 | ✅ 79/79 |

See [`docs/TestCoverage.md`](docs/TestCoverage.md) for the full primitive
coverage matrix and gap analysis.

---

## Documentation

| Document | Contents |
| --- | --- |
| [`docs/design.md`](docs/design.md) | Architecture, design principles, config/data split, capability matrix |
| [`docs/backend_integration.md`](docs/backend_integration.md) | How to port MicrOSAL to a new backend |
| [`docs/TestCoverage.md`](docs/TestCoverage.md) | Test suite inventory and coverage matrix |
| [`docs/threading_model.md`](docs/threading_model.md) | Thread lifecycle, priorities, and scheduling model |
| [`docs/c_api.md`](docs/c_api.md) | Pure-C API reference |

API documentation can be generated with Doxygen:

```bash
doxygen Doxyfile
# output → docs/doxygen/html/index.html
```

---

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

> **AI generation disclosure:** a significant portion of this codebase was
> substantially generated with AI coding assistance (GitHub Copilot and similar
> tools). The human author directed the design, specification, review, curation,
> and testing of all AI-generated content. See [`NOTICE`](NOTICE) for the full
> disclosure and intellectual property notices.
