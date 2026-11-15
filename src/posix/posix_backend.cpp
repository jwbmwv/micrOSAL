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
///          - timer    → joinable pthread watcher
///          - event_flags → pthread_mutex + pthread_cond_broadcast + uint32_t
///          - wait_set → poll() on file descriptors
///
///          @note Dynamic allocation (malloc): used for timer watcher context
///          objects only.  All other objects are caller-supplied or pooled.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#if !defined(OSAL_POSIXLIKE_BACKEND_SELECTED) && !defined(OSAL_BACKEND_POSIX)
#define OSAL_BACKEND_POSIX
#endif
#include <osal/osal.hpp>
#include "../common/backend_timeout_adapter.hpp"
#include "../common/posix/posix_mutex_policy.hpp"
#include "../common/posix/posix_thread_attributes.hpp"

#include <pthread.h>
#include <semaphore.h>
#include <ctime>
#include <cerrno>
#include <poll.h>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <atomic>
#include <algorithm>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

template<typename Handle>
[[nodiscard]] constexpr bool handle_is_null(const Handle* handle) noexcept
{
    return (handle == nullptr) || (handle->native == nullptr);
}

constexpr int          kPosixMaxAffinityCpus      = 64;
constexpr std::size_t  kPosixMaxPollFds           = 16U;
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

// Determine the best clock for condvars.
/// macOS lacks pthread_condattr_setclock, so fall back to CLOCK_REALTIME.
#if defined(__APPLE__) || !defined(_POSIX_MONOTONIC_CLOCK)
constexpr clockid_t kCondClock = CLOCK_REALTIME;
#else
constexpr clockid_t kCondClock = CLOCK_MONOTONIC;
#endif

[[nodiscard]] timespec ms_to_abs_timespec(clockid_t clk, osal::tick_t timeout_ticks) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(clk, timeout_ticks);
}

[[nodiscard]] timespec ms_to_abs_timespec_realtime(osal::tick_t timeout_ticks) noexcept
{
    return ms_to_abs_timespec(CLOCK_REALTIME, timeout_ticks);
}

[[nodiscard]] timespec ms_to_abs_timespec_cond(osal::tick_t timeout_ticks) noexcept
{
    return ms_to_abs_timespec(kCondClock, timeout_ticks);
}

[[nodiscard]] int to_poll_timeout_ms(osal::tick_t timeout_ticks) noexcept
{
    return osal::detail::backend_timeout_adapter::to_poll_timeout_ms(timeout_ticks);
}

int cond_init_monotonic(pthread_cond_t* cond) noexcept
{
#if defined(__APPLE__) || !defined(_POSIX_MONOTONIC_CLOCK)
    return pthread_cond_init(cond, nullptr);
#else
    pthread_condattr_t attr{};
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    const int rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
#endif
}

struct posix_thread_ctx
{
    void (*entry)(void*);
    void* arg;
};

void* posix_thread_entry(void* ctx_raw) noexcept
{
    auto* const ctx = static_cast<posix_thread_ctx*>(ctx_raw);
    if (ctx == nullptr)
    {
        return nullptr;
    }

    ctx->entry(ctx->arg);
    delete ctx;
    return nullptr;
}

struct posix_timer_ctx
{
    osal_timer_callback_t   fn;
    void*                   arg;
    pthread_t               watcher;
    mutable pthread_mutex_t mutex;
    pthread_cond_t          changed;
    osal::tick_t            period_ticks;
    bool                    auto_reload;
    bool                    active{false};
    bool                    shutdown{false};
    bool                    watcher_started{false};
    bool                    release_from_watcher{false};
};

void posix_timer_release(posix_timer_ctx* ctx) noexcept
{
    pthread_cond_destroy(&ctx->changed);
    pthread_mutex_destroy(&ctx->mutex);
    delete ctx;
}

