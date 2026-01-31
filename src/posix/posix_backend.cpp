// SPDX-License-Identifier: Apache-2.0
/// @file posix_backend.cpp
/// @brief POSIX implementation of all OSAL C-linkage functions
/// @details Targets any POSIX-conforming system: embedded Linux, macOS,
///          QNX, VxWorks/RTP, etc.  Does NOT use Linux-specific syscalls
///          (no epoll, no eventfd, no timerfd).
///
///          Primitives:
///          - thread   → pthread
///          - mutex    → pthread_mutex (PTHREAD_MUTEX_RECURSIVE)
///          - semaphore→ sem_t (UNNAMED)
///          - queue    → circular buffer protected by pthread_mutex + pthread_cond
///          - timer    → POSIX timer_t (SIGEV_THREAD) OR emulated via pthread
///          - event_flags → pthread_mutex + pthread_cond_broadcast + uint32_t
///          - wait_set → poll() on file descriptors
///
///          @note Dynamic allocation (malloc): used for timer_create SIGEV_THREAD
///          context objects only.  All other objects are caller-supplied or pooled.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#if !defined(OSAL_POSIXLIKE_BACKEND_SELECTED) && !defined(OSAL_BACKEND_POSIX)
#define OSAL_BACKEND_POSIX
#endif
#include <osal/osal.hpp>
#include "../common/backend_timeout_adapter.hpp"

#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <atomic>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Determine the best clock for condvars.
/// macOS lacks pthread_condattr_setclock, so fall back to CLOCK_REALTIME.
#if defined(__APPLE__) || !defined(_POSIX_MONOTONIC_CLOCK)
static constexpr clockid_t kCondClock = CLOCK_REALTIME;
#else
static constexpr clockid_t kCondClock = CLOCK_MONOTONIC;
#endif

/// @brief Build an absolute timespec relative to the given clock.
static struct timespec ms_to_abs_timespec(clockid_t clk, osal::tick_t timeout_ticks) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(clk, timeout_ticks);
}

/// @brief Absolute timespec for sem_timedwait / pthread_mutex_timedlock.
/// POSIX mandates these use CLOCK_REALTIME (no clockid parameter).
static struct timespec ms_to_abs_timespec_realtime(osal::tick_t timeout_ticks) noexcept
{
    return ms_to_abs_timespec(CLOCK_REALTIME, timeout_ticks);
}

/// @brief Absolute timespec for condvar waits (uses MONOTONIC where available).
static struct timespec ms_to_abs_timespec_cond(osal::tick_t timeout_ticks) noexcept
{
    return ms_to_abs_timespec(kCondClock, timeout_ticks);
}

/// @brief Init a condvar with CLOCK_MONOTONIC where supported.
static int cond_init_monotonic(pthread_cond_t* cond) noexcept
{
#if defined(__APPLE__) || !defined(_POSIX_MONOTONIC_CLOCK)
    return pthread_cond_init(cond, nullptr);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    const int rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
#endif
}

// ---------------------------------------------------------------------------
// Shared-include macro contracts for posix_rwlock.inl, posix_condvar.inl,
// and posix_pthread_work_queue.inl.
// POSIX: rwlock timed waits use CLOCK_REALTIME (mandated by pthread_rwlock_timedrdlock).
// POSIX: condvar/work-queue timed waits follow kCondClock (MONOTONIC or REALTIME).
// ---------------------------------------------------------------------------
#define OSAL_POSIX_RW_ABS(t) ms_to_abs_timespec_realtime(t)
#define OSAL_POSIX_COND_ABS(t) ms_to_abs_timespec_cond(t)

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

