# MicrOSAL — Test Coverage

This document catalogues what is and is not tested.  It is updated manually
whenever new tests are added.  Three independent test suites exist:

- **Linux/doctest suite** — 21 test binaries built with CMake, run via CTest; targets the Linux backend.
- **FreeRTOS/doctest suite** — single `tests/freertos/src/main.cpp`, built with CMake, run via
  CTest; uses the FreeRTOS-Kernel v11 POSIX simulation port (runs as a Linux process, no emulator
  required).
- **NuttX/doctest suite** — `tests/nuttx/src/main.cpp` built as a NuttX builtin application;
  runs inside the NuttX sim/nsh simulator on x86-64 Linux.
- **Zephyr/ztest suite** — single `tests/zephyr/src/main.cpp`, built with `west twister`;
  targets the Zephyr backend on `native_sim` (Linux process) and `nrf52840dk/nrf52840`
  (Renode SoC simulation).

---

## Backend under test

Five hosted build targets are verified automatically in CI:

| Backend | Test suite | Emulator / runner | Coverage |
| --- | --- | --- | --- |
| Linux | CMake / CTest (doctest) | None (native process) | ✅ 21 binaries, 21/21 pass |
| POSIX | CMake / CTest (doctest) | None (native process) | ✅ in CI (`OSAL_BACKEND=POSIX`); same 21 binaries |
| Bare-metal | CMake / CTest (doctest) | None (compile+run) | ✅ in CI (`OSAL_BACKEND=BAREMETAL`) |
| FreeRTOS v11 | CMake / CTest (doctest) | POSIX sim port (no emulator) | ✅ in CI (`freertos-test` job); **32/32 pass** |
| NuttX (latest main) | NuttX builtin app (printf framework) | NuttX sim/nsh on x86-64 | ✅ in CI; **26/26 pass** |
| Zephyr (`native_sim`) | west twister (ztest) | Native Linux process | ✅ 75 cases × 2 configs = 150/150 pass |
| Zephyr (`nrf52840dk`) | west twister (ztest + Renode) | **Renode** v1.15.3 SoC simulation | ✅ **79/79 pass** on nrf52840dk/nrf52840 (Zephyr v4.3.0) |
| ThreadX / PX5 | — | — | ❌ Not in CI |
| QNX | — | — | ❌ Not in CI |
| VxWorks | — | — | ❌ Not in CI |
| All others | — | — | ❌ Not in CI |

### Running the Zephyr suite

```bash
# native_sim (fastest)
source /path/to/zephyr-rtos/zephyr-env.sh
west twister -T tests/zephyr -p native_sim \
    --extra-args="EXTRA_ZEPHYR_MODULES=/path/to/micrOSAL"

# nRF52840-DK via Renode (requires Renode portable in PATH)
# 1. Build the test image with twister:
west twister -T tests/zephyr -p nrf52840dk/nrf52840 --build-only \
    --extra-args="EXTRA_ZEPHYR_MODULES=/path/to/micrOSAL" \
    --outdir twister-renode-out
# 2. Run the ELF directly in Renode:
ELF=twister-renode-out/nrf52840dk_nrf52840/zephyr/micrOSAL/tests/zephyr/microsal.ztest.renode_nrf52840/zephyr/zephyr.elf
renode --disable-xwt -e "
  mach create
  machine LoadPlatformDescription @platforms/cpus/nrf52840.repl
  uart0 CreateFileBackend @/tmp/renode_uart.log true
  sysbus LoadELF @${ELF}
  start
  sleep 120
  quit"
cat /tmp/renode_uart.log
```

### Running the FreeRTOS suite

```bash
# FreeRTOS-Kernel v11 is fetched automatically via CMake FetchContent.
cmake -B build-freertos tests/freertos -DCMAKE_BUILD_TYPE=Debug
cmake --build build-freertos -j$(nproc)
ctest --test-dir build-freertos --output-on-failure
```

### Running the NuttX suite

The NuttX test app lives in `tests/nuttx/`.  It is designed to be placed
(or symlinked) into `nuttx-apps/testing/microsal_test/` of a NuttX workspace:

```bash
git clone https://github.com/apache/nuttx.git
git clone https://github.com/apache/nuttx-apps.git
ln -s /path/to/micrOSAL/tests/nuttx nuttx-apps/testing/microsal_test

# Download doctest single-header into the vendor directory
mkdir -p nuttx-apps/testing/microsal_test/vendor/doctest/doctest
wget https://github.com/doctest/doctest/releases/download/v2.4.11/doctest.h \
    -O nuttx-apps/testing/microsal_test/vendor/doctest/doctest/doctest.h

# Configure and build
cd nuttx
cmake -B build -DBOARD_CONFIG=sim/nsh -DNUTTX_APPS_DIR=../nuttx-apps
echo 'CONFIG_TESTING_MICROSAL_TEST=y' >> build/.config
cmake build && cmake --build build -j$(nproc)

# Run — feed the test command to NSH
printf 'microsal_test\nexit\n' | ./build/nuttx
```

