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
#include "../common/posix/posix_mutex_policy.hpp"
#include "../common/posix/posix_thread_attributes.hpp"

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
#include <algorithm>

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
constexpr std::int64_t kNanosecondsPerSecond      = 1'000'000'000LL;

[[nodiscard]] std::int64_t timespec_to_nanoseconds(const timespec& ts) noexcept
{
    return (static_cast<std::int64_t>(ts.tv_sec) * kNanosecondsPerSecond) + static_cast<std::int64_t>(ts.tv_nsec);
}

template<typename NativeThreadId>
[[nodiscard]] osal::thread_id_t native_thread_id_token(const NativeThreadId& native_id) noexcept
{
    constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;

    std::uint64_t     hash  = kFnvOffset;
    const auto* const bytes = reinterpret_cast<const unsigned char*>(&native_id);
    for (std::size_t i = 0; i < sizeof(native_id); ++i)
    {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }

    if constexpr (sizeof(osal::thread_id_t) >= sizeof(hash))
    {
        return static_cast<osal::thread_id_t>(hash);
    }
    else
    {
        return static_cast<osal::thread_id_t>(hash ^ (hash >> 32U));
    }
}

[[nodiscard]] osal::priority_t native_priority_to_osal(int policy, int native_priority) noexcept
{
    const int max_p = sched_get_priority_max(policy);
    const int min_p = sched_get_priority_min(policy);
    if (max_p <= min_p)
    {
        return osal::PRIORITY_NORMAL;
    }

    const int clamped_priority = std::clamp(native_priority, min_p, max_p);
    return static_cast<osal::priority_t>(((clamped_priority - min_p) * osal::PRIORITY_HIGHEST) / (max_p - min_p));
}

[[nodiscard]] osal::affinity_t cpu_set_to_affinity_mask(const cpu_set_t& cpuset) noexcept
{
    constexpr int kRepresentableAffinityCpus = 32;

    osal::affinity_t affinity = 0U;
    bool             all_set  = true;
    for (int cpu = 0; cpu < kRepresentableAffinityCpus; ++cpu)
    {
        if (CPU_ISSET(cpu, &cpuset))
        {
            affinity |= (1U << static_cast<unsigned>(cpu));
        }
        else
        {
            all_set = false;
        }
    }

    return all_set ? osal::AFFINITY_ANY : affinity;
}

[[nodiscard]] timespec ms_to_abs_timespec(osal::tick_t t) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_MONOTONIC, t);
}

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
    std::atomic<bool>     release_from_watcher;
    bool                  watcher_started;
    bool                  auto_reload;
    struct itimerspec     spec;
};

