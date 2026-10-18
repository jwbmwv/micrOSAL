// SPDX-License-Identifier: Apache-2.0
/// @file qnx_backend.cpp
/// @brief QNX Neutrino implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to QNX Neutrino native APIs:
///          - thread   → pthread_create / pthread_join / pthread_detach
///          - mutex    → pthread_mutex_* (PTHREAD_PRIO_INHERIT protocol)
///          - semaphore→ sem_init / sem_timedwait_monotonic / sem_post
///          - queue    → mq_open / mq_timedsend / mq_timedreceive (with CLOCK_MONOTONIC)
///          - timer    → owned pthread watcher + monotonic condition variable
///          - event_flags→ pthread_cond_* (condvar + bitmask, monotonic clock)
///          - wait_set → ionotify / select based (stubbed for initial release)
///
///          Uses CLOCK_MONOTONIC for timed operations except standard rwlocks.
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
#include <cstdio>
#include <fcntl.h>

#include <pthread.h>
#include <semaphore.h>
#include <mqueue.h>
#include <ctime>
#include <unistd.h>
#include <sched.h>
#include <csignal>
#include <sys/neutrino.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

/// @brief Get CLOCK_MONOTONIC and add ms to produce an absolute timespec.
struct timespec ms_to_abs_mono(osal::tick_t ms) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_MONOTONIC, ms);
}

/// @brief Map OSAL priority [0=lowest..255=highest] to QNX SCHED_RR range.
int osal_to_qnx_priority(osal::priority_t p) noexcept
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
    osal_timer_callback_t   fn;
    void*                   arg;
    pthread_t               watcher;
    mutable pthread_mutex_t mutex;
    pthread_cond_t          changed;
    osal::tick_t            period_ticks;
    timespec                deadline{};
    std::uint64_t           arm_revision{};
    bool                    auto_reload;
    bool                    active{false};
    bool                    shutdown{false};
    bool                    release_from_watcher{false};
};

void qnx_timer_release(qnx_timer_ctx* ctx) noexcept
{
    pthread_cond_destroy(&ctx->changed);
    pthread_mutex_destroy(&ctx->mutex);
    delete ctx;
}

