// SPDX-License-Identifier: Apache-2.0
/// @file bare_metal_backend.cpp
/// @brief Bare-metal / no-RTOS implementation of all OSAL C-linkage functions
/// @details Provides a minimal cooperative multitasking layer with:
///          - Critical sections via user-supplied macros or ARM PRIMASK
///          - Spin-based mutex using atomic test-and-set
///          - Counting semaphore using atomic counter
///          - Circular-buffer queue protected by critical sections
///          - Software timer driven by a user-supplied `SysTick` handler
///          - Event flags using atomic bit operations
///          - Static cooperative thread "tasks" with setjmp/longjmp
///          - No wait_set
///
/// @par Critical section macros (override before including this file):
/// @code
///   #define OSAL_BM_ENTER_CRITICAL()  __disable_irq()
///   #define OSAL_BM_EXIT_CRITICAL()   __enable_irq()
/// @endcode
/// If not defined, the backend falls back to C11 _Noreturn-safe busy waits
/// (safe on single-core systems without real interrupts).
///
/// @par SysTick integration:
/// The user's SysTick_Handler (or equivalent) MUST call:
/// @code
///   extern "C" void osal_baremetal_tick();
/// @endcode
/// once per tick period.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_BAREMETAL
#define OSAL_BACKEND_BAREMETAL
#endif
#ifndef OSAL_BAREMETAL_MAX_TASKS
#define OSAL_BAREMETAL_MAX_TASKS 8
#endif
#include <osal/osal.hpp>

#include <cstring>
#include <cassert>
#include <setjmp.h>
#include <cstdint>
#include <atomic>
#if defined(OSAL_BM_TEST_SELF_TICK)
#include <cstdlib>
#include <ucontext.h>
#endif

// ---------------------------------------------------------------------------
// Critical section — user may override
// ---------------------------------------------------------------------------
#if !defined(OSAL_BM_ENTER_CRITICAL)
#if defined(__arm__) || defined(__ARM_ARCH)
#define OSAL_BM_ENTER_CRITICAL() __asm volatile("cpsid i" ::: "memory")
#define OSAL_BM_EXIT_CRITICAL() __asm volatile("cpsie i" ::: "memory")
#else
// No-op fallback for hosted/simulation builds.
#define OSAL_BM_ENTER_CRITICAL() \
    do                           \
    {                            \
    } while (false)
#define OSAL_BM_EXIT_CRITICAL() \
    do                          \
    {                           \
    } while (false)
#endif
#endif

// ---------------------------------------------------------------------------
// Tick counter (volatile so the compiler doesn't cache it across calls)
// ---------------------------------------------------------------------------
static volatile std::uint64_t g_ticks = 0U;

#if defined(OSAL_BM_TEST_SELF_TICK)
static void bm_advance_timers() noexcept;
static bool bm_self_tick_active = false;

static inline void bm_maybe_self_tick() noexcept
{
    if (bm_self_tick_active)
    {
        return;
    }

    bm_self_tick_active = true;
    g_ticks             = g_ticks + 1;
    bm_advance_timers();
    bm_self_tick_active = false;
}
#else
static inline void bm_maybe_self_tick() noexcept {}
#endif

/// @brief Called by the SysTick handler (or equivalent) each tick.
extern "C" void osal_baremetal_tick() noexcept
{
    g_ticks = g_ticks + 1;

    // Advance software timers.
    // See timer section below.
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

extern "C"
{
    /// @brief Return elapsed milliseconds from the bare-metal tick counter.
    /// @return Monotonic time in milliseconds (one tick = one millisecond by default).
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(g_ticks);
    }

    /// @brief Return system time — identical to `osal_clock_monotonic_ms` on bare-metal.
    /// @return Milliseconds since boot.
    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    /// @brief Return the raw tick counter.
    /// @return Current `g_ticks` value cast to `osal::tick_t`.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(g_ticks);
    }

    /// @brief Return the nominal tick period in microseconds.
    /// @return 1000 µs (1 ms) by default; override `OSAL_BAREMETAL_TICK_PERIOD_US` to change.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1'000U;  // Default: 1 ms tick.  Override OSAL_BAREMETAL_TICK_PERIOD_US if different.
    }

    // ---------------------------------------------------------------------------
    // Thread — cooperative tasks using setjmp/longjmp
    // ---------------------------------------------------------------------------
    /// @note Bare-metal "threads" are cooperative: thread_yield() switches to the
    ///       next ready task.  There is no pre-emption.