// For periodic timers, each callback is followed by a full period wait.
// Callbacks never overlap and missed expirations are not replayed.
void* posix_timer_watcher(void* raw) noexcept
{
    auto* const ctx = static_cast<posix_timer_ctx*>(raw);
    if (ctx == nullptr)
    {
        return nullptr;
    }

    pthread_mutex_lock(&ctx->mutex);
    for (;;)
    {
        while (!ctx->shutdown && !ctx->active)
        {
            pthread_cond_wait(&ctx->changed, &ctx->mutex);
        }
        if (ctx->shutdown)
        {
            break;
        }

        const timespec deadline    = ms_to_abs_timespec_cond(ctx->period_ticks);
        const int      wait_result = pthread_cond_timedwait(&ctx->changed, &ctx->mutex, &deadline);
        if (ctx->shutdown || (wait_result != ETIMEDOUT) || !ctx->active)
        {
            continue;
        }

        if (!ctx->auto_reload)
        {
            ctx->active = false;
        }
        const auto  callback     = ctx->fn;
        void* const callback_arg = ctx->arg;

        pthread_mutex_unlock(&ctx->mutex);
        callback(callback_arg);
        pthread_mutex_lock(&ctx->mutex);
    }

    const bool release_from_watcher = ctx->release_from_watcher;
    pthread_mutex_unlock(&ctx->mutex);
    if (release_from_watcher)
    {
        (void)pthread_detach(pthread_self());
        posix_timer_release(ctx);
    }
    return nullptr;
}

osal::result posix_timer_arm_locked(posix_timer_ctx* ctx) noexcept
{
    if (ctx->shutdown)
    {
        return osal::error_code::not_initialized;
    }
    if (ctx->period_ticks == osal::NO_WAIT)
    {
        ctx->active = false;
        pthread_cond_broadcast(&ctx->changed);
        return osal::ok();
    }
    if (!ctx->watcher_started)
    {
        if (pthread_create(&ctx->watcher, nullptr, posix_timer_watcher, ctx) != 0)
        {
            return osal::error_code::out_of_resources;
        }
        ctx->watcher_started = true;
    }
    ctx->active = true;
    pthread_cond_broadcast(&ctx->changed);
    return osal::ok();
}

}  // namespace

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

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(osal_clock_monotonic_ms());
    }

    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1'000U;
    }  // 1 ms per tick

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
    // Thread
    // ---------------------------------------------------------------------------

    // Uses SCHED_FIFO when priority differs from PRIORITY_NORMAL.
    // On Linux, sets CPU affinity via pthread_setaffinity_np() if affinity != AFFINITY_ANY.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        if (handle == nullptr || entry == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }

        auto* ctx = new (std::nothrow) posix_thread_ctx{entry, arg};
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
            rc = pthread_create(thread_id, &attr, posix_thread_entry, ctx);
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

        handle->native = thread_id;
        return osal::ok();
    }

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* const thread_id = static_cast<pthread_t*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            const int rc = pthread_join(*thread_id, nullptr);
            if (rc != 0)
            {
                return osal::error_code::unknown;
            }
            delete thread_id;
            handle->native = nullptr;
            return osal::ok();
        }

#if defined(_POSIX_TIMEOUTS) && (_POSIX_TIMEOUTS >= 200112L)
        const struct timespec abs = ms_to_abs_timespec_realtime(timeout_ticks);
        const int             rc  = pthread_timedjoin_np(*thread_id, nullptr, &abs);  // Linux extension
        if (rc == ETIMEDOUT)
        {
            return osal::error_code::timeout;
        }
        if (rc != 0)
        {
            return osal::error_code::unknown;
        }
        delete thread_id;
        handle->native = nullptr;
        return osal::ok();
#else
        const int rc = pthread_join(*thread_id, nullptr);
        (void)timeout_ticks;
        if (rc != 0)
        {
            return osal::error_code::unknown;
        }
        delete thread_id;
        handle->native = nullptr;
        return osal::ok();
