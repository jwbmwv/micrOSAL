// SPDX-License-Identifier: Apache-2.0
/// @file cmsis_rtos2_backend.cpp
/// @brief CMSIS-RTOS2 (v2) implementation of all OSAL C-linkage functions
/// @details Maps every osal_* function to the CMSIS-RTOS2 API (cmsis_os2.h):
///          - thread      → osThreadNew / osThreadJoin / osThreadTerminate
///          - mutex       → osMutexNew / osMutexAcquire / osMutexRelease
///          - semaphore   → osSemaphoreNew / osSemaphoreAcquire / osSemaphoreRelease
///          - queue       → osMessageQueueNew / osMessageQueuePut / osMessageQueueGet
///          - timer       → osTimerNew / osTimerStart / osTimerStop
///          - event_flags → osEventFlagsNew / osEventFlagsSet / osEventFlagsWait
///          - memory_pool → osMemoryPoolNew / osMemoryPoolAlloc / osMemoryPoolFree
///
///          CMSIS-RTOS2 does not provide:
///          - Condition variable → emulated (Birrell pattern)
///          - Work queue → emulated (thread + ring buffer)
///          - Read-write lock → emulated (mutex + condvar + counter)
///
///          All primitive IDs are stored directly in handle->native.
///          Slot tracking pools manage lifetime without dynamic allocation.
///
///          Includes required: <cmsis_os2.h> (CMSIS-RTOS2 header)
///          Build macro:       OSAL_BACKEND_CMSIS_RTOS2
///
/// @copyright Copyright (c) 2026 James Baldwin. AI-assisted — see NOTICE.
/// @author James Baldwin
#ifndef OSAL_BACKEND_CMSIS_RTOS2
#define OSAL_BACKEND_CMSIS_RTOS2
#endif
#include <osal/osal.hpp>

#include <cmsis_os2.h>  ///< CMSIS-RTOS2 header
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// Pool sizes (tunable)
// ---------------------------------------------------------------------------
#ifndef OSAL_CMSIS2_MAX_THREADS
#define OSAL_CMSIS2_MAX_THREADS 8
#endif
#ifndef OSAL_CMSIS2_MAX_MUTEXES
#define OSAL_CMSIS2_MAX_MUTEXES 16
#endif
#ifndef OSAL_CMSIS2_MAX_SEMS
#define OSAL_CMSIS2_MAX_SEMS 16
#endif
#ifndef OSAL_CMSIS2_MAX_QUEUES
#define OSAL_CMSIS2_MAX_QUEUES 8
#endif
#ifndef OSAL_CMSIS2_MAX_TIMERS
#define OSAL_CMSIS2_MAX_TIMERS 8
#endif
#ifndef OSAL_CMSIS2_MAX_EVENTS
#define OSAL_CMSIS2_MAX_EVENTS 8
#endif
#ifndef OSAL_CMSIS2_MAX_POOLS
#define OSAL_CMSIS2_MAX_POOLS 4
#endif

// ---------------------------------------------------------------------------
// Slot tracking
// ---------------------------------------------------------------------------
struct cmsis2_thread_slot
{
    osThreadId_t id;
    bool         used;
};
static cmsis2_thread_slot cmsis2_threads[OSAL_CMSIS2_MAX_THREADS];

struct cmsis2_mutex_slot
{
    osMutexId_t id;
    bool        used;
};
static cmsis2_mutex_slot cmsis2_mutexes[OSAL_CMSIS2_MAX_MUTEXES];

struct cmsis2_sem_slot
{
    osSemaphoreId_t id;
    bool            used;
};
static cmsis2_sem_slot cmsis2_sems[OSAL_CMSIS2_MAX_SEMS];

struct cmsis2_queue_slot
{
    osMessageQueueId_t id;
    std::size_t        item_size;
    bool               used;
};
static cmsis2_queue_slot cmsis2_queues[OSAL_CMSIS2_MAX_QUEUES];

