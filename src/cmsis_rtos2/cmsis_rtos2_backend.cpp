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
#include <atomic>
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

namespace
{

// ---------------------------------------------------------------------------
// Slot tracking
// ---------------------------------------------------------------------------
struct cmsis2_thread_slot
{
    osThreadId_t      id{};
    std::atomic<bool> used;
};
cmsis2_thread_slot cmsis2_threads[OSAL_CMSIS2_MAX_THREADS];

struct cmsis2_mutex_slot
{
    osMutexId_t       id{};
    std::atomic<bool> used;
};
cmsis2_mutex_slot cmsis2_mutexes[OSAL_CMSIS2_MAX_MUTEXES];

struct cmsis2_sem_slot
{
    osSemaphoreId_t   id{};
    std::atomic<bool> used;
};
cmsis2_sem_slot cmsis2_sems[OSAL_CMSIS2_MAX_SEMS];

struct cmsis2_queue_slot
{
    osMessageQueueId_t id{};
    std::size_t        item_size{};
    std::atomic<bool>  used;
};
cmsis2_queue_slot cmsis2_queues[OSAL_CMSIS2_MAX_QUEUES];

struct cmsis2_timer_slot
{
    osTimerId_t           id{};
    osal_timer_callback_t fn{};
    void*                 arg{};
    osal::tick_t          period{};
    std::atomic<bool>     used;
    std::atomic<bool>     ever_started;
};
cmsis2_timer_slot cmsis2_timers[OSAL_CMSIS2_MAX_TIMERS];

struct cmsis2_event_slot
{
    osEventFlagsId_t  id{};
    std::atomic<bool> used;
};
cmsis2_event_slot cmsis2_events[OSAL_CMSIS2_MAX_EVENTS];

struct cmsis2_pool_slot
{
    osMemoryPoolId_t  id{};
    std::size_t       block_size{};
    std::atomic<bool> used;
};
cmsis2_pool_slot cmsis2_pools[OSAL_CMSIS2_MAX_POOLS];
static_assert(std::atomic<bool>::is_always_lock_free);

// ---------------------------------------------------------------------------
// Pool helpers
// ---------------------------------------------------------------------------
template<typename T, std::size_t N>
T* slot_acquire(T (&pool)[N]) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!pool[i].used.exchange(true, std::memory_order_acquire))
        {
            return &pool[i];
        }
    }
    return nullptr;
}