---

## Primitive coverage matrix

Legend:

- ✅ Test file exists, happy-path covered
- ⚠️ Partial — some cases missing (see notes)
- ❌ Not tested

| Primitive | Test file | Status | Notable gaps |
| --- | --- | --- | --- |
| `osal::clock` | `test_clock.cpp` | ✅ | `osal_clock_tick_period_us` not asserted |
| `osal::mutex` | `test_mutex.cpp` | ✅ | `try_lock_for` timeout path not tested |
| `osal::semaphore` | `test_semaphore.cpp` | ⚠️ | `give_isr` / `take_isr` not called |
| `osal::thread` | `test_thread.cpp` | ✅ | Affinity not tested; task-notification native path (FreeRTOS/embOS) not in CI — stub (`not_supported`) path covered |
| `osal::queue` | `test_queue.cpp` | ⚠️ | `send_isr` / `receive_isr` not tested; overflow behaviour not asserted |
| `osal::timer` | `test_timer.cpp` | ✅ | One-shot and periodic both covered |
| `osal::event_flags` | `test_event_flags.cpp` | ⚠️ | `set_isr` not tested; WAIT_ALL timeout not tested |
| `osal::wait_set` | `test_wait_set.cpp` | ✅ | Linux epoll path (Linux suite); `not_supported` path asserted on Zephyr |
| `osal::condvar` | `test_condvar.cpp` | ✅ | `wait_for` timeout, predicate-wait + cross-thread notify covered |
| `osal::work_queue` | `test_work_queue.cpp` | ✅ | `submit_from_isr` not tested (returns `not_supported` on Linux) |
| `osal::stream_buffer` | `test_stream_buffer.cpp` | ✅ | `send_isr`/`receive_isr` called from task context; trigger-level 1 and 4 tested; cross-thread blocking send/receive covered |
| `osal::message_buffer` | `test_message_buffer.cpp` | ⚠️ | ISR send/receive not tested; oversized-message truncation not tested |
| `osal::rwlock` | `test_rwlock.cpp` | ✅ | RAII guards, concurrent readers, timed-lock success+timeout covered on both Linux and Zephyr |
| `osal::spinlock` | `test_spinlock.cpp` | ✅ | construction, lock/unlock, try_lock, lock_guard RAII, sequential cycles; native path only on Zephyr — stub (`not_supported`) path fully exercised on Linux |
| `osal::barrier` | `test_barrier.cpp` | ✅ | construction, stub path, count-1 immediate release, two-thread rendezvous, serial-thread identification; native `pthread_barrier_t` path exercised on Linux/POSIX |
| `osal::memory_pool` | `test_memory_pool.cpp` | ✅ | Exhaustion, `allocate_for` timeout, cross-thread unblock covered |
| `osal::ring_buffer` | `test_ring_buffer.cpp` | ✅ | Wrap-around, SPSC producer/consumer stress, capacity-1 covered |
| `osal::thread_local_data` | `test_thread_local_data.cpp` | ✅ | Per-thread isolation, key reuse, key exhaustion covered |
| `result` / `error_code` | `test_result.cpp` | ✅ | All error codes and comparison operators covered |
| C API (`osal_c.h`) | `test_c_api.cpp` | ⚠️ | C11 compilation verified; ISR variants not called from C |
| Integration (multi-primitive) | `test_integration.cpp` | ⚠️ | Producer/consumer with queue+semaphore; no rwlock or memory_pool integration |
| Thread task notification | `test_thread.cpp` | ⚠️ | Stub path (`not_supported`) verified on Linux; native `xTaskNotify` (FreeRTOS) and `OS_TASKEVENT` (embOS) round-trip covered in concept but not yet wired into the hosted CI suites |

---

## Zephyr test suite (`tests/zephyr/src/main.cpp`)

75–79 test cases across 18 ztest suites (count varies by platform — some stream_buffer
ISR and trigger-level tests only compile on real targets, not on `native_sim`).  Verified on two platforms:

- **`native_sim`** (Zephyr v4.3.0) — 75 cases × 2 configs (generic + `native_sim`) → **150/150 pass**.
- **`nrf52840dk/nrf52840`** (Zephyr v4.3.0, **Renode** v1.15.3) — 79 test cases → **79/79 pass**.