#include "../common/posix/posix_clock.inl"

    /// @brief Return the current CLOCK_MONOTONIC time in milliseconds (1 tick = 1 ms).
    /// @return Monotonic millisecond tick count via osal_clock_monotonic_ms().
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(osal_clock_monotonic_ms());
    }

    /// @brief Return the tick period in microseconds (1 tick = 1 ms on POSIX).
    /// @return 1000 always.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1'000U;
    }  // 1 ms per tick

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    struct posix_thread_ctx
    {
        void (*entry)(void*);
        void* arg;
    };

    static void* posix_thread_entry(void* ctx_raw) noexcept
    {
        auto* ctx = static_cast<posix_thread_ctx*>(ctx_raw);
        ctx->entry(ctx->arg);
        delete ctx;
        return nullptr;
    }

    /// @brief Create a POSIX pthread.
    /// @details Uses SCHED_FIFO when @p priority differs from PRIORITY_NORMAL.
    ///          On Linux, sets CPU affinity via pthread_setaffinity_np() if @p affinity != AFFINITY_ANY.
    /// @param handle      Output handle stores pthread_t as a void pointer.
    /// @param entry       Thread entry function.
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority mapped to SCHED_FIFO range.
    /// @param affinity    CPU affinity bitmask (Linux only; ignored elsewhere).
    /// @param stack_bytes Stack size in bytes; optional @p stack used if non-null.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        assert(handle && entry);

        auto* ctx = new (std::nothrow) posix_thread_ctx{entry, arg};
        if (!ctx)
        {
            return osal::error_code::out_of_resources;
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        if (stack && stack_bytes > 0U)
        {
            pthread_attr_setstack(&attr, stack, stack_bytes);
        }

        // Set FIFO scheduling if priority != default.
        if (priority != osal::PRIORITY_NORMAL)
        {
            struct sched_param sp;
            const int          policy = SCHED_FIFO;
            const int          max_p  = sched_get_priority_max(policy);
            const int          min_p  = sched_get_priority_min(policy);
            sp.sched_priority =
                min_p +
                static_cast<int>((static_cast<std::uint32_t>(priority) * static_cast<std::uint32_t>(max_p - min_p)) /
                                 static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
            pthread_attr_setschedpolicy(&attr, policy);
            pthread_attr_setschedparam(&attr, &sp);
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        }

        pthread_t tid;
        const int rc = pthread_create(&tid, &attr, posix_thread_entry, ctx);
        pthread_attr_destroy(&attr);

        if (rc != 0)
        {
            delete ctx;
            return osal::error_code::out_of_resources;
        }

#if defined(__linux__)
        if (affinity != osal::AFFINITY_ANY)
        {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            for (int cpu = 0; cpu < 64; ++cpu)
            {
                if ((affinity & (1U << static_cast<unsigned>(cpu))) != 0U)
                {
                    CPU_SET(cpu, &cpuset);
                }
            }
            pthread_setaffinity_np(tid, sizeof(cpuset), &cpuset);
        }
#else
        (void)affinity;
#endif

        handle->native = reinterpret_cast<void*>(tid);
        return osal::ok();
    }

    /// @brief Wait for a thread to exit via pthread_join() or pthread_timedjoin_np() (Linux/POSIX.1).
    /// @param handle         Thread handle.
    /// @param timeout_ticks  Maximum wait; WAIT_FOREVER uses blocking pthread_join().
    /// @return osal::ok() on success; osal::error_code::timeout, ::unknown, or ::not_initialized.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        const pthread_t tid = reinterpret_cast<pthread_t>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            const int rc = pthread_join(tid, nullptr);
            if (rc != 0)
            {
                return osal::error_code::unknown;
            }
            handle->native = nullptr;
            return osal::ok();
        }

#if defined(_POSIX_TIMEOUTS) && (_POSIX_TIMEOUTS >= 200112L)
        const struct timespec abs = ms_to_abs_timespec_realtime(timeout_ticks);
        const int             rc  = pthread_timedjoin_np(tid, nullptr, &abs);  // Linux extension
        if (rc == ETIMEDOUT)
        {
            return osal::error_code::timeout;
        }
        if (rc != 0)
        {
            return osal::error_code::unknown;
        }
        handle->native = nullptr;
        return osal::ok();
#else
        const int rc = pthread_join(tid, nullptr);
        (void)timeout_ticks;
        if (rc != 0)
        {
            return osal::error_code::unknown;
        }
        handle->native = nullptr;
        return osal::ok();
