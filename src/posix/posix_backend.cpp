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

/// @brief Determine the best clock for condvars.
/// macOS lacks pthread_condattr_setclock, so fall back to CLOCK_REALTIME.
#if defined(__APPLE__) || !defined(_POSIX_MONOTONIC_CLOCK)
constexpr clockid_t kCondClock = CLOCK_REALTIME;
#else
constexpr clockid_t kCondClock = CLOCK_MONOTONIC;
#endif

/// @brief Build an absolute timespec relative to the given clock.
/// @param[in] clk            Clock used as the timespec reference.
/// @param[in] timeout_ticks  Relative OSAL timeout in ticks.
/// @return Absolute deadline expressed in @p clk time.
[[nodiscard]] timespec ms_to_abs_timespec(clockid_t clk, osal::tick_t timeout_ticks) noexcept
{
    return osal::detail::backend_timeout_adapter::to_abs_timespec(clk, timeout_ticks);
}

/// @brief Absolute timespec for sem_timedwait / pthread_mutex_timedlock.
/// POSIX mandates these use CLOCK_REALTIME (no clockid parameter).
/// @param[in] timeout_ticks  Relative OSAL timeout in ticks.
/// @return Absolute CLOCK_REALTIME deadline.
[[nodiscard]] timespec ms_to_abs_timespec_realtime(osal::tick_t timeout_ticks) noexcept
{
    return ms_to_abs_timespec(CLOCK_REALTIME, timeout_ticks);
}

/// @brief Absolute timespec for condvar waits (uses MONOTONIC where available).
/// @param[in] timeout_ticks  Relative OSAL timeout in ticks.
/// @return Absolute deadline expressed in the configured condvar clock.
[[nodiscard]] timespec ms_to_abs_timespec_cond(osal::tick_t timeout_ticks) noexcept
{
    return ms_to_abs_timespec(kCondClock, timeout_ticks);
}

[[nodiscard]] int to_poll_timeout_ms(osal::tick_t timeout_ticks) noexcept
{
    return osal::detail::backend_timeout_adapter::to_poll_timeout_ms(timeout_ticks);
}

/// @brief Init a condvar with CLOCK_MONOTONIC where supported.
/// @param[out] cond  Condvar to initialize.
/// @return Zero on success; otherwise the native pthread error code.
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