#define OSAL_BM_MAX_TASKS OSAL_BAREMETAL_MAX_TASKS
#define OSAL_BM_STACK_GUARD 0xDEADBEEFU

    struct bm_task
    {
#if defined(OSAL_BM_TEST_SELF_TICK)
        ucontext_t ctx;
#else
        jmp_buf ctx;
#endif
        bool started;
        bool finished;
        bool valid;
        bool detached;
        bool waiting;
        void (*entry)(void*);
        void*         arg;
        std::uint8_t* stack;
        std::size_t   stack_bytes;
    };

    static bm_task bm_tasks[OSAL_BM_MAX_TASKS];
    static int     bm_current         = -1;  ///< Index of currently running task; -1 = caller/scheduler context.
    static int     bm_last_scheduled  = -1;
    static bool    bm_sched_ctx_valid = false;
#if defined(OSAL_BM_TEST_SELF_TICK)
    static ucontext_t bm_sched_ctx;  ///< Caller context returned to when no peer task can run.
#else
    static jmp_buf bm_sched_ctx;  ///< Caller context returned to when no peer task can run.
#endif

#if defined(OSAL_BM_TEST_SELF_TICK)
    static int bm_find_next_ready_task(int current, bool include_waiting) noexcept
    {
        const int anchor = (current < 0) ? bm_last_scheduled : current;
        const int start  = (anchor < 0) ? 0 : ((anchor + 1) % OSAL_BM_MAX_TASKS);
        for (int i = 0; i < OSAL_BM_MAX_TASKS; ++i)
        {
            const int idx = (start + i) % OSAL_BM_MAX_TASKS;
            if (idx == current)
            {
                continue;
            }
            if (bm_tasks[idx].valid && !bm_tasks[idx].finished && (include_waiting || !bm_tasks[idx].waiting))
            {
                return idx;
            }
        }
        return -1;
    }

    static void bm_task_trampoline(int idx) noexcept
    {
        bm_current = idx;
        bm_tasks[idx].entry(bm_tasks[idx].arg);
        bm_tasks[idx].finished = true;
        bm_tasks[idx].waiting  = false;
        if (bm_tasks[idx].detached)
        {
            bm_tasks[idx].valid = false;
        }

        const int next = bm_find_next_ready_task(idx, false);
        if (next >= 0)
        {
            bm_last_scheduled = next;
            bm_current        = next;
            setcontext(&bm_tasks[next].ctx);
        }

        bm_current = -1;
        if (bm_sched_ctx_valid)
        {
            setcontext(&bm_sched_ctx);
        }

        std::abort();
    }

    /// @brief Internal cooperative scheduler — invoked by yield and sleep.
    static void bm_schedule() noexcept
    {
        if (bm_current < 0)
        {
            const int next = bm_find_next_ready_task(-1, true);
            if (next < 0)
            {
                return;
            }

            bm_last_scheduled = next;
            bm_current        = next;
            swapcontext(&bm_sched_ctx, &bm_tasks[next].ctx);
            bm_current = -1;
            return;
        }

        const int previous = bm_current;
        const int next     = bm_find_next_ready_task(previous, false);
        if (next >= 0)
        {
            bm_last_scheduled = next;
            bm_current        = next;
            swapcontext(&bm_tasks[previous].ctx, &bm_tasks[next].ctx);
            bm_current = previous;
            return;
        }

        if (bm_sched_ctx_valid)
        {
            bm_current = -1;
            swapcontext(&bm_tasks[previous].ctx, &bm_sched_ctx);
            bm_current = previous;
        }
    }
