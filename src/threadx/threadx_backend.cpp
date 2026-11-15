// SPDX-License-Identifier: Apache-2.0
/// @file threadx_backend.cpp
/// @brief Azure RTOS ThreadX implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the Native ThreadX API (tx_api.h).
///
///          All kernel objects are held in static pools.
///          Dynamic allocation is never used.
///
///          Recursive mutex: ThreadX mutex DOES support recursive locking
///          when created with TX_INHERIT (priority inheritance).  A second
///          create flag TX_NO_INHERIT disables inheritance; we use TX_INHERIT
///          always and allow recursive lock tracking.
///
///          Timer: one-shot vs auto-reload is modelled by re-arming in the
///          expiry callback for the auto-reload case.
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_THREADX
#define OSAL_BACKEND_THREADX
#endif
#include <osal/osal.hpp>

#include <tx_api.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>

// ---------------------------------------------------------------------------
// Pool sizes
// ---------------------------------------------------------------------------
#define OSAL_TX_MAX_THREADS 8
#define OSAL_TX_MAX_MUTEXES 16
#define OSAL_TX_MAX_SEMS 16
#define OSAL_TX_MAX_QUEUES 8
#define OSAL_TX_MAX_TIMERS 8
#define OSAL_TX_MAX_EVTGRPS 8

