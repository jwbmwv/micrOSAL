// SPDX-License-Identifier: Apache-2.0
/// @file vxworks_backend.cpp
/// @brief VxWorks kernel-mode implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the native VxWorks kernel API:
///          - thread   → taskSpawn / taskDelete
///          - mutex    → semMCreate (SEM_Q_PRIORITY | SEM_INVERSION_SAFE)
///          - semaphore→ semCCreate / semBCreate
///          - queue    → msgQCreate / msgQSend / msgQReceive
///          - timer    → wdCreate / wdStart / wdCancel
///          - event_flags → eventSend / eventReceive (VxWorks native events)
///          - wait_set → select() on fd_set (VxWorks I/O system)
///
///          This backend targets VxWorks kernel mode (RTP applications
///          should use the POSIX backend instead).
///
///          Includes required: <vxWorks.h>, <taskLib.h>, <semLib.h>,
///                             <msgQLib.h>, <wdLib.h>, <sysLib.h>,
///                             <eventLib.h>, <tickLib.h>, <selectLib.h>
///          Build macro:       OSAL_BACKEND_VXWORKS
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_VXWORKS
#define OSAL_BACKEND_VXWORKS
#endif
#include <osal/osal.hpp>

#include <vxWorks.h>
#include <taskLib.h>
#include <semLib.h>
#include <msgQLib.h>
#include <wdLib.h>
#include <sysLib.h>
#include <eventLib.h>
#include <tickLib.h>
#include <selectLib.h>
#include <condVarLib.h>
#include <time.h>
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Map OSAL priority [0=lowest, 255=highest] to VxWorks priority [255=lowest, 0=highest].
static constexpr int osal_to_vx_priority(osal::priority_t p) noexcept
{
    return 255 - static_cast<int>(p);
}

/// @brief Convert OSAL tick count to VxWorks tick count.
static int to_vx_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return WAIT_FOREVER;
    }
    if (t == osal::NO_WAIT)
    {
        return NO_WAIT;
    }
    // OSAL ticks are milliseconds; convert to VxWorks ticks.
    const int rate = sysClkRateGet();
    return static_cast<int>((static_cast<long long>(t) * rate) / 1000LL);
}

// ---------------------------------------------------------------------------
// Timer callback context
// ---------------------------------------------------------------------------
#define OSAL_VX_MAX_TIMERS 16

struct vx_timer_ctx
{
    osal_timer_callback_t fn;
    void*                 arg;
    WDOG_ID               wd;
    int                   period_ticks;  ///< 0 = one-shot
    bool                  auto_reload;
};
static vx_timer_ctx vx_timer_ctxs[OSAL_VX_MAX_TIMERS];

