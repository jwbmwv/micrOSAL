// SPDX-License-Identifier: Apache-2.0
/// @file zephyr_backend.cpp
/// @brief Zephyr RTOS implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the corresponding Zephyr kernel API.
///
///          Minimum required version: Zephyr 3.x+
///          All kernel objects used (k_mutex, k_sem, k_msgq, k_timer,
///          k_event, k_condvar, k_mem_slab) are part of the stable Zephyr
///          kernel API available since 3.0.  The CI tests run against
///          Zephyr 3.7 LTS and 4.5, but no 4.x-only APIs are required.
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
#include <osal/detail/atomic_compat.hpp>

#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <zephyr/sys/util.h>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <limits>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

#if defined(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) && (CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER == 1)
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

[[nodiscard]] std::int64_t zephyr_cycles_to_nanoseconds(std::uint64_t cycles) noexcept
{
    const std::uint64_t frequency_hz = static_cast<std::uint64_t>(sys_clock_hw_cycles_per_sec());
    if (frequency_hz == 0U)
    {
        return 0;
    }

    const std::uint64_t whole_seconds = cycles / frequency_hz;
    const std::uint64_t remainder     = cycles % frequency_hz;
    if (whole_seconds > (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / kNanosecondsPerSecond))
    {
        return std::numeric_limits<std::int64_t>::max();
    }

    const std::uint64_t nanoseconds =
        (whole_seconds * kNanosecondsPerSecond) + ((remainder * kNanosecondsPerSecond) / frequency_hz);
    if (nanoseconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(nanoseconds);
}

[[nodiscard]] std::int64_t zephyr_cycle_resolution_ns() noexcept
{
    const std::uint64_t frequency_hz = static_cast<std::uint64_t>(sys_clock_hw_cycles_per_sec());
    if (frequency_hz == 0U)
    {
        return 0;
    }
    return static_cast<std::int64_t>((kNanosecondsPerSecond + frequency_hz - 1U) / frequency_hz);
}
#endif

[[nodiscard]] osal::priority_t zephyr_to_osal_priority(int priority) noexcept
{
    const int zmax = CONFIG_NUM_PREEMPT_PRIORITIES - 1;
    if (zmax <= 0)
    {
        return osal::PRIORITY_NORMAL;
    }

    if (priority < 0)
    {
        priority = 0;
    }
    else if (priority > zmax)
    {
        priority = zmax;
    }

    return static_cast<osal::priority_t>(((zmax - priority) * osal::PRIORITY_HIGHEST) / zmax);
}

// Zephyr exposes the live CPU mask through k_thread internals when SMP masks are enabled.
#if defined(CONFIG_SCHED_CPU_MASK)
[[nodiscard]] osal::affinity_t zephyr_thread_affinity_mask(k_tid_t tid) noexcept
{
    using cpu_mask_t          = decltype(tid->base.cpu_mask);
    const cpu_mask_t cpu_mask = tid->base.cpu_mask;
    if (cpu_mask == std::numeric_limits<cpu_mask_t>::max())
    {
        return osal::AFFINITY_ANY;
    }
    return static_cast<osal::affinity_t>(cpu_mask);
}
#endif

constexpr int osal_to_zephyr_priority(osal::priority_t p) noexcept
{
    // Map [0,255] -> [CONFIG_NUM_PREEMPT_PRIORITIES-1, 0]
    const int zmax = CONFIG_NUM_PREEMPT_PRIORITIES - 1;  // NOLINT(cppcoreguidelines-init-variables)
    return zmax - static_cast<int>((static_cast<std::uint32_t>(p) * static_cast<std::uint32_t>(zmax)) /
                                   static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
}

k_timeout_t to_zephyr_timeout(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return K_FOREVER;
    }
    if (t == osal::NO_WAIT)
    {
        return K_NO_WAIT;
    }
    const auto ticks   = static_cast<std::uint64_t>(t);
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<k_ticks_t>::max());
    return K_TICKS(static_cast<k_ticks_t>(ticks > maximum ? maximum : ticks));
}

}  // namespace

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

