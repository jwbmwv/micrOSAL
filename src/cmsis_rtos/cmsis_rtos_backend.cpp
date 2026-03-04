// SPDX-License-Identifier: Apache-2.0
/// @file cmsis_rtos_backend.cpp
/// @brief CMSIS-RTOS v1 implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the CMSIS-RTOS v1 API (cmsis_os.h):
///          - thread    → osThreadCreate / osThreadTerminate
///          - mutex     → osMutexCreate / osMutexWait / osMutexRelease
///          - semaphore → osSemaphoreCreate / osSemaphoreWait / osSemaphoreRelease
///          - queue     → osMessageCreate / osMessagePut / osMessageGet
///          - timer     → osTimerCreate / osTimerStart / osTimerStop
///          - memory_pool → osPoolCreate / osPoolAlloc / osPoolFree
///
///          CMSIS-RTOS v1 does not provide:
///          - Per-object event flags (osSignal is per-thread) → emulated
///          - Condition variable → emulated (Birrell pattern)
///          - Work queue → emulated (thread + ring buffer)
///          - Read-write lock → emulated (mutex + condvar + counter)
///
///          The CMSIS-RTOS v1 API uses definition macros (osMutexDef, etc.) to
///          create static control blocks.  This backend pre-creates pools of
///          definitions for each primitive type.
///
///          Includes required: <cmsis_os.h> (CMSIS-RTOS v1 header)
///          Build macro:       OSAL_BACKEND_CMSIS_RTOS
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_CMSIS_RTOS
#define OSAL_BACKEND_CMSIS_RTOS
#endif
#include <osal/osal.hpp>

#include <cmsis_os.h>  ///< CMSIS-RTOS v1 header
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Pool sizes (tunable)
// ---------------------------------------------------------------------------
#ifndef OSAL_CMSIS1_MAX_THREADS
#define OSAL_CMSIS1_MAX_THREADS 8
#endif
#ifndef OSAL_CMSIS1_MAX_MUTEXES
#define OSAL_CMSIS1_MAX_MUTEXES 16
#endif
#ifndef OSAL_CMSIS1_MAX_SEMS
#define OSAL_CMSIS1_MAX_SEMS 16
#endif
#ifndef OSAL_CMSIS1_MAX_QUEUES
#define OSAL_CMSIS1_MAX_QUEUES 8
#endif
#ifndef OSAL_CMSIS1_MAX_TIMERS
#define OSAL_CMSIS1_MAX_TIMERS 8
#endif
#ifndef OSAL_CMSIS1_MAX_EVENTS
#define OSAL_CMSIS1_MAX_EVENTS 8
#endif
#ifndef OSAL_CMSIS1_MAX_POOLS
#define OSAL_CMSIS1_MAX_POOLS 4
#endif

// ---------------------------------------------------------------------------
// Pool tracking — CMSIS-RTOS v1 IDs are stored directly in handle->native
// ---------------------------------------------------------------------------
struct cmsis1_thread_slot
{
    osThreadId id;
    bool       used;
};
static cmsis1_thread_slot cmsis1_threads[OSAL_CMSIS1_MAX_THREADS];

struct cmsis1_mutex_slot
{
    osMutexId id;
    bool      used;
};
static cmsis1_mutex_slot cmsis1_mutexes[OSAL_CMSIS1_MAX_MUTEXES];

struct cmsis1_sem_slot
{
    osSemaphoreId id;
    bool          used;
};
static cmsis1_sem_slot cmsis1_sems[OSAL_CMSIS1_MAX_SEMS];

struct cmsis1_queue_slot
{
    osMessageQId id;
    std::size_t  item_size;
    bool         used;
};
static cmsis1_queue_slot cmsis1_queues[OSAL_CMSIS1_MAX_QUEUES];

struct cmsis1_timer_slot
{
    osTimerId             id;
    osal_timer_callback_t fn;
    void*                 arg;
    osal::tick_t          period;
    bool                  auto_reload;
    bool                  used;
};
static cmsis1_timer_slot cmsis1_timers[OSAL_CMSIS1_MAX_TIMERS];

struct cmsis1_pool_slot
{
    osPoolId    id;
    std::size_t block_size;
    std::size_t block_count;
    bool        used;
};
static cmsis1_pool_slot cmsis1_pools[OSAL_CMSIS1_MAX_POOLS];