/// @brief Run one timer with serialized fixed-delay periodic callbacks.
/// @details For periodic timers, each callback is followed by a full period wait.
///          Callbacks never overlap and missed expirations are not replayed.
/// @param[in] raw  Timer context supplied to pthread_create().
/// @return nullptr when the watcher exits.
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

    /// @brief Return the current CLOCK_MONOTONIC time in nanoseconds.
    std::int64_t osal_clock_high_resolution_ns() noexcept
    {
        timespec ts{};
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        {
            return osal_clock_monotonic_ms() * kNanosecondsPerMillisecond;
        }
        return timespec_to_nanoseconds(ts);
    }

    /// @brief Return the nominal CLOCK_MONOTONIC resolution in nanoseconds.
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

    /// @brief Create a POSIX pthread.
    /// @details Uses SCHED_FIFO when @p priority differs from PRIORITY_NORMAL.
    ///          On Linux, sets CPU affinity via pthread_setaffinity_np() if @p affinity != AFFINITY_ANY.
    /// @param[out] handle      Output handle that stores pthread_t as a void pointer.
    /// @param[in] entry        Thread entry function.
    /// @param[in] arg          Opaque argument forwarded to @p entry.
    /// @param[in] priority     OSAL priority mapped to SCHED_FIFO range.
    /// @param[in] affinity     CPU affinity bitmask (Linux only; ignored elsewhere).
    /// @param[in] stack        Optional caller-provided stack storage.
    /// @param[in] stack_bytes  Size of @p stack in bytes.
    /// @retval osal::ok()                            Thread created successfully.
    /// @retval osal::error_code::out_of_resources    Thread context or pthread creation failed.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        assert((handle != nullptr) && (entry != nullptr));

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
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        if ((stack != nullptr) && (stack_bytes > 0U))
        {
            pthread_attr_setstack(&attr, stack, stack_bytes);
        }

        // Set FIFO scheduling if priority != default.
        if (priority != osal::PRIORITY_NORMAL)
        {
            struct sched_param sp
            {
            };
            const int policy = SCHED_FIFO;
            const int max_p  = sched_get_priority_max(policy);
            const int min_p  = sched_get_priority_min(policy);
            sp.sched_priority =
                min_p +
                static_cast<int>((static_cast<std::uint32_t>(priority) * static_cast<std::uint32_t>(max_p - min_p)) /
                                 static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
            pthread_attr_setschedpolicy(&attr, policy);
            pthread_attr_setschedparam(&attr, &sp);
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        }

        const int rc = pthread_create(thread_id, &attr, posix_thread_entry, ctx);
        pthread_attr_destroy(&attr);

        if (rc != 0)
        {
            delete ctx;
            delete thread_id;
            return osal::error_code::out_of_resources;
        }

#if defined(__linux__)
        if (affinity != osal::AFFINITY_ANY)
        {
            cpu_set_t cpuset{};
            CPU_ZERO(&cpuset);
            for (int cpu = 0; cpu < kPosixMaxAffinityCpus; ++cpu)
            {
                if ((affinity & (1U << static_cast<unsigned>(cpu))) != 0U)
                {
                    CPU_SET(cpu, &cpuset);
                }
            }
            pthread_setaffinity_np(*thread_id, sizeof(cpuset), &cpuset);
        }
#else
        (void)affinity;
#endif

        handle->native = thread_id;
        return osal::ok();
    }

    /// @brief Wait for a thread to exit via pthread_join() or pthread_timedjoin_np() (Linux/POSIX.1).
    /// @param[in,out] handle     Thread handle; cleared after a successful join.
    /// @param[in] timeout_ticks  Maximum wait; WAIT_FOREVER uses blocking pthread_join().
    /// @retval osal::ok()                         Thread joined successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native thread.
    /// @retval osal::error_code::timeout          The finite join timeout expired.
    /// @retval osal::error_code::unknown          The native join operation failed.
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

    /// @brief Detach the pthread via pthread_detach().
    /// @param[in,out] handle  Thread handle; cleared after successful detachment.
    /// @retval osal::ok()                         Thread detached successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native thread.
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

    /// @brief Return a comparable opaque identifier for a thread.
    /// @param[in] handle   Thread handle, or nullptr to identify the calling thread.
    /// @param[out] out_id  Receives the opaque thread identifier.
    /// @retval osal::ok()                         The identifier was written to @p out_id.
    /// @retval osal::error_code::invalid_argument @p out_id is null.
    /// @retval osal::error_code::not_initialized  @p handle has no native thread.
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

    /// @brief Query the current scheduler priority of a thread.
    /// @param[in] handle         Thread handle, or nullptr to query the calling thread.
    /// @param[out] out_priority  Receives the OSAL priority.
    /// @retval osal::ok()                         The priority was written to @p out_priority.
    /// @retval osal::error_code::invalid_argument @p out_priority is null.
    /// @retval osal::error_code::not_initialized  @p handle has no native thread.
    /// @retval osal::error_code::unknown          The native priority query failed.
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

    /// @brief Draft thread stack-watermark query for POSIX.
    /// @details POSIX pthreads do not expose a portable low-watermark metric,
    ///          so the draft API remains unsupported on this backend.
    /// @param[in] handle      Thread handle; ignored by this backend.
    /// @param[out] out_bytes  Receives zero when non-null.
    /// @retval osal::error_code::not_supported  POSIX has no portable stack-watermark query.
    osal::result osal_thread_stack_low_watermark_bytes(const osal::active_traits::thread_handle_t* /*handle*/,
                                                       std::size_t* out_bytes) noexcept
    {
        if (out_bytes != nullptr)
        {
            *out_bytes = 0U;
        }
        return osal::error_code::not_supported;
    }

    /// @brief Returns the accumulated CPU time for a thread in microseconds.
    /// @details Uses @c CLOCK_THREAD_CPUTIME_ID for the current thread and
    ///          @c pthread_getcpuclockid() for an arbitrary joinable thread.
    /// @param[in] handle   Thread handle, or nullptr to query the calling thread.
    /// @param[out] out_us  Receives accumulated CPU time in microseconds.
    /// @retval osal::ok()                         The execution time was written to @p out_us.
    /// @retval osal::error_code::invalid_argument @p out_us is null.
    /// @retval osal::error_code::not_initialized  @p handle has no native thread.
    /// @retval osal::error_code::unknown          The native clock query failed.
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

    /// @brief Change the thread's scheduler priority using its current policy.
    /// @param[in] handle    Thread handle.
    /// @param[in] priority  OSAL priority mapped to the policy's range.
    /// @retval osal::ok()                              Thread priority changed successfully.
    /// @retval osal::error_code::not_initialized       The handle is null or has no native thread.
    /// @retval osal::error_code::permission_denied     The caller lacks scheduling permission.
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

    /// @brief Set CPU affinity via pthread_setaffinity_np() (Linux-only).
    /// @param[in] handle    Thread handle.
    /// @param[in] affinity  CPU bitmask; each set bit represents a processor to allow.
    /// @retval osal::ok()                         Thread affinity changed successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native thread.
    /// @retval osal::error_code::not_supported    The backend does not support thread affinity.
    /// @retval osal::error_code::unknown          The native affinity operation failed.
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
            if ((affinity & (1U << static_cast<unsigned>(cpu))) != 0U)
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

    /// @brief Suspend a thread (not supported on POSIX through this OSAL).
    /// @param[in] handle  Thread handle; ignored by this backend.
    /// @retval osal::error_code::not_supported  Always; POSIX suspension is not exposed by this OSAL.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported on POSIX through this OSAL).
    /// @param[in] handle  Thread handle; ignored by this backend.
    /// @retval osal::error_code::not_supported  Always; POSIX resumption is not exposed by this OSAL.
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
    /// @param[in] ms  Minimum delay in milliseconds.
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

    /// @brief Allocate and initialise a pthread_mutex_t; ERRORCHECK or RECURSIVE type.
    /// @param[out] handle    Output handle that owns a heap-allocated pthread_mutex_t.
    /// @param[in] recursive  True selects PTHREAD_MUTEX_RECURSIVE; false selects ERRORCHECK.
    /// @retval osal::ok()                         Mutex created successfully.
    /// @retval osal::error_code::out_of_resources Native mutex allocation or initialization failed.
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
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    /// @brief Destroy the pthread_mutex_t and free heap storage.
    /// @param[in,out] handle  Mutex handle; cleared after destruction.
    /// @retval osal::ok()  Always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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
    /// @param[in] handle         Mutex handle.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks; WAIT_FOREVER uses pthread_mutex_lock().
    /// @retval osal::ok()                         Mutex acquired successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native mutex.
    /// @retval osal::error_code::would_block      The mutex was unavailable with NO_WAIT.
    /// @retval osal::error_code::timeout          The finite acquisition timeout expired.
    /// @retval osal::error_code::unknown          The native mutex operation failed.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
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
    /// @param[in] handle  Mutex handle.
    /// @retval osal::ok()                         Mutex acquired successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native mutex.
    /// @retval osal::error_code::would_block      The mutex is unavailable.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release the mutex via pthread_mutex_unlock().
    /// @param[in] handle  Mutex handle.
    /// @retval osal::ok()                         Mutex released successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native mutex.
    /// @retval osal::error_code::not_owner        The calling thread does not own the mutex.
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

    /// @brief Allocate and initialise an unnamed POSIX sem_t via sem_init().
    /// @param[out] handle        Output handle that owns a heap-allocated sem_t.
    /// @param[in] initial_count  Starting token count.
    /// @retval osal::ok()                         Semaphore created successfully.
    /// @retval osal::error_code::out_of_resources Native semaphore allocation or initialization failed.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        auto* s = new (std::nothrow) sem_t;
        if (s == nullptr)
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
    /// @param[in,out] handle  Semaphore handle; cleared after destruction.
    /// @retval osal::ok()  Always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::ok();
        }
        sem_destroy(static_cast<sem_t*>(handle->native));
        delete static_cast<sem_t*>(handle->native);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (post) the semaphore via sem_post().
    /// @param[in] handle  Semaphore handle.
    /// @retval osal::ok()                         Semaphore incremented successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native semaphore.
    /// @retval osal::error_code::unknown          The native semaphore operation failed.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        return (sem_post(static_cast<sem_t*>(handle->native)) == 0) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Increment the semaphore from ISR context (sem_post is async-signal-safe on POSIX).
    /// @param[in] handle  Semaphore handle.
    /// @retval osal::ok()                         Semaphore incremented successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native semaphore.
    /// @retval osal::error_code::unknown          The native semaphore operation failed.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // sem_post is async-signal-safe
    }

    /// @brief Decrement (wait on) the semaphore.
    /// @details Uses sem_wait() (forever), sem_trywait() (NO_WAIT), or
    ///          sem_timedwait() with CLOCK_REALTIME (timed).
    /// @param[in] handle         Semaphore handle.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks.
    /// @retval osal::ok()                         Semaphore decremented successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native semaphore.
    /// @retval osal::error_code::would_block      The semaphore has no token with NO_WAIT.
    /// @retval osal::error_code::timeout          The finite wait timeout expired.
    /// @retval osal::error_code::unknown          The native semaphore operation failed.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle_is_null(handle)) [[unlikely]]
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<sem_t*>(handle->native);
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            int rc = 0;
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
    /// @param[in] handle  Semaphore handle.
    /// @retval osal::ok()                         Semaphore decremented successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native semaphore.
    /// @retval osal::error_code::would_block      The semaphore has no token.
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
    /// @param[out] handle  Output handle that owns a heap-allocated posix_queue_obj.
    /// @param[in] buf      Caller-provided mutable storage for @p cap elements.
    /// @param[in] item_sz  Size of each element in bytes.
    /// @param[in] cap      Maximum number of elements the queue can hold.
    /// @retval osal::ok()                         Queue created successfully.
    /// @retval osal::error_code::out_of_resources Queue control-object allocation failed.
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

    /// @brief Destroy the circular-buffer queue: destroy condvars, mutex, and free the object.
    /// @param[in,out] handle  Queue handle; cleared after destruction.
    /// @retval osal::ok()  Always.
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

    /// @brief Send an item into the queue; blocks on the not_full condvar if the queue is full.
    /// @param[in] handle         Queue handle.
    /// @param[in] item           Item to copy into the queue.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks; NO_WAIT returns immediately if full.
    /// @retval osal::ok()                         Item queued successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native queue.
    /// @retval osal::error_code::would_block      The queue is full with NO_WAIT.
    /// @retval osal::error_code::timeout          The finite send timeout expired.
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

    /// @brief Send an item non-blocking (ISR-compatible).
    /// @param[in] handle  Queue handle.
    /// @param[in] item    Item to copy into the queue.
    /// @retval osal::ok()                         Item queued successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native queue.
    /// @retval osal::error_code::would_block      The queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive an item from the queue; blocks on the not_empty condvar if the queue is empty.
    /// @param[in] handle         Queue handle.
    /// @param[out] item          Receives the dequeued item.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks; NO_WAIT returns immediately if empty.
    /// @retval osal::ok()                         Item dequeued successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native queue.
    /// @retval osal::error_code::would_block      The queue is empty with NO_WAIT.
    /// @retval osal::error_code::timeout          The finite receive timeout expired.
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

    /// @brief Receive an item non-blocking (ISR-compatible).
    /// @param[in] handle  Queue handle.
    /// @param[out] item   Receives the dequeued item.
    /// @retval osal::ok()                         Item dequeued successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native queue.
    /// @retval osal::error_code::would_block      The queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Copy the front item without removing it (timeout parameter ignored).
    /// @param[in] handle  Queue handle.
    /// @param[out] item   Receives a copy of the head item.
    /// @retval osal::ok()                         Head item copied successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native queue.
    /// @retval osal::error_code::would_block      The queue is empty.
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

    /// @brief Return the number of items currently in the queue.
    /// @param[in] handle  Queue handle.
    /// @return Mutex-protected snapshot of the item count; 0 if the handle is invalid.
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

    /// @brief Return the number of free slots remaining in the queue.
    /// @param[in] handle  Queue handle.
    /// @return Mutex-protected snapshot of free slots; 0 if the handle is invalid.
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

    /// @brief Create a POSIX timer with a lazily-started joinable watcher thread.
    /// @param[out] handle       Output handle that owns a heap-allocated posix_timer_ctx.
    /// @param[in] cb            Function called on each expiry.
    /// @param[in] arg           Opaque argument forwarded to @p cb.
    /// @param[in] period_ticks  Expiry period in OSAL ticks (milliseconds).
    /// @param[in] auto_reload   True to fire repeatedly; false for one-shot.
    /// @retval osal::ok()                         Timer created successfully.
    /// @retval osal::error_code::out_of_resources Timer control-object allocation or initialization failed.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert((handle != nullptr) && (cb != nullptr));
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

    /// @brief Stop the watcher and release its context after any active callback completes.
    /// @param[in,out] handle  Timer handle; cleared after destruction.
    /// @retval osal::ok()  Always.
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

    /// @brief Arm the timer watcher.
    /// @param[in] handle  Timer handle.
    /// @retval osal::ok()                         Timer armed successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native timer.
    /// @retval osal::error_code::out_of_resources The watcher thread could not be created.
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

    /// @brief Disarm the timer watcher.
    /// @param[in] handle  Timer handle.
    /// @retval osal::ok()                         Timer disarmed successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native timer.
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

    /// @brief Restart the timer from its full period.
    /// @param[in] handle  Timer handle.
    /// @retval osal::ok()                         Timer reset successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native timer.
    /// @retval osal::error_code::out_of_resources The watcher thread could not be created.
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

    /// @brief Update the timer period and immediately arm it.
    /// @param[in] handle  Timer handle.
    /// @param[in] p       New period in OSAL ticks (milliseconds).
    /// @retval osal::ok()                         Period updated and timer armed successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native timer.
    /// @retval osal::error_code::out_of_resources The watcher thread could not be created.
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
        ctx->period_ticks         = p;
        const osal::result result = posix_timer_arm_locked(ctx);
        pthread_mutex_unlock(&ctx->mutex);
        return result;
    }

    /// @brief Query whether the timer watcher is currently armed.
    /// @param[in] handle  Timer handle.
    /// @return True if the watcher is waiting for expiry.
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

    /// @brief Allocate a posix_wait_set_obj backed by up to OSAL_POSIX_MAX_POLL_FDS entries.
    /// @param[out] handle  Output handle that owns a heap-allocated posix_wait_set_obj.
    /// @retval osal::ok()                         Wait-set created successfully.
    /// @retval osal::error_code::out_of_resources Wait-set allocation failed.
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

    /// @brief Free the posix_wait_set_obj and clear the handle.
    /// @param[in,out] handle  Wait-set handle; cleared after destruction.
    /// @retval osal::ok()  Always.
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

    /// @brief Register a file descriptor and its poll event mask in the wait-set.
    /// @param[in] handle  Wait-set handle.
    /// @param[in] fd      File descriptor to monitor.
    /// @param[in] events  poll() event mask (POLLIN, POLLOUT, etc.).
    /// @retval osal::ok()                         File descriptor registered successfully.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native wait-set.
    /// @retval osal::error_code::out_of_resources The wait-set has reached its descriptor limit.
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

    /// @brief Remove a file descriptor from the wait-set by swapping with the last entry.
    /// @param[in] handle  Wait-set handle.
    /// @param[in] fd      File descriptor to remove.
    /// @retval osal::ok()                            File descriptor removed successfully.
    /// @retval osal::error_code::not_initialized     The handle is null or has no native wait-set.
    /// @retval osal::error_code::invalid_argument    The file descriptor is not registered.
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

    /// @brief Block on poll() until at least one file descriptor has an event or the timeout expires.
    /// @param[in] handle         Wait-set handle.
    /// @param[out] fds_ready     Output array for ready file descriptors, up to @p max_ready entries.
    /// @param[in] max_ready      Maximum number of ready file descriptors to report.
    /// @param[out] n_ready       Receives the number of file descriptors written to @p fds_ready.
    /// @param[in] timeout_ticks  Maximum wait in OSAL ticks; WAIT_FOREVER = -1, NO_WAIT = 0.
    /// @retval osal::ok()                         One or more file descriptors are ready.
    /// @retval osal::error_code::not_initialized  The handle is null or has no native wait-set.
    /// @retval osal::error_code::timeout          The wait timeout expired.
    /// @retval osal::error_code::unknown          The native poll operation failed.
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