#else

    /// @brief Internal cooperative scheduler — invoked by yield and sleep.
    static void bm_schedule() noexcept
    {
        const int current = bm_current;
        const int start   = (current < 0) ? 0 : ((current + 1) % OSAL_BM_MAX_TASKS);

        for (int i = 0; i < OSAL_BM_MAX_TASKS; ++i)
        {
            const int idx = (start + i) % OSAL_BM_MAX_TASKS;
            if (idx == current)
            {
                continue;
            }
            if (bm_tasks[idx].valid && !bm_tasks[idx].finished)
            {
                bm_current = idx;
                if (!bm_tasks[idx].started)
                {
                    bm_tasks[idx].started = true;
                    // Call entry directly (no stack switching here — user provides stack via config).
                    bm_tasks[idx].entry(bm_tasks[idx].arg);
                    bm_tasks[idx].finished = true;
                    bm_current             = current;
                }
                else
                {
                    longjmp(bm_tasks[idx].ctx, 1);
                }
                return;
            }
        }

        if (current >= 0 && bm_sched_ctx_valid)
        {
            bm_current = -1;
            longjmp(bm_sched_ctx, 1);
        }
    }
#endif

    /// @brief Register a cooperative task (thread) in the bare-metal scheduler.
    /// @param handle      Output handle; `handle->native` points to the `bm_task` slot.
    /// @param entry       Task entry function.
    /// @param arg         Argument passed to @p entry at first call.
    /// @param stack       Caller-supplied stack buffer (stored but not mapped).
    /// @param stack_bytes Stack buffer size in bytes.
    /// @return `osal::ok()` on success, `out_of_resources` if `OSAL_BM_MAX_TASKS` is full.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t /*priority*/, osal::affinity_t /*affinity*/, void*       stack,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        assert(handle && entry);
        for (int i = 0; i < OSAL_BM_MAX_TASKS; ++i)
        {
            if (!bm_tasks[i].valid)
            {
                bm_tasks[i]             = {};
                bm_tasks[i].valid       = true;
                bm_tasks[i].started     = false;
                bm_tasks[i].finished    = false;
                bm_tasks[i].entry       = entry;
                bm_tasks[i].arg         = arg;
                bm_tasks[i].detached    = false;
                bm_tasks[i].waiting     = false;
                bm_tasks[i].stack       = static_cast<std::uint8_t*>(stack);
                bm_tasks[i].stack_bytes = stack_bytes;
#if defined(OSAL_BM_TEST_SELF_TICK)
                getcontext(&bm_tasks[i].ctx);
                bm_tasks[i].ctx.uc_stack.ss_sp   = bm_tasks[i].stack;
                bm_tasks[i].ctx.uc_stack.ss_size = bm_tasks[i].stack_bytes;
                bm_tasks[i].ctx.uc_link          = nullptr;
                makecontext(&bm_tasks[i].ctx, reinterpret_cast<void (*)()>(bm_task_trampoline), 1, i);
                bm_tasks[i].started = true;
#endif
                handle->native = static_cast<void*>(&bm_tasks[i]);
                return osal::ok();
            }
        }
        return osal::error_code::out_of_resources;
    }

    /// @brief Yield until the target task finishes, with optional tick deadline.
    /// @param handle  Thread handle.
    /// @param timeout Max ticks to wait; `OSAL_WAIT_FOREVER` to block indefinitely.
    /// @return `osal::ok()` when the task finishes, `timeout` on expiry.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto*               t        = static_cast<bm_task*>(handle->native);
        const std::uint64_t deadline = g_ticks + static_cast<std::uint64_t>(timeout);
        while (!t->finished)
        {
            osal_thread_yield();
            if (timeout != osal::WAIT_FOREVER && g_ticks >= deadline)
            {
                return osal::error_code::timeout;
            }
        }
        t->valid       = false;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Detach a task (clears `handle->native`; task continues running).
    /// @param handle Thread handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        static_cast<bm_task*>(handle->native)->detached = true;
        handle->native                                  = nullptr;
        return osal::ok();
    }

    /// @brief Priority change — not supported on bare-metal (no pre-emption).
    /// @return `not_supported` always.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t*, osal::priority_t) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief CPU affinity — not supported on bare-metal (single-core).
    /// @return `not_supported` always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t*, osal::affinity_t) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Thread suspend — not supported on bare-metal.
    /// @return `not_supported` always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Thread resume — not supported on bare-metal.
    /// @return `not_supported` always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Cooperatively yield to the next ready task via `setjmp`/`bm_schedule()`.
    void osal_thread_yield() noexcept
    {
        bm_maybe_self_tick();

#if defined(OSAL_BM_TEST_SELF_TICK)
        if (bm_current < 0)
        {
            const bool previous_sched_ctx_valid = bm_sched_ctx_valid;
            bm_sched_ctx_valid                  = true;
            bm_schedule();
            bm_sched_ctx_valid = previous_sched_ctx_valid;
            return;
        }

        bm_schedule();
        return;
#else
        if (bm_current < 0)
        {
            const bool previous_sched_ctx_valid = bm_sched_ctx_valid;
            bm_sched_ctx_valid                  = true;
            if (setjmp(bm_sched_ctx) == 0)
            {
                bm_schedule();
            }
            bm_sched_ctx_valid = previous_sched_ctx_valid;
            return;
        }

        if (setjmp(bm_tasks[bm_current].ctx) == 0)
        {
            bm_schedule();
        }
#endif
    }

    int osal_baremetal_current_task_index() noexcept
    {
        return bm_current;
    }

    static inline void bm_wait_yield() noexcept
    {
#if defined(OSAL_BM_TEST_SELF_TICK)
        if (bm_current >= 0)
        {
            bm_tasks[bm_current].waiting = true;
        }
#endif
        osal_thread_yield();
#if defined(OSAL_BM_TEST_SELF_TICK)
        if (bm_current >= 0)
        {
            bm_tasks[bm_current].waiting = false;
        }
#endif
    }

    /// @brief Spin-yield until at least @p ms ticks have elapsed since entry.
    /// @param ms Milliseconds to sleep (1 tick = 1 ms by default).
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        const std::uint64_t wake = g_ticks + static_cast<std::uint64_t>(ms);
        while (g_ticks < wake)
        {
            bm_wait_yield();
        }
    }

    // ---------------------------------------------------------------------------
    // Mutex — spin lock using std::atomic_flag
    // ---------------------------------------------------------------------------