struct cmsis2_timer_slot
{
    osTimerId_t           id;
    osal_timer_callback_t fn;
    void*                 arg;
    osal::tick_t          period;
    bool                  used;
};
static cmsis2_timer_slot cmsis2_timers[OSAL_CMSIS2_MAX_TIMERS];

struct cmsis2_event_slot
{
    osEventFlagsId_t id;
    bool             used;
};
static cmsis2_event_slot cmsis2_events[OSAL_CMSIS2_MAX_EVENTS];

struct cmsis2_pool_slot
{
    osMemoryPoolId_t id;
    std::size_t      block_size;
    bool             used;
};
static cmsis2_pool_slot cmsis2_pools[OSAL_CMSIS2_MAX_POOLS];

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

/// @brief Map OSAL priority [0=lowest..255=highest] to CMSIS-RTOS2 priority enum.
static constexpr osPriority_t osal_to_cmsis2_priority(osal::priority_t p) noexcept
{
    if (p <= 16)
        return osPriorityIdle;
    if (p <= 48)
        return osPriorityLow;
    if (p <= 80)
        return osPriorityLow1;
    if (p <= 96)
        return osPriorityBelowNormal;
    if (p <= 128)
        return osPriorityNormal;
    if (p <= 160)
        return osPriorityAboveNormal;
    if (p <= 192)
        return osPriorityHigh;
    if (p <= 240)
        return osPriorityRealtime;
    return osPriorityRealtime7;
}

/// @brief Convert OSAL ticks to CMSIS-RTOS2 timeout (ms).
static constexpr uint32_t to_cmsis2_timeout(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
        return osWaitForever;
    return static_cast<uint32_t>(t);
}

