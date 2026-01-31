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
#include <cassert>
#include <cstring>

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
static TX_THREAD            tx_threads[OSAL_TX_MAX_THREADS];
static bool                 tx_thread_used[OSAL_TX_MAX_THREADS];
static TX_MUTEX             tx_mutexes[OSAL_TX_MAX_MUTEXES];
static bool                 tx_mutex_used[OSAL_TX_MAX_MUTEXES];
static TX_SEMAPHORE         tx_sems[OSAL_TX_MAX_SEMS];
static bool                 tx_sem_used[OSAL_TX_MAX_SEMS];
static TX_QUEUE             tx_queues[OSAL_TX_MAX_QUEUES];
static bool                 tx_queue_used[OSAL_TX_MAX_QUEUES];
static TX_TIMER             tx_timers[OSAL_TX_MAX_TIMERS];
static bool                 tx_timer_used[OSAL_TX_MAX_TIMERS];
static TX_EVENT_FLAGS_GROUP tx_evtgrps[OSAL_TX_MAX_EVTGRPS];
static bool                 tx_evtgrp_used[OSAL_TX_MAX_EVTGRPS];

template<typename T, std::size_t N>
static T* pool_acquire(T (&pool)[N], bool (&used)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!used[i])
        {
            used[i] = true;
            return &pool[i];
        }
    }
    return nullptr;
}