#ifndef OSAL_BM_MAX_MUTEXES
#define OSAL_BM_MAX_MUTEXES 8
#endif
    struct bm_mutex
    {
        std::atomic_flag flag;
    };
    static bm_mutex bm_mutex_pool[OSAL_BM_MAX_MUTEXES];
    static bool     bm_mutex_used[OSAL_BM_MAX_MUTEXES];

    /// @brief Allocate a spin-lock mutex from the static pool.
    /// @param handle Output handle.
    /// @return `osal::ok()` on success, `out_of_resources` if `OSAL_BM_MAX_MUTEXES` is full.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        for (int i = 0; i < OSAL_BM_MAX_MUTEXES; ++i)
        {
            if (!bm_mutex_used[i])
            {
                bm_mutex_used[i] = true;
                bm_mutex_pool[i].flag.clear();
                handle->native = static_cast<void*>(&bm_mutex_pool[i]);
                return osal::ok();
            }
        }
        return osal::error_code::out_of_resources;
    }

    /// @brief Return a spin-lock mutex to the pool.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        for (int i = 0; i < OSAL_BM_MAX_MUTEXES; ++i)
        {
            if (&bm_mutex_pool[i] == handle->native)
            {
                bm_mutex_used[i] = false;
                break;
            }
        }
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire the spin-lock mutex, yielding cooperatively until timeout.
    /// @param handle  Mutex handle.
    /// @param timeout OSAL tick deadline; `WAIT_FOREVER` to block indefinitely.
    /// @return `osal::ok()` on success, `timeout` on expiry.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto*               m = static_cast<bm_mutex*>(handle->native);
        const std::uint64_t deadline =
            (timeout == osal::WAIT_FOREVER) ? UINT64_MAX : g_ticks + static_cast<std::uint64_t>(timeout);
        while (m->flag.test_and_set(std::memory_order_acquire))
        {
            if (g_ticks >= deadline)
            {
                return osal::error_code::timeout;
            }
            bm_wait_yield();
        }
        return osal::ok();
    }

    /// @brief Try to acquire the mutex atomically without blocking.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` if acquired, `would_block` otherwise.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<bm_mutex*>(handle->native);
        return m->flag.test_and_set(std::memory_order_acquire) ? osal::error_code::would_block : osal::ok();
    }

    /// @brief Release the spin-lock mutex.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        static_cast<bm_mutex*>(handle->native)->flag.clear(std::memory_order_release);
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Semaphore — atomic counter
    // ---------------------------------------------------------------------------