void linux_timer_release(linux_timer_ctx* ctx) noexcept
{
    close(ctx->timerfd);
    close(ctx->shutdown_fd);
    delete ctx;
}

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

    for (;;)
    {
        if (!ctx->running.load(std::memory_order_acquire))
        {
            break;
        }
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

    if (ctx->release_from_watcher.load(std::memory_order_acquire))
    {
        (void)pthread_detach(pthread_self());
        linux_timer_release(ctx);
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
// Standard timed rwlocks use CLOCK_REALTIME; condition variables use CLOCK_MONOTONIC.
// ---------------------------------------------------------------------------
#define OSAL_POSIX_RW_ABS(t) osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_REALTIME, t)
#define OSAL_POSIX_COND_ABS(t) ms_to_abs_timespec(t)

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock (Linux CLOCK_MONOTONIC / CLOCK_REALTIME)
    // ---------------------------------------------------------------------------

#include "../common/posix/posix_clock.inl"

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(osal_clock_monotonic_ms());
    }

    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1'000U;
    }

    std::int64_t osal_clock_high_resolution_ns() noexcept
    {
        timespec ts{};
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        {
            return osal_clock_monotonic_ms() * kNanosecondsPerMillisecond;
        }
        return timespec_to_nanoseconds(ts);
    }

    std::int64_t osal_clock_high_resolution_resolution_ns() noexcept
    {
        timespec ts{};
        if (clock_getres(CLOCK_MONOTONIC, &ts) != 0)
        {
            return kNanosecondsPerMillisecond;
        }
        const std::int64_t resolution_ns = timespec_to_nanoseconds(ts);
        return (resolution_ns > 0) ? resolution_ns : 1LL;
    }

    // ---------------------------------------------------------------------------
    // Thread (identical to POSIX, plus affinity via sched_setaffinity)
    // ---------------------------------------------------------------------------

    // Calls pthread_setname_np() if name is non-null.
    // Calls pthread_setaffinity_np() if affinity != AFFINITY_ANY.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        if (handle == nullptr || entry == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
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
        const int      initialized = pthread_attr_init(&attr);
        int            rc          = initialized;
        if (rc == 0)
        {
            rc = osal::detail::configure_pthread_thread_attributes(&attr, priority, affinity, stack, stack_bytes);
        }
        if (rc == 0)
        {
            rc = pthread_create(thread_id, &attr, linux_thread_entry, ctx);
        }
        if (initialized == 0)
        {
            (void)pthread_attr_destroy(&attr);
        }
        if (rc != 0)
        {
            delete ctx;
            delete thread_id;
            return osal::detail::pthread_mutex_status(rc);
        }

        if (name != nullptr)
        {
            pthread_setname_np(*thread_id, name);
        }

        handle->native = thread_id;
        return osal::ok();
    }

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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
        const struct timespec abs = osal::detail::backend_timeout_adapter::to_abs_timespec(CLOCK_REALTIME, timeout);
        const int             rc  = pthread_timedjoin_np(*thread_id, nullptr, &abs);
        if (rc == 0)
        {
            delete thread_id;
            handle->native = nullptr;
            return osal::ok();
        }
        return (rc == ETIMEDOUT) ? osal::error_code::timeout : osal::error_code::unknown;
    }

    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* const thread_id = static_cast<pthread_t*>(handle->native);
        pthread_detach(*thread_id);
        delete thread_id;
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_thread_get_id(const osal::active_traits::thread_handle_t* handle,
                                    osal::thread_id_t*                          out_id) noexcept
    {
        if (out_id == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        pthread_t thread_id = pthread_self();
        if (handle != nullptr)
        {
            if (handle->native == nullptr)
            {
                return osal::error_code::not_initialized;
            }
            thread_id = *static_cast<const pthread_t*>(handle->native);
        }

        *out_id = native_thread_id_token(thread_id);
        return osal::ok();
    }

    osal::result osal_thread_get_priority(const osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t*                           out_priority) noexcept
    {
        if (out_priority == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        pthread_t thread_id = pthread_self();
        if (handle != nullptr)
        {
            if (handle->native == nullptr)
            {
                return osal::error_code::not_initialized;
            }
            thread_id = *static_cast<const pthread_t*>(handle->native);
        }

        struct sched_param sp
        {
        };
        int policy = 0;
        if (pthread_getschedparam(thread_id, &policy, &sp) != 0)
        {
            return osal::error_code::unknown;
        }

        *out_priority = native_priority_to_osal(policy, sp.sched_priority);
        return osal::ok();
    }

    osal::result osal_thread_get_affinity(const osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t*                           out_affinity) noexcept
    {
        if (out_affinity == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        pthread_t thread_id = pthread_self();
        if (handle != nullptr)
        {
            if (handle->native == nullptr)
            {
                return osal::error_code::not_initialized;
            }
            thread_id = *static_cast<const pthread_t*>(handle->native);
        }

        cpu_set_t cpuset{};
        if (pthread_getaffinity_np(thread_id, sizeof(cpuset), &cpuset) != 0)
        {
            return osal::error_code::unknown;
        }

        *out_affinity = cpu_set_to_affinity_mask(cpuset);
        return osal::ok();
    }

    osal::result osal_thread_current_cpu(std::uint32_t* out_cpu) noexcept
    {
        if (out_cpu == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        const int cpu = sched_getcpu();
        if (cpu < 0)
        {
            return osal::error_code::unknown;
        }

        *out_cpu = static_cast<std::uint32_t>(cpu);
        return osal::ok();
    }

    // Linux pthreads do not expose a portable low-watermark metric,
    // so the draft API remains unsupported on this backend.
    osal::result osal_thread_stack_low_watermark_bytes(const osal::active_traits::thread_handle_t* /*handle*/,
                                                       std::size_t* out_bytes) noexcept
    {
        if (out_bytes != nullptr)
        {
            *out_bytes = 0U;
        }
        return osal::error_code::not_supported;
    }

    // Uses CLOCK_THREAD_CPUTIME_ID for the current thread and
    // pthread_getcpuclockid() for an arbitrary joinable thread.
    osal::result osal_thread_execution_time_us(const osal::active_traits::thread_handle_t* handle,
                                               std::int64_t*                               out_us) noexcept
    {
        if (out_us == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        clockid_t clock_id = CLOCK_THREAD_CPUTIME_ID;
        if (handle != nullptr)
        {
            if (handle->native == nullptr)
            {
                return osal::error_code::not_initialized;
            }
            const auto* const thread_id = static_cast<const pthread_t*>(handle->native);
            if (pthread_getcpuclockid(*thread_id, &clock_id) != 0)
            {
                return osal::error_code::unknown;
            }
        }

        timespec ts{};
        if (clock_gettime(clock_id, &ts) != 0)
        {
            return osal::error_code::unknown;
        }

        *out_us =
            (static_cast<std::int64_t>(ts.tv_sec) * 1'000'000LL) + (static_cast<std::int64_t>(ts.tv_nsec) / 1'000LL);
        return osal::ok();
    }

    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* const thread_id = static_cast<pthread_t*>(handle->native);
        cpu_set_t   cpuset{};
        CPU_ZERO(&cpuset);
        for (int cpu = 0; cpu < kLinuxMaxAffinityCpus; ++cpu)
        {
            if (affinity == osal::AFFINITY_ANY || (cpu < 32 && (affinity & (1U << static_cast<unsigned>(cpu))) != 0U))
            {
                CPU_SET(cpu, &cpuset);
            }
        }
        return (pthread_setaffinity_np(*thread_id, sizeof(cpuset), &cpuset) == 0) ? osal::ok()
                                                                                  : osal::error_code::unknown;
    }

    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    void osal_thread_yield() noexcept
    {
        sched_yield();
    }
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

    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept
    {
        return osal::detail::create_pthread_mutex(handle,
                                                  {recursive ? osal::mutex_type::recursive : osal::mutex_type::normal});
    }

    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::ok();
        }
        const int status = pthread_mutex_destroy(static_cast<pthread_mutex_t*>(handle->native));
        if (status != 0)
        {
            return osal::detail::pthread_mutex_status(status);
        }
        delete static_cast<pthread_mutex_t*>(handle->native);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t t) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<pthread_mutex_t*>(handle->native);
        if (t == osal::WAIT_FOREVER)
        {
            return osal::detail::pthread_mutex_status(pthread_mutex_lock(m));
        }
        if (t == osal::NO_WAIT)
        {
            return osal::detail::pthread_mutex_status(pthread_mutex_trylock(m));
        }
        const struct timespec abs = ms_to_abs_timespec(t);
        return osal::detail::pthread_mutex_status(pthread_mutex_clocklock(m, CLOCK_MONOTONIC, &abs));
    }

    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* h) noexcept
    {
        return osal_mutex_lock(h, osal::NO_WAIT);
    }

    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        return (pthread_mutex_unlock(static_cast<pthread_mutex_t*>(handle->native)) == 0) ? osal::ok()
                                                                                          : osal::error_code::not_owner;
    }