template<typename T, std::size_t N>
void slot_release(T (&pool)[N], T* p) noexcept
{
    (void)pool;
    if (p != nullptr)
    {
        p->used.store(false, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr osPriority_t osal_to_cmsis2_priority(osal::priority_t p) noexcept
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

constexpr uint32_t to_cmsis2_timeout(osal::tick_t t) noexcept
{
    if (t == osal::WAIT_FOREVER)
        return osWaitForever;
    return static_cast<uint32_t>(t);
}

// ---------------------------------------------------------------------------
// Timer trampoline
// ---------------------------------------------------------------------------
void cmsis2_timer_callback(void* arg) noexcept
{
    auto* slot = static_cast<cmsis2_timer_slot*>(arg);
    if (slot != nullptr && slot->fn != nullptr)
    {
        slot->fn(slot->arg);
    }
}

}  // namespace

extern "C"
{
    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

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

    std::int64_t osal_clock_system_ms() noexcept
    {
        return osal_clock_monotonic_ms();
    }

    osal::tick_t osal_clock_ticks() noexcept
    {
        return static_cast<osal::tick_t>(osKernelGetTickCount());
    }

    std::uint32_t osal_clock_tick_period_us() noexcept
    {
        const uint32_t freq = osKernelGetTickFreq();
        return (freq > 0U) ? static_cast<std::uint32_t>(1'000'000U / freq) : 1000U;
    }

    // ---------------------------------------------------------------------------
    // Thread
    // ---------------------------------------------------------------------------

    osal::result osal_thread_create(osal::active_traits::thread_handle_t* handle, void (*entry)(void*), void* arg,
                                    osal::priority_t priority, osal::affinity_t /*affinity*/, void* stack,
                                    osal::stack_size_t stack_bytes, const char* name) noexcept
    {
        if (handle == nullptr || entry == nullptr || (stack != nullptr && stack_bytes == 0U))
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
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

    osal::result osal_thread_join(osal::active_traits::thread_handle_t* handle, osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        if (timeout != osal::WAIT_FOREVER)
        {
            return osal::error_code::not_supported;
        }
        auto*            slot = static_cast<cmsis2_thread_slot*>(handle->native);
        const osStatus_t s    = osThreadJoin(slot->id);
        if (s != osOK)
        {
            return osal::error_code::unknown;
        }
        slot_release(cmsis2_threads, slot);
        handle->native = nullptr;
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    osal::result osal_thread_detach(osal::active_traits::thread_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis2_thread_slot*>(handle->native);
        if (osThreadDetach(slot->id) != osOK)
        {
            return osal::error_code::unknown;
        }
        slot_release(cmsis2_threads, slot);
        handle->native = nullptr;
        return osal::ok();
    }

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
        osThreadYield();
    }

    void osal_thread_sleep_ms(std::uint32_t ms) noexcept
    {
        if (ms == 0U)
        {
            osThreadYield();
            return;
        }
        const std::uint64_t ticks     = (static_cast<std::uint64_t>(ms) * osKernelGetTickFreq() + 999U) / 1000U;
        std::uint64_t       remaining = ticks;
        while (remaining != 0U)
        {
            const auto chunk = static_cast<std::uint32_t>(remaining > 0x7fffffffULL ? 0x7fffffffULL : remaining);
            if (osDelay(chunk) != osOK)
            {
                return;
            }
            remaining -= chunk;
        }
    }

    // ---------------------------------------------------------------------------
    // Mutex
    // ---------------------------------------------------------------------------

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
        auto*            slot = static_cast<cmsis2_mutex_slot*>(handle->native);
        const osStatus_t s    = osMutexRelease(slot->id);
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

    // ---------------------------------------------------------------------------
    // Semaphore
    // ---------------------------------------------------------------------------

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

    osal::result osal_semaphore_give_isr(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_give(handle);  // CMSIS-RTOS2 Release is ISR-safe
    }

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

    osal::result osal_semaphore_try_take(osal::active_traits::semaphore_handle_t* handle) noexcept
    {
        return osal_semaphore_take(handle, osal::NO_WAIT);
    }

    // ---------------------------------------------------------------------------
    // Queue (osMessageQueue)
    // ---------------------------------------------------------------------------

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

    osal::result osal_queue_send_isr(osal::active_traits::queue_handle_t* handle, const void* item) noexcept
    {
        return osal_queue_send(handle, item, osal::NO_WAIT);  // ISR-safe in CMSIS-RTOS2
    }

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

    osal::result osal_queue_receive_isr(osal::active_traits::queue_handle_t* handle, void* item) noexcept
    {
        return osal_queue_receive(handle, item, osal::NO_WAIT);
    }

    osal::result osal_queue_peek(osal::active_traits::queue_handle_t* /*handle*/, void* /*item*/,
                                 osal::tick_t /*timeout*/) noexcept
    {
        return osal::error_code::not_supported;  // CMSIS-RTOS2 has no peek
    }

    std::size_t osal_queue_count(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot = static_cast<const cmsis2_queue_slot*>(handle->native);
        return static_cast<std::size_t>(osMessageQueueGetCount(slot->id));
    }

    std::size_t osal_queue_free(const osal::active_traits::queue_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot = static_cast<const cmsis2_queue_slot*>(handle->native);
        return static_cast<std::size_t>(osMessageQueueGetSpace(slot->id));
    }

    // ---------------------------------------------------------------------------
    // Timer (osTimer)
    // ---------------------------------------------------------------------------

    osal::result osal_timer_create(osal::active_traits::timer_handle_t* handle, const char* name,
                                   osal_timer_callback_t callback, void* arg, osal::tick_t period_ticks,
                                   bool auto_reload) noexcept
    {
        if (handle == nullptr || callback == nullptr || period_ticks == 0U || period_ticks == osal::WAIT_FOREVER ||
            period_ticks > UINT32_MAX)
        {
            return osal::error_code::invalid_argument;
        }
        if (handle->native != nullptr)
        {
            return osal::error_code::already_exists;
        }
        auto* slot = slot_acquire(cmsis2_timers);
        if (slot == nullptr)
        {
            return osal::error_code::out_of_resources;
        }
        slot->fn     = callback;
        slot->arg    = arg;
        slot->period = period_ticks;
        slot->ever_started.store(false, std::memory_order_relaxed);

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

    osal::result osal_timer_destroy(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::ok();
        }
        auto* slot = static_cast<cmsis2_timer_slot*>(handle->native);
        // osTimerDelete does not specify an in-flight user callback join.
        if (slot->ever_started.load(std::memory_order_acquire))
        {
            return osal::error_code::not_supported;
        }
        if (osTimerDelete(slot->id) != osOK)
        {
            return osal::error_code::unknown;
        }
        slot->fn = nullptr;
        slot_release(cmsis2_timers, slot);
        handle->native = nullptr;
        return osal::ok();
    }

    osal::result osal_timer_start(osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis2_timer_slot*>(handle->native);
        slot->ever_started.store(true, std::memory_order_release);
        const osStatus_t s = osTimerStart(slot->id, static_cast<uint32_t>(slot->period));
        return (s == osOK) ? osal::ok() : osal::error_code::unknown;
    }

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

    osal::result osal_timer_reset(osal::active_traits::timer_handle_t* handle) noexcept
    {
        osal_timer_stop(handle);
        return osal_timer_start(handle);
    }

    osal::result osal_timer_set_period(osal::active_traits::timer_handle_t* handle,
                                       osal::tick_t                         new_period_ticks) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto* slot = static_cast<cmsis2_timer_slot*>(handle->native);
        if (new_period_ticks == 0U || new_period_ticks == osal::WAIT_FOREVER || new_period_ticks > UINT32_MAX)
        {
            return osal::error_code::invalid_argument;
        }
        if (osTimerIsRunning(slot->id) != 0U)
        {
            if (osTimerStart(slot->id, static_cast<uint32_t>(new_period_ticks)) != osOK)
            {
                return osal::error_code::unknown;
            }
        }
        slot->period = new_period_ticks;
        return osal::ok();
    }

    bool osal_timer_is_active(const osal::active_traits::timer_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return false;
        }
        const auto* slot = static_cast<const cmsis2_timer_slot*>(handle->native);
        return osTimerIsRunning(slot->id) != 0U;
    }

    // ---------------------------------------------------------------------------
    // Event flags (native osEventFlags)
    // ---------------------------------------------------------------------------

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

    osal::event_bits_t osal_event_flags_get(const osal::active_traits::event_flags_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot = static_cast<const cmsis2_event_slot*>(handle->native);
        return static_cast<osal::event_bits_t>(osEventFlagsGet(slot->id));
    }

    osal::result osal_event_flags_wait_any(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*    slot    = static_cast<cmsis2_event_slot*>(handle->native);
        uint32_t options = osFlagsWaitAny;  // NOLINT(cppcoreguidelines-init-variables)
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

    osal::result osal_event_flags_wait_all(osal::active_traits::event_flags_handle_t* handle, osal::event_bits_t bits,
                                           osal::event_bits_t* actual, bool clear_on_exit,
                                           osal::tick_t timeout) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return osal::error_code::not_initialized;
        }
        auto*    slot    = static_cast<cmsis2_event_slot*>(handle->native);
        uint32_t options = osFlagsWaitAll;  // NOLINT(cppcoreguidelines-init-variables)
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

    osal::result osal_event_flags_set_isr(osal::active_traits::event_flags_handle_t* handle,
                                          osal::event_bits_t                         bits) noexcept
    {
        return osal_event_flags_set(handle, bits);  // CMSIS-RTOS2 osEventFlagsSet is ISR-safe
    }

    // ---------------------------------------------------------------------------
    // Wait-set — not supported
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
    // Memory pool (native — osMemoryPool)
    // ---------------------------------------------------------------------------

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

    void* osal_memory_pool_allocate(osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return nullptr;
        }
        auto* slot = static_cast<cmsis2_pool_slot*>(handle->native);
        return osMemoryPoolAlloc(slot->id, 0U);  // non-blocking
    }

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

    std::size_t osal_memory_pool_available(const osal::active_traits::memory_pool_handle_t* handle) noexcept
    {
        if (handle == nullptr || handle->native == nullptr)
        {
            return 0U;
        }
        const auto* slot = static_cast<const cmsis2_pool_slot*>(handle->native);
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