#ifndef OSAL_BM_MAX_SEMS
#define OSAL_BM_MAX_SEMS 8
#endif
    struct bm_semaphore
    {
        std::atomic<unsigned> count;
    };
    static bm_semaphore bm_sem_pool[OSAL_BM_MAX_SEMS];
    static bool         bm_sem_used[OSAL_BM_MAX_SEMS];

    /// @brief Allocate a counting semaphore from the static pool.
    /// @param handle Output handle.
    /// @param init   Initial count.
    /// @return `osal::ok()` on success, `out_of_resources` if `OSAL_BM_MAX_SEMS` is full.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned init,
                                       unsigned /*max*/) noexcept
    {
        for (int i = 0; i < OSAL_BM_MAX_SEMS; ++i)
        {
            if (!bm_sem_used[i])
            {
                bm_sem_used[i] = true;
                bm_sem_pool[i].count.store(init, std::memory_order_relaxed);
                handle->native = static_cast<void*>(&bm_sem_pool[i]);
                return osal::ok();
            }
        }
        return osal::error_code::out_of_resources;
    }

    /// @brief Return a semaphore to the pool.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        for (int i = 0; i < OSAL_BM_MAX_SEMS; ++i)
        {
            if (&bm_sem_pool[i] == handle->native)
            {
                bm_sem_used[i] = false;
                break;
            }
        }
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment the semaphore count atomically.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        static_cast<bm_semaphore*>(handle->native)->count.fetch_add(1U, std::memory_order_release);
        return osal::ok();
    }

    /// @brief Increment the semaphore from ISR context (delegates to `give`).
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` on success.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);
    }

    /// @brief Decrement the semaphore count, yielding cooperatively until available.
    /// @param handle  Semaphore handle.
    /// @param timeout OSAL tick deadline.
    /// @return `osal::ok()` on success, `timeout` on expiry.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto*               s = static_cast<bm_semaphore*>(handle->native);
        const std::uint64_t deadline =
            (timeout == osal::WAIT_FOREVER) ? UINT64_MAX : g_ticks + static_cast<std::uint64_t>(timeout);
        while (true)
        {
            unsigned c = s->count.load(std::memory_order_acquire);
            if (c > 0U)
            {
                if (s->count.compare_exchange_weak(c, c - 1U, std::memory_order_acquire))
                {
                    return osal::ok();
                }
            }
            else
            {
                if (g_ticks >= deadline)
                {
                    return osal::error_code::timeout;
                }
                bm_wait_yield();
            }
        }
    }

    /// @brief Try to decrement the semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` if decremented, `timeout` otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue — static circular buffer with critical-section protection
    // ---------------------------------------------------------------------------