#include "../common/posix/posix_bounded_semaphore.inl"

    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || initial_count > max_count)
        {
            return osal::error_code::invalid_argument;
        }
        auto* s = new (std::nothrow) posix_bounded_semaphore;
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
        handle->native = s;
        return osal::ok();
    }

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::ok();
        }
        sem_destroy(&static_cast<posix_bounded_semaphore*>(handle->native)->sem);
        delete static_cast<posix_bounded_semaphore*>(handle->native);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        return bounded_sem_post(static_cast<posix_bounded_semaphore*>(handle->native));
    }

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* h) noexcept
    {
        return osal_semaphore_give(h);
    }

    // Uses sem_wait() (forever), sem_trywait() (NO_WAIT), or
    // GNU sem_clockwait(CLOCK_MONOTONIC) (timed).
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle, osal::tick_t t) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<posix_bounded_semaphore*>(handle->native);
        if (t == osal::WAIT_FOREVER)
        {
            int rc{-1};
            while (true)
            {
                rc = bounded_sem_complete(s, sem_wait(&s->sem));
                if ((rc != -1) || (errno != EINTR))
                {
                    break;
                }
            }
            return (rc == 0) ? osal::ok() : osal::error_code::unknown;
        }
        if (t == osal::NO_WAIT)
        {
            return (bounded_sem_complete(s, sem_trywait(&s->sem)) == 0) ? osal::ok() : osal::error_code::would_block;
        }
        const struct timespec abs = ms_to_abs_timespec(t);
        int                   rc{0};
        do
        {
            rc = bounded_sem_complete(s, sem_clockwait(&s->sem, CLOCK_MONOTONIC, &abs));
        } while (rc == -1 && errno == EINTR);
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

    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* h) noexcept
    {
        return osal_semaphore_take(h, osal::NO_WAIT);
    }

    // Internal mutex and condvars use CLOCK_MONOTONIC for timed waits.
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

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item, osal::tick_t t) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* h, const void* i) noexcept
    {
        return osal_queue_send(h, i, osal::NO_WAIT);
    }

    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item, osal::tick_t t) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* h, void* i) noexcept
    {
        return osal_queue_receive(h, i, osal::NO_WAIT);
    }

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item, osal::tick_t) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* h) noexcept
    {
        if (handle_is_null(h)) [[unlikely]]
        {
            return 0U;
        }
        const auto* q = static_cast<const linux_queue_obj*>(h->native);
        pthread_mutex_lock(&q->mutex);
        const std::size_t n = q->count;
        pthread_mutex_unlock(&q->mutex);
        return n;
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* h) noexcept
    {
        if (handle_is_null(h)) [[unlikely]]
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

    // The watcher thread is spawned lazily on the first osal_timer_start() call.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || cb == nullptr || period_ticks == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        auto* ctx = new (std::nothrow) linux_timer_ctx{};
        if (ctx == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        ctx->fn          = cb;
        ctx->arg         = arg;
        ctx->auto_reload = auto_reload;
        ctx->running.store(false, std::memory_order_relaxed);
        ctx->release_from_watcher.store(false, std::memory_order_relaxed);
        ctx->watcher_started = false;

        ctx->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
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

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* const ctx = static_cast<linux_timer_ctx*>(handle->native);
        if (!ctx->running.load(std::memory_order_acquire))
        {
            if (ctx->watcher_started)
            {
                return osal::error_code::not_supported;
            }
            ctx->running.store(true, std::memory_order_release);
            if (pthread_create(&ctx->watcher, nullptr, linux_timer_watcher, ctx) != 0)
            {
                ctx->running.store(false, std::memory_order_release);
                return osal::error_code::out_of_resources;
            }
            ctx->watcher_started = true;
        }
        return (timerfd_settime(ctx->timerfd, 0, &ctx->spec, nullptr) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* const ctx = static_cast<linux_timer_ctx*>(handle->native);
        struct itimerspec dis
        {
        };
        return timerfd_settime(ctx->timerfd, 0, &dis, nullptr) == 0 ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* h) noexcept
    {
        const osal::result stopped = osal_timer_stop(h);
        if (!stopped.ok())
        {
            return stopped;
        }
        return osal_timer_start(h);
    }

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle, osal::tick_t p) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* const ctx = static_cast<linux_timer_ctx*>(handle->native);
        if (p == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        itimerspec current{};
        if (timerfd_gettime(ctx->timerfd, &current) != 0)
        {
            return osal::error_code::unknown;
        }
        const bool active      = current.it_value.tv_sec != 0 || current.it_value.tv_nsec != 0;
        const auto sec         = static_cast<time_t>(p / 1000U);
        const auto nanoseconds = static_cast<std::int64_t>(p % 1000U) * kNanosecondsPerMillisecond;
        const auto nsec        = static_cast<decltype(timespec{}.tv_nsec)>(nanoseconds);
        ctx->spec.it_value     = {sec, nsec};
        ctx->spec.it_interval  = ctx->auto_reload ? timespec{sec, nsec} : timespec{0, 0};
        return !active || timerfd_settime(ctx->timerfd, 0, &ctx->spec, nullptr) == 0 ? osal::ok()
                                                                                     : osal::error_code::unknown;
    }

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::ok();
        }
        auto* const        ctx     = static_cast<linux_timer_ctx*>(handle->native);
        const osal::result stopped = osal_timer_stop(handle);
        if (!stopped.ok())
        {
            return stopped;
        }
        if (ctx->watcher_started)
        {
            ctx->running.store(false, std::memory_order_release);
            // Signal the watcher thread to exit via the shutdown eventfd.
            const std::uint64_t val          = 1U;
            const ssize_t       write_result = write(ctx->shutdown_fd, &val, sizeof(val));
            if (write_result != static_cast<ssize_t>(sizeof(val)) && errno != EAGAIN)
            {
                return osal::error_code::unknown;
            }
            if (ctx->watcher_started && (pthread_equal(pthread_self(), ctx->watcher) != 0))
            {
                ctx->release_from_watcher.store(true, std::memory_order_release);
                handle->native = nullptr;
                return osal::ok();
            }
            if (ctx->watcher_started)
            {
                const int status = pthread_join(ctx->watcher, nullptr);
                if (status != 0)
                {
                    return osal::detail::pthread_mutex_status(status);
                }
            }
        }
        linux_timer_release(ctx);
        handle->native = nullptr;
        return osal::ok();
    }

    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::ok();
        }
        auto* ws = static_cast<linux_wait_set_obj*>(handle->native);
        close(ws->epfd);
        delete ws;
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t* handle, int fd,
                                   std::uint32_t events) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t* handle, int fd) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* ws = static_cast<linux_wait_set_obj*>(handle->native);
        return (epoll_ctl(ws->epfd, EPOLL_CTL_DEL, fd, nullptr) == 0) ? osal::ok() : osal::error_code::invalid_argument;
    }

    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t* handle, int* fds_ready,
                                    std::size_t max_ready, std::size_t* n_ready, osal::tick_t timeout) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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
