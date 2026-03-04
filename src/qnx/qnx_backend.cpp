// SPDX-License-Identifier: Apache-2.0
/// @file qnx_backend.cpp
/// @brief QNX Neutrino implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to QNX Neutrino native APIs:
///          - thread   → pthread_create / pthread_join / pthread_detach
///          - mutex    → pthread_mutex_* (PTHREAD_PRIO_INHERIT protocol)
///          - semaphore→ sem_init / sem_timedwait_monotonic / sem_post
///          - queue    → mq_open / mq_timedsend / mq_timedreceive (with CLOCK_MONOTONIC)
///          - timer    → timer_create(CLOCK_MONOTONIC) + pulse delivery
///          - event_flags→ pthread_cond_* (condvar + bitmask, monotonic clock)
///          - wait_set → ionotify / select based (stubbed for initial release)
///
///          Uses CLOCK_MONOTONIC throughout for timed operations.
///          QNX Neutrino provides monotonic variants:
///            pthread_mutex_timedlock_monotonic, sem_timedwait_monotonic,
///            pthread_cond_timedwait_monotonic
///
///          Includes required: standard QNX Neutrino headers
///          Build macro:       OSAL_BACKEND_QNX
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_QNX
#define OSAL_BACKEND_QNX
#endif
#include <osal/osal.hpp>
#include "../common/backend_timeout_adapter.hpp"

#include <cassert>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <atomic>

#include <pthread.h>
#include <semaphore.h>
#include <mqueue.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <sys/neutrino.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Get CLOCK_MONOTONIC and add ms to produce an absolute timespec.
static struct timespec ms_to_abs_mono(osal::tick_t ms) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_MONOTONIC, ms);
}

/// @brief Map OSAL priority [0=lowest..255=highest] to QNX SCHED_RR range.
static int osal_to_qnx_priority(osal::priority_t p) noexcept
{
    const int lo = sched_get_priority_min(SCHED_RR);
    const int hi = sched_get_priority_max(SCHED_RR);
    return lo + static_cast<int>((static_cast<long>(p) * (hi - lo)) / 255L);
}

// ---------------------------------------------------------------------------
// Timer internal structure
// ---------------------------------------------------------------------------
struct qnx_timer_ctx
{
    timer_t               timerid;
    osal_timer_callback_t fn;
    void*                 arg;
    struct itimerspec     its;
    bool                  auto_reload;
    std::atomic<bool>     alive;
};

static qnx_timer_ctx* timer_alloc() noexcept
{
    auto* ctx = new (std::nothrow) qnx_timer_ctx{};
    if (ctx != nullptr)
    {
        ctx->alive.store(true, std::memory_order_relaxed);
    }
    return ctx;
}

/// @brief SIGEV_THREAD callback for QNX timer.
static void qnx_timer_callback(union sigval sv) noexcept
{
    auto* ctx = static_cast<qnx_timer_ctx*>(sv.sival_ptr);
    if (ctx != nullptr && ctx->alive.load(std::memory_order_acquire) && ctx->fn != nullptr)
    {
        ctx->fn(ctx->arg);
    }
}

// ---------------------------------------------------------------------------
// Queue name helper
// ---------------------------------------------------------------------------
static std::atomic<unsigned> qnx_q_counter{0};

// ---------------------------------------------------------------------------
// Initialise a pthread_cond_t that uses CLOCK_MONOTONIC for timed waits.
// QNX supports this via standard condattr.
// ---------------------------------------------------------------------------
static int cond_init_monotonic(pthread_cond_t* cond) noexcept
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

// ---------------------------------------------------------------------------
// Shared-include macro contracts for posix_condvar.inl and posix_rwlock.inl.
// QNX: all timed operations use CLOCK_MONOTONIC via ms_to_abs_mono.
// ---------------------------------------------------------------------------
#define OSAL_POSIX_RW_ABS(t) ms_to_abs_mono(t)
#define OSAL_POSIX_COND_ABS(t) ms_to_abs_mono(t)

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