namespace
{

struct k_mutex    zephyr_mutexes[OSAL_ZEPHYR_MAX_MUTEXES];
bool              zephyr_mutex_used[OSAL_ZEPHYR_MAX_MUTEXES];
struct k_sem      zephyr_sems[OSAL_ZEPHYR_MAX_SEMS];
bool              zephyr_sem_used[OSAL_ZEPHYR_MAX_SEMS];
struct k_msgq     zephyr_queues[OSAL_ZEPHYR_MAX_QUEUES];
bool              zephyr_queue_used[OSAL_ZEPHYR_MAX_QUEUES];
struct k_timer    zephyr_timers[OSAL_ZEPHYR_MAX_TIMERS];
bool              zephyr_timer_used[OSAL_ZEPHYR_MAX_TIMERS];
struct k_thread   zephyr_threads[OSAL_ZEPHYR_MAX_THREADS];
bool              zephyr_thread_used[OSAL_ZEPHYR_MAX_THREADS];
struct k_spinlock zephyr_pool_lock;

template<typename T, std::size_t N>
T* acquire_from_pool(T (&pool)[N], bool (&used)[N]) noexcept
{
    const k_spinlock_key_t key = k_spin_lock(&zephyr_pool_lock);
    for (std::size_t i = 0U; i < N; ++i)
    {
        if (!used[i])
        {
            used[i] = true;
            k_spin_unlock(&zephyr_pool_lock, key);
            return &pool[i];
        }
    }
    k_spin_unlock(&zephyr_pool_lock, key);
    return nullptr;
}

template<typename T, std::size_t N>
void release_to_pool(T (&pool)[N], bool (&used)[N], T* p) noexcept
{
    const k_spinlock_key_t key = k_spin_lock(&zephyr_pool_lock);
    for (std::size_t i = 0U; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            used[i] = false;
            k_spin_unlock(&zephyr_pool_lock, key);
            return;
        }
    }
    k_spin_unlock(&zephyr_pool_lock, key);
}

}  // namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(k_uptime_get());
    }

    // Zephyr 3.x does not provide a separate wall-clock API;
    // k_uptime_get() (monotonic uptime) is therefore returned.
    std::int64_t osal_clock_system_ms() noexcept
    {
        // Zephyr 3.x+: no separate wall-clock API; uptime is the best option.
        return static_cast<std::int64_t>(k_uptime_get());
    }

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(k_uptime_ticks());
    }

    // Computed as 1000000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / CONFIG_SYS_CLOCK_TICKS_PER_SEC);
    }

    std::int64_t osal_clock_high_resolution_ns() noexcept
    {
#if defined(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) && (CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER == 1)
        const std::int64_t ns = zephyr_cycles_to_nanoseconds(k_cycle_get_64());
        if (ns > 0)
        {
            return ns;
        }
#endif
        return osal_clock_monotonic_ms() * 1'000'000LL;
    }

    std::int64_t osal_clock_high_resolution_resolution_ns() noexcept
    {
#if defined(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) && (CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER == 1)
        const std::int64_t resolution_ns = zephyr_cycle_resolution_ns();
        return (resolution_ns > 0) ? resolution_ns : 1'000'000LL;
#else
        return static_cast<std::int64_t>(osal_clock_tick_period_us()) * 1'000LL;
#endif
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    // Allocates a k_thread object from the static pool, calls
    // k_thread_create(), optionally names it via k_thread_name_set(),
    // and applies CPU-mask affinity when CONFIG_SCHED_CPU_MASK is enabled.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t affinity, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        if (handle == nullptr || entry == nullptr || stack == nullptr || stack_bytes == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
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

    // Calls k_thread_join() with the converted timeout.  On success
    // the pool slot is released and the handle cleared.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_thread_join(ZK_THREAD(handle),
                                     to_zephyr_timeout(timeout_ticks));  // NOLINT(cppcoreguidelines-init-variables)
        if (rc == 0)
        {
            release_to_pool(zephyr_threads, zephyr_thread_used, ZK_THREAD(handle));
            handle->native = nullptr;
            return osal::ok();
        }
        return (rc == -EAGAIN) ? osal::error_code::timeout : osal::error_code::unknown;
    }

    // Zephyr has no native detach/reclaim operation for caller-owned TCBs.
    // Keep the handle joinable rather than reusing a live pool slot.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return osal::error_code::not_supported;
    }

    // Calls k_thread_priority_set() after mapping the OSAL priority
    // to the Zephyr preemptive priority range.
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

    // Uses k_thread_cpu_mask_clear() / k_thread_cpu_mask_enable()
    // when CONFIG_SCHED_CPU_MASK is enabled; returns
    // error_code::not_supported otherwise.
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

    // Calls k_thread_suspend().
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_thread_suspend(ZK_THREAD(handle));
        return osal::ok();
    }

    // Calls k_thread_resume().
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_thread_resume(ZK_THREAD(handle));
        return osal::ok();
    }

    osal::result osal_thread_get_id(const osal::active_traits::thread_handle_t* handle,
                                    osal::thread_id_t*                          out_id) noexcept
    {
        if (out_id == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        k_tid_t tid = k_current_get();
        if (handle != nullptr)
        {
            if (handle->native == nullptr)
            {
                return osal::error_code::not_initialized;
            }
            tid = ZK_THREAD(handle);
        }

        *out_id = reinterpret_cast<osal::thread_id_t>(tid);
        return osal::ok();
    }

    osal::result osal_thread_get_priority(const osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t*                           out_priority) noexcept
    {
        if (out_priority == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        k_tid_t tid = k_current_get();
        if (handle != nullptr)
        {
            if (handle->native == nullptr)
            {
                return osal::error_code::not_initialized;
            }
            tid = ZK_THREAD(handle);
        }

        *out_priority = zephyr_to_osal_priority(k_thread_priority_get(tid));
        return osal::ok();
    }

    osal::result osal_thread_get_affinity(const osal::active_traits::thread_handle_t* handle,
                                          osal::affinity_t*                           out_affinity) noexcept
    {
#if defined(CONFIG_SCHED_CPU_MASK)
        if (out_affinity == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        k_tid_t tid = k_current_get();
        if (handle != nullptr)
        {
            if (handle->native == nullptr)
            {
                return osal::error_code::not_initialized;
            }
            tid = ZK_THREAD(handle);
        }

        *out_affinity = zephyr_thread_affinity_mask(tid);
        return osal::ok();
#else
        (void)handle;
        (void)out_affinity;
        return osal::error_code::not_supported;
#endif
    }

    osal::result osal_thread_current_cpu(std::uint32_t* out_cpu) noexcept
    {
        if (out_cpu == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

#if defined(CONFIG_MP_MAX_NUM_CPUS) && (CONFIG_MP_MAX_NUM_CPUS > 1)
        return osal::error_code::not_supported;
#else
        *out_cpu = 0U;
        return osal::ok();
#endif
    }

    // Uses k_thread_stack_space_get() for the target thread, or
    // the current thread when handle is null.
    osal::result osal_thread_stack_low_watermark_bytes(const osal::active_traits::thread_handle_t* handle,
                                                       std::size_t*                                out_bytes) noexcept
    {
#if defined(CONFIG_THREAD_STACK_INFO) && (CONFIG_THREAD_STACK_INFO == 1) && defined(CONFIG_INIT_STACKS) && \
    (CONFIG_INIT_STACKS == 1)
        if (out_bytes == nullptr)
        {
            return osal::error_code::invalid_argument;
        }

        k_tid_t tid = k_current_get();
        if (handle != nullptr)
        {
            if (handle->native == nullptr)
            {
                return osal::error_code::not_initialized;
            }
            tid = ZK_THREAD(handle);
        }

        std::size_t unused = 0U;
        const int   rc     = k_thread_stack_space_get(tid, &unused);
        if (rc == 0)
        {
            *out_bytes = unused;
            return osal::ok();
        }
        if (rc == -ENOTSUP)
        {
            return osal::error_code::not_supported;
        }
        return osal::error_code::unknown;
#else
        (void)handle;
        if (out_bytes != nullptr)
        {
            *out_bytes = 0U;
        }
        return osal::error_code::not_supported;
#endif
    }

    // Runtime-stat collection is not enabled in this first Zephyr slice.
    osal::result osal_thread_execution_time_us(const osal::active_traits::thread_handle_t* /*handle*/,
                                               std::int64_t* out_us) noexcept
    {
        if (out_us != nullptr)
        {
            *out_us = 0;
        }
        return osal::error_code::not_supported;
    }

    // Calls k_yield().
    void osal_thread_yield() noexcept
    {
        k_yield();
    }

    // Calls k_sleep(K_MSEC(ms)).
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        k_sleep(K_MSEC(static_cast<int64_t>(ms)));
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    // Allocates a k_mutex from the static pool and calls k_mutex_init().
    // Zephyr's k_mutex is inherently recursive; the recursive flag is accepted
    // but has no behavioral effect.
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

    // Calls k_mutex_lock() using the converted k_timeout_t.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_mutex_lock(ZK_MUTEX(handle),
                                    to_zephyr_timeout(timeout_ticks));  // NOLINT(cppcoreguidelines-init-variables)
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

    // Delegates to osal_mutex_lock() with NO_WAIT.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    // Calls k_mutex_unlock().
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

    // Allocates a k_sem from the static pool and calls k_sem_init().
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

    // Calls k_sem_give().
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_sem_give(ZK_SEM(handle));
        return osal::ok();
    }

    // Delegates to osal_semaphore_give() — k_sem_give() is ISR-safe on Zephyr.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        // On Zephyr k_sem_give is ISR-safe.
        return osal_semaphore_give(handle);
    }

    // Calls k_sem_take() using the converted k_timeout_t.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc =
            k_sem_take(ZK_SEM(handle), to_zephyr_timeout(timeout_ticks));  // NOLINT(cppcoreguidelines-init-variables)
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    // Delegates to osal_semaphore_take() with NO_WAIT.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (k_msgq)
    // ---------------------------------------------------------------------------

    // Allocates a k_msgq from the static pool and calls k_msgq_init()
    // with the caller-supplied buffer.
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

    // Calls k_msgq_purge() to drain the queue, then releases the pool slot.
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

    // Calls k_msgq_put() using the converted k_timeout_t.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_msgq_put(ZK_MSGQ(handle), const_cast<void*>(item),
                                  to_zephyr_timeout(timeout_ticks));  // NOLINT(cppcoreguidelines-init-variables)
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    // Delegates to osal_queue_send() with NO_WAIT.
    // k_msgq_put() is ISR-safe on Zephyr.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    // Calls k_msgq_get() using the converted k_timeout_t.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int rc = k_msgq_get(ZK_MSGQ(handle), item,
                                  to_zephyr_timeout(timeout_ticks));  // NOLINT(cppcoreguidelines-init-variables)
        return (rc == 0) ? osal::ok() : osal::error_code::timeout;
    }

    // Delegates to osal_queue_receive() with NO_WAIT.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    // Calls k_msgq_peek().  The timeout parameter is unused because
    // k_msgq_peek() is non-blocking.
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

    // Calls k_msgq_num_used_get().
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<std::size_t>(k_msgq_num_used_get(ZK_MSGQ(handle)));
    }

    // Calls k_msgq_num_free_get().
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<std::size_t>(k_msgq_num_free_get(ZK_MSGQ(handle)));
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
    namespace
    {
    zephyr_timer_ctx timer_ctx[OSAL_ZEPHYR_MAX_TIMERS];
    }  // namespace

    // Allocates a k_timer from the static pool and a zephyr_timer_ctx slot,
    // registers the expiry callback via k_timer_user_data_set(), and calls
    // k_timer_init().  The timer is not started until osal_timer_start() is called.
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

        // The timer's reserved pool index also owns its callback context.
        zephyr_timer_ctx* ctx = &timer_ctx[t - zephyr_timers];
        ctx->fn               = callback;
        ctx->arg              = arg;
        ctx->period_ticks     = period_ticks;
        ctx->auto_reload      = auto_reload;

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

    // Calls k_timer_stop(), clears the context callback to mark the slot free,
    // then releases the k_timer pool entry.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
