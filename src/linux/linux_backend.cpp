// SPDX-License-Identifier: Apache-2.0
/// @file linux_backend.cpp
/// @brief Linux-specific OSAL backend — extends the POSIX backend with
///        Linux-only syscalls for higher-performance primitives.
///
///          Specific Linux extensions used:
///          - `timerfd_create` + `epoll_wait` for the wait_set
///          - `eventfd` for event_flags ISR-safe path
///          - `epoll_create1`/`epoll_ctl`/`epoll_wait` for wait_set
///          - `sched_setaffinity` / `pthread_setaffinity_np` for affinity
///          - `pthread_timedjoin_np` for timed join
///
///          All other primitives (mutex, semaphore, queue, timer, clock)
///          delegate to the same code as posix_backend.cpp — they are
///          re-implemented here directly rather than added via #include to
///          avoid violating ODR if both backends were somehow linked.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
// Ensure glibc exposes Linux-specific APIs (sem_clockwait, pthread_mutex_clocklock, etc.).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef OSAL_BACKEND_LINUX
#define OSAL_BACKEND_LINUX
#endif
#include <osal/osal.hpp>
#include "../common/backend_timeout_adapter.hpp"

// Linux-only headers
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sched.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <ctime>
#include <poll.h>
#include <cerrno>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <new>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

template<typename Handle>
[[nodiscard]] constexpr bool handle_is_null(const Handle* handle) noexcept
{
    return (handle == nullptr) || (handle->native == nullptr);
}

constexpr int          kLinuxMaxAffinityCpus      = 64;
constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000LL;

/// @brief Compute an absolute timespec on CLOCK_MONOTONIC, offset by @p t ticks
///        (1 tick = 1 ms on the Linux backend).
[[nodiscard]] timespec ms_to_abs_timespec(osal::tick_t t) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_MONOTONIC, t);
}

/// @brief Initialise a pthread_cond_t that uses CLOCK_MONOTONIC for timed waits.
int cond_init_monotonic(pthread_cond_t* cond) noexcept
{
    pthread_condattr_t attr{};
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    const int rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

struct linux_thread_ctx
{
    void (*entry)(void*);
    void* arg;
};

void* linux_thread_entry(void* raw) noexcept
{
    auto* const ctx = static_cast<linux_thread_ctx*>(raw);
    if (ctx == nullptr)
    {
        return nullptr;
    }

    ctx->entry(ctx->arg);
    delete ctx;
    return nullptr;
}

struct linux_queue_obj
{
    mutable pthread_mutex_t mutex;
    pthread_cond_t          not_full;
    pthread_cond_t          not_empty;
    std::uint8_t*           buf;
    std::size_t             item_size, capacity, head, tail, count;
};

struct linux_timer_ctx
{
    osal_timer_callback_t fn;
    void*                 arg;
    int                   timerfd;
    int                   shutdown_fd;  ///< eventfd used for clean thread shutdown.
    pthread_t             watcher;
    std::atomic<bool>     running;
    bool                  auto_reload;
    struct itimerspec     spec;
};

void* linux_timer_watcher(void* raw) noexcept
{
    auto* const ctx = static_cast<linux_timer_ctx*>(raw);
    if (ctx == nullptr)
    {
        return nullptr;
    }

    struct pollfd fds[2]{};
    fds[0].fd     = ctx->timerfd;
    fds[0].events = POLLIN;
    fds[1].fd     = ctx->shutdown_fd;
    fds[1].events = POLLIN;

    while (ctx->running.load(std::memory_order_acquire))
    {
        const int ret = poll(fds, 2, -1);
        if (ret <= 0)
        {
            continue;
        }

        // Shutdown signalled — exit immediately.
        if ((fds[1].revents & POLLIN) != 0)
        {
            break;
        }

        if ((fds[0].revents & POLLIN) != 0)
        {
            std::uint64_t exp{0U};
            const ssize_t n = read(ctx->timerfd, &exp, sizeof(exp));
            if ((n == static_cast<ssize_t>(sizeof(exp))) && (ctx->fn != nullptr))
            {
                ctx->fn(ctx->arg);
            }
        }
    }
    return nullptr;
}

struct linux_wait_set_obj
{
    int epfd;
};

}  // namespace

