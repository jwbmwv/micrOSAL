// SPDX-License-Identifier: Apache-2.0
/// @file zephyr_backend.cpp
/// @brief Zephyr RTOS implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the corresponding Zephyr kernel API.
///
///          Minimum required version: Zephyr 3.x+
///          All kernel objects used (k_mutex, k_sem, k_msgq, k_timer,
///          k_event, k_condvar, k_mem_slab) are part of the stable Zephyr
///          kernel API available since 3.0.  The CI tests run against
///          Zephyr 4.3, but no 4.x-only APIs are required.
///
///          Kernel object strategy:
///          - All Zephyr kernel objects (k_mutex, k_sem, k_msgq, k_timer) are
///            allocated statically.  For thread and queue they use the caller-
///            supplied buffers so no heap is required.
///
///          Include requirements (provided by Zephyr build system):
///          - <zephyr/kernel.h>  (brings in all kernel API)
///          - <zephyr/sys/util.h>
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_ZEPHYR
#define OSAL_BACKEND_ZEPHYR
#endif
#include <osal/osal.hpp>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <cassert>
#include <cstring>
#include <cerrno>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// @brief Converts an OSAL priority to Zephyr thread priority.
/// Zephyr: lower integer = higher priority (0 = highest cooperative).
static constexpr int osal_to_zephyr_priority(osal::priority_t p) noexcept
{
    // Map [0,255] -> [CONFIG_NUM_PREEMPT_PRIORITIES-1, 0]
    const int zmax = CONFIG_NUM_PREEMPT_PRIORITIES - 1;
    return zmax - static_cast<int>((static_cast<std::uint32_t>(p) * static_cast<std::uint32_t>(zmax)) /
                                   static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
}

/// @brief Converts tick count to Zephyr k_timeout_t.
static k_timeout_t to_zephyr_timeout(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return K_FOREVER;
    }
    if (t == osal::NO_WAIT)
    {
        return K_NO_WAIT;
    }
    return K_MSEC(static_cast<int64_t>(t));  // osal tick is ms on Zephyr backend
}

// Accessor macros: Zephyr objects are stored via the handle's native pointer.
#define ZK_MUTEX(h) static_cast<struct k_mutex*>((h)->native)
#define ZK_SEM(h) static_cast<struct k_sem*>((h)->native)
#define ZK_MSGQ(h) static_cast<struct k_msgq*>((h)->native)
#define ZK_TIMER(h) static_cast<struct k_timer*>((h)->native)
#define ZK_THREAD(h) static_cast<struct k_thread*>((h)->native)

#if defined(CONFIG_MICRO_OSAL) && (!defined(CONFIG_MULTITHREADING) || (CONFIG_MULTITHREADING != 1))
#error "MicrOSAL Zephyr backend requires CONFIG_MULTITHREADING=y."
#endif

#if defined(CONFIG_MICRO_OSAL) && (!defined(CONFIG_EVENTS) || (CONFIG_EVENTS != 1))
#error "MicrOSAL Zephyr backend requires CONFIG_EVENTS=y for native event flags."
#endif

// ---------------------------------------------------------------------------
// Static kernel object pools — one entry per concurrent object.
// For production use, adjust the Zephyr Kconfig symbols or override the
// OSAL_ZEPHYR_* macros before compiling this translation unit.
// ---------------------------------------------------------------------------
#ifndef OSAL_ZEPHYR_MAX_MUTEXES
#if defined(CONFIG_MICRO_OSAL_MUTEX_MAX)
#define OSAL_ZEPHYR_MAX_MUTEXES CONFIG_MICRO_OSAL_MUTEX_MAX
#else
#define OSAL_ZEPHYR_MAX_MUTEXES 16
#endif
#endif

#ifndef OSAL_ZEPHYR_MAX_SEMS
#if defined(CONFIG_MICRO_OSAL_SEMAPHORE_MAX)
#define OSAL_ZEPHYR_MAX_SEMS CONFIG_MICRO_OSAL_SEMAPHORE_MAX
#else
#define OSAL_ZEPHYR_MAX_SEMS 16
#endif
#endif

#ifndef OSAL_ZEPHYR_MAX_QUEUES
#if defined(CONFIG_MICRO_OSAL_QUEUE_MAX)
#define OSAL_ZEPHYR_MAX_QUEUES CONFIG_MICRO_OSAL_QUEUE_MAX
#else
#define OSAL_ZEPHYR_MAX_QUEUES 8
#endif
#endif

#ifndef OSAL_ZEPHYR_MAX_TIMERS
#if defined(CONFIG_MICRO_OSAL_TIMER_MAX)
#define OSAL_ZEPHYR_MAX_TIMERS CONFIG_MICRO_OSAL_TIMER_MAX
#else
#define OSAL_ZEPHYR_MAX_TIMERS 8
#endif
#endif

#ifndef OSAL_ZEPHYR_MAX_THREADS
#if defined(CONFIG_MICRO_OSAL_THREAD_MAX)
#define OSAL_ZEPHYR_MAX_THREADS CONFIG_MICRO_OSAL_THREAD_MAX
#else
#define OSAL_ZEPHYR_MAX_THREADS 8
#endif
#endif

#ifndef OSAL_ZEPHYR_MAX_EVENTS
#if defined(CONFIG_MICRO_OSAL_EVENT_FLAGS_MAX)
#define OSAL_ZEPHYR_MAX_EVENTS CONFIG_MICRO_OSAL_EVENT_FLAGS_MAX
#else
#define OSAL_ZEPHYR_MAX_EVENTS 8
#endif
#endif

#ifndef OSAL_ZEPHYR_MAX_CONDVARS
#if defined(CONFIG_MICRO_OSAL_CONDVAR_MAX)
#define OSAL_ZEPHYR_MAX_CONDVARS CONFIG_MICRO_OSAL_CONDVAR_MAX
#else
#define OSAL_ZEPHYR_MAX_CONDVARS 16
#endif
#endif

#ifndef OSAL_ZEPHYR_MAX_WORK_QUEUES
#if defined(CONFIG_MICRO_OSAL_WORK_QUEUE_MAX)
#define OSAL_ZEPHYR_MAX_WORK_QUEUES CONFIG_MICRO_OSAL_WORK_QUEUE_MAX
#else
#define OSAL_ZEPHYR_MAX_WORK_QUEUES 4
#endif
#endif

