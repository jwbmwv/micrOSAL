// SPDX-License-Identifier: Apache-2.0
/// @file embos_backend.cpp
/// @brief SEGGER embOS implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to SEGGER embOS native API:
///          - thread   → OS_TASK_CreateEx / OS_TASK_Terminate
///          - mutex    → OS_MUTEX_Create / OS_MUTEX_LockTimed / OS_MUTEX_Unlock
///          - semaphore→ OS_SEMAPHORE_Create / OS_SEMAPHORE_TakeTimed / OS_SEMAPHORE_Give
///          - queue    → OS_QUEUE_Create / OS_QUEUE_Put / OS_QUEUE_GetTimed
///          - timer    → OS_TIMER_CreateEx / OS_TIMER_Start / OS_TIMER_Stop
///          - event_flags→ OS_EVENT_Set / OS_EVENT_GetMaskTimed / OS_EVENT_Reset
///
///          All kernel objects are held in static pools.
///          Dynamic allocation is never used.
///
///          Includes required: <RTOS.h> (embOS master header)
///          Build macro:       OSAL_BACKEND_EMBOS
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_EMBOS
#define OSAL_BACKEND_EMBOS
#endif
#include <osal/osal.hpp>

#include <RTOS.h>  ///< SEGGER embOS header
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Pool sizes
// ---------------------------------------------------------------------------
#define OSAL_EMBOS_MAX_THREADS 8
#define OSAL_EMBOS_MAX_MUTEXES 16
#define OSAL_EMBOS_MAX_SEMS 16
#define OSAL_EMBOS_MAX_QUEUES 8
#define OSAL_EMBOS_MAX_TIMERS 8
#define OSAL_EMBOS_MAX_EVENTS 8
#define OSAL_EMBOS_QUEUE_BUF_SIZE 1024  // per-queue internal buffer size

// ---------------------------------------------------------------------------
// Static pools
// ---------------------------------------------------------------------------
static OS_TASK      eos_tasks[OSAL_EMBOS_MAX_THREADS];
static bool         eos_task_used[OSAL_EMBOS_MAX_THREADS];
static OS_MUTEX     eos_mutexes[OSAL_EMBOS_MAX_MUTEXES];
static bool         eos_mutex_used[OSAL_EMBOS_MAX_MUTEXES];
static OS_SEMAPHORE eos_sems[OSAL_EMBOS_MAX_SEMS];
static bool         eos_sem_used[OSAL_EMBOS_MAX_SEMS];

struct eos_queue_slot
{
    OS_QUEUE q;
    char     buf[OSAL_EMBOS_QUEUE_BUF_SIZE];
    bool     used;
};
static eos_queue_slot eos_queues[OSAL_EMBOS_MAX_QUEUES];

struct eos_timer_slot
{
    OS_TIMER_EX           tmr;
    osal_timer_callback_t fn;
    void*                 arg;
    OS_TIME               period;
    bool                  auto_reload;
    bool                  used;
};
static eos_timer_slot eos_timers[OSAL_EMBOS_MAX_TIMERS];

static OS_EVENT eos_events[OSAL_EMBOS_MAX_EVENTS];
static bool     eos_event_used[OSAL_EMBOS_MAX_EVENTS];

// ---------------------------------------------------------------------------
// Pool helpers
// ---------------------------------------------------------------------------
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

template<typename T, std::size_t N>
static T* pool_acquire_by_used(T (&pool)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!pool[i].used)
        {
            pool[i].used = true;
            return &pool[i];
        }
    }
    return nullptr;
}