| ztest suite | Cases | What is covered |
| --- | --- | --- |
| `osal_clock` | 2 | Monotonic clock positive, tick counter non-negative |
| `osal_mutex` | 6 | Construction, lock/unlock, try_lock, recursive, config, lock_guard |
| `osal_semaphore` | 6 | Binary, counting, initial count, config, `take_for` timeout |
| `osal_queue` | 5 | Construction, starts empty, send/receive, FIFO order, full detection |
| `osal_thread` | 4 | Default invalid, create+join, yield, `sleep_for` elapsed check |
| `osal_timer` | 4 | Construction, one-shot fires once, periodic fires ≥2×, config |
| `osal_event_flags` | 4 | Construction, set+get, clear, `wait_any` immediate |
| `osal_condvar` | 4 | Construction, notify-only, cross-thread wait+notify, `wait_for` timeout |
| `osal_work_queue` | 3 | Construction, submit+flush, config |
| `osal_memory_pool` | 2 | Alloc/free round-trip + count, config construction |
| `osal_rwlock` | 5 | Basic lock/unlock, concurrent readers, `write_lock_for`, RAII guards |
| `osal_stream_buffer` | 4 (`native_sim`) / 8 (`nrf52840dk`) | Construction, send/receive, empty receive = 0, reset; + `send_isr`/`receive_isr` round-trip, `receive_isr` empty = 0, trigger-level below/at threshold (nrf52840dk only) |
| `osal_message_buffer` | 4 | Construction, send/receive, FIFO ordering, reset |
| `osal_c_api` | 5 | Mutex, semaphore, config construction, stream\_buffer, message\_buffer C round-trips |
| `osal_integration` | 4 | Cross-thread: semaphore signal, event\_flags set, stream\_buffer, message\_buffer |
| `osal_ring_buffer` | 6 | Construction, push/pop, FIFO order, full detection, peek, reset |
| `osal_thread_local_data` | 4 | Construction, initial nullptr, set/get, per-thread isolation |
| `osal_wait_set` | 3 | Valid in emulated mode, `add` → `not_supported`, `wait` → `not_supported` |

**Not covered** by the Zephyr suite: `result`/`error_code` smoke test,
timed-lock timeout paths for mutex and queue, ISR variants (FreeRTOS-specific
`_isr` APIs are now tested in the FreeRTOS doctest suite instead).

---

## FreeRTOS test suite (`tests/freertos/src/main.cpp`)

Built with CMake + FreeRTOS-Kernel v11 (GCC/POSIX simulation port).
Runs as a standard Linux process via CTest — no emulator required.
FreeRTOS-Kernel and doctest are fetched automatically by FetchContent.

| Primitive | Cases | What is covered |
| --- | --- | --- |
| `osal::mutex` | 4 | lock/unlock, try_lock, timed-lock timeout, lock_guard |
| `osal::semaphore` | 4 | binary give/take, counting, take_for timeout, cross-thread signal |
| `osal::queue` | 3 | send/receive, FIFO order, full detection |
| `osal::timer` | 2 | one-shot fires once, periodic fires ≥4× |
| `osal::event_flags` | 3 | set+get, wait_any immediate, cross-thread set |
| `osal::stream_buffer` | 4 | construction, send/receive round-trip, full rejection, reset, `send_isr`/`receive_isr` |
| `osal::message_buffer` | 2 | send/receive, FIFO order |
| `osal::ring_buffer` | 1 | push/pop FIFO (header-only, backend-independent) |
| `osal::thread` | 2 | create+join, sleep_for elapsed |
| `osal::condvar` | 1 | cross-thread wait+notify |
| `osal::thread_local_data` | 1 | per-thread isolation |
| `osal::memory_pool` | 2 | alloc/free, exhaustion → nullptr |
| `result`/`error_code` | 1 | ok vs error, code() accessor |

**Notable gaps:** ISR variants exercised only lightly (FreeRTOS POSIX sim uses
the task-context fallback for `send_isr`/`receive_isr`); no `set_isr` for
event_flags (FreeRTOS ISR APIs require a real ISR context).

**Backend fixes uncovered by this suite:**

- `freertos_backend.cpp`: `struct cb_pair` / `static pairs[]` were local to `osal_timer_create`;
  `osal_timer_destroy` could not cast the ID pointer. Fixed by hoisting `fr_cb_pair` and
  `fr_cb_pairs[]` to file scope (also fixed undefined `OSAL_FREERTOS_MAX_TIMERS` when
  `OSAL_FREERTOS_DYNAMIC_ALLOC` is defined).