#ifndef OSAL_ZEPHYR_WQ_MAX_DEPTH
#if defined(CONFIG_MICRO_OSAL_WORK_QUEUE_DEPTH)
#define OSAL_ZEPHYR_WQ_MAX_DEPTH CONFIG_MICRO_OSAL_WORK_QUEUE_DEPTH
#else
#define OSAL_ZEPHYR_WQ_MAX_DEPTH 32
#endif
#endif

static_assert(OSAL_ZEPHYR_MAX_MUTEXES > 0, "OSAL_ZEPHYR_MAX_MUTEXES must be > 0.");
static_assert(OSAL_ZEPHYR_MAX_SEMS > 0, "OSAL_ZEPHYR_MAX_SEMS must be > 0.");
static_assert(OSAL_ZEPHYR_MAX_QUEUES > 0, "OSAL_ZEPHYR_MAX_QUEUES must be > 0.");
static_assert(OSAL_ZEPHYR_MAX_TIMERS > 0, "OSAL_ZEPHYR_MAX_TIMERS must be > 0.");
static_assert(OSAL_ZEPHYR_MAX_THREADS > 0, "OSAL_ZEPHYR_MAX_THREADS must be > 0.");
static_assert(OSAL_ZEPHYR_MAX_EVENTS > 0, "OSAL_ZEPHYR_MAX_EVENTS must be > 0.");
static_assert(OSAL_ZEPHYR_MAX_CONDVARS > 0, "OSAL_ZEPHYR_MAX_CONDVARS must be > 0.");
static_assert(OSAL_ZEPHYR_MAX_WORK_QUEUES > 0, "OSAL_ZEPHYR_MAX_WORK_QUEUES must be > 0.");
static_assert(OSAL_ZEPHYR_WQ_MAX_DEPTH > 0, "OSAL_ZEPHYR_WQ_MAX_DEPTH must be > 0.");

static struct k_mutex  zephyr_mutexes[OSAL_ZEPHYR_MAX_MUTEXES];
static bool            zephyr_mutex_used[OSAL_ZEPHYR_MAX_MUTEXES];
static struct k_sem    zephyr_sems[OSAL_ZEPHYR_MAX_SEMS];
static bool            zephyr_sem_used[OSAL_ZEPHYR_MAX_SEMS];
static struct k_msgq   zephyr_queues[OSAL_ZEPHYR_MAX_QUEUES];
static bool            zephyr_queue_used[OSAL_ZEPHYR_MAX_QUEUES];
static struct k_timer  zephyr_timers[OSAL_ZEPHYR_MAX_TIMERS];
static bool            zephyr_timer_used[OSAL_ZEPHYR_MAX_TIMERS];
static struct k_thread zephyr_threads[OSAL_ZEPHYR_MAX_THREADS];
static bool            zephyr_thread_used[OSAL_ZEPHYR_MAX_THREADS];

template<typename T, std::size_t N>
static T* acquire_from_pool(T (&pool)[N], bool (&used)[N]) noexcept
{
    for (std::size_t i = 0U; i < N; ++i)
    {
        if (!used[i])
        {
            used[i] = true;
            return &pool[i];
        }
    }
    return nullptr;
}