#ifndef OSAL_BM_MAX_QUEUES
#define OSAL_BM_MAX_QUEUES 4
#endif
    struct bm_queue
    {
        std::uint8_t* buf;
        std::size_t   item_size, capacity, head, tail, count;
    };
    static bm_queue bm_queue_pool[OSAL_BM_MAX_QUEUES];
    static bool     bm_queue_used[OSAL_BM_MAX_QUEUES];

    /// @brief Allocate a circular queue from the static pool, backed by @p buf.
    /// @param handle    Output handle.
    /// @param buf       Caller-supplied ring buffer (must be `item_size * capacity` bytes).
    /// @param item_size Size of each item in bytes.
    /// @param capacity  Maximum number of items.
    /// @return `osal::ok()` on success, `out_of_resources` if `OSAL_BM_MAX_QUEUES` is full.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buf, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        for (int i = 0; i < OSAL_BM_MAX_QUEUES; ++i)
        {
            if (!bm_queue_used[i])
            {
                bm_queue_used[i] = true;
                bm_queue_pool[i] = {static_cast<std::uint8_t*>(buf), item_size, capacity, 0, 0, 0};
                handle->native   = static_cast<void*>(&bm_queue_pool[i]);
                return osal::ok();
            }
        }
        return osal::error_code::out_of_resources;
    }

    /// @brief Return a queue to the pool.
    /// @param handle Queue handle.
    /// @return `osal::ok()` always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        for (int i = 0; i < OSAL_BM_MAX_QUEUES; ++i)
        {
            if (&bm_queue_pool[i] == handle->native)
            {
                bm_queue_used[i] = false;
                break;
            }
        }
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Copy @p item into the circular buffer, yielding until space or timeout.
    /// @param handle  Queue handle.
    /// @param item    Source item to enqueue.
    /// @param timeout OSAL tick deadline; `NO_WAIT` to fail immediately if full.
    /// @return `osal::ok()` on success, `would_block` if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto*               q = static_cast<bm_queue*>(handle->native);
        const std::uint64_t deadline =
            (timeout == osal::WAIT_FOREVER) ? UINT64_MAX : g_ticks + static_cast<std::uint64_t>(timeout);
        while (true)
        {
            OSAL_BM_ENTER_CRITICAL();
            if (q->count < q->capacity)
            {
                std::memcpy(q->buf + q->tail * q->item_size, item, q->item_size);
                q->tail = (q->tail + 1U) % q->capacity;
                q->count++;
                OSAL_BM_EXIT_CRITICAL();
                return osal::ok();
            }
            OSAL_BM_EXIT_CRITICAL();
            if (timeout == osal::NO_WAIT || g_ticks >= deadline)
            {
                return osal::error_code::would_block;
            }
            bm_wait_yield();
        }
    }

    /// @brief Enqueue an item from ISR context (no-wait).
    /// @param handle Queue handle.
    /// @param item   Source item.
    /// @return `osal::ok()` on success, `would_block` if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Dequeue an item, yielding until one is available or timeout.
    /// @param handle  Queue handle.
    /// @param item    Destination buffer.
    /// @param timeout OSAL tick deadline.
    /// @return `osal::ok()` on success, `timeout` if the queue is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto*               q = static_cast<bm_queue*>(handle->native);
        const std::uint64_t deadline =
            (timeout == osal::WAIT_FOREVER) ? UINT64_MAX : g_ticks + static_cast<std::uint64_t>(timeout);
        while (true)
        {
            OSAL_BM_ENTER_CRITICAL();
            if (q->count > 0U)
            {
                std::memcpy(item, q->buf + q->head * q->item_size, q->item_size);
                q->head = (q->head + 1U) % q->capacity;
                q->count--;
                OSAL_BM_EXIT_CRITICAL();
                return osal::ok();
            }
            OSAL_BM_EXIT_CRITICAL();
            if (timeout == osal::NO_WAIT || g_ticks >= deadline)
            {
                return osal::error_code::timeout;
            }
            bm_wait_yield();
        }
    }

    /// @brief Dequeue from ISR context (no-wait).
    /// @param handle Queue handle.
    /// @param item   Destination buffer.
    /// @return `osal::ok()` on success, `timeout` if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Copy the head item without removing it.
    /// @param handle Queue handle.
    /// @param item   Destination buffer.
    /// @return `osal::ok()` on success, `would_block` if the queue is empty.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item, osal::tick_t) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<bm_queue*>(handle->native);
        OSAL_BM_ENTER_CRITICAL();
        if (q->count == 0)
        {
            OSAL_BM_EXIT_CRITICAL();
            return osal::error_code::would_block;
        }
        std::memcpy(item, q->buf + q->head * q->item_size, q->item_size);
        OSAL_BM_EXIT_CRITICAL();
        return osal::ok();
    }

    /// @brief Return the number of items currently in the queue.
    /// @param handle Queue handle (const).
    /// @return Item count, or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return 0U;
        return static_cast<const bm_queue*>(handle->native)->count;
    }

    /// @brief Return the number of free slots remaining in the queue.
    /// @param handle Queue handle (const).
    /// @return Free slot count, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return 0U;
        auto* q = static_cast<const bm_queue*>(handle->native);
        return q->capacity - q->count;
    }

    // ---------------------------------------------------------------------------
    // Timer — software timers polled from osal_baremetal_tick()
    // ---------------------------------------------------------------------------