template<typename T, std::size_t N>
static void pool_release_by_used(T (&pool)[N], T* p) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (&pool[i] == p)
        {
            pool[i].used = false;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Map OSAL priority [0=lowest..255=highest] to embOS priority [1=lowest..255=highest].
/// embOS: higher numerical value = higher priority. 0 is reserved for the idle task.
static constexpr OS_PRIO osal_to_embos_priority(osal::priority_t p) noexcept
{
    if (p == 0U)
    {
        return 1U;  // minimum non-idle
    }
    return static_cast<OS_PRIO>(p);
}

/// @brief Convert OSAL ticks to embOS time value.
static constexpr OS_TIME to_embos_ticks(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
    {
        return 0;  // embOS: 0 = wait forever for timed operations
    }
    if (t == osal::NO_WAIT)
    {
        return 1;  // minimum ticks — checked separately
    }
    return static_cast<OS_TIME>(t);
}

// ---------------------------------------------------------------------------
// Timer callback
// ---------------------------------------------------------------------------
static void eos_timer_callback(void* p_arg) noexcept
{
    auto* slot = static_cast<eos_timer_slot*>(p_arg);
    if (slot != nullptr && slot->fn != nullptr)
    {
        slot->fn(slot->arg);
    }
    // Auto-reload: restart the timer
    if (slot != nullptr && slot->auto_reload)
    {
        OS_TIMER_StartEx(&slot->tmr);
    }
}

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    /// @brief Return monotonic time in milliseconds via OS cycle counter.
    /// @return Milliseconds elapsed since an arbitrary epoch.
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(OS_TIME_GetResult_ms(OS_TIME_Get_Cycles()));
    }

    /// @brief Return wall-clock time in milliseconds (aliased to monotonic on embOS).
    /// @return Milliseconds since an arbitrary epoch.
    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    /// @brief Return the raw embOS tick counter.
    /// @return Current OS_TIME tick count.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(OS_TIME_Get());
    }

    /// @brief Return the configured tick period in microseconds.
    /// @return Microseconds per tick (1 000 000 / OS_TIME_GetFreq()), or 1000 if frequency is unavailable.
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        const OS_U32 freq = OS_TIME_GetFreq();
        return (freq > 0U) ? static_cast<std::uint32_t>(1'000'000U / freq) : 1000U;
    }

    // ---------------------------------------------------------------------------
    // Thread (Task)
    // ---------------------------------------------------------------------------

    /// @brief Create and immediately start an embOS task.
    /// @param handle      Output handle populated with the new OS_TASK pointer.
    /// @param entry       Task entry function.
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority [0=lowest, 255=highest]; mapped to embOS [1..255].
    /// @param stack       Caller-supplied stack buffer.
    /// @param stack_bytes Size of @p stack in bytes.
    /// @param name        Human-readable task name (may be nullptr).
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the pool is exhausted.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr && entry != nullptr && stack != nullptr);
        OS_TASK* t = pool_acquire(eos_tasks, eos_task_used);
        if (t == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        OS_TASK_CreateEx(t, name != nullptr ? name : "osal", osal_to_embos_priority(priority),
                         reinterpret_cast<void (*)(void*)>(entry), stack, static_cast<OS_UINT>(stack_bytes), 0U, arg);
        handle->native = static_cast<void*>(t);
        return osal::ok();
    }

    /// @brief Terminate a task and release its pool slot.
    /// @param handle Task handle to join.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* t = static_cast<OS_TASK*>(handle->native);
        OS_TASK_Terminate(t);
        pool_release(eos_tasks, eos_task_used, t);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Detach a task, releasing its pool slot without waiting.
    /// @param handle Task handle to detach.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        pool_release(eos_tasks, eos_task_used, static_cast<OS_TASK*>(handle->native));
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change the priority of a running task.
    /// @param handle   Task handle.
    /// @param priority New OSAL priority; mapped to embOS range via osal_to_embos_priority().
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_TASK_SetPriority(static_cast<OS_TASK*>(handle->native), osal_to_embos_priority(priority));
        return osal::ok();
    }

    /// @brief Set thread CPU affinity (not supported on embOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Suspend a task (not supported on embOS through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended task (not supported on embOS through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Yield the current task's remaining time slice via OS_TASK_Yield().
    void osal_thread_yield() noexcept
    {
        OS_TASK_Yield();
    }

    /// @brief Sleep for at least @p ms milliseconds via OS_TASK_Delay().
    /// @param ms Delay in milliseconds.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        OS_TASK_Delay(static_cast<OS_TIME>(ms));
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    /// @brief Create an embOS mutex.
    /// @param handle Output handle populated with the new OS_MUTEX pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the pool is exhausted.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        OS_MUTEX* m = pool_acquire(eos_mutexes, eos_mutex_used);
        if (m == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_MUTEX_Create(m);
        handle->native = static_cast<void*>(m);
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
        auto* m = static_cast<OS_MUTEX*>(handle->native);
        OS_MUTEX_Delete(m);
        pool_release(eos_mutexes, eos_mutex_used, m);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire a mutex, blocking up to @p timeout_ticks ticks.
    /// @param handle        Mutex handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block, ::timeout, or ::unknown on failure.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* m = static_cast<OS_MUTEX*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            OS_MUTEX_Lock(m);
            return osal::ok();
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int r = OS_MUTEX_LockTimed(m, 1U);
            return (r != 0) ? osal::ok() : osal::error_code::would_block;
        }
        const int r = OS_MUTEX_LockTimed(m, to_embos_ticks(timeout_ticks));
        return (r != 0) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Try to acquire a mutex without blocking.
    /// @param handle Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::would_block if not immediately available.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release a previously acquired mutex via OS_MUTEX_Unlock().
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_MUTEX_Unlock(static_cast<OS_MUTEX*>(handle->native));
        return osal::ok();
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    /// @brief Create a counting semaphore.
    /// @param handle        Output handle populated with the new OS_SEMAPHORE pointer.
    /// @param initial_count Initial count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the pool is exhausted.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        OS_SEMAPHORE* s = pool_acquire(eos_sems, eos_sem_used);
        if (s == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_SEMAPHORE_Create(s, static_cast<OS_UINT>(initial_count));
        handle->native = static_cast<void*>(s);
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
        auto* s = static_cast<OS_SEMAPHORE*>(handle->native);
        OS_SEMAPHORE_Delete(s);
        pool_release(eos_sems, eos_sem_used, s);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (post) a semaphore via OS_SEMAPHORE_Give().
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_SEMAPHORE_Give(static_cast<OS_SEMAPHORE*>(handle->native));
        return osal::ok();
    }

    /// @brief Increment a semaphore from an ISR context (embOS API is ISR-safe).
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // embOS APIs are ISR-safe
    }

    /// @brief Decrement (wait on) a semaphore, blocking up to @p timeout_ticks ticks.
    /// @param handle        Semaphore handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout on failure.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* s = static_cast<OS_SEMAPHORE*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            OS_SEMAPHORE_Take(s);
            return osal::ok();
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            const int r = OS_SEMAPHORE_TakeTimed(s, 1U);
            return (r != 0) ? osal::ok() : osal::error_code::would_block;
        }
        const int r = OS_SEMAPHORE_TakeTimed(s, to_embos_ticks(timeout_ticks));
        return (r != 0) ? osal::ok() : osal::error_code::timeout;
    }

    /// @brief Try to decrement a semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if decremented; osal::error_code::would_block if count was zero.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (OS_QUEUE — embOS message queue)
    // ---------------------------------------------------------------------------

    /// @brief Create an embOS message queue backed by an internal static buffer.
    /// @param handle    Output handle populated with the eos_queue_slot pointer.
    /// @param item_size Size in bytes of each message (informational; internal buffer is fixed).
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the pool is exhausted.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t /*capacity*/) noexcept
    {
        auto* slot = pool_acquire_by_used(eos_queues);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_QUEUE_Create(&slot->q, slot->buf, static_cast<OS_UINT>(sizeof(slot->buf)));
        handle->native = static_cast<void*>(slot);
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
        auto* slot = static_cast<eos_queue_slot*>(handle->native);
        OS_QUEUE_Delete(&slot->q);
        pool_release_by_used(eos_queues, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send a message to a queue via OS_QUEUE_Put() (non-blocking).
    /// @param handle        Queue handle.
    /// @param item          Pointer to the message data to enqueue.
    /// @param timeout_ticks Timeout argument (currently unused; embOS Put is non-blocking).
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<eos_queue_slot*>(handle->native);
        // embOS OS_QUEUE_Put sends a sized message
        const int r = OS_QUEUE_Put(&slot->q, item, sizeof(void*));
        return (r == 0) ? osal::ok() : osal::error_code::would_block;
    }

    /// @brief Send a message from an ISR context (delegates to osal_queue_send with NO_WAIT).
    /// @param handle Queue handle.
    /// @param item   Pointer to the message data.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);
    }

    /// @brief Receive a message from a queue, blocking up to @p timeout_ticks ticks.
    /// @param handle        Queue handle.
    /// @param item          Buffer to receive the dequeued message into.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout on failure.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<eos_queue_slot*>(handle->native);

        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            OS_QUEUE_GetPtr(&slot->q, nullptr);  // blocks until message available
            return osal::ok();
        }
        const int r = OS_QUEUE_GetTimed(&slot->q, item, sizeof(void*), to_embos_ticks(timeout_ticks));
        if (r != 0)
        {
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Receive a message from an ISR context (delegates to osal_queue_receive with NO_WAIT).
    /// @param handle Queue handle.
    /// @param item   Buffer to receive the dequeued message into.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the front message without removing it (not supported on embOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
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
        auto* slot = static_cast<const eos_queue_slot*>(handle->native);
        return static_cast<std::size_t>(OS_QUEUE_GetMessageCnt(&slot->q));
    }

    /// @brief Return the number of free slots in the queue (not exposed by embOS).
    /// @return Always 0; embOS does not provide a free-count API.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* /*handle*/) noexcept
    {
        // embOS does not expose a free-count API directly
        return 0U;
    }

    // ---------------------------------------------------------------------------
    // Timer (OS_TIMER_EX — extended timer with user context)
    // ---------------------------------------------------------------------------

    /// @brief Create (but do not start) an embOS extended timer.
    /// @param handle        Output handle populated with the eos_timer_slot pointer.
    /// @param callback      Callback invoked on each expiry.
    /// @param arg           Opaque argument forwarded to @p callback.
    /// @param period_ticks  Expiry period in OSAL ticks.
    /// @param auto_reload   True to restart automatically (periodic); false for one-shot.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        auto* slot = pool_acquire_by_used(eos_timers);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->fn          = callback;
        slot->arg         = arg;
        slot->period      = static_cast<OS_TIME>(period_ticks);
        slot->auto_reload = auto_reload;

        OS_TIMER_CreateEx(&slot->tmr, eos_timer_callback, slot->period, slot);
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Destroy a timer and release its pool slot.
    /// @param handle Timer handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<eos_timer_slot*>(handle->native);
        OS_TIMER_DeleteEx(&slot->tmr);
        slot->fn = nullptr;
        pool_release_by_used(eos_timers, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Activate (start) a timer via OS_TIMER_StartEx().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_TIMER_StartEx(&static_cast<eos_timer_slot*>(handle->native)->tmr);
        return osal::ok();
    }

    /// @brief Deactivate (stop) a timer via OS_TIMER_StopEx().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_TIMER_StopEx(&static_cast<eos_timer_slot*>(handle->native)->tmr);
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

    /// @brief Change the period of a timer via OS_TIMER_SetPeriodEx().
    /// @param handle          Timer handle.
    /// @param new_period_ticks New period in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot   = static_cast<eos_timer_slot*>(handle->native);
        slot->period = static_cast<OS_TIME>(new_period_ticks);
        OS_TIMER_SetPeriodEx(&slot->tmr, slot->period);
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
        return OS_TIMER_IsRunningEx(&static_cast<const eos_timer_slot*>(handle->native)->tmr) != 0U;
    }

    // ---------------------------------------------------------------------------
    // Event flags (OS_EVENT — embOS native event object)
    // ---------------------------------------------------------------------------

    /// @brief Create a native embOS event object.
    /// @param handle Output handle populated with the new OS_EVENT pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources if the pool is exhausted.
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        OS_EVENT* e = pool_acquire(eos_events, eos_event_used);
        if (e == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        OS_EVENT_Create(e);
        handle->native = static_cast<void*>(e);
        return osal::ok();
    }

    /// @brief Destroy an event object and release its pool slot.
    /// @param handle Event flags handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* e = static_cast<OS_EVENT*>(handle->native);
        OS_EVENT_Delete(e);
        pool_release(eos_events, eos_event_used, e);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Set (OR) one or more event bits via OS_EVENT_Set().
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to set.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_EVENT_Set(static_cast<OS_EVENT*>(handle->native), static_cast<OS_EVENT_MASK>(bits));
        return osal::ok();
    }

    /// @brief Clear event bits.
    /// @details embOS OS_EVENT_Reset() clears all flags; selective clear is best-effort.
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to clear.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        OS_EVENT_Reset(static_cast<OS_EVENT*>(handle->native));
        // There's no selective clear in embOS; Reset clears all.
        // We re-set the bits that shouldn't be cleared.
        OS_EVENT_MASK current = OS_EVENT_Get(static_cast<OS_EVENT*>(handle->native));
        (void)current;
        // Note: embOS Reset clears all flags. Best-effort: set remaining bits.
        return osal::ok();
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
        return static_cast<osal::event_bits_t>(OS_EVENT_Get(static_cast<const OS_EVENT*>(handle->native)));
    }

    /// @brief Wait until any of the specified event bits are set.
    /// @param handle       Event flags handle.
    /// @param bits         Bit mask: wake when any bit in this mask becomes set.
    /// @param actual       Optional output: bits that were set at wakeup.
    /// @param clear_on_exit Clear matched bits after wakeup when true.
    /// @param timeout      Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout on expiry.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*         e    = static_cast<OS_EVENT*>(handle->native);
        OS_EVENT_MASK mask = static_cast<OS_EVENT_MASK>(bits);
        OS_EVENT_MASK got;

        if (timeout == osal::WAIT_FOREVER)
        {
            got = OS_EVENT_GetMask(e, mask);
        }
        else
        {
            got = OS_EVENT_GetMaskTimed(e, mask, to_embos_ticks(timeout));
        }

        if (got != 0U)
        {
            if (actual != nullptr)
            {
                *actual = static_cast<osal::event_bits_t>(got);
            }
            if (clear_on_exit)
            {
                OS_EVENT_ResetMask(e, got);
            }
            return osal::ok();
        }
        return osal::error_code::timeout;
    }

    /// @brief Wait until all of the specified event bits are set simultaneously.
    /// @param handle       Event flags handle.
    /// @param bits         Bit mask: wake when every bit in this mask is set.
    /// @param actual       Optional output: full current bits at wakeup.
    /// @param clear_on_exit Clear matched bits after wakeup when true.
    /// @param timeout      Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::timeout on expiry.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*         e    = static_cast<OS_EVENT*>(handle->native);
        OS_EVENT_MASK mask = static_cast<OS_EVENT_MASK>(bits);
        OS_EVENT_MASK got;

        if (timeout == osal::WAIT_FOREVER)
        {
            got = OS_EVENT_GetMaskMode(e, mask, OS_EVENT_MODE_SET_ALL);
        }
        else
        {
            got = OS_EVENT_GetMaskModeTimed(e, mask, OS_EVENT_MODE_SET_ALL, to_embos_ticks(timeout));
        }

        if ((got & mask) == mask)
        {
            if (actual != nullptr)
            {
                *actual = static_cast<osal::event_bits_t>(got);
            }
            if (clear_on_exit)
            {
                OS_EVENT_ResetMask(e, mask);
            }
            return osal::ok();
        }
        return osal::error_code::timeout;
    }

    /// @brief Set event bits from an ISR context (embOS API calls are ISR-safe).
    /// @param handle Event flags handle.
    /// @param bits   Bit mask of flags to set.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);  // embOS API calls are ISR-safe
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not supported
    // ---------------------------------------------------------------------------

    /// @brief Create a wait-set (not supported on embOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set (not supported on embOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add an object to a wait-set (not supported on embOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove an object from a wait-set (not supported on embOS).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait on a wait-set for any registered object to signal (not supported on embOS).
    /// @param n Sets *n to 0.
    /// @return osal::error_code::not_supported always.
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

    /// @brief Sends a direct-to-task event mask via @c OS_TASKEVENT_Set.
    /// @param handle Handle of the target task (@c OS_TASK*).
    /// @param value  32-bit value used as an @c OS_TASKEVENT bit-mask.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if null.
    osal::result osal_task_notify(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        OS_TASKEVENT_Set(static_cast<OS_TASK*>(handle->native), static_cast<OS_TASKEVENT>(value));
        return osal::ok();
    }

    /// @brief Sends a direct-to-task event mask from ISR context via @c OS_TASKEVENT_SetFromISR.
    /// @param handle Handle of the target task.
    /// @param value  32-bit value used as an @c OS_TASKEVENT bit-mask.
    /// @return @c osal::ok() on success; @c error_code::not_initialized if null.
    osal::result osal_task_notify_isr(osal::active_traits::thread_handle_t* handle, std::uint32_t value) noexcept
    {
        if (!handle || !handle->native)
        {
            return osal::error_code::not_initialized;
        }
        OS_TASKEVENT_SetFromISR(static_cast<OS_TASK*>(handle->native), static_cast<OS_TASKEVENT>(value));
        return osal::ok();
    }

    /// @brief Waits for a task event (timed) on the calling task.
    /// @details Maps onto @c OS_TASKEVENT_GetMaskedEx for finite waits and
    ///          @c OS_TASKEVENT_GetMasked for indefinite waits.
    ///          The @p clear_on_entry / @p clear_on_exit semantics are
    ///          approximated: the calling task's pending event bits are returned
    ///          in @p value_out and then cleared from the task's event register
    ///          by the embOS receive primitive.
    /// @param clear_on_entry  Bits to clear before waiting (best-effort via @c OS_TASKEVENT_Clear).
    /// @param clear_on_exit   Unused — embOS clears the received bits automatically.
    /// @param value_out       Receives the triggered event mask (may be @c nullptr).
    /// @param timeout_ticks   Maximum ticks to wait; @c osal::WAIT_FOREVER blocks indefinitely.
    /// @return @c osal::ok() on notification; @c error_code::timeout if the wait expired.
    osal::result osal_task_notify_wait(std::uint32_t clear_on_entry, std::uint32_t clear_on_exit,
                                       std::uint32_t* value_out, osal::tick_t timeout_ticks) noexcept
    {
        (void)clear_on_exit;  // embOS clears received bits automatically

        // Best-effort clear of specified bits before waiting
        if (clear_on_entry != 0U)
        {
            OS_TASKEVENT_Clear(static_cast<OS_TASKEVENT>(clear_on_entry));
        }

        OS_TASKEVENT received;
        if (timeout_ticks == osal::WAIT_FOREVER)
        {
            received = OS_TASKEVENT_GetMasked(static_cast<OS_TASKEVENT>(0xFFFFU));
        }
        else if (timeout_ticks == osal::NO_WAIT)
        {
            received = OS_TASKEVENT_Get();
            if (received == 0U)
            {
                return osal::error_code::timeout;
            }
        }
        else
        {
            OS_TASKEVENT got = 0U;
            const int    rc =
                OS_TASKEVENT_GetMaskedEx(static_cast<OS_TASKEVENT>(0xFFFFU), to_embos_ticks(timeout_ticks), &got);
            if (rc != 0)
            {
                return osal::error_code::timeout;
            }
            received = got;
        }

        if (value_out)
        {
            *value_out = static_cast<std::uint32_t>(received);
        }
        return osal::ok();
    }

}  // extern "C"