// ---------------------------------------------------------------------------
// Shared-include macro contracts for posix_rwlock.inl, posix_condvar.inl,
// and posix_pthread_work_queue.inl.
// Linux: all timed operations use CLOCK_MONOTONIC (GNU extensions available).
// ---------------------------------------------------------------------------
#define OSAL_POSIX_RW_ABS(t) ms_to_abs_timespec(t)
#define OSAL_POSIX_COND_ABS(t) ms_to_abs_timespec(t)

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock (Linux CLOCK_MONOTONIC / CLOCK_REALTIME)
    // ---------------------------------------------------------------------------

#include "../common/posix/posix_clock.inl"

    /// @brief Return the current CLOCK_MONOTONIC time in milliseconds (1 tick = 1 ms on Linux).
    /// @return Monotonic millisecond tick count via osal_clock_monotonic_ms().
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(osal_clock_monotonic_ms());
    }

    /// @brief Return the tick period in microseconds (1 tick = 1 ms on Linux).
    /// @return 1000 always.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1'000U;
    }

    // ---------------------------------------------------------------------------
    // Thread (identical to POSIX, plus affinity via sched_setaffinity)
    // ---------------------------------------------------------------------------

    /// @brief Create a Linux pthread with optional SCHED_FIFO priority, affinity, and name.
    /// @details Calls pthread_setname_np() if @p name is non-null.
    ///          Calls pthread_setaffinity_np() if @p affinity != AFFINITY_ANY.
    /// @param handle      Output handle stores pthread_t as a void pointer.
    /// @param entry       Thread entry function.
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority; non-NORMAL selects SCHED_FIFO.
    /// @param affinity    CPU bitmask; each bit represents a processor.
    /// @param stack_bytes Stack size in bytes; optional @p stack used if non-null.
    /// @param name        Optional thread name (up to 15 chars on Linux).
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert((handle != nullptr) && (entry != nullptr));
        auto* ctx = new (std::nothrow) linux_thread_ctx{entry, arg};
        if (ctx == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        auto* thread_id = new (std::nothrow) pthread_t{};
        if (thread_id == nullptr)
        {
            delete ctx;
            return osal::error_code::out_of_resources;
        }

        pthread_attr_t attr{};
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        if ((stack != nullptr) && (stack_bytes > 0U))
        {
            pthread_attr_setstack(&attr, stack, stack_bytes);
        }

        if (priority != osal::PRIORITY_NORMAL)
        {
            const int policy = SCHED_FIFO;
            struct sched_param sp
            {
            };
            sp.sched_priority = sched_get_priority_min(policy) +
                                static_cast<int>((static_cast<std::uint32_t>(priority) *
                                                  static_cast<std::uint32_t>(sched_get_priority_max(policy) -
                                                                             sched_get_priority_min(policy))) /
                                                 static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
            pthread_attr_setschedpolicy(&attr, policy);
            pthread_attr_setschedparam(&attr, &sp);
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        }

        const int rc = pthread_create(thread_id, &attr, linux_thread_entry, ctx);
        pthread_attr_destroy(&attr);
        if (rc != 0)
        {
            delete ctx;
            delete thread_id;
            return osal::error_code::out_of_resources;
        }

        if (name != nullptr)
        {
            pthread_setname_np(*thread_id, name);
        }

        if (affinity != osal::AFFINITY_ANY)
        {
            cpu_set_t cpuset{};
            CPU_ZERO(&cpuset);
            for (int cpu = 0; cpu < kLinuxMaxAffinityCpus; ++cpu)
            {
                if ((affinity & (1U << static_cast<unsigned>(cpu))) != 0U)
                {
                    CPU_SET(cpu, &cpuset);
                }
            }
            pthread_setaffinity_np(*thread_id, sizeof(cpuset), &cpuset);
        }

        handle->native = thread_id;
        return osal::ok();
    }

    /// @brief Join a thread using pthread_timedjoin_np() for finite timeouts.
    /// @param handle  Thread handle.
    /// @param timeout Maximum wait in OSAL ticks; WAIT_FOREVER uses pthread_join().
    /// @return osal::ok() on success; osal::error_code::timeout, ::unknown, or ::not_initialized.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* const thread_id = static_cast<pthread_t*>(handle->native);
        if (timeout == osal::WAIT_FOREVER)
        {
            if (pthread_join(*thread_id, nullptr) != 0)
            {
                return osal::error_code::unknown;
            }
            delete thread_id;
            handle->native = nullptr;
            return osal::ok();
        }
        const struct timespec abs = ms_to_abs_timespec(timeout);
        const int             rc  = pthread_timedjoin_np(*thread_id, nullptr, &abs);
        if (rc == 0)
        {
            delete thread_id;
            handle->native = nullptr;
            return osal::ok();
        }
        return (rc == ETIMEDOUT) ? osal::error_code::timeout : osal::error_code::unknown;
    }

    /// @brief Detach the pthread via pthread_detach().
    /// @param handle Thread handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* const thread_id = static_cast<pthread_t*>(handle->native);
        pthread_detach(*thread_id);
        delete thread_id;
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
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* const thread_id = static_cast<pthread_t*>(handle->native);
        struct sched_param sp
        {
        };
        int policy{0};
        pthread_getschedparam(*thread_id, &policy, &sp);
        sp.sched_priority = sched_get_priority_min(policy) +
                            static_cast<int>((static_cast<std::uint32_t>(priority) *
                                              static_cast<std::uint32_t>(sched_get_priority_max(policy) -
                                                                         sched_get_priority_min(policy))) /
                                             static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
        return (pthread_setschedparam(*thread_id, policy, &sp) == 0) ? osal::ok() : osal::error_code::permission_denied;
    }

    /// @brief Set thread CPU affinity via pthread_setaffinity_np().
    /// @param handle   Thread handle.
    /// @param affinity CPU bitmask; each set bit represents a processor to allow.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* const thread_id = static_cast<pthread_t*>(handle->native);
        cpu_set_t   cpuset{};
        CPU_ZERO(&cpuset);
        for (int cpu = 0; cpu < kLinuxMaxAffinityCpus; ++cpu)
        {
            if ((affinity & (1U << static_cast<unsigned>(cpu))) != 0U)
            {
                CPU_SET(cpu, &cpuset);
            }
        }
        return (pthread_setaffinity_np(*thread_id, sizeof(cpuset), &cpuset) == 0) ? osal::ok()
                                                                                  : osal::error_code::unknown;
    }

    /// @brief Suspend a thread (not supported on Linux through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported on Linux through this OSAL).
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
        const auto nanoseconds = static_cast<std::int64_t>(ms % 1000U) * kNanosecondsPerMillisecond;
        const struct timespec ts
        {
            static_cast<time_t>(ms / 1000U), static_cast<decltype(timespec{}.tv_nsec)>(nanoseconds),
        };
        nanosleep(&ts, nullptr);
    }

    // ---------------------------------------------------------------------------
    // Mutex, Semaphore, Queue — identical to posix_backend
    // ---------------------------------------------------------------------------

    /// @brief Allocate and initialise a pthread_mutex_t; ERRORCHECK or RECURSIVE type.
    /// @param handle    Output handle owns a heap-allocated pthread_mutex_t.
    /// @param recursive True selects PTHREAD_MUTEX_RECURSIVE; false selects ERRORCHECK.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept
    {
        auto* m = new (std::nothrow) pthread_mutex_t;
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        pthread_mutexattr_t attr{};
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
        const int rc = pthread_mutex_init(m, &attr);
        pthread_mutexattr_destroy(&attr);
        if (rc != 0)
        {
            delete m;
            return osal::error_code::out_of_resources;
        }
        handle->native = m;
        return osal::ok();
    }

    /// @brief Destroy the pthread_mutex_t and free heap storage.
    /// @param handle Mutex handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::ok();
        }
        pthread_mutex_destroy(static_cast<pthread_mutex_t*>(handle->native));
        delete static_cast<pthread_mutex_t*>(handle->native);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire the mutex; uses GNU pthread_mutex_clocklock(CLOCK_MONOTONIC) for timed waits.
    /// @param handle Mutex handle.
    /// @param t      Maximum wait in OSAL ticks; WAIT_FOREVER uses pthread_mutex_lock().
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t t) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<pthread_mutex_t*>(handle->native);
        if (t == osal::WAIT_FOREVER)
        {
            return (pthread_mutex_lock(m) == 0) ? osal::ok() : osal::error_code::unknown;
        }
        if (t == osal::NO_WAIT)
        {
            return (pthread_mutex_trylock(m) == 0) ? osal::ok() : osal::error_code::would_block;
        }
        const struct timespec abs = ms_to_abs_timespec(t);
        const int             rc  = pthread_mutex_clocklock(m, CLOCK_MONOTONIC, &abs);
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
    /// @param h Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::would_block otherwise.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* h) noexcept
    {
        return osal_mutex_lock(h, osal::NO_WAIT);
    }

    /// @brief Release the mutex via pthread_mutex_unlock().
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::not_owner on failure.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        return (pthread_mutex_unlock(static_cast<pthread_mutex_t*>(handle->native)) == 0) ? osal::ok()
                                                                                          : osal::error_code::not_owner;
    }

    /// @brief Allocate and initialise an unnamed POSIX sem_t via sem_init().
    /// @param handle Output handle owns a heap-allocated sem_t.
    /// @param init   Starting token count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned init,
                                       unsigned) noexcept
    {
        auto* s = new (std::nothrow) sem_t;
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        if (sem_init(s, 0, init) != 0)
        {
            delete s;
            return osal::error_code::out_of_resources;
        }
        handle->native = s;
        return osal::ok();
    }

    /// @brief Destroy the sem_t via sem_destroy() and free heap storage.
    /// @param handle Semaphore handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
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
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        return (sem_post(static_cast<sem_t*>(handle->native)) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Increment the semaphore from ISR context (sem_post is async-signal-safe on Linux).
    /// @param h Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* h) noexcept
    {
        return osal_semaphore_give(h);
    }

    /// @brief Decrement (wait on) the semaphore.
    /// @details Uses sem_wait() (forever), sem_trywait() (NO_WAIT), or
    ///          GNU sem_clockwait(CLOCK_MONOTONIC) (timed).
    /// @param handle Semaphore handle.
    /// @param t      Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle, osal::tick_t t) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<sem_t*>(handle->native);
        if (t == osal::WAIT_FOREVER)
        {
            int rc{-1};
            while (true)
            {
                rc = sem_wait(s);
                if ((rc != -1) || (errno != EINTR))
                {
                    break;
                }
            }
            return (rc == 0) ? osal::ok() : osal::error_code::unknown;
        }
        if (t == osal::NO_WAIT)
        {
            return (sem_trywait(s) == 0) ? osal::ok() : osal::error_code::would_block;
        }
        const struct timespec abs = ms_to_abs_timespec(t);
        if (sem_clockwait(s, CLOCK_MONOTONIC, &abs) == 0)
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
    /// @param h Semaphore handle.
    /// @return osal::ok() if a token was available; osal::error_code::would_block otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* h) noexcept
    {
        return osal_semaphore_take(h, osal::NO_WAIT);
    }

    /// @brief Create a circular-buffer queue backed by caller-supplied @p buf.
    /// @details Internal mutex and condvars use CLOCK_MONOTONIC for timed waits.
    /// @param handle  Output handle owns a heap-allocated linux_queue_obj.
    /// @param buf     Caller-provided storage for capacity × item_size bytes.
    /// @param item_sz Size in bytes of each element.
    /// @param cap     Maximum number of elements the queue can hold.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buf, std::size_t item_sz,
                                   std::size_t cap) noexcept
    {
        auto* q = new (std::nothrow) linux_queue_obj{};
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        pthread_mutex_init(&q->mutex, nullptr);
        cond_init_monotonic(&q->not_full);
        cond_init_monotonic(&q->not_empty);
        q->buf       = static_cast<std::uint8_t*>(buf);
        q->item_size = item_sz;
        q->capacity  = cap;
        q->head = q->tail = q->count = 0;
        handle->native               = q;
        return osal::ok();
    }

    /// @brief Destroy the circular-buffer queue: destroy condvars, mutex, and free the object.
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::ok();
        }
        auto* q = static_cast<linux_queue_obj*>(handle->native);
        pthread_cond_destroy(&q->not_full);
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mutex);
        delete q;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send an item into the queue; blocks on not_full if full.
    /// @param handle Queue handle.
    /// @param item   Pointer to the item to copy.
    /// @param t      Maximum wait in OSAL ticks; NO_WAIT returns immediately if full.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item, osal::tick_t t) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<linux_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        while (q->count >= q->capacity)
        {
            if (t == osal::NO_WAIT)
            {
                pthread_mutex_unlock(&q->mutex);
                return osal::error_code::would_block;
            }
            if (t == osal::WAIT_FOREVER)
            {
                pthread_cond_wait(&q->not_full, &q->mutex);
            }
            else
            {
                const struct timespec abs = ms_to_abs_timespec(t);
                if (pthread_cond_timedwait(&q->not_full, &q->mutex, &abs) == ETIMEDOUT)
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
    /// @param h Queue handle.
    /// @param i Pointer to the item to copy.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* h, const void* i) noexcept
    {
        return osal_queue_send(h, i, osal::NO_WAIT);
    }

    /// @brief Receive an item from the queue; blocks on not_empty if the queue is empty.
    /// @param handle Queue handle.
    /// @param item   Buffer to copy the dequeued item into.
    /// @param t      Maximum wait in OSAL ticks; NO_WAIT returns immediately if empty.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item, osal::tick_t t) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<linux_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        while (q->count == 0)
        {
            if (t == osal::NO_WAIT)
            {
                pthread_mutex_unlock(&q->mutex);
                return osal::error_code::would_block;
            }
            if (t == osal::WAIT_FOREVER)
            {
                pthread_cond_wait(&q->not_empty, &q->mutex);
            }
            else
            {
                const struct timespec abs = ms_to_abs_timespec(t);
                if (pthread_cond_timedwait(&q->not_empty, &q->mutex, &abs) == ETIMEDOUT)
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
    /// @param h Queue handle.
    /// @param i Buffer to copy the dequeued item into.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* h, void* i) noexcept
    {
        return osal_queue_receive(h, i, osal::NO_WAIT);
    }

    /// @brief Copy the front item without removing it (timeout parameter ignored).
    /// @param handle Queue handle.
    /// @param item   Buffer to copy the head item into.
    /// @return osal::ok() if an item was available; osal::error_code::would_block if empty.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item, osal::tick_t) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* q = static_cast<linux_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        if (q->count == 0)
        {
            pthread_mutex_unlock(&q->mutex);
            return osal::error_code::would_block;
        }
        std::memcpy(item, q->buf + q->head * q->item_size, q->item_size);
        pthread_mutex_unlock(&q->mutex);
        return osal::ok();
    }

    /// @brief Return the number of items currently in the queue.
    /// @param h Queue handle.
    /// @return Item count, or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* h) noexcept
    {
        if (handle_is_null(h))
        {
            return 0U;
        }
        const auto* q = static_cast<const linux_queue_obj*>(h->native);
        pthread_mutex_lock(&q->mutex);
        const std::size_t n = q->count;
        pthread_mutex_unlock(&q->mutex);
        return n;
    }

    /// @brief Return the number of free slots in the queue.
    /// @param h Queue handle.
    /// @return Free slot count, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* h) noexcept
    {
        if (handle_is_null(h))
        {
            return 0U;
        }
        const auto* q = static_cast<const linux_queue_obj*>(h->native);
        pthread_mutex_lock(&q->mutex);
        const std::size_t n = q->capacity - q->count;
        pthread_mutex_unlock(&q->mutex);
        return n;
    }

    // ---------------------------------------------------------------------------
    // Timer (timerfd + background pthread)
    // ---------------------------------------------------------------------------

    /// @brief Create a CLOCK_MONOTONIC timerfd timer; a background thread reads expiry counts.
    /// @details The watcher thread is spawned lazily on the first osal_timer_start() call.
    /// @param handle       Output handle owns a heap-allocated linux_timer_ctx.
    /// @param cb           Function called on each expiry.
    /// @param arg          Opaque argument forwarded to @p cb.
    /// @param period_ticks Expiry period in OSAL ticks (milliseconds).
    /// @param auto_reload  If true, sets it_interval; otherwise one-shot.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert((handle != nullptr) && (cb != nullptr));
        auto* ctx = new (std::nothrow) linux_timer_ctx{};
        if (ctx == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        ctx->fn          = cb;
        ctx->arg         = arg;
        ctx->auto_reload = auto_reload;
        ctx->running.store(false, std::memory_order_relaxed);

        ctx->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (ctx->timerfd < 0)
        {
            delete ctx;
            return osal::error_code::out_of_resources;
        }

        ctx->shutdown_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (ctx->shutdown_fd < 0)
        {
            close(ctx->timerfd);
            delete ctx;
            return osal::error_code::out_of_resources;
        }

        const auto sec         = static_cast<time_t>(period_ticks / 1000U);
        const auto nanoseconds = static_cast<std::int64_t>(period_ticks % 1000U) * kNanosecondsPerMillisecond;
        const auto nsec        = static_cast<decltype(timespec{}.tv_nsec)>(nanoseconds);
        ctx->spec.it_value     = {sec, nsec};
        ctx->spec.it_interval  = auto_reload ? timespec{sec, nsec} : timespec{0, 0};

        handle->native = static_cast<void*>(ctx);
        return osal::ok();
    }

    /// @brief Arm the timerfd via timerfd_settime(); spawns the watcher pthread on first call.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* const ctx = static_cast<linux_timer_ctx*>(handle->native);
        if (!ctx->running.load(std::memory_order_acquire))
        {
            ctx->running.store(true, std::memory_order_release);
            pthread_create(&ctx->watcher, nullptr, linux_timer_watcher, ctx);
        }
        return (timerfd_settime(ctx->timerfd, 0, &ctx->spec, nullptr) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Disarm the timerfd by passing a zero itimerspec to timerfd_settime().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* const ctx = static_cast<linux_timer_ctx*>(handle->native);
        struct itimerspec dis
        {
        };
        timerfd_settime(ctx->timerfd, 0, &dis, nullptr);
        return osal::ok();
    }

    /// @brief Stop and immediately restart the timer.
    /// @param h Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* h) noexcept
    {
        osal_timer_stop(h);
        return osal_timer_start(h);
    }

    /// @brief Update the itimerspec and immediately arm the timerfd with the new period.
    /// @param handle Timer handle.
    /// @param p      New period in OSAL ticks (milliseconds).
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle, osal::tick_t p) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* const ctx         = static_cast<linux_timer_ctx*>(handle->native);
        const auto  sec         = static_cast<time_t>(p / 1000U);
        const auto  nanoseconds = static_cast<std::int64_t>(p % 1000U) * kNanosecondsPerMillisecond;
        const auto  nsec        = static_cast<decltype(timespec{}.tv_nsec)>(nanoseconds);
        ctx->spec.it_value      = {sec, nsec};
        ctx->spec.it_interval   = ctx->auto_reload ? timespec{sec, nsec} : timespec{0, 0};
        return (timerfd_settime(ctx->timerfd, 0, &ctx->spec, nullptr) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Disarm the timerfd, signal the watcher thread to exit, close fds and free the context.
    /// @param handle Timer handle; no-op if null.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::ok();
        }
        auto* const ctx = static_cast<linux_timer_ctx*>(handle->native);
        osal_timer_stop(handle);
        if (ctx->running.load(std::memory_order_acquire))
        {
            ctx->running.store(false, std::memory_order_release);
            // Signal the watcher thread to exit via the shutdown eventfd.
            const std::uint64_t val = 1U;
            (void)write(ctx->shutdown_fd, &val, sizeof(val));
            pthread_join(ctx->watcher, nullptr);
        }
        close(ctx->timerfd);
        close(ctx->shutdown_fd);
        delete ctx;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Query whether the timerfd is armed via timerfd_gettime().
    /// @param handle Timer handle.
    /// @return True if it_value is non-zero.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return false;
        }
        const auto* ctx = static_cast<const linux_timer_ctx*>(handle->native);
        struct itimerspec cur
        {
        };
        timerfd_gettime(ctx->timerfd, &cur);
        return (cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0);
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

    // ---------------------------------------------------------------------------
    // Wait-set (epoll)
    // ---------------------------------------------------------------------------

    /// @brief Create an epoll instance via epoll_create1(EPOLL_CLOEXEC).
    /// @param handle Output handle owns a heap-allocated linux_wait_set_obj with the epfd.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        const int epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0)
        {
            return osal::error_code::out_of_resources;
        }
        auto* ws = new (std::nothrow) linux_wait_set_obj{epfd};
        if (ws == nullptr)
        {
            close(epfd);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(ws);
        return osal::ok();
    }

    /// @brief Close the epoll fd and free the wait-set object.
    /// @param handle Wait-set handle; no-op if null.
    /// @return osal::ok() always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::ok();
        }
        auto* ws = static_cast<linux_wait_set_obj*>(handle->native);
        close(ws->epfd);
        delete ws;
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Register a file descriptor with the epoll instance via EPOLL_CTL_ADD.
    /// @param handle Handle to the wait-set.
    /// @param fd     File descriptor to monitor.
    /// @param events epoll event mask (EPOLLIN, EPOLLOUT, etc.).
    /// @return osal::ok() on success; osal::error_code::invalid_argument or ::not_initialized on failure.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t* handle, int fd,
                                   std::uint32_t events) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* ws = static_cast<linux_wait_set_obj*>(handle->native);
        struct epoll_event ev
        {
        };
        ev.events  = events;
        ev.data.fd = fd;
        return (epoll_ctl(ws->epfd, EPOLL_CTL_ADD, fd, &ev) == 0) ? osal::ok() : osal::error_code::invalid_argument;
    }

    /// @brief Remove a file descriptor from the epoll instance via EPOLL_CTL_DEL.
    /// @param handle Handle to the wait-set.
    /// @param fd     File descriptor to remove.
    /// @return osal::ok() on success; osal::error_code::invalid_argument if not found.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t* handle, int fd) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* ws = static_cast<linux_wait_set_obj*>(handle->native);
        return (epoll_ctl(ws->epfd, EPOLL_CTL_DEL, fd, nullptr) == 0) ? osal::ok() : osal::error_code::invalid_argument;
    }

    /// @brief Block on epoll_wait() until events fire or the timeout expires.
    /// @param handle      Wait-set handle.
    /// @param fds_ready   Output array filled with ready file descriptors.
    /// @param max_ready   Maximum ready fds to report (capped at OSAL_WAIT_SET_MAX_ENTRIES).
    /// @param n_ready     Set to the number of ready fds placed in @p fds_ready.
    /// @param timeout     Maximum wait in OSAL ticks; WAIT_FOREVER = -1, NO_WAIT = 0.
    /// @return osal::ok() if events fired; osal::error_code::timeout or ::unknown on failure.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t* handle, int* fds_ready,
                                    std::size_t max_ready, std::size_t* n_ready, osal::tick_t timeout) noexcept
    {
        if (handle_is_null(handle))
        {
            return osal::error_code::not_initialized;
        }
        auto* ws = static_cast<linux_wait_set_obj*>(handle->native);
        if (n_ready != nullptr)
        {
            *n_ready = 0U;
        }

        const int to_ms = osal::detail::backend_timeout_adapter::to_poll_timeout_ms(timeout);

        struct epoll_event events[OSAL_WAIT_SET_MAX_ENTRIES]{};
        const int max = static_cast<int>(max_ready < OSAL_WAIT_SET_MAX_ENTRIES ? max_ready : OSAL_WAIT_SET_MAX_ENTRIES);
        const int rc  = epoll_wait(ws->epfd, events, max, to_ms);
        if (rc < 0)
        {
            return osal::error_code::unknown;
        }
        if (rc == 0)
        {
            return osal::error_code::timeout;
        }

        for (int i = 0; i < rc; ++i)
        {
            if (fds_ready != nullptr)
            {
                fds_ready[i] = events[i].data.fd;
            }
        }
        if (n_ready != nullptr)
        {
            *n_ready = static_cast<std::size_t>(rc);
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
