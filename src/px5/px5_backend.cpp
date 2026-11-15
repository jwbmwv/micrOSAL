// SPDX-License-Identifier: Apache-2.0
/// @file px5_backend.cpp
/// @brief PX5 RTOS implementation of all OSAL C-linkage functions
/// @details PX5 is largely API-compatible with Azure RTOS ThreadX but adds
///          a native `PX5_WAIT_SET` (select/poll equivalent for RTOS objects).
///
///          This backend re-uses the full ThreadX implementation for all
///          primitives and adds a real wait_set implementation via
///          `px5_wait_set_*` APIs.
///
///          Includes required: <px5_api.h>
///          Build macro:       OSAL_BACKEND_PX5
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_PX5
#define OSAL_BACKEND_PX5
#endif
#include <osal/osal.hpp>

#include <px5_api.h>  ///< PX5 extends tx_api.h
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>

/// @note PX5 is API-compatible with ThreadX.  All ThreadX primitives use
///       the same TX_* structures and functions.  Only the wait_set
///       implementation differs.

// ---------------------------------------------------------------------------
// Static pools — identical sizes and layout to threadx_backend.cpp
// ---------------------------------------------------------------------------
#define OSAL_PX5_MAX_THREADS 8
#define OSAL_PX5_MAX_MUTEXES 16
#define OSAL_PX5_MAX_SEMS 16
#define OSAL_PX5_MAX_QUEUES 8
#define OSAL_PX5_MAX_TIMERS 8
#define OSAL_PX5_MAX_EVTGRPS 8
#define OSAL_PX5_MAX_WAITSETS 4

namespace
{

TX_THREAD             px5_threads[OSAL_PX5_MAX_THREADS];
std::atomic<bool>     px5_thread_used[OSAL_PX5_MAX_THREADS];
TX_MUTEX              px5_mutexes[OSAL_PX5_MAX_MUTEXES];
std::atomic<bool>     px5_mutex_used[OSAL_PX5_MAX_MUTEXES];
TX_SEMAPHORE          px5_sems[OSAL_PX5_MAX_SEMS];
std::atomic<bool>     px5_sem_used[OSAL_PX5_MAX_SEMS];
unsigned              px5_sem_max[OSAL_PX5_MAX_SEMS];
std::atomic<unsigned> px5_sem_count[OSAL_PX5_MAX_SEMS];
static_assert(std::atomic<unsigned>::is_always_lock_free);
TX_QUEUE             px5_queues[OSAL_PX5_MAX_QUEUES];
std::atomic<bool>    px5_queue_used[OSAL_PX5_MAX_QUEUES];
ULONG                px5_queue_storage[OSAL_PX5_MAX_QUEUES][256];
std::size_t          px5_queue_item_size[OSAL_PX5_MAX_QUEUES];
TX_TIMER             px5_timers[OSAL_PX5_MAX_TIMERS];
std::atomic<bool>    px5_timer_used[OSAL_PX5_MAX_TIMERS];
std::atomic<bool>    px5_timer_started[OSAL_PX5_MAX_TIMERS];
TX_EVENT_FLAGS_GROUP px5_evtgrps[OSAL_PX5_MAX_EVTGRPS];
std::atomic<bool>    px5_evtgrp_used[OSAL_PX5_MAX_EVTGRPS];
PX5_WAIT_SET         px5_wait_sets[OSAL_PX5_MAX_WAITSETS];
std::atomic<bool>    px5_ws_used[OSAL_PX5_MAX_WAITSETS];
static_assert(std::atomic<bool>::is_always_lock_free);

template<typename T, std::size_t N>
T* pool_acquire(T (&pool)[N], std::atomic<bool> (&used)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!used[i].exchange(true, std::memory_order_acquire))
        {
            return &pool[i];
        }
    }
    return nullptr;
}

template<typename T, std::size_t N>
void pool_release(T (&pool)[N], std::atomic<bool> (&used)[N], T* p) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            used[i].store(false, std::memory_order_release);
            return;
        }
    }
}

