// SPDX-License-Identifier: Apache-2.0
/// @file nuttx_backend.cpp
/// @brief Apache NuttX RTOS implementation of all OSAL C-linkage functions
/// @details NuttX is POSIX-conformant, but this backend uses NuttX-native APIs
///          where they provide advantages over pure POSIX:
///
///          - nxsem_*   — returns -errno directly (no global errno overhead)
///          - nxmutex_* — proper recursive mutex with priority inheritance
///          - nxmq_*    — native message queues (POSIX mq_* compatible but typed)
///          - work_queue() for deferred timer callbacks on the hi-/lo-priority
///            work queues (avoids spawning a thread per timer like SIGEV_THREAD)
///          - clock_systime_ticks() for direct tick access
///
///          Falls back to POSIX where no native advantage exists (e.g., pthread
///          for threads).
///
///          Includes required: <nuttx/config.h>, <nuttx/semaphore.h>,
///                             <nuttx/mutex.h>, <nuttx/wqueue.h>,
///                             <nuttx/clock.h>
///          Build macro:       OSAL_BACKEND_NUTTX
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_NUTTX
#define OSAL_BACKEND_NUTTX
#endif
#include <osal/osal.hpp>
#include "../common/backend_timeout_adapter.hpp"

#include <nuttx/config.h>
#include <nuttx/semaphore.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>
#include <nuttx/clock.h>

#include <pthread.h>
#include <mqueue.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <string.h>
#include <new>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Convert ms timeout to absolute timespec (CLOCK_MONOTONIC).
static struct timespec ms_to_abs_timespec(osal::tick_t ms) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_MONOTONIC, ms);
}

/// @brief Select the best clock source for queue condition variables.
#if defined(__APPLE__) || !defined(_POSIX_MONOTONIC_CLOCK)
static constexpr clockid_t kQueueCondClock = CLOCK_REALTIME;
#else
static constexpr clockid_t kQueueCondClock = CLOCK_MONOTONIC;
#endif

/// @brief Convert queue wait timeouts to an absolute condvar deadline.
static struct timespec ms_to_abs_timespec_cond(osal::tick_t timeout_ticks) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(kQueueCondClock, timeout_ticks);
}

/// @brief Init a queue condvar with CLOCK_MONOTONIC where supported.
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
// Timer context (work-queue based)
// ---------------------------------------------------------------------------
#define OSAL_NX_MAX_TIMERS 16

struct nx_timer_ctx
{
    struct work_s         work;
    osal_timer_callback_t fn;
    void*                 arg;
    uint32_t              period_ticks;
    bool                  auto_reload;
    bool                  active;
};
static nx_timer_ctx nx_timer_ctxs[OSAL_NX_MAX_TIMERS];

static void nx_timer_worker(void* arg) noexcept
{
    auto* ctx = static_cast<nx_timer_ctx*>(arg);
    if (ctx->fn != nullptr)
    {
        ctx->fn(ctx->arg);
    }
    if (ctx->auto_reload && ctx->active)
    {
        work_queue(LPWORK, &ctx->work, nx_timer_worker, ctx, ctx->period_ticks);
    }
}

// ---------------------------------------------------------------------------
// Shared-include macro contract for posix_rwlock.inl.
// NuttX: timed rwlock uses CLOCK_MONOTONIC via ms_to_abs_timespec.
// ---------------------------------------------------------------------------
#define OSAL_POSIX_RW_ABS(t) ms_to_abs_timespec(t)

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

