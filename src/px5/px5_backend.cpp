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
#include <cassert>
#include <cstring>

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

TX_THREAD            px5_threads[OSAL_PX5_MAX_THREADS];
bool                 px5_thread_used[OSAL_PX5_MAX_THREADS];
TX_MUTEX             px5_mutexes[OSAL_PX5_MAX_MUTEXES];
bool                 px5_mutex_used[OSAL_PX5_MAX_MUTEXES];
TX_SEMAPHORE         px5_sems[OSAL_PX5_MAX_SEMS];
bool                 px5_sem_used[OSAL_PX5_MAX_SEMS];
TX_QUEUE             px5_queues[OSAL_PX5_MAX_QUEUES];
bool                 px5_queue_used[OSAL_PX5_MAX_QUEUES];
TX_TIMER             px5_timers[OSAL_PX5_MAX_TIMERS];
bool                 px5_timer_used[OSAL_PX5_MAX_TIMERS];
TX_EVENT_FLAGS_GROUP px5_evtgrps[OSAL_PX5_MAX_EVTGRPS];
bool                 px5_evtgrp_used[OSAL_PX5_MAX_EVTGRPS];
PX5_WAIT_SET         px5_wait_sets[OSAL_PX5_MAX_WAITSETS];
bool                 px5_ws_used[OSAL_PX5_MAX_WAITSETS];

template<typename T, std::size_t N>
T* pool_acquire(T (&pool)[N], bool (&used)[N]) noexcept
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
void pool_release(T (&pool)[N], bool (&used)[N], T* p) noexcept
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

// Helper macros
/// @brief Map OSAL priority [0=lowest, 255=highest] to PX5/ThreadX priority [0=highest, TX_MAX_PRIORITIES-1=lowest].
constexpr UINT osal_to_px5_priority(osal::priority_t p) noexcept
{
    const UINT top = TX_MAX_PRIORITIES - 1U;
    return top - static_cast<UINT>((static_cast<std::uint32_t>(p) * static_cast<std::uint32_t>(top)) /
                                   static_cast<std::uint32_t>(osal::PRIORITY_HIGHEST));
}