// Helper macros
constexpr UINT osal_to_px5_priority(osal::priority_t p) noexcept
{
    const UINT top = TX_MAX_PRIORITIES - 1U;
    return top - static_cast<UINT>((static_cast<std::uint32_t>(p) * static_cast<std::uint32_t>(top)) /
                                   static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
}

constexpr ULONG to_px5_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return TX_WAIT_FOREVER;
    }
    if (t == osal::NO_WAIT)
    {
        return TX_NO_WAIT;
    }
    return static_cast<ULONG>(t);  // NOLINT(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
}

#define PX5_THREAD_PTR(h) static_cast<TX_THREAD*>((h)->native)
#define PX5_MUTEX_PTR(h) static_cast<TX_MUTEX*>((h)->native)
#define PX5_SEM_PTR(h) static_cast<TX_SEMAPHORE*>((h)->native)
#define PX5_QUEUE_PTR(h) static_cast<TX_QUEUE*>((h)->native)
#define PX5_TIMER_PTR(h) static_cast<TX_TIMER*>((h)->native)
#define PX5_EVT_PTR(h) static_cast<TX_EVENT_FLAGS_GROUP*>((h)->native)
#define PX5_WS_PTR(h) static_cast<PX5_WAIT_SET*>((h)->native)

// ---------------------------------------------------------------------------
// Timer context
// ---------------------------------------------------------------------------
struct px5_timer_ctx
{
    osal_timer_callback_t fn{};
    void*                 arg{};
    ULONG                 period{};
    TX_TIMER*             timer{};
    ULONG                 initial{};
};
px5_timer_ctx px5_timer_ctxs[OSAL_PX5_MAX_TIMERS];

}  // anonymous namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock — identical to ThreadX
    // ---------------------------------------------------------------------------

    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(tx_time_get()) * 1000LL / TX_TIMER_TICKS_PER_SECOND;
    }

    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(tx_time_get());
    }

    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / TX_TIMER_TICKS_PER_SECOND);
    }

    // ---------------------------------------------------------------------------
    // Thread (ThreadX API — identical to threadx_backend)
    // ---------------------------------------------------------------------------

    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
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
        TX_THREAD* t = pool_acquire(px5_threads, px5_thread_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        const UINT prio = osal_to_px5_priority(priority);
        const UINT rc   = tx_thread_create(t, const_cast<CHAR*>(name != nullptr ? name : "osal"),
                                           reinterpret_cast<void (*)(ULONG)>(reinterpret_cast<void*>(entry)),
                                           reinterpret_cast<ULONG>(arg), stack, static_cast<ULONG>(stack_bytes), prio,
                                           prio, TX_NO_TIME_SLICE, TX_AUTO_START);
        if (rc != TX_SUCCESS)
        {
            pool_release(px5_threads, px5_thread_used, t);
            return osal::error_code::out_of_resources;
        }
        handle->native = t;
        return osal::ok();
    }

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (tx_thread_terminate(PX5_THREAD_PTR(handle)) != TX_SUCCESS ||
            tx_thread_delete(PX5_THREAD_PTR(handle)) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        pool_release(px5_threads, px5_thread_used, PX5_THREAD_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return osal::error_code::not_supported;
    }

    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        UINT old;
        return (tx_thread_priority_change(PX5_THREAD_PTR(handle), osal_to_px5_priority(priority), &old) == TX_SUCCESS)
                   ? osal::ok()
                   : osal::error_code::unknown;
    }

    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t*, osal::affinity_t) noexcept
    {
        return osal::error_code::not_supported;
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
        tx_thread_relinquish();
    }
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        const ULONG t = (static_cast<ULONG>(ms) * TX_TIMER_TICKS_PER_SECOND) / 1000U;
        tx_thread_sleep(t == 0U ? 1U : t);
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool) noexcept
    {
        TX_MUTEX* m = pool_acquire(px5_mutexes, px5_mutex_used);
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        if (tx_mutex_create(m, const_cast<CHAR*>("m"), TX_INHERIT) != TX_SUCCESS)
        {
            pool_release(px5_mutexes, px5_mutex_used, m);
            return osal::error_code::out_of_resources;
        }
        handle->native = m;
        return osal::ok();
    }

    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_mutex_delete(PX5_MUTEX_PTR(handle));
        pool_release(px5_mutexes, px5_mutex_used, PX5_MUTEX_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = tx_mutex_get(PX5_MUTEX_PTR(handle), to_px5_ticks(timeout));
        if (rc == TX_SUCCESS)
        {
            return osal::ok();
        }
        if (rc == TX_NOT_AVAILABLE || rc == TX_WAIT_ABORTED)
        {
            return osal::error_code::timeout;
        }
        return osal::error_code::unknown;
    }

    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (tx_mutex_put(PX5_MUTEX_PTR(handle)) == TX_SUCCESS) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned init,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || init > max_count)
        {
            return osal::error_code::invalid_argument;
        }
        TX_SEMAPHORE* s = pool_acquire(px5_sems, px5_sem_used);
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        if (tx_semaphore_create(s, const_cast<CHAR*>("s"), static_cast<ULONG>(init)) != TX_SUCCESS)
        {
            pool_release(px5_sems, px5_sem_used, s);
            return osal::error_code::out_of_resources;
        }
        handle->native            = s;
        px5_sem_max[s - px5_sems] = max_count;
        px5_sem_count[s - px5_sems].store(init, std::memory_order_relaxed);
        return osal::ok();
    }

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        if (tx_semaphore_delete(PX5_SEM_PTR(handle)) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        pool_release(px5_sems, px5_sem_used, PX5_SEM_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const auto index    = PX5_SEM_PTR(handle) - px5_sems;
        auto&      count    = px5_sem_count[index];
        unsigned   value    = count.load(std::memory_order_relaxed);
        bool       reserved = false;
        for (unsigned attempt = 0; attempt < 32U; ++attempt)
        {
            if (value >= px5_sem_max[index])
            {
                return osal::error_code::overflow;
            }
            if (count.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed))
            {
                reserved = true;
                break;
            }
        }
        if (!reserved)
        {
            return osal::error_code::would_block;
        }
        if (tx_semaphore_put(PX5_SEM_PTR(handle)) != TX_SUCCESS)
        {
            count.fetch_sub(1U, std::memory_order_relaxed);
            return osal::error_code::unknown;
        }
        return osal::ok();
    }

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);
    }

    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (tx_semaphore_get(PX5_SEM_PTR(handle), to_px5_ticks(timeout)) != TX_SUCCESS)
        {
            return osal::error_code::timeout;
        }
        px5_sem_count[PX5_SEM_PTR(handle) - px5_sems].fetch_sub(1U, std::memory_order_relaxed);
        return osal::ok();
    }

    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (identical to ThreadX)
    // ---------------------------------------------------------------------------

    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buf, std::size_t item_sz,
                                   std::size_t cap) noexcept
    {
        if (handle == nullptr || buf == nullptr || item_sz == 0U || cap == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        if (item_sz > 16U * sizeof(ULONG))
        {
            return osal::error_code::not_supported;
        }
        const UINT words = static_cast<UINT>((item_sz + sizeof(ULONG) - 1U) / sizeof(ULONG));
        if (cap > 256U / words)
        {
            return osal::error_code::out_of_resources;
        }
        TX_QUEUE* q = pool_acquire(px5_queues, px5_queue_used);
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        const auto idx           = static_cast<std::size_t>(q - px5_queues);
        px5_queue_item_size[idx] = item_sz;
        if (tx_queue_create(q, const_cast<CHAR*>("q"), words, px5_queue_storage[idx],
                            static_cast<ULONG>(cap * words * sizeof(ULONG))) != TX_SUCCESS)
        {
            pool_release(px5_queues, px5_queue_used, q);
            return osal::error_code::out_of_resources;
        }
        handle->native = q;
        return osal::ok();
    }

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_queue_delete(PX5_QUEUE_PTR(handle));
        pool_release(px5_queues, px5_queue_used, PX5_QUEUE_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (item == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        ULONG message[16]{};
        std::memcpy(message, item, px5_queue_item_size[PX5_QUEUE_PTR(handle) - px5_queues]);
        return (tx_queue_send(PX5_QUEUE_PTR(handle), message, to_px5_ticks(timeout)) == TX_SUCCESS)
                   ? osal::ok()
                   : osal::error_code::timeout;
    }

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (item == nullptr)
        {
            return osal::error_code::invalid_argument;
        }
        ULONG      message[16]{};
        const UINT rc = tx_queue_receive(PX5_QUEUE_PTR(handle), message, to_px5_ticks(timeout));
        if (rc == TX_SUCCESS)
        {
            std::memcpy(item, message, px5_queue_item_size[PX5_QUEUE_PTR(handle) - px5_queues]);
        }
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::timeout;
    }

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t*, void*, osal::tick_t) noexcept
    {
        return osal::error_code::not_supported;
    }

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ULONG      e = 0U, a = 0U, s = 0U;
        TX_THREAD* f = nullptr;
        TX_QUEUE*  n = nullptr;
        tx_queue_info_get(const_cast<TX_QUEUE*>(PX5_QUEUE_PTR(handle)), nullptr, &e, &a, &f, &s, &n);
        return static_cast<std::size_t>(e);
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ULONG      e = 0U, a = 0U, s = 0U;
        TX_THREAD* f = nullptr;
        TX_QUEUE*  n = nullptr;
        tx_queue_info_get(const_cast<TX_QUEUE*>(PX5_QUEUE_PTR(handle)), nullptr, &e, &a, &f, &s, &n);
        return static_cast<std::size_t>(a);
    }

    // ---------------------------------------------------------------------------
    // Timer
    // ---------------------------------------------------------------------------

    namespace
    {

    void px5_timer_expiry(ULONG idx) noexcept
    {
        if (px5_timer_ctxs[idx].fn != nullptr)
        {
            px5_timer_ctxs[idx].fn(px5_timer_ctxs[idx].arg);
        }
    }

    }  // anonymous namespace

    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period, bool auto_reload) noexcept
    {
        if (handle == nullptr || cb == nullptr || period == 0U || period == osal::WAIT_FOREVER ||
            period > std::numeric_limits<ULONG>::max())
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        TX_TIMER* t = pool_acquire(px5_timers, px5_timer_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        const auto idx = static_cast<ULONG>(t - px5_timers);
        px5_timer_started[idx].store(false, std::memory_order_relaxed);
        const ULONG resched = auto_reload ? static_cast<ULONG>(period) : 0U;
        px5_timer_ctxs[idx] = {cb, arg, resched, t, static_cast<ULONG>(period)};
        if (tx_timer_create(t, const_cast<CHAR*>(name != nullptr ? name : "t"), px5_timer_expiry, idx,
                            static_cast<ULONG>(period), resched, TX_NO_ACTIVATE) != TX_SUCCESS)
        {
            px5_timer_ctxs[idx].fn = nullptr;
            pool_release(px5_timers, px5_timer_used, t);
            return osal::error_code::out_of_resources;
        }
        handle->native = t;
        return osal::ok();
    }

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        // No SDK-backed callback-dispatch barrier is available for slot reuse.
        if (px5_timer_started[PX5_TIMER_PTR(handle) - px5_timers].load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (tx_timer_delete(PX5_TIMER_PTR(handle)) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        for (auto& c : px5_timer_ctxs)
        {
            if (c.timer == PX5_TIMER_PTR(handle))
            {
                c.fn = nullptr;
                break;
            }
        }
        pool_release(px5_timers, px5_timer_used, PX5_TIMER_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        UINT active = TX_FALSE;
        if (tx_timer_info_get(PX5_TIMER_PTR(handle), nullptr, &active, nullptr, nullptr, nullptr) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        if (active == TX_TRUE)
        {
            return osal::ok();
        }
        const auto& ctx = px5_timer_ctxs[PX5_TIMER_PTR(handle) - px5_timers];
        px5_timer_started[PX5_TIMER_PTR(handle) - px5_timers].store(true, std::memory_order_release);
        if (tx_timer_change(PX5_TIMER_PTR(handle), ctx.initial, ctx.period) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        return tx_timer_activate(PX5_TIMER_PTR(handle)) == TX_SUCCESS ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return tx_timer_deactivate(PX5_TIMER_PTR(handle)) == TX_SUCCESS ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        const auto stopped = osal_timer_stop(handle);
        if (!stopped.ok())
        {
            return stopped;
        }
        return osal_timer_start(handle);
    }

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle, osal::tick_t new_period) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (new_period == 0U || new_period == osal::WAIT_FOREVER || new_period > std::numeric_limits<ULONG>::max())
        {
            return osal::error_code::invalid_argument;
        }
        auto& ctx    = px5_timer_ctxs[PX5_TIMER_PTR(handle) - px5_timers];
        UINT  active = TX_FALSE;
        if (tx_timer_info_get(PX5_TIMER_PTR(handle), nullptr, &active, nullptr, nullptr, nullptr) != TX_SUCCESS ||
            tx_timer_deactivate(PX5_TIMER_PTR(handle)) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        const ULONG next = static_cast<ULONG>(new_period);
        if (tx_timer_change(PX5_TIMER_PTR(handle), next, ctx.period == 0U ? 0U : static_cast<ULONG>(new_period)) !=
            TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        if (ctx.period != 0U)
        {
            ctx.period = static_cast<ULONG>(new_period);
        }
        ctx.initial = static_cast<ULONG>(new_period);
        return active != TX_TRUE || tx_timer_activate(PX5_TIMER_PTR(handle)) == TX_SUCCESS ? osal::ok()
                                                                                           : osal::error_code::unknown;
    }

    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        UINT active = TX_FALSE;
        tx_timer_info_get(const_cast<TX_TIMER*>(PX5_TIMER_PTR(handle)), nullptr, &active, nullptr, nullptr, nullptr);
        return active == TX_TRUE;
    }

    // ---------------------------------------------------------------------------
    // Event flags (native TX_EVENT_FLAGS_GROUP)
    // ---------------------------------------------------------------------------

    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        TX_EVENT_FLAGS_GROUP* e = pool_acquire(px5_evtgrps, px5_evtgrp_used);
        if (e == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        if (tx_event_flags_create(e, const_cast<CHAR*>("ef")) != TX_SUCCESS)
        {
            pool_release(px5_evtgrps, px5_evtgrp_used, e);
            return osal::error_code::out_of_resources;
        }
        handle->native = e;
        return osal::ok();
    }

    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_event_flags_delete(PX5_EVT_PTR(handle));
        pool_release(px5_evtgrps, px5_evtgrp_used, PX5_EVT_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (tx_event_flags_set(PX5_EVT_PTR(handle), static_cast<ULONG>(bits), TX_OR) == TX_SUCCESS)
                   ? osal::ok()
                   : osal::error_code::unknown;
    }

    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (tx_event_flags_set(PX5_EVT_PTR(handle), static_cast<ULONG>(~bits), TX_AND) == TX_SUCCESS)
                   ? osal::ok()
                   : osal::error_code::unknown;
    }

    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ULONG actual = 0U;
        tx_event_flags_get(const_cast<TX_EVENT_FLAGS_GROUP*>(PX5_EVT_PTR(handle)), 0xFFFFFFFFUL, TX_OR, &actual,
                           TX_NO_WAIT);
        return static_cast<osal::event_bits_t>(actual);
    }

    namespace
    {

    osal::result px5_evt_wait(osal::active_traits::event_flags_handle_t* h, osal::event_bits_t bits,
                              osal::event_bits_t* actual, bool coe, bool all, osal::tick_t timeout) noexcept
    {
        if (h == nullptr || h->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        UINT opt = all ? TX_AND : TX_OR;
        if (coe)
        {
            opt = all ? TX_AND_CLEAR : TX_OR_CLEAR;
        }
        ULONG      a  = 0U;
        const UINT rc = tx_event_flags_get(PX5_EVT_PTR(h), static_cast<ULONG>(bits), opt, &a, to_px5_ticks(timeout));
        if (actual != nullptr)
        {
            *actual = static_cast<osal::event_bits_t>(a);
        }
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::timeout;
    }

    }  // anonymous namespace

    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* h, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool coe, osal::tick_t timeout) noexcept
    {
        return px5_evt_wait(h, bits, actual, coe, false, timeout);
    }

    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* h, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool coe, osal::tick_t timeout) noexcept
    {
        return px5_evt_wait(h, bits, actual, coe, true, timeout);
    }

    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);
    }

    // ---------------------------------------------------------------------------
    // Wait-set (native PX5_WAIT_SET)
    // ---------------------------------------------------------------------------

    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        PX5_WAIT_SET* ws = pool_acquire(px5_wait_sets, px5_ws_used);
        if (ws == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        if (px5_wait_set_create(ws, const_cast<CHAR*>("ws")) != PX5_SUCCESS)
        {
            pool_release(px5_wait_sets, px5_ws_used, ws);
            return osal::error_code::out_of_resources;
        }
        handle->native = ws;
        return osal::ok();
    }

    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        px5_wait_set_delete(PX5_WS_PTR(handle));
        pool_release(px5_wait_sets, px5_ws_used, PX5_WS_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t* handle, int fd,
                                   std::uint32_t events) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        // PX5 wait_set_add takes a kernel object pointer and event mask.
        const UINT rc = px5_wait_set_add(PX5_WS_PTR(handle), reinterpret_cast<void*>(static_cast<uintptr_t>(fd)),
                                         static_cast<ULONG>(events));
        return (rc == PX5_SUCCESS) ? osal::ok() : osal::error_code::invalid_argument;
    }

    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t* handle, int fd) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = px5_wait_set_remove(PX5_WS_PTR(handle), reinterpret_cast<void*>(static_cast<uintptr_t>(fd)));
        return (rc == PX5_SUCCESS) ? osal::ok() : osal::error_code::invalid_argument;
    }

    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t* handle, int* fds_ready,
                                    std::size_t /*max_ready*/, std::size_t*              n_ready,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        ULONG      actual_events = 0U;
        VOID*      signaled_obj  = nullptr;
        const UINT rc =
            px5_wait_set_wait(PX5_WS_PTR(handle), &signaled_obj, &actual_events, to_px5_ticks(timeout_ticks));
        if (rc == PX5_SUCCESS)
        {
            if (fds_ready != nullptr)
            {
                *fds_ready = static_cast<int>(reinterpret_cast<uintptr_t>(signaled_obj));
            }
            if (n_ready != nullptr)
            {
                *n_ready = 1U;
            }
            return osal::ok();
        }
        if (n_ready != nullptr)
        {
            *n_ready = 0U;
        }
        return osal::error_code::timeout;
    }

    // ---------------------------------------------------------------------------
    // Condition variable (emulated — Birrell pattern: mutex + semaphore)
    // ---------------------------------------------------------------------------

#include "../common/emulated_condvar.inl"

    // ---------------------------------------------------------------------------
    // Work queue (emulated — OSAL thread + mutex + semaphore + ring buffer)
    // ---------------------------------------------------------------------------

#include "../common/emulated_work_queue.inl"

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
#include "../common/emulated_spinlock.inl"

    // ---------------------------------------------------------------------------
    // Barrier
    // ---------------------------------------------------------------------------
#include "../common/emulated_barrier.inl"

    // ---------------------------------------------------------------------------
    // Task notification
    // ---------------------------------------------------------------------------
#include "../common/emulated_task_notify.inl"

}  // extern "C"