#include "../common/posix/posix_clock.inl"

    /// @brief Return the raw NuttX system tick counter via clock_systime_ticks().
    /// @return Current tick value.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(clock_systime_ticks());
    }

    /// @brief Return the tick period in microseconds from CONFIG_USEC_PER_TICK.
    /// @return Microseconds per tick as configured in the NuttX build.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return CONFIG_USEC_PER_TICK;  // NuttX tick period in microseconds
    }

    // ---------------------------------------------------------------------------
    // Thread (pthread — NuttX pthreads are first-class)
    // ---------------------------------------------------------------------------

    struct nx_thread_ctx
    {
        void (*entry)(void*);
        void* arg;
    };

    static void* nx_thread_trampoline(void* raw) noexcept
    {
        auto* ctx = static_cast<nx_thread_ctx*>(raw);
        ctx->entry(ctx->arg);
        delete ctx;
        return nullptr;
    }

    /// @brief Create a NuttX pthread via pthread_create() with SCHED_RR policy.
    /// @param handle      Output handle owns a heap-allocated pthread_t pointer.
    /// @param entry       Thread entry function.
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority used directly as POSIX sched_priority.
    /// @param stack_bytes Stack size in bytes; optional @p stack pointer used if non-null.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        assert(handle != nullptr && entry != nullptr);
        auto* ctx = new (std::nothrow) nx_thread_ctx{entry, arg};
        if (!ctx)
        {
            return osal::error_code::out_of_resources;
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stack_bytes);
        if (stack != nullptr)
        {
            pthread_attr_setstack(&attr, stack, stack_bytes);
        }

        struct sched_param sp
        {
        };
        sp.sched_priority = static_cast<int>(priority);
        pthread_attr_setschedparam(&attr, &sp);
        pthread_attr_setschedpolicy(&attr, SCHED_RR);

        auto* tid = new (std::nothrow) pthread_t;
        if (!tid)
        {
            delete ctx;
            pthread_attr_destroy(&attr);
            return osal::error_code::out_of_resources;
        }

        const int rc = pthread_create(tid, &attr, nx_thread_trampoline, ctx);
        pthread_attr_destroy(&attr);
        if (rc != 0)
        {
            delete tid;
            delete ctx;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(tid);
        return osal::ok();
    }

    /// @brief Wait for a pthread to exit via pthread_join() and free its handle.
    /// @param handle Thread handle to join.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*     tid = static_cast<pthread_t*>(handle->native);
        const int rc  = pthread_join(*tid, nullptr);
        delete tid;
        handle->native = nullptr;
        return (rc == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Detach the pthread via pthread_detach() and free its handle.
    /// @param handle Thread handle to detach.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* tid = static_cast<pthread_t*>(handle->native);
        pthread_detach(*tid);
        delete tid;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change a thread's SCHED_RR priority via pthread_setschedparam().
    /// @param handle   Thread handle.
    /// @param priority New OSAL priority used directly as sched_priority.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* tid = static_cast<pthread_t*>(handle->native);
        struct sched_param sp
        {
        };
        sp.sched_priority = static_cast<int>(priority);
        return (pthread_setschedparam(*tid, SCHED_RR, &sp) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Set thread CPU affinity (not supported on NuttX through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Suspend a thread (not supported through this OSAL on NuttX).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported through this OSAL on NuttX).
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
        struct timespec ts;
        ts.tv_sec  = static_cast<time_t>(ms / 1000U);
        ts.tv_nsec = static_cast<long>((ms % 1000U) * 1'000'000L);
        nanosleep(&ts, nullptr);
    }

    // ---------------------------------------------------------------------------
    // Mutex (nxmutex — NuttX native, priority inheritance built-in)
    // ---------------------------------------------------------------------------

    /// @brief Allocate and initialise a NuttX nxmutex via nxmutex_init().
    /// @param handle Output handle owns a heap-allocated mutex_t pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        auto* m = new (std::nothrow) mutex_t;
        if (!m)
        {
            return osal::error_code::out_of_resources;
        }
        const int rc = nxmutex_init(m);
        if (rc < 0)
        {
            delete m;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    /// @brief Destroy a nxmutex via nxmutex_destroy() and free heap storage.
    /// @param handle Mutex handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* m = static_cast<mutex_t*>(handle->native);
        nxmutex_destroy(m);
        delete m;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire the nxmutex, using nxmutex_lock(), nxmutex_trylock(), or nxmutex_clocklock().
    /// @param handle        Mutex handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks (milliseconds for nxmutex_clocklock).
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown on failure.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<mutex_t*>(handle->native);
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            return (nxmutex_lock(m) >= 0) ? osal::ok() : osal::error_code::unknown;
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            return (nxmutex_trylock(m) >= 0) ? osal::ok() : osal::error_code::would_block;
        }
        // NuttX nxmutex does not have a timed lock; fall back to nxsem_clockwait
        // on the underlying semaphore (nxmutex wraps nxsem internally).
        const struct timespec abs = ms_to_abs_timespec(timeout_ticks);
        const int             rc  = nxmutex_clocklock(m, CLOCK_MONOTONIC, &abs);
        if (rc >= 0)
        {
            return osal::ok();
        }
        return (rc == -ETIMEDOUT) ? osal::error_code::timeout : osal::error_code::unknown;
    }

    /// @brief Try to acquire the nxmutex without blocking.
    /// @param handle Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::would_block otherwise.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release the nxmutex via nxmutex_unlock().
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::not_owner on failure.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (nxmutex_unlock(static_cast<mutex_t*>(handle->native)) >= 0) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore (nxsem — NuttX native, returns -errno directly)
    // ---------------------------------------------------------------------------

    /// @brief Allocate and initialise a NuttX nxsem via nxsem_init().
    /// @param handle        Output handle owns a heap-allocated sem_t pointer.
    /// @param initial_count Initial token count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        auto* s = new (std::nothrow) sem_t;
        if (!s)
        {
            return osal::error_code::out_of_resources;
        }
        if (nxsem_init(s, 0, static_cast<int>(initial_count)) < 0)
        {
            delete s;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(s);
        return osal::ok();
    }

    /// @brief Destroy the nxsem via nxsem_destroy() and free heap storage.
    /// @param handle Semaphore handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        nxsem_destroy(static_cast<sem_t*>(handle->native));
        delete static_cast<sem_t*>(handle->native);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (post) the nxsem via nxsem_post().
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (nxsem_post(static_cast<sem_t*>(handle->native)) >= 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Increment a semaphore from ISR context (nxsem_post is ISR-safe in NuttX).
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // nxsem_post is ISR-safe in NuttX
    }

    /// @brief Decrement (wait on) the nxsem.
    /// @details Uses nxsem_wait() (forever), nxsem_trywait() (NO_WAIT), or
    ///          nxsem_clockwait() (timed); restarts on EINTR for blocking wait.
    /// @param handle        Semaphore handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks; mapped to CLOCK_MONOTONIC timespec.
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown on failure.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<sem_t*>(handle->native);
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            int rc;
            do
            {
                rc = nxsem_wait(s);
            } while (rc == -EINTR);
            return (rc >= 0) ? osal::ok() : osal::error_code::unknown;
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            return (nxsem_trywait(s) >= 0) ? osal::ok() : osal::error_code::would_block;
        }
        const struct timespec abs = ms_to_abs_timespec(timeout_ticks);
        const int             rc  = nxsem_clockwait(s, CLOCK_MONOTONIC, &abs);
        if (rc >= 0)
        {
            return osal::ok();
        }
        return (rc == -ETIMEDOUT) ? osal::error_code::timeout : osal::error_code::unknown;
    }

    /// @brief Try to decrement the nxsem without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if a token was available; osal::error_code::would_block otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue — NuttX POSIX message queue (mq_*)
    // ---------------------------------------------------------------------------

    struct nx_queue_obj
    {
        mqd_t                   mq;
        std::size_t             item_size;
        std::size_t             capacity;
        std::size_t             count;
        mutable pthread_mutex_t mutex;
        pthread_cond_t          not_full;
        pthread_cond_t          not_empty;
        bool                    cache_valid;
        std::uint8_t*           cache;
    };

    static int g_nx_mq_counter = 0;

    /// @brief Create a POSIX message queue with a unique generated name via mq_open().
    /// @details The name is immediately unlinked so the queue is destroyed when closed.
    ///          A small front-item cache plus condvars preserve OSAL peek/count semantics
    ///          even though POSIX mqueues do not natively support non-destructive peek.
    /// @param handle    Output handle owns a heap-allocated nx_queue_obj.
    /// @param item_size Size in bytes of each message.
    /// @param capacity  Maximum number of messages the queue can hold.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        assert(handle != nullptr && item_size > 0 && capacity > 0);
        auto* q = new (std::nothrow) nx_queue_obj{};
        if (!q)
        {
            return osal::error_code::out_of_resources;
        }

        q->cache = new (std::nothrow) std::uint8_t[item_size];
        if (q->cache == nullptr)
        {
            delete q;
            return osal::error_code::out_of_resources;
        }

        if (pthread_mutex_init(&q->mutex, nullptr) != 0)
        {
            delete[] q->cache;
            delete q;
            return osal::error_code::out_of_resources;
        }

        if (cond_init_monotonic(&q->not_full) != 0)
        {
            pthread_mutex_destroy(&q->mutex);
            delete[] q->cache;
            delete q;
            return osal::error_code::out_of_resources;
        }

        if (cond_init_monotonic(&q->not_empty) != 0)
        {
            pthread_cond_destroy(&q->not_full);
            pthread_mutex_destroy(&q->mutex);
            delete[] q->cache;
            delete q;
            return osal::error_code::out_of_resources;
        }

        // Generate a unique name for the POSIX mqueue.
        char name[32];
        snprintf(name, sizeof(name), "/osal_q_%d", g_nx_mq_counter++);

        struct mq_attr attr
        {
        };
        attr.mq_maxmsg  = static_cast<long>(capacity);
        attr.mq_msgsize = static_cast<long>(item_size);

        q->mq = mq_open(name, O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK, 0600, &attr);
        if (q->mq == (mqd_t)-1)
        {
            pthread_cond_destroy(&q->not_empty);
            pthread_cond_destroy(&q->not_full);
            pthread_mutex_destroy(&q->mutex);
            delete[] q->cache;
            delete q;
            return osal::error_code::out_of_resources;
        }
        mq_unlink(name);  // unlink so it disappears when closed
        q->item_size   = item_size;
        q->capacity    = capacity;
        q->count       = 0U;
        q->cache_valid = false;
        handle->native = static_cast<void*>(q);
        return osal::ok();
    }

    /// @brief Close and destroy the message queue via mq_close() and free its object.
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* q = static_cast<nx_queue_obj*>(handle->native);
        mq_close(q->mq);
        pthread_cond_destroy(&q->not_empty);
        pthread_cond_destroy(&q->not_full);
        pthread_mutex_destroy(&q->mutex);
        delete[] q->cache;
        delete q;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send a message via the native mqueue after waiting on logical free space.
    /// @param handle        Queue handle.
    /// @param item          Pointer to the message data.
    /// @param timeout_ticks Maximum wait in OSAL ticks; 0 = non-blocking, WAIT_FOREVER = blocking.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<nx_queue_obj*>(handle->native);

        pthread_mutex_lock(&q->mutex);
        while (q->count == q->capacity)
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

        const int rc = mq_send(q->mq, static_cast<const char*>(item), q->item_size, 0);
        if (rc != 0)
        {
            const int err = errno;
            pthread_mutex_unlock(&q->mutex);
            if (err == EAGAIN)
            {
                return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
            }
            return osal::error_code::unknown;
        }

        ++q->count;
        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->mutex);
        return osal::ok();
    }

    /// @brief Send a message non-blocking (ISR-compatible).
    /// @param handle Queue handle.
    /// @param item   Pointer to the message data.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive a message from the cached head item or native mqueue.
    /// @param handle        Queue handle.
    /// @param item          Buffer to receive the dequeued message into.
    /// @param timeout_ticks Maximum wait in OSAL ticks; 0 = non-blocking, WAIT_FOREVER = blocking.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<nx_queue_obj*>(handle->native);

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

        if (q->cache_valid)
        {
            std::memcpy(item, q->cache, q->item_size);
            q->cache_valid = false;
            --q->count;
            pthread_cond_signal(&q->not_full);
            pthread_mutex_unlock(&q->mutex);
            return osal::ok();
        }

        const ssize_t rc = mq_receive(q->mq, static_cast<char*>(item), q->item_size, nullptr);
        if (rc < 0)
        {
            const int err = errno;
            pthread_mutex_unlock(&q->mutex);
            if (err == EAGAIN)
            {
                return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
            }
            return osal::error_code::unknown;
        }

        --q->count;
        pthread_cond_signal(&q->not_full);
        pthread_mutex_unlock(&q->mutex);
        return osal::ok();
    }

    /// @brief Receive a message non-blocking (ISR-compatible).
    /// @param handle Queue handle.
    /// @param item   Buffer to receive the dequeued message into.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the front message without removing it.
    /// @details POSIX mqueues do not natively support peek, so the backend caches
    ///          the current front item until the next receive consumes it.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if empty.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }

        auto* q = static_cast<nx_queue_obj*>(handle->native);
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

        if (!q->cache_valid)
        {
            const ssize_t rc = mq_receive(q->mq, reinterpret_cast<char*>(q->cache), q->item_size, nullptr);
            if (rc < 0)
            {
                const int err = errno;
                pthread_mutex_unlock(&q->mutex);
                if (err == EAGAIN)
                {
                    return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
                }
                return osal::error_code::unknown;
            }
            q->cache_valid = true;
        }

        std::memcpy(item, q->cache, q->item_size);
        pthread_mutex_unlock(&q->mutex);
        return osal::ok();
    }

    /// @brief Return the current logical message count.
    /// @param handle Queue handle.
    /// @return Logical message count, or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* q = static_cast<const nx_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        const std::size_t count = q->count;
        pthread_mutex_unlock(&q->mutex);
        return count;
    }

    /// @brief Return the number of free logical message slots.
    /// @param handle Queue handle.
    /// @return Free slot count, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* q = static_cast<const nx_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        const std::size_t free_slots = q->capacity - q->count;
        pthread_mutex_unlock(&q->mutex);
        return free_slots;
    }

    // ---------------------------------------------------------------------------
    // Timer (NuttX work queue — avoids spawning a thread per timer)
    // ---------------------------------------------------------------------------

    /// @brief Reserve an nx_timer_ctx slot and configure it for deferred work.
    /// @details Timers are dispatched on the LPWORK queue; auto-reload re-queues on each expiry.
    /// @param handle       Output handle encodes the slot index as an intptr_t.
    /// @param callback     Work function invoked at each expiry.
    /// @param arg          Opaque argument forwarded to @p callback.
    /// @param period_ticks Expiry period stored as NuttX ticks passed to work_queue().
    /// @param auto_reload  If true, the work item is re-queued after each callback.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if no slot is free.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        long idx = -1;
        for (long i = 0; i < OSAL_NX_MAX_TIMERS; ++i)
        {
            if (nx_timer_ctxs[i].fn == nullptr)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
        {
            return osal::error_code::out_of_resources;
        }

        auto& ctx = nx_timer_ctxs[idx];
        std::memset(&ctx.work, 0, sizeof(ctx.work));
        ctx.fn           = callback;
        ctx.arg          = arg;
        ctx.period_ticks = static_cast<uint32_t>(period_ticks);
        ctx.auto_reload  = auto_reload;
        ctx.active       = false;

        handle->native = reinterpret_cast<void*>(static_cast<intptr_t>(idx));
        return osal::ok();
    }

    /// @brief Cancel the work item and release the timer slot.
    /// @param handle Timer handle (slot index); no-op if null.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::ok();
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx >= 0 && idx < OSAL_NX_MAX_TIMERS)
        {
            auto& ctx  = nx_timer_ctxs[idx];
            ctx.active = false;
            work_cancel(LPWORK, &ctx.work);
            ctx.fn = nullptr;
        }
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Submit the work item to LPWORK via work_queue().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx < 0 || idx >= OSAL_NX_MAX_TIMERS || nx_timer_ctxs[idx].fn == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto& ctx    = nx_timer_ctxs[idx];
        ctx.active   = true;
        const int rc = work_queue(LPWORK, &ctx.work, nx_timer_worker, &ctx, ctx.period_ticks);
        return (rc >= 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Cancel the pending work item via work_cancel().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is invalid.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx < 0 || idx >= OSAL_NX_MAX_TIMERS)
        {
            return osal::error_code::not_initialized;
        }
        auto& ctx  = nx_timer_ctxs[idx];
        ctx.active = false;
        work_cancel(LPWORK, &ctx.work);
        return osal::ok();
    }

    /// @brief Stop and immediately restart the timer work item.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Update the stored tick period; takes effect on the next osal_timer_start().
    /// @param handle          Timer handle.
    /// @param new_period_ticks New period in NuttX ticks.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the index is invalid.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx < 0 || idx >= OSAL_NX_MAX_TIMERS)
        {
            return osal::error_code::not_initialized;
        }
        nx_timer_ctxs[idx].period_ticks = static_cast<uint32_t>(new_period_ticks);
        return osal::ok();
    }

    /// @brief Query the active flag stored in the nx_timer_ctx.
    /// @param handle Timer handle.
    /// @return True if the timer has been started and not yet cancelled.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return false;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx < 0 || idx >= OSAL_NX_MAX_TIMERS)
        {
            return false;
        }
        return nx_timer_ctxs[idx].active;
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

    // ---------------------------------------------------------------------------
    // Condition variable (nxsem-based — atomically unlocks nxmutex + waits)
    // ---------------------------------------------------------------------------

    struct nx_condvar_obj
    {
        sem_t        sem;
        volatile int nwaiters;
    };

    /// @brief Allocate and initialise an nxsem-based condvar (sem count = 0).
    /// @param handle Output handle owns a heap-allocated nx_condvar_obj.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle)
            return osal::error_code::invalid_argument;
        auto* obj = new (std::nothrow) nx_condvar_obj{};
        if (!obj)
            return osal::error_code::out_of_resources;
        nxsem_init(&obj->sem, 0, 0);
        obj->nwaiters  = 0;
        handle->native = static_cast<void*>(obj);
        return osal::ok();
    }

    /// @brief Destroy the nxsem and free heap storage.
    /// @param handle Condvar handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::ok();
        auto* obj = static_cast<nx_condvar_obj*>(handle->native);
        nxsem_destroy(&obj->sem);
        delete obj;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Atomically release @p mutex and block on the nxsem until signalled.
    /// @param handle  Condvar handle.
    /// @param mutex   nxmutex that the caller holds; released before sleeping.
    /// @param timeout Maximum wait in OSAL ticks (milliseconds).
    /// @return osal::ok() on success; osal::error_code::timeout or ::unknown on failure.
    osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                                   osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native || !mutex || !mutex->native)
            return osal::error_code::not_initialized;
        auto* obj = static_cast<nx_condvar_obj*>(handle->native);
        auto* mtx = static_cast<mutex_t*>(mutex->native);
        obj->nwaiters++;
        nxmutex_unlock(mtx);
        int rc;
        if (timeout == osal::WAIT_FOREVER)
        {
            do
            {
                rc = nxsem_wait(&obj->sem);
            } while (rc == -EINTR);
        }
        else
        {
            const struct timespec abs = ms_to_abs_timespec(timeout);
            rc                        = nxsem_clockwait(&obj->sem, CLOCK_MONOTONIC, &abs);
        }
        obj->nwaiters--;
        nxmutex_lock(mtx);
        if (rc >= 0)
            return osal::ok();
        if (rc == -ETIMEDOUT)
            return osal::error_code::timeout;
        return osal::error_code::unknown;
    }

    /// @brief Wake one thread waiting on the condvar via nxsem_post() (if any are waiting).
    /// @param handle Condvar handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::error_code::not_initialized;
        auto* obj = static_cast<nx_condvar_obj*>(handle->native);
        if (obj->nwaiters > 0)
        {
            nxsem_post(&obj->sem);
        }
        return osal::ok();
    }

    /// @brief Wake all threads waiting on the condvar via repeated nxsem_post().
    /// @param handle Condvar handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::error_code::not_initialized;
        auto*     obj = static_cast<nx_condvar_obj*>(handle->native);
        const int n   = obj->nwaiters;
        for (int i = 0; i < n; ++i)
        {
            nxsem_post(&obj->sem);
        }
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Wait-set — NuttX supports poll() (POSIX)
    // ---------------------------------------------------------------------------

    /// @brief Create a wait-set (not yet implemented for NuttX).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set (not yet implemented for NuttX).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add an object to a wait-set (not yet implemented for NuttX).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove an object from a wait-set (not yet implemented for NuttX).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait on a wait-set (not yet implemented for NuttX).
    /// @param n Sets *n to 0.
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t*, int*, std::size_t, std::size_t* n,
                                    osal::tick_t) noexcept
    {
        if (n)
            *n = 0U;
        return osal::error_code::not_supported;
    }

    // ---------------------------------------------------------------------------
    // Work queue (emulated fallback — TODO: replace with native work_queue(LPWORK, ...))
    // ---------------------------------------------------------------------------

#include "../common/emulated_work_queue.inl"

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