#ifndef OSAL_BM_MAX_TIMERS
#define OSAL_BM_MAX_TIMERS 8
#endif
    struct bm_timer
    {
        osal_timer_callback_t fn;
        void*                 arg;
        osal::tick_t          period;    ///< Reload value in ticks
        std::uint64_t         deadline;  ///< Absolute tick of next expiry
        bool                  auto_reload;
        bool                  active;
        bool                  valid;
    };
    static bm_timer bm_timer_pool[OSAL_BM_MAX_TIMERS];

    // Advance timers — called from osal_baremetal_tick().
}  // close extern "C" temporarily to define the timer advance function

static void bm_advance_timers() noexcept
{
    for (auto& t : bm_timer_pool)
    {
        if (!t.valid || !t.active)
        {
            continue;
        }
        if (g_ticks >= t.deadline)
        {
            if (t.fn)
            {
                t.fn(t.arg);
            }
            if (t.auto_reload)
            {
                t.deadline = g_ticks + static_cast<std::uint64_t>(t.period);
            }
            else
            {
                t.active = false;
            }
        }
    }
}

extern "C"
{
    // Re-open and add timer advance to tick callback (patch the symbol).
    // Because osal_baremetal_tick() is already defined above, we expose an
    // extended tick that the user can call if they want timer support.
    /// @brief Extended tick entry point that also advances software timers.
    /// @details Call this instead of (or in addition to) `osal_baremetal_tick()` when
    ///          software timer support is needed.
    void osal_baremetal_tick_with_timers() noexcept
    {
        g_ticks = g_ticks + 1;
        bm_advance_timers();
    }

    /// @brief Allocate a software timer from the static pool.
    /// @param handle      Output handle.
    /// @param cb          Callback invoked when the timer fires.
    /// @param arg         Argument passed to @p cb.
    /// @param period      Period in OSAL ticks.
    /// @param auto_reload `true` for periodic; `false` for one-shot.
    /// @return `osal::ok()` on success, `out_of_resources` if `OSAL_BM_MAX_TIMERS` is full.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period, bool auto_reload) noexcept
    {
        assert(handle && cb);
        for (int i = 0; i < OSAL_BM_MAX_TIMERS; ++i)
        {
            if (!bm_timer_pool[i].valid)
            {
                bm_timer_pool[i] = {cb, arg, period, 0, auto_reload, false, true};
                handle->native   = static_cast<void*>(&bm_timer_pool[i]);
                return osal::ok();
            }
        }
        return osal::error_code::out_of_resources;
    }

    /// @brief Mark a timer slot as invalid (destroys the timer).
    /// @param handle Timer handle.
    /// @return `osal::ok()` always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        static_cast<bm_timer*>(handle->native)->valid = false;
        handle->native                                = nullptr;
        return osal::ok();
    }

    /// @brief Arm a software timer; sets the deadline to `now + period`.
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* t     = static_cast<bm_timer*>(handle->native);
        t->deadline = g_ticks + static_cast<std::uint64_t>(t->period);
        t->active   = true;
        return osal::ok();
    }

    /// @brief Disarm a software timer (marks it inactive).
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        static_cast<bm_timer*>(handle->native)->active = false;
        return osal::ok();
    }

    /// @brief Stop and re-arm, reloading the period from the stored value.
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success.
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Update the timer period and reschedule the next deadline.
    /// @param handle  Timer handle.
    /// @param p       New period in OSAL ticks.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle, osal::tick_t p) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* t     = static_cast<bm_timer*>(handle->native);
        t->period   = p;
        t->deadline = g_ticks + static_cast<std::uint64_t>(p);
        return osal::ok();
    }

    /// @brief Query whether a software timer is currently armed.
    /// @param handle Timer handle (const).
    /// @return `true` if the timer is armed, `false` otherwise.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return false;
        return static_cast<const bm_timer*>(handle->native)->active;
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

    // ---------------------------------------------------------------------------
    // Wait-set — not supported on bare-metal
    // ---------------------------------------------------------------------------

    /// @brief Wait-set not supported on bare-metal.
    /// @return `not_supported` always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set destroy not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set add not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set remove not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set wait not supported.
    /// @param n Set to 0 if non-null.
    /// @return `not_supported` always.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t*, int*, std::size_t, std::size_t* n,
                                    osal::tick_t) noexcept
    {
        if (n)
            *n = 0U;
        return osal::error_code::not_supported;
    }

    // ---------------------------------------------------------------------------
    // Condition variable (emulated — Birrell pattern: mutex + semaphore)
    // ---------------------------------------------------------------------------