// ---------------------------------------------------------------------------
// Static object pools
// ---------------------------------------------------------------------------
namespace
{

TX_THREAD         tx_threads[OSAL_TX_MAX_THREADS];
std::atomic<bool> tx_thread_used[OSAL_TX_MAX_THREADS];
struct tx_thread_ctx
{
    void (*entry)(void*){};
    void* arg{};
};
tx_thread_ctx        tx_thread_ctxs[OSAL_TX_MAX_THREADS];
TX_MUTEX             tx_mutexes[OSAL_TX_MAX_MUTEXES];
std::atomic<bool>    tx_mutex_used[OSAL_TX_MAX_MUTEXES];
TX_SEMAPHORE         tx_sems[OSAL_TX_MAX_SEMS];
std::atomic<bool>    tx_sem_used[OSAL_TX_MAX_SEMS];
unsigned             tx_sem_max[OSAL_TX_MAX_SEMS];
TX_QUEUE             tx_queues[OSAL_TX_MAX_QUEUES];
std::atomic<bool>    tx_queue_used[OSAL_TX_MAX_QUEUES];
ULONG                tx_queue_storage[OSAL_TX_MAX_QUEUES][256];
std::size_t          tx_queue_item_size[OSAL_TX_MAX_QUEUES];
TX_TIMER             tx_timers[OSAL_TX_MAX_TIMERS];
std::atomic<bool>    tx_timer_used[OSAL_TX_MAX_TIMERS];
std::atomic<bool>    tx_timer_started[OSAL_TX_MAX_TIMERS];
TX_EVENT_FLAGS_GROUP tx_evtgrps[OSAL_TX_MAX_EVTGRPS];
std::atomic<bool>    tx_evtgrp_used[OSAL_TX_MAX_EVTGRPS];
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

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void tx_thread_entry(ULONG index) noexcept
{
    const auto& ctx = tx_thread_ctxs[index];
    ctx.entry(ctx.arg);
}

constexpr UINT osal_to_tx_priority(osal::priority_t p) noexcept
{
    const UINT tx_max = TX_MAX_PRIORITIES - 1U;
    return tx_max - static_cast<UINT>((static_cast<std::uint32_t>(p) * static_cast<std::uint32_t>(tx_max)) /
                                      static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
}

constexpr ULONG to_tx_ticks(osal::tick_t t) noexcept
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

#define TX_THREAD_PTR(h) static_cast<TX_THREAD*>((h)->native)
#define TX_MUTEX_PTR(h) static_cast<TX_MUTEX*>((h)->native)
#define TX_SEM_PTR(h) static_cast<TX_SEMAPHORE*>((h)->native)
#define TX_QUEUE_PTR(h) static_cast<TX_QUEUE*>((h)->native)
#define TX_TIMER_PTR(h) static_cast<TX_TIMER*>((h)->native)
#define TX_EVT_PTR(h) static_cast<TX_EVENT_FLAGS_GROUP*>((h)->native)

// ---------------------------------------------------------------------------
// Timer callback context
// ---------------------------------------------------------------------------
struct tx_timer_ctx
{
    osal_timer_callback_t fn{};
    void*                 arg{};
    ULONG                 period{};  ///< 0 = one-shot
    TX_TIMER*             timer{};
    ULONG                 initial{};
};
tx_timer_ctx tx_timer_ctxs[OSAL_TX_MAX_TIMERS];

}  // namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
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
    // Thread
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
        TX_THREAD* t = pool_acquire(tx_threads, tx_thread_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        const auto index      = static_cast<ULONG>(t - tx_threads);
        tx_thread_ctxs[index] = {entry, arg};
        const UINT prio       = osal_to_tx_priority(priority);
        const UINT rc =
            tx_thread_create(t, const_cast<CHAR*>(name != nullptr ? name : "osal"), tx_thread_entry, index, stack,
                             static_cast<ULONG>(stack_bytes), prio, prio, TX_NO_TIME_SLICE, TX_AUTO_START);

        if (rc != TX_SUCCESS)
        {
            pool_release(tx_threads, tx_thread_used, t);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout_ticks*/) noexcept
    {
        // ThreadX does not have a native join.  Terminate + delete.
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        TX_THREAD* t = TX_THREAD_PTR(handle);
        if (tx_thread_terminate(t) != TX_SUCCESS || tx_thread_delete(t) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        pool_release(tx_threads, tx_thread_used, t);
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
        UINT       old_prio;
        const UINT rc = tx_thread_priority_change(TX_THREAD_PTR(handle), osal_to_tx_priority(priority), &old_prio);
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
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
        const ULONG ticks = (static_cast<ULONG>(ms) * TX_TIMER_TICKS_PER_SECOND) / 1000U;
        tx_thread_sleep(ticks == 0U ? 1U : ticks);
    }

    // ---------------------------------------------------------------------------
    // Mutex (TX_MUTEX with priority inheritance)
    // ---------------------------------------------------------------------------

    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        TX_MUTEX* m = pool_acquire(tx_mutexes, tx_mutex_used);
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        // TX_INHERIT enables priority inheritance AND recursive locking.
        const UINT rc = tx_mutex_create(m, const_cast<CHAR*>("m"), TX_INHERIT);
        if (rc != TX_SUCCESS)
        {
            pool_release(tx_mutexes, tx_mutex_used, m);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(m);
        return osal::ok();
    }

    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_mutex_delete(TX_MUTEX_PTR(handle));
        pool_release(tx_mutexes, tx_mutex_used, TX_MUTEX_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = tx_mutex_get(TX_MUTEX_PTR(handle), to_tx_ticks(timeout_ticks));
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
        const UINT rc = tx_mutex_put(TX_MUTEX_PTR(handle));
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        if (handle == nullptr || max_count == 0U || initial_count > max_count)
        {
            return osal::error_code::invalid_argument;
        }
        TX_SEMAPHORE* s = pool_acquire(tx_sems, tx_sem_used);
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        const UINT rc = tx_semaphore_create(s, const_cast<CHAR*>("s"), static_cast<ULONG>(initial_count));
        if (rc != TX_SUCCESS)
        {
            pool_release(tx_sems, tx_sem_used, s);
            return osal::error_code::out_of_resources;
        }
        handle->native          = static_cast<void*>(s);
        tx_sem_max[s - tx_sems] = max_count;
        return osal::ok();
    }

    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        if (tx_semaphore_delete(TX_SEM_PTR(handle)) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        pool_release(tx_sems, tx_sem_used, TX_SEM_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = tx_semaphore_ceiling_put(TX_SEM_PTR(handle), tx_sem_max[TX_SEM_PTR(handle) - tx_sems]);
        return rc == TX_SUCCESS ? osal::ok() : osal::error_code::overflow;
    }

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // tx_semaphore_put is ISR-safe
    }

    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = tx_semaphore_get(TX_SEM_PTR(handle), to_tx_ticks(timeout_ticks));
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::timeout;
    }

    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (TX_QUEUE — works with 32-bit word items)
    // ---------------------------------------------------------------------------

    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        if (handle == nullptr || buffer == nullptr || item_size == 0U || capacity == 0U)
        {
            return osal::error_code::invalid_argument;
        }
        if (item_size > 16U * sizeof(ULONG))
        {
            return osal::error_code::not_supported;
        }
        const UINT msg_size_words = static_cast<UINT>((item_size + sizeof(ULONG) - 1U) / sizeof(ULONG));
        if (capacity > 256U / msg_size_words)
        {
            return osal::error_code::out_of_resources;
        }
        TX_QUEUE* q = pool_acquire(tx_queues, tx_queue_used);
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        const auto idx          = static_cast<std::size_t>(q - tx_queues);
        tx_queue_item_size[idx] = item_size;
        const UINT rc           = tx_queue_create(q, const_cast<CHAR*>("q"), msg_size_words, tx_queue_storage[idx],
                                                  static_cast<ULONG>(capacity * msg_size_words * sizeof(ULONG)));

        if (rc != TX_SUCCESS)
        {
            pool_release(tx_queues, tx_queue_used, q);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(q);
        return osal::ok();
    }

    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_queue_delete(TX_QUEUE_PTR(handle));
        pool_release(tx_queues, tx_queue_used, TX_QUEUE_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
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
        std::memcpy(message, item, tx_queue_item_size[TX_QUEUE_PTR(handle) - tx_queues]);
        const UINT rc = tx_queue_send(TX_QUEUE_PTR(handle), message, to_tx_ticks(timeout_ticks));
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::timeout;
    }

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
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
        const UINT rc = tx_queue_receive(TX_QUEUE_PTR(handle), message, to_tx_ticks(timeout_ticks));
        if (rc == TX_SUCCESS)
        {
            std::memcpy(item, message, tx_queue_item_size[TX_QUEUE_PTR(handle) - tx_queues]);
        }
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::timeout;
    }

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t /*timeout_ticks*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        (void)item;
        return osal::error_code::not_supported;
    }

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ULONG      enqueued  = 0U;
        ULONG      available = 0U;
        ULONG      suspended = 0U;
        TX_THREAD* first     = nullptr;
        TX_QUEUE*  next      = nullptr;
        tx_queue_info_get(const_cast<TX_QUEUE*>(TX_QUEUE_PTR(handle)), nullptr, &enqueued, &available, &first,
                          &suspended, &next);
        return static_cast<std::size_t>(enqueued);
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ULONG      enqueued  = 0U;
        ULONG      available = 0U;
        ULONG      suspended = 0U;
        TX_THREAD* first     = nullptr;
        TX_QUEUE*  next      = nullptr;
        tx_queue_info_get(const_cast<TX_QUEUE*>(TX_QUEUE_PTR(handle)), nullptr, &enqueued, &available, &first,
                          &suspended, &next);
        return static_cast<std::size_t>(available);
    }

    // ---------------------------------------------------------------------------
    // Timer (TX_TIMER)
    // ---------------------------------------------------------------------------

    namespace
    {

    void tx_timer_expiry(ULONG ctx_idx) noexcept
    {
        auto& ctx = tx_timer_ctxs[ctx_idx];
        if (ctx.fn != nullptr)
        {
            ctx.fn(ctx.arg);
        }
        // Periodic re-arm is handled by tx_timer activating with reschedule_ticks.
    }

    }  // namespace

    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || callback == nullptr || period_ticks == 0U || period_ticks == osal::WAIT_FOREVER ||
            period_ticks > std::numeric_limits<ULONG>::max())
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        TX_TIMER* t = pool_acquire(tx_timers, tx_timer_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        const auto ctx_idx = static_cast<ULONG>(t - tx_timers);
        tx_timer_started[ctx_idx].store(false, std::memory_order_relaxed);
        tx_timer_ctxs[ctx_idx].fn      = callback;
        tx_timer_ctxs[ctx_idx].arg     = arg;
        tx_timer_ctxs[ctx_idx].period  = auto_reload ? static_cast<ULONG>(period_ticks) : 0U;
        tx_timer_ctxs[ctx_idx].timer   = t;
        tx_timer_ctxs[ctx_idx].initial = static_cast<ULONG>(period_ticks);

        const ULONG reschedule = auto_reload ? static_cast<ULONG>(period_ticks) : 0U;
        const UINT  rc = tx_timer_create(t, const_cast<CHAR*>(name != nullptr ? name : "t"), tx_timer_expiry, ctx_idx,
                                         static_cast<ULONG>(period_ticks), reschedule, TX_NO_ACTIVATE);

        if (rc != TX_SUCCESS)
        {
            tx_timer_ctxs[ctx_idx].fn = nullptr;
            pool_release(tx_timers, tx_timer_used, t);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        // No SDK-backed callback-dispatch barrier is available for slot reuse.
        if (tx_timer_started[TX_TIMER_PTR(handle) - tx_timers].load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (tx_timer_delete(TX_TIMER_PTR(handle)) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        for (auto& ctx : tx_timer_ctxs)
        {
            if (ctx.timer == TX_TIMER_PTR(handle))
            {
                ctx.fn = nullptr;
                break;
            }
        }
        pool_release(tx_timers, tx_timer_used, TX_TIMER_PTR(handle));
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
        if (tx_timer_info_get(TX_TIMER_PTR(handle), nullptr, &active, nullptr, nullptr, nullptr) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        if (active == TX_TRUE)
        {
            return osal::ok();
        }
        const auto& ctx = tx_timer_ctxs[TX_TIMER_PTR(handle) - tx_timers];
        tx_timer_started[TX_TIMER_PTR(handle) - tx_timers].store(true, std::memory_order_release);
        if (tx_timer_change(TX_TIMER_PTR(handle), ctx.initial, ctx.period) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        return tx_timer_activate(TX_TIMER_PTR(handle)) == TX_SUCCESS ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return tx_timer_deactivate(TX_TIMER_PTR(handle)) == TX_SUCCESS ? osal::ok() : osal::error_code::unknown;
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

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (new_period_ticks == 0U || new_period_ticks == osal::WAIT_FOREVER ||
            new_period_ticks > std::numeric_limits<ULONG>::max())
        {
            return osal::error_code::invalid_argument;
        }
        auto& ctx    = tx_timer_ctxs[TX_TIMER_PTR(handle) - tx_timers];
        UINT  active = TX_FALSE;
        if (tx_timer_info_get(TX_TIMER_PTR(handle), nullptr, &active, nullptr, nullptr, nullptr) != TX_SUCCESS ||
            tx_timer_deactivate(TX_TIMER_PTR(handle)) != TX_SUCCESS)
        {
            return osal::error_code::unknown;
        }
        const ULONG next = static_cast<ULONG>(new_period_ticks);
        const UINT  rc =
            tx_timer_change(TX_TIMER_PTR(handle), next, ctx.period == 0U ? 0U : static_cast<ULONG>(new_period_ticks));
        if (rc == TX_SUCCESS)
        {
            if (ctx.period != 0U)
            {
                ctx.period = static_cast<ULONG>(new_period_ticks);
            }
            ctx.initial = static_cast<ULONG>(new_period_ticks);
            return active != TX_TRUE || tx_timer_activate(TX_TIMER_PTR(handle)) == TX_SUCCESS
                       ? osal::ok()
                       : osal::error_code::unknown;
        }
        return osal::error_code::unknown;
    }

    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        UINT active_flag = TX_FALSE;
        tx_timer_info_get(const_cast<TX_TIMER*>(TX_TIMER_PTR(handle)), nullptr, &active_flag, nullptr, nullptr,
                          nullptr);
        return active_flag == TX_TRUE;
    }

    // ---------------------------------------------------------------------------
    // Event flags (native TX_EVENT_FLAGS_GROUP)
    // ---------------------------------------------------------------------------

    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        TX_EVENT_FLAGS_GROUP* e = pool_acquire(tx_evtgrps, tx_evtgrp_used);
        if (e == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        const UINT rc = tx_event_flags_create(e, const_cast<CHAR*>("ef"));
        if (rc != TX_SUCCESS)
        {
            pool_release(tx_evtgrps, tx_evtgrp_used, e);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(e);
        return osal::ok();
    }

    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_event_flags_delete(TX_EVT_PTR(handle));
        pool_release(tx_evtgrps, tx_evtgrp_used, TX_EVT_PTR(handle));
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
        const UINT rc = tx_event_flags_set(TX_EVT_PTR(handle), static_cast<ULONG>(bits), TX_OR);
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        // ThreadX clear: set with AND and the inverse.
        const UINT rc = tx_event_flags_set(TX_EVT_PTR(handle), static_cast<ULONG>(~bits), TX_AND);
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::unknown;
    }

    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ULONG actual = 0U;
        tx_event_flags_get(const_cast<TX_EVENT_FLAGS_GROUP*>(TX_EVT_PTR(handle)), 0xFFFFFFFFUL, TX_OR, &actual,
                           TX_NO_WAIT);
        return static_cast<osal::event_bits_t>(actual);
    }

    namespace
    {

    osal::result tx_event_wait_impl(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t wait_bits,
                                    osal::event_bits_t* actual_bits, bool clear_on_exit, bool all,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        UINT get_option = all ? TX_AND : TX_OR;
        if (clear_on_exit)
        {
            get_option = all ? TX_AND_CLEAR : TX_OR_CLEAR;
        }

        ULONG      actual = 0U;
        const UINT rc     = tx_event_flags_get(TX_EVT_PTR(handle), static_cast<ULONG>(wait_bits), get_option, &actual,
                                               to_tx_ticks(timeout_ticks));

        if (actual_bits != nullptr)
        {
            *actual_bits = static_cast<osal::event_bits_t>(actual);
        }
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::timeout;
    }

    }  // namespace

    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, osal::tick_t timeout_ticks) noexcept
    {
        return tx_event_wait_impl(handle, wait_bits, actual_bits, clear_on_exit, false, timeout_ticks);
    }

    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, osal::tick_t timeout_ticks) noexcept
    {
        return tx_event_wait_impl(handle, wait_bits, actual_bits, clear_on_exit, true, timeout_ticks);
    }

    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);  // tx_event_flags_set is ISR-safe
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not supported on ThreadX
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