/// @brief Map OSAL tick count to PX5/ThreadX wait option.
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
};
px5_timer_ctx px5_timer_ctxs[OSAL_PX5_MAX_TIMERS];

}  // anonymous namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock — identical to ThreadX
    // ---------------------------------------------------------------------------

    /// @brief Return monotonic time in milliseconds.
    /// @return Milliseconds elapsed since an arbitrary epoch (tick counter).
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(tx_time_get()) * static_cast<std::int64_t>(1000U / TX_TIMER_TICKS_PER_SECOND);
    }

    /// @brief Return wall-clock time in milliseconds (aliased to monotonic on PX5).
    /// @return Milliseconds since an arbitrary epoch.
    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    /// @brief Return the raw PX5 tick counter.
    /// @return Current tick count.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(tx_time_get());
    }

    /// @brief Return the configured tick period in microseconds.
    /// @return Microseconds per tick (derived from TX_TIMER_TICKS_PER_SECOND).
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return static_cast<std::uint32_t>(1'000'000U / TX_TIMER_TICKS_PER_SECOND);
    }

    // ---------------------------------------------------------------------------
    // Thread (ThreadX API — identical to threadx_backend)
    // ---------------------------------------------------------------------------

    /// @brief Create and immediately start a PX5 thread.
    /// @param handle   Output handle populated with the new TX_THREAD pointer.
    /// @param entry    Thread entry function.
    /// @param arg      Opaque argument forwarded to @p entry.
    /// @param priority OSAL priority [0=lowest, 255=highest]; mapped to TX range.
    /// @param stack    Caller-supplied stack buffer.
    /// @param stack_bytes Size of @p stack in bytes.
    /// @param name     Human-readable thread name (may be nullptr).
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the pool is exhausted.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr && entry != nullptr && stack != nullptr);
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

    /// @brief Terminate a thread and release its pool slot.
    /// @param handle Thread handle to join.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        tx_thread_terminate(PX5_THREAD_PTR(handle));
        tx_thread_delete(PX5_THREAD_PTR(handle));
        pool_release(px5_threads, px5_thread_used, PX5_THREAD_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Detach a thread, releasing its pool slot without waiting.
    /// @param handle Thread handle to detach.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        pool_release(px5_threads, px5_thread_used, PX5_THREAD_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change the priority of a running thread.
    /// @param handle   Thread handle.
    /// @param priority New OSAL priority; mapped to TX range via osal_to_px5_priority().
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
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

    /// @brief Set thread CPU affinity (not supported on PX5).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t*, osal::affinity_t) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Suspend a thread (not supported on PX5).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported on PX5).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Yield the current thread's remaining time slice.
    void osal_thread_yield() noexcept
    {
        tx_thread_relinquish();
    }
    /// @brief Sleep for at least @p ms milliseconds.
    /// @param ms Delay in milliseconds; rounds up to at least one tick.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        const ULONG t = (static_cast<ULONG>(ms) * TX_TIMER_TICKS_PER_SECOND) / 1000U;
        tx_thread_sleep(t == 0U ? 1U : t);
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    /// @brief Create a PX5 mutex with priority inheritance.
    /// @param handle Output handle populated with the new TX_MUTEX pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
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

    /// @brief Destroy a mutex and release its pool slot.
    /// @param handle Mutex handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
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

    /// @brief Acquire a mutex, blocking up to @p timeout ticks.
    /// @param handle  Mutex handle.
    /// @param timeout Maximum wait in OSAL ticks (osal::WAIT_FOREVER or osal::NO_WAIT allowed).
    /// @return osal::ok() on success; osal::error_code::timeout or ::unknown on failure.
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

    /// @brief Try to acquire a mutex without blocking.
    /// @param handle Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::timeout if not immediately available.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release a previously acquired mutex.
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_owner if the caller does not own the mutex.
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

    /// @brief Create a counting semaphore.
    /// @param handle Output handle populated with the new TX_SEMAPHORE pointer.
    /// @param init   Initial count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned init,
                                       unsigned) noexcept
    {
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
        handle->native = s;
        return osal::ok();
    }

    /// @brief Destroy a semaphore and release its pool slot.
    /// @param handle Semaphore handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_semaphore_delete(PX5_SEM_PTR(handle));
        pool_release(px5_sems, px5_sem_used, PX5_SEM_PTR(handle));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (post) a semaphore.
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        tx_semaphore_put(PX5_SEM_PTR(handle));
        return osal::ok();
    }

    /// @brief Increment a semaphore from an ISR context.
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);
    }

    /// @brief Decrement (wait on) a semaphore, blocking up to @p timeout ticks.
    /// @param handle  Semaphore handle.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout if the semaphore was not signalled in time.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (tx_semaphore_get(PX5_SEM_PTR(handle), to_px5_ticks(timeout)) == TX_SUCCESS) ? osal::ok()
                                                                                            : osal::error_code::timeout;
    }

    /// @brief Try to decrement a semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if decremented; osal::error_code::timeout if count was zero.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (identical to ThreadX)
    // ---------------------------------------------------------------------------

    /// @brief Create a fixed-size message queue backed by @p buf.
    /// @param handle  Output handle populated with the new TX_QUEUE pointer.
    /// @param buf     Caller-supplied storage buffer.
    /// @param item_sz Size in bytes of each message; rounded up to ULONG words.
    /// @param cap     Maximum number of messages the queue can hold.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* buf, std::size_t item_sz,
                                   std::size_t cap) noexcept
    {
        TX_QUEUE* q = pool_acquire(px5_queues, px5_queue_used);
        if (q == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        const UINT words = static_cast<UINT>((item_sz + sizeof(ULONG) - 1U) / sizeof(ULONG));
        if (tx_queue_create(q, const_cast<CHAR*>("q"), words, buf, static_cast<ULONG>(cap * words * sizeof(ULONG))) !=
            TX_SUCCESS)
        {
            pool_release(px5_queues, px5_queue_used, q);
            return osal::error_code::out_of_resources;
        }
        handle->native = q;
        return osal::ok();
    }

    /// @brief Destroy a queue and release its pool slot.
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
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

    /// @brief Send a message to a queue, blocking up to @p timeout ticks.
    /// @param handle  Queue handle.
    /// @param item    Pointer to the message data to copy into the queue.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout if the queue was full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (tx_queue_send(PX5_QUEUE_PTR(handle), const_cast<void*>(item), to_px5_ticks(timeout)) == TX_SUCCESS)
                   ? osal::ok()
                   : osal::error_code::timeout;
    }

    /// @brief Send a message from an ISR (non-blocking).
    /// @param handle Queue handle.
    /// @param item   Pointer to the message data.
    /// @return osal::ok() on success; osal::error_code::timeout if the queue was full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive a message from a queue, blocking up to @p timeout ticks.
    /// @param handle  Queue handle.
    /// @param item    Buffer to receive the dequeued message into.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout if the queue was empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (tx_queue_receive(PX5_QUEUE_PTR(handle), item, to_px5_ticks(timeout)) == TX_SUCCESS)
                   ? osal::ok()
                   : osal::error_code::timeout;
    }

    /// @brief Receive a message from an ISR (non-blocking).
    /// @param handle Queue handle.
    /// @param item   Buffer to receive the dequeued message into.
    /// @return osal::ok() on success; osal::error_code::timeout if the queue was empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the front message without removing it (not supported on PX5).
    /// @return osal::error_code::not_supported always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t*, void*, osal::tick_t) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Return the number of messages currently in the queue.
    /// @param handle Queue handle.
    /// @return Message count, or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ULONG      e = 0U, a = 0U, s = 0U;
        TX_THREAD *f = nullptr, *n = nullptr;
        tx_queue_info_get(const_cast<TX_QUEUE*>(PX5_QUEUE_PTR(handle)), nullptr, &e, &a, &s, &f, &n);
        return static_cast<std::size_t>(e);
    }

    /// @brief Return the number of free slots remaining in the queue.
    /// @param handle Queue handle.
    /// @return Available slots, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        ULONG      e = 0U, a = 0U, s = 0U;
        TX_THREAD *f = nullptr, *n = nullptr;
        tx_queue_info_get(const_cast<TX_QUEUE*>(PX5_QUEUE_PTR(handle)), nullptr, &e, &a, &s, &f, &n);
        return static_cast<std::size_t>(a);
    }

    // ---------------------------------------------------------------------------
    // Timer
    // ---------------------------------------------------------------------------

    namespace
    {

    /// @brief Internal PX5 timer expiry callback; dispatches to the user callback.
    /// @param idx Index into px5_timer_ctxs[] identifying which timer fired.
    void px5_timer_expiry(ULONG idx) noexcept
    {
        if (px5_timer_ctxs[idx].fn != nullptr)
        {
            px5_timer_ctxs[idx].fn(px5_timer_ctxs[idx].arg);
        }
    }

    }  // anonymous namespace

    /// @brief Create (but do not start) a PX5 timer.
    /// @param handle      Output handle populated with the new TX_TIMER pointer.
    /// @param name        Human-readable timer name (may be nullptr).
    /// @param cb          Callback invoked on each expiry.
    /// @param arg         Opaque argument forwarded to @p cb.
    /// @param period      Expiry period in OSAL ticks.
    /// @param auto_reload True to restart automatically (periodic); false for one-shot.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t cb, void* arg, osal::tick_t period, bool auto_reload) noexcept
    {
        assert(handle != nullptr && cb != nullptr);
        TX_TIMER* t = pool_acquire(px5_timers, px5_timer_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        ULONG idx = OSAL_PX5_MAX_TIMERS;
        for (ULONG i = 0; i < OSAL_PX5_MAX_TIMERS; ++i)
        {
            if (px5_timer_ctxs[i].fn == nullptr)
            {
                idx = i;
                break;
            }
        }
        if (idx == OSAL_PX5_MAX_TIMERS)
        {
            pool_release(px5_timers, px5_timer_used, t);
            return osal::error_code::out_of_resources;
        }
        px5_timer_ctxs[idx] = {cb, arg, static_cast<ULONG>(period), t};
        const ULONG resched = auto_reload ? static_cast<ULONG>(period) : 0U;
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

    /// @brief Destroy a timer, releasing its pool slot and context entry.
    /// @param handle Timer handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        tx_timer_delete(PX5_TIMER_PTR(handle));
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

    /// @brief Activate (start) a timer.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        tx_timer_activate(PX5_TIMER_PTR(handle));
        return osal::ok();
    }

    /// @brief Deactivate (stop) a timer.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        tx_timer_deactivate(PX5_TIMER_PTR(handle));
        return osal::ok();
    }

    /// @brief Stop and immediately restart a timer.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Change the period of an active or inactive timer.
    /// @param handle     Timer handle.
    /// @param new_period New period in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle, osal::tick_t new_period) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        tx_timer_deactivate(PX5_TIMER_PTR(handle));
        tx_timer_change(PX5_TIMER_PTR(handle), static_cast<ULONG>(new_period), static_cast<ULONG>(new_period));
        tx_timer_activate(PX5_TIMER_PTR(handle));
        return osal::ok();
    }

    /// @brief Query whether a timer is currently active.
    /// @param handle Timer handle.
    /// @return True if the timer is running; false otherwise.
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

    /// @brief Create a native PX5 event flags group.
    /// @param handle Output handle populated with the new TX_EVENT_FLAGS_GROUP pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
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

    /// @brief Destroy an event flags group and release its pool slot.
    /// @param handle Event flags handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
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

    /// @brief Set (OR) one or more event bits.
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to set.
    /// @return osal::ok() on success; osal::error_code::unknown on TX failure.
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

    /// @brief Clear (AND with complement) one or more event bits.
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to clear.
    /// @return osal::ok() on success; osal::error_code::unknown on TX failure.
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

    /// @brief Read the current event flags without waiting or clearing.
    /// @param handle Event flags handle.
    /// @return Current bit mask; 0 if the handle is invalid.
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

    /// @brief Internal helper: wait for event flags with configurable AND/OR and clear-on-exit semantics.
    /// @param h       Event flags handle.
    /// @param bits    Bit mask to wait for.
    /// @param actual  Optional output: bits that were set at wakeup.
    /// @param coe     Clear-on-exit: clear matched bits after wakeup when true.
    /// @param all     True = wait for ALL bits; false = wait for ANY bit.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout on expiry.
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

    /// @brief Wait until any of the specified event bits are set.
    /// @param h       Event flags handle.
    /// @param bits    Bit mask: wake when any bit in this mask becomes set.
    /// @param actual  Optional output: full current bits at wakeup.
    /// @param coe     Clear matched bits after wakeup when true.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout on expiry.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* h, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool coe, osal::tick_t timeout) noexcept
    {
        return px5_evt_wait(h, bits, actual, coe, false, timeout);
    }

    /// @brief Wait until all of the specified event bits are set simultaneously.
    /// @param h       Event flags handle.
    /// @param bits    Bit mask: wake when every bit in this mask is set.
    /// @param actual  Optional output: full current bits at wakeup.
    /// @param coe     Clear matched bits after wakeup when true.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout on expiry.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* h, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool coe, osal::tick_t timeout) noexcept
    {
        return px5_evt_wait(h, bits, actual, coe, true, timeout);
    }

    /// @brief Set event bits from an ISR context.
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to set.
    /// @return osal::ok() on success; osal::error_code::unknown on TX failure.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);
    }

    // ---------------------------------------------------------------------------
    // Wait-set (native PX5_WAIT_SET)
    // ---------------------------------------------------------------------------

    /// @brief Create a native PX5 wait-set (select/poll equivalent).
    /// @param handle Output handle populated with the new PX5_WAIT_SET pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
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

    /// @brief Destroy a wait-set and release its pool slot.
    /// @param handle Wait-set handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
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

    /// @brief Register a kernel object with the wait-set.
    /// @param handle Wait-set handle.
    /// @param fd     Opaque object identifier (cast to kernel object pointer internally).
    /// @param events Event mask to monitor on the object.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::invalid_argument on failure.
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

    /// @brief Remove a previously registered object from the wait-set.
    /// @param handle Wait-set handle.
    /// @param fd     Object identifier to deregister.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::invalid_argument on failure.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t* handle, int fd) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const UINT rc = px5_wait_set_remove(PX5_WS_PTR(handle), reinterpret_cast<void*>(static_cast<uintptr_t>(fd)));
        return (rc == PX5_SUCCESS) ? osal::ok() : osal::error_code::invalid_argument;
    }

    /// @brief Block until any registered object signals, up to @p timeout_ticks.
    /// @param handle        Wait-set handle.
    /// @param fds_ready     Optional output: identifier of the first signalled object.
    /// @param n_ready       Optional output: number of signalled objects (always 0 or 1 on PX5).
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() if an object signalled; osal::error_code::timeout on expiry.
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