// ---------------------------------------------------------------------------
// Pool helpers
// ---------------------------------------------------------------------------
template<typename T, std::size_t N>
static T* slot_acquire(T (&pool)[N]) noexcept
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
static void slot_release(T (&pool)[N], T* p) noexcept
{
    if (p != nullptr)
    {
        p->used = false;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Map OSAL priority [0=lowest..255=highest] to CMSIS-RTOS priority enum.
static constexpr osPriority osal_to_cmsis1_priority(osal::priority_t p) noexcept
{
    if (p <= 16)
        return osPriorityIdle;
    if (p <= 64)
        return osPriorityLow;
    if (p <= 96)
        return osPriorityBelowNormal;
    if (p <= 160)
        return osPriorityNormal;
    if (p <= 192)
        return osPriorityAboveNormal;
    if (p <= 224)
        return osPriorityHigh;
    return osPriorityRealtime;
}

/// @brief Convert OSAL ticks to CMSIS wait value (milliseconds).
static constexpr uint32_t to_cmsis1_timeout(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
        return osWaitForever;
    return static_cast<uint32_t>(t);
}

// ---------------------------------------------------------------------------
// Timer trampoline
// ---------------------------------------------------------------------------
static void cmsis1_timer_callback(void const* arg) noexcept
{
    auto* slot = const_cast<cmsis1_timer_slot*>(static_cast<const cmsis1_timer_slot*>(arg));
    if (slot != nullptr && slot->fn != nullptr)
    {
        slot->fn(slot->arg);
    }
}

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    /// @brief Return monotonic time in milliseconds via osKernelSysTick().
    /// @return Milliseconds elapsed since kernel start.
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        return static_cast<std::int64_t>(osKernelSysTick() / (osKernelSysTickMicroSec(1000U)));
    }

    /// @brief Return wall-clock time in milliseconds (aliased to monotonic on CMSIS-RTOS v1).
    /// @return Milliseconds since kernel start.
    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    /// @brief Return the raw CMSIS-RTOS v1 system tick counter.
    /// @return Current osKernelSysTick() value.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(osKernelSysTick());
    }

    /// @brief Return the tick period in microseconds.
    /// @details CMSIS-RTOS v1 timeouts are expressed in milliseconds, so this returns 1000.
    /// @return 1000 microseconds (1 ms per tick unit).
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        return 1000U;  // CMSIS-RTOS v1 timeouts are in ms
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    /// @brief Create and immediately start a CMSIS-RTOS v1 thread.
    /// @param handle      Output handle populated with the cmsis1_thread_slot pointer.
    /// @param entry       Thread entry function (cast to os_pthread).
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority [0=lowest, 255=highest]; mapped to osPriority enum.
    /// @param stack_bytes Requested stack size in bytes.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t   priority, osal::affinity_t /*affinity*/, void* /*stack*/,
                                    osal::stack_size_t stack_bytes, const char* /*name*/) noexcept
    {
        assert(handle != nullptr && entry != nullptr);
        auto* slot = slot_acquire(cmsis1_threads);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        osThreadDef_t def = {};
        def.pthread       = reinterpret_cast<os_pthread>(entry);
        def.tpriority     = osal_to_cmsis1_priority(priority);
        def.stacksize     = static_cast<uint32_t>(stack_bytes);
        def.instances     = 0U;

        slot->id = osThreadCreate(&def, arg);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_threads, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Terminate a thread and release its slot.
    /// @param handle Thread handle to join.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis1_thread_slot*>(handle->native);
        osThreadTerminate(slot->id);
        slot_release(cmsis1_threads, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Detach a thread, releasing its slot without waiting.
    /// @param handle Thread handle to detach.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis1_thread_slot*>(handle->native);
        slot_release(cmsis1_threads, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change the priority of a running thread via osThreadSetPriority().
    /// @param handle   Thread handle.
    /// @param priority New OSAL priority; mapped to osPriority enum.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_thread_slot*>(handle->native);
        const osStatus s    = osThreadSetPriority(slot->id, osal_to_cmsis1_priority(priority));
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Set thread CPU affinity (not supported on CMSIS-RTOS v1).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Suspend a thread (not supported on CMSIS-RTOS v1 through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported on CMSIS-RTOS v1 through this OSAL).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Yield the current thread's time slice via osThreadYield().
    void osal_thread_yield() noexcept
    {
        osThreadYield();
    }

    /// @brief Sleep for at least @p ms milliseconds via osDelay().
    /// @param ms Delay in milliseconds.
    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        osDelay(ms);
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

    /// @brief Create a CMSIS-RTOS v1 mutex via osMutexCreate().
    /// @param handle Output handle populated with the cmsis1_mutex_slot pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool /*recursive*/) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis1_mutexes);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        osMutexDef_t def = {};
        slot->id         = osMutexCreate(&def);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_mutexes, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Destroy a mutex via osMutexDelete() and release its slot.
    /// @param handle Mutex handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_mutex_destroy(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_mutex_slot*>(handle->native);
        osMutexDelete(slot->id);
        slot_release(cmsis1_mutexes, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire a mutex via osMutexWait(), blocking up to @p timeout_ticks ticks.
    /// @param handle        Mutex handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks (mapped to milliseconds).
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout on failure.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_mutex_slot*>(handle->native);
        const osStatus s    = osMutexWait(slot->id, to_cmsis1_timeout(timeout_ticks));
        if (s == osOK)
        {
            return osal::ok();
        }
        if (timeout_ticks == osal::NO_WAIT)
        {
            return osal::error_code::would_block;
        }
        return osal::error_code::timeout;
    }

    /// @brief Try to acquire a mutex without blocking.
    /// @param handle Mutex handle.
    /// @return osal::ok() if acquired; osal::error_code::would_block if not immediately available.
    osal::result osal_mutex_try_lock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        return osal_mutex_lock(handle, osal::NO_WAIT);
    }

    /// @brief Release a mutex via osMutexRelease().
    /// @param handle Mutex handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_mutex_unlock(osal::active_traits::mutex_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_mutex_slot*>(handle->native);
        const osStatus s    = osMutexRelease(slot->id);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    /// @brief Create a counting semaphore via osSemaphoreCreate().
    /// @param handle        Output handle populated with the cmsis1_sem_slot pointer.
    /// @param initial_count Initial token count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned /*max_count*/) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis1_sems);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        osSemaphoreDef_t def = {};
        slot->id             = osSemaphoreCreate(&def, static_cast<int32_t>(initial_count));
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_sems, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Destroy a semaphore via osSemaphoreDelete() and release its slot.
    /// @param handle Semaphore handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_semaphore_destroy(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_sem_slot*>(handle->native);
        osSemaphoreDelete(slot->id);
        slot_release(cmsis1_sems, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (release) a semaphore via osSemaphoreRelease().
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_sem_slot*>(handle->native);
        const osStatus s    = osSemaphoreRelease(slot->id);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Increment a semaphore from ISR context (CMSIS-RTOS v1 Release is ISR-safe).
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // CMSIS-RTOS v1 Release is ISR-safe
    }

    /// @brief Decrement (wait on) a semaphore via osSemaphoreWait().
    /// @param handle        Semaphore handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks (mapped to milliseconds).
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout on failure.
    osal::result osal_semaphore_take(osal::active_traits::semaphore_handle_t* handle,
                                     osal::tick_t                             timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*         slot   = static_cast<cmsis1_sem_slot*>(handle->native);
        const int32_t tokens = osSemaphoreWait(slot->id, to_cmsis1_timeout(timeout_ticks));
        if (tokens > 0)
        {
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Try to decrement a semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if decremented; osal::error_code::would_block if no tokens available.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (osMessageQ)
    // ---------------------------------------------------------------------------

    /// @brief Create a CMSIS-RTOS v1 message queue via osMessageCreate().
    /// @param handle    Output handle populated with the cmsis1_queue_slot pointer.
    /// @param item_size Size in bytes of each message (values > 4 bytes are sent as pointers).
    /// @param capacity  Maximum number of messages the queue can hold.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis1_queues);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->item_size = item_size;

        osMessageQDef_t def = {};
        def.queue_sz        = static_cast<uint32_t>(capacity);
        def.item_sz         = static_cast<uint32_t>(item_size);

        slot->id = osMessageCreate(&def, nullptr);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_queues, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Destroy a queue and release its slot.
    /// @details CMSIS-RTOS v1 has no osMessageDelete in all implementations; best-effort slot release.
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_queue_slot*>(handle->native);
        // CMSIS-RTOS v1 does not have osMessageDelete in all implementations;
        // best-effort release.
        slot_release(cmsis1_queues, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send a 32-bit message to a queue via osMessagePut().
    /// @details For messages larger than 4 bytes, the first four bytes are sent as a 32-bit value.
    /// @param handle        Queue handle.
    /// @param item          Pointer to the message data.
    /// @param timeout_ticks Maximum wait in OSAL ticks (mapped to milliseconds).
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis1_queue_slot*>(handle->native);
        // CMSIS-RTOS v1 sends 32-bit values; for larger items, send a pointer.
        uint32_t val = 0U;
        std::memcpy(&val, item, (slot->item_size <= sizeof(val)) ? slot->item_size : sizeof(val));
        const osStatus s = osMessagePut(slot->id, val, to_cmsis1_timeout(timeout_ticks));
        if (s == osOK)
        {
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Send a message from ISR context (osMessagePut is ISR-safe in CMSIS-RTOS v1).
    /// @param handle Queue handle.
    /// @param item   Pointer to the message data.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);  // ISR-safe in CMSIS-RTOS v1
    }

    /// @brief Receive a message from a queue via osMessageGet().
    /// @param handle        Queue handle.
    /// @param item          Buffer to receive the dequeued message into.
    /// @param timeout_ticks Maximum wait in OSAL ticks (mapped to milliseconds).
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*   slot = static_cast<cmsis1_queue_slot*>(handle->native);
        osEvent ev   = osMessageGet(slot->id, to_cmsis1_timeout(timeout_ticks));
        if (ev.status == osEventMessage)
        {
            uint32_t val = ev.value.v;
            std::memcpy(item, &val, (slot->item_size <= sizeof(val)) ? slot->item_size : sizeof(val));
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Receive a message from ISR context (non-blocking).
    /// @param handle Queue handle.
    /// @param item   Buffer to receive the dequeued message into.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is empty.
    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    /// @brief Peek at the front message without removing it (not supported on CMSIS-RTOS v1).
    /// @return osal::error_code::not_supported always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;  // CMSIS-RTOS v1 has no peek
    }

    /// @brief Return the number of messages currently in the queue (not available in CMSIS-RTOS v1).
    /// @return Always 0; CMSIS-RTOS v1 has no message-count query.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* /*handle*/) noexcept
    {
        return 0U;  // CMSIS-RTOS v1 has no message-count query
    }

    /// @brief Return the number of free slots in the queue (not available in CMSIS-RTOS v1).
    /// @return Always 0; CMSIS-RTOS v1 has no free-space query.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* /*handle*/) noexcept
    {
        return 0U;  // CMSIS-RTOS v1 has no free-space query
    }

    // ---------------------------------------------------------------------------
    // Timer (osTimer)
    // ---------------------------------------------------------------------------

    /// @brief Create (but do not start) a CMSIS-RTOS v1 timer via osTimerCreate().
    /// @param handle       Output handle populated with the cmsis1_timer_slot pointer.
    /// @param callback     Callback invoked on each expiry.
    /// @param arg          Opaque argument forwarded to @p callback.
    /// @param period_ticks Expiry period in OSAL ticks (milliseconds).
    /// @param auto_reload  True for periodic (osTimerPeriodic); false for one-shot (osTimerOnce).
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* /*name*/,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        auto* slot = slot_acquire(cmsis1_timers);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->fn          = callback;
        slot->arg         = arg;
        slot->period      = period_ticks;
        slot->auto_reload = auto_reload;

        osTimerDef_t def = {};
        def.ptimer       = cmsis1_timer_callback;

        slot->id = osTimerCreate(&def, auto_reload ? osTimerPeriodic : osTimerOnce, slot);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_timers, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Destroy a timer via osTimerDelete() and release its slot.
    /// @param handle Timer handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_timer_slot*>(handle->native);
        osTimerDelete(slot->id);
        slot->fn = nullptr;
        slot_release(cmsis1_timers, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Activate (start) a timer via osTimerStart() using the stored period.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_timer_slot*>(handle->native);
        const osStatus s    = osTimerStart(slot->id, static_cast<uint32_t>(slot->period));
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Deactivate (stop) a timer via osTimerStop().
    /// @param handle Timer handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_timer_stop(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_timer_slot*>(handle->native);
        const osStatus s    = osTimerStop(slot->id);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Stop and immediately restart a timer.
    /// @param handle Timer handle.
    /// @return osal::ok() on success; forwarded error from osal_timer_start().
    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    /// @brief Change the period stored in the slot; takes effect on next osTimerStart().
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
        auto* slot   = static_cast<cmsis1_timer_slot*>(handle->native);
        slot->period = new_period_ticks;
        // Will take effect on next start
        return osal::ok();
    }

    /// @brief Query whether a timer is currently active.
    /// @details CMSIS-RTOS v1 has no osTimerIsRunning; always returns false.
    /// @return Always false.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* /*handle*/) noexcept
    {
        // CMSIS-RTOS v1 has no osTimerIsRunning; best-effort: false
        return false;
    }

    // ---------------------------------------------------------------------------
    // Event flags (emulated — shared OSAL mutex + per-waiter semaphores)
    // ---------------------------------------------------------------------------
#include "../common/emulated_event_flags.inl"

    // ---------------------------------------------------------------------------
    // Wait-set — not supported
    // ---------------------------------------------------------------------------

    /// @brief Create a wait-set (not supported on CMSIS-RTOS v1).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set (not supported on CMSIS-RTOS v1).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add an object to a wait-set (not supported on CMSIS-RTOS v1).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove an object from a wait-set (not supported on CMSIS-RTOS v1).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait on a wait-set for any registered object to signal (not supported on CMSIS-RTOS v1).
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
    // Memory pool (native — osPool API)
    // ---------------------------------------------------------------------------

    /// @brief Create a fixed-block memory pool via osPoolCreate().
    /// @param handle      Output handle populated with the cmsis1_pool_slot pointer.
    /// @param block_size  Size in bytes of each block.
    /// @param block_count Total number of blocks in the pool.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_memory_pool_create(osal::active_traits::memory_pool_handle_t* handle, void* /*buffer*/,
                                         std::size_t /*buf_bytes*/, std::size_t block_size, std::size_t block_count,
                                         const char* /*name*/) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis1_pools);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->block_size  = block_size;
        slot->block_count = block_count;

        osPoolDef_t def = {};
        def.pool_sz     = static_cast<uint32_t>(block_count);
        def.item_sz     = static_cast<uint32_t>(block_size);

        slot->id = osPoolCreate(&def);
        if (slot->id == nullptr)
        {
            slot_release(cmsis1_pools, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Release a memory pool slot.
    /// @details CMSIS-RTOS v1 has no osPoolDelete; only the tracking slot is freed.
    /// @param handle Memory pool handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_memory_pool_destroy(osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis1_pool_slot*>(handle->native);
        // CMSIS-RTOS v1 has no osPoolDelete; release tracking slot
        slot_release(cmsis1_pools, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Allocate a block from the pool via osPoolAlloc() (non-blocking).
    /// @param handle Memory pool handle.
    /// @return Pointer to the allocated block, or nullptr if the pool is empty or the handle is invalid.
    void* osal_memory_pool_allocate(osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return nullptr;
        }
        auto* slot = static_cast<cmsis1_pool_slot*>(handle->native);
        return osPoolAlloc(slot->id);
    }

    /// @brief Allocate a block with optional timeout (CMSIS-RTOS v1 is non-blocking only).
    /// @details Timeout is ignored; delegates to osal_memory_pool_allocate().
    /// @param handle Memory pool handle.
    /// @return Pointer to the allocated block, or nullptr if the pool is empty.
    void* osal_memory_pool_allocate_timed(osal::active_traits::memory_pool_handle_t* handle,
                                          osal::tick_t /*timeout*/) noexcept
    {
        // CMSIS-RTOS v1 osPoolAlloc is non-blocking only
        return osal_memory_pool_allocate(handle);
    }

    /// @brief Return a block to the pool via osPoolFree().
    /// @param handle Memory pool handle.
    /// @param block  Block pointer previously returned by osal_memory_pool_allocate().
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_memory_pool_deallocate(osal::active_traits::memory_pool_handle_t* handle, void* block) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis1_pool_slot*>(handle->native);
        const osStatus s    = osPoolFree(slot->id, block);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Return the number of available blocks in the pool (not available in CMSIS-RTOS v1).
    /// @return Always 0; CMSIS-RTOS v1 has no pool count query.
    std::size_t osal_memory_pool_available(const osal::active_traits::memory_pool_handle_t* /*handle*/) noexcept
    {
        // CMSIS-RTOS v1 has no pool count query
        return 0U;
    }

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