// ---------------------------------------------------------------------------
// Timer trampoline
// ---------------------------------------------------------------------------
static void cmsis2_timer_callback(void* arg) noexcept
{
    auto* slot = static_cast<cmsis2_timer_slot*>(arg);
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

    /// @brief Return monotonic time in milliseconds via osKernelGetTickCount() / osKernelGetTickFreq().
    /// @return Milliseconds elapsed since kernel start.
    std::int64_t osal_clock_monotonic_ms() noexcept
    {
        const uint32_t freq  = osKernelGetTickFreq();
        const uint32_t ticks = osKernelGetTickCount();
        if (freq > 0U)
        {
            return static_cast<std::int64_t>(ticks) * 1000 / static_cast<std::int64_t>(freq);
        }
        return static_cast<std::int64_t>(ticks);
    }

    /// @brief Return wall-clock time in milliseconds (aliased to monotonic on CMSIS-RTOS2).
    /// @return Milliseconds since kernel start.
    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    /// @brief Return the raw CMSIS-RTOS2 kernel tick counter.
    /// @return Current osKernelGetTickCount() value.
    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(osKernelGetTickCount());
    }

    /// @brief Return the tick period in microseconds derived from osKernelGetTickFreq().
    /// @return Microseconds per tick (e.g. 1000 for a 1 kHz tick).
    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        const uint32_t freq = osKernelGetTickFreq();
        return (freq > 0U) ? static_cast<std::uint32_t>(1'000'000U / freq) : 1000U;
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    /// @brief Create and immediately start a CMSIS-RTOS2 thread via osThreadNew().
    /// @param handle      Output handle populated with the cmsis2_thread_slot pointer.
    /// @param entry       Thread entry function (cast to osThreadFunc_t).
    /// @param arg         Opaque argument forwarded to @p entry.
    /// @param priority    OSAL priority [0=lowest, 255=highest]; mapped to osPriority_t enum.
    /// @param stack_bytes Requested stack size in bytes.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        assert(handle != nullptr && entry != nullptr);
        auto* slot = slot_acquire(cmsis2_threads);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        osThreadAttr_t attr = {};
        attr.name           = name;
        attr.priority       = osal_to_cmsis2_priority(priority);
        attr.stack_mem      = stack;
        attr.stack_size     = static_cast<uint32_t>(stack_bytes);
        attr.attr_bits      = osThreadJoinable;

        slot->id = osThreadNew(reinterpret_cast<osThreadFunc_t>(entry), arg, &attr);
        if (slot->id == nullptr)
        {
            slot_release(cmsis2_threads, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Block until a thread exits via osThreadJoin(), then release its slot.
    /// @param handle Thread handle to join.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t /*timeout*/) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*            slot = static_cast<cmsis2_thread_slot*>(handle->native);
        const osStatus_t s    = osThreadJoin(slot->id);
        slot_release(cmsis2_threads, slot);
        handle->native = nullptr;
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Detach a thread via osThreadDetach() and release its slot.
    /// @param handle Thread handle to detach.
    /// @return osal::ok() on success; osal::error_code::not_initialized if the handle is null.
    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis2_thread_slot*>(handle->native);
        osThreadDetach(slot->id);
        slot_release(cmsis2_threads, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Change the priority of a running thread via osThreadSetPriority().
    /// @param handle   Thread handle.
    /// @param priority New OSAL priority mapped to osPriority_t.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_thread_set_priority(osal::active_traits::thread_handle_t* handle,
                                          osal::priority_t                      priority) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*            slot = static_cast<cmsis2_thread_slot*>(handle->native);
        const osStatus_t s    = osThreadSetPriority(slot->id, osal_to_cmsis2_priority(priority));
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Set thread CPU affinity (not supported on CMSIS-RTOS2).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_set_affinity(osal::active_traits::thread_handle_t* /*handle*/,
                                          osal::affinity_t /*affinity*/) noexcept
    {
        return osal::error_code::not_supported;
    }

    /// @brief Suspend a thread (not supported through this OSAL on CMSIS-RTOS2).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_suspend(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Resume a suspended thread (not supported through this OSAL on CMSIS-RTOS2).
    /// @return osal::error_code::not_supported always.
    osal::result osal_thread_resume(osal::active_traits::thread_handle_t* handle) noexcept
    {
        (void)handle;
        return osal::error_code::not_supported;
    }

    /// @brief Yield the current thread's remaining time slice via osThreadYield().
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

    /// @brief Create a CMSIS-RTOS2 mutex via osMutexNew() with priority inheritance.
    /// @param handle    Output handle populated with the cmsis2_mutex_slot pointer.
    /// @param recursive If true, enables osMutexRecursive in the mutex attributes.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_mutex_create(osal::active_traits::mutex_handle_t* handle, bool recursive) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis2_mutexes);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        osMutexAttr_t attr = {};
        attr.attr_bits     = osMutexPrioInherit;
        if (recursive)
        {
            attr.attr_bits |= osMutexRecursive;
        }

        slot->id = osMutexNew(&attr);
        if (slot->id == nullptr)
        {
            slot_release(cmsis2_mutexes, slot);
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
        auto* slot = static_cast<cmsis2_mutex_slot*>(handle->native);
        osMutexDelete(slot->id);
        slot_release(cmsis2_mutexes, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Acquire a mutex via osMutexAcquire(), blocking up to @p timeout_ticks.
    /// @param handle        Mutex handle.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout on failure.
    osal::result osal_mutex_lock(osal::active_traits::mutex_handle_t* handle, osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*            slot = static_cast<cmsis2_mutex_slot*>(handle->native);
        const osStatus_t s    = osMutexAcquire(slot->id, to_cmsis2_timeout(timeout_ticks));
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
    /// @return osal::ok() if acquired; osal::error_code::would_block otherwise.
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
        auto*            slot = static_cast<cmsis2_mutex_slot*>(handle->native);
        const osStatus_t s    = osMutexRelease(slot->id);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

    /// @brief Create a counting semaphore via osSemaphoreNew().
    /// @param handle        Output handle populated with the cmsis2_sem_slot pointer.
    /// @param initial_count Initial token count.
    /// @param max_count     Maximum token count.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_semaphore_create(osal::active_traits::semaphore_handle_t* handle, unsigned initial_count,
                                       unsigned max_count) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis2_sems);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        slot->id = osSemaphoreNew(max_count, initial_count, nullptr);
        if (slot->id == nullptr)
        {
            slot_release(cmsis2_sems, slot);
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
        auto* slot = static_cast<cmsis2_sem_slot*>(handle->native);
        osSemaphoreDelete(slot->id);
        slot_release(cmsis2_sems, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Increment (release) a semaphore via osSemaphoreRelease().
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_semaphore_give(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*            slot = static_cast<cmsis2_sem_slot*>(handle->native);
        const osStatus_t s    = osSemaphoreRelease(slot->id);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Increment a semaphore from ISR context (osSemaphoreRelease is ISR-safe).
    /// @param handle Semaphore handle.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // CMSIS-RTOS2 Release is ISR-safe
    }

    /// @brief Decrement (wait on) a semaphore via osSemaphoreAcquire().
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
        auto*            slot = static_cast<cmsis2_sem_slot*>(handle->native);
        const osStatus_t s    = osSemaphoreAcquire(slot->id, to_cmsis2_timeout(timeout_ticks));
        if (s == osOK)
        {
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Try to decrement a semaphore without blocking.
    /// @param handle Semaphore handle.
    /// @return osal::ok() if a token was available; osal::error_code::would_block otherwise.
    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (osMessageQueue)
    // ---------------------------------------------------------------------------

    /// @brief Create a CMSIS-RTOS2 message queue via osMessageQueueNew().
    /// @param handle    Output handle populated with the cmsis2_queue_slot pointer.
    /// @param item_size Size in bytes of each message.
    /// @param capacity  Maximum number of messages the queue can hold.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_queue_create(osal::active_traits::queue_handle_t* handle, void* /*buffer*/, std::size_t item_size,
                                   std::size_t capacity) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis2_queues);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->item_size = item_size;

        slot->id = osMessageQueueNew(static_cast<uint32_t>(capacity), static_cast<uint32_t>(item_size), nullptr);
        if (slot->id == nullptr)
        {
            slot_release(cmsis2_queues, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Destroy a queue via osMessageQueueDelete() and release its slot.
    /// @param handle Queue handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_queue_destroy(osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis2_queue_slot*>(handle->native);
        osMessageQueueDelete(slot->id);
        slot_release(cmsis2_queues, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Send a message via osMessageQueuePut().
    /// @param handle        Queue handle.
    /// @param item          Pointer to the message data.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is full.
    osal::result osal_queue_send(osal::active_traits::queue_handle_t* handle, const void* item,
                                 osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*            slot = static_cast<cmsis2_queue_slot*>(handle->native);
        const osStatus_t s    = osMessageQueuePut(slot->id, item, 0U, to_cmsis2_timeout(timeout_ticks));
        if (s == osOK)
        {
            return osal::ok();
        }
        return (timeout_ticks == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Send a message from ISR context (osMessageQueuePut is ISR-safe in CMSIS-RTOS2).
    /// @param handle Queue handle.
    /// @param item   Pointer to the message data.
    /// @return osal::ok() on success; osal::error_code::would_block if the queue is full.
    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);  // ISR-safe in CMSIS-RTOS2
    }

    /// @brief Receive a message from a queue via osMessageQueueGet().
    /// @param handle        Queue handle.
    /// @param item          Buffer to receive the dequeued message into.
    /// @param timeout_ticks Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout if the queue is empty.
    osal::result osal_queue_receive(osal::active_traits::queue_handle_t* handle, void* item,
                                    osal::tick_t timeout_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*            slot = static_cast<cmsis2_queue_slot*>(handle->native);
        const osStatus_t s    = osMessageQueueGet(slot->id, item, nullptr, to_cmsis2_timeout(timeout_ticks));
        if (s == osOK)
        {
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

    /// @brief Peek at the front message without removing it (not supported on CMSIS-RTOS2).
    /// @return osal::error_code::not_supported always.
    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;  // CMSIS-RTOS2 has no peek
    }

    /// @brief Return the number of messages currently in the queue via osMessageQueueGetCount().
    /// @param handle Queue handle.
    /// @return Message count, or 0 if the handle is invalid.
    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* slot = static_cast<const cmsis2_queue_slot*>(handle->native);
        return static_cast<std::size_t>(osMessageQueueGetCount(slot->id));
    }

    /// @brief Return the number of free slots in the queue via osMessageQueueGetSpace().
    /// @param handle Queue handle.
    /// @return Free slots count, or 0 if the handle is invalid.
    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* slot = static_cast<const cmsis2_queue_slot*>(handle->native);
        return static_cast<std::size_t>(osMessageQueueGetSpace(slot->id));
    }

    // ---------------------------------------------------------------------------
    // Timer (osTimer)
    // ---------------------------------------------------------------------------

    /// @brief Create (but do not start) a CMSIS-RTOS2 timer via osTimerNew().
    /// @param handle       Output handle populated with the cmsis2_timer_slot pointer.
    /// @param name         Optional timer name for debugging.
    /// @param callback     Callback invoked on each expiry.
    /// @param arg          Opaque argument forwarded to @p callback.
    /// @param period_ticks Expiry period in OSAL ticks.
    /// @param auto_reload  True for periodic (osTimerPeriodic); false for one-shot (osTimerOnce).
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        assert(handle != nullptr && callback != nullptr);
        auto* slot = slot_acquire(cmsis2_timers);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->fn     = callback;
        slot->arg    = arg;
        slot->period = period_ticks;

        osTimerAttr_t attr = {};
        attr.name          = name;

        slot->id = osTimerNew(cmsis2_timer_callback, auto_reload ? osTimerPeriodic : osTimerOnce, slot, &attr);
        if (slot->id == nullptr)
        {
            slot_release(cmsis2_timers, slot);
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
        auto* slot = static_cast<cmsis2_timer_slot*>(handle->native);
        osTimerDelete(slot->id);
        slot->fn = nullptr;
        slot_release(cmsis2_timers, slot);
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
        auto*            slot = static_cast<cmsis2_timer_slot*>(handle->native);
        const osStatus_t s    = osTimerStart(slot->id, static_cast<uint32_t>(slot->period));
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
        auto*            slot = static_cast<cmsis2_timer_slot*>(handle->native);
        const osStatus_t s    = osTimerStop(slot->id);
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

    /// @brief Update the timer period and restart if currently running.
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
        auto* slot   = static_cast<cmsis2_timer_slot*>(handle->native);
        slot->period = new_period_ticks;
        if (osTimerIsRunning(slot->id))
        {
            osTimerStop(slot->id);
            osTimerStart(slot->id, static_cast<uint32_t>(new_period_ticks));
        }
        return osal::ok();
    }

    /// @brief Query whether a timer is currently running via osTimerIsRunning().
    /// @param handle Timer handle.
    /// @return True if the timer is active; false otherwise.
    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        auto* slot = static_cast<const cmsis2_timer_slot*>(handle->native);
        return osTimerIsRunning(slot->id) != 0U;
    }

    // ---------------------------------------------------------------------------
    // Event flags (native osEventFlags)
    // ---------------------------------------------------------------------------

    /// @brief Create a CMSIS-RTOS2 event-flags object via osEventFlagsNew().
    /// @param handle Output handle populated with the cmsis2_event_slot pointer.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_event_flags_create(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis2_events);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }

        slot->id = osEventFlagsNew(nullptr);
        if (slot->id == nullptr)
        {
            slot_release(cmsis2_events, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Destroy an event-flags object via osEventFlagsDelete() and release its slot.
    /// @param handle Event-flags handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_event_flags_destroy(osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis2_event_slot*>(handle->native);
        osEventFlagsDelete(slot->id);
        slot_release(cmsis2_events, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Set (OR) one or more flags via osEventFlagsSet().
    /// @param handle Event-flags handle.
    /// @param bits   Bitmask of flags to set.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_event_flags_set(osal::active_traits::event_flags_handle_t* handle,
                                      osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis2_event_slot*>(handle->native);
        const uint32_t r    = osEventFlagsSet(slot->id, static_cast<uint32_t>(bits));
        // osEventFlagsSet returns flags value on success, osFlagsError on failure
        return (r != osFlagsErrorParameter && r != osFlagsErrorResource) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Clear (AND NOT) one or more flags via osEventFlagsClear().
    /// @param handle Event-flags handle.
    /// @param bits   Bitmask of flags to clear.
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_event_flags_clear(osal::active_traits::event_flags_handle_t* handle,
                                        osal::event_bits_t                         bits) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*          slot = static_cast<cmsis2_event_slot*>(handle->native);
        const uint32_t r    = osEventFlagsClear(slot->id, static_cast<uint32_t>(bits));
        return (r != osFlagsErrorParameter && r != osFlagsErrorResource) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Read the current flag bitmask via osEventFlagsGet() without modifying flags.
    /// @param handle Event-flags handle.
    /// @return Current flag bitmask, or 0 if the handle is invalid.
    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* slot = static_cast<const cmsis2_event_slot*>(handle->native);
        return static_cast<osal::event_bits_t>(osEventFlagsGet(slot->id));
    }

    /// @brief Wait until any of the specified flags are set via osEventFlagsWait().
    /// @param handle        Event-flags handle.
    /// @param bits          Bitmask of flags to watch.
    /// @param actual        Output for the flags that were set at wake-up; may be null.
    /// @param clear_on_exit If true, matched flags are cleared atomically on return.
    /// @param timeout       Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout on failure.
    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*    slot    = static_cast<cmsis2_event_slot*>(handle->native);
        uint32_t options = osFlagsWaitAny;
        if (!clear_on_exit)
        {
            options |= osFlagsNoClear;
        }
        const uint32_t r = osEventFlagsWait(slot->id, static_cast<uint32_t>(bits), options, to_cmsis2_timeout(timeout));
        if ((r != osFlagsErrorTimeout) && (r != osFlagsErrorParameter) && (r != osFlagsErrorResource))
        {
            if (actual != nullptr)
            {
                *actual = static_cast<osal::event_bits_t>(r);
            }
            return osal::ok();
        }
        return (timeout == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Wait until all of the specified flags are set via osEventFlagsWait() with osFlagsWaitAll.
    /// @param handle        Event-flags handle.
    /// @param bits          Bitmask of flags that must all be set.
    /// @param actual        Output for the flags that were set at wake-up; may be null.
    /// @param clear_on_exit If true, matched flags are cleared atomically on return.
    /// @param timeout       Maximum wait in OSAL ticks.
    /// @return osal::ok() on success; osal::error_code::would_block or ::timeout on failure.
    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*    slot    = static_cast<cmsis2_event_slot*>(handle->native);
        uint32_t options = osFlagsWaitAll;
        if (!clear_on_exit)
        {
            options |= osFlagsNoClear;
        }
        const uint32_t r = osEventFlagsWait(slot->id, static_cast<uint32_t>(bits), options, to_cmsis2_timeout(timeout));
        if ((r != osFlagsErrorTimeout) && (r != osFlagsErrorParameter) && (r != osFlagsErrorResource))
        {
            if (actual != nullptr)
            {
                *actual = static_cast<osal::event_bits_t>(r);
            }
            return osal::ok();
        }
        return (timeout == osal::NO_WAIT) ? osal::error_code::would_block : osal::error_code::timeout;
    }

    /// @brief Set flags from ISR context (osEventFlagsSet is ISR-safe in CMSIS-RTOS2).
    /// @param handle Event-flags handle.
    /// @param bits   Bitmask of flags to set.
    /// @return osal::ok() on success; osal::error_code::unknown on OS failure.
    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);  // CMSIS-RTOS2 osEventFlagsSet is ISR-safe
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not supported
    // ---------------------------------------------------------------------------

    /// @brief Create a wait-set (not supported on CMSIS-RTOS2).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_create(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Destroy a wait-set (not supported on CMSIS-RTOS2).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_destroy(osal::active_traits::wait_set_handle_t*) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Add an object to a wait-set (not supported on CMSIS-RTOS2).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_add(osal::active_traits::wait_set_handle_t*, int, std::uint32_t) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Remove an object from a wait-set (not supported on CMSIS-RTOS2).
    /// @return osal::error_code::not_supported always.
    osal::result osal_wait_set_remove(osal::active_traits::wait_set_handle_t*, int) noexcept
    {
        return osal::error_code::not_supported;
    }
    /// @brief Wait on a wait-set for any registered object to signal (not supported on CMSIS-RTOS2).
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
    // Memory pool (native — osMemoryPool)
    // ---------------------------------------------------------------------------

    /// @brief Create a fixed-block memory pool via osMemoryPoolNew().
    /// @param handle      Output handle populated with the cmsis2_pool_slot pointer.
    /// @param block_size  Size in bytes of each block.
    /// @param block_count Total number of blocks in the pool.
    /// @param name        Optional pool name visible in debugging tools.
    /// @return osal::ok() on success; osal::error_code::out_of_resources on failure.
    osal::result osal_memory_pool_create(osal::active_traits::memory_pool_handle_t* handle, void* /*buffer*/,
                                         std::size_t /*buf_bytes*/, std::size_t block_size, std::size_t block_count,
                                         const char* name) noexcept
    {
        assert(handle != nullptr);
        auto* slot = slot_acquire(cmsis2_pools);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->block_size = block_size;

        osMemoryPoolAttr_t attr = {};
        attr.name               = name;

        slot->id = osMemoryPoolNew(static_cast<uint32_t>(block_count), static_cast<uint32_t>(block_size), &attr);
        if (slot->id == nullptr)
        {
            slot_release(cmsis2_pools, slot);
            return osal::error_code::out_of_resources;
        }
        handle->native = static_cast<void*>(slot);
        return osal::ok();
    }

    /// @brief Destroy a memory pool via osMemoryPoolDelete() and release its slot.
    /// @param handle Memory pool handle; no-op if null or already destroyed.
    /// @return osal::ok() always.
    osal::result osal_memory_pool_destroy(osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis2_pool_slot*>(handle->native);
        osMemoryPoolDelete(slot->id);
        slot_release(cmsis2_pools, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    /// @brief Allocate a block from the pool non-blocking via osMemoryPoolAlloc(..., 0).
    /// @param handle Memory pool handle.
    /// @return Pointer to the allocated block, or nullptr if the pool is empty or the handle is invalid.
    void* osal_memory_pool_allocate(osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return nullptr;
        }
        auto* slot = static_cast<cmsis2_pool_slot*>(handle->native);
        return osMemoryPoolAlloc(slot->id, 0U);  // non-blocking
    }

    /// @brief Allocate a block with timeout via osMemoryPoolAlloc().
    /// @param handle  Memory pool handle.
    /// @param timeout Maximum wait in OSAL ticks.
    /// @return Pointer to the allocated block, or nullptr on timeout or invalid handle.
    void* osal_memory_pool_allocate_timed(osal::active_traits::memory_pool_handle_t* handle,
                                          osal::tick_t                               timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return nullptr;
        }
        auto* slot = static_cast<cmsis2_pool_slot*>(handle->native);
        return osMemoryPoolAlloc(slot->id, to_cmsis2_timeout(timeout));
    }

    /// @brief Return a block to the pool via osMemoryPoolFree().
    /// @param handle Memory pool handle.
    /// @param block  Block pointer previously returned by osal_memory_pool_allocate().
    /// @return osal::ok() on success; osal::error_code::not_initialized or ::unknown on failure.
    osal::result osal_memory_pool_deallocate(osal::active_traits::memory_pool_handle_t* handle, void* block) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*            slot = static_cast<cmsis2_pool_slot*>(handle->native);
        const osStatus_t s    = osMemoryPoolFree(slot->id, block);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    /// @brief Return the number of available blocks via osMemoryPoolGetSpace().
    /// @param handle Memory pool handle.
    /// @return Free block count, or 0 if the handle is invalid.
    std::size_t osal_memory_pool_available(const osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        auto* slot = static_cast<const cmsis2_pool_slot*>(handle->native);
        return static_cast<std::size_t>(osMemoryPoolGetSpace(slot->id));
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