#endif
    }

    /// @brief Detach the pthread via pthread_detach().
    /// @param handle Thread handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        pthread_detach(reinterpret_cast<pthread_t>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change the thread's scheduler priority using its current policy.
    /// @param handle   Thread handle.
    /// @param priority New OSAL priority mapped to the policy's range.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::permission_denied.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        struct sched_param sp;
        int                policy;
        pthread_getschedparam(reinterpret_cast<pthread_t>(handle->native), &policy, &sp);
        const int max_p = sched_get_priority_max(policy);
        const int min_p = sched_get_priority_min(policy);
        sp.sched_priority =
            min_p +
            static_cast<int>((static_cast<std::uint32_t>(priority) * static_cast<std::uint32_t>(max_p - min_p)) /
                             static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
        const int rc = pthread_setschedparam(reinterpret_cast<pthread_t>(handle->native), policy, &sp);
        return (rc == 0) ? osal::ok() : osal::error_code::permission_denied;
    }

    /// @brief Set CPU affinity via pthread_setaffinity_np() (Linux-only).
    /// @param handle   Thread handle.
    /// @param affinity CPU bitmask; each set bit represents a processor to allow.
    /// @return osal::ok() on Linux success; osal::error_code::not_supported on non-Linux.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept
    {
#if defined(__linux__)
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        for (int cpu = 0; cpu < 64; ++cpu)
        {
            if ((affinity & (1U << static_cast<unsigned>(cpu))) != 0U)
            {
                CPU_SET(cpu, &cpuset);
            }
        }
        const int rc = pthread_setaffinity_np(reinterpret_cast<pthread_t>(handle->native), sizeof(cpuset), &cpuset);
        return (rc == 0) ? osal::ok() : osal::error_code::unknown;
#else
        (void)handle;
        (void)affinity;
        return osal::error_code::not_supported;
#endif
    }

    /// @brief Suspend a thread (not supported on POSIX through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported on POSIX through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Yield the current thread via sched_yield().
    void osal_thread_yield() noexcept
    {
        sched_yield();
    }
    /// @brief Sleep for at least @p ms milliseconds via nanosleep().
    /// @param ms Delay in milliseconds.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        struct timespec ts
        {
            static_cast<time_t>(ms / 1000U), static_cast<long>((ms % 1000U) * 1'000'000L)
        };
        nanosleep(&ts, nullptr);
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    /// @brief Allocate and initialise a pthread_mutex_t; ERRORCHECK or RECURSIVE type.
    /// @param handle    Output handle owns a heap-allocated pthread_mutex_t.
    /// @param recursive True selects PTHREAD_MUTEX_RECURSIVE; false selects ERRORCHECK.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept
    {
        auto* m = new (std::nothrow) pthread_mutex_t;
        if (!m)
        {
            return osal::error_code::out_of_resources;
        }

        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
        const int rc = pthread_mutex_init(m, &attr);
        pthread_mutexattr_destroy(&attr);

        if (rc != 0)
        {
            delete m;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    /// @brief Destroy the pthread_mutex_t and free heap storage.
    /// @param handle Mutex handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        auto* m = static_cast<pthread_mutex_t*>(handle->native);
        pthread_mutex_destroy(m);
        delete m;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire the mutex; uses pthread_mutex_timedlock() with CLOCK_REALTIME for finite timeouts.
    /// @param handle        Mutex handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks; WAIT_FOREVER uses pthread_mutex_lock().
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown on failure.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<pthread_mutex_t*>(handle->native);
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            return (pthread_mutex_lock(m) == 0) ? osal::ok() : osal::error_code::unknown;
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int rc = pthread_mutex_trylock(m);
            return (rc == 0) ? osal::ok() : osal::error_code::would_block;
        }
        const struct timespec abs = ms_to_abs_timespec_realtime(timeout_ticks);
        const int             rc  = pthread_mutex_timedlock(m, &abs);
        if (rc == 0)
        {
            return osal::ok();
        }
        if (rc == ETIMEDOUT)
        {
            return osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    /// @brief Try to acquire the mutex without blocking.
    /// @param handle Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::would_block otherwise.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release the mutex via pthread_mutex_unlock().
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::not_owner on failure.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = pthread_mutex_unlock(static_cast<pthread_mutex_t*>(handle->native));
        return (rc == 0) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore (unnamed POSIX sem_t)
    // ---------------------------------------------------------------------------

    /// @brief Allocate and initialise an unnamed POSIX sem_t via sem_init().
    /// @param handle        Output handle owns a heap-allocated sem_t.
    /// @param initial_count Starting token count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        auto* s = new (std::nothrow) sem_t;
        if (!s)
        {
            return osal::error_code::out_of_resources;
        }
        if (sem_init(s, 0, initial_count) != 0)
        {
            delete s;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(s);
        return osal::ok();
    }

    /// @brief Destroy the sem_t via sem_destroy() and free heap storage.
    /// @param handle Semaphore handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        sem_destroy(static_cast<sem_t*>(handle->native));
        delete static_cast<sem_t*>(handle->native);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (post) the semaphore via sem_post().
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        return (sem_post(static_cast<sem_t*>(handle->native)) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Increment the semaphore from ISR context (sem_post is async-signal-safe on POSIX).
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // sem_post is async-signal-safe
    }

    /// @brief Decrement (wait on) the semaphore.
    /// @details Uses sem_wait() (forever), sem_trywait() (NO_WAIT), or
    ///          sem_timedwait() with CLOCK_REALTIME (timed).
    /// @param handle        Semaphore handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<sem_t*>(handle->native);
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            int rc;
            do
            {
                rc = sem_wait(s);
            } while (rc == -1 && errno == EINTR);
            return (rc == 0) ? osal::ok() : osal::error_code::unknown;
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int rc = sem_trywait(s);
            return (rc == 0) ? osal::ok() : osal::error_code::would_block;
        }
        const struct timespec abs = ms_to_abs_timespec_realtime(timeout_ticks);
        const int             rc  = sem_timedwait(s, &abs);
        if (rc == 0)
        {
            return osal::ok();
        }
        if (errno == ETIMEDOUT)
        {
            return osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    /// @brief Try to decrement the semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if a token was available; osal::error_code::would_block otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue — mutex + condvar circular buffer
    // ---------------------------------------------------------------------------

    struct posix_queue_obj
    {
        pthread_mutex_t mutex;
        pthread_cond_t  not_full;
        pthread_cond_t  not_empty;
        std::uint8_t*   buf;  ///< caller-supplied
        std::size_t     item_size;
        std::size_t     capacity;
        std::size_t     head;
        std::size_t     tail;
        std::size_t     count;
    };

    /// @brief Create a circular-buffer queue backed by caller-supplied @p buffer.
    /// @details Internal mutex and condvars use the best available monotonic clock.
    /// @param handle    Output handle owns a heap-allocated posix_queue_obj.
    /// @param buffer    Caller-provided storage for capacity × item_size bytes.
    /// @param item_size Size in bytes of each element.
    /// @param capacity  Maximum number of elements the queue can hold.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        assert(buffer && item_size > 0 && capacity > 0);
        auto* q = new (std::nothrow) posix_queue_obj{};
        if (!q)
        {
            return osal::error_code::out_of_resources;
        }
        pthread_mutex_init(&q->mutex, nullptr);
        cond_init_monotonic(&q->not_full);
        cond_init_monotonic(&q->not_empty);
        q->buf       = static_cast<std::uint8_t*>(buffer);
        q->item_size = item_size;
        q->capacity  = capacity;
        q->head = q->tail = q->count = 0U;
        handle->native               = static_cast<void*>(q);
        return osal::ok();
    }

    /// @brief Destroy the circular-buffer queue: destroy condvars, mutex, and free the object.
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        auto* q = static_cast<posix_queue_obj*>(handle->native);
        pthread_cond_destroy(&q->not_full);
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mutex);
        delete q;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send an item into the queue; blocks on the not_full condvar if the queue is full.
    /// @param handle        Queue handle.
    /// @param item          Pointer to the item to copy.
    /// @param timeout_ticks Maximum wait in OSAL ticks; NO_WAIT returns immediately if full.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<posix_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        while (q->count >= q->capacity)
        {
            if (timeout_ticks == osal::NO_WAIT)
            {
                pthread_mutex_unlock(&q->mutex);
                return osal::error_code::would_block;
            }
            if (timeout_ticks == osal::WAIT_FOREVER)
            {
                pthread_cond_wait(&q->not_full, &q->mutex);
            }
            else
            {
                const struct timespec abs = ms_to_abs_timespec_cond(timeout_ticks);
                const int             rc  = pthread_cond_timedwait(&q->not_full, &q->mutex, &abs);
                if (rc == ETIMEDOUT)
                {
                    pthread_mutex_unlock(&q->mutex);
                    return osal::error_code::timeout;
                }
            }
        }
        std::memcpy(q->buf + q->tail * q->item_size, item, q->item_size);
        q->tail = (q->tail + 1U) % q->capacity;
        q->count++;
        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->mutex);
        return osal::ok();
    }

    /// @brief Send an item non-blocking (ISR-compatible).
    /// @param handle Queue handle.
    /// @param item   Pointer to the item to copy.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive an item from the queue; blocks on the not_empty condvar if the queue is empty.
    /// @param handle        Queue handle.
    /// @param item          Buffer to copy the dequeued item into.
    /// @param timeout_ticks Maximum wait in OSAL ticks; NO_WAIT returns immediately if empty.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<posix_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        while (q->count == 0U)
        {
            if (timeout_ticks == osal::NO_WAIT)
            {
                pthread_mutex_unlock(&q->mutex);
                return osal::error_code::would_block;
            }
            if (timeout_ticks == osal::WAIT_FOREVER)
            {
                pthread_cond_wait(&q->not_empty, &q->mutex);
            }
            else
            {
                const struct timespec abs = ms_to_abs_timespec_cond(timeout_ticks);
                const int             rc  = pthread_cond_timedwait(&q->not_empty, &q->mutex, &abs);
                if (rc == ETIMEDOUT)
                {
                    pthread_mutex_unlock(&q->mutex);
                    return osal::error_code::timeout;
                }
            }
        }
        std::memcpy(item, q->buf + q->head * q->item_size, q->item_size);
        q->head = (q->head + 1U) % q->capacity;
        q->count--;
        pthread_cond_signal(&q->not_full);
        pthread_mutex_unlock(&q->mutex);
        return osal::ok();
    }

    /// @brief Receive an item non-blocking (ISR-compatible).
    /// @param handle Queue handle.
    /// @param item   Buffer to copy the dequeued item into.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Copy the front item without removing it (timeout parameter ignored).
    /// @param handle Queue handle.
    /// @param item   Buffer to copy the head item into.
    /// @return osal::ok() if an item was available; osal::error_code::would_block if the queue is empty.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t /*timeout_ticks*/) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<posix_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        if (q->count == 0U)
        {
            pthread_mutex_unlock(&q->mutex);
            return osal::error_code::would_block;
        }
        std::memcpy(item, q->buf + q->head * q->item_size, q->item_size);
        pthread_mutex_unlock(&q->mutex);
        return osal::ok();
    }

    /// @brief Return the number of items currently in the queue.
    /// @param handle Queue handle.
    /// @return Approximate item count (non-atomic read); 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        auto* q = static_cast<const posix_queue_obj*>(handle->native);
        // Non-atomic: caller must be ok with approximate count.
        return q->count;
    }

    /// @brief Return the number of free slots remaining in the queue.
    /// @param handle Queue handle.
    /// @return Approximate free count (capacity - count); 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return 0U;
        }
        auto* q = static_cast<const posix_queue_obj*>(handle->native);
        return q->capacity - q->count;
    }

    // ---------------------------------------------------------------------------
    // Timer (POSIX timer_t with SIGEV_THREAD)
    // ---------------------------------------------------------------------------

    struct posix_timer_ctx
    {
        osal_timer_callback_t fn;
        void*                 arg;
        timer_t               id;
        struct itimerspec     spec;
        bool                  auto_reload;
        std::atomic<bool>     alive{true};  ///< Guard against in-flight SIGEV_THREAD callbacks
    };

    /// @brief Create a CLOCK_MONOTONIC POSIX timer with SIGEV_THREAD callback delivery.
    /// @details For auto-reload timers, it_interval matches it_value.
    ///          The alive flag protects against stale callbacks after destroy.
    /// @param handle       Output handle owns a heap-allocated posix_timer_ctx.
    /// @param cb           Function called on each expiry (from a kernel thread).
    /// @param arg          Opaque argument forwarded to @p cb.
    /// @param period_ticks Expiry period in OSAL ticks (milliseconds).
    /// @param auto_reload  If true, sets it_interval for periodic fire.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle && cb);
        auto* ctx = new (std::nothrow) posix_timer_ctx{};
        if (!ctx)
        {
            return osal::error_code::out_of_resources;
        }
        ctx->fn          = cb;
        ctx->arg         = arg;
        ctx->auto_reload = auto_reload;

        const time_t sec              = static_cast<time_t>(period_ticks / 1000U);
        const long   nsec             = static_cast<long>((period_ticks % 1000U) * 1'000'000L);
        ctx->spec.it_value.tv_sec     = sec;
        ctx->spec.it_value.tv_nsec    = nsec;
        ctx->spec.it_interval.tv_sec  = auto_reload ? sec : 0;
        ctx->spec.it_interval.tv_nsec = auto_reload ? nsec : 0L;

        struct sigevent sev
        {
        };
        sev.sigev_notify          = SIGEV_THREAD;
        sev.sigev_value.sival_ptr = static_cast<void*>(ctx);
        sev.sigev_notify_function = [](union sigval sv)
        {
            auto* c = static_cast<posix_timer_ctx*>(sv.sival_ptr);
            if (c && c->alive.load(std::memory_order_acquire) && c->fn)
            {
                c->fn(c->arg);
            }
        };

        if (timer_create(CLOCK_MONOTONIC, &sev, &ctx->id) != 0)
        {
            delete ctx;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(ctx);
        return osal::ok();
    }

    /// @brief Disarm, mark dead, and delete the POSIX timer; yields briefly for in-flight callbacks.
    /// @param handle Timer handle; no-op if null.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        auto* ctx = static_cast<posix_timer_ctx*>(handle->native);
        // Disarm the timer first to prevent new SIGEV_THREAD callbacks.
        struct itimerspec disarm
        {
        };
        timer_settime(ctx->id, 0, &disarm, nullptr);
        // Mark dead so any in-flight callback thread bails out.
        ctx->alive.store(false, std::memory_order_release);
        timer_delete(ctx->id);
        // Brief yield to allow in-flight callback threads to finish.
        struct timespec ts
        {
            0, 2'000'000L
        };  // 2 ms
        nanosleep(&ts, nullptr);
        delete ctx;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Arm the timer via timer_settime() with the stored itimerspec.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<posix_timer_ctx*>(handle->native);
        return (timer_settime(ctx->id, 0, &ctx->spec, nullptr) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Disarm the timer by passing a zero itimerspec to timer_settime().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<posix_timer_ctx*>(handle->native);
        struct itimerspec disarm
        {
        };
        return (timer_settime(ctx->id, 0, &disarm, nullptr) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Stop and immediately restart the timer.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Update the itimerspec and immediately arm the timer with the new period.
    /// @param handle      Timer handle.
    /// @param new_period  New period in OSAL ticks (milliseconds); applied via timer_settime().
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle, osal::tick_t new_period) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto*        ctx              = static_cast<posix_timer_ctx*>(handle->native);
        const time_t sec              = static_cast<time_t>(new_period / 1000U);
        const long   nsec             = static_cast<long>((new_period % 1000U) * 1'000'000L);
        ctx->spec.it_value.tv_sec     = sec;
        ctx->spec.it_value.tv_nsec    = nsec;
        ctx->spec.it_interval.tv_sec  = ctx->auto_reload ? sec : 0;
        ctx->spec.it_interval.tv_nsec = ctx->auto_reload ? nsec : 0L;
        return (timer_settime(ctx->id, 0, &ctx->spec, nullptr) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Query whether the timer is currently armed via timer_gettime().
    /// @param handle Timer handle.
    /// @return True if it_value is non-zero.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return false;
        }
        auto* ctx = static_cast<const posix_timer_ctx*>(handle->native);
        struct itimerspec cur
        {
        };
        timer_gettime(ctx->id, &cur);
        return (cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0);
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

// ---------------------------------------------------------------------------
// Wait-set (poll())
// ---------------------------------------------------------------------------
#define OSAL_POSIX_MAX_POLL_FDS 16

    struct posix_wait_set_obj
    {
        struct pollfd fds[OSAL_POSIX_MAX_POLL_FDS];
        std::size_t   n_fds;
    };

    /// @brief Allocate a posix_wait_set_obj backed by up to OSAL_POSIX_MAX_POLL_FDS entries.
    /// @param handle Output handle owns a heap-allocated posix_wait_set_obj.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        auto* ws = new (std::nothrow) posix_wait_set_obj{};
        if (!ws)
        {
            return osal::error_code::out_of_resources;
        }
        ws->n_fds      = 0U;
        handle->native = static_cast<void*>(ws);
        return osal::ok();
    }

    /// @brief Free the posix_wait_set_obj and clear the handle.
    /// @param handle Wait-set handle; no-op if null.
    /// @return osal::ok() always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::ok();
        }
        delete static_cast<posix_wait_set_obj*>(handle->native);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Register a file descriptor and its poll event mask in the wait-set.
    /// @param handle Handle to the wait-set.
    /// @param fd     File descriptor to monitor.
    /// @param events poll() event mask (POLLIN, POLLOUT, etc.).
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the set is full.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t* handle, int fd,
                                   std::uint32_t events) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* ws = static_cast<posix_wait_set_obj*>(handle->native);
        if (ws->n_fds >= OSAL_POSIX_MAX_POLL_FDS)
        {
            return osal::error_code::out_of_resources;
        }
        ws->fds[ws->n_fds] = {fd, static_cast<short>(events), 0};
        ws->n_fds++;
        return osal::ok();
    }

    /// @brief Remove a file descriptor from the wait-set by swapping with the last entry.
    /// @param handle Handle to the wait-set.
    /// @param fd     File descriptor to remove.
    /// @return osal::ok() if found and removed; osal::error_code::invalid_argument if not found.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t* handle, int fd) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* ws = static_cast<posix_wait_set_obj*>(handle->native);
        for (std::size_t i = 0; i < ws->n_fds; ++i)
        {
            if (ws->fds[i].fd == fd)
            {
                ws->fds[i] = ws->fds[--ws->n_fds];
                return osal::ok();
            }
        }
        return osal::error_code::invalid_argument;
    }

    /// @brief Block on poll() until at least one file descriptor has an event or the timeout expires.
    /// @param handle      Wait-set handle.
    /// @param fds_ready   Output array filled with ready file descriptors (up to @p max_ready).
    /// @param max_ready   Maximum number of ready fds to report.
    /// @param n_ready     Set to the number of ready fds written into @p fds_ready.
    /// @param timeout_ticks Maximum wait in OSAL ticks; WAIT_FOREVER = -1, NO_WAIT = 0.
    /// @return osal::ok() if events fired; osal::error_code::timeout or ::unknown on failure.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t* handle, int* fds_ready,
                                    std::size_t max_ready, std::size_t* n_ready, osal::tick_t timeout_ticks) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        auto* ws = static_cast<posix_wait_set_obj*>(handle->native);
        if (n_ready)
        {
            *n_ready = 0U;
        }

        const int to_ms = (timeout_ticks == osal::WAIT_FOREVER) ? -1
                          : (timeout_ticks == osal::NO_WAIT)    ? 0
                                                                : static_cast<int>(timeout_ticks);

        // Reset revents.
        for (std::size_t i = 0; i < ws->n_fds; ++i)
        {
            ws->fds[i].revents = 0;
        }

        const int rc = poll(ws->fds, static_cast<nfds_t>(ws->n_fds), to_ms);
        if (rc < 0)
        {
            return osal::error_code::unknown;
        }
        if (rc == 0)
        {
            return osal::error_code::timeout;
        }

        std::size_t out = 0U;
        for (std::size_t i = 0; i < ws->n_fds && out < max_ready; ++i)
        {
            if (ws->fds[i].revents != 0)
            {
                if (fds_ready)
                {
                    fds_ready[out] = ws->fds[i].fd;
                }
                out++;
            }
        }
        if (n_ready)
        {
            *n_ready = out;
        }
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Condition variable (native — pthread_cond_t with CLOCK_MONOTONIC)
    // ---------------------------------------------------------------------------
#include "../common/posix/posix_condvar.inl"

    // ---------------------------------------------------------------------------
    // Work queue (emulated — dedicated pthread + ring buffer)
    // ---------------------------------------------------------------------------
#include "../common/posix/posix_pthread_work_queue.inl"

    // ---------------------------------------------------------------------------
    // Memory pool (emulated — bitmap + mutex + counting semaphore)
    // ---------------------------------------------------------------------------

#include "../common/emulated_memory_pool.inl"

    // ---------------------------------------------------------------------------
    // Read-write lock (native — pthread_rwlock_t)
    // ---------------------------------------------------------------------------
#include "../common/posix/posix_rwlock.inl"

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
#include "../common/posix/posix_barrier.inl"

    // ---------------------------------------------------------------------------
    // Task notification
    // ---------------------------------------------------------------------------
#include "../common/emulated_task_notify.inl"

}  // extern "C"