- `freertos_backend.cpp`: `osal_thread_join` called `vTaskDelete` immediately without waiting
  for task completion. Fixed by introducing `fr_thread_ctx_t` (dynamic-alloc path) that wraps
  the user entry function and signals a binary semaphore on return; join waits on that semaphore.
- `freertos_backend.cpp`: `osal_message_buffer_available` returned `0` or `1` (boolean) instead
  of the actual next-message size. Fixed to use `xStreamBufferNextMessageLengthBytes`.

---

## NuttX test suite (`tests/nuttx/src/main.cpp`)

Built as a NuttX builtin application and run inside the NuttX `sim/nsh`
simulator (x86-64 Linux process).  The NuttX RTOS and its apps are cloned from
the latest `apache/nuttx` and `apache/nuttx-apps` main branches.

| Primitive | Cases | What is covered |
| --- | --- | --- |
| `osal::mutex` | 4 | lock/unlock, try_lock, lock_guard, timed-lock timeout |
| `osal::semaphore` | 4 | binary give/take, counting, take_for timeout, cross-thread signal |
| `osal::queue` | 2 | FIFO send/receive, full detection |
| `osal::timer` | 2 | one-shot fires once, periodic fires ≥4× |
| `osal::condvar` | 2 | cross-thread wait+notify, wait_for timeout |
| `osal::rwlock` | 2 | basic read/write lock, write_guard RAII |
| `osal::stream_buffer` | 2 | basic send/receive, reset |
| `osal::message_buffer` | 1 | FIFO send/receive |
| `osal::ring_buffer` | 1 | push/pop FIFO |
| `osal::thread` | 2 | create+join, sleep_for elapsed |
| `osal::thread_local_data` | 1 | per-thread isolation |
| `osal::memory_pool` | 2 | alloc/free, try_allocate → nullptr on exhaustion |
| `result`/`error_code` | 1 | ok vs error distinguishable |

---

## Code-path gaps (all backends)

These paths are difficult to reach on a hosted Linux target and are
**not covered** by any existing test:

| Gap | Reason |
| --- | --- |
| `nullptr` handle arguments | Error paths tested implicitly via destructors; no explicit null-handle test exists |
| `out_of_resources` on pool exhaustion (threads, mutexes) | Static pool sizes are large; needs a loop to exhaust |
| `overflow` on queue / work_queue / stream_buffer | Partially covered in stream_buffer; not systematic |
| ISR-safe variants (`give_isr`, `send_isr`, etc.) | Linux returns `not_supported`; cannot be exercised meaningfully on a hosted build |
| `WAIT_FOREVER` combined with thread cancellation | No cancellation support in OSAL |
| `NO_WAIT` (zero-timeout) fast-fail on every primitive | Not systematically tested |
| Timed-lock actually expiring (not just succeeding) | Covered for condvar, semaphore, rwlock, memory_pool; **missing** for mutex and queue |
| Memory ordering under relaxed CPU (`-march=native`) | Needs a dedicated TSan / TSAN-enabled build |

---

## Suggestions for new tests

The following would meaningfully increase confidence:

1. **`test_isr_paths.cpp`** — call `_isr` variants from task context to assert they return `ok`
   or `not_supported` as documented (semi-covered in stream_buffer already).
2. **`test_error_paths.cpp`** — systematically pass `nullptr` and already-destroyed handles to every public API.
3. **`test_no_wait.cpp`** — exercise `NO_WAIT` (zero-timeout) on mutex, semaphore, queue, rwlock when resource is unavailable.
4. **`test_stress.cpp`** — 4–8 threads hammering shared primitives for 30 seconds to surface
   memory-ordering and starvation bugs.
5. **FreeRTOS ISR-context tests** — a FreeRTOS-specific test that sets up a real timer ISR and
   calls `give_isr`/`send_isr` from interrupt context to verify the native FreeRTOS ISR API path.
6. **ThreadX / PX5 CI** — add compile+sim run once a permissively-licensed simulator (e.g. Eclipse ThreadX QEMU) is available.
7. **FreeRTOS v9 / v10** — add to the FreeRTOS CI matrix once backwards-compat validation is needed.

---

## How to add a test

1. Create `tests/test_<primitive>.cpp` following the pattern in existing files (doctest `TEST_CASE` / `SUBCASE`).
2. Add `osal_add_test(test_<primitive>  test_<primitive>.cpp)` to `tests/CMakeLists.txt`.
3. Rebuild: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`.
4. Update this table.