template<typename T, std::size_t N>
static void release_to_pool(T (&pool)[N], bool (&used)[N], T* p) noexcept
{
    for (std::size_t i = 0U; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            used[i] = false;
            return;
        }
    }
}

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    /// @brief Returns the monotonic clock value in milliseconds.
    /// @return Elapsed milliseconds since system boot, via @c k_uptime_get().
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(k_uptime_get());
    }

    /// @brief Returns the system (wall-clock) time in milliseconds.
    /// @details Zephyr 3.x does not provide a separate wall-clock API;
    ///          @c k_uptime_get() (monotonic uptime) is therefore returned.
    /// @return Milliseconds since system boot, identical to @c osal_clock_monotonic_ms().
    std::int64_t osal_clock_system_ms() noexcept
    {
        // Zephyr 3.x+: no separate wall-clock API; uptime is the best option.
        return static_cast<std::int64_t>(k_uptime_get());
    }

    /// @brief Returns the current kernel tick counter.
    /// @return Current tick value via @c k_uptime_ticks().
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(k_uptime_ticks());
    }

    /// @brief Returns the duration of one kernel tick in microseconds.
    /// @details Computed as @c 1000000 / @c CONFIG_SYS_CLOCK_TICKS_PER_SEC.
    /// @return Microseconds per tick.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / CONFIG_SYS_CLOCK_TICKS_PER_SEC);
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    /// @brief Creates and starts a Zephyr thread.
    /// @details Allocates a @c k_thread object from the static pool, calls
    ///          @c k_thread_create(), optionally names it via @c k_thread_name_set(),
    ///          and applies CPU-mask affinity when @c CONFIG_SCHED_CPU_MASK is enabled.
    /// @param handle     Output handle; receives the allocated @c k_thread pointer.
    /// @param entry      Thread entry function pointer.
    /// @param arg        Argument forwarded to @p entry.
    /// @param priority   OSAL priority mapped to Zephyr preemptive priority.
    /// @param affinity   CPU affinity bitmask; @c AFFINITY_ANY leaves scheduling unrestricted.
    /// @param stack      Caller-supplied stack buffer.
    /// @param stack_bytes Size of @p stack in bytes.
    /// @param name       Optional thread name (may be @c nullptr).
    /// @return @c osal::ok() on success, @c error_code::out_of_resources if the pool is exhausted.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr && entry != nullptr && stack != nullptr);
        struct k_thread* t = acquire_from_pool(zephyr_threads, zephyr_thread_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        const k_tid_t tid = k_thread_create(
            t, static_cast<k_thread_stack_t*>(stack), stack_bytes,
            [](void* a, void* b, void* /*c*/)
            {
                auto fn = reinterpret_cast<void (*)(void*)>(a);
                fn(b);
            },
            reinterpret_cast<void*>(entry), arg, nullptr, osal_to_zephyr_priority(priority), 0, K_NO_WAIT);

        if (tid == nullptr)
        {
            release_to_pool(zephyr_threads, zephyr_thread_used, t);
            return osal::error_code::out_of_resources;
        }

        if (name != nullptr)
        {
            k_thread_name_set(tid, name);
        }

#if defined(CONFIG_SCHED_CPU_MASK)
        if (affinity != osal::AFFINITY_ANY)
        {
            k_thread_cpu_mask_clear(tid);
            for (int cpu = 0; cpu < CONFIG_MP_MAX_NUM_CPUS; ++cpu)
            {
                if ((affinity & (1U << static_cast<unsigned>(cpu))) != 0U)
                {
                    k_thread_cpu_mask_enable(tid, cpu);
                }
            }
        }
#else
        (void)affinity;
#endif

        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    /// @brief Waits for a thread to terminate.
    /// @details Calls @c k_thread_join() with the converted timeout.  On success
    ///          the pool slot is released and the handle cleared.
    /// @param handle        Handle of the thread to join.
    /// @param timeout_ticks Maximum wait time in ticks; use @c WAIT_FOREVER to block indefinitely.
    /// @return @c osal::ok() on success, @c error_code::timeout if the deadline expires,
    ///         or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_thread_join(ZK_THREAD(handle), to_zephyr_timeout(timeout_ticks));
        if (rc == 0)
        {
            release_to_pool(zephyr_threads, zephyr_thread_used, ZK_THREAD(handle));
            handle->native = nullptr;
            return osal::ok();
        }
        return (rc == -EAGAIN) ? osal::error_code::timeout : osal::error_code::unknown;
    }

    /// @brief Detaches a thread so its resources are released automatically on exit.
    /// @details Zephyr has no native @c k_thread_detach API.  This implementation
    ///          simply releases the pool slot and clears the handle, preventing a
    ///          subsequent @c osal_thread_join().
    /// @param handle Handle of the thread to detach.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        // Zephyr does not have a native k_thread_detach API — simply release
        // the pool slot and clear the handle so join() sees detached state.
        release_to_pool(zephyr_threads, zephyr_thread_used, ZK_THREAD(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Adjusts the scheduling priority of a running thread.
    /// @details Calls @c k_thread_priority_set() after mapping the OSAL priority
    ///          to the Zephyr preemptive priority range.
    /// @param handle   Handle of the target thread.
    /// @param priority New OSAL priority value.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_thread_priority_set(ZK_THREAD(handle), osal_to_zephyr_priority(priority));
        return osal::ok();
    }

    /// @brief Sets the CPU affinity mask of a thread.
    /// @details Uses @c k_thread_cpu_mask_clear() / @c k_thread_cpu_mask_enable()
    ///          when @c CONFIG_SCHED_CPU_MASK is enabled; returns
    ///          @c error_code::not_supported otherwise.
    /// @param handle   Handle of the target thread.
    /// @param affinity Bitmask of allowed CPU cores.
    /// @return @c osal::ok() on success, @c error_code::not_supported if the config is absent,
    ///         or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept
    {
#if defined(CONFIG_SCHED_CPU_MASK)
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_thread_cpu_mask_clear(ZK_THREAD(handle));
        for (int cpu = 0; cpu < CONFIG_MP_MAX_NUM_CPUS; ++cpu)
        {
            if ((affinity & (1U << static_cast<unsigned>(cpu))) != 0U)
            {
                k_thread_cpu_mask_enable(ZK_THREAD(handle), cpu);
            }
        }
        return osal::ok();
#else
        (void)handle;
        (void)affinity;
        return osal::error_code::not_supported;
#endif
    }

    /// @brief Suspends a thread, preventing it from being scheduled.
    /// @details Calls @c k_thread_suspend().
    /// @param handle Handle of the thread to suspend.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_thread_suspend(ZK_THREAD(handle));
        return osal::ok();
    }

    /// @brief Resumes a previously suspended thread.
    /// @details Calls @c k_thread_resume().
    /// @param handle Handle of the thread to resume.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_thread_resume(ZK_THREAD(handle));
        return osal::ok();
    }

    /// @brief Yields the CPU to other threads of the same or higher priority.
    /// @details Calls @c k_yield().
    void osal_thread_yield() noexcept
    {
        k_yield();
    }

    /// @brief Puts the calling thread to sleep for the specified number of milliseconds.
    /// @details Calls @c k_sleep(K_MSEC(ms)).
    /// @param ms Duration to sleep in milliseconds.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        k_sleep(K_MSEC(static_cast<int64_t>(ms)));
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    /// @brief Creates a mutex.
    /// @details Allocates a @c k_mutex from the static pool and calls @c k_mutex_init().
    ///          Zephyr's @c k_mutex is inherently recursive; the @p recursive flag is accepted
    ///          but has no behavioral effect.
    /// @param handle    Output handle receiving the initialized @c k_mutex pointer.
    /// @param recursive Ignored — Zephyr mutexes are always recursive.
    /// @return @c osal::ok() on success, or @c error_code::out_of_resources if the pool is full.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept
    {
        // Zephyr k_mutex is inherently recursive.  We just note the mode.
        (void)recursive;
        struct k_mutex* m = acquire_from_pool(zephyr_mutexes, zephyr_mutex_used);
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        k_mutex_init(m);
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    /// @brief Destroys a mutex and returns it to the static pool.
    /// @param handle Handle of the mutex to destroy; a @c nullptr or already-null handle is ignored.
    /// @return Always @c osal::ok().
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        release_to_pool(zephyr_mutexes, zephyr_mutex_used, ZK_MUTEX(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquires a mutex with an optional timeout.
    /// @details Calls @c k_mutex_lock() using the converted @c k_timeout_t.
    /// @param handle        Handle of the mutex to acquire.
    /// @param timeout_ticks Maximum wait time in ticks; @c WAIT_FOREVER or @c NO_WAIT are honoured.
    /// @return @c osal::ok() on success, @c error_code::timeout if the deadline expires,
    ///         or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_mutex_lock(ZK_MUTEX(handle), to_zephyr_timeout(timeout_ticks));
        if (rc == 0)
        {
            return osal::ok();
        }
        if (rc == -EAGAIN)
        {
            return osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    /// @brief Attempts to acquire a mutex without blocking.
    /// @details Delegates to @c osal_mutex_lock() with @c NO_WAIT.
    /// @param handle Handle of the mutex.
    /// @return @c osal::ok() if acquired, @c error_code::timeout if unavailable.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Releases a previously acquired mutex.
    /// @details Calls @c k_mutex_unlock().
    /// @param handle Handle of the mutex to release.
    /// @return @c osal::ok() on success, @c error_code::not_owner if the caller does not own the lock,
    ///         or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_mutex_unlock(ZK_MUTEX(handle));
        return (rc == 0) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    /// @brief Creates a counting semaphore.
    /// @details Allocates a @c k_sem from the static pool and calls @c k_sem_init().
    /// @param handle        Output handle receiving the initialized @c k_sem pointer.
    /// @param initial_count Initial semaphore count.
    /// @param max_count     Maximum semaphore count.
    /// @return @c osal::ok() on success, or @c error_code::out_of_resources if the pool is full.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        struct k_sem* s = acquire_from_pool(zephyr_sems, zephyr_sem_used);
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        k_sem_init(s, initial_count, max_count);
        handle->native = static_cast<void*>(s);
        return osal::ok();
    }

    /// @brief Destroys a semaphore and returns it to the static pool.
    /// @param handle Handle of the semaphore to destroy; a @c nullptr or already-null handle is ignored.
    /// @return Always @c osal::ok().
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        release_to_pool(zephyr_sems, zephyr_sem_used, ZK_SEM(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Signals (gives) a semaphore.
    /// @details Calls @c k_sem_give().
    /// @param handle Handle of the semaphore.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_sem_give(ZK_SEM(handle));
        return osal::ok();
    }

    /// @brief Signals a semaphore from an ISR context.
    /// @details Delegates to @c osal_semaphore_give() — @c k_sem_give() is ISR-safe on Zephyr.
    /// @param handle Handle of the semaphore.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        // On Zephyr k_sem_give is ISR-safe.
        return osal_semaphore_give(handle);
    }

    /// @brief Waits on a semaphore with an optional timeout.
    /// @details Calls @c k_sem_take() using the converted @c k_timeout_t.
    /// @param handle        Handle of the semaphore.
    /// @param timeout_ticks Maximum wait time in ticks; @c WAIT_FOREVER or @c NO_WAIT are honoured.
    /// @return @c osal::ok() on success, @c error_code::timeout if the count is zero and the deadline
    ///         expires, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_sem_take(ZK_SEM(handle), to_zephyr_timeout(timeout_ticks));
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Attempts to decrement a semaphore without blocking.
    /// @details Delegates to @c osal_semaphore_take() with @c NO_WAIT.
    /// @param handle Handle of the semaphore.
    /// @return @c osal::ok() if taken, @c error_code::timeout if the count is zero.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (k_msgq)
    // ---------------------------------------------------------------------------

    /// @brief Creates a message queue backed by a Zephyr @c k_msgq.
    /// @details Allocates a @c k_msgq from the static pool and calls @c k_msgq_init()
    ///          with the caller-supplied buffer.
    /// @param handle    Output handle receiving the initialized @c k_msgq pointer.
    /// @param buffer    Caller-supplied storage buffer; must be at least @p item_size * @p capacity bytes.
    /// @param item_size Size of each queue item in bytes.
    /// @param capacity  Maximum number of items the queue can hold.
    /// @return @c osal::ok() on success, or @c error_code::out_of_resources if the pool is full.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        struct k_msgq* q = acquire_from_pool(zephyr_queues, zephyr_queue_used);
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        k_msgq_init(q, static_cast<char*>(buffer), item_size, static_cast<uint32_t>(capacity));
        handle->native = static_cast<void*>(q);
        return osal::ok();
    }

    /// @brief Destroys a message queue, discarding all pending items.
    /// @details Calls @c k_msgq_purge() to drain the queue, then releases the pool slot.
    /// @param handle Handle of the queue to destroy; a @c nullptr or already-null handle is ignored.
    /// @return Always @c osal::ok().
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        k_msgq_purge(ZK_MSGQ(handle));
        release_to_pool(zephyr_queues, zephyr_queue_used, ZK_MSGQ(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Enqueues an item into the message queue with an optional timeout.
    /// @details Calls @c k_msgq_put() using the converted @c k_timeout_t.
    /// @param handle        Handle of the queue.
    /// @param item          Pointer to the data to enqueue; copied by value.
    /// @param timeout_ticks Maximum wait time if the queue is full.
    /// @return @c osal::ok() on success, @c error_code::timeout if the queue remains full
    ///         until the deadline, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_msgq_put(ZK_MSGQ(handle), const_cast<void*>(item), to_zephyr_timeout(timeout_ticks));
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Enqueues an item from an ISR context (non-blocking).
    /// @details Delegates to @c osal_queue_send() with @c NO_WAIT.
    ///          @c k_msgq_put() is ISR-safe on Zephyr.
    /// @param handle Handle of the queue.
    /// @param item   Pointer to the data to enqueue.
    /// @return @c osal::ok() on success, @c error_code::timeout if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Dequeues an item from the message queue with an optional timeout.
    /// @details Calls @c k_msgq_get() using the converted @c k_timeout_t.
    /// @param handle        Handle of the queue.
    /// @param item          Destination buffer; receives the dequeued item by copy.
    /// @param timeout_ticks Maximum wait time if the queue is empty.
    /// @return @c osal::ok() on success, @c error_code::timeout if the queue remains empty
    ///         until the deadline, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_msgq_get(ZK_MSGQ(handle), item, to_zephyr_timeout(timeout_ticks));
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Dequeues an item from an ISR context (non-blocking).
    /// @details Delegates to @c osal_queue_receive() with @c NO_WAIT.
    /// @param handle Handle of the queue.
    /// @param item   Destination buffer for the dequeued item.
    /// @return @c osal::ok() on success, @c error_code::timeout if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Copies the front item of the queue without removing it.
    /// @details Calls @c k_msgq_peek().  The timeout parameter is unused because
    ///          @c k_msgq_peek() is non-blocking.
    /// @param handle         Handle of the queue.
    /// @param item           Destination buffer; receives a copy of the head item.
    /// @param timeout_ticks  Unused — peek is always non-blocking on Zephyr.
    /// @return @c osal::ok() if an item was peeked, @c error_code::would_block if the queue is empty,
    ///         or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t /*timeout_ticks*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_msgq_peek(ZK_MSGQ(handle), item);
        return (rc == 0) ? osal::ok() : osal::error_code::would_block;
    }

    /// @brief Returns the number of items currently enqueued.
    /// @details Calls @c k_msgq_num_used_get().
    /// @param handle Handle of the queue.
    /// @return Number of items waiting in the queue, or 0 if @p handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<std::size_t>(k_msgq_num_used_get(const_cast<struct k_msgq*>(ZK_MSGQ(handle))));
    }

    /// @brief Returns the number of free slots remaining in the queue.
    /// @details Calls @c k_msgq_num_free_get().
    /// @param handle Handle of the queue.
    /// @return Number of free slots, or 0 if @p handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<std::size_t>(k_msgq_num_free_get(const_cast<struct k_msgq*>(ZK_MSGQ(handle))));
    }

    // ---------------------------------------------------------------------------
    // Timer
    // ---------------------------------------------------------------------------

    struct zephyr_timer_ctx
    {
        osal_timer_callback_t fn;
        void*                 arg;
        osal::tick_t          period_ticks;
        bool                  auto_reload;
    };
    static zephyr_timer_ctx timer_ctx[OSAL_ZEPHYR_MAX_TIMERS];

    /// @brief Creates a software timer backed by a Zephyr @c k_timer.
    /// @details Allocates a @c k_timer from the static pool and a @c zephyr_timer_ctx slot,
    ///          registers the expiry callback via @c k_timer_user_data_set(), and calls
    ///          @c k_timer_init().  The timer is not started until @c osal_timer_start() is called.
    /// @param handle       Output handle receiving the initialized @c k_timer pointer.
    /// @param callback     Function invoked on each expiry.
    /// @param arg          User argument forwarded to @p callback.
    /// @param period_ticks Timer period expressed in kernel ticks.
    /// @param auto_reload  If @c true the timer repeats; if @c false it fires once.
    /// @return @c osal::ok() on success, or @c error_code::out_of_resources if any pool is exhausted.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        struct k_timer* t = acquire_from_pool(zephyr_timers, zephyr_timer_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        // Find a ctx slot.
        zephyr_timer_ctx* ctx = nullptr;
        for (auto& c : timer_ctx)
        {
            if (c.fn == nullptr)
            {
                ctx = &c;
                break;
            }
        }
        if (ctx == nullptr)
        {
            release_to_pool(zephyr_timers, zephyr_timer_used, t);
            return osal::error_code::out_of_resources;
        }
        ctx->fn           = callback;
        ctx->arg          = arg;
        ctx->period_ticks = period_ticks;
        ctx->auto_reload  = auto_reload;

        auto expiry = [](struct k_timer* tmr) noexcept
        {
            auto* c = static_cast<zephyr_timer_ctx*>(k_timer_user_data_get(tmr));
            if (c != nullptr && c->fn != nullptr)
            {
                c->fn(c->arg);
            }
        };

        k_timer_init(t, expiry, nullptr);
        k_timer_user_data_set(t, static_cast<void*>(ctx));

        // Store period in handle so start() can use it.
        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    /// @brief Stops and destroys a timer, returning it to the static pool.
    /// @details Calls @c k_timer_stop(), clears the context callback to mark the slot free,
    ///          then releases the @c k_timer pool entry.
    /// @param handle Handle of the timer to destroy; a @c nullptr or already-null handle is ignored.
    /// @return Always @c osal::ok().
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        k_timer_stop(ZK_TIMER(handle));
        auto* ctx = static_cast<zephyr_timer_ctx*>(k_timer_user_data_get(ZK_TIMER(handle)));
        if (ctx != nullptr)
        {
            ctx->fn = nullptr;
        }
        release_to_pool(zephyr_timers, zephyr_timer_used, ZK_TIMER(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Starts or restarts the timer.
    /// @details Reads the period and auto-reload flag from the stored @c zephyr_timer_ctx and
    ///          calls @c k_timer_start().  For one-shot timers the repeat period is @c K_NO_WAIT.
    /// @param handle Handle of the timer to start.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*             ctx      = static_cast<zephyr_timer_ctx*>(k_timer_user_data_get(ZK_TIMER(handle)));
        const k_timeout_t duration = K_TICKS(static_cast<k_ticks_t>(ctx->period_ticks));
        const k_timeout_t period   = ctx->auto_reload ? duration : K_NO_WAIT;
        k_timer_start(ZK_TIMER(handle), duration, period);
        return osal::ok();
    }

    /// @brief Stops a running timer.
    /// @details Calls @c k_timer_stop().  Has no effect if the timer is already stopped.
    /// @param handle Handle of the timer to stop.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_timer_stop(ZK_TIMER(handle));
        return osal::ok();
    }

    /// @brief Resets the timer, restarting it from the beginning of its period.
    /// @details Delegates to @c osal_timer_start(), which calls @c k_timer_start() with the
    ///          stored period.
    /// @param handle Handle of the timer to reset.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        return osal_timer_start(handle);
    }

    /// @brief Changes the timer period and immediately restarts it.
    /// @details Calls @c k_timer_start() with both the initial duration and the repeat period
    ///          set to @p new_period_ticks.  The old auto-reload state is superseded.
    /// @param handle          Handle of the timer.
    /// @param new_period_ticks New period expressed in kernel ticks.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_timer_start(ZK_TIMER(handle), K_TICKS(static_cast<k_ticks_t>(new_period_ticks)),
                      K_TICKS(static_cast<k_ticks_t>(new_period_ticks)));
        return osal::ok();
    }

    /// @brief Checks whether the timer is currently running.
    /// @details Calls @c k_timer_remaining_get(); a non-zero result indicates an active timer.
    /// @param handle Handle of the timer.
    /// @return @c true if the timer is running, @c false if stopped or @p handle is invalid.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        return k_timer_remaining_get(const_cast<struct k_timer*>(ZK_TIMER(handle))) > 0U;
    }

    // ---------------------------------------------------------------------------
    // Event flags (native — Zephyr k_event, requires CONFIG_EVENTS=y)
    // ---------------------------------------------------------------------------

    static struct k_event zephyr_events[OSAL_ZEPHYR_MAX_EVENTS];
    static bool           zephyr_event_used[OSAL_ZEPHYR_MAX_EVENTS];

    /// @brief Creates an event-flags object backed by a Zephyr @c k_event.
    /// @details Allocates a @c k_event from the static pool and calls @c k_event_init().
    ///          Requires @c CONFIG_EVENTS=y in the Zephyr build.
    /// @param handle Output handle receiving the initialized @c k_event pointer.
    /// @return @c osal::ok() on success, @c error_code::out_of_resources if the pool is full,
    ///         or @c error_code::invalid_argument if @p handle is @c nullptr.
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (!handle)
        {
            return osal::error_code::invalid_argument;
        }
        struct k_event* ev = nullptr;
        for (std::size_t i = 0U; i < OSAL_ZEPHYR_MAX_EVENTS; ++i)
        {
            if (!zephyr_event_used[i])
            {
                zephyr_event_used[i] = true;
                ev                   = &zephyr_events[i];
                break;
            }
        }
        if (ev == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        k_event_init(ev);
        handle->native = static_cast<void*>(ev);
        return osal::ok();
    }

    /// @brief Destroys an event-flags object and returns it to the static pool.
    /// @param handle Handle of the event-flags object; a @c nullptr or already-null handle is ignored.
    /// @return Always @c osal::ok().
    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        auto* ev = static_cast<struct k_event*>(handle->native);
        for (std::size_t i = 0U; i < OSAL_ZEPHYR_MAX_EVENTS; ++i)
        {
            if (&zephyr_events[i] == ev)
            {
                zephyr_event_used[i] = false;
                break;
            }
        }
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Sets (posts) event bits, waking any waiters whose mask is satisfied.
    /// @details Calls @c k_event_post().
    /// @param handle Handle of the event-flags object.
    /// @param bits   Bitmask of event bits to set.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        k_event_post(static_cast<struct k_event*>(handle->native), static_cast<uint32_t>(bits));
        return osal::ok();
    }

    /// @brief Clears the specified event bits.
    /// @details Calls @c k_event_clear().
    /// @param handle Handle of the event-flags object.
    /// @param bits   Bitmask of event bits to clear.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        k_event_clear(static_cast<struct k_event*>(handle->native), static_cast<uint32_t>(bits));
        return osal::ok();
    }

    /// @brief Returns the current event-flag bitmask without waiting.
    /// @details Reads the @c .events field of the underlying @c k_event atomically.
    /// @param handle Handle of the event-flags object.
    /// @return Current bitmask of set bits, or 0 if @p handle is invalid.
    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        // k_event stores events in the .events field — read atomically.
        return static_cast<osal::event_bits_t>(static_cast<const struct k_event*>(handle->native)->events);
    }

    /// @brief Waits until at least one of the specified event bits is set.
    /// @details Calls @c k_event_wait() with @c reset=false.  When @p clear_on_exit is @c true
    ///          the matched bits are cleared manually via @c k_event_clear() after the wait succeeds.
    ///          Returns @c error_code::would_block when called with @c NO_WAIT and no bits match.
    /// @param handle      Handle of the event-flags object.
    /// @param wait_bits   Bitmask of bits to wait for (any of these).
    /// @param actual      Optional output; receives the matched bits on success.
    /// @param clear_on_exit If @c true, matched bits are atomically cleared after waking.
    /// @param timeout     Maximum wait time in ticks.
    /// @return @c osal::ok() on success, @c error_code::timeout on deadline,
    ///         @c error_code::would_block when @c NO_WAIT and no bits are set,
    ///         or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* ev = static_cast<struct k_event*>(handle->native);

        // k_event_wait: returns the matched event bits; 0 on timeout.
        // NOTE: Zephyr's 'reset' param clears ALL events BEFORE waiting,
        //       which is NOT the OSAL clear_on_exit semantics. We always
        //       pass reset=false and manually clear matched bits on success.
        const uint32_t matched = k_event_wait(ev, static_cast<uint32_t>(wait_bits), false, to_zephyr_timeout(timeout));
        if (actual != nullptr)
        {
            *actual = static_cast<osal::event_bits_t>(matched);
        }
        if (matched == 0U)
        {
            return (timeout == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
        }
        if (clear_on_exit)
        {
            k_event_clear(ev, matched);
        }
        return osal::ok();
    }

    /// @brief Waits until all of the specified event bits are simultaneously set.
    /// @details Calls @c k_event_wait_all() with @c reset=false.  When @p clear_on_exit is @c true
    ///          the matched bits are cleared manually via @c k_event_clear() after the wait succeeds.
    ///          Returns @c error_code::would_block when called with @c NO_WAIT and not all bits match.
    /// @param handle       Handle of the event-flags object.
    /// @param wait_bits    Bitmask of bits that must ALL be set before waking.
    /// @param actual       Optional output; receives the full matched bitmask on success.
    /// @param clear_on_exit If @c true, matched bits are cleared after waking.
    /// @param timeout      Maximum wait time in ticks.
    /// @return @c osal::ok() on success, @c error_code::timeout on deadline,
    ///         @c error_code::would_block when @c NO_WAIT and the condition is not met,
    ///         or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* ev = static_cast<struct k_event*>(handle->native);

        // k_event_wait_all: waits until ALL bits in wait_bits are set.
        // Same reset caveat as wait_any — always false, clear manually.
        const uint32_t matched =
            k_event_wait_all(ev, static_cast<uint32_t>(wait_bits), false, to_zephyr_timeout(timeout));
        if (actual != nullptr)
        {
            *actual = static_cast<osal::event_bits_t>(matched);
        }
        if (matched == 0U)
        {
            return (timeout == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
        }
        if (clear_on_exit)
        {
            k_event_clear(ev, matched);
        }
        return osal::ok();
    }

    /// @brief Sets event bits from an ISR context.
    /// @details Calls @c k_event_post(), which is ISR-safe on Zephyr.
    /// @param handle Handle of the event-flags object.
    /// @param bits   Bitmask of event bits to set.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        // k_event_post is ISR-safe in Zephyr.
        k_event_post(static_cast<struct k_event*>(handle->native), static_cast<uint32_t>(bits));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not natively supported on Zephyr
    // ---------------------------------------------------------------------------

    /// @brief Not supported on Zephyr — always returns @c error_code::not_supported.
    /// @param[out] handle Unused.
    /// @return @c error_code::not_supported.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Not supported on Zephyr — always returns @c error_code::not_supported.
    /// @param handle Unused.
    /// @return @c error_code::not_supported.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Not supported on Zephyr — always returns @c error_code::not_supported.
    /// @param handle Unused.
    /// @return @c error_code::not_supported.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Not supported on Zephyr — always returns @c error_code::not_supported.
    /// @param handle Unused.
    /// @return @c error_code::not_supported.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Not supported on Zephyr — always returns @c error_code::not_supported.
    /// @details Sets @p *n to 0 if non-null.
    /// @param n Optional output; set to 0.
    /// @return @c error_code::not_supported.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t*, int*, std::size_t, std::size_t* n,
                                    osal::tick_t) noexcept
    {
        if (n)
            *n = 0U;
        return osal::error_code::not_supported;
    }

    // ---------------------------------------------------------------------------
    // Condition variable (native — k_condvar)
    // ---------------------------------------------------------------------------

    struct zephyr_condvar_obj
    {
        struct k_condvar cv;
        bool             valid;
    };

    static zephyr_condvar_obj condvar_pool[OSAL_ZEPHYR_MAX_CONDVARS];

    static zephyr_condvar_obj* condvar_alloc() noexcept
    {
        for (auto& c : condvar_pool)
        {
            if (!c.valid)
            {
                c.valid = true;
                return &c;
            }
        }
        return nullptr;
    }

    /// @brief Creates a condition variable backed by a Zephyr @c k_condvar.
    /// @details Allocates a @c zephyr_condvar_obj from the static pool and calls @c k_condvar_init().
    /// @param handle Output handle receiving the initialized condvar pointer.
    /// @return @c osal::ok() on success, @c error_code::out_of_resources if the pool is full,
    ///         or @c error_code::invalid_argument if @p handle is @c nullptr.
    osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle)
            return osal::error_code::invalid_argument;
        auto* obj = condvar_alloc();
        if (!obj)
            return osal::error_code::out_of_resources;
        k_condvar_init(&obj->cv);
        handle->native = obj;
        return osal::ok();
    }

    /// @brief Destroys a condition variable and returns its pool slot.
    /// @param handle Handle of the condvar to destroy; a @c nullptr or already-null handle is ignored.
    /// @return Always @c osal::ok().
    osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::ok();
        auto* obj      = static_cast<zephyr_condvar_obj*>(handle->native);
        obj->valid     = false;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Atomically releases the mutex and waits for a condition variable signal.
    /// @details Calls @c k_condvar_wait(), which atomically releases @p mutex and blocks.
    ///          On waking (or timeout) the mutex is re-acquired before returning.
    /// @param handle  Handle of the condition variable.
    /// @param mutex   Handle of the mutex to release atomically during the wait.
    /// @param timeout Maximum wait time in ticks.
    /// @return @c osal::ok() on success, @c error_code::timeout if the deadline expires,
    ///         or @c error_code::not_initialized if either handle is invalid.
    osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                                   osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native || !mutex || !mutex->native)
            return osal::error_code::not_initialized;
        auto* obj = static_cast<zephyr_condvar_obj*>(handle->native);
        // Zephyr OSAL mutex handle stores a struct k_mutex* directly.
        auto*     mtx = static_cast<struct k_mutex*>(mutex->native);
        const int rc  = k_condvar_wait(&obj->cv, mtx, to_zephyr_timeout(timeout));
        if (rc == 0)
            return osal::ok();
        if (rc == -EAGAIN || rc == -ETIMEDOUT)
            return osal::error_code::timeout;
        return osal::error_code::unknown;
    }

    /// @brief Wakes one thread waiting on the condition variable.
    /// @details Calls @c k_condvar_signal().
    /// @param handle Handle of the condition variable.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::error_code::not_initialized;
        auto* obj = static_cast<zephyr_condvar_obj*>(handle->native);
        k_condvar_signal(&obj->cv);
        return osal::ok();
    }

    /// @brief Wakes all threads waiting on the condition variable.
    /// @details Calls @c k_condvar_broadcast().
    /// @param handle Handle of the condition variable.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::error_code::not_initialized;
        auto* obj = static_cast<zephyr_condvar_obj*>(handle->native);
        k_condvar_broadcast(&obj->cv);
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Work queue (native — Zephyr k_work_queue)
    // ---------------------------------------------------------------------------

    struct zephyr_wq_entry
    {
        struct k_work           work;
        osal_work_func_t        func;
        void*                   arg;
        struct zephyr_wq_obj_s* parent;
        volatile bool           in_use;
    };

    struct zephyr_wq_obj_s
    {
        struct k_work_q workq;
        zephyr_wq_entry entries[OSAL_ZEPHYR_WQ_MAX_DEPTH];
        std::size_t     capacity;
        struct k_sem    flush_sem;
    };

    static zephyr_wq_obj_s zephyr_wq_pool[OSAL_ZEPHYR_MAX_WORK_QUEUES];
    static bool            zephyr_wq_used[OSAL_ZEPHYR_MAX_WORK_QUEUES];

    static zephyr_wq_obj_s* zephyr_wq_acquire() noexcept
    {
        for (std::size_t i = 0U; i < OSAL_ZEPHYR_MAX_WORK_QUEUES; ++i)
        {
            if (!zephyr_wq_used[i])
            {
                zephyr_wq_used[i] = true;
                return &zephyr_wq_pool[i];
            }
        }
        return nullptr;
    }

    static void zephyr_wq_release(zephyr_wq_obj_s* p) noexcept
    {
        for (std::size_t i = 0U; i < OSAL_ZEPHYR_MAX_WORK_QUEUES; ++i)
        {
            if (&zephyr_wq_pool[i] == p)
            {
                zephyr_wq_used[i] = false;
                return;
            }
        }
    }

    static void zephyr_wq_handler(struct k_work* work) noexcept
    {
        auto* e = CONTAINER_OF(work, zephyr_wq_entry, work);
        if (e->func != nullptr)
        {
            e->func(e->arg);
        }
        else
        {
            // Sentinel — flush complete.
            k_sem_give(&e->parent->flush_sem);
        }
        e->in_use = false;
    }

    /// @brief Creates a work queue backed by a Zephyr @c k_work_queue.
    /// @details Allocates a @c zephyr_wq_obj_s from the static pool, initialises each
    ///          @c zephyr_wq_entry with @c k_work_init(), and starts the queue thread via
    ///          @c k_work_queue_start().
    /// @param handle      Output handle receiving the work-queue object pointer.
    /// @param stack       Caller-supplied stack for the work-queue thread.
    /// @param stack_bytes Size of @p stack in bytes.
    /// @param depth       Maximum number of pending work items; must not exceed
    ///                    @c OSAL_ZEPHYR_WQ_MAX_DEPTH.
    /// @return @c osal::ok() on success, @c error_code::out_of_resources if the pool is full,
    ///         or @c error_code::invalid_argument for invalid parameters or depth overflow.
    osal::result osal_work_queue_create(osal::active_traits::work_queue_handle_t* handle, void* stack,
                                        std::size_t stack_bytes, std::size_t depth, const char* /*name*/) noexcept
    {
        if (!handle || !stack || stack_bytes == 0U || depth == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        auto* wq = zephyr_wq_acquire();
        if (!wq)
        {
            return osal::error_code::out_of_resources;
        }
        if (depth > OSAL_ZEPHYR_WQ_MAX_DEPTH)
        {
            zephyr_wq_release(wq);
            return osal::error_code::invalid_argument;
        }
        wq->capacity = depth;
        for (std::size_t i = 0U; i < depth; ++i)
        {
            k_work_init(&wq->entries[i].work, zephyr_wq_handler);
            wq->entries[i].parent = wq;
            wq->entries[i].in_use = false;
        }
        k_sem_init(&wq->flush_sem, 0, 1);
        k_work_queue_start(&wq->workq, static_cast<k_thread_stack_t*>(stack), stack_bytes,
                           CONFIG_NUM_PREEMPT_PRIORITIES - 1, nullptr);
        handle->native = static_cast<void*>(wq);
        return osal::ok();
    }

    /// @brief Drains, stops, and destroys a work queue.
    /// @details Calls @c k_work_queue_drain() to wait for all pending items,
    ///          then @c k_thread_abort() to stop the queue thread, and finally
    ///          releases the pool slot.  The internal @c k_work_q is zeroed so the
    ///          slot can be reused.
    /// @param handle Handle of the work queue to destroy; a @c nullptr or already-null handle is ignored.
    /// @return Always @c osal::ok().
    osal::result osal_work_queue_destroy(osal::active_traits::work_queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        auto* wq = static_cast<zephyr_wq_obj_s*>(handle->native);
        // Drain pending work, plug to stop new submissions.
        (void)k_work_queue_drain(&wq->workq, true);
        // Abort the work queue's internal thread so the pool entry can be reused.
        k_thread_abort(&wq->workq.thread);
        // Reset the k_work_q so K_WORK_QUEUE_STARTED_BIT is cleared for reuse.
        (void)std::memset(&wq->workq, 0, sizeof(wq->workq));
        zephyr_wq_release(wq);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Submits a work item to the work queue.
    /// @details Finds a free @c zephyr_wq_entry slot, populates it, marks it in-use, and
    ///          calls @c k_work_submit_to_queue().
    /// @param handle Handle of the work queue.
    /// @param func   Callback function to execute on the work queue thread.
    /// @param arg    Argument forwarded to @p func.
    /// @return @c osal::ok() on success, @c error_code::overflow if all slots are busy,
    ///         or @c error_code::invalid_argument for invalid inputs.
    osal::result osal_work_queue_submit(osal::active_traits::work_queue_handle_t* handle, osal_work_func_t func,
                                        void* arg) noexcept
    {
        if (!handle || !handle->native || !func)
        {
            return osal::error_code::invalid_argument;
        }
        auto* wq = static_cast<zephyr_wq_obj_s*>(handle->native);
        for (std::size_t i = 0U; i < wq->capacity; ++i)
        {
            if (!wq->entries[i].in_use)
            {
                wq->entries[i].func   = func;
                wq->entries[i].arg    = arg;
                wq->entries[i].in_use = true;
                k_work_submit_to_queue(&wq->workq, &wq->entries[i].work);
                return osal::ok();
            }
        }
        return osal::error_code::overflow;
    }

    /// @brief Submits a work item from an ISR context.
    /// @details Delegates to @c osal_work_queue_submit() — @c k_work_submit_to_queue() is
    ///          ISR-safe on Zephyr.
    /// @param handle Handle of the work queue.
    /// @param func   Callback function to execute.
    /// @param arg    Argument forwarded to @p func.
    /// @return @c osal::ok() on success, or @c error_code::overflow if all slots are busy.
    osal::result osal_work_queue_submit_from_isr(osal::active_traits::work_queue_handle_t* handle,
                                                 osal_work_func_t func, void* arg) noexcept
    {
        // k_work_submit_to_queue is ISR-safe on Zephyr.
        return osal_work_queue_submit(handle, func, arg);
    }

    /// @brief Waits until all currently queued work items have been executed.
    /// @details Submits a sentinel entry (func == nullptr) whose handler calls
    ///          @c k_sem_give() on the queue's flush semaphore, then waits on that
    ///          semaphore with the given timeout.
    /// @param handle  Handle of the work queue.
    /// @param timeout Maximum wait time in ticks.
    /// @return @c osal::ok() when the flush sentinel is executed, @c error_code::timeout
    ///         on deadline, @c error_code::overflow if no slot is available for the sentinel,
    ///         or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_work_queue_flush(osal::active_traits::work_queue_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* wq = static_cast<zephyr_wq_obj_s*>(handle->native);
        // Submit a sentinel item and wait on the semaphore.
        for (std::size_t i = 0U; i < wq->capacity; ++i)
        {
            if (!wq->entries[i].in_use)
            {
                wq->entries[i].func   = nullptr;
                wq->entries[i].arg    = nullptr;
                wq->entries[i].in_use = true;
                k_work_submit_to_queue(&wq->workq, &wq->entries[i].work);
                int rc = k_sem_take(&wq->flush_sem, to_zephyr_timeout(timeout));
                return (rc == 0) ? osal::ok() : osal::result{osal::error_code::timeout};
            }
        }
        return osal::error_code::overflow;
    }

    /// @brief Cancels all pending work items in the queue.
    /// @details Calls @c k_work_cancel() on every in-use entry and clears the in-use flag.
    ///          Items that are already executing cannot be cancelled.
    /// @param handle Handle of the work queue.
    /// @return @c osal::ok() on success, or @c error_code::not_initialized if @p handle is invalid.
    osal::result osal_work_queue_cancel_all(osal::active_traits::work_queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* wq = static_cast<zephyr_wq_obj_s*>(handle->native);
        for (std::size_t i = 0U; i < wq->capacity; ++i)
        {
            if (wq->entries[i].in_use)
            {
                k_work_cancel(&wq->entries[i].work);
                wq->entries[i].in_use = false;
            }
        }
        return osal::ok();
    }

    /// @brief Returns the number of work items currently marked as in-use (pending or executing).
    /// @details Counts @c zephyr_wq_entry slots where @c in_use is @c true.
    /// @param handle Handle of the work queue.
    /// @return Count of in-use entries, or 0 if @p handle is invalid.
    std::size_t osal_work_queue_pending(const osal::active_traits::work_queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        auto*       wq = static_cast<const zephyr_wq_obj_s*>(handle->native);
        std::size_t n  = 0U;
        for (std::size_t i = 0U; i < wq->capacity; ++i)
        {
            if (wq->entries[i].in_use)
            {
                ++n;
            }
        }
        return n;
    }

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
#include "./zephyr_spinlock.inl"

    // ---------------------------------------------------------------------------
    // Barrier
    // ---------------------------------------------------------------------------
#include "../common/emulated_barrier.inl"

    // ---------------------------------------------------------------------------
    // Task notification
    // ---------------------------------------------------------------------------
#include "../common/emulated_task_notify.inl"

}  // extern "C"