#endif
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

    // POSIX pthreads do not expose a portable low-watermark metric,
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
        const int max_p = sched_get_priority_max(policy);
        const int min_p = sched_get_priority_min(policy);
        sp.sched_priority =
            min_p +
            static_cast<int>((static_cast<std::uint32_t>(priority) * static_cast<std::uint32_t>(max_p - min_p)) /
                             static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
        const int rc = pthread_setschedparam(*thread_id, policy, &sp);
        return (rc == 0) ? osal::ok() : osal::error_code::permission_denied;
    }

    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t                      affinity) noexcept
    {
#if defined(__linux__)
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* const thread_id = static_cast<pthread_t*>(handle->native);
        cpu_set_t   cpuset{};
        CPU_ZERO(&cpuset);
        for (int cpu = 0; cpu < kPosixMaxAffinityCpus; ++cpu)
        {
            if (affinity == osal::AFFINITY_ANY || (cpu < 32 && (affinity & (1U << static_cast<unsigned>(cpu))) != 0U))
            {
                CPU_SET(cpu, &cpuset);
            }
        }
        const int rc = pthread_setaffinity_np(*thread_id, sizeof(cpuset), &cpuset);
        return (rc == 0) ? osal::ok() : osal::error_code::unknown;
#else
        (void)handle;
        (void)affinity;
        return osal::error_code::not_supported;
#endif
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
    // Mutex
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
        auto*     m      = static_cast<pthread_mutex_t*>(handle->native);
        const int status = pthread_mutex_destroy(m);
        if (status != 0)
        {
            return osal::detail::pthread_mutex_status(status);
        }
        delete m;
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<pthread_mutex_t*>(handle->native);
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            return osal::detail::pthread_mutex_status(pthread_mutex_lock(m));
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int rc = pthread_mutex_trylock(m);
            return osal::detail::pthread_mutex_status(rc);
        }
        const struct timespec abs = ms_to_abs_timespec_realtime(timeout_ticks);
        return osal::detail::pthread_mutex_status(pthread_mutex_timedlock(m, &abs));
    }

    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        const int rc = pthread_mutex_unlock(static_cast<pthread_mutex_t*>(handle->native));
        return (rc == 0) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore (unnamed POSIX sem_t)
    // ---------------------------------------------------------------------------

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
        handle->native = static_cast<void*>(s);
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

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // sem_post is async-signal-safe
    }

    // Uses sem_wait() (forever), sem_trywait() (NO_WAIT), or
    // sem_timedwait() with CLOCK_REALTIME (timed).
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<posix_bounded_semaphore*>(handle->native);
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            int rc = 0;
            do
            {
                rc = bounded_sem_complete(s, sem_wait(&s->sem));
            } while (rc == -1 && errno == EINTR);
            return (rc == 0) ? osal::ok() : osal::error_code::unknown;
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int rc = bounded_sem_complete(s, sem_trywait(&s->sem));
            return (rc == 0) ? osal::ok() : osal::error_code::would_block;
        }
        const struct timespec abs = ms_to_abs_timespec_realtime(timeout_ticks);
        int                   rc{0};
        do
        {
            rc = bounded_sem_complete(s, sem_timedwait(&s->sem, &abs));
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

    // Internal mutex and condvars use the best available monotonic clock.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buf, std::size_t item_sz,
                                   std::size_t cap) noexcept
    {
        assert((buf != nullptr) && (item_sz > 0U) && (cap > 0U));
        auto* q = new (std::nothrow) posix_queue_obj{};
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
        q->head = q->tail = q->count = 0U;
        handle->native               = static_cast<void*>(q);
        return osal::ok();
    }

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t /*timeout_ticks*/) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return 0U;
        }
        auto* const q = static_cast<posix_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        const std::size_t count = q->count;
        pthread_mutex_unlock(&q->mutex);
        return count;
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return 0U;
        }
        auto* const q = static_cast<posix_queue_obj*>(handle->native);
        pthread_mutex_lock(&q->mutex);
        const std::size_t free_slots = q->capacity - q->count;
        pthread_mutex_unlock(&q->mutex);
        return free_slots;
    }

    // ---------------------------------------------------------------------------
    // Timer (pthread watcher)
    // ---------------------------------------------------------------------------

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
        auto* ctx = new (std::nothrow) posix_timer_ctx{};
        if (ctx == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        ctx->fn           = cb;
        ctx->arg          = arg;
        ctx->period_ticks = period_ticks;
        ctx->auto_reload  = auto_reload;

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
        handle->native = static_cast<void*>(ctx);
        return osal::ok();
    }

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::ok();
        }
        auto* ctx = static_cast<posix_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        ctx->active   = false;
        ctx->shutdown = true;
        pthread_cond_broadcast(&ctx->changed);
        if (!ctx->watcher_started)
        {
            pthread_mutex_unlock(&ctx->mutex);
            posix_timer_release(ctx);
            handle->native = nullptr;
            return osal::ok();
        }
        const bool destroy_from_watcher = pthread_equal(pthread_self(), ctx->watcher) != 0;
        if (destroy_from_watcher)
        {
            ctx->release_from_watcher = true;
            pthread_mutex_unlock(&ctx->mutex);
            handle->native = nullptr;
            return osal::ok();
        }
        pthread_mutex_unlock(&ctx->mutex);
        (void)pthread_join(ctx->watcher, nullptr);
        posix_timer_release(ctx);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<posix_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        const osal::result result = posix_timer_arm_locked(ctx);
        pthread_mutex_unlock(&ctx->mutex);
        return result;
    }

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<posix_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        if (ctx->shutdown)
        {
            pthread_mutex_unlock(&ctx->mutex);
            return osal::error_code::not_initialized;
        }
        ctx->active = false;
        pthread_cond_broadcast(&ctx->changed);
        pthread_mutex_unlock(&ctx->mutex);
        return osal::ok();
    }

    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<posix_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        const osal::result result = posix_timer_arm_locked(ctx);
        pthread_mutex_unlock(&ctx->mutex);
        return result;
    }

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle, osal::tick_t p) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* ctx = static_cast<posix_timer_ctx*>(handle->native);
        pthread_mutex_lock(&ctx->mutex);
        if (ctx->shutdown)
        {
            pthread_mutex_unlock(&ctx->mutex);
            return osal::error_code::not_initialized;
        }
        if (p == 0U)
        {
            pthread_mutex_unlock(&ctx->mutex);
            return osal::error_code::invalid_argument;
        }
        ctx->period_ticks         = p;
        const osal::result result = ctx->active ? posix_timer_arm_locked(ctx) : osal::ok();
        pthread_mutex_unlock(&ctx->mutex);
        return result;
    }

    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return false;
        }
        const auto* ctx = static_cast<const posix_timer_ctx*>(handle->native);
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
    // Wait-set (poll())
    // ---------------------------------------------------------------------------
    struct posix_wait_set_obj
    {
        struct pollfd fds[kPosixMaxPollFds];
        std::size_t   n_fds;
    };

    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        auto* ws = new (std::nothrow) posix_wait_set_obj{};
        if (ws == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        ws->n_fds      = 0U;
        handle->native = static_cast<void*>(ws);
        return osal::ok();
    }

    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::ok();
        }
        delete static_cast<posix_wait_set_obj*>(handle->native);
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
        auto* ws = static_cast<posix_wait_set_obj*>(handle->native);
        if (ws->n_fds >= kPosixMaxPollFds)
        {
            return osal::error_code::out_of_resources;
        }
        ws->fds[ws->n_fds] = {fd, static_cast<decltype(pollfd{}.events)>(events), 0};
        ws->n_fds++;
        return osal::ok();
    }

    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t* handle, int fd) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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

    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t* handle, int* fds_ready,
                                    std::size_t max_ready, std::size_t* n_ready, osal::tick_t timeout_ticks) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* ws = static_cast<posix_wait_set_obj*>(handle->native);
        if (n_ready != nullptr)
        {
            *n_ready = 0U;
        }

        const int to_ms = to_poll_timeout_ms(timeout_ticks);

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
                if (fds_ready != nullptr)
                {
                    fds_ready[out] = ws->fds[i].fd;
                }
                out++;
            }
        }
        if (n_ready != nullptr)
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