/// @brief Owned watcher serializes callbacks and makes join a lifetime barrier.
void* qnx_timer_watcher(void* raw) noexcept
{
    auto* ctx = static_cast<qnx_timer_ctx*>(raw);
    pthread_mutex_lock(&ctx->mutex);
    while (!ctx->shutdown)
    {
        while (!ctx->shutdown && !ctx->active)
        {
            pthread_cond_wait(&ctx->changed, &ctx->mutex);
        }
        if (ctx->shutdown)
        {
            break;
        }
        const timespec deadline = ctx->deadline;
        const auto     revision = ctx->arm_revision;
        const int      rc       = pthread_cond_timedwait(&ctx->changed, &ctx->mutex, &deadline);
        if (ctx->shutdown || rc != ETIMEDOUT || !ctx->active || revision != ctx->arm_revision)
        {
            continue;
        }
        if (!ctx->auto_reload)
        {
            ctx->active = false;
        }
        pthread_mutex_unlock(&ctx->mutex);
        ctx->fn(ctx->arg);
        pthread_mutex_lock(&ctx->mutex);
        if (ctx->active && ctx->auto_reload && revision == ctx->arm_revision)
        {
            ctx->deadline = ms_to_abs_mono(ctx->period_ticks);
        }
    }
    const bool release = ctx->release_from_watcher;
    pthread_mutex_unlock(&ctx->mutex);
    if (release)
    {
        pthread_detach(pthread_self());
        qnx_timer_release(ctx);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Queue name helper
// ---------------------------------------------------------------------------
std::atomic<unsigned> qnx_q_counter{0};

struct qnx_queue_ctx
{
    mqd_t mq;
    mqd_t nonblocking_mq;
    long  msg_size;
};

struct qnx_thread_ctx
{
    void (*entry)(void*);
    void*            arg;
    osal::affinity_t affinity;
};

void* qnx_thread_entry(void* raw) noexcept
{
    auto* ctx = static_cast<qnx_thread_ctx*>(raw);
    if (ctx->affinity != osal::AFFINITY_ANY)
    {
        ThreadCtl(_NTO_TCTL_RUNMASK, reinterpret_cast<void*>(static_cast<uintptr_t>(ctx->affinity)));
    }
    const auto entry = ctx->entry;
    void*      arg   = ctx->arg;
    delete ctx;
    entry(arg);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Initialise a pthread_cond_t that uses CLOCK_MONOTONIC for timed waits.
// QNX supports this via standard condattr.
// ---------------------------------------------------------------------------
int cond_init_monotonic(pthread_cond_t* cond) noexcept
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared-include macro contracts for posix_condvar.inl and posix_rwlock.inl.
// Standard timed rwlocks use CLOCK_REALTIME; condition variables use CLOCK_MONOTONIC.
// ---------------------------------------------------------------------------
#define OSAL_POSIX_RW_ABS(t) osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_REALTIME, t)
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
    /// @param[out] handle      Output handle owns a heap-allocated pthread_t pointer.
    /// @param[in] entry        Thread entry function.
    /// @param[in] arg          Opaque argument forwarded to @p entry.
    /// @param[in] priority     OSAL priority mapped to QNX SCHED_RR range via osal_to_qnx_priority().
    /// @param[in] stack_bytes  Stack size in bytes; optional @p stack used if non-null.
    /// @retval osal::ok()                          On success.
    /// @retval osal::error_code::out_of_resources  On failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        if (handle == nullptr || entry == nullptr || (stack != nullptr && stack_bytes == 0U))
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
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

        auto* ctx = new (std::nothrow) qnx_thread_ctx{entry, arg, affinity};
        if (!ctx)
        {
            pthread_attr_destroy(&attr);
            delete tid;
            return osal::error_code::out_of_resources;
        }
        const int rc = pthread_create(tid, &attr, qnx_thread_entry, ctx);
        pthread_attr_destroy(&attr);

        if (rc != 0)
        {
            delete ctx;
            delete tid;
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(tid);
        return osal::ok();
    }

    /// @brief Join a thread using pthread_timedjoin_monotonic() for finite timeouts.
    /// @param[in,out] handle  Thread handle to join.
    /// @param[in] timeout     Maximum wait in OSAL ticks; only WAIT_FOREVER uses pthread_join().
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::timeout          On failure.
    /// @retval osal::error_code::not_initialized  On failure.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* tid = static_cast<pthread_t*>(handle->native);

        int rc{0};
        if (timeout == osal::WAIT_FOREVER)
        {
            rc = pthread_join(*tid, nullptr);
        }
        else
        {
            struct timespec ts = ms_to_abs_mono(timeout);
            rc                 = pthread_timedjoin_monotonic(*tid, nullptr, &ts);
        }
        if (rc != 0)
        {
            return rc == ETIMEDOUT ? osal::error_code::timeout : osal::error_code::unknown;
        }
        delete tid;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Detach the pthread via pthread_detach() and free its handle.
    /// @param[in,out] handle  Thread handle to detach.
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::not_initialized  If the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* tid = static_cast<pthread_t*>(handle->native);
        if (pthread_detach(*tid) != 0)
        {
            return osal::error_code::unknown;
        }
        delete tid;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change a thread's SCHED_RR priority via pthread_setschedparam().
    /// @param[in] handle    Thread handle.
    /// @param[in] priority  New OSAL priority mapped through osal_to_qnx_priority().
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::not_initialized  On failure.
    /// @retval osal::error_code::unknown          On failure.
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

    /// @brief Set the target thread CPU affinity via QNX ThreadCtlExt.
    /// @param[in] handle    Thread handle.
    /// @param[in] affinity  CPU runmask; each bit represents a processor.
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::not_initialized  On failure.
    /// @retval osal::error_code::unknown          On failure.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto      rmask = static_cast<unsigned>(affinity);
        const int rc    = ThreadCtlExt(0, static_cast<int>(*static_cast<pthread_t*>(handle->native)), _NTO_TCTL_RUNMASK,
                                       reinterpret_cast<void*>(static_cast<uintptr_t>(rmask)));
        return (rc == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Suspend a thread (not supported through this OSAL on QNX).
    /// @retval osal::error_code::not_supported  Always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported through this OSAL on QNX).
    /// @retval osal::error_code::not_supported  Always.
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
    /// @param[in] ms  Delay in milliseconds.
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
    /// @param[out] handle    Output handle owns a heap-allocated pthread_mutex_t.
    /// @param[in] recursive  True for PTHREAD_MUTEX_RECURSIVE type.
    /// @retval osal::ok()                          On success.
    /// @retval osal::error_code::out_of_resources  On failure.
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
    /// @param[in,out] handle  Mutex handle; no-op if null or already destroyed.
    /// @retval osal::ok()  Always.
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
    /// @param[in] handle         Mutex handle.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks; WAIT_FOREVER uses pthread_mutex_lock().
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::would_block      The operation would block.
    /// @retval osal::error_code::timeout          The operation timed out.
    /// @retval osal::error_code::not_initialized  The object is not initialized.
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
    /// @param[in] handle  Mutex handle.
    /// @retval osal::ok()                     If acquired.
    /// @retval osal::error_code::would_block  Otherwise.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release the pthread_mutex_t via pthread_mutex_unlock().
    /// @param[in] handle  Mutex handle.
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::not_initialized  On failure.
    /// @retval osal::error_code::not_owner        On failure.
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

#include "../common/posix/posix_bounded_semaphore.inl"

    /// @brief Allocate and initialise a bounded POSIX sem_t via sem_init().
    /// @param[out] handle        Output handle owns a heap-allocated sem_t.
    /// @param[in] initial_count  Starting token count.
    /// @param[in] max_count      Maximum token count; excess gives return overflow.
    /// @retval osal::ok()                          On success.
    /// @retval osal::error_code::out_of_resources  On failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || initial_count > max_count)
        {
            return osal::error_code::invalid_argument;
        }
        auto* s = new (std::nothrow) posix_bounded_semaphore{};
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        if (sem_init(&s->sem, 0, initial_count) != 0)
        {
            delete s;
            return osal::error_code::out_of_resources;
        }
        s->count.store(initial_count, std::memory_order_relaxed);
        s->maximum     = max_count;
        handle->native = static_cast<void*>(s);
        return osal::ok();
    }

    /// @brief Destroy the sem_t via sem_destroy() and free heap storage.
    /// @param[in,out] handle  Semaphore handle; no-op if null or already destroyed.
    /// @retval osal::ok()  Always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* s = static_cast<posix_bounded_semaphore*>(handle->native);
        sem_destroy(&s->sem);
        delete s;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (post) the semaphore via sem_post().
    /// @param[in] handle  Semaphore handle.
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::not_initialized  On failure.
    /// @retval osal::error_code::overflow         On failure.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return bounded_sem_post(static_cast<posix_bounded_semaphore*>(handle->native));
    }

    /// @brief Increment the semaphore from ISR context (sem_post is ISR-safe on QNX).
    /// @param[in] handle  Semaphore handle.
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::overflow         On failure.
    /// @retval osal::error_code::not_initialized  On failure.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);
    }

    /// @brief Decrement (wait on) the semaphore.
    /// @details Uses sem_wait() (forever), sem_trywait() (NO_WAIT), or
    ///          QNX sem_timedwait_monotonic() (timed).
    /// @param[in] handle         Semaphore handle.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks.
    /// @retval osal::ok()                     On success.
    /// @retval osal::error_code::would_block  The operation would block.
    /// @retval osal::error_code::timeout      The operation timed out.
    /// @retval osal::error_code::unknown      The native operation failed.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<posix_bounded_semaphore*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            while (bounded_sem_complete(s, sem_wait(&s->sem)) != 0)
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
            const int rc = bounded_sem_complete(s, sem_trywait(&s->sem));
            return (rc == 0) ? osal::ok() : osal::error_code::would_block;
        }
        // QNX-specific monotonic timed wait
        struct timespec ts = ms_to_abs_mono(timeout_ticks);
        int             rc{0};
        do
        {
            rc = bounded_sem_complete(s, sem_timedwait_monotonic(&s->sem, &ts));
        } while (rc == -1 && errno == EINTR);
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Try to decrement the semaphore without blocking.
    /// @param[in] handle  Semaphore handle.
    /// @retval osal::ok()                     If a token was available.
    /// @retval osal::error_code::would_block  Otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (POSIX message queue)
    // ---------------------------------------------------------------------------

    /// @brief Create a POSIX message queue with a unique name; unlinked immediately after open.
    /// @details Separate opens retain fixed blocking and nonblocking modes;
    ///          mq_unlink() ensures the queue is cleaned up when closed.
    /// @param[out] handle    Output handle owns a heap-allocated {mqd_t, msg_size} context.
    /// @param[in] item_size  Size in bytes of each message.
    /// @param[in] capacity   Maximum number of messages the queue can hold.
    /// @retval osal::ok()                          On success.
    /// @retval osal::error_code::out_of_resources  On failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        if (handle == nullptr || item_size == 0U || capacity == 0U || item_size > static_cast<std::size_t>(LONG_MAX) ||
            capacity > static_cast<std::size_t>(LONG_MAX))
        {
            return osal::error_code::invalid_argument;
        }
        char           name[32]{};
        const unsigned id = qnx_q_counter.fetch_add(1U, std::memory_order_relaxed);
        (void)snprintf(name, sizeof(name), "/osal_q_%u_%u",
                       static_cast<unsigned>(getpid()),  // NOLINT(cppcoreguidelines-pro-type-vararg)
                       id);

        struct mq_attr attr
        {
        };
        attr.mq_maxmsg  = static_cast<long>(capacity);
        attr.mq_msgsize = static_cast<long>(item_size);

        mqd_t mq = mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (mq == static_cast<mqd_t>(-1))
        {
            return osal::error_code::out_of_resources;
        }
        const mqd_t nonblocking_mq = mq_open(name, O_RDWR | O_NONBLOCK);
        mq_unlink(name);  // Each descriptor retains its own immutable blocking mode.
        if (nonblocking_mq == static_cast<mqd_t>(-1))
        {
            mq_close(mq);
            return osal::error_code::out_of_resources;
        }

        // Store mqd_t as a pointer-sized value
        auto* ctx = new (std::nothrow) qnx_queue_ctx;
        if (ctx == nullptr)
        {
            mq_close(nonblocking_mq);
            mq_close(mq);
            return osal::error_code::out_of_resources;
        }
        ctx->mq             = mq;
        ctx->nonblocking_mq = nonblocking_mq;
        ctx->msg_size       = attr.mq_msgsize;
        handle->native      = static_cast<void*>(ctx);
        return osal::ok();
    }

    /// @brief Close the message queue via mq_close() and free the context.
    /// @param[in,out] handle  Queue handle; no-op if null or already destroyed.
    /// @retval osal::ok()  Always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* ctx = static_cast<qnx_queue_ctx*>(handle->native);
        mq_close(ctx->mq);
        mq_close(ctx->nonblocking_mq);
        delete ctx;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send a message via mq_send() or QNX mq_timedsend_monotonic().
    /// @param[in] handle         Queue handle.
    /// @param[in] item           Pointer to the message data.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks; NO_WAIT uses non-blocking send.
    /// @retval osal::ok()                     On success.
    /// @retval osal::error_code::would_block  If the queue is full.
    /// @retval osal::error_code::timeout      If the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<qnx_queue_ctx*>(handle->native);

        if (timeout_ticks == osal::NO_WAIT)
        {
            const int rc = mq_send(ctx->nonblocking_mq, static_cast<const char*>(item),
                                   static_cast<std::size_t>(ctx->msg_size), 0);
            return (rc == 0) ? osal::ok() : osal::error_code::would_block;
        }
        int rc = 0;
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
        if (rc == 0)
        {
            return osal::ok();
        }
        return (errno == ETIMEDOUT) ? osal::error_code::timeout : osal::error_code::would_block;
    }

    /// @brief Send a message non-blocking (ISR-compatible).
    /// @param[in] handle  Queue handle.
    /// @param[in] item    Pointer to the message data.
    /// @retval osal::ok()                     On success.
    /// @retval osal::error_code::would_block  If the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive a message via mq_receive() or QNX mq_timedreceive_monotonic().
    /// @param[in] handle         Queue handle.
    /// @param[in] item           Buffer to receive the dequeued message into.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks; NO_WAIT uses non-blocking receive.
    /// @retval osal::ok()                     On success.
    /// @retval osal::error_code::would_block  If the queue is empty.
    /// @retval osal::error_code::timeout      If the queue is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<qnx_queue_ctx*>(handle->native);

        if (timeout_ticks == osal::NO_WAIT)
        {
            const ssize_t n = mq_receive(ctx->nonblocking_mq, static_cast<char*>(item),
                                         static_cast<std::size_t>(ctx->msg_size), nullptr);
            return (n >= 0) ? osal::ok() : osal::error_code::would_block;
        }
        ssize_t n = 0;
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
        if (n >= 0)
        {
            return osal::ok();
        }
        return (errno == ETIMEDOUT) ? osal::error_code::timeout : osal::error_code::would_block;
    }

    /// @brief Receive a message non-blocking (ISR-compatible).
    /// @param[in] handle  Queue handle.
    /// @param[in] item    Buffer to receive the dequeued message into.
    /// @retval osal::ok()                     On success.
    /// @retval osal::error_code::would_block  If the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the front message without removing it (POSIX mq has no peek).
    /// @retval osal::error_code::not_supported  Always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Return the number of messages currently in the queue via mq_getattr().
    /// @param[in] handle  Queue handle.
    /// @return Message count (mq_curmsgs), or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* ctx = static_cast<const qnx_queue_ctx*>(handle->native);
        struct mq_attr attr
        {
        };
        mq_getattr(ctx->mq, &attr);
        return static_cast<std::size_t>(attr.mq_curmsgs);
    }

    /// @brief Return the number of free message slots via mq_getattr() (mq_maxmsg - mq_curmsgs).
    /// @param[in] handle  Queue handle.
    /// @return Free slot count, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* ctx = static_cast<const qnx_queue_ctx*>(handle->native);
        struct mq_attr attr
        {
        };
        mq_getattr(ctx->mq, &attr);
        return static_cast<std::size_t>(attr.mq_maxmsg - attr.mq_curmsgs);
    }

    // ---------------------------------------------------------------------------
    // Timer (CLOCK_MONOTONIC + owned pthread watcher)
    // ---------------------------------------------------------------------------

    /// @brief Create a CLOCK_MONOTONIC timer with serialized watcher callbacks.
    /// @details Periodic callbacks use fixed-delay scheduling, like the POSIX backend.
    /// @param[out] handle       Output handle owns a heap-allocated qnx_timer_ctx.
    /// @param[in] callback      Function called on each expiry.
    /// @param[in] arg           Opaque argument forwarded to @p callback.
    /// @param[in] period_ticks  Expiry period in OSAL ticks (milliseconds).
    /// @param[in] auto_reload   If true, wait a full period after each callback completes.
    /// @retval osal::ok()                          On success.
    /// @retval osal::error_code::out_of_resources  On failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || callback == nullptr || period_ticks == 0U || period_ticks == osal::WAIT_FOREVER)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        auto* ctx = new (std::nothrow) qnx_timer_ctx{};
        if (ctx == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        ctx->fn           = callback;
        ctx->arg          = arg;
        ctx->auto_reload  = auto_reload;
        ctx->period_ticks = period_ticks;
        if (pthread_mutex_init(&ctx->mutex, nullptr) != 0)
        {
            delete ctx;
            return osal::error_code::out_of_resources;
        }
        if (cond_init_monotonic(&ctx->changed) != 0)
        {
            pthread_mutex_destroy(&ctx->mutex);
            delete ctx;
            return osal::error_code::out_of_resources;
        }
        if (pthread_create(&ctx->watcher, nullptr, qnx_timer_watcher, ctx) != 0)
        {
            qnx_timer_release(ctx);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(ctx);
        return osal::ok();
    }

    /// @brief Disarm the timer, wait for any in-flight callback to finish, then delete it.
    /// @param[in] handle  Timer handle; no-op if null.
    /// @retval osal::ok()  Always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* ctx = static_cast<qnx_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        ctx->active   = false;
        ctx->shutdown = true;
        pthread_cond_broadcast(&ctx->changed);
        if (pthread_equal(pthread_self(), ctx->watcher))
        {
            ctx->release_from_watcher = true;
            pthread_mutex_unlock(&ctx->mutex);
            handle->native = nullptr;
            return osal::ok();
        }
        pthread_mutex_unlock(&ctx->mutex);
        if (pthread_join(ctx->watcher, nullptr) != 0)
        {
            return osal::error_code::unknown;
        }
        qnx_timer_release(ctx);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Arm the timer watcher with the stored period.
    /// @param[in] handle  Timer handle.
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::not_initialized  On failure.
    /// @retval osal::error_code::unknown          On failure.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<qnx_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        ctx->active   = ctx->period_ticks != osal::NO_WAIT;
        ctx->deadline = ms_to_abs_mono(ctx->period_ticks);
        ++ctx->arm_revision;
        pthread_cond_broadcast(&ctx->changed);
        pthread_mutex_unlock(&ctx->mutex);
        return osal::ok();
    }

    /// @brief Disarm the timer watcher.
    /// @param[in] handle  Timer handle.
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::not_initialized  If the handle is null.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<qnx_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        ctx->active = false;
        pthread_cond_broadcast(&ctx->changed);
        pthread_mutex_unlock(&ctx->mutex);
        return osal::ok();
    }

    /// @brief Stop and immediately restart the timer.
    /// @param[in] handle  Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Update the period; an active timer begins a fresh interval.
    /// @param[in] handle            Timer handle.
    /// @param[in] new_period_ticks  New period in OSAL ticks (milliseconds).
    /// @retval osal::ok()                         On success.
    /// @retval osal::error_code::not_initialized  If the handle is null.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<qnx_timer_ctx*>(handle->native);
        if (new_period_ticks == 0U || new_period_ticks == osal::WAIT_FOREVER)
        {
            return osal::error_code::invalid_argument;
        }
        pthread_mutex_lock(&ctx->mutex);
        ctx->period_ticks = new_period_ticks;
        if (ctx->active)
        {
            ctx->deadline = ms_to_abs_mono(new_period_ticks);
            ++ctx->arm_revision;
            pthread_cond_broadcast(&ctx->changed);
        }
        pthread_mutex_unlock(&ctx->mutex);
        return osal::ok();
    }

    /// @brief Query whether the timer watcher is armed.
    /// @param[in] handle  Timer handle.
    /// @return True if it_value is non-zero (timer has not yet fired or is periodic).
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        const auto* ctx = static_cast<const qnx_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        const bool active = ctx->active;
        pthread_mutex_unlock(&ctx->mutex);
        return active;
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
    /// @retval osal::error_code::not_supported  Always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set (not supported in this release).
    /// @retval osal::error_code::not_supported  Always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add an object to a wait-set (not supported in this release).
    /// @retval osal::error_code::not_supported  Always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove an object from a wait-set (not supported in this release).
    /// @retval osal::error_code::not_supported  Always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait on a wait-set (not supported in this release).
    /// @param[in] n  Sets *n to 0.
    /// @retval osal::error_code::not_supported  Always.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t*, int*, std::size_t, std::size_t* n,
                                    osal::tick_t) noexcept
    {
        if (n != nullptr)
        {
            *n = 0U;
        }
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