#include "../common/emulated_condvar.inl"

    // ---------------------------------------------------------------------------
    // Work queue (emulated — OSAL thread + mutex + semaphore + ring buffer)
    // ---------------------------------------------------------------------------

#include "../common/emulated_work_queue.inl"

    // ---------------------------------------------------------------------------
    // Memory pool (emulated — bitmap + mutex + counting semaphore)
    // ---------------------------------------------------------------------------

#include "../common/emulated_memory_pool.inl"

    // ---------------------------------------------------------------------------
    // Read-write lock (emulated — mutex + condvar + counter)
    // ---------------------------------------------------------------------------

#include "../common/emulated_rwlock.inl"

    // ---------------------------------------------------------------------------
    // Stream buffer (emulated — SPSC lock-free ring + binary semaphores)
    // ---------------------------------------------------------------------------

#include "../common/emulated_stream_buffer.inl"

    // ---------------------------------------------------------------------------
    // Message buffer (emulated — length-prefixed SPSC ring + binary semaphores)
    // ---------------------------------------------------------------------------

#include "../common/emulated_message_buffer.inl"
    // ---------------------------------------------------------------------------
    // Spinlock
    // ---------------------------------------------------------------------------
#include "../common/emulated_spinlock.inl"

    // ---------------------------------------------------------------------------
    // Barrier
    // ---------------------------------------------------------------------------
#include "../common/emulated_barrier.inl"

    // ---------------------------------------------------------------------------
    // Task notification
    // ---------------------------------------------------------------------------
#include "../common/emulated_task_notify.inl"

}  // extern "C"