template<typename T, std::size_t N>
static void pool_release(T (&pool)[N], bool (&used)[N], T* p) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            used[i] = false;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// @brief Map OSAL priority [0=lowest, 255=highest] to ThreadX priority [0=highest, 31=lowest].
static constexpr UINT osal_to_tx_priority(osal::priority_t p) noexcept
{
    const UINT tx_max = TX_MAX_PRIORITIES - 1U;
    return tx_max - static_cast<UINT>((static_cast<std::uint32_t>(p) * static_cast<std::uint32_t>(tx_max)) /
                                      static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
}

/// @brief Map OSAL tick count to ThreadX wait option.
static constexpr ULONG to_tx_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return TX_WAIT_FOREVER;
    }
    if (t == osal::NO_WAIT)
    {
        return TX_NO_WAIT;
    }
    return static_cast<ULONG>(t);
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
    osal_timer_callback_t fn;
    void*                 arg;
    ULONG                 period;  ///< 0 = one-shot
    TX_TIMER*             timer;
};
static tx_timer_ctx tx_timer_ctxs[OSAL_TX_MAX_TIMERS];

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    /// @brief Return elapsed milliseconds from the ThreadX tick counter.
    /// @return Monotonic time in milliseconds (derived from `tx_time_get()`).
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(tx_time_get()) * static_cast<std::int64_t>(1000U / TX_TIMER_TICKS_PER_SECOND);
    }

    /// @brief Return system time — delegates to `osal_clock_monotonic_ms` on ThreadX.
    /// @return Milliseconds since boot.
    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    /// @brief Return the raw ThreadX tick counter.
    /// @return `tx_time_get()` cast to `osal::tick_t`.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(tx_time_get());
    }

    /// @brief Return the tick period in microseconds.
    /// @return 1 000 000 / `TX_TIMER_TICKS_PER_SECOND`.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1'000'000U / TX_TIMER_TICKS_PER_SECOND;
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    /// @brief Create and auto-start a ThreadX thread via `tx_thread_create`.
    /// @param handle      Output handle.
    /// @param entry       Thread entry function.
    /// @param arg         Argument passed to @p entry.
    /// @param priority    OSAL priority (converted via `osal_to_tx_priority`).
    /// @param stack       Caller-supplied stack buffer.
    /// @param stack_bytes Stack buffer size in bytes.
    /// @param name        Optional thread name.
    /// @return `osal::ok()` on success, `out_of_resources` if pool or `tx_thread_create` fails.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr && entry != nullptr && stack != nullptr);
        TX_THREAD* t = pool_acquire(tx_threads, tx_thread_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        const UINT prio = osal_to_tx_priority(priority);
        const UINT rc   = tx_thread_create(t, const_cast<CHAR*>(name != nullptr ? name : "osal"),
                                           reinterpret_cast<void (*)(ULONG)>(reinterpret_cast<void*>(entry)),
                                           reinterpret_cast<ULONG>(arg), stack, static_cast<ULONG>(stack_bytes), prio,
                                           prio, TX_NO_TIME_SLICE, TX_AUTO_START);

        if (rc != TX_SUCCESS)
        {
            pool_release(tx_threads, tx_thread_used, t);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    /// @brief Terminate and delete a ThreadX thread (no native join).
    /// @param handle Thread handle.
    /// @return `osal::ok()` on success, `not_initialized` if @p handle is null.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout_ticks*/) noexcept
    {
        // ThreadX does not have a native join.  Terminate + delete.
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        TX_THREAD* t = TX_THREAD_PTR(handle);
        tx_thread_terminate(t);
        tx_thread_delete(t);
        pool_release(tx_threads, tx_thread_used, t);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Release the thread slot; the thread object remains until explicitly deleted.
    /// @param handle Thread handle.
    /// @return `osal::ok()` on success, `not_initialized` if @p handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        // In ThreadX there is no "detach" concept — the thread object
        // lives until explicitly deleted.  Mark as released.
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        pool_release(tx_threads, tx_thread_used, TX_THREAD_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change thread priority via `tx_thread_priority_change`.
    /// @param handle   Thread handle.
    /// @param priority New OSAL priority.
    /// @return `osal::ok()` on success, `unknown` on kernel error.
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

    /// @brief CPU affinity — not supported on ThreadX.
    /// @return `not_supported` always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Thread suspend — not supported via this OSAL backend.
    /// @return `not_supported` always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Thread resume — not supported via this OSAL backend.
    /// @return `not_supported` always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Voluntarily yield via `tx_thread_relinquish`.
    void osal_thread_yield() noexcept
    {
        tx_thread_relinquish();
    }

    /// @brief Sleep for @p ms milliseconds via `tx_thread_sleep`.
    /// @param ms Milliseconds to sleep (converted to ThreadX ticks).
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        const ULONG ticks = (static_cast<ULONG>(ms) * TX_TIMER_TICKS_PER_SECOND) / 1000U;
        tx_thread_sleep(ticks == 0U ? 1U : ticks);
    }

    // ---------------------------------------------------------------------------
    // Mutex (TX_MUTEX with priority inheritance)
    // ---------------------------------------------------------------------------

    /// @brief Create a `TX_MUTEX` with `TX_INHERIT` (priority inheritance + recursive).
    /// @param handle Output handle.
    /// @return `osal::ok()` on success, `out_of_resources` on failure.
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

    /// @brief Delete a `TX_MUTEX` via `tx_mutex_delete`.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` always.
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

    /// @brief Acquire the mutex via `tx_mutex_get`.
    /// @param handle         Mutex handle.
    /// @param timeout_ticks  OSAL tick timeout.
    /// @return `osal::ok()` on success, `timeout` on expiry.
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

    /// @brief Try to acquire the mutex without blocking.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` if acquired, `timeout` otherwise.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release the mutex via `tx_mutex_put`.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` on success, `not_owner` if the calling thread does not own it.
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

    /// @brief Create a `TX_SEMAPHORE` via `tx_semaphore_create`.
    /// @param handle        Output handle.
    /// @param initial_count Starting count.
    /// @return `osal::ok()` on success, `out_of_resources` on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
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
        handle->native = static_cast<void*>(s);
        return osal::ok();
    }

    /// @brief Delete a `TX_SEMAPHORE` via `tx_semaphore_delete`.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_semaphore_delete(TX_SEM_PTR(handle));
        pool_release(tx_sems, tx_sem_used, TX_SEM_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment the semaphore via `tx_semaphore_put`.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        tx_semaphore_put(TX_SEM_PTR(handle));
        return osal::ok();
    }

    /// @brief Increment from ISR context (`tx_semaphore_put` is ISR-safe).
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` on success.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // tx_semaphore_put is ISR-safe
    }

    /// @brief Decrement the semaphore via `tx_semaphore_get`.
    /// @param handle         Semaphore handle.
    /// @param timeout_ticks  OSAL tick timeout.
    /// @return `osal::ok()` on success, `timeout` on expiry.
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

    /// @brief Try to decrement without blocking.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` if decremented, `timeout` otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (TX_QUEUE — works with 32-bit word items)
    // ---------------------------------------------------------------------------

    /// @brief Create a `TX_QUEUE` backed by @p buffer.
    /// @param handle    Output handle.
    /// @param buffer    Caller-supplied backing memory.
    /// @param item_size Message size in bytes (rounded up to 32-bit words).
    /// @param capacity  Maximum number of messages.
    /// @return `osal::ok()` on success, `out_of_resources` on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buffer, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        TX_QUEUE* q = pool_acquire(tx_queues, tx_queue_used);
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        // ThreadX message size is in 32-bit words.
        const UINT msg_size_words = static_cast<UINT>((item_size + sizeof(ULONG) - 1U) / sizeof(ULONG));

        const UINT rc = tx_queue_create(q, const_cast<CHAR*>("q"), msg_size_words, buffer,
                                        static_cast<ULONG>(capacity * msg_size_words * sizeof(ULONG)));

        if (rc != TX_SUCCESS)
        {
            pool_release(tx_queues, tx_queue_used, q);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(q);
        return osal::ok();
    }

    /// @brief Delete a `TX_QUEUE` and release the pool slot.
    /// @param handle Queue handle.
    /// @return `osal::ok()` always.
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

    /// @brief Send a message via `tx_queue_send`.
    /// @param handle         Queue handle.
    /// @param item           Message pointer.
    /// @param timeout_ticks  OSAL tick timeout.
    /// @return `osal::ok()` on success, `timeout` if full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = tx_queue_send(TX_QUEUE_PTR(handle), const_cast<void*>(item), to_tx_ticks(timeout_ticks));
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Send from ISR context (no-wait).
    /// @param handle Queue handle.
    /// @param item   Message pointer.
    /// @return `osal::ok()` on success.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive a message via `tx_queue_receive`.
    /// @param handle         Queue handle.
    /// @param item           Destination buffer.
    /// @param timeout_ticks  OSAL tick timeout.
    /// @return `osal::ok()` on success, `timeout` if empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = tx_queue_receive(TX_QUEUE_PTR(handle), item, to_tx_ticks(timeout_ticks));
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Receive from ISR context (no-wait).
    /// @param handle Queue handle.
    /// @param item   Destination buffer.
    /// @return `osal::ok()` on success.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the front of the queue — not supported on ThreadX.
    /// @return `not_supported` always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* item,
                                 osal::tick_t /*timeout_ticks*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = tx_queue_front_send(TX_QUEUE_PTR(handle), item, TX_NO_WAIT);
        // This actually re-inserts at front — real peek requires custom dequeue.
        // Substitute: call front send to read, then re-insert.
        (void)rc;
        return osal::error_code::not_supported;
    }

    /// @brief Query the number of messages currently enqueued.
    /// @param handle Queue handle (const).
    /// @return Enqueued message count.
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
        TX_THREAD* next      = nullptr;
        tx_queue_info_get(const_cast<TX_QUEUE*>(TX_QUEUE_PTR(handle)), nullptr, &enqueued, &available, &suspended,
                          &first, &next);
        return static_cast<std::size_t>(enqueued);
    }

    /// @brief Query the number of available message slots.
    /// @param handle Queue handle (const).
    /// @return Free slot count.
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
        TX_THREAD* next      = nullptr;
        tx_queue_info_get(const_cast<TX_QUEUE*>(TX_QUEUE_PTR(handle)), nullptr, &enqueued, &available, &suspended,
                          &first, &next);
        return static_cast<std::size_t>(available);
    }

    // ---------------------------------------------------------------------------
    // Timer (TX_TIMER)
    // ---------------------------------------------------------------------------

    static void tx_timer_expiry(ULONG ctx_idx) noexcept
    {
        auto& ctx = tx_timer_ctxs[ctx_idx];
        if (ctx.fn != nullptr)
        {
            ctx.fn(ctx.arg);
        }
        // Periodic re-arm is handled by tx_timer activating with reschedule_ticks.
    }

    /// @brief Create a `TX_TIMER` wrapping @p callback.
    /// @param handle       Output handle.
    /// @param name         Optional timer name.
    /// @param callback     Function invoked on expiry.
    /// @param arg          Argument passed to @p callback.
    /// @param period_ticks Period in OSAL ticks.
    /// @param auto_reload  `true` for periodic; `false` for one-shot.
    /// @return `osal::ok()` on success, `out_of_resources` if pool or context table is full.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        TX_TIMER* t = pool_acquire(tx_timers, tx_timer_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        // Find a context slot.
        ULONG ctx_idx = OSAL_TX_MAX_TIMERS;
        for (ULONG i = 0; i < OSAL_TX_MAX_TIMERS; ++i)
        {
            if (tx_timer_ctxs[i].fn == nullptr)
            {
                ctx_idx = i;
                break;
            }
        }
        if (ctx_idx == OSAL_TX_MAX_TIMERS)
        {
            pool_release(tx_timers, tx_timer_used, t);
            return osal::error_code::out_of_resources;
        }

        tx_timer_ctxs[ctx_idx].fn     = callback;
        tx_timer_ctxs[ctx_idx].arg    = arg;
        tx_timer_ctxs[ctx_idx].period = static_cast<ULONG>(period_ticks);
        tx_timer_ctxs[ctx_idx].timer  = t;

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

    /// @brief Delete and release a `TX_TIMER`.
    /// @param handle Timer handle.
    /// @return `osal::ok()` always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_timer_delete(TX_TIMER_PTR(handle));
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

    /// @brief Arm a timer via `tx_timer_activate`.
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = tx_timer_activate(TX_TIMER_PTR(handle));
        return (rc == TX_SUCCESS || rc == TX_ACTIVATE_ERROR) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Disarm a timer via `tx_timer_deactivate`.
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        tx_timer_deactivate(TX_TIMER_PTR(handle));
        return osal::ok();
    }

    /// @brief Stop and re-arm a timer.
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success.
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Change the timer period via `tx_timer_change`.
    /// @param handle           Timer handle.
    /// @param new_period_ticks New period in OSAL ticks.
    /// @return `osal::ok()` on success.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        tx_timer_deactivate(TX_TIMER_PTR(handle));
        // tx_timer_info_get + tx_timer_change for ThreadX >=5.6
        const UINT rc = tx_timer_change(TX_TIMER_PTR(handle), static_cast<ULONG>(new_period_ticks),
                                        static_cast<ULONG>(new_period_ticks));
        tx_timer_activate(TX_TIMER_PTR(handle));
        return (rc == TX_SUCCESS) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Query whether a timer is active via `tx_timer_info_get`.
    /// @param handle Timer handle (const).
    /// @return `true` if the timer is active.
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

    /// @brief Create a `TX_EVENT_FLAGS_GROUP` via `tx_event_flags_create`.
    /// @param handle Output handle.
    /// @return `osal::ok()` on success, `out_of_resources` on failure.
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

    /// @brief Delete a `TX_EVENT_FLAGS_GROUP`.
    /// @param handle Event-flags handle.
    /// @return `osal::ok()` always.
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

    /// @brief Set (OR) bits via `tx_event_flags_set`.
    /// @param handle Event-flags handle.
    /// @param bits   Bit mask to OR in.
    /// @return `osal::ok()` on success.
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

    /// @brief Clear bits via `tx_event_flags_set` with `TX_AND` and inverted mask.
    /// @param handle Event-flags handle.
    /// @param bits   Bit mask to clear.
    /// @return `osal::ok()` on success.
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

    /// @brief Read the current event bits without blocking.
    /// @param handle Event-flags handle (const).
    /// @return Current bit pattern.
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

    static osal::result tx_event_wait_impl(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, bool all, osal::tick_t timeout_ticks) noexcept
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

    /// @brief Wait until any of @p wait_bits are set.
    /// @param handle         Event-flags handle.
    /// @param wait_bits      Bit mask (any bit sufficient).
    /// @param actual_bits    If non-null, receives the bit value at wakeup.
    /// @param clear_on_exit  `true` to clear matched bits before returning.
    /// @param timeout_ticks  OSAL tick timeout.
    /// @return `osal::ok()` when matched, `timeout` on expiry.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, osal::tick_t timeout_ticks) noexcept
    {
        return tx_event_wait_impl(handle, wait_bits, actual_bits, clear_on_exit, false, timeout_ticks);
    }

    /// @brief Wait until all of @p wait_bits are set.
    /// @param handle         Event-flags handle.
    /// @param wait_bits      Bit mask (all bits must be set).
    /// @param actual_bits    If non-null, receives the bit value at wakeup.
    /// @param clear_on_exit  `true` to clear matched bits before returning.
    /// @param timeout_ticks  OSAL tick timeout.
    /// @return `osal::ok()` when all matched, `timeout` on expiry.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle,
                                           osal::event_bits_t wait_bits, osal::event_bits_t* actual_bits,
                                           bool clear_on_exit, osal::tick_t timeout_ticks) noexcept
    {
        return tx_event_wait_impl(handle, wait_bits, actual_bits, clear_on_exit, true, timeout_ticks);
    }

    /// @brief Set bits from ISR context (`tx_event_flags_set` is ISR-safe).
    /// @param handle Event-flags handle.
    /// @param bits   Bit mask to OR in.
    /// @return `osal::ok()` on success.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);  // tx_event_flags_set is ISR-safe
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not supported on ThreadX
    // ---------------------------------------------------------------------------

    /// @brief Wait-set not supported on ThreadX.
    /// @return `not_supported` always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set destroy not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set add not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set remove not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait-set wait not supported.
    /// @param n Set to 0 if non-null.
    /// @return `not_supported` always.
    osal::result osal_wait_set_wait(osal::active_traits::wait_set_handle_t*, int*, std::size_t, std::size_t* n,
                                    osal::tick_t) noexcept
    {
        if (n)
            *n = 0U;
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