#if defined(CONFIG_SMP)
        // k_timer_stop does not fence an expiry already dispatched on another CPU.
        return osal::error_code::not_supported;
#else
        if (k_is_in_isr())
        {
            return osal::error_code::not_supported;
        }
        const unsigned int key = irq_lock();
        k_timer_stop(ZK_TIMER(handle));
        auto* ctx = static_cast<zephyr_timer_ctx*>(k_timer_user_data_get(ZK_TIMER(handle)));
        if (ctx != nullptr)
        {
            ctx->fn = nullptr;
        }
        release_to_pool(zephyr_timers, zephyr_timer_used, ZK_TIMER(handle));
        handle->native = nullptr;
        irq_unlock(key);
        return osal::ok();
#endif
    }

    // Reads the period and auto-reload flag from the stored zephyr_timer_ctx and
    // calls k_timer_start().  For one-shot timers the repeat period is K_NO_WAIT.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*             ctx      = static_cast<zephyr_timer_ctx*>(k_timer_user_data_get(ZK_TIMER(handle)));
        const k_timeout_t duration = to_zephyr_timeout(ctx->period_ticks);
        const k_timeout_t period   = ctx->auto_reload ? duration : K_NO_WAIT;
        k_timer_start(ZK_TIMER(handle), duration, period);
        return osal::ok();
    }

    // Calls k_timer_stop().  Has no effect if the timer is already stopped.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_timer_stop(ZK_TIMER(handle));
        return osal::ok();
    }

    // Delegates to osal_timer_start(), which calls k_timer_start() with the
    // stored period.
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        return osal_timer_start(handle);
    }

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (new_period_ticks == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        const unsigned int key    = irq_lock();
        const bool         active = k_timer_remaining_ticks(ZK_TIMER(handle)) > 0U;
        auto*              ctx    = static_cast<zephyr_timer_ctx*>(k_timer_user_data_get(ZK_TIMER(handle)));
        ctx->period_ticks         = new_period_ticks;
        const osal::result status = active ? osal_timer_start(handle) : osal::ok();
        irq_unlock(key);
        return status;
    }

    // Calls k_timer_remaining_get(); a non-zero result indicates an active timer.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        return k_timer_remaining_get(ZK_TIMER(handle)) > 0U;
    }

    // ---------------------------------------------------------------------------
    // Event flags (native — Zephyr k_event, requires CONFIG_EVENTS=y)
    // ---------------------------------------------------------------------------

    namespace
    {
    struct k_event zephyr_events[OSAL_ZEPHYR_MAX_EVENTS];
    bool           zephyr_event_used[OSAL_ZEPHYR_MAX_EVENTS];
    }  // namespace

    // Allocates a k_event from the static pool and calls k_event_init().
    // Requires CONFIG_EVENTS=y in the Zephyr build.
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        struct k_event* ev = acquire_from_pool(zephyr_events, zephyr_event_used);
        if (ev == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        k_event_init(ev);
        handle->native = static_cast<void*>(ev);
        return osal::ok();
    }

    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* ev = static_cast<struct k_event*>(handle->native);
        release_to_pool(zephyr_events, zephyr_event_used, ev);
        handle->native = nullptr;
        return osal::ok();
    }

    // Calls k_event_post().
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_event_post(static_cast<struct k_event*>(handle->native), static_cast<uint32_t>(bits));
        return osal::ok();
    }

    // Calls k_event_clear().
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        k_event_clear(static_cast<struct k_event*>(handle->native), static_cast<uint32_t>(bits));
        return osal::ok();
    }

    // Reads the .events field of the underlying k_event atomically.
    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        // k_event stores events in the .events field — read atomically.
        return static_cast<osal::event_bits_t>(static_cast<const struct k_event*>(handle->native)->events);
    }

    // Calls k_event_wait() with reset=false.  When clear_on_exit is true
    // the matched bits are cleared manually via k_event_clear() after the wait succeeds.
    // Returns error_code::would_block when called with NO_WAIT and no bits match.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ev = static_cast<struct k_event*>(handle->native);

        // k_event_wait: returns the matched event bits; 0 on timeout.
        // NOTE: Zephyr's 'reset' param clears ALL events BEFORE waiting,
        //       which is NOT the OSAL clear_on_exit semantics. We always
        //       pass reset=false and manually clear matched bits on success.
        const uint32_t matched = k_event_wait(ev, static_cast<uint32_t>(wait_bits), false,
                                              to_zephyr_timeout(timeout));  // NOLINT(cppcoreguidelines-init-variables)
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

    // Calls k_event_wait_all() with reset=false.  When clear_on_exit is true
    // the matched bits are cleared manually via k_event_clear() after the wait succeeds.
    // Returns error_code::would_block when called with NO_WAIT and not all bits match.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* ev = static_cast<struct k_event*>(handle->native);

        // k_event_wait_all: waits until ALL bits in wait_bits are set.
        // Same reset caveat as wait_any — always false, clear manually.
        const uint32_t matched =  // NOLINT(cppcoreguidelines-init-variables)
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

    // Calls k_event_post(), which is ISR-safe on Zephyr.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
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

    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    // Sets *n to 0 if non-null.
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
    // Condition variable (native — k_condvar)
    // ---------------------------------------------------------------------------

    struct zephyr_condvar_obj
    {
        struct k_condvar cv;
    };

    namespace
    {
    zephyr_condvar_obj condvar_pool[OSAL_ZEPHYR_MAX_CONDVARS];
    bool               condvar_used[OSAL_ZEPHYR_MAX_CONDVARS];

    zephyr_condvar_obj* condvar_alloc() noexcept
    {
        return acquire_from_pool(condvar_pool, condvar_used);
    }
    }  // namespace

    // Allocates a zephyr_condvar_obj from the static pool and calls k_condvar_init().
    osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        auto* obj = condvar_alloc();
        if (obj == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        k_condvar_init(&obj->cv);
        handle->native = obj;
        return osal::ok();
    }

    osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* obj = static_cast<zephyr_condvar_obj*>(handle->native);
        release_to_pool(condvar_pool, condvar_used, obj);
        handle->native = nullptr;
        return osal::ok();
    }

    // Calls k_condvar_wait(), which atomically releases mutex and blocks.
    // On waking (or timeout) the mutex is re-acquired before returning.
    osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                                   osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr || mutex == nullptr || mutex->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* obj = static_cast<zephyr_condvar_obj*>(handle->native);
        // Zephyr OSAL mutex handle stores a struct k_mutex* directly.
        auto*     mtx = static_cast<struct k_mutex*>(mutex->native);
        const int rc =
            k_condvar_wait(&obj->cv, mtx, to_zephyr_timeout(timeout));  // NOLINT(cppcoreguidelines-init-variables)
        if (rc == 0)
        {
            return osal::ok();
        }
        if (rc == -EAGAIN || rc == -ETIMEDOUT)
        {
            return osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    // Calls k_condvar_signal().
    osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* obj = static_cast<zephyr_condvar_obj*>(handle->native);
        k_condvar_signal(&obj->cv);
        return osal::ok();
    }

    // Calls k_condvar_broadcast().
    osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* obj = static_cast<zephyr_condvar_obj*>(handle->native);
        k_condvar_broadcast(&obj->cv);
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Work queue (native — Zephyr k_work_queue)
    // ---------------------------------------------------------------------------

    struct zephyr_wq_entry
    {
        struct k_work work
        {
        };
        osal_work_func_t        func{};
        void*                   arg{};
        struct zephyr_wq_obj_s* parent{};
        std::atomic_bool        in_use{false};
        std::uint64_t           marker{};
    };

    struct zephyr_wq_obj_s
    {
        struct k_work_q workq
        {
        };
        zephyr_wq_entry entries[OSAL_ZEPHYR_WQ_MAX_DEPTH];
        std::size_t     capacity{};
        struct k_sem flush_sem
        {
        };
        struct k_mutex flush_mutex
        {
        };
        struct k_spinlock lock
        {
        };
        std::uint64_t next_marker{};
        std::uint64_t completed_marker{};
    };

    namespace
    {
    zephyr_wq_obj_s zephyr_wq_pool[OSAL_ZEPHYR_MAX_WORK_QUEUES];
    bool            zephyr_wq_used[OSAL_ZEPHYR_MAX_WORK_QUEUES];

    zephyr_wq_obj_s* zephyr_wq_acquire() noexcept
    {
        return acquire_from_pool(zephyr_wq_pool, zephyr_wq_used);
    }

    void zephyr_wq_release(zephyr_wq_obj_s* p) noexcept
    {
        release_to_pool(zephyr_wq_pool, zephyr_wq_used, p);
    }

    void zephyr_wq_handler(struct k_work* work) noexcept
    {
        auto*            e      = CONTAINER_OF(work, zephyr_wq_entry, work);
        auto*            wq     = e->parent;
        k_spinlock_key_t key    = k_spin_lock(&wq->lock);
        const auto       func   = e->func;
        void* const      arg    = e->arg;
        const auto       marker = e->marker;
        k_spin_unlock(&wq->lock, key);
        if (func != nullptr)
        {
            func(arg);
        }
        key = k_spin_lock(&wq->lock);
        if (marker != 0U)
        {
            wq->completed_marker = marker;
            k_sem_give(&wq->flush_sem);
        }
        e->in_use.store(false, std::memory_order_release);
        k_spin_unlock(&wq->lock, key);
    }
    }  // namespace

    // Allocates a zephyr_wq_obj_s from the static pool, initialises each
    // zephyr_wq_entry with k_work_init(), and starts the queue thread via
    // k_work_queue_start().
    osal::result osal_work_queue_create(osal::active_traits::work_queue_handle_t* handle, void* stack,
                                        std::size_t stack_bytes, std::size_t depth, const char* /*name*/) noexcept
    {
        if (handle == nullptr || stack == nullptr || stack_bytes == 0U || depth == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        auto* wq = zephyr_wq_acquire();
        if (wq == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        if (depth > OSAL_ZEPHYR_WQ_MAX_DEPTH)
        {
            zephyr_wq_release(wq);
            return osal::error_code::invalid_argument;
        }
        wq->capacity         = depth;
        wq->next_marker      = 0U;
        wq->completed_marker = 0U;
        for (std::size_t i = 0U; i < depth; ++i)
        {
            k_work_init(&wq->entries[i].work, zephyr_wq_handler);
            wq->entries[i].parent = wq;
            wq->entries[i].in_use.store(false, std::memory_order_relaxed);
        }
        k_sem_init(&wq->flush_sem, 0, 1);
        k_mutex_init(&wq->flush_mutex);
        k_work_queue_start(&wq->workq, static_cast<k_thread_stack_t*>(stack), stack_bytes,
                           CONFIG_NUM_PREEMPT_PRIORITIES - 1, nullptr);
        handle->native = static_cast<void*>(wq);
        return osal::ok();
    }

    // Calls k_work_queue_drain() to wait for all pending items,
    // then k_thread_abort() to stop the queue thread, and finally
    // releases the pool slot.  The internal k_work_q is zeroed so the
    // slot can be reused.
    osal::result osal_work_queue_destroy(osal::active_traits::work_queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* wq = static_cast<zephyr_wq_obj_s*>(handle->native);
        if (k_is_in_isr() || k_current_get() == k_work_queue_thread_get(&wq->workq))
        {
            return osal::error_code::not_supported;
        }
        // Drain pending work, plug to stop new submissions.
        (void)k_work_queue_drain(&wq->workq, true);
        // Abort the work queue's internal thread so the pool entry can be reused.
        k_thread_abort(k_work_queue_thread_get(&wq->workq));
        // Reset the k_work_q so K_WORK_QUEUE_STARTED_BIT is cleared for reuse.
        (void)std::memset(&wq->workq, 0, sizeof(wq->workq));
        zephyr_wq_release(wq);
        handle->native = nullptr;
        return osal::ok();
    }

    // Finds a free zephyr_wq_entry slot, populates it, marks it in-use, and
    // calls k_work_submit_to_queue().
    osal::result osal_work_queue_submit(osal::active_traits::work_queue_handle_t* handle, osal_work_func_t func,
                                        void* arg) noexcept
    {
        if (handle == nullptr || handle->native == nullptr || func == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        auto*                  wq  = static_cast<zephyr_wq_obj_s*>(handle->native);
        const k_spinlock_key_t key = k_spin_lock(&wq->lock);
        for (std::size_t i = 0U; i < wq->capacity; ++i)
        {
            bool expected = false;
            if (wq->entries[i].in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                              std::memory_order_relaxed))
            {
                wq->entries[i].func   = func;
                wq->entries[i].arg    = arg;
                wq->entries[i].marker = 0U;
                const int rc          = k_work_submit_to_queue(&wq->workq, &wq->entries[i].work);
                if (rc < 0)
                {
                    wq->entries[i].in_use.store(false, std::memory_order_release);
                }
                k_spin_unlock(&wq->lock, key);
                return (rc < 0) ? osal::result{osal::error_code::unknown} : osal::ok();
            }
        }
        k_spin_unlock(&wq->lock, key);
        return osal::error_code::overflow;
    }

    // Delegates to osal_work_queue_submit() — k_work_submit_to_queue() is
    // ISR-safe on Zephyr.
    osal::result osal_work_queue_submit_from_isr(osal::active_traits::work_queue_handle_t* handle,
                                                 osal_work_func_t func, void* arg) noexcept
    {
        // k_work_submit_to_queue is ISR-safe on Zephyr.
        return osal_work_queue_submit(handle, func, arg);
    }

    // Serializes flush calls and waits for a uniquely numbered sentinel.
    // Semaphore signals from earlier timed-out calls cannot complete a later flush.
    osal::result osal_work_queue_flush(osal::active_traits::work_queue_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* wq = static_cast<zephyr_wq_obj_s*>(handle->native);
        if (k_is_in_isr() || k_current_get() == k_work_queue_thread_get(&wq->workq))
        {
            return osal::error_code::not_supported;
        }
        const auto deadline = sys_timepoint_calc(to_zephyr_timeout(timeout));
        if (k_mutex_lock(&wq->flush_mutex, sys_timepoint_timeout(deadline)) != 0)
        {
            return osal::error_code::timeout;
        }
        std::uint64_t    marker = 0U;
        k_spinlock_key_t key    = k_spin_lock(&wq->lock);
        for (std::size_t i = 0U; i < wq->capacity; ++i)
        {
            bool expected = false;
            if (wq->entries[i].in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                              std::memory_order_relaxed))
            {
                wq->entries[i].func   = nullptr;
                wq->entries[i].arg    = nullptr;
                marker                = ++wq->next_marker;
                wq->entries[i].marker = marker;
                if (k_work_submit_to_queue(&wq->workq, &wq->entries[i].work) < 0)
                {
                    wq->entries[i].in_use.store(false, std::memory_order_release);
                    k_spin_unlock(&wq->lock, key);
                    k_mutex_unlock(&wq->flush_mutex);
                    return osal::error_code::unknown;
                }
                break;
            }
        }
        k_spin_unlock(&wq->lock, key);
        if (marker == 0U)
        {
            k_mutex_unlock(&wq->flush_mutex);
            return osal::error_code::overflow;
        }
        osal::result result = osal::error_code::timeout;
        for (;;)
        {
            key                  = k_spin_lock(&wq->lock);
            const bool completed = wq->completed_marker >= marker;
            k_spin_unlock(&wq->lock, key);
            if (completed)
            {
                result = osal::ok();
                break;
            }
            if (k_sem_take(&wq->flush_sem, sys_timepoint_timeout(deadline)) != 0)
            {
                break;
            }
        }
        k_mutex_unlock(&wq->flush_mutex);
        return result;
    }

    // Calls k_work_cancel() on every in-use entry, releasing only idle items.
    // Items that are already executing cannot be cancelled.
    osal::result osal_work_queue_cancel_all(osal::active_traits::work_queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*                  wq  = static_cast<zephyr_wq_obj_s*>(handle->native);
        const k_spinlock_key_t key = k_spin_lock(&wq->lock);
        for (std::size_t i = 0U; i < wq->capacity; ++i)
        {
            if (wq->entries[i].in_use.load(std::memory_order_acquire) && wq->entries[i].marker == 0U)
            {
                if (k_work_cancel(&wq->entries[i].work) == 0)
                {
                    wq->entries[i].in_use.store(false, std::memory_order_release);
                }
            }
        }
        k_spin_unlock(&wq->lock, key);
        return osal::ok();
    }

    // Counts zephyr_wq_entry slots where in_use is true.
    std::size_t osal_work_queue_pending(const osal::active_traits::work_queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* wq = static_cast<const zephyr_wq_obj_s*>(handle->native);
        std::size_t n  = 0U;
        for (std::size_t i = 0U; i < wq->capacity; ++i)
        {
            if (wq->entries[i].in_use.load(std::memory_order_acquire))
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