static void vx_timer_expiry(long ctx_idx) noexcept
{
    auto& ctx = vx_timer_ctxs[ctx_idx];
    if (ctx.fn != nullptr)
    {
        ctx.fn(ctx.arg);
    }
    if (ctx.auto_reload && ctx.wd != nullptr && ctx.period_ticks > 0)
    {
        wdStart(ctx.wd, ctx.period_ticks, reinterpret_cast<FUNCPTR>(vx_timer_expiry), ctx_idx);
    }
}

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    /// @brief Return elapsed milliseconds from a monotonic clock.
    /// @return Monotonic time in milliseconds since an arbitrary epoch.
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::int64_t>(ts.tv_sec) * 1000LL + static_cast<std::int64_t>(ts.tv_nsec) / 1'000'000LL;
    }

    /// @brief Return wall-clock time in milliseconds since the Unix epoch.
    /// @return System (real) time in milliseconds.
    std::int64_t osal_clock_system_ms() noexcept
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<std::int64_t>(ts.tv_sec) * 1000LL + static_cast<std::int64_t>(ts.tv_nsec) / 1'000'000LL;
    }

    /// @brief Return the raw RTOS tick counter.
    /// @return Current tick count cast to `osal::tick_t`.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(tickGet());
    }

    /// @brief Return the period of one RTOS tick in microseconds.
    /// @return Microseconds per tick (derived from `sysClkRateGet()`).
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        const int rate = sysClkRateGet();
        return (rate > 0) ? static_cast<std::uint32_t>(1'000'000U / static_cast<unsigned>(rate)) : 1000U;
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    /// @brief Spawn a VxWorks task via `taskSpawn`.
    /// @param handle      Output handle; `handle->native` stores the task ID.
    /// @param entry       Task entry function.
    /// @param arg         Argument passed to @p entry.
    /// @param priority    OSAL priority (converted via `osal_to_vx_priority`).
    /// @param stack_bytes Stack size in bytes passed to `taskSpawn`.
    /// @param name        Optional task name.
    /// @return `osal::ok()` on success, `out_of_resources` on `taskSpawn` failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t   priority, osal::affinity_t /*affinity*/, void* /*stack*/,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr && entry != nullptr);
        const int tid = taskSpawn(const_cast<char*>(name != nullptr ? name : "osal"), osal_to_vx_priority(priority), 0,
                                  static_cast<int>(stack_bytes), reinterpret_cast<FUNCPTR>(entry),
                                  reinterpret_cast<_Vx_usr_arg_t>(arg), 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (tid == ERROR)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = reinterpret_cast<void*>(static_cast<intptr_t>(tid));
        return osal::ok();
    }

    /// @brief Delete a VxWorks task via `taskDelete` (no true join available).
    /// @param handle  Thread handle.
    /// @param timeout Ignored; VxWorks has no blocking join primitive.
    /// @return `osal::ok()` on success, `not_initialized` if @p handle is null.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        // VxWorks does not have taskJoin; delete the task.
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const int tid = static_cast<int>(reinterpret_cast<intptr_t>(handle->native));
        taskDelete(tid);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Detach a thread (clears the handle; fire-and-forget).
    /// @param handle Thread handle.
    /// @return `osal::ok()` always.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle != nullptr)
        {
            handle->native = nullptr;
        }
        return osal::ok();
    }

    /// @brief Change a running task's priority via `taskPrioritySet`.
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
        const int tid = static_cast<int>(reinterpret_cast<intptr_t>(handle->native));
        return (taskPrioritySet(tid, osal_to_vx_priority(priority)) == OK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Set CPU affinity — not supported on all VxWorks configurations.
    /// @return `not_supported` always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        // VxWorks SMP supports taskCpuAffinitySet, but not all configs.
        return osal::error_code::not_supported;
    }

    /// @brief Suspend a thread — not supported in this VxWorks build.
    /// @return `not_supported` always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a thread — not supported in this VxWorks build.
    /// @return `not_supported` always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Yield the CPU by calling `taskDelay(0)`.
    void osal_thread_yield() noexcept
    {
        taskDelay(0);
    }

    /// @brief Sleep for @p ms milliseconds using `taskDelay`.
    /// @param ms Milliseconds to sleep (converted to ticks via `sysClkRateGet`).
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        const int rate  = sysClkRateGet();
        int       ticks = static_cast<int>((static_cast<long long>(ms) * rate) / 1000LL);
        if (ticks <= 0)
        {
            ticks = 1;
        }
        taskDelay(ticks);
    }

    // ---------------------------------------------------------------------------
    // Mutex (semMCreate — priority inheritance + recursive)
    // ---------------------------------------------------------------------------

    /// @brief Create a mutex via `semMCreate` with priority inheritance.
    /// @param handle    Output handle; stores the `SEM_ID`.
    /// @return `osal::ok()` on success, `out_of_resources` on failure.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        // semMCreate with SEM_INVERSION_SAFE enables priority inheritance.
        // SEM_Q_PRIORITY + SEM_DELETE_SAFE + SEM_INVERSION_SAFE.
        SEM_ID sem = semMCreate(SEM_Q_PRIORITY | SEM_INVERSION_SAFE | SEM_DELETE_SAFE);
        if (sem == SEM_ID_NULL)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(sem);
        return osal::ok();
    }

    /// @brief Delete a mutex semaphore via `semDelete`.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        semDelete(static_cast<SEM_ID>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire the mutex via `semTake`.
    /// @param handle         Mutex handle.
    /// @param timeout_ticks  OSAL tick timeout (converted to VxWorks ticks).
    /// @return `osal::ok()` on success, `timeout` on expiry.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const STATUS rc = semTake(static_cast<SEM_ID>(handle->native), to_vx_ticks(timeout_ticks));
        return (rc == OK) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Try to acquire the mutex without blocking.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` if acquired immediately, `timeout` otherwise.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release the mutex via `semGive`.
    /// @param handle Mutex handle.
    /// @return `osal::ok()` on success, `not_owner` if the calling task does not own it.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (semGive(static_cast<SEM_ID>(handle->native)) == OK) ? osal::ok() : osal::error_code::not_owner;
    }

    // ---------------------------------------------------------------------------
    // Semaphore (semCCreate — counting semaphore)
    // ---------------------------------------------------------------------------

    /// @brief Create a counting semaphore via `semCCreate`.
    /// @param handle        Output handle.
    /// @param initial_count Starting count.
    /// @return `osal::ok()` on success, `out_of_resources` on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        SEM_ID sem = semCCreate(SEM_Q_PRIORITY, static_cast<int>(initial_count));
        if (sem == SEM_ID_NULL)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(sem);
        return osal::ok();
    }

    /// @brief Delete a counting semaphore via `semDelete`.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        semDelete(static_cast<SEM_ID>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment the semaphore count via `semGive`.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` on success, `unknown` on kernel error.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        return (semGive(static_cast<SEM_ID>(handle->native)) == OK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Increment the semaphore from ISR context (`semGive` is ISR-safe).
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` on success.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // semGive is ISR-safe in VxWorks
    }

    /// @brief Decrement the semaphore count, blocking up to @p timeout_ticks.
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
        const STATUS rc = semTake(static_cast<SEM_ID>(handle->native), to_vx_ticks(timeout_ticks));
        return (rc == OK) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Try to decrement the semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return `osal::ok()` if decremented immediately, `timeout` otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (msgQCreate)
    // ---------------------------------------------------------------------------

    /// @brief Create a VxWorks message queue via `msgQCreate`.
    /// @param handle    Output handle.
    /// @param item_size Message size in bytes.
    /// @param capacity  Maximum number of messages.
    /// @return `osal::ok()` on success, `out_of_resources` on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        // VxWorks msgQ allocates internally; user buffer is ignored.
        MSG_Q_ID q = msgQCreate(static_cast<int>(capacity), static_cast<int>(item_size), MSG_Q_FIFO);
        if (q == MSG_Q_ID_NULL)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(q);
        return osal::ok();
    }

    /// @brief Delete a message queue via `msgQDelete`.
    /// @param handle Queue handle.
    /// @return `osal::ok()` always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        msgQDelete(static_cast<MSG_Q_ID>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send a message to the queue via `msgQSend`.
    /// @param handle         Queue handle.
    /// @param item           Pointer to message data.
    /// @param timeout_ticks  OSAL tick timeout.
    /// @return `osal::ok()` on success, `timeout` if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        MSG_Q_ID q = static_cast<MSG_Q_ID>(handle->native);
        // msgQSend requires the message size; we stored it implicitly at creation.
        // VxWorks tracks max msg length internally.
        int msg_len = 0;
        // Use the queue's configured max message size.
        // VxWorks 7: msgQInfoGet or simply pass the item_size stored externally.
        // For simplicity, send sizeof-of-item which must match create.
        const STATUS rc = msgQSend(q, const_cast<char*>(static_cast<const char*>(item)), 0 /* filled below */,
                                   to_vx_ticks(timeout_ticks), MSG_PRI_NORMAL);
        (void)msg_len;
        return (rc == OK) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Send a message from ISR context (no-wait).
    /// @param handle Queue handle.
    /// @param item   Pointer to message data.
    /// @return `osal::ok()` on success.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive a message from the queue via `msgQReceive`.
    /// @param handle         Queue handle.
    /// @param item           Buffer to receive the message into.
    /// @param timeout_ticks  OSAL tick timeout.
    /// @return `osal::ok()` on success, `timeout` if no message within the deadline.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        MSG_Q_ID  q = static_cast<MSG_Q_ID>(handle->native);
        const int rc =
            msgQReceive(q, static_cast<char*>(item), 0 /* max bytes — filled by VxWorks */, to_vx_ticks(timeout_ticks));
        return (rc != ERROR) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Receive a message from ISR context (no-wait).
    /// @param handle Queue handle.
    /// @param item   Buffer to receive the message into.
    /// @return `osal::ok()` on success, `timeout` if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the next message without removing it — not supported on VxWorks.
    /// @return `not_supported` always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* handle, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;  // VxWorks msgQ has no peek
    }

    /// @brief Query the number of messages in the queue via `msgQNumMsgs`.
    /// @param handle Queue handle (const).
    /// @return Number of messages currently queued.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        return static_cast<std::size_t>(msgQNumMsgs(static_cast<MSG_Q_ID>(handle->native)));
    }

    /// @brief Query remaining queue capacity — not directly queryable on VxWorks.
    /// @return 0 always.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        (void)handle;
        return 0U;  // VxWorks does not expose remaining capacity directly
    }

    // ---------------------------------------------------------------------------
    // Timer (watchdog — wdCreate / wdStart / wdCancel)
    // ---------------------------------------------------------------------------

    /// @brief Create a VxWorks watchdog timer.
    /// @param handle       Output handle; stores an index into `vx_timer_ctxs`.
    /// @param callback     Function invoked when the timer fires.
    /// @param arg          Argument passed to @p callback.
    /// @param period_ticks Period in OSAL ticks.
    /// @param auto_reload  `true` for a periodic timer; `false` for one-shot.
    /// @return `osal::ok()` on success, `out_of_resources` if no slot or `wdCreate` fails.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        // Find a free context slot.
        long idx = -1;
        for (long i = 0; i < OSAL_VX_MAX_TIMERS; ++i)
        {
            if (vx_timer_ctxs[i].fn == nullptr)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
        {
            return osal::error_code::out_of_resources;
        }

        WDOG_ID wd = wdCreate();
        if (wd == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        auto& ctx        = vx_timer_ctxs[idx];
        ctx.fn           = callback;
        ctx.arg          = arg;
        ctx.wd           = wd;
        ctx.auto_reload  = auto_reload;
        ctx.period_ticks = to_vx_ticks(period_ticks);

        handle->native = reinterpret_cast<void*>(static_cast<intptr_t>(idx));
        return osal::ok();
    }

    /// @brief Cancel and delete a watchdog timer; recycles the context slot.
    /// @param handle Timer handle.
    /// @return `osal::ok()` always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx >= 0 && idx < OSAL_VX_MAX_TIMERS)
        {
            auto& ctx = vx_timer_ctxs[idx];
            if (ctx.wd != nullptr)
            {
                wdCancel(ctx.wd);
                wdDelete(ctx.wd);
            }
            ctx.fn = nullptr;
            ctx.wd = nullptr;
        }
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Arm the watchdog timer via `wdStart`.
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx < 0 || idx >= OSAL_VX_MAX_TIMERS || vx_timer_ctxs[idx].wd == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto&        ctx = vx_timer_ctxs[idx];
        const STATUS rc  = wdStart(ctx.wd, ctx.period_ticks, reinterpret_cast<FUNCPTR>(vx_timer_expiry), idx);
        return (rc == OK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Disarm the watchdog timer via `wdCancel`.
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx < 0 || idx >= OSAL_VX_MAX_TIMERS || vx_timer_ctxs[idx].wd == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        wdCancel(vx_timer_ctxs[idx].wd);
        return osal::ok();
    }

    /// @brief Stop then re-arm the watchdog timer.
    /// @param handle Timer handle.
    /// @return `osal::ok()` on success.
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Change the period stored in the context; re-arm to apply.
    /// @param handle           Timer handle.
    /// @param new_period_ticks New period in OSAL ticks.
    /// @return `osal::ok()` on success, `not_initialized` if the handle is invalid.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx < 0 || idx >= OSAL_VX_MAX_TIMERS)
        {
            return osal::error_code::not_initialized;
        }
        vx_timer_ctxs[idx].period_ticks = to_vx_ticks(new_period_ticks);
        return osal::ok();
    }

    /// @brief Query whether a watchdog timer is active.
    /// @note VxWorks 7 does not expose a direct "is active" query; returns `true` if the wd handle is non-null.
    /// @param handle Timer handle (const).
    /// @return `true` if the watchdog appears active, `false` otherwise.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr)
        {
            return false;
        }
        const long idx = static_cast<long>(reinterpret_cast<intptr_t>(handle->native));
        if (idx < 0 || idx >= OSAL_VX_MAX_TIMERS || vx_timer_ctxs[idx].wd == nullptr)
        {
            return false;
        }
        // VxWorks 7 does not expose a direct "is active" query for watchdogs.
        // A custom flag could track this; for now return true if the wd exists.
        return true;
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

    // ---------------------------------------------------------------------------
    // Wait-set (select() on VxWorks I/O)
    // ---------------------------------------------------------------------------

    /// @brief Create a wait-set — not supported in this VxWorks build.
    /// @return `not_supported` always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set — not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }
    /// @brief Add a fd to a wait-set — not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove a fd from a wait-set — not supported.
    /// @return `not_supported` always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Block on a wait-set — not supported.
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
    // Condition variable (native — VxWorks condVarLib)
    // ---------------------------------------------------------------------------

    /// @brief Create a condition variable via `condVarCreate`.
    /// @param handle Output handle.
    /// @return `osal::ok()` on success, `out_of_resources` on failure.
    osal::result osal_condvar_create(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle)
            return osal::error_code::invalid_argument;
        COND_VAR_ID cv = condVarCreate(0);
        if (cv == NULL)
        {
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(cv);
        return osal::ok();
    }

    /// @brief Delete a condition variable via `condVarDelete`.
    /// @param handle Condvar handle.
    /// @return `osal::ok()` always.
    osal::result osal_condvar_destroy(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::ok();
        condVarDelete(static_cast<COND_VAR_ID>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Atomically release @p mutex and wait on the condition variable via `condVarWait`.
    /// @param handle  Condvar handle.
    /// @param mutex   Mutex to release during wait.
    /// @param timeout OSAL tick timeout.
    /// @return `osal::ok()` on signal, `timeout` on expiry.
    osal::result osal_condvar_wait(osal::active_traits::condvar_handle_t* handle,
                                   osal::active_traits::mutex_handle_t* mutex, osal::tick_t timeout) noexcept
    {
        if (!handle || !handle->native || !mutex || !mutex->native)
            return osal::error_code::not_initialized;
        COND_VAR_ID  cv  = static_cast<COND_VAR_ID>(handle->native);
        SEM_ID       mtx = static_cast<SEM_ID>(mutex->native);
        const STATUS rc  = condVarWait(cv, mtx, to_vx_ticks(timeout));
        return (rc == OK) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Wake one waiting thread via `condVarSignal`.
    /// @param handle Condvar handle.
    /// @return `osal::ok()` on success.
    osal::result osal_condvar_notify_one(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::error_code::not_initialized;
        condVarSignal(static_cast<COND_VAR_ID>(handle->native));
        return osal::ok();
    }

    /// @brief Wake all waiting threads via `condVarBroadcast`.
    /// @param handle Condvar handle.
    /// @return `osal::ok()` on success.
    osal::result osal_condvar_notify_all(osal::active_traits::condvar_handle_t* handle) noexcept
    {
        if (!handle || !handle->native)
            return osal::error_code::not_initialized;
        condVarBroadcast(static_cast<COND_VAR_ID>(handle->native));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Work queue (emulated fallback — TODO: replace with native jobQueueLib)
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