#include "../common/posix/posix_clock.inl"

    /// @brief Return the current CLOCK_MONOTONIC time expressed as milliseconds.
    /// @return Monotonic millisecond tick count.
    osal::tick_t osal_clock_ticks() noexcept
    {
        struct timespec ts
        {
        };
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<osal::tick_t>(ts.tv_sec * 1000U + ts.tv_nsec / 1'000'000);
    }

    /// @brief Return the tick period in microseconds (1 tick = 1 ms on QNX).
    /// @return 1000 always.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1000U;  // 1 tick = 1 ms
    }

    // ---------------------------------------------------------------------------
    // Thread (pthread)
    // ---------------------------------------------------------------------------

    /// @brief Create a QNX pthread with SCHED_RR policy via pthread_create().
    /// @param handle      Output handle owns a heap-allocated pthread_t pointer.
    /// @param entry       Thread entry function.
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority mapped to QNX SCHED_RR range via osal_to_qnx_priority().
    /// @param stack_bytes Stack size in bytes; optional @p stack used if non-null.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        assert(handle != nullptr && entry != nullptr);
        pthread_attr_t attr;
        pthread_attr_init(&attr);

        if (stack != nullptr && stack_bytes > 0U)
        {
            pthread_attr_setstack(&attr, stack, static_cast<std::size_t>(stack_bytes));
        }
        else if (stack_bytes > 0U)
        {
            pthread_attr_setstacksize(&attr, static_cast<std::size_t>(stack_bytes));
        }

        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_RR);
        struct sched_param sp
        {
        };
        sp.sched_priority = osal_to_qnx_priority(priority);
        pthread_attr_setschedparam(&attr, &sp);

        auto* tid = new (std::nothrow) pthread_t{};
        if (tid == nullptr)
        {
            pthread_attr_destroy(&attr);
            return osal::error_code::out_of_resources;
        }

        using entry_fn = void* (*)(void*);
        const int rc   = pthread_create(tid, &attr, reinterpret_cast<entry_fn>(entry), arg);
        pthread_attr_destroy(&attr);

        if (rc != 0)
        {
            delete tid;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(tid);
        return osal::ok();
    }

    /// @brief Join a thread using pthread_timedjoin_monotonic() for finite timeouts.
    /// @param handle  Thread handle to join.
    /// @param timeout Maximum wait in OSAL ticks; WAIT_FOREVER or 0 uses pthread_join().
    /// @return osal::ok() on success; osal::error_code::timeout or ::not_initialized on failure.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* tid = static_cast<pthread_t*>(handle->native);

        if (timeout == osal::WAIT_FOREVER || timeout == 0U)
        {
            pthread_join(*tid, nullptr);
        }
        else
        {
            struct timespec ts = ms_to_abs_mono(timeout);
            const int       rc = pthread_timedjoin_monotonic(*tid, nullptr, &ts);
            if (rc == ETIMEDOUT)
            {
                return osal::error_code::timeout;
            }
        }
        delete tid;
        handle->native = nullptr;
        return osal::ok();
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
    /// @param priority New OSAL priority mapped through osal_to_qnx_priority().
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        struct sched_param sp
        {
        };
        sp.sched_priority = osal_to_qnx_priority(priority);
        const int rc      = pthread_setschedparam(*static_cast<pthread_t*>(handle->native), SCHED_RR, &sp);
        return (rc == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Set thread CPU affinity via QNX ThreadCtl(_NTO_TCTL_RUNMASK).
    /// @param handle   Thread handle.
    /// @param affinity CPU runmask; each bit represents a processor.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        // QNX: thread affinity via ThreadCtl or runmask
        unsigned  rmask = static_cast<unsigned>(affinity);
        const int rc    = ThreadCtl(_NTO_TCTL_RUNMASK, reinterpret_cast<void*>(static_cast<uintptr_t>(rmask)));
        return (rc == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Suspend a thread (not supported through this OSAL on QNX).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported through this OSAL on QNX).
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
        };
        ts.tv_sec  = static_cast<time_t>(ms / 1000U);
        ts.tv_nsec = static_cast<long>((ms % 1000U) * 1'000'000L);
        nanosleep(&ts, nullptr);
    }

    // ---------------------------------------------------------------------------
    // Mutex (pthread_mutex with PTHREAD_PRIO_INHERIT, recursive)
    // ---------------------------------------------------------------------------

    /// @brief Create a PTHREAD_PRIO_INHERIT mutex; optionally recursive.
    /// @param handle    Output handle owns a heap-allocated pthread_mutex_t.
    /// @param recursive True for PTHREAD_MUTEX_RECURSIVE type.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept
    {
        auto* m = new (std::nothrow) pthread_mutex_t{};
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        if (recursive)
        {
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        }
        pthread_mutex_init(m, &attr);
        pthread_mutexattr_destroy(&attr);
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    /// @brief Destroy the pthread_mutex_t and free heap storage.
    /// @param handle Mutex handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* m = static_cast<pthread_mutex_t*>(handle->native);
        pthread_mutex_destroy(m);
        delete m;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire the mutex using QNX pthread_mutex_timedlock_monotonic() for timed waits.
    /// @param handle        Mutex handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks; WAIT_FOREVER uses pthread_mutex_lock().
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::not_initialized.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<pthread_mutex_t*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            pthread_mutex_lock(m);
            return osal::ok();
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int rc = pthread_mutex_trylock(m);
            return (rc == 0) ? osal::ok() : osal::error_code::would_block;
        }
        // QNX-specific monotonic timed lock
        struct timespec ts = ms_to_abs_mono(timeout_ticks);
        const int       rc = pthread_mutex_timedlock_monotonic(m, &ts);
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Try to acquire the mutex without blocking.
    /// @param handle Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::would_block otherwise.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release the pthread_mutex_t via pthread_mutex_unlock().
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::not_owner on failure.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = pthread_mutex_unlock(static_cast<pthread_mutex_t*>(handle->native));
        return (rc == 0) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore (POSIX named semaphore with sem_timedwait_monotonic)
    // ---------------------------------------------------------------------------

    /// @brief Allocate and initialise a POSIX sem_t via sem_init().
    /// @param handle        Output handle owns a heap-allocated sem_t.
    /// @param initial_count Starting token count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        auto* s = new (std::nothrow) sem_t{};
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        sem_init(s, 0, initial_count);
        handle->native = static_cast<void*>(s);
        return osal::ok();
    }

    /// @brief Destroy the sem_t via sem_destroy() and free heap storage.
    /// @param handle Semaphore handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* s = static_cast<sem_t*>(handle->native);
        sem_destroy(s);
        delete s;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (post) the semaphore via sem_post().
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::overflow on failure.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = sem_post(static_cast<sem_t*>(handle->native));
        return (rc == 0) ? osal::ok() : osal::error_code::overflow;
    }

    /// @brief Increment the semaphore from ISR context (sem_post is ISR-safe on QNX).
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::overflow or ::not_initialized on failure.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);
    }

    /// @brief Decrement (wait on) the semaphore.
    /// @details Uses sem_wait() (forever), sem_trywait() (NO_WAIT), or
    ///          QNX sem_timedwait_monotonic() (timed).
    /// @param handle        Semaphore handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown.
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
            while (sem_wait(s) != 0)
            {
                if (errno != EINTR)
                {
                    return osal::error_code::unknown;
                }
            }
            return osal::ok();
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int rc = sem_trywait(s);
            return (rc == 0) ? osal::ok() : osal::error_code::would_block;
        }
        // QNX-specific monotonic timed wait
        struct timespec ts = ms_to_abs_mono(timeout_ticks);
        const int       rc = sem_timedwait_monotonic(s, &ts);
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Try to decrement the semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if a token was available; osal::error_code::would_block otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (POSIX message queue)
    // ---------------------------------------------------------------------------

    /// @brief Create a POSIX message queue with a unique name; unlinked immediately after open.
    /// @details Uses mq_open() with O_CREAT|O_RDWR|O_NONBLOCK; mq_unlink() is called so the
    ///          queue is cleaned up when closed.
    /// @param handle    Output handle owns a heap-allocated {mqd_t, msg_size} context.
    /// @param item_size Size in bytes of each message.
    /// @param capacity  Maximum number of messages the queue can hold.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        char           name[32];
        const unsigned id = qnx_q_counter.fetch_add(1U, std::memory_order_relaxed);
        snprintf(name, sizeof(name), "/osal_q_%u_%u", static_cast<unsigned>(getpid()), id);

        struct mq_attr attr
        {
        };
        attr.mq_maxmsg  = static_cast<long>(capacity > 0 ? capacity : 8);
        attr.mq_msgsize = static_cast<long>(item_size > 0 ? item_size : sizeof(void*));

        mqd_t mq = mq_open(name, O_CREAT | O_RDWR | O_NONBLOCK, 0600, &attr);
        if (mq == static_cast<mqd_t>(-1))
        {
            return osal::error_code::out_of_resources;
        }
        mq_unlink(name);  // unlink now; fd remains valid until mq_close

        // Store mqd_t as a pointer-sized value
        auto* ctx = new (std::nothrow) struct
        {
            mqd_t mq;
            long  msg_size;
        };
        if (ctx == nullptr)
        {
            mq_close(mq);
            return osal::error_code::out_of_resources;
        }
        ctx->mq        = mq;
        ctx->msg_size  = attr.mq_msgsize;
        handle->native = static_cast<void*>(ctx);
        return osal::ok();
    }

    /// @brief Close the message queue via mq_close() and free the context.
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* ctx = static_cast < struct
        {
            mqd_t mq;
            long  msg_size;
        }* > (handle->native);
        mq_close(ctx->mq);
        delete ctx;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send a message via mq_send() or QNX mq_timedsend_monotonic().
    /// @param handle        Queue handle.
    /// @param item          Pointer to the message data.
    /// @param timeout_ticks Maximum wait in OSAL ticks; NO_WAIT uses non-blocking send.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast < struct
        {
            mqd_t mq;
            long  msg_size;
        }* > (handle->native);

        if (timeout_ticks == osal::NO_WAIT)
        {
            const int rc = mq_send(ctx->mq, static_cast<const char*>(item), static_cast<std::size_t>(ctx->msg_size), 0);
            return (rc == 0) ? osal::ok() : osal::error_code::would_block;
        }
        // For timed/blocking sends, remove O_NONBLOCK temporarily
        struct mq_attr old_attr
        {
        };
        struct mq_attr new_attr
        {
        };
        new_attr.mq_flags = 0;  // blocking
        mq_setattr(ctx->mq, &new_attr, &old_attr);

        int rc;
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            rc = mq_send(ctx->mq, static_cast<const char*>(item), static_cast<std::size_t>(ctx->msg_size), 0);
        }
        else
        {
            struct timespec ts = ms_to_abs_mono(timeout_ticks);
            rc                 = mq_timedsend_monotonic(ctx->mq, static_cast<const char*>(item),
                                                        static_cast<std::size_t>(ctx->msg_size), 0, &ts);
        }
        mq_setattr(ctx->mq, &old_attr, nullptr);  // restore

        if (rc == 0)
        {
            return osal::ok();
        }
        return (errno == ETIMEDOUT) ? osal::error_code::timeout : osal::error_code::would_block;
    }

    /// @brief Send a message non-blocking (ISR-compatible).
    /// @param handle Queue handle.
    /// @param item   Pointer to the message data.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive a message via mq_receive() or QNX mq_timedreceive_monotonic().
    /// @param handle        Queue handle.
    /// @param item          Buffer to receive the dequeued message into.
    /// @param timeout_ticks Maximum wait in OSAL ticks; NO_WAIT uses non-blocking receive.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast < struct
        {
            mqd_t mq;
            long  msg_size;
        }* > (handle->native);

        if (timeout_ticks == osal::NO_WAIT)
        {
            const ssize_t n =
                mq_receive(ctx->mq, static_cast<char*>(item), static_cast<std::size_t>(ctx->msg_size), nullptr);
            return (n >= 0) ? osal::ok() : osal::error_code::would_block;
        }
        struct mq_attr old_attr
        {
        };
        struct mq_attr new_attr
        {
        };
        new_attr.mq_flags = 0;
        mq_setattr(ctx->mq, &new_attr, &old_attr);

        ssize_t n;
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            n = mq_receive(ctx->mq, static_cast<char*>(item), static_cast<std::size_t>(ctx->msg_size), nullptr);
        }
        else
        {
            struct timespec ts = ms_to_abs_mono(timeout_ticks);
            n = mq_timedreceive_monotonic(ctx->mq, static_cast<char*>(item), static_cast<std::size_t>(ctx->msg_size),
                                          nullptr, &ts);
        }
        mq_setattr(ctx->mq, &old_attr, nullptr);

        if (n >= 0)
        {
            return osal::ok();
        }
        return (errno == ETIMEDOUT) ? osal::error_code::timeout : osal::error_code::would_block;
    }

    /// @brief Receive a message non-blocking (ISR-compatible).
    /// @param handle Queue handle.
    /// @param item   Buffer to receive the dequeued message into.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the front message without removing it (POSIX mq has no peek).
    /// @return osal::error_code::not_supported always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Return the number of messages currently in the queue via mq_getattr().
    /// @param handle Queue handle.
    /// @return Message count (mq_curmsgs), or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* ctx = static_cast < const struct
        {
            mqd_t mq;
            long  msg_size;
        }* > (handle->native);
        struct mq_attr attr
        {
        };
        mq_getattr(ctx->mq, &attr);
        return static_cast<std::size_t>(attr.mq_curmsgs);
    }

    /// @brief Return the number of free message slots via mq_getattr() (mq_maxmsg - mq_curmsgs).
    /// @param handle Queue handle.
    /// @return Free slot count, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* ctx = static_cast < const struct
        {
            mqd_t mq;
            long  msg_size;
        }* > (handle->native);
        struct mq_attr attr
        {
        };
        mq_getattr(ctx->mq, &attr);
        return static_cast<std::size_t>(attr.mq_maxmsg - attr.mq_curmsgs);
    }

    // ---------------------------------------------------------------------------
    // Timer (CLOCK_MONOTONIC + SIGEV_THREAD)
    // ---------------------------------------------------------------------------

    /// @brief Create a CLOCK_MONOTONIC POSIX timer with SIGEV_THREAD delivery.
    /// @details The callback is invoked from a kernel-spawned thread on expiry.
    /// @param handle       Output handle owns a heap-allocated qnx_timer_ctx.
    /// @param callback     Function called on each expiry.
    /// @param arg          Opaque argument forwarded to @p callback.
    /// @param period_ticks Expiry period in OSAL ticks (milliseconds).
    /// @param auto_reload  If true, the interval (it_interval) is set for periodic fire.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        auto* ctx = timer_alloc();
        if (ctx == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        ctx->fn          = callback;
        ctx->arg         = arg;
        ctx->auto_reload = auto_reload;

        // Convert ticks (ms) to itimerspec
        const time_t sec          = static_cast<time_t>(period_ticks / 1000U);
        const long   nsec         = static_cast<long>((period_ticks % 1000U) * 1'000'000L);
        ctx->its.it_value.tv_sec  = sec;
        ctx->its.it_value.tv_nsec = nsec;
        if (auto_reload)
        {
            ctx->its.it_interval.tv_sec  = sec;
            ctx->its.it_interval.tv_nsec = nsec;
        }

        struct sigevent sev
        {
        };
        sev.sigev_notify            = SIGEV_THREAD;
        sev.sigev_notify_function   = qnx_timer_callback;
        sev.sigev_value.sival_ptr   = ctx;
        sev.sigev_notify_attributes = nullptr;

        if (timer_create(CLOCK_MONOTONIC, &sev, &ctx->timerid) != 0)
        {
            delete ctx;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(ctx);
        return osal::ok();
    }

    /// @brief Disarm the timer, wait for any in-flight callback to finish, then delete it.
    /// @param handle Timer handle; no-op if null.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* ctx = static_cast<qnx_timer_ctx*>(handle->native);
        ctx->alive.store(false, std::memory_order_release);

        // Disarm before delete
        struct itimerspec disarm
        {
        };
        timer_settime(ctx->timerid, 0, &disarm, nullptr);

        // Brief settle to avoid racing with in-flight callback
        struct timespec settle = {0, 5'000'000};  // 5 ms
        nanosleep(&settle, nullptr);

        timer_delete(ctx->timerid);
        delete ctx;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Arm the timer via timer_settime() with the stored itimerspec.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*     ctx = static_cast<qnx_timer_ctx*>(handle->native);
        const int rc  = timer_settime(ctx->timerid, 0, &ctx->its, nullptr);
        return (rc == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Disarm the timer by passing a zero itimerspec to timer_settime().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<qnx_timer_ctx*>(handle->native);
        struct itimerspec disarm
        {
        };
        timer_settime(ctx->timerid, 0, &disarm, nullptr);
        return osal::ok();
    }

    /// @brief Stop and immediately restart the timer.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Update the stored itimerspec period; takes effect on the next osal_timer_start().
    /// @param handle          Timer handle.
    /// @param new_period_ticks New period in OSAL ticks (milliseconds).
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*        ctx          = static_cast<qnx_timer_ctx*>(handle->native);
        const time_t sec          = static_cast<time_t>(new_period_ticks / 1000U);
        const long   nsec         = static_cast<long>((new_period_ticks % 1000U) * 1'000'000L);
        ctx->its.it_value.tv_sec  = sec;
        ctx->its.it_value.tv_nsec = nsec;
        if (ctx->auto_reload)
        {
            ctx->its.it_interval.tv_sec  = sec;
            ctx->its.it_interval.tv_nsec = nsec;
        }
        return osal::ok();
    }

    /// @brief Query whether the timer is still armed via timer_gettime().
    /// @param handle Timer handle.
    /// @return True if it_value is non-zero (timer has not yet fired or is periodic).
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        auto* ctx = static_cast<const qnx_timer_ctx*>(handle->native);
        struct itimerspec cur
        {
        };
        timer_gettime(ctx->timerid, &cur);
        return cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0;
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

    // ---------------------------------------------------------------------------
    // Condition variable (native — pthread_cond_t with CLOCK_MONOTONIC)
    // ---------------------------------------------------------------------------
#include "../common/posix/posix_condvar.inl"

    // ---------------------------------------------------------------------------
    // Wait-set — not supported in initial release
    // ---------------------------------------------------------------------------

    /// @brief Create a wait-set (not supported in this release).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set (not supported in this release).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add an object to a wait-set (not supported in this release).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove an object from a wait-set (not supported in this release).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait on a wait-set (not supported in this release).
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
    // Work queue (emulated — OSAL thread + mutex + semaphore + ring buffer)
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
